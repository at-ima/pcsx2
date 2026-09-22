// SPDX-FileCopyrightText: 2026 PCSX2 Dev Team
// SPDX-License-Identifier: GPL-3.0+

#include "Common.h"
#include "MTVU.h"
#include "arm64/VU1Recompiler.h"
#include "arm64/VU1Pipeline.h"
#include "common/HostSys.h"
#include "vixl/aarch64/macro-assembler-aarch64.h"

#include <algorithm>
#include <array>
#include <memory>
#include <vector>

namespace
{
	using namespace vixl::aarch64;
	// Larger blocks amortize preparation, but increase generated code and validation work.
	constexpr u32 MaxInstructions = 256;
	// Free-space threshold before compiling both ordinary and deferred paths,
	// not a fixed allocation per block or an independent throughput setting.
	constexpr size_t MaxBlockBytes = MaxInstructions * 2048;

	using Arm64VU1::Instruction;

	constexpr size_t VectorOffset(u32 reg)
	{
		return reg == 32 ? offsetof(VURegs, ACC) : offsetof(VURegs, VF) + sizeof(VECTOR) * reg;
	}

	struct VectorCache
	{
		std::array<int, 33> slots;
		std::array<u32, 8> offsets;
		u32 count = 0;

		VectorCache()
		{
			slots.fill(-1);
			offsets.fill(~u32(0));
		}
		VRegister Host(u32 reg) const { return VRegister(8 + slots[reg], 128); }
	};

	struct RetirementSchedule
	{
		u8 cycles = 0; // Zero means that incoming timing is still unknown.
		u8 retired = 0;
		u8 remaining = 0;
		// A divide issued earlier in the block may still be in the FDIV pipe here,
		// so this pair has to retire it itself instead of leaving that to the
		// generic per-pair preparation it is replacing.
		bool fdiv_pending = false;
	};

	struct Block
	{
		using Function = void (*)(u64 start, u64 cycles, u32 packet_pending);
		std::array<Instruction, MaxInstructions> instructions{};
		std::array<RetirementSchedule, MaxInstructions> schedule{};
		std::array<u32, MaxInstructions * 2> words{};
		struct SourceRange
		{
			u32 pc, first, count;
		};
		std::vector<SourceRange> ranges;
		std::array<u32, MaxInstructions> next_pc{};
		std::array<bool, MaxInstructions> delay{};
		bool has_branches = false;
		bool loops_to_entry = false;
		u32 count = 0;
		VectorCache cache;
		Function function = nullptr;
	};

	std::array<std::unique_ptr<Block>, VU1_PROGSIZE / 8> s_blocks;
	u8* s_base = nullptr;
	u8* s_write = nullptr;
	u8* s_end = nullptr;
	u32 s_options = 0;
	Arm64VU1::PipelineCode s_pipeline;

	constexpr size_t VF(u32 reg) { return offsetof(VURegs, VF) + sizeof(VECTOR) * reg; }
	constexpr size_t VI(u32 reg) { return offsetof(VURegs, VI) + sizeof(REG_VI) * reg; }
	MemOperand Field(size_t offset) { return MemOperand(x19, offset); }

	u32 Options()
	{
		return (CHECK_VU_OVERFLOW(0) ? 1 : 0) | (CHECK_VU_OVERFLOW(1) ? 2 : 0) | (CHECK_VUADDSUBHACK ? 4 : 0) |
		       (EmuConfig.Cpu.VU1FPCR.GetDenormalsAreZero() ? 8 : 0) |
		       (CpuVU1 == &CpuArm64VU1 && !CHECK_XGKICKHACK ? 16 : 0) |
		       (THREAD_VU1 ? 32 : 0);
	}

	void InvalidateAll()
	{
		for (auto& block : s_blocks)
			block.reset();
		s_write = s_base;
		s_pipeline = {};
		s_options = Options();
	}


	enum class Op
	{
		None,
		Add,
		Sub,
		Mul,
		Madd,
		Msub,
		Max,
		Min,
		Abs,
		Itof,
		Ftoi,
		Clip,
		Opmula,
		Opmsub,
		Unsupported
	};
	struct Upper
	{
		Op op = Op::Unsupported;
		int broadcast = -1; // -1: vector, 0..3: lane, 4: I, 5: Q
		bool acc = false;
		u32 scale = 0;
	};

	Upper DecodeUpper(u32 code)
	{
		const u32 op = code & 0x3f;
		if (code & 0x58000000) // E/D/T need the complete interpreter control path.
			return {};
		if (op < 0x1c)
		{
			constexpr Op ops[] = {Op::Add, Op::Sub, Op::Madd, Op::Msub, Op::Max, Op::Min, Op::Mul};
			return {ops[op / 4], static_cast<int>(op % 4)};
		}
		if (op < 0x30)
		{
			constexpr Op ops[] = {Op::Mul, Op::Max, Op::Mul, Op::Min,
				Op::Add, Op::Madd, Op::Add, Op::Madd, Op::Sub, Op::Msub, Op::Sub, Op::Msub,
				Op::Add, Op::Madd, Op::Mul, Op::Max, Op::Sub, Op::Msub, Op::Opmsub, Op::Min};
			const int bc = op >= 0x28 ? -1 : (op == 0x1d || op == 0x1f || (op & 2)) ? 4 :
			                                                                          5;
			if (op == 0x22 && CHECK_VUADDSUBHACK)
				return {};
			return {ops[op - 0x1c], bc};
		}
		if (op < 0x3c)
			return {};
		const u32 sub = (code >> 6) & 31;
		const int lane = op & 3;
		if (sub < 4)
		{
			constexpr Op ops[] = {Op::Add, Op::Sub, Op::Madd, Op::Msub};
			return {ops[sub], lane, true};
		}
		if (sub == 4 || sub == 5)
		{
			constexpr u32 scales[] = {0, 4, 12, 15};
			return {sub == 4 ? Op::Itof : Op::Ftoi, -1, false, scales[lane]};
		}
		if (sub == 6)
			return {Op::Mul, lane, true};
		if (sub == 7 && lane == 0)
			return {Op::Mul, 5, true};
		if (sub == 7 && lane == 1)
			return {Op::Abs};
		if (sub == 7 && lane == 2)
			return {Op::Mul, 4, true};
		if (sub == 7 && lane == 3)
			return {Op::Clip};
		if (sub == 8 || sub == 9)
		{
			const bool ternary = lane & 1;
			return {sub == 8 ? (ternary ? Op::Madd : Op::Add) : (ternary ? Op::Msub : Op::Sub), lane < 2 ? 5 : 4, true};
		}
		if (sub == 10 && lane == 0)
			return {Op::Add, -1, true};
		if (sub == 10 && lane == 1)
			return {Op::Madd, -1, true};
		if (sub == 10 && lane == 2)
			return {Op::Mul, -1, true};
		if (sub == 11 && lane == 0)
			return {Op::Sub, -1, true};
		if (sub == 11 && lane == 1)
			return {Op::Msub, -1, true};
		if (sub == 11 && lane == 2)
			return {Op::Opmula, -1, true};
		if (sub == 11 && lane == 3)
			return {Op::None};
		return {};
	}

	enum class Lower
	{
		Lq,
		Sq,
		Lqi,
		Sqi,
		Lqd,
		Sqd,
		Iaddiu,
		Isubiu,
		Iadd,
		Isub,
		Iaddi,
		Iand,
		Ior,
		Move,
		Mr32,
		Mfir,
		Mtir,
		Ilw,
		Ilwr,
		Isw,
		Fcand,
		Fceq,
		Fcor,
		Fcset,
		Fcget,
		Fseq,
		Fsset,
		Fsand,
		Fsor,
		Fmeq,
		Fmand,
		Fmor,
		Div,
		Sqrt,
		Rsqrt,
		Waitq,
		Esadd,
		Ersadd,
		Eleng,
		Erleng,
		Esum,
		Ercpr,
		Esqrt,
		Ersqrt,
		Waitp,
		Ibeq,
		Ibne,
		Ibgtz,
		Ibltz,
		Ibgez,
		Iblez,
		Xgkick,
		Branch,
		Bal,
		Jr,
		Jalr,
		Xtop,
		Xitop,
		Unsupported
	};
	Lower DecodeLower(u32 code)
	{
		switch (code >> 25)
		{
			case 0:
				return Lower::Lq;
			case 1:
				return Lower::Sq;
			case 4:
				return Lower::Ilw;
			case 5:
				return Lower::Isw;
			case 0x10:
				return Lower::Fceq;
			case 0x11:
				return Lower::Fcset;
			case 0x12:
				return Lower::Fcand;
			case 0x13:
				return Lower::Fcor;
			case 0x14:
				return Lower::Fseq;
			case 0x15:
				return Lower::Fsset;
			case 0x16:
				return Lower::Fsand;
			case 0x17:
				return Lower::Fsor;
			case 0x18:
				return Lower::Fmeq;
			case 0x1a:
				return Lower::Fmand;
			case 0x1b:
				return Lower::Fmor;
			case 0x1c:
				return Lower::Fcget;
			case 0x28:
				return Lower::Ibeq;
			case 0x29:
				return Lower::Ibne;
			case 0x2c:
				return Lower::Ibltz;
			case 0x2d:
				return Lower::Ibgtz;
			case 0x2e:
				return Lower::Iblez;
			case 0x2f:
				return Lower::Ibgez;
			case 8:
				return Lower::Iaddiu;
			case 9:
				return Lower::Isubiu;
			case 0x20:
				return Lower::Branch;
			case 0x21:
				return Lower::Bal;
			case 0x24:
				return Lower::Jr;
			case 0x25:
				return Lower::Jalr;
			case 0x40:
				switch (code & 0x3f)
				{
					case 0x30:
						return Lower::Iadd;
					case 0x31:
						return Lower::Isub;
					case 0x32:
						return Lower::Iaddi;
					case 0x34:
						return Lower::Iand;
					case 0x35:
						return Lower::Ior;
				}
				switch (code & 0x7ff)
				{
					case 0x37c:
						return Lower::Lqi;
					case 0x37d:
						return Lower::Sqi;
					case 0x37e:
						return Lower::Lqd;
					case 0x37f:
						return Lower::Sqd;
					case 0x33c:
						return Lower::Move;
					case 0x33d:
						return Lower::Mr32;
					case 0x3fd:
						return Lower::Mfir;
					case 0x3fc:
						return Lower::Mtir;
					case 0x3bc:
						return Lower::Div;
					case 0x3bd:
						return Lower::Sqrt;
					case 0x3be:
						return Lower::Rsqrt;
					case 0x3bf:
						return Lower::Waitq;
					case 0x73c:
						return Lower::Esadd;
					case 0x73d:
						return Lower::Ersadd;
					case 0x73e:
						return Lower::Eleng;
					case 0x73f:
						return Lower::Erleng;
					case 0x77e:
						return Lower::Esum;
					case 0x7be:
						return Lower::Ercpr;
					case 0x7bc:
						return Lower::Esqrt;
					case 0x7bd:
						return Lower::Ersqrt;
					case 0x7bf:
						return Lower::Waitp;
					case 0x3fe:
						return Lower::Ilwr;
					case 0x6fc:
						return CpuVU1 == &CpuArm64VU1 && !CHECK_XGKICKHACK ? Lower::Xgkick : Lower::Unsupported;
					case 0x6bc:
						return Lower::Xtop;
					case 0x6bd:
						return Lower::Xitop;
				}
				break;
		}
		return Lower::Unsupported;
	}


	bool IsIntegerBranch(Lower op)
	{
		return op == Lower::Ibeq || op == Lower::Ibne || op == Lower::Ibgtz ||
		       op == Lower::Ibltz || op == Lower::Ibgez || op == Lower::Iblez;
	}

	// JR/JALR/BAL: always-taken branches with no compile-time-constant continuation
	// (JR/JALR's target is a register; BAL's is static but handled the same way for
	// one code path). EmitControlFlow, not EmitLower, generates these.
	bool IsRegisterBranch(Lower op)
	{
		return op == Lower::Jr || op == Lower::Jalr || op == Lower::Bal;
	}

	void StoreWord(MacroAssembler& a, u32 value, size_t offset)
	{
		a.Mov(w9, value);
		a.Str(w9, Field(offset));
	}

	void StoreMasked(MacroAssembler& a, VRegister value, MemOperand address, u32 mask)
	{
		if (!mask)
			return;
		if (mask == 15)
			a.Str(value.Q(), address);
		else
		{
			a.Ldr(q3, address);
			for (u32 i = 0; i < 4; i++)
				if (mask & (8 >> i))
					a.Ins(v3.V4S(), i, value.V4S(), i);
			a.Str(q3, address);
		}
	}

	void LoadVector(MacroAssembler& a, const VectorCache& cache, VRegister value, u32 reg)
	{
		if (cache.slots[reg] >= 0)
			a.Mov(value.V16B(), cache.Host(reg).V16B());
		else
			a.Ldr(value.Q(), Field(VectorOffset(reg)));
	}

	void StoreVector(MacroAssembler& a, const VectorCache& cache, VRegister value, u32 reg, u32 mask = 15)
	{
		if (cache.slots[reg] < 0)
		{
			StoreMasked(a, value, Field(VectorOffset(reg)), mask);
			return;
		}
		const VRegister dest = cache.Host(reg);
		if (mask == 15)
			a.Mov(dest.V16B(), value.V16B());
		else
			for (u32 lane = 0; lane < 4; lane++)
				if (mask & (8 >> lane))
					a.Ins(dest.V4S(), lane, value.V4S(), lane);
	}

	void AssignVectorCache(Block& block)
	{
		std::array<u32, 33> uses{};
		for (u32 i = 0; i < block.count; i++)
		{
			const auto& ins = block.instructions[i];
			for (const _VURegsNum* regs : {&ins.uregs, &ins.lregs})
			{
				if (regs->VFread0)
					uses[regs->VFread0]++;
				if (regs->VFread1)
					uses[regs->VFread1]++;
				if (regs->VFwrite)
					uses[regs->VFwrite]++;
				if (regs->VIread & (1 << REG_ACC_FLAG))
					uses[32]++;
				if (regs->VIwrite & (1 << REG_ACC_FLAG))
					uses[32]++;
			}
		}
		for (u32 slot = 0; slot < block.cache.offsets.size(); slot++)
		{
			const auto best = std::max_element(uses.begin(), uses.end());
			if (*best < 2)
				break;
			const u32 reg = best - uses.begin();
			block.cache.slots[reg] = slot;
			block.cache.offsets[slot] = VectorOffset(reg);
			block.cache.count++;
			*best = 0;
		}
	}

	// Unconditionally: matches vuDouble's denormal flush exactly, ignoring
	// VU1FPCR. Most callers want ClampInput below instead, which skips this
	// when hardware FZ makes it redundant -- but that shortcut only holds when
	// the clamped value is guaranteed to pass through more arithmetic before
	// being stored; a handful of EFU ops (ERCPR/ESQRT/ERSQRT) can store this
	// exact value completely unchanged, with no further instruction for
	// hardware FZ to act on.
	// v6/v7 hold the overflow clamp bounds (0x7f7fffff / 0xff7fffff), and v24
	// holds the FP exponent mask (0x7f800000), all for the whole block, loaded
	// once by EmitBlockConstants at block entry and after any call that may
	// reach the real _vuXGKICKTransfer (which is free to clobber caller-saved
	// v0-v7/v16-v31 under the AAPCS64 ABI, unlike our own hand-rolled
	// VU1Pipeline.cpp stubs, which never touch vector registers at all).
	// v8-v15 hold the VectorCache and v28-v31 are used by EmitDeferredRegion's
	// per-slot MAC/status/clip flag cache, so v6/v7/v24 are deliberately
	// outside both ranges.
	void ClampInputAlways(MacroAssembler& a, VRegister reg)
	{
		a.And(v17.V16B(), reg.V16B(), v24.V16B());
		a.Movi(v18.V4S(), 0x80000000);
		a.And(v18.V16B(), reg.V16B(), v18.V16B());
		a.Cmeq(v19.V4S(), v17.V4S(), 0);
		a.Bsl(v19.V16B(), v18.V16B(), reg.V16B());
		a.Mov(reg.V16B(), v19.V16B());
		if (CHECK_VU_OVERFLOW(0))
		{
			// Signed min clamps positive infinities/NaNs; unsigned min clamps
			// their negative encodings. Finite values and signed zeros are intact.
			a.Smin(reg.V4S(), reg.V4S(), v6.V4S());
			a.Umin(reg.V4S(), reg.V4S(), v7.V4S());
		}
	}

	void ClampInput(MacroAssembler& a, VRegister reg)
	{
		// Arithmetic flushes signed denormal inputs in hardware when FPCR.FZ is
		// enabled. Other FPCR modes still need the interpreter's explicit clamp.
		if (EmuConfig.Cpu.VU1FPCR.GetDenormalsAreZero())
		{
			if (CHECK_VU_OVERFLOW(0))
			{
				a.Smin(reg.V4S(), reg.V4S(), v6.V4S());
				a.Umin(reg.V4S(), reg.V4S(), v7.V4S());
			}
			return;
		}
		ClampInputAlways(a, reg);
	}

	// Loads the shared per-block constants: the overflow-clamp bounds into
	// v6/v7 when this block's options actually need them, and the FP
	// exponent mask into v24 unconditionally (StoreMAC's MAC/status flag
	// classification always needs it). Call once at block entry, and again
	// after any Blr that might reach the real _vuXGKICKTransfer C++ function.
	void EmitBlockConstants(MacroAssembler& a)
	{
		a.Movi(v24.V4S(), 0x7f800000);
		if (!CHECK_VU_OVERFLOW(0) && !CHECK_VU_OVERFLOW(1))
			return;
		a.Movi(v6.V4S(), 0x7f7fffff);
		a.Movi(v7.V4S(), 0xff7fffff);
	}

	void StoreMAC(MacroAssembler& a, const VectorCache& cache, const Upper& op, u32 code, int mask_override = -1)
	{
		// OPMULA/OPMSUB always write xyz regardless of the encoded bits at this
		// position, which are not a destination mask for those two opcodes.
		const u32 mask = mask_override >= 0 ? static_cast<u32>(mask_override) : (code >> 21) & 15;
		const bool flush = EmuConfig.Cpu.VU1FPCR.GetFlushToZero();
		// v0 is the result. Classify all lanes using the interpreter's FP zero test.
		// v24 holds the exponent mask for the whole block; see EmitBlockConstants.
		a.And(v17.V16B(), v0.V16B(), v24.V16B());
		a.Fcmeq(v18.V4S(), v0.V4S(), 0.0);
		if (!flush)
		{
			a.Cmeq(v19.V4S(), v17.V4S(), 0);
			a.Bic(v19.V16B(), v19.V16B(), v18.V16B()); // underflow
			a.Orr(v21.V16B(), v18.V16B(), v19.V16B());
		}
		a.Cmeq(v20.V4S(), v17.V4S(), v24.V4S()); // overflow
		// Weight each enabled lane by its architectural MAC bit before the
		// horizontal sum. The four bit groups do not overlap or carry.
		const auto weight = [mask](u32 lane) -> u64 { return mask & (8 >> lane); };
		a.Ldr(q22, weight(2) | (weight(3) << 32), weight(0) | (weight(1) << 32));
		a.And(v21.V16B(), flush ? v18.V16B() : v21.V16B(), v22.V16B());
		a.Sshr(v23.V4S(), v0.V4S(), 31);
		a.And(v23.V16B(), v23.V16B(), v22.V16B());
		a.Shl(v23.V4S(), v23.V4S(), 4);
		a.Orr(v21.V16B(), v21.V16B(), v23.V16B());
		if (!flush)
		{
			a.And(v23.V16B(), v19.V16B(), v22.V16B());
			a.Shl(v23.V4S(), v23.V4S(), 8);
			a.Orr(v21.V16B(), v21.V16B(), v23.V16B());
		}
		a.Shl(v22.V4S(), v22.V4S(), 12);
		a.And(v22.V16B(), v20.V16B(), v22.V16B());
		a.Orr(v21.V16B(), v21.V16B(), v22.V16B());
		a.Addv(s22, v21.V4S());
		a.Fmov(w10, s22);
		a.Ldr(w9, Field(offsetof(VURegs, macflag)));
		a.And(w9, w9, 0xffff0000);
		a.Orr(w9, w9, w10);
		a.Str(w9, Field(offsetof(VURegs, macflag)));
		a.Orr(w11, w10, Operand(w10, LSR, 1));
		a.Orr(w11, w11, Operand(w11, LSR, 2));
		a.And(w11, w11, 0x1111);
		a.Orr(w11, w11, Operand(w11, LSR, 3));
		a.Orr(w11, w11, Operand(w11, LSR, 6));
		a.And(w11, w11, 15);
		a.Str(w11, Field(offsetof(VURegs, statusflag)));
		// FZ arithmetic has already produced signed zero for tiny results. The
		// reference FP comparison consequently reports zero, not underflow.
		if (!flush)
		{
			a.Movi(v22.V4S(), 0x80000000);
			a.And(v22.V16B(), v0.V16B(), v22.V16B());
			a.Bsl(v19.V16B(), v22.V16B(), v0.V16B());
			a.Mov(v0.V16B(), v19.V16B());
		}
		if (CHECK_VU_OVERFLOW(1))
		{
			// v6/v7 hold the clamp bounds for the whole block; see
			// EmitBlockConstants.
			a.Smin(v0.V4S(), v0.V4S(), v6.V4S());
			a.Umin(v0.V4S(), v0.V4S(), v7.V4S());
		}
		const u32 fd = (code >> 6) & 31;
		if (op.acc || fd)
			StoreVector(a, cache, v0, op.acc ? 32 : fd, mask);
	}

	void EmitUpper(MacroAssembler& a, const VectorCache& cache, u32 code)
	{
		const Upper op = DecodeUpper(code);
		if (op.op == Op::None)
			return;
		const u32 fs = (code >> 11) & 31, ft = (code >> 16) & 31, fd = (code >> 6) & 31;
		const u32 mask = (code >> 21) & 15;
		LoadVector(a, cache, q0, fs);
		if (op.op == Op::Clip)
		{
			// CLIP compares signed bit patterns, including non-finite inputs.
			// A denormal W uses the largest denormal threshold; FP compares differ.
			LoadVector(a, cache, q1, ft);
			a.Umov(w0, v1.V4S(), 3);
			a.And(w1, w0, 0x7fffffff);
			a.Mov(w2, 0x007fffff);
			a.Tst(w0, 0x7f800000);
			a.Csel(w1, w1, w2, ne);
			a.Dup(v1.V4S(), w1);
			a.Movi(v2.V4S(), 0x80000000);
			a.Eor(v2.V16B(), v0.V16B(), v2.V16B());
			a.Cmgt(v0.V4S(), v0.V4S(), v1.V4S());
			a.Cmgt(v2.V4S(), v2.V4S(), v1.V4S());
			// Pack +X/-X/+Y/-Y/+Z/-Z into the next six history bits.
			a.Mov(x0, 0x0000000400000001ull);
			a.Fmov(d3, x0);
			a.Mov(w0, 16);
			a.Ins(v3.V4S(), 2, w0);
			a.And(v0.V16B(), v0.V16B(), v3.V16B());
			a.Shl(v3.V4S(), v3.V4S(), 1);
			a.And(v2.V16B(), v2.V16B(), v3.V16B());
			a.Orr(v0.V16B(), v0.V16B(), v2.V16B());
			a.Addv(s0, v0.V4S());
			a.Fmov(w0, s0);
			a.Ldr(w1, Field(offsetof(VURegs, clipflag)));
			a.Orr(w0, w0, Operand(w1, LSL, 6));
			a.And(w0, w0, 0xffffff);
			a.Str(w0, Field(offsetof(VURegs, clipflag)));
			return;
		}
		if (op.op == Op::Abs || op.op == Op::Itof || op.op == Op::Ftoi)
		{
			if (!ft)
				return;
			if (op.op == Op::Abs)
			{
				a.Movi(v1.V4S(), 0x7fffffff);
				a.And(v0.V16B(), v0.V16B(), v1.V16B());
			}
			else if (op.op == Op::Itof)
			{
				a.Scvtf(v0.V4S(), v0.V4S());
				if (op.scale)
				{
					a.Movi(v1.V4S(), 0x3f800000 - (op.scale << 23));
					a.Fmul(v0.V4S(), v0.V4S(), v1.V4S());
				}
			}
			else
			{
				if (op.scale)
				{
					a.Movi(v1.V4S(), 0x3f800000 + (op.scale << 23));
					a.Fmul(v0.V4S(), v0.V4S(), v1.V4S());
				}
				a.Movi(v1.V4S(), 0x7f800000);
				a.And(v2.V16B(), v0.V16B(), v1.V16B());
				a.Movi(v1.V4S(), 0x4f000000);
				a.Cmhs(v2.V4S(), v2.V4S(), v1.V4S());
				a.Sshr(v4.V4S(), v0.V4S(), 31);
				a.Movi(v1.V4S(), 0x7fffffff);
				a.Eor(v4.V16B(), v4.V16B(), v1.V16B());
				a.Fcvtzs(v0.V4S(), v0.V4S());
				a.Bsl(v2.V16B(), v4.V16B(), v0.V16B());
				a.Mov(v0.V16B(), v2.V16B());
			}
			StoreVector(a, cache, v0, ft, mask);
			return;
		}
		if (op.broadcast < 4)
		{
			LoadVector(a, cache, q1, ft);
			if (op.broadcast >= 0)
				a.Dup(v1.V4S(), v1.V4S(), op.broadcast);
		}
		else
		{
			a.Ldr(s1, Field(VI(op.broadcast == 4 ? REG_I : REG_Q)));
			a.Dup(v1.V4S(), v1.V4S(), 0);
		}
		if (op.op == Op::Max || op.op == Op::Min)
		{
			if (!fd)
				return;
			a.And(v2.V16B(), v0.V16B(), v1.V16B());
			a.Cmlt(v2.V4S(), v2.V4S(), 0);
			a.Smax(v4.V4S(), v0.V4S(), v1.V4S());
			a.Smin(v0.V4S(), v0.V4S(), v1.V4S());
			if (op.op == Op::Max)
				a.Bsl(v2.V16B(), v0.V16B(), v4.V16B());
			else
				a.Bsl(v2.V16B(), v4.V16B(), v0.V16B());
			StoreVector(a, cache, v2, fd, mask);
			return;
		}
		if (op.op == Op::Opmula || op.op == Op::Opmsub)
		{
			// Outer product: rotate Fs to yzx and Ft to zxy before multiplying.
			// The W lane of each shuffled vector is never read (mask is fixed
			// to xyz), so it is left with whatever the copy produces.
			a.Mov(v3.V16B(), v0.V16B());
			a.Ins(v0.V4S(), 0, v3.V4S(), 1);
			a.Ins(v0.V4S(), 1, v3.V4S(), 2);
			a.Ins(v0.V4S(), 2, v3.V4S(), 0);
			a.Mov(v3.V16B(), v1.V16B());
			a.Ins(v1.V4S(), 0, v3.V4S(), 2);
			a.Ins(v1.V4S(), 1, v3.V4S(), 0);
			a.Ins(v1.V4S(), 2, v3.V4S(), 1);
			ClampInput(a, v0);
			ClampInput(a, v1);
			if (op.op == Op::Opmula)
				a.Fmul(v0.V4S(), v0.V4S(), v1.V4S());
			else
			{
				LoadVector(a, cache, q2, 32);
				ClampInput(a, v2);
				// Match the ARM64 interpreter's contracted multiply/subtract.
				a.Fmls(v2.V4S(), v0.V4S(), v1.V4S());
				a.Mov(v0.V16B(), v2.V16B());
			}
			StoreMAC(a, cache, op, code, 0xE);
			return;
		}
		ClampInput(a, v0);
		ClampInput(a, v1);
		switch (op.op)
		{
			case Op::Add:
				a.Fadd(v0.V4S(), v0.V4S(), v1.V4S());
				break;
			case Op::Sub:
				a.Fsub(v0.V4S(), v0.V4S(), v1.V4S());
				break;
			case Op::Mul:
				a.Fmul(v0.V4S(), v0.V4S(), v1.V4S());
				break;
			case Op::Madd:
			case Op::Msub:
				LoadVector(a, cache, q2, 32);
				ClampInput(a, v2);
				// Match the ARM64 interpreter's contracted multiply/add operations.
				if (op.op == Op::Madd)
					a.Fmla(v2.V4S(), v0.V4S(), v1.V4S());
				else
					a.Fmls(v2.V4S(), v0.V4S(), v1.V4S());
				a.Mov(v0.V16B(), v2.V16B());
				break;
			default:
				break;
		}
		StoreMAC(a, cache, op, code);
	}

	void BackupVI(MacroAssembler& a, u32 reg)
	{
		Label done;
		a.Ldrb(w9, Field(offsetof(VURegs, VIBackupCycles)));
		a.Ldr(w10, Field(offsetof(VURegs, VIRegNumber)));
		a.Cmp(w10, reg);
		a.Ccmp(w9, 0, ZFlag, eq);
		a.B(ne, &done);
		a.Ldrh(w9, Field(VI(reg)));
		a.Str(w9, Field(offsetof(VURegs, VIOldValue)));
		StoreWord(a, reg, offsetof(VURegs, VIRegNumber));
		a.Bind(&done);
		a.Mov(w9, 2);
		a.Strb(w9, Field(offsetof(VURegs, VIBackupCycles)));
	}

	// Shared by DIV/SQRT/RSQRT: a still-pending previous FDIV op stalls issue
	// (_vuTestFDIVStalls), same as the generic FMAC-read hazard the caller's
	// prepare stub covers. The interpreter retires the pipe (_vuTestPipes)
	// between that stall and the new op, so the old result must reach Q
	// before this one replaces it.
	void EmitFDIVStall(MacroAssembler& a)
	{
		Label not_pending;
		a.Ldr(w9, Field(offsetof(VURegs, fdiv) + offsetof(fdivPipe, enable)));
		a.Cbz(w9, &not_pending);
		a.Ldr(x9, Field(offsetof(VURegs, fdiv) + offsetof(fdivPipe, sCycle)));
		a.Ldr(w10, Field(offsetof(VURegs, fdiv) + offsetof(fdivPipe, Cycle)));
		a.Add(x9, x9, x10);
		a.Cmp(x26, x9);
		a.Csel(x26, x9, x26, lo);
		// The caller only publishes its cached cycle for scheduled pairs, so
		// a stall taken here has to reach architectural state on its own.
		a.Str(x26, Field(offsetof(VURegs, cycle)));
		a.Ldr(w10, Field(offsetof(VURegs, fdiv) + offsetof(fdivPipe, reg)));
		a.Str(w10, Field(VI(REG_Q)));
		a.Ldr(w10, Field(VI(REG_STATUS_FLAG)));
		a.And(w10, w10, 0xfcf);
		a.Ldr(w11, Field(offsetof(VURegs, fdiv) + offsetof(fdivPipe, statusflag)));
		a.And(w11, w11, 0xc30);
		a.Orr(w10, w10, w11);
		a.Str(w10, Field(VI(REG_STATUS_FLAG)));
		// The forced advance above always reaches at least the entry's own
		// ready cycle, so it is fully retired now. DIV/SQRT/RSQRT immediately
		// re-arm the pipe below and would overwrite this either way, but
		// WAITQ issues nothing further and needs the pipe left empty, matching
		// _vuTestPipes retiring it once _vuTestFDIVStalls has advanced VU->cycle.
		a.Str(wzr, Field(offsetof(VURegs, fdiv) + offsetof(fdivPipe, enable)));
		// The ordinary per-pair prepare/retire already drained the FMAC ring
		// once, before this pair's own body could force the clock forward
		// above; entries that only became ready because of that forcing are
		// otherwise never drained (a run of unrelated pairs right after can
		// be swept into a deferred region that skips the runtime retire path
		// entirely, leaving them stuck until the block ends). This mirrors
		// _vuTestPipes running again, after _vuTestFDIVStalls, in the
		// interpreter's own per-instruction dispatch order.
		a.Mov(x16, reinterpret_cast<uintptr_t>(s_pipeline.retire_queues));
		a.Blr(x16);
		EmitBlockConstants(a);
		a.Bind(&not_pending);
	}

	// w0 = result bits, w1 = statusflag bits, both already computed by the caller.
	void EmitFDIVFinish(MacroAssembler& a, u32 cycles)
	{
		a.Str(w1, Field(offsetof(VURegs, statusflag)));
		// The op also leaves its result in the staging Q field itself, distinct
		// from the architectural VI(REG_Q) the FDIV pipe retires into later.
		a.Str(w0, Field(offsetof(VURegs, q)));
		a.Str(w0, Field(offsetof(VURegs, fdiv) + offsetof(fdivPipe, reg)));
		a.Str(w1, Field(offsetof(VURegs, fdiv) + offsetof(fdivPipe, statusflag)));
		a.Str(x26, Field(offsetof(VURegs, fdiv) + offsetof(fdivPipe, sCycle)));
		a.Mov(w9, cycles);
		a.Str(w9, Field(offsetof(VURegs, fdiv) + offsetof(fdivPipe, Cycle)));
		a.Mov(w9, 1);
		a.Str(w9, Field(offsetof(VURegs, fdiv) + offsetof(fdivPipe, enable)));
	}

	// Shared by every EFU-issuing op (ESADD..EEXP) and WAITP: a still-pending
	// previous EFU op stalls issue (_vuTestEFUStalls), mirroring EmitFDIVStall's
	// generic-hazard/pipe-retirement handling for Q, but for P. The interpreter
	// decrements the pending entry's own Cycle field by one before testing
	// readiness -- "the stall is released 1 cycle before P is updated"
	// (VUops.cpp) -- which only affects the forced VURegs::cycle advance below,
	// since every EFU-issuing op immediately overwrites the entry's Cycle right
	// after anyway (WAITP leaves it disabled instead); mutate efu.Cycle to
	// match exactly, needs proper testing right at the cycle-wrap boundary.
	void EmitEFUStall(MacroAssembler& a)
	{
		Label not_pending;
		a.Ldr(w9, Field(offsetof(VURegs, efu) + offsetof(efuPipe, enable)));
		a.Cbz(w9, &not_pending);
		a.Ldr(w10, Field(offsetof(VURegs, efu) + offsetof(efuPipe, Cycle)));
		a.Sub(w10, w10, 1);
		a.Str(w10, Field(offsetof(VURegs, efu) + offsetof(efuPipe, Cycle)));
		a.Ldr(x9, Field(offsetof(VURegs, efu) + offsetof(efuPipe, sCycle)));
		a.Add(x9, x9, x10);
		a.Cmp(x26, x9);
		a.Csel(x26, x9, x26, lo);
		a.Str(x26, Field(offsetof(VURegs, cycle)));
		a.Ldr(w10, Field(offsetof(VURegs, efu) + offsetof(efuPipe, reg)));
		a.Str(w10, Field(VI(REG_P)));
		a.Str(wzr, Field(offsetof(VURegs, efu) + offsetof(efuPipe, enable)));
		a.Mov(x16, reinterpret_cast<uintptr_t>(s_pipeline.retire_queues));
		a.Blr(x16);
		EmitBlockConstants(a);
		a.Bind(&not_pending);
	}

	// w0 = result bits, already computed by the caller. EFU has no status-flag
	// interaction at all, unlike the FDIV pipe.
	void EmitEFUFinish(MacroAssembler& a, u32 cycles)
	{
		a.Str(w0, Field(offsetof(VURegs, p)));
		a.Str(w0, Field(offsetof(VURegs, efu) + offsetof(efuPipe, reg)));
		a.Str(x26, Field(offsetof(VURegs, efu) + offsetof(efuPipe, sCycle)));
		a.Mov(w9, cycles);
		a.Str(w9, Field(offsetof(VURegs, efu) + offsetof(efuPipe, Cycle)));
		a.Mov(w9, 1);
		a.Str(w9, Field(offsetof(VURegs, efu) + offsetof(efuPipe, enable)));
	}

	void EmitLower(MacroAssembler& a, const VectorCache& cache, u32 code)
	{
		const Lower op = DecodeLower(code);
		const u32 fs = (code >> 11) & 31, ft = (code >> 16) & 31, id = (code >> 6) & 15;
		const u32 is = fs & 15, it = ft & 15, mask = (code >> 21) & 15;
		if (op == Lower::Fcand || op == Lower::Fceq || op == Lower::Fcor)
		{
			// Read the retired flag instance, not the current CLIP accumulator.
			a.Ldr(w0, Field(VI(REG_CLIP_FLAG)));
			a.And(w0, w0, 0xffffff);
			a.Mov(w1, code & 0xffffff);
			if (op == Lower::Fcand)
				a.Tst(w0, w1);
			else if (op == Lower::Fceq)
				a.Cmp(w0, w1);
			else
			{
				a.Orr(w0, w0, w1);
				a.Mov(w1, 0xffffff);
				a.Cmp(w0, w1);
			}
			a.Cset(w0, op == Lower::Fcand ? ne : eq);
			a.Strh(w0, Field(VI(1)));
			return;
		}
		if (op == Lower::Fseq || op == Lower::Fsand || op == Lower::Fsor)
		{
			// Reads the retired status instance's low 12 bits, same as FCAND/FCEQ/FCOR
			// read the retired CLIP instance.
			if (!it)
				return;
			a.Ldr(w0, Field(VI(REG_STATUS_FLAG)));
			a.And(w0, w0, 0xfff);
			a.Mov(w1, (((code >> 21) & 1) << 11) | (code & 0x7ff));
			if (op == Lower::Fseq)
			{
				a.Cmp(w0, w1);
				a.Cset(w0, eq);
			}
			else if (op == Lower::Fsand)
				a.And(w0, w0, w1);
			else
				a.Orr(w0, w0, w1);
			a.Strh(w0, Field(VI(it)));
			return;
		}
		if (op == Lower::Fmeq || op == Lower::Fmand || op == Lower::Fmor)
		{
			if (!it)
				return;
			a.Ldr(w0, Field(VI(REG_MAC_FLAG)));
			a.And(w0, w0, 0xffff);
			a.Ldrh(w1, Field(VI(is)));
			if (op == Lower::Fmeq)
			{
				a.Cmp(w0, w1);
				a.Cset(w0, eq);
			}
			else if (op == Lower::Fmand)
				a.And(w0, w0, w1);
			else
				a.Orr(w0, w0, w1);
			a.Strh(w0, Field(VI(it)));
			return;
		}
		if (op == Lower::Fcget)
		{
			if (!it)
				return;
			a.Ldr(w0, Field(VI(REG_CLIP_FLAG)));
			a.And(w0, w0, 0xfff);
			a.Strh(w0, Field(VI(it)));
			return;
		}
		if (op == Lower::Fcset)
		{
			// Writes the staging clipflag, same as CLIP; the generic FMAC-pipe path
			// retires it into VI(REG_CLIP_FLAG) after four cycles.
			a.Mov(w0, code & 0xffffff);
			a.Str(w0, Field(offsetof(VURegs, clipflag)));
			return;
		}
		if (op == Lower::Fsset)
		{
			a.Ldr(w0, Field(offsetof(VURegs, statusflag)));
			a.And(w0, w0, 0x3f);
			a.Orr(w0, w0, (((code >> 21) & 1) << 11 | (code & 0x7ff)) & 0xfc0);
			a.Str(w0, Field(offsetof(VURegs, statusflag)));
			return;
		}
		if (op == Lower::Div)
		{
			// Mirrors _vuDIV/_vuFDIVAdd: compute now (matching the interpreter's
			// immediate statusflag write), then stage Q into the shared FDIV pipe
			// so the generic retirement code publishes it after 7 cycles, needs
			// proper testing against every division-by-zero/sign combination.
			EmitFDIVStall(a);
			const u32 fsf = (code >> 21) & 3, ftf = (code >> 23) & 3;
			LoadVector(a, cache, q0, fs);
			LoadVector(a, cache, q1, ft);
			a.Dup(v2.V4S(), v0.V4S(), fsf);
			a.Dup(v3.V4S(), v1.V4S(), ftf);
			ClampInput(a, v2); // fs, after vuDouble
			ClampInput(a, v3); // ft, after vuDouble
			a.Umov(w2, v2.V4S(), 0);
			a.Umov(w3, v3.V4S(), 0);
			Label is_zero, have_result, done;
			a.Fcmp(s3, 0.0);
			a.B(eq, &is_zero);
			a.Fdiv(s4, s2, s3);
			a.Dup(v4.V4S(), v4.V4S(), 0);
			ClampInput(a, v4); // result, after vuDouble
			a.Umov(w0, v4.V4S(), 0);
			a.Ldr(w1, Field(offsetof(VURegs, statusflag)));
			a.And(w1, w1, 0xffffffcf);
			a.B(&have_result);
			a.Bind(&is_zero);
			{
				a.Ldr(w1, Field(offsetof(VURegs, statusflag)));
				a.And(w1, w1, 0xffffffcf);
				a.Fcmp(s2, 0.0);
				Label fs_nonzero;
				a.B(ne, &fs_nonzero);
				a.Orr(w1, w1, 0x10); // fs == 0 && ft == 0: invalid (I flag)
				a.B(&done);
				a.Bind(&fs_nonzero);
				a.Orr(w1, w1, 0x20); // fs != 0 && ft == 0: divide-by-zero (D flag)
				a.Bind(&done);
			}
			// Signed max float, matching the sign of fs's numerator over ft's zero.
			a.Eor(w0, w2, w3);
			a.Mov(w4, 0x7f7fffff);
			a.Mov(w5, 0xff7fffff);
			a.Tst(w0, 0x80000000);
			a.Csel(w0, w5, w4, ne);
			a.Bind(&have_result);
			EmitFDIVFinish(a, 7);
			return;
		}
		if (op == Lower::Sqrt)
		{
			// Mirrors _vuSQRT: q = sqrt(fabs(ft)), needs proper testing against
			// every sign/denormal/non-finite combination.
			EmitFDIVStall(a);
			const u32 ftf = (code >> 23) & 3;
			LoadVector(a, cache, q1, ft);
			a.Dup(v3.V4S(), v1.V4S(), ftf);
			ClampInput(a, v3); // ft, after vuDouble
			a.Fabs(s4, s3);
			a.Fsqrt(s4, s4);
			a.Dup(v4.V4S(), v4.V4S(), 0);
			ClampInput(a, v4); // result, after vuDouble
			a.Umov(w0, v4.V4S(), 0);
			a.Ldr(w1, Field(offsetof(VURegs, statusflag)));
			a.And(w1, w1, 0xffffffcf);
			// "pl" (N==0) matches a plain "ft < 0.0" comparison, including the
			// false-for-NaN case; "lt"/"ge" treat unordered operands as taken.
			a.Fcmp(s3, 0.0);
			Label not_negative;
			a.B(pl, &not_negative);
			a.Orr(w1, w1, 0x10); // I flag
			a.Bind(&not_negative);
			EmitFDIVFinish(a, 7);
			return;
		}
		if (op == Lower::Rsqrt)
		{
			// Mirrors _vuRSQRT: q = fs / sqrt(fabs(ft)), with ft == 0 branching
			// into a deeper zero-handling case than DIV's, needs proper testing
			// against every sign/zero/denormal/non-finite combination.
			EmitFDIVStall(a);
			const u32 fsf = (code >> 21) & 3, ftf = (code >> 23) & 3;
			LoadVector(a, cache, q0, fs);
			LoadVector(a, cache, q1, ft);
			a.Dup(v2.V4S(), v0.V4S(), fsf);
			a.Dup(v3.V4S(), v1.V4S(), ftf);
			ClampInput(a, v2); // fs, after vuDouble
			ClampInput(a, v3); // ft, after vuDouble
			a.Umov(w2, v2.V4S(), 0);
			a.Umov(w3, v3.V4S(), 0);
			Label ft_zero, have_result;
			a.Fcmp(s3, 0.0);
			a.B(eq, &ft_zero);
			a.Fabs(s5, s3);
			a.Fsqrt(s5, s5);
			a.Fdiv(s4, s2, s5);
			a.Dup(v4.V4S(), v4.V4S(), 0);
			ClampInput(a, v4); // result, after vuDouble
			a.Umov(w0, v4.V4S(), 0);
			a.Ldr(w1, Field(offsetof(VURegs, statusflag)));
			a.And(w1, w1, 0xffffffcf);
			// See the SQRT case: "pl" is the correct false-for-NaN "ft < 0.0" test.
			a.Fcmp(s3, 0.0);
			a.B(pl, &have_result);
			a.Orr(w1, w1, 0x10); // I flag
			a.B(&have_result);
			a.Bind(&ft_zero);
			{
				a.Ldr(w1, Field(offsetof(VURegs, statusflag)));
				a.And(w1, w1, 0xffffffcf);
				a.Orr(w1, w1, 0x20); // D flag, always: division by zero
				a.Eor(w0, w2, w3);
				a.And(w0, w0, 0x80000000);
				Label fs_zero;
				a.Fcmp(s2, 0.0);
				a.B(eq, &fs_zero);
				// fs != 0: signed max float, matching the sign of fs over ft's zero.
				a.Mov(w4, 0x7f7fffff);
				a.Orr(w0, w0, w4);
				a.B(&have_result);
				a.Bind(&fs_zero);
				// fs == 0 too: signed zero, and I joins D (both flags set).
				a.Orr(w1, w1, 0x10);
			}
			a.Bind(&have_result);
			EmitFDIVFinish(a, 13);
			return;
		}
		if (op == Lower::Waitq)
		{
			// _vuWAITQ's body is empty; its only effect is the pending-FDIV
			// stall every FDIV-pipe op takes before doing anything else. It
			// does not issue a new pipe entry, so there is nothing to finish.
			EmitFDIVStall(a);
			return;
		}
		if (op == Lower::Esadd || op == Lower::Ersadd || op == Lower::Eleng || op == Lower::Erleng)
		{
			// _vuESADD/_vuERSADD/_vuELENG/_vuERLENG all reduce fs.xyz to
			// p = fs.x^2 + fs.y^2 + fs.z^2 (left-to-right, matching the
			// interpreter's evaluation order), then diverge: ESADD stores it
			// directly, ERSADD takes its reciprocal (skipped when zero), ELENG
			// takes its square root (skipped when negative or NaN), ERLENG
			// chains both. None of the four clamp their output, matching the
			// interpreter (no vuDouble call on the result there), needs proper
			// testing against every sign/zero/denormal/non-finite combination.
			EmitEFUStall(a);
			LoadVector(a, cache, q0, fs);
			ClampInput(a, v0);
			a.Dup(v2.V4S(), v0.V4S(), 0);
			a.Dup(v3.V4S(), v0.V4S(), 1);
			a.Dup(v4.V4S(), v0.V4S(), 2);
			a.Fmul(s2, s2, s2);
			a.Fmul(s3, s3, s3);
			a.Fmul(s4, s4, s4);
			a.Fadd(s5, s2, s3);
			a.Fadd(s5, s5, s4);
			if (op == Lower::Ersadd)
			{
				Label done;
				a.Fcmp(s5, 0.0);
				a.B(eq, &done);
				a.Fmov(s6, 1.0f);
				a.Fdiv(s5, s6, s5);
				a.Bind(&done);
			}
			else if (op == Lower::Eleng || op == Lower::Erleng)
			{
				// "lt" is true for both a real negative sum and an unordered
				// (NaN) one, so this correctly skips the square root -- leaving
				// p as the original sum -- for exactly the cases where a plain
				// "p >= 0" test would be false in C.
				Label skip_sqrt;
				a.Fcmp(s5, 0.0);
				a.B(lt, &skip_sqrt);
				a.Fsqrt(s5, s5);
				if (op == Lower::Erleng)
				{
					Label done;
					a.Fcmp(s5, 0.0);
					a.B(eq, &done);
					a.Fmov(s6, 1.0f);
					a.Fdiv(s5, s6, s5);
					a.Bind(&done);
				}
				a.Bind(&skip_sqrt);
			}
			a.Fmov(w0, s5);
			u32 cycles = 11; // Esadd
			if (op == Lower::Ersadd || op == Lower::Eleng)
				cycles = 18;
			else if (op == Lower::Erleng)
				cycles = 24;
			EmitEFUFinish(a, cycles);
			return;
		}
		if (op == Lower::Esum)
		{
			// _vuESUM: p = fs.x + fs.y + fs.z + fs.w, left-to-right, no clamp.
			EmitEFUStall(a);
			LoadVector(a, cache, q0, fs);
			ClampInput(a, v0);
			a.Dup(v2.V4S(), v0.V4S(), 0);
			a.Dup(v3.V4S(), v0.V4S(), 1);
			a.Dup(v4.V4S(), v0.V4S(), 2);
			a.Dup(v5.V4S(), v0.V4S(), 3);
			a.Fadd(s2, s2, s3);
			a.Fadd(s2, s2, s4);
			a.Fadd(s2, s2, s5);
			a.Fmov(w0, s2);
			EmitEFUFinish(a, 12);
			return;
		}
		if (op == Lower::Ercpr || op == Lower::Esqrt || op == Lower::Ersqrt)
		{
			// _vuERCPR/_vuESQRT/_vuERSQRT read a single Fs[fsf] lane directly,
			// unlike the FDIV pipe's SQRT/RSQRT which take fabs() first: ERCPR
			// takes its reciprocal (skipped when zero); ESQRT/ERSQRT take its
			// square root, skipped entirely -- p keeps the original value --
			// when negative or NaN; ERSQRT chains a reciprocal after. No output
			// clamp, matching the interpreter, needs proper testing against
			// every sign/zero/denormal/non-finite combination.
			const u32 fsf = mask & 3;
			EmitEFUStall(a);
			LoadVector(a, cache, q0, fs);
			a.Dup(v2.V4S(), v0.V4S(), fsf);
			// The "skip" branch below can store this value completely
			// unchanged, with no arithmetic instruction for hardware FZ to act
			// on, so it needs the unconditional clamp (see ClampInputAlways).
			ClampInputAlways(a, v2);
			if (op == Lower::Ercpr)
			{
				// _vuERCPR divides "1.0" (a double literal, unlike ERSADD/
				// ERLENG/ERSQRT's "1.0f") by p, so the division itself happens
				// in double precision and only the final assignment back to
				// float rounds -- a different result than a native single-
				// precision division for some inputs. Widen, divide, narrow.
				Label done;
				a.Fcmp(s2, 0.0);
				a.B(eq, &done);
				a.Fcvt(d2, s2);
				a.Fmov(d6, 1.0);
				a.Fdiv(d2, d6, d2);
				a.Fcvt(s2, d2);
				a.Bind(&done);
			}
			else
			{
				Label skip_sqrt;
				a.Fcmp(s2, 0.0);
				a.B(lt, &skip_sqrt);
				a.Fsqrt(s2, s2);
				if (op == Lower::Ersqrt)
				{
					Label done;
					a.Fcmp(s2, 0.0);
					a.B(eq, &done);
					a.Fmov(s6, 1.0f);
					a.Fdiv(s2, s6, s2);
					a.Bind(&done);
				}
				a.Bind(&skip_sqrt);
			}
			a.Fmov(w0, s2);
			EmitEFUFinish(a, op == Lower::Ersqrt ? 18 : 12);
			return;
		}
		if (op == Lower::Waitp)
		{
			// _vuWAITP's body is empty; its only effect is the pending-EFU
			// stall every EFU-pipe op takes before doing anything else.
			EmitEFUStall(a);
			return;
		}
		if (op == Lower::Ilw || op == Lower::Ilwr)
		{
			if (!it || !mask)
				return;
			a.Ldrh(w0, Field(VI(is)));
			if (op == Lower::Ilw)
			{
				const s32 imm = static_cast<s32>(code << 21) >> 21;
				a.Add(w0, w0, imm);
			}
			// ILWR has no immediate: VI[Is] is already the quadword index.
			a.And(w0, w0, 0x3ff);
			a.Ldr(x1, Field(offsetof(VURegs, Mem)));
			a.Add(x1, x1, Operand(x0, LSL, 4));
			const u32 lane = (mask & 1) ? 3 : (mask & 2) ? 2 :
			                              (mask & 4)     ? 1 :
			                                               0;
			a.Ldrh(w0, MemOperand(x1, lane * 4));
			a.Strh(w0, Field(VI(it))); // Neither instruction creates an arithmetic VI backup.
			return;
		}
		if (op == Lower::Xtop || op == Lower::Xitop)
		{
			if (!it)
				return;
			// THREAD_VU1 (MTVU) redirects the VIF1 registers this pipe reads to the
			// snapshot the VU1 thread owns; baked in at compile time like the
			// Xgkick/CHECK_XGKICKHACK decode gate above, since the setting can't
			// change while this block is executing.
			VIFregisters& vifRegs = THREAD_VU1 ? vu1Thread.vifRegs : vif1Regs;
			const u32* const src = op == Lower::Xtop ? &vifRegs.top : &vifRegs.itop;
			a.Mov(x0, reinterpret_cast<uintptr_t>(src));
			a.Ldr(w0, MemOperand(x0));
			a.Strh(w0, Field(VI(it)));
			return;
		}
		if (op == Lower::Isw)
		{
			// Unlike ILW's single selected lane, ISW writes each masked lane
			// independently (X/Y/Z/W are separate addresses), so it still writes
			// when It == 0 and StoreMasked's usual broadcast-then-mask fits directly.
			const s32 imm = static_cast<s32>(code << 21) >> 21;
			a.Ldrh(w0, Field(VI(is)));
			a.Add(w0, w0, imm);
			a.And(w0, w0, 0x3ff);
			a.Ldr(x1, Field(offsetof(VURegs, Mem)));
			a.Add(x1, x1, Operand(x0, LSL, 4));
			a.Ldrh(w0, Field(VI(it)));
			a.Dup(v0.V4S(), w0);
			StoreMasked(a, v0, MemOperand(x1), mask);
			return;
		}
		if (op == Lower::Move || op == Lower::Mr32 || op == Lower::Mfir)
		{
			if (!ft)
				return;
			if (op == Lower::Mfir)
			{
				a.Ldrsh(w0, Field(VI(is)));
				a.Dup(v0.V4S(), w0);
			}
			else
			{
				LoadVector(a, cache, q0, fs);
				if (op == Lower::Mr32)
					a.Ext(v0.V16B(), v0.V16B(), v0.V16B(), 4);
			}
			StoreVector(a, cache, v0, ft, mask);
			return;
		}
		if (op == Lower::Lqi || op == Lower::Sqi || op == Lower::Lqd || op == Lower::Sqd)
		{
			const bool load = op == Lower::Lqi || op == Lower::Lqd;
			const bool decrement = op == Lower::Lqd || op == Lower::Sqd;
			const u32 base = load ? is : it;
			// Preserve the interpreter's guards, including the full encoded register
			// fields used by LQI/SQI/SQD. Even a suppressed update creates a backup.
			const bool update = (load ? (decrement ? is : fs) : ft) != 0;
			BackupVI(a, base);
			a.Ldrh(w2, Field(VI(base)));
			if (decrement && update)
				a.Sub(w2, w2, 1);
			if (!load || ft)
			{
				a.And(w0, w2, 0x3ff);
				a.Ldr(x1, Field(offsetof(VURegs, Mem)));
				a.Add(x1, x1, Operand(x0, LSL, 4));
				if (load)
				{
					a.Ldr(q0, MemOperand(x1));
					StoreVector(a, cache, v0, ft, mask);
				}
				else
				{
					LoadVector(a, cache, q0, fs);
					StoreMasked(a, v0, MemOperand(x1), mask);
				}
			}
			if (update)
			{
				if (!decrement)
					a.Add(w2, w2, 1);
				a.Strh(w2, Field(VI(base)));
			}
			return;
		}
		if (op == Lower::Lq || op == Lower::Sq)
		{
			if (op == Lower::Lq && !ft)
				return;
			const s32 imm = static_cast<s32>(code << 21) >> 21;
			a.Ldrh(w0, Field(VI(op == Lower::Lq ? is : it)));
			a.Add(w0, w0, imm);
			a.And(w0, w0, 0x3ff);
			a.Ldr(x1, Field(offsetof(VURegs, Mem)));
			a.Add(x1, x1, Operand(x0, LSL, 4));
			if (op == Lower::Lq)
			{
				a.Ldr(q0, MemOperand(x1));
				StoreVector(a, cache, v0, ft, mask);
			}
			else
			{
				LoadVector(a, cache, q0, fs);
				StoreMasked(a, v0, MemOperand(x1), mask);
			}
			return;
		}
		const bool immediate = op == Lower::Iaddiu || op == Lower::Isubiu || op == Lower::Iaddi || op == Lower::Mtir;
		const u32 dest = immediate ? it : id;
		if (!dest)
			return;
		BackupVI(a, dest);
		if (op == Lower::Mtir)
		{
			if (cache.slots[fs] >= 0)
				a.Umov(w0, cache.Host(fs).V8H(), ((code >> 21) & 3) * 2);
			else
				a.Ldrh(w0, Field(VF(fs) + ((code >> 21) & 3) * 4));
		}
		else
		{
			a.Ldrh(w0, Field(VI(is)));
			if (immediate)
			{
				const s32 imm = op == Lower::Iaddi ? static_cast<s32>(code << 21) >> 27 : ((code >> 10) & 0x7800) | (code & 0x7ff);
				if (op == Lower::Isubiu)
					a.Sub(w0, w0, imm);
				else
					a.Add(w0, w0, imm);
			}
			else
			{
				a.Ldrh(w1, Field(VI(it)));
				switch (op)
				{
					case Lower::Iadd:
						a.Add(w0, w0, w1);
						break;
					case Lower::Isub:
						a.Sub(w0, w0, w1);
						break;
					case Lower::Iand:
						a.And(w0, w0, w1);
						break;
					case Lower::Ior:
						a.Orr(w0, w0, w1);
						break;
					default:
						break;
				}
			}
		}
		a.Strh(w0, Field(VI(dest)));
	}

	// q28..q31 and x25..x28 are reserved for deferred pipeline state.
	void EmitPair(MacroAssembler& a, const VectorCache& cache, const Instruction& ins, bool publish_code)
	{
		const bool immediate = ins.upper & 0x80000000;
		const bool discard = !immediate && ins.uregs.VFwrite && ins.uregs.VFwrite == ins.lregs.VFwrite;
		const u32 backup = !immediate && !discard && ins.uregs.VFwrite &&
		                           (ins.uregs.VFwrite == ins.lregs.VFread0 || ins.uregs.VFwrite == ins.lregs.VFread1) ?
		                       ins.uregs.VFwrite :
		                       0;
		if (backup)
			LoadVector(a, cache, q27, backup);
		// WAITQ's stall-and-retire must land before the paired upper instruction
		// runs: VU1microInterp.cpp calls _vuTestLowerStalls/_vuTestPipes ahead of
		// _vu1ExecUpper, precisely so an upper op broadcasting Q in the same pair
		// observes the freshly retired value instead of whatever was pending
		// beforehand. EmitLower's own Waitq case still runs afterward but is then
		// a no-op (fdiv.enable is already clear by the time it gets there).
		if (!immediate && DecodeLower(ins.lower) == Lower::Waitq)
			EmitFDIVStall(a);
		if (publish_code)
			StoreWord(a, ins.upper, offsetof(VURegs, code));
		EmitUpper(a, cache, ins.upper);
		if (immediate)
			StoreWord(a, ins.lower, VI(REG_I));
		else if (!discard)
		{
			if (backup)
			{
				LoadVector(a, cache, q26, backup);
				StoreVector(a, cache, q27, backup);
			}
			if (publish_code)
				StoreWord(a, ins.lower, offsetof(VURegs, code));
			if (DecodeLower(ins.lower) != Lower::Branch && !IsIntegerBranch(DecodeLower(ins.lower)) &&
				DecodeLower(ins.lower) != Lower::Xgkick && !IsRegisterBranch(DecodeLower(ins.lower)))
				EmitLower(a, cache, ins.lower);
			if (backup)
				StoreVector(a, cache, q26, backup);
		}
	}

	void EmitControlFlow(MacroAssembler& a, const Block& block, u32 index)
	{
		const auto& ins = block.instructions[index];
		if (!(ins.upper & 0x80000000) && DecodeLower(ins.lower) == Lower::Branch)
		{
			const s32 displacement = (static_cast<s32>(ins.lower << 21) >> 21) * 8;
			StoreWord(a, (ins.pc + 8 + displacement) & VU1_PROGMASK, offsetof(VURegs, branchpc));
			StoreWord(a, 1, offsetof(VURegs, branch));
		}
		else if (!(ins.upper & 0x80000000) && IsIntegerBranch(DecodeLower(ins.lower)))
		{
			Label done;
			const auto load_operand = [&](const Register& dest, u32 reg) {
				Label current;
				a.Ldrsh(dest, Field(VI(reg)));
				a.Ldrb(w9, Field(offsetof(VURegs, VIBackupCycles)));
				a.Cbz(w9, &current);
				a.Ldr(w9, Field(offsetof(VURegs, VIRegNumber)));
				a.Cmp(w9, reg);
				a.B(ne, &current);
				a.Ldrsh(dest, Field(offsetof(VURegs, VIOldValue)));
				a.Bind(&current);
			};
			load_operand(w0, (ins.lower >> 11) & 15);
			const Lower op = DecodeLower(ins.lower);
			if (op == Lower::Ibgtz || op == Lower::Ibltz || op == Lower::Ibgez || op == Lower::Iblez)
			{
				// Skip (to done) on the condition opposite the one that takes the branch.
				a.Cmp(w0, 0);
				Condition skip;
				switch (op)
				{
					case Lower::Ibgtz:
						skip = le;
						break;
					case Lower::Ibltz:
						skip = ge;
						break;
					case Lower::Ibgez:
						skip = lt;
						break;
					default: // Iblez
						skip = gt;
						break;
				}
				a.B(skip, &done);
			}
			else
			{
				load_operand(w1, (ins.lower >> 16) & 15);
				a.Cmp(w0, w1);
				a.B(op == Lower::Ibeq ? ne : eq, &done);
			}
			const s32 displacement = (static_cast<s32>(ins.lower << 21) >> 21) * 8;
			StoreWord(a, (ins.pc + 8 + displacement) & VU1_PROGMASK, offsetof(VURegs, branchpc));
			StoreWord(a, 1, offsetof(VURegs, branch));
			a.Bind(&done);
		}
		else if (!(ins.upper & 0x80000000) && IsRegisterBranch(DecodeLower(ins.lower)))
		{
			const Lower op = DecodeLower(ins.lower);
			const u32 it_reg = (ins.lower >> 16) & 15;
			if (op == Lower::Bal)
			{
				const s32 displacement = (static_cast<s32>(ins.lower << 21) >> 21) * 8;
				StoreWord(a, (ins.pc + 8 + displacement) & VU1_PROGMASK, offsetof(VURegs, branchpc));
			}
			else
			{
				// JR/JALR: runtime target from VI[Is].US[0] * 8. Unlike the integer
				// branches above, _vuJR/_vuJALR read the live register directly with
				// no VI-backup bypass, so this doesn't apply one either.
				const u32 is_reg = (ins.lower >> 11) & 15;
				a.Ldrh(w0, Field(VI(is_reg)));
				a.Lsl(w0, w0, 3);
				a.And(w0, w0, VU1_PROGMASK);
				a.Str(w0, Field(offsetof(VURegs, branchpc)));
			}
			if (op != Lower::Jr && it_reg)
			{
				// Compile() never lets a JR/JALR/BAL land in a pending delay slot
				// (it bails out of the trace instead), so VU->branch==1 can't be true
				// here. _vuJALR/_vuBAL then link to (TPC+8)/8, but TPC has already
				// been advanced past this instruction by the time they read it, so
				// the return address is two pairs ahead: this instruction's own
				// address + 16, not + 8.
				a.Mov(w0, (ins.pc + 16) / 8);
				a.Strh(w0, Field(VI(it_reg)));
			}
			StoreWord(a, 1, offsetof(VURegs, branch));
		}
		else if (block.delay[index])
		{
			const auto& prev = block.instructions[index - 1];
			const bool register_target = !(prev.upper & 0x80000000) && IsRegisterBranch(DecodeLower(prev.lower));
			StoreWord(a, 0, offsetof(VURegs, branch));
			if (register_target)
			{
				// The preceding JR/JALR/BAL already resolved branchpc itself (at
				// runtime for JR/JALR, statically for BAL); Compile() never gives this
				// delay slot a compile-time next_pc to fall back on, so copy it instead
				// of storing a literal.
				a.Ldr(w9, Field(offsetof(VURegs, branchpc)));
				a.Str(w9, Field(VI(REG_TPC)));
			}
			else
			{
				StoreWord(a, block.next_pc[index], VI(REG_TPC));
			}
		}
	}

	void EmitFmacMetadata(MacroAssembler& a, const Instruction& ins)
	{
		const bool upper = ins.uregs.pipe == VUPIPE_FMAC;
		const bool lower = ins.lregs.pipe == VUPIPE_FMAC;
		const u64 regs = (upper ? ins.uregs.VFwrite : 0) | (u64(lower ? ins.lregs.VFwrite : 0) << 32);
		const u64 flags = (upper ? ins.uregs.VIwrite : 0) | (lower ? ins.lregs.VIwrite : 0);
		a.Mov(x9, regs);
		a.Str(x9, MemOperand(x0, offsetof(fmacPipe, regupper)));
		a.Mov(x9, flags | (u64(upper ? ins.uregs.VFwxyzw : 0) << 32));
		a.Str(x9, MemOperand(x0, offsetof(fmacPipe, flagreg)));
		a.Mov(x9, lower ? ins.lregs.VFwxyzw : 0);
		a.Str(x9, MemOperand(x0, offsetof(fmacPipe, xyzwlower))); // also clear padding
	}

	void EmitFinish(MacroAssembler& a, const Instruction& ins)
	{
		static_assert(sizeof(fmacPipe) == 48 && offsetof(fmacPipe, reglower) == 4 &&
					  offsetof(fmacPipe, flagreg) == 8 && offsetof(fmacPipe, xyzwupper) == 12 &&
					  offsetof(fmacPipe, xyzwlower) == 16 && offsetof(fmacPipe, sCycle) == 24 &&
					  offsetof(fmacPipe, Cycle) == 32 && offsetof(fmacPipe, macflag) == 36 &&
					  offsetof(fmacPipe, statusflag) == 40 && offsetof(fmacPipe, clipflag) == 44);
		const bool upper = ins.uregs.pipe == VUPIPE_FMAC;
		const bool lower = ins.lregs.pipe == VUPIPE_FMAC;
		if (!upper && !lower)
			return;
		// The supported lower instructions either use FMAC or have zero IALU latency.
		// Emit the same queue entry as _vuClearFMAC + _vuAddUpper/LowerStalls.
		// w12 keeps the original fmacwritepos around so the increment below
		// doesn't need to reload what x0's address computation already consumed.
		a.Ldr(w12, Field(offsetof(VURegs, fmacwritepos)));
		a.Mov(w1, sizeof(fmacPipe));
		a.Madd(x0, x12, x1, x19);
		a.Add(x0, x0, offsetof(VURegs, fmac));
		EmitFmacMetadata(a, ins);
		a.Str(x26, MemOperand(x0, offsetof(fmacPipe, sCycle)));
		a.Mov(w9, 4);
		a.Ldr(w10, Field(offsetof(VURegs, macflag)));
		a.Stp(w9, w10, MemOperand(x0, offsetof(fmacPipe, Cycle)));
		a.Ldr(w9, Field(offsetof(VURegs, statusflag)));
		a.Ldr(w10, Field(offsetof(VURegs, clipflag)));
		a.Stp(w9, w10, MemOperand(x0, offsetof(fmacPipe, statusflag)));
		a.Ldr(w9, Field(offsetof(VURegs, fmaccount)));
		a.Add(w9, w9, 1);
		a.Str(w9, Field(offsetof(VURegs, fmaccount)));
		a.Add(w9, w12, 1);
		a.And(w9, w9, 3);
		a.Str(w9, Field(offsetof(VURegs, fmacwritepos)));
	}

	void EmitKick(MacroAssembler& a, u32 code)
	{
		Label fresh;
		a.Ldr(w9, Field(offsetof(VURegs, xgkickenable)));
		a.Cbz(w9, &fresh);
		a.Str(x26, Field(offsetof(VURegs, cycle)));
		a.Mov(x16, reinterpret_cast<uintptr_t>(s_pipeline.flush_kick));
		a.Blr(x16);
		a.Ldr(x26, Field(offsetof(VURegs, cycle)));
		EmitBlockConstants(a);
		a.Bind(&fresh);
		// Read VI after flushing, matching the reference lower-op ordering.
		a.Ldrh(w0, Field(VI((code >> 11) & 15)));
		a.And(w0, w0, 0x3ff);
		a.Lsl(w0, w0, 4);
		a.Str(w0, Field(offsetof(VURegs, xgkickaddr)));
		a.Mov(w9, VU1_MEMSIZE);
		a.Sub(w9, w9, w0);
		a.Str(w9, Field(offsetof(VURegs, xgkickdiff)));
		StoreWord(a, VURegs::XgkickPacket, offsetof(VURegs, xgkickenable));
		StoreWord(a, 0, offsetof(VURegs, xgkicksizeremaining));
		StoreWord(a, 0, offsetof(VURegs, xgkickendpacket));
		StoreWord(a, 1, offsetof(VURegs, xgkickcyclecount));
		a.Str(x26, Field(offsetof(VURegs, xgkicklastcycle)));
		// VPU_STAT's VGW bit is EE-thread state; the MTVU thread must not write it.
		if (!THREAD_VU1)
		{
			a.Mov(x0, reinterpret_cast<uintptr_t>(&VU0.VI[REG_VPU_STAT].UL));
			a.Ldr(w9, MemOperand(x0));
			a.Orr(w9, w9, 1 << 12);
			a.Str(w9, MemOperand(x0));
		}
	}

	void EmitIntegerIssue(MacroAssembler& a, const Instruction& ins)
	{
		if (ins.lregs.pipe != VUPIPE_IALU || !ins.lregs.cycles)
			return;
		a.Ldr(w0, Field(offsetof(VURegs, ialuwritepos)));
		a.Mov(w1, sizeof(ialuPipe));
		a.Madd(x1, x0, x1, x19);
		a.Add(x1, x1, offsetof(VURegs, ialu));
		a.Str(x26, MemOperand(x1, offsetof(ialuPipe, sCycle)));
		a.Mov(w9, ins.lregs.cycles);
		a.Str(w9, MemOperand(x1, offsetof(ialuPipe, Cycle)));
		a.Mov(w9, ins.lregs.VIwrite);
		a.Str(w9, MemOperand(x1, offsetof(ialuPipe, reg)));
		a.Add(w0, w0, 1);
		a.And(w0, w0, 3);
		a.Str(w0, Field(offsetof(VURegs, ialuwritepos)));
		a.Ldr(w0, Field(offsetof(VURegs, ialucount)));
		a.Add(w0, w0, 1);
		a.Str(w0, Field(offsetof(VURegs, ialucount)));
	}

	bool HasFmac(const Instruction& ins)
	{
		return ins.uregs.pipe == VUPIPE_FMAC || ins.lregs.pipe == VUPIPE_FMAC;
	}

	void AnalyzeRetirement(Block& block)
	{
		// Ages saturate at four (already retired). -1 represents an age which
		// still depends on incoming timing. Every pair advances at least one cycle.
		std::array<int, MaxInstructions> ages{};
		ages.fill(-1);
		u32 integer_ready = 0, fdiv_ready = 0, efu_ready = 0;
		for (u32 i = 0; i < block.count; i++)
		{
			const auto& ins = block.instructions[i];
			int cycles = -1;
			if (i >= 3)
			{
				if (!ins.readsVF || ins.dependency == 0)
					cycles = 1;
				else
				{
					int slots = ins.dependency;
					for (u32 distance = 1; distance <= 3; distance++)
					{
						if (HasFmac(block.instructions[i - distance]) && --slots == 0)
						{
							const int age = ages[i - distance];
							if (age >= 0)
								cycles = std::max(1, 4 - age);
							break;
						}
					}
				}
			}
			// VI waits and kick/transfer callbacks can change timing. Forget exact
			// producer ages until enough new pairs establish a known schedule.
			if ((ins.lregs.pipe == VUPIPE_BRANCH && ins.lregs.VIread) ||
				ins.lregs.pipe == VUPIPE_XGKICK || (i && block.instructions[i - 1].lregs.pipe == VUPIPE_XGKICK))
				cycles = -1;
			// Touching Q or P while an entry issued earlier is still in the FDIV/EFU
			// pipe stalls the pair until that entry retires (_vuTestFDIVStalls), so
			// VURegs::cycle jumps by an amount nothing here can predict. Hand the age
			// tracking below an unknown advance instead of the nominal one; this is
			// what keeps the pairs after a WAITQ or a Q read off the schedule until
			// their producers' ages have provably recovered. Read before this pair's
			// own issue updates fdiv_ready/efu_ready, since a pipe it arms itself
			// cannot be what it stalls on.
			constexpr u32 kQP = (1 << REG_Q) | (1 << REG_P);
			if ((i < fdiv_ready || i < efu_ready) &&
				((ins.uregs.VIread | ins.lregs.VIread | ins.uregs.VIwrite | ins.lregs.VIwrite) & kQP))
				cycles = -1;
			if (ins.lregs.pipe == VUPIPE_IALU && ins.lregs.cycles)
				integer_ready = i + 5;
			// Divides are frequent enough in transform code that excluding their whole
			// latency from scheduling costs far more than retiring the pipe's single
			// slot inline: those pairs carry fdiv_pending instead and do it themselves
			// (see EmitScheduledPrepare). Deferred regions still cannot cover them,
			// since they hold the status flag in a host register.
			if (ins.lregs.pipe == VUPIPE_FDIV && ins.lregs.cycles)
				fdiv_ready = i + ins.lregs.cycles + 1;
			// The EFU pipe's single slot (ESADD..EEXP, retiring into P) is rare enough
			// that it keeps the simpler treatment: stay generic for its full latency.
			if (ins.lregs.pipe == VUPIPE_EFU && ins.lregs.cycles)
				efu_ready = i + ins.lregs.cycles + 1;
			if (i >= 7 && cycles > 0 && i >= integer_ready && i >= efu_ready &&
				!(ins.lregs.pipe == VUPIPE_BRANCH && ins.lregs.VIread))
			{
				RetirementSchedule plan{static_cast<u8>(cycles)};
				plan.fdiv_pending = i < fdiv_ready;
				for (u32 j = i - 4; j < i; j++)
				{
					const auto& producer = block.instructions[j];
					if (!HasFmac(producer))
						continue;
					if (ages[j] < 0)
					{
						plan.cycles = 0;
						break;
					}
					if (ages[j] >= 4)
						continue;
					if (ages[j] + cycles < 4)
						plan.remaining++;
					else
					{
						if ((producer.uregs.VIwrite | producer.lregs.VIwrite) &
							((1 << REG_STATUS_FLAG) | (1 << REG_CLIP_FLAG)))
							plan.cycles = 0;
						plan.retired++;
					}
				}
				// Deferred regions keep retired flags in host registers. Materialize
				// them before an instruction observes architectural flag, Q or P
				// state. Q and P retire from the FDIV/EFU pipes on the same
				// generic per-pair path. An FDIV/EFU issue also stamps its own
				// sCycle and stalls on the previous entry, so it needs the exact
				// cycle rather than a batched one.
				if (((ins.uregs.VIread | ins.lregs.VIread) &
						((1 << REG_STATUS_FLAG) | (1 << REG_MAC_FLAG) | (1 << REG_CLIP_FLAG) | (1 << REG_Q) | (1 << REG_P))) ||
					(ins.lregs.VIwrite & ((1 << REG_Q) | (1 << REG_P))))
					plan.cycles = 0;
				block.schedule[i] = plan;
			}
			for (u32 j = 0; j < i; j++)
			{
				if (cycles > 0 && ages[j] >= 0)
					ages[j] = std::min(4, ages[j] + cycles);
				else if (static_cast<int>(i - j - 1) + std::max(1, cycles) >= 4 ||
						 (ages[j] >= 0 && ages[j] + std::max(1, cycles) >= 4))
					ages[j] = 4;
				else
					ages[j] = -1;
			}
			ages[i] = 0;
		}
	}

	void EmitScheduleReadiness(MacroAssembler& a)
	{
		// Bit 0 validates incoming FMAC timing and excludes callbacks. Bit 1
		// additionally permits scheduled execution once special queues drain.
		Label done;
		a.Cmp(w25, 1);
		a.B(ne, &done);
		for (size_t offset : {offsetof(VURegs, fdiv) + offsetof(fdivPipe, enable),
				 offsetof(VURegs, efu) + offsetof(efuPipe, enable), offsetof(VURegs, ialucount)})
		{
			a.Ldr(w9, Field(offset));
			a.Cbnz(w9, &done);
		}
		a.Mov(w25, 3);
		a.Bind(&done);
	}

	void EmitScheduleGuard(MacroAssembler& a)
	{
		Label done, loop, ready;
		a.Mov(w25, 0);
		// XGKICK may call C++ and change state during the prefix. Promotion
		// requires a fresh guard after the explicit packet boundary.
		a.Ldr(w9, Field(offsetof(VURegs, xgkickenable)));
		a.Cbnz(w9, &done);
		a.Ldr(x9, Field(offsetof(VURegs, cycle)));
		// Each pair can advance four cycles, including dependency stalls.
		a.Cmn(x9, MaxInstructions * 4 + 4);
		a.B(hs, &done);
		a.Ldr(w10, Field(offsetof(VURegs, fmaccount)));
		a.Cmp(w10, 4);
		a.B(hi, &done);
		a.Ldr(w11, Field(offsetof(VURegs, fmacreadpos)));
		a.Cmp(w11, 3);
		a.B(hi, &done);
		a.Ldr(w12, Field(offsetof(VURegs, fmacwritepos)));
		a.Add(w13, w11, w10);
		a.And(w13, w13, 3);
		a.Cmp(w12, w13);
		a.B(ne, &done);
		a.Cbz(w10, &ready);
		a.Mov(x14, 0);
		a.Bind(&loop);
		a.Mov(w12, sizeof(fmacPipe));
		a.Madd(x12, x11, x12, x19);
		a.Add(x12, x12, offsetof(VURegs, fmac));
		a.Ldr(x13, MemOperand(x12, offsetof(fmacPipe, sCycle)));
		a.Cmp(x13, x9);
		a.B(hi, &done);
		// At most one entry is issued per cycle. This also prevents a queue
		// overflow in the generic prefix before incoming entries have matured.
		a.Cmp(x13, x14);
		a.B(lo, &done);
		a.Add(x14, x13, 1);
		a.Ldr(w13, MemOperand(x12, offsetof(fmacPipe, Cycle)));
		a.Cmp(w13, 4);
		a.B(hi, &done);
		a.Add(w11, w11, 1);
		a.And(w11, w11, 3);
		a.Subs(w10, w10, 1);
		a.B(ne, &loop);
		a.Bind(&ready);
		a.Mov(w25, 1);
		EmitScheduleReadiness(a);
		a.Bind(&done);
	}

	void EmitBackupCountdown(MacroAssembler& a, u32 cycles)
	{
		a.Ldrb(w9, Field(offsetof(VURegs, VIBackupCycles)));
		if (cycles == 1)
		{
			a.Cmp(w9, 0);
			a.Cset(w10, ne);
		}
		else
		{
			a.Mov(w10, cycles);
			a.Cmp(w9, w10);
			a.Csel(w10, w9, w10, lo);
		}
		a.Sub(w9, w9, w10);
		a.Strb(w9, Field(offsetof(VURegs, VIBackupCycles)));
	}

	void EmitScheduledPrepare(MacroAssembler& a, const Block& block, u32 index)
	{
		const auto& plan = block.schedule[index];
		a.Add(x26, x26, plan.cycles);
		StoreWord(a, block.instructions[index].pc + 8, VI(REG_TPC));
		if (plan.retired)
		{
			a.Ldr(w10, Field(offsetof(VURegs, fmacreadpos)));
			a.Ldr(w13, Field(VI(REG_STATUS_FLAG)));
			a.And(w13, w13, 0xff0);
			for (u32 i = 0; i < plan.retired; i++)
			{
				a.Mov(w11, sizeof(fmacPipe));
				a.Madd(x12, x10, x11, x19);
				a.Add(x12, x12, offsetof(VURegs, fmac));
				a.Ldr(w11, MemOperand(x12, offsetof(fmacPipe, statusflag)));
				a.And(w11, w11, 15);
				// All sticky bits survive, but only the last MAC/non-sticky flags
				// are observable after retirement. No flag readers run between slots.
				a.Orr(w13, w13, Operand(w11, LSL, 6));
				if (i + 1 == plan.retired)
				{
					a.Orr(w13, w13, w11);
					a.Str(w13, Field(VI(REG_STATUS_FLAG)));
					a.Ldr(w11, MemOperand(x12, offsetof(fmacPipe, macflag)));
					a.Str(w11, Field(VI(REG_MAC_FLAG)));
				}
				a.Add(w10, w10, 1);
				a.And(w10, w10, 3);
			}
			a.Str(w10, Field(offsetof(VURegs, fmacreadpos)));
			StoreWord(a, plan.remaining, offsetof(VURegs, fmaccount));
		}
		if (plan.fdiv_pending)
		{
			// The FDIV slot the generic preparation would have drained, in
			// VUPipeline::Retire's order: after the FMAC writeback above, because
			// both merge into VI[REG_STATUS_FLAG]. Cheap when nothing is due, which
			// is the common case even inside a divide's latency.
			Label end;
			constexpr size_t offset = offsetof(VURegs, fdiv);
			a.Ldr(w10, Field(offset + offsetof(fdivPipe, enable)));
			a.Cbz(w10, &end);
			a.Ldr(x10, Field(offset + offsetof(fdivPipe, sCycle)));
			a.Ldr(w11, Field(offset + offsetof(fdivPipe, Cycle)));
			a.Sub(x10, x26, x10);
			a.Cmp(x10, x11);
			a.B(lo, &end);
			a.Str(wzr, Field(offset + offsetof(fdivPipe, enable)));
			a.Ldr(w10, Field(offset + offsetof(fdivPipe, reg)));
			a.Str(w10, Field(VI(REG_Q)));
			a.Ldr(w10, Field(VI(REG_STATUS_FLAG)));
			a.And(w10, w10, 0xfcf);
			a.Ldr(w11, Field(offset + offsetof(fdivPipe, statusflag)));
			a.And(w11, w11, 0xc30);
			a.Orr(w10, w10, w11);
			a.Str(w10, Field(VI(REG_STATUS_FLAG)));
			a.Bind(&end);
		}
		// Special work has drained at scheduled pairs. ILW clears readiness and
		// keeps retirement generic until its queue drains; broader games need proper testing.
		EmitBackupCountdown(a, plan.cycles);
	}

	// A fully budgeted, callback-free region can keep its four FMAC flag
	// snapshots in q28..q31. Queue metadata and cycle stamps are compile-time
	// facts and only need materializing when returning to the dispatcher.
	void EmitDeferredRegion(MacroAssembler& a, const Block& block, u32 first, u32 end)
	{
		static_assert(offsetof(VURegs, statusflag) == offsetof(VURegs, macflag) + 4 &&
					  offsetof(VURegs, clipflag) == offsetof(VURegs, macflag) + 8);
		a.Ldr(w27, Field(offsetof(VURegs, fmacwritepos)));
		a.Ldr(w25, Field(VI(REG_STATUS_FLAG)));
		a.Ldr(w28, Field(VI(REG_MAC_FLAG)));
		auto slot_address = [&](u32 relative) {
			a.Add(w0, w27, relative & 3);
			a.And(w0, w0, 3);
			a.Mov(w1, sizeof(fmacPipe));
			a.Madd(x0, x0, x1, x19);
			a.Add(x0, x0, offsetof(VURegs, fmac));
		};
		for (u32 slot = 0; slot < 4; slot++)
		{
			slot_address(slot);
			a.Ldr(VRegister(28 + slot, 64), MemOperand(x0, offsetof(fmacPipe, macflag)));
			a.Ldr(w9, MemOperand(x0, offsetof(fmacPipe, clipflag)));
			a.Ins(VRegister(28 + slot, 128).V4S(), 2, w9);
		}
		struct Slot
		{
			int writer = -1;
			u32 issue_cycle = 0;
		};
		std::array<Slot, 4> slots{};
		u32 issued = 0, elapsed = 0, backup_cycles = 0;
		for (u32 i = first; i < end; i++)
		{
			const auto& plan = block.schedule[i];
			const auto& ins = block.instructions[i];
			elapsed += plan.cycles;
			a.Add(x26, x26, plan.cycles);
			for (u32 j = 0; j < plan.retired; j++)
			{
				const u32 slot = (issued - plan.remaining - plan.retired + j) & 3;
				const VRegister flags(28 + slot, 128);
				a.Umov(w9, flags.V4S(), 1);
				a.And(w9, w9, 15);
				a.And(w25, w25, 0xff0);
				a.Orr(w25, w25, w9);
				a.Orr(w25, w25, Operand(w9, LSL, 6));
				if (j + 1 == plan.retired)
					a.Umov(w28, flags.V4S(), 0);
			}
			// Only an integer write can inspect/reset the backup countdown in a
			// supported pair. Accumulate time until that observer or the block exit.
			backup_cycles += plan.cycles;
			if (ins.lregs.VIwrite & 0xffff)
			{
				EmitBackupCountdown(a, std::min(backup_cycles, 255u));
				backup_cycles = 0;
			}
			EmitPair(a, block.cache, ins, false);
			EmitControlFlow(a, block, i);
			if (HasFmac(ins))
			{
				const u32 slot = issued++ & 3;
				// Only the first three lanes are used; the fourth is ignored.
				a.Ldr(VRegister(28 + slot, 128), Field(offsetof(VURegs, macflag)));
				slots[slot].writer = i;
				slots[slot].issue_cycle = elapsed;
			}
		}
		if (backup_cycles)
			EmitBackupCountdown(a, std::min(backup_cycles, 255u));
		// Restore even inactive overwritten slots: save states and differential
		// execution observe the complete architectural queue, not just live entries.
		for (u32 slot = 0; slot < 4; slot++)
		{
			if (slots[slot].writer < 0)
				continue;
			slot_address(slot);
			EmitFmacMetadata(a, block.instructions[slots[slot].writer]);
			a.Sub(x9, x26, elapsed - slots[slot].issue_cycle);
			a.Str(x9, MemOperand(x0, offsetof(fmacPipe, sCycle)));
			a.Mov(w9, 4);
			a.Str(w9, MemOperand(x0, offsetof(fmacPipe, Cycle)));
			a.Str(VRegister(28 + slot, 64), MemOperand(x0, offsetof(fmacPipe, macflag)));
			a.Umov(w9, VRegister(28 + slot, 128).V4S(), 2);
			a.Str(w9, MemOperand(x0, offsetof(fmacPipe, clipflag)));
		}
		const auto& last = block.instructions[end - 1];
		const u32 live = block.schedule[end - 1].remaining + HasFmac(last);
		a.Add(w9, w27, issued & 3);
		a.And(w9, w9, 3);
		a.Str(w9, Field(offsetof(VURegs, fmacwritepos)));
		a.Sub(w9, w9, live);
		a.And(w9, w9, 3);
		a.Str(w9, Field(offsetof(VURegs, fmacreadpos)));
		StoreWord(a, live, offsetof(VURegs, fmaccount));
		a.Str(w25, Field(VI(REG_STATUS_FLAG)));
		a.Str(w28, Field(VI(REG_MAC_FLAG)));
		// A JR/JALR/BAL delay slot ending the region already published the right
		// TPC itself (EmitControlFlow's block.delay case, run above in this same
		// loop): its target isn't the compile-time-constant next_pc this generic
		// epilogue otherwise stores, so don't clobber it with that stale value.
		const bool last_is_register_branch_delay = block.delay[end - 1] && end >= 2 &&
			!(block.instructions[end - 2].upper & 0x80000000) &&
			IsRegisterBranch(DecodeLower(block.instructions[end - 2].lower));
		if (!last_is_register_branch_delay)
			StoreWord(a, block.next_pc[end - 1], VI(REG_TPC));
		const bool upper_code = (last.upper & 0x80000000) ||
		                        (last.uregs.VFwrite && last.uregs.VFwrite == last.lregs.VFwrite);
		StoreWord(a, upper_code ? last.upper : last.lower, offsetof(VURegs, code));
	}

	Block& Compile(u32 pc)
	{
		if (static_cast<size_t>(s_end - s_write) < MaxBlockBytes)
			InvalidateAll();
		auto block = std::make_unique<Block>();
		const u32 saved_code = VU1.code;
		// Decode static B and taken integer-branch edges with one vector-cache and
		// pipeline schedule. Reachable source bytes are validated before entry.
		// A repeated entry can loop natively; other repeated PCs, unsupported
		// pairs and the size limit end the trace.
		u32 next_pc = pc, branch_target = 0;
		bool pending_branch = false;
		// JR/JALR/BAL: the target isn't a compile-time constant to continue tracing
		// into (JR/JALR read it from a register; BAL's static target is deliberately
		// not exploited, to keep one code path). EmitControlFlow resolves branchpc
		// itself instead of relying on a precomputed branch_target/next_pc, so the
		// trace simply ends once their delay slot has been emitted.
		bool pending_branch_terminal = false;
		std::array<bool, VU1_PROGSIZE / 8> visited{};
		for (u32 i = 0; i < MaxInstructions && next_pc < VU1_PROGSIZE; i++)
		{
			auto& ins = block->instructions[i];
			if (visited[next_pc / 8])
			{
				// Reenter only this trace's incoming-state path, with no unresolved
				// delay. Other internal targets need their own state contract.
				block->loops_to_entry = next_pc == pc && !pending_branch;
				break;
			}
			ins.pc = next_pc;
			std::memcpy(&ins.lower, VU1.Micro + ins.pc, 4);
			std::memcpy(&ins.upper, VU1.Micro + ins.pc + 4, 4);
			block->words[i * 2] = ins.lower;
			block->words[i * 2 + 1] = ins.upper;
			if (DecodeUpper(ins.upper).op == Op::Unsupported ||
				(!(ins.upper & 0x80000000) && DecodeLower(ins.lower) == Lower::Unsupported))
				break;
			const bool conditional = !(ins.upper & 0x80000000) && IsIntegerBranch(DecodeLower(ins.lower));
			// Follow the taken edge; the generated guard exits on the other path.
			if (conditional && pending_branch)
				break;
			const bool branch = !(ins.upper & 0x80000000) && DecodeLower(ins.lower) == Lower::Branch;
			if (branch && (pending_branch || i + 1 == MaxInstructions))
				break;
			const bool terminal_branch = !(ins.upper & 0x80000000) && IsRegisterBranch(DecodeLower(ins.lower));
			// A register/link branch found in a pending delay slot (branch-in-branch)
			// isn't natively resolved either, same as the nested-conditional case above.
			if (terminal_branch && (pending_branch || i + 1 == MaxInstructions))
				break;
			visited[ins.pc / 8] = true;
			block->delay[i] = pending_branch;
			const bool finishing_terminal_delay = pending_branch && pending_branch_terminal;
			// branch_target is meaningless for a terminal delay slot; EmitControlFlow
			// recognizes that case from the preceding instruction and ignores it.
			block->next_pc[i] = pending_branch ? branch_target : ins.pc + 8;
			if (pending_branch)
			{
				pending_branch = false;
				pending_branch_terminal = false;
			}
			if (branch || conditional)
			{
				const s32 displacement = (static_cast<s32>(ins.lower << 21) >> 21) * 8;
				branch_target = (ins.pc + 8 + displacement) & VU1_PROGMASK;
				pending_branch = true;
				block->has_branches = true;
			}
			else if (terminal_branch)
			{
				pending_branch = true;
				pending_branch_terminal = true;
				block->has_branches = true;
			}
			next_pc = block->next_pc[i];
			if (pending_branch)
				next_pc &= VU1_PROGMASK; // A delay pair may wrap micro memory.
			if (!block->ranges.empty() && block->ranges.back().pc + block->ranges.back().count * 8 == ins.pc)
				block->ranges.back().count++;
			else
				block->ranges.push_back({ins.pc, i, 1});
			VU1.code = ins.upper;
			VU1regs_UPPER_OPCODE[ins.upper & 0x3f](&ins.uregs);
			if (!(ins.upper & 0x80000000))
			{
				VU1.code = ins.lower;
				VU1regs_LOWER_OPCODE[ins.lower >> 25](&ins.lregs);
				// _vuRegsWAITQ (shared with the interpreter/x86) declares no
				// reads or writes at all, since its only effect is forcing the
				// pending FDIV entry to retire early. Without a VIwrite(Q) of
				// its own, nothing below treats it differently from a plain
				// 1-cycle op, so it can be scheduled or swept into a deferred
				// region as if its timing were fixed and it touched no
				// architectural state — neither is true. Tag it exactly like
				// DIV/SQRT/RSQRT so the existing Q/STATUS/MAC/CLIP exclusion
				// keeps it on the generic (non-deferred) path.
				if (DecodeLower(ins.lower) == Lower::Waitq)
					ins.lregs.VIwrite |= 1 << REG_Q;
				// _vuRegsWAITP has the same gap as _vuRegsWAITQ above, for P
				// instead of Q.
				if (DecodeLower(ins.lower) == Lower::Waitp)
					ins.lregs.VIwrite |= 1 << REG_P;
			}
			for (const _VURegsNum* regs : {&ins.uregs, &ins.lregs})
			{
				// FDIV (DIV/SQRT/RSQRT) and EFU (ESADD..WAITP) reads are also
				// FMAC-hazard checked by the interpreter's _vuTestLowerStalls
				// (_vuTestEFUStalls itself starts by calling _vuTestFMACStalls),
				// same as ordinary FMAC consumers.
				if (regs->pipe != VUPIPE_FMAC && regs->pipe != VUPIPE_FDIV && regs->pipe != VUPIPE_EFU)
					continue;
				if (regs->VFread0)
					ins.readMasks[regs->VFread0] |= regs->VFr0xyzw;
				if (regs->VFread1)
					ins.readMasks[regs->VFread1] |= regs->VFr1xyzw;
			}
			ins.readsVF = std::any_of(ins.readMasks.begin(), ins.readMasks.end(), [](u8 mask) { return mask != 0; });
			if (i >= 3)
			{
				// Four cycles have elapsed since block entry, so incoming FMAC results
				// are ready. Only the preceding three pairs can still cause a stall.
				// All supported FMAC results have the same four-cycle latency: the
				// newest conflicting producer determines the wait for the whole pair.
				ins.dependency = 0;
				int8_t slots = 0;
				for (u32 distance = 1; distance <= 3; distance++)
				{
					const auto& previous = block->instructions[i - distance];
					if (previous.uregs.pipe != VUPIPE_FMAC && previous.lregs.pipe != VUPIPE_FMAC)
						continue;
					slots++;
					if ((previous.uregs.pipe == VUPIPE_FMAC && (ins.readMasks[previous.uregs.VFwrite] & previous.uregs.VFwxyzw)) ||
						(previous.lregs.pipe == VUPIPE_FMAC && (ins.readMasks[previous.lregs.VFwrite] & previous.lregs.VFwxyzw)))
					{
						ins.dependency = slots;
						break;
					}
				}
			}
			block->count++;
			if (finishing_terminal_delay)
				break; // JR/JALR/BAL's delay slot is native; the runtime target is not.
		}
		VU1.code = saved_code;
		if (block->count)
		{
			AnalyzeRetirement(*block);
			AssignVectorCache(*block);
			const VectorCache& cache = block->cache;
			if (!s_pipeline.prepare[0])
			{
				HostSys::BeginCodeWrite();
				s_pipeline = Arm64VU1::CompilePipeline(s_write, s_end - s_write, &_vuXGKICKTransfer, (s_options & 16) != 0);
				HostSys::EndCodeWrite();
				HostSys::FlushInstructionCache(s_write, static_cast<u32>(s_pipeline.size));
				s_write += (s_pipeline.size + 15) & ~size_t(15);
			}
			HostSys::BeginCodeWrite();
			MacroAssembler a(s_write, s_end - s_write);
			// Defer each sufficiently long known region separately. Conditional
			// exits must see materialized queues, without penalizing later regions.
			struct DeferredRegion
			{
				u32 first, end, cycles;
			};
			std::vector<DeferredRegion> regions;
			// A deferred region keeps the status flag in a host register, so it cannot
			// contain a pair that retires the FDIV pipe into the architectural one.
			const auto deferrable = [&](u32 i) {
				return block->schedule[i].cycles && !block->schedule[i].fdiv_pending;
			};
			for (u32 i = 7; i < block->count;)
			{
				if (!deferrable(i))
				{
					i++;
					continue;
				}
				const u32 first = i;
				u32 cycles = 0;
				while (i < block->count && deferrable(i))
					cycles += block->schedule[i++].cycles;
				if (i - first >= 8)
					regions.push_back({first, i, cycles});
			}
			const bool deferred = !regions.empty();
			std::array<Label, MaxInstructions> deferred_entries, resumes;
			Label exit, loop_entry, finished;
			const int saved_size = deferred ? 96 : 80;
			// Save only the d8..d15 registers this block modifies.
			// Round paired saves up to retain 16-byte stack alignment.
			const u32 saved_vectors = (cache.count + 1) & ~1u;
			const int frame_size = saved_size + saved_vectors * 8;
			a.Stp(x19, x20, MemOperand(sp, -frame_size, PreIndex));
			a.Stp(x21, x22, MemOperand(sp, 16));
			a.Stp(x23, x24, MemOperand(sp, 32));
			a.Stp(x25, x26, MemOperand(sp, 48));
			a.Str(lr, MemOperand(sp, 64));
			a.Str(w2, MemOperand(sp, 72));
			if (deferred)
				a.Stp(x27, x28, MemOperand(sp, 80));
			if (cache.count)
				for (u32 slot = 0; slot < saved_vectors; slot += 2)
					a.Stp(VRegister(8 + slot, 64), VRegister(9 + slot, 64), MemOperand(sp, saved_size + slot * 8));
			a.Mov(x19, reinterpret_cast<uintptr_t>(&VU1));
			u32 scheduled_pairs = 0;
			for (u32 i = 7; i < block->count; i++)
				scheduled_pairs += block->schedule[i].cycles != 0;
			// Amortize the entry guard over several scheduled pairs.
			const bool scheduled = scheduled_pairs >= 4;
			if (scheduled)
				EmitScheduleGuard(a);
			// Keep queue insertion and budget checks off the cycle store/load chain.
			a.Ldr(x26, Field(offsetof(VURegs, cycle)));
			a.Mov(x20, x0);
			a.Mov(x21, x1);
			a.Mov(x24, reinterpret_cast<uintptr_t>(cache.offsets.data()));
			for (u32 slot = 0; slot < cache.count; slot++)
				a.Ldr(VRegister(8 + slot, 128), Field(cache.offsets[slot]));
			EmitBlockConstants(a);
			// Share the most frequent helper address in x22; keep preparation code
			// outside the emitted instruction stream to avoid instruction-cache growth.
			const auto& prepare = s_pipeline.prepare;
			std::array<u32, std::size(prepare)> uses{};
			for (u32 i = 0; i < block->count; i++)
			{
				const auto& ins = block->instructions[i];
				uses[ins.readsVF ? ins.dependency + 2 : 0]++;
			}
			const u32 shared_prepare = std::max_element(uses.begin(), uses.end()) - uses.begin();
			a.Mov(x22, reinterpret_cast<uintptr_t>(prepare[shared_prepare]));
			a.Mov(x23, reinterpret_cast<uintptr_t>(block->instructions.data()));
			a.Bind(&loop_entry);
			bool cycle_dirty = false;
			bool readiness_checked = false;
			u32 region_index = 0;
			for (u32 i = 0; i < block->count; i++)
			{
				const auto& ins = block->instructions[i];
				if (region_index < regions.size() && i == regions[region_index].end)
				{
					a.Bind(&resumes[region_index]);
					region_index++;
				}
				const bool region_start = region_index < regions.size() && i == regions[region_index].first;
				const bool schedule_pair = scheduled && block->schedule[i].cycles != 0;
				if (schedule_pair && (!readiness_checked || !block->schedule[i - 1].cycles || region_start))
				{
					EmitScheduleReadiness(a);
					readiness_checked = true;
				}
				if (region_start)
				{
					Label partial;
					a.Tbz(w25, 1, &partial);
					a.Sub(x9, x26, x20);
					a.Sub(x9, x21, x9);
					a.Cmp(x9, regions[region_index].cycles);
					a.B(hs, &deferred_entries[region_index]);
					a.Bind(&partial);
				}
				Label generic_prepare, prepared;
				if (schedule_pair)
				{
					a.Tbz(w25, 1, &generic_prepare);
					EmitScheduledPrepare(a, *block, i);
					a.B(&prepared);
					a.Bind(&generic_prepare);
				}
				// Publish the cached cycle only when a preceding scheduled pair may
				// have changed it. Generic preparation/callbacks use architectural state.
				if (cycle_dirty)
					a.Str(x26, Field(offsetof(VURegs, cycle)));
				const u32 selected_prepare = ins.readsVF ? ins.dependency + 2 : 0;
				a.Add(x0, x23, i * sizeof(Instruction));
				const bool integer_branch = ins.lregs.pipe == VUPIPE_BRANCH && ins.lregs.VIread;
				if (integer_branch)
				{
					a.Mov(x16, reinterpret_cast<uintptr_t>(s_pipeline.branch_prepare));
					a.Blr(x16);
				}
				else if (selected_prepare == shared_prepare)
					a.Blr(x22);
				else
				{
					a.Mov(x16, reinterpret_cast<uintptr_t>(prepare[selected_prepare]));
					a.Blr(x16);
				}
				a.Ldr(x26, Field(offsetof(VURegs, cycle)));
				if (schedule_pair)
					a.Bind(&prepared);
				cycle_dirty = schedule_pair;
				EmitPair(a, cache, ins, true);
				const bool kick = ins.lregs.pipe == VUPIPE_XGKICK;
				if (kick)
				{
					EmitKick(a, ins.lower);
					if (scheduled)
						a.Mov(w25, 0);
				}
				EmitFinish(a, ins);
				EmitIntegerIssue(a, ins);
				if (scheduled && ins.lregs.pipe == VUPIPE_IALU && ins.lregs.cycles)
					a.And(w25, w25, 1);
				EmitControlFlow(a, *block, i);
				if (!kick && (i == 0 || block->instructions[i - 1].lregs.pipe == VUPIPE_XGKICK))
				{
					Label no_packet;
					if (i == 0)
					{
						a.Ldr(w9, MemOperand(sp, 72));
						a.Cbz(w9, &no_packet);
					}
					// Commit the delayed pair before GIF observes VU state, then
					// continue with the same cache assignment and host frame.
					a.Str(x26, Field(offsetof(VURegs, cycle)));
					a.Mov(x16, reinterpret_cast<uintptr_t>(s_pipeline.finish_packet));
					a.Blr(x16);
					a.Ldr(x26, Field(offsetof(VURegs, cycle)));
					EmitBlockConstants(a);
					// Scheduling was disabled while the packet was pending. Recheck
					// after the callback; generic preparation rebuilds known timing.
					if (scheduled)
						EmitScheduleGuard(a);
					a.Bind(&no_packet);
				}
				if (integer_branch && i + 1 < block->count)
				{
					// The fallthrough path has no pending branch. Publish the common
					// cache at exit; only the taken path executes the connected delay.
					a.Ldr(w9, Field(offsetof(VURegs, branch)));
					a.Cbz(w9, &exit);
				}
				a.Sub(x9, x26, x20);
				a.Cmp(x9, x21);
				a.B(hs, &exit);
			}
			if (deferred)
				a.B(&finished);
			for (u32 r = 0; r < regions.size(); r++)
			{
				const auto& region = regions[r];
				a.Bind(&deferred_entries[r]);
				EmitDeferredRegion(a, *block, region.first, region.end);
				// The emitter borrows w25 for flags. Entry proved both guards and
				// no special work can be issued inside the region.
				a.Mov(w25, 3);
				if (region.end < block->count)
				{
					// Queues are complete; retain VF/ACC for the following region.
					// Exact budget exhaustion must exit before its first pair.
					a.Sub(x9, x26, x20);
					a.Cmp(x9, x21);
					a.B(lo, &resumes[r]);
				}
				a.B(region.end == block->count ? &finished : &exit);
			}
			a.Bind(&finished);
			if (block->loops_to_entry)
			{
				a.Sub(x9, x26, x20);
				a.Cmp(x9, x21);
				a.B(hs, &exit);
				// Keep VF/ACC and the host frame, but start pipeline preparation
				// afresh. Every reachable source pair was validated before entry.
				a.Str(x26, Field(offsetof(VURegs, cycle)));
				if (scheduled)
					EmitScheduleGuard(a);
				a.Mov(w9, block->instructions[block->count - 1].lregs.pipe == VUPIPE_XGKICK ? 1 : 0);
				a.Str(w9, MemOperand(sp, 72));
				a.B(&loop_entry);
			}
			a.Bind(&exit);
			a.Str(x26, Field(offsetof(VURegs, cycle)));
			for (u32 slot = 0; slot < cache.count; slot++)
				a.Str(VRegister(8 + slot, 128), Field(cache.offsets[slot]));
			if (cache.count)
				for (u32 slot = 0; slot < saved_vectors; slot += 2)
					a.Ldp(VRegister(8 + slot, 64), VRegister(9 + slot, 64), MemOperand(sp, saved_size + slot * 8));
			if (deferred)
				a.Ldp(x27, x28, MemOperand(sp, 80));
			a.Ldr(lr, MemOperand(sp, 64));
			a.Ldp(x25, x26, MemOperand(sp, 48));
			a.Ldp(x23, x24, MemOperand(sp, 32));
			a.Ldp(x21, x22, MemOperand(sp, 16));
			a.Ldp(x19, x20, MemOperand(sp, frame_size, PostIndex));
			a.Ret();
			a.FinalizeCode();
			const size_t size = a.GetSizeOfCodeGenerated();
			HostSys::EndCodeWrite();
			HostSys::FlushInstructionCache(s_write, static_cast<u32>(size));
			block->function = reinterpret_cast<Block::Function>(s_write);
			s_write += (size + 15) & ~size_t(15);
		}
		auto& result = s_blocks[pc / 8];
		result = std::move(block);
		return *result;
	}

	bool PacketXgkickPending()
	{
		return CpuVU1 == &CpuArm64VU1 && !CHECK_XGKICKHACK &&
		       VU1.xgkickenable == VURegs::XgkickPacket && VU1.xgkicksizeremaining == 0;
	}

	bool Matches(const Block& block, u32 pc, u64 max_pairs)
	{
		if (!block.count)
			return std::memcmp(block.words.data(), VU1.Micro + pc, 8) == 0;
		// Every pair advances at least one cycle. A short call cannot reach the
		// remainder of a connected trace; validate that remainder when it can.
		for (const auto& range : block.ranges)
		{
			const u32 count = static_cast<u32>(std::min<u64>(range.count, max_pairs));
			if (std::memcmp(block.words.data() + range.first * 2, VU1.Micro + range.pc, count * 8) != 0)
				return false;
			max_pairs -= count;
			if (!max_pairs)
				break;
		}
		return true;
	}
} // namespace

Arm64VU1Recompiler CpuArm64VU1;

Arm64VU1Recompiler::Arm64VU1Recompiler()
{
	m_Idx = 1;
	IsInterpreter = false;
}

void Arm64VU1Recompiler::Reserve()
{
	s_base = SysMemory::GetVU1Rec();
	s_end = SysMemory::GetVU1RecEnd();
	InvalidateAll();
}

void Arm64VU1Recompiler::Shutdown()
{
	InvalidateAll();
	s_base = s_write = s_end = nullptr;
}

void Arm64VU1Recompiler::Reset()
{
	CpuIntVU1.Reset();
	InvalidateAll();
}

void Arm64VU1Recompiler::SetStartPC(u32 pc) { VU1.start_pc = pc; }
void Arm64VU1Recompiler::Step()
{
	const bool pending = PacketXgkickPending();
	if (!pending)
	{
		CpuIntVU1.Step();
		return;
	}
	u32 words[2];
	std::memcpy(words, VU1.Micro + (VU1.VI[REG_TPC].UL & VU1_PROGMASK), sizeof(words));
	const bool new_kick = !(words[1] & 0x80000000) && (words[0] & 0xfe0007ff) == 0x800006fc;
	CpuIntVU1.Step();
	// A second XGKICK flushes the old request itself and starts a new delay.
	if (!new_kick && PacketXgkickPending())
		_vuXGKICKTransfer(0, true);
}
void Arm64VU1Recompiler::Clear(u32, u32)
{
	// Entries validate reachable source bytes before execution, including
	// wrapped MPG uploads and debugger/state-load writes which omit Clear().
	// Retaining them avoids recompiling identical program uploads.
}
size_t Arm64VU1Recompiler::GetCommittedCache() const { return s_base ? s_write - s_base : 0; }

void Arm64VU1Recompiler::Execute(u32 cycles)
{
	if (!s_base)
		Reserve();
	if (s_options != Options())
		InvalidateAll();
	const FPControlRegisterBackup fpcr(EmuConfig.Cpu.VU1FPCR);
	// Under MTVU this runs on the VU1 thread, where VU0.VI[REG_VPU_STAT] belongs to
	// the EE thread. Reading its busy bit here raced with the EE clearing it, and
	// writing it corrupted the EE's VIF1 stall handling. Use the VU1-local flag the
	// MTVU dispatcher sets instead; the interpreter clears it at the E-bit.
	const bool mtvu = THREAD_VU1;
	VU1.VI[REG_TPC].UL <<= 3;
	const u64 start = VU1.cycle;
	while (VU1.cycle - start < cycles)
	{
		if (!(mtvu ? (VU1.flags & VUFLAG_MTVURUNNING) : (VU0.VI[REG_VPU_STAT].UL & 0x100)))
		{
			if (VU1.branch == 1)
			{
				VU1.VI[REG_TPC].UL = VU1.branchpc;
				VU1.branch = 0;
			}
			break;
		}
		const u32 pc = VU1.VI[REG_TPC].UL & VU1_PROGMASK;
		VU1.VI[REG_TPC].UL = pc;
		// Pending branch/E-bit delay slots must be retired by the original path.
		if (VU1.branch || VU1.ebit || (pc & 7))
		{
			Step();
			continue;
		}
		const bool pending = PacketXgkickPending();
		const u64 remaining = cycles - (VU1.cycle - start);
		Block* block = s_blocks[pc / 8].get();
		if (!block || !Matches(*block, pc, remaining))
			block = &Compile(pc);
		// A restored chained-delay state still requires interpreter branch retirement.
		if (block->function && !(block->has_branches && VU1.takedelaybranch))
		{
			// The generated first-pair boundary publishes the delayed transfer,
			// including its lower store, even when that pair exhausts the budget.
			block->function(start, cycles, pending);
		}
		else
			Step();
	}
	VU1.VI[REG_TPC].UL >>= 3;
	// nextBlockCycles pairs VU1.cycle with the EE clock for the synchronous path.
	// Under MTVU neither operand is valid here: MTVU resets VU1.cycle to 0 per run,
	// and cpuRegs.cycle is owned by the EE thread.
	if (!mtvu)
		VU1.nextBlockCycles = (VU1.cycle - cpuRegs.cycle) + 1;
}
