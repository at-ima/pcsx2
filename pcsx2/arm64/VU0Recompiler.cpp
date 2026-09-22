// SPDX-FileCopyrightText: 2026 PCSX2 Dev Team
// SPDX-License-Identifier: GPL-3.0+

#include "Common.h"
#include "arm64/VU0Recompiler.h"
#include "arm64/VU0Pipeline.h"
#include "common/HostSys.h"
#include "vixl/aarch64/macro-assembler-aarch64.h"

#include <algorithm>
#include <array>
#include <memory>
#include <vector>

// Native ARM64 block recompiler for VU0 micro-mode, mirroring
// arm64/VU1Recompiler.cpp's codegen for the FMAC/upper-instruction set and a
// subset of lower instructions, reduced for VU0's simpler execution model
// and an initial, deliberately narrow supported-opcode set:
//  - VU0 has no XGKICK/GIF path (there is no Lower::Xgkick case at all).
//  - VU0 always runs synchronously on the EE thread, so there is no
//    MTVU-equivalent concept here.
//  - A block may contain a static B and integer-conditional branches: the
//    trace follows the taken edge and the generated guard leaves the block on
//    the fallthrough. JR/JALR/BAL still end it -- their continuation is not a
//    compile-time constant -- as does any other lower op classified
//    VUPIPE_BRANCH. A repeated PC also ends the trace, so unlike
//    VU1Recompiler.cpp there is no loop-to-entry back-edge to generate yet.
//  - A block never contains an E/M/D/T-bit pair (VU0's M-bit -- unlike
//    VU1 -- ends interpreter execution after the pair; see VU0microInterp.cpp's
//    VUFLAG_MFLAGSET check), or a DIV/SQRT/RSQRT/WAITQ/EFU-pipe (ESADD..WAITP)
//    lower op. All of these fall back to the interpreter for now; the FMAC
//    pipe (the profiled hot path, see PERFORMANCE.md) is fully covered.
// needs proper testing across the supported opcode set and its interaction
// with pairs still falling back to the interpreter mid-program.
namespace
{
	using namespace vixl::aarch64;

	constexpr u32 MaxInstructions = 256;
	constexpr size_t MaxBlockBytes = MaxInstructions * 2048;

	using Arm64VU0::Instruction;

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

	struct Block
	{
		using Function = void (*)(u64 start, u64 cycles);
		std::array<Instruction, MaxInstructions> instructions{};
		std::array<u32, MaxInstructions * 2> words{};
		// A trace that follows a branch is no longer one contiguous span of micro
		// memory, so validation walks the traced ranges instead of one memcmp.
		struct SourceRange
		{
			u32 pc, first, count;
		};
		std::vector<SourceRange> ranges;
		std::array<u32, MaxInstructions> next_pc{};
		std::array<bool, MaxInstructions> delay{};
		bool has_branches = false;
		u32 count = 0;
		VectorCache cache;
		Function function = nullptr;
	};

	std::array<std::unique_ptr<Block>, VU0_PROGSIZE / 8> s_blocks;
	u8* s_base = nullptr;
	u8* s_write = nullptr;
	u8* s_end = nullptr;
	u32 s_options = 0;
	Arm64VU0::PipelineCode s_pipeline;

	constexpr size_t VF(u32 reg) { return offsetof(VURegs, VF) + sizeof(VECTOR) * reg; }
	constexpr size_t VI(u32 reg) { return offsetof(VURegs, VI) + sizeof(REG_VI) * reg; }
	MemOperand Field(size_t offset) { return MemOperand(x19, offset); }

	u32 Options()
	{
		return (CHECK_VU_OVERFLOW(0) ? 1 : 0) | (EmuConfig.Cpu.VU0FPCR.GetDenormalsAreZero() ? 2 : 0) |
		       (CpuVU0 == &CpuArm64VU0 ? 4 : 0);
	}

	void InvalidateAll()
	{
		for (auto& block : s_blocks)
			block.reset();
		s_write = s_base;
		// The pipeline stubs live at the start of the same buffer, so rewinding
		// s_write without dropping them leaves prepare[] pointing at memory the
		// next compiled block overwrites -- its Blr would then land in that
		// block's own prologue and push a frame per call until the stack
		// overflows. Same as arm64/VU1Recompiler.cpp's InvalidateAll.
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

	// Identical to VU1Recompiler.cpp's DecodeUpper: the FMAC upper-instruction
	// encoding is shared between VU0 and VU1.
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

	// Reduced from VU1Recompiler.cpp's Lower: no Xgkick (VU0 has no GIF path),
	// no Div/Sqrt/Rsqrt/Waitq (FDIV pipe) or Esadd..Waitp (EFU pipe) -- those
	// opcodes fall straight through to Unsupported below, same as any other
	// not-yet-natively-compiled instruction. JR/JALR/BAL have no entry either:
	// they are detected generically via lregs.pipe == VUPIPE_BRANCH in
	// Compile(), which ends the trace there.
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
		Branch,
		Ibeq,
		Ibne,
		Ibltz,
		Ibgtz,
		Iblez,
		Ibgez,
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
			case 0x20:
				return Lower::Branch;
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
					case 0x3bc:
						return Lower::Div;
					case 0x3bd:
						return Lower::Sqrt;
					case 0x3be:
						return Lower::Rsqrt;
					case 0x3bf:
						return Lower::Waitq;
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
					case 0x3fe:
						return Lower::Ilwr;
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

	// The lower ops routed through the FDIV pipe, i.e. the ones the interpreter
	// sends into _vuTestFDIVStalls before executing the pair.
	bool IsFDIVPipe(Lower op)
	{
		return op == Lower::Div || op == Lower::Sqrt || op == Lower::Rsqrt || op == Lower::Waitq;
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

	// v6/v7 hold the overflow clamp bounds and v24 the FP exponent mask, for
	// the whole block, loaded once by EmitBlockConstants at block entry. See
	// VU1Recompiler.cpp's ClampInputAlways for the full rationale (shared
	// register layout convention; this file never calls the real
	// _vuXGKICKTransfer, so there is no post-Blr reload site to mirror).
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
			a.Smin(reg.V4S(), reg.V4S(), v6.V4S());
			a.Umin(reg.V4S(), reg.V4S(), v7.V4S());
		}
	}

	void ClampInput(MacroAssembler& a, VRegister reg)
	{
		if (EmuConfig.Cpu.VU0FPCR.GetDenormalsAreZero())
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

	void EmitBlockConstants(MacroAssembler& a)
	{
		a.Movi(v24.V4S(), 0x7f800000);
		if (!CHECK_VU_OVERFLOW(0))
			return;
		a.Movi(v6.V4S(), 0x7f7fffff);
		a.Movi(v7.V4S(), 0xff7fffff);
	}

	void StoreMAC(MacroAssembler& a, const VectorCache& cache, const Upper& op, u32 code, int mask_override = -1)
	{
		const u32 mask = mask_override >= 0 ? static_cast<u32>(mask_override) : (code >> 21) & 15;
		const bool flush = EmuConfig.Cpu.VU0FPCR.GetFlushToZero();
		a.And(v17.V16B(), v0.V16B(), v24.V16B());
		a.Fcmeq(v18.V4S(), v0.V4S(), 0.0);
		if (!flush)
		{
			a.Cmeq(v19.V4S(), v17.V4S(), 0);
			a.Bic(v19.V16B(), v19.V16B(), v18.V16B()); // underflow
			a.Orr(v21.V16B(), v18.V16B(), v19.V16B());
		}
		a.Cmeq(v20.V4S(), v17.V4S(), v24.V4S()); // overflow
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
		if (!flush)
		{
			a.Movi(v22.V4S(), 0x80000000);
			a.And(v22.V16B(), v0.V16B(), v22.V16B());
			a.Bsl(v19.V16B(), v22.V16B(), v0.V16B());
			a.Mov(v0.V16B(), v19.V16B());
		}
		if (CHECK_VU_OVERFLOW(0))
		{
			a.Smin(v0.V4S(), v0.V4S(), v6.V4S());
			a.Umin(v0.V4S(), v0.V4S(), v7.V4S());
		}
		const u32 fd = (code >> 6) & 31;
		if (op.acc || fd)
			StoreVector(a, cache, v0, op.acc ? 32 : fd, mask);
	}

	// Identical to VU1Recompiler.cpp's EmitUpper: the FMAC upper-instruction
	// codegen is shared between VU0 and VU1 (same ISA, same arithmetic).
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

	// Ported from VU1Recompiler.cpp. A pending divide has to finish before another
	// FDIV-pipe op issues (_vuTestFDIVStalls), which can force VURegs::cycle
	// forward mid-pair, so the queues this pair's prepare call already drained are
	// drained again afterwards -- matching the interpreter running _vuTestPipes
	// again after _vuTestFDIVStalls in its own per-instruction dispatch order.
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
		a.Str(x26, Field(offsetof(VURegs, cycle)));
		a.Ldr(w10, Field(offsetof(VURegs, fdiv) + offsetof(fdivPipe, reg)));
		a.Str(w10, Field(VI(REG_Q)));
		a.Ldr(w10, Field(VI(REG_STATUS_FLAG)));
		a.And(w10, w10, 0xfcf);
		a.Ldr(w11, Field(offsetof(VURegs, fdiv) + offsetof(fdivPipe, statusflag)));
		a.And(w11, w11, 0xc30);
		a.Orr(w10, w10, w11);
		a.Str(w10, Field(VI(REG_STATUS_FLAG)));
		// The forced advance above always reaches at least the entry's own ready
		// cycle, so it is fully retired now. DIV/SQRT/RSQRT re-arm the pipe right
		// after and would overwrite this either way, but WAITQ issues nothing and
		// needs the pipe left empty.
		a.Str(wzr, Field(offsetof(VURegs, fdiv) + offsetof(fdivPipe, enable)));
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

	// Reduced from VU1Recompiler.cpp's EmitLower: only the non-FDIV/EFU-pipe
	// cases DecodeLower can return (see above) are handled here.
	void EmitLower(MacroAssembler& a, const VectorCache& cache, u32 code)
	{
		const Lower op = DecodeLower(code);
		const u32 fs = (code >> 11) & 31, ft = (code >> 16) & 31, id = (code >> 6) & 15;
		const u32 is = fs & 15, it = ft & 15, mask = (code >> 21) & 15;
		if (op == Lower::Fcand || op == Lower::Fceq || op == Lower::Fcor)
		{
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
			// _vuWAITQ's body is empty; its only effect is the pending-FDIV stall
			// every FDIV-pipe op takes before doing anything else. It does not
			// issue a new pipe entry, so there is nothing to finish.
			EmitFDIVStall(a);
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
			a.And(w0, w0, 0x3ff);
			a.Ldr(x1, Field(offsetof(VURegs, Mem)));
			a.Add(x1, x1, Operand(x0, LSL, 4));
			const u32 lane = (mask & 1) ? 3 : (mask & 2) ? 2 :
			                              (mask & 4)     ? 1 :
			                                               0;
			a.Ldrh(w0, MemOperand(x1, lane * 4));
			a.Strh(w0, Field(VI(it)));
			return;
		}
		if (op == Lower::Isw)
		{
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

	// Mirrors VU1Recompiler.cpp's EmitPair, minus the XGKICK/branch/Waitq
	// exclusions in the lower-instruction guard: a block never contains any
	// of those pairs to begin with (see Compile()), so the guard would be
	// dead code here.
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
		// Every FDIV-pipe lower op stalls on a still-pending divide before it does
		// anything else, and that stall must land before the paired upper
		// instruction runs: VU0microInterp.cpp calls _vuTestLowerStalls (hence
		// _vuTestFDIVStalls) and _vuTestPipes ahead of _vu0ExecUpper, precisely so
		// an upper op broadcasting Q in the same pair observes the freshly retired
		// value instead of whatever was pending beforehand. This covers WAITQ and
		// DIV/SQRT/RSQRT alike -- a "DIV + MULq" pair reads the *previous* divide's
		// Q, not the one the same pair issues. EmitLower's own cases still call
		// EmitFDIVStall afterwards, but it is then a no-op (fdiv.enable is already
		// clear by the time it gets there).
		if (!immediate && IsFDIVPipe(DecodeLower(ins.lower)))
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
			const Lower lower_op = DecodeLower(ins.lower);
			if (lower_op != Lower::Branch && !IsIntegerBranch(lower_op))
				EmitLower(a, cache, ins.lower);
			if (backup)
				StoreVector(a, cache, q26, backup);
		}
	}

	// Ported from VU1Recompiler.cpp's EmitControlFlow, minus its JR/JALR/BAL
	// (register-target) path: Compile() still stops the trace before those, so
	// they keep falling back to the interpreter. A branch publishes branchpc and
	// VU->branch here; the following delay-slot pair clears VU->branch and
	// overwrites the TPC the pipeline stub wrote with the resolved target.
	// needs proper testing against microprograms with back-to-back branches.
	void EmitControlFlow(MacroAssembler& a, const Block& block, u32 index)
	{
		const auto& ins = block.instructions[index];
		const bool immediate = ins.upper & 0x80000000;
		const Lower op = immediate ? Lower::Unsupported : DecodeLower(ins.lower);
		if (!immediate && op == Lower::Branch)
		{
			const s32 displacement = (static_cast<s32>(ins.lower << 21) >> 21) * 8;
			StoreWord(a, (ins.pc + 8 + displacement) & VU0_PROGMASK, offsetof(VURegs, branchpc));
			StoreWord(a, 1, offsetof(VURegs, branch));
		}
		else if (!immediate && IsIntegerBranch(op))
		{
			Label done;
			// VIBackupCycles/VIOldValue reproduce the interpreter's one-pair integer
			// write delay, so a branch reading the register the previous pair wrote
			// sees the pre-write value exactly as VU0microInterp.cpp does.
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
			StoreWord(a, (ins.pc + 8 + displacement) & VU0_PROGMASK, offsetof(VURegs, branchpc));
			StoreWord(a, 1, offsetof(VURegs, branch));
			a.Bind(&done);
		}
		else if (block.delay[index])
		{
			StoreWord(a, 0, offsetof(VURegs, branch));
			StoreWord(a, block.next_pc[index], VI(REG_TPC));
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

	// Builds a straight-line trace starting at pc. Stops one pair before
	// anything this file does not natively compile: an unsupported upper or
	// lower op, a branch (lregs.pipe == VUPIPE_BRANCH), or an E/M/D/T-bit
	// pair. That excluded pair, and everything after it, still runs on the
	// interpreter via Arm64VU0Recompiler::Step() -- see Execute() below.
	__noinline Block& Compile(u32 pc)
	{
		if (static_cast<size_t>(s_end - s_write) < MaxBlockBytes)
			InvalidateAll();
		auto block = std::make_unique<Block>();
		const u32 saved_code = VU0.code;
		// Follow static B and taken integer-branch edges into one trace, with a
		// single vector-cache and pipeline schedule. A repeated PC ends the trace:
		// unlike VU1 there is no native loop-to-entry back-edge here yet, so a
		// revisit simply becomes the next block's entry.
		u32 next_pc = pc, branch_target = 0;
		bool pending_branch = false;
		std::array<bool, VU0_PROGSIZE / 8> visited{};
		for (u32 i = 0; i < MaxInstructions && next_pc < VU0_PROGSIZE; i++)
		{
			auto& ins = block->instructions[i];
			if (visited[next_pc / 8])
				break;
			ins.pc = next_pc;
			std::memcpy(&ins.lower, VU0.Micro + ins.pc, 4);
			std::memcpy(&ins.upper, VU0.Micro + ins.pc + 4, 4);
			block->words[i * 2] = ins.lower;
			block->words[i * 2 + 1] = ins.upper;
			// M-bit ends interpreter execution after this pair completes
			// (VUFLAG_MFLAGSET, VU0microInterp.cpp), unlike VU1. E/D/T are
			// already excluded by DecodeUpper below.
			if (ins.upper & 0x20000000)
				break;
			if (DecodeUpper(ins.upper).op == Op::Unsupported)
				break;
			const bool immediate = ins.upper & 0x80000000;
			if (!immediate)
			{
				VU0.code = ins.lower;
				VU0regs_LOWER_OPCODE[ins.lower >> 25](&ins.lregs);
				const Lower lower_op = DecodeLower(ins.lower);
				const bool is_branch = lower_op == Lower::Branch || IsIntegerBranch(lower_op);
				// JR/JALR/BAL still end the trace: their continuation is not a
				// compile-time constant, so there is nothing to keep tracing into.
				if (ins.lregs.pipe == VUPIPE_BRANCH && !is_branch)
					break;
				// A branch landing in another branch's delay slot is not resolved
				// natively, and a branch as the final slot has nowhere to put its
				// delay pair.
				if (is_branch && (pending_branch || i + 1 == MaxInstructions))
					break;
				if (lower_op == Lower::Unsupported)
					break;
			}
			VU0.code = ins.upper;
			VU0regs_UPPER_OPCODE[ins.upper & 0x3f](&ins.uregs);
			for (const _VURegsNum* regs : {&ins.uregs, &ins.lregs})
			{
				if (regs->pipe != VUPIPE_FMAC)
					continue;
				if (regs->VFread0)
					ins.readMasks[regs->VFread0] |= regs->VFr0xyzw;
				if (regs->VFread1)
					ins.readMasks[regs->VFread1] |= regs->VFr1xyzw;
			}
			ins.readsVF = std::any_of(ins.readMasks.begin(), ins.readMasks.end(), [](u8 mask) { return mask != 0; });
			if (i >= 3)
			{
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
			visited[ins.pc / 8] = true;
			block->delay[i] = pending_branch;
			block->next_pc[i] = pending_branch ? branch_target : ins.pc + 8;
			if (pending_branch)
				pending_branch = false;
			if (!(ins.upper & 0x80000000))
			{
				const Lower lower_op = DecodeLower(ins.lower);
				if (lower_op == Lower::Branch || IsIntegerBranch(lower_op))
				{
					const s32 displacement = (static_cast<s32>(ins.lower << 21) >> 21) * 8;
					branch_target = (ins.pc + 8 + displacement) & VU0_PROGMASK;
					pending_branch = true;
					block->has_branches = true;
				}
			}
			// Record the traced spans so Matches() can revalidate a trace that is
			// no longer one contiguous run of micro memory.
			if (!block->ranges.empty() && block->ranges.back().pc + block->ranges.back().count * 8 == ins.pc)
				block->ranges.back().count++;
			else
				block->ranges.push_back({ins.pc, i, 1});
			block->count++;
			next_pc = block->next_pc[i];
			if (pending_branch)
				next_pc &= VU0_PROGMASK; // A delay pair may wrap micro memory.
		}
		// A trace may not end on an unretired branch: its delay pair carries the
		// target, so drop the branch and let the interpreter retire it instead.
		while (block->count && block->delay[block->count - 1] == false &&
			   !(block->instructions[block->count - 1].upper & 0x80000000) &&
			   (DecodeLower(block->instructions[block->count - 1].lower) == Lower::Branch ||
				   IsIntegerBranch(DecodeLower(block->instructions[block->count - 1].lower))))
		{
			block->count--;
			while (!block->ranges.empty() && block->ranges.back().first >= block->count)
				block->ranges.pop_back();
			if (!block->ranges.empty())
				block->ranges.back().count = block->count - block->ranges.back().first;
		}
		VU0.code = saved_code;
		if (block->count)
		{
			AssignVectorCache(*block);
			const VectorCache& cache = block->cache;
			if (!s_pipeline.prepare[0])
			{
				HostSys::BeginCodeWrite();
				s_pipeline = Arm64VU0::CompilePipeline(s_write, s_end - s_write);
				HostSys::EndCodeWrite();
				HostSys::FlushInstructionCache(s_write, static_cast<u32>(s_pipeline.size));
				s_write += (s_pipeline.size + 15) & ~size_t(15);
			}
			HostSys::BeginCodeWrite();
			MacroAssembler a(s_write, s_end - s_write);
			Label exit;
			// Save only the d8..d15 registers this block modifies. Round paired
			// saves up to retain 16-byte stack alignment.
			const u32 saved_vectors = (cache.count + 1) & ~1u;
			const int frame_size = 48 + saved_vectors * 8;
			a.Stp(x19, x20, MemOperand(sp, -frame_size, PreIndex));
			a.Stp(x21, x23, MemOperand(sp, 16));
			a.Str(lr, MemOperand(sp, 32));
			a.Str(x26, MemOperand(sp, 40));
			if (cache.count)
				for (u32 slot = 0; slot < saved_vectors; slot += 2)
					a.Stp(VRegister(8 + slot, 64), VRegister(9 + slot, 64), MemOperand(sp, 48 + slot * 8));
			a.Mov(x19, reinterpret_cast<uintptr_t>(&VU0));
			a.Ldr(x26, Field(offsetof(VURegs, cycle)));
			a.Mov(x20, x0);
			a.Mov(x21, x1);
			a.Mov(x23, reinterpret_cast<uintptr_t>(block->instructions.data()));
			for (u32 slot = 0; slot < cache.count; slot++)
				a.Ldr(VRegister(8 + slot, 128), Field(cache.offsets[slot]));
			EmitBlockConstants(a);
			const auto& prepare = s_pipeline.prepare;
			for (u32 i = 0; i < block->count; i++)
			{
				const auto& ins = block->instructions[i];
				a.Add(x0, x23, i * sizeof(Instruction));
				// An integer branch tests registers a pending integer load may still
				// owe it, so it needs the entry that waits for that load.
				const bool integer_branch = ins.lregs.pipe == VUPIPE_BRANCH && ins.lregs.VIread;
				const u32 selected_prepare = ins.readsVF ? ins.dependency + 2 : 0;
				a.Mov(x16, integer_branch ? reinterpret_cast<uintptr_t>(s_pipeline.branch_prepare) :
											reinterpret_cast<uintptr_t>(prepare[selected_prepare]));
				a.Blr(x16);
				a.Ldr(x26, Field(offsetof(VURegs, cycle)));
				EmitPair(a, cache, ins, true);
				EmitFinish(a, ins);
				EmitIntegerIssue(a, ins);
				EmitControlFlow(a, *block, i);
				if (!(ins.upper & 0x80000000) && IsIntegerBranch(DecodeLower(ins.lower)) &&
					i + 1 < block->count)
				{
					// The trace followed the taken edge, so only that path may run the
					// connected delay pair. On the fallthrough there is no pending
					// branch: leave with the TPC the pipeline stub already wrote
					// (this pair's pc + 8, i.e. the delay slot) and let the dispatcher
					// pick the trace up from there.
					a.Ldr(w9, Field(offsetof(VURegs, branch)));
					a.Cbz(w9, &exit);
				}
				a.Sub(x9, x26, x20);
				a.Cmp(x9, x21);
				a.B(hs, &exit);
			}
			a.Bind(&exit);
			a.Str(x26, Field(offsetof(VURegs, cycle)));
			for (u32 slot = 0; slot < cache.count; slot++)
				a.Str(VRegister(8 + slot, 128), Field(cache.offsets[slot]));
			if (cache.count)
				for (u32 slot = 0; slot < saved_vectors; slot += 2)
					a.Ldp(VRegister(8 + slot, 64), VRegister(9 + slot, 64), MemOperand(sp, 48 + slot * 8));
			a.Ldr(x26, MemOperand(sp, 40));
			a.Ldr(lr, MemOperand(sp, 32));
			a.Ldp(x21, x23, MemOperand(sp, 16));
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

	// A trace that follows a branch is not one contiguous span, so validation
	// walks the recorded source ranges. An unsupported-first-pair block still
	// caches (and must still validate) the one pair that was rejected, so
	// self-modified code at pc that becomes supported later is recompiled
	// instead of falling back forever.
	bool Matches(const Block& block, u32 pc, u64 max_pairs)
	{
		if (!block.count)
			return std::memcmp(block.words.data(), VU0.Micro + pc, 8) == 0;
		// Every pair advances at least one cycle, so a short call cannot reach the
		// tail of a long trace; only validate as far as this call could run.
		for (const auto& range : block.ranges)
		{
			const u32 count = static_cast<u32>(std::min<u64>(range.count, max_pairs));
			if (std::memcmp(block.words.data() + range.first * 2, VU0.Micro + range.pc, count * 8) != 0)
				return false;
			max_pairs -= count;
			if (!max_pairs)
				break;
		}
		return true;
	}

} // namespace

Arm64VU0Recompiler CpuArm64VU0;

Arm64VU0Recompiler::Arm64VU0Recompiler()
{
	m_Idx = 0;
	IsInterpreter = false;
}

void Arm64VU0Recompiler::Reserve()
{
	s_base = SysMemory::GetVU0Rec();
	s_end = SysMemory::GetVU0RecEnd();
	InvalidateAll();
}

void Arm64VU0Recompiler::Shutdown()
{
	InvalidateAll();
	s_base = s_write = s_end = nullptr;
}

void Arm64VU0Recompiler::Reset()
{
	CpuIntVU0.Reset();
	InvalidateAll();
}

void Arm64VU0Recompiler::SetStartPC(u32 pc)
{
	VU0.start_pc = pc;
}

void Arm64VU0Recompiler::Step()
{
	CpuIntVU0.Step();
}

void Arm64VU0Recompiler::Clear(u32, u32)
{
	// Entries validate reachable source bytes before execution, same as
	// Arm64VU1Recompiler::Clear.
}

size_t Arm64VU0Recompiler::GetCommittedCache() const
{
	return s_base ? s_write - s_base : 0;
}

void Arm64VU0Recompiler::Execute(u32 cycles)
{
	if (!s_base)
		Reserve();
	if (s_options != Options())
		InvalidateAll();
	const FPControlRegisterBackup fpcr(EmuConfig.Cpu.VU0FPCR);
	VU0.VI[REG_TPC].UL <<= 3;
	// An M-bit pair (see the file comment above) sets this to end that pair's
	// Execute() call early; the interpreter (VU0microInterp.cpp) and the old
	// x86 recompiler (x86/microVU.cpp) both clear it again here so the next
	// call resumes stepping. This call was missing it, so once any VU0
	// microprogram hit an M-bit the flag stayed set forever and every later
	// Execute() call returned immediately without advancing VU0.cycle --
	// livelocking the EE thread, which keeps calling Execute() expecting
	// progress. Needs proper testing across more M-bit-using microprograms.
	VU0.flags &= ~VUFLAG_MFLAGSET;
	const u64 start = VU0.cycle;
	while (VU0.cycle - start < cycles)
	{
		if (!(VU0.VI[REG_VPU_STAT].UL & 0x1))
		{
			if (VU0.branch)
			{
				VU0.VI[REG_TPC].UL = VU0.branchpc;
				VU0.branch = 0;
			}
			break;
		}
		if (VU0.flags & VUFLAG_MFLAGSET)
			break;
		const u32 pc = VU0.VI[REG_TPC].UL & VU0_PROGMASK;
		VU0.VI[REG_TPC].UL = pc;
		// Pending branch/E-bit delay slots must be retired by the interpreter.
		if (VU0.branch || VU0.ebit || (pc & 7))
		{
			Step();
			continue;
		}
		const u64 remaining = cycles - (VU0.cycle - start);
		Block* block = s_blocks[pc / 8].get();
		if (!block || !Matches(*block, pc, remaining))
			block = &Compile(pc);
		// A restored chained-delay state still needs interpreter branch retirement.
		if (block->function && !(block->has_branches && VU0.takedelaybranch))
		{
			block->function(start, cycles);
		}
		else
		{
			Step();
		}
	}
	VU0.VI[REG_TPC].UL >>= 3;
	VU0.nextBlockCycles = (VU0.cycle - cpuRegs.cycle) + 1;
}
