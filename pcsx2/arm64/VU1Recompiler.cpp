// SPDX-FileCopyrightText: 2026 PCSX2 Dev Team
// SPDX-License-Identifier: GPL-3.0+

#include "Common.h"
#include "arm64/VU1Recompiler.h"
#include "arm64/VU1Pipeline.h"
#include "common/HostSys.h"
#include "vixl/aarch64/macro-assembler-aarch64.h"

#include <algorithm>
#include <array>
#include <memory>

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
	};

	struct Block
	{
		using Function = void (*)(u64 start, u64 cycles);
		std::array<Instruction, MaxInstructions> instructions{};
		std::array<RetirementSchedule, MaxInstructions> schedule{};
		std::array<u32, MaxInstructions * 2> words{};
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
		       (EmuConfig.Cpu.VU1FPCR.GetDenormalsAreZero() ? 8 : 0);
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
				Op::Add, Op::Madd, Op::Mul, Op::Max, Op::Sub, Op::Msub, Op::Unsupported, Op::Min};
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
		if (sub == 11 && lane == 3)
			return {Op::None};
		return {};
	}

	enum class Lower
	{
		Lq,
		Sq,
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
					case 0x33c:
						return Lower::Move;
					case 0x33d:
						return Lower::Mr32;
					case 0x3fd:
						return Lower::Mfir;
					case 0x3fc:
						return Lower::Mtir;
				}
				break;
		}
		return Lower::Unsupported;
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

	void ClampInput(MacroAssembler& a, VRegister reg)
	{
		// Arithmetic flushes signed denormal inputs in hardware when FPCR.FZ is
		// enabled. Other FPCR modes still need the interpreter's explicit clamp.
		if (!EmuConfig.Cpu.VU1FPCR.GetDenormalsAreZero())
		{
			a.Movi(v16.V4S(), 0x7f800000);
			a.And(v17.V16B(), reg.V16B(), v16.V16B());
			a.Movi(v18.V4S(), 0x80000000);
			a.And(v18.V16B(), reg.V16B(), v18.V16B());
			a.Cmeq(v19.V4S(), v17.V4S(), 0);
			a.Bsl(v19.V16B(), v18.V16B(), reg.V16B());
			a.Mov(reg.V16B(), v19.V16B());
		}
		if (CHECK_VU_OVERFLOW(0))
		{
			// Signed min clamps positive infinities/NaNs; unsigned min clamps
			// their negative encodings. Finite values and signed zeros are intact.
			a.Movi(v16.V4S(), 0x7f7fffff);
			a.Movi(v17.V4S(), 0xff7fffff);
			a.Smin(reg.V4S(), reg.V4S(), v16.V4S());
			a.Umin(reg.V4S(), reg.V4S(), v17.V4S());
		}
	}

	void StoreMAC(MacroAssembler& a, const VectorCache& cache, const Upper& op, u32 code)
	{
		const u32 mask = (code >> 21) & 15;
		const bool flush = EmuConfig.Cpu.VU1FPCR.GetFlushToZero();
		// v0 is the result. Classify all lanes using the interpreter's FP zero test.
		a.Movi(v16.V4S(), 0x7f800000);
		a.And(v17.V16B(), v0.V16B(), v16.V16B());
		a.Fcmeq(v18.V4S(), v0.V4S(), 0.0);
		if (!flush)
		{
			a.Cmeq(v19.V4S(), v17.V4S(), 0);
			a.Bic(v19.V16B(), v19.V16B(), v18.V16B()); // underflow
			a.Orr(v21.V16B(), v18.V16B(), v19.V16B());
		}
		a.Cmeq(v20.V4S(), v17.V4S(), v16.V4S()); // overflow
		a.Movi(v22.V4S(), 1);
		a.And(v21.V16B(), flush ? v18.V16B() : v21.V16B(), v22.V16B());
		a.Ushr(v23.V4S(), v0.V4S(), 31);
		a.Shl(v23.V4S(), v23.V4S(), 4);
		a.Orr(v21.V16B(), v21.V16B(), v23.V16B());
		if (!flush)
		{
			a.Movi(v22.V4S(), 0x100);
			a.And(v22.V16B(), v19.V16B(), v22.V16B());
			a.Orr(v21.V16B(), v21.V16B(), v22.V16B());
		}
		a.Movi(v22.V4S(), 0x1000);
		a.And(v22.V16B(), v20.V16B(), v22.V16B());
		a.Orr(v21.V16B(), v21.V16B(), v22.V16B());
		// Each lane contributes four non-overlapping flag bits.
		a.Mov(w10, 0);
		for (u32 i = 0; i < 4; i++)
		{
			if (!(mask & (8 >> i)))
				continue;
			a.Umov(w9, v21.V4S(), i);
			a.Orr(w10, w10, Operand(w9, LSL, 3 - i));
		}
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
			a.Movi(v22.V4S(), 0x7f7fffff);
			a.Movi(v23.V4S(), 0xff7fffff);
			a.Smin(v0.V4S(), v0.V4S(), v22.V4S());
			a.Umin(v0.V4S(), v0.V4S(), v23.V4S());
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

	void EmitLower(MacroAssembler& a, const VectorCache& cache, u32 code)
	{
		const Lower op = DecodeLower(code);
		const u32 fs = (code >> 11) & 31, ft = (code >> 16) & 31, id = (code >> 6) & 15;
		const u32 is = fs & 15, it = ft & 15, mask = (code >> 21) & 15;
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
			EmitLower(a, cache, ins.lower);
			if (backup)
				StoreVector(a, cache, q26, backup);
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
		a.Ldr(w0, Field(offsetof(VURegs, fmacwritepos)));
		a.Mov(w1, sizeof(fmacPipe));
		a.Madd(x0, x0, x1, x19);
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
		a.Ldr(w9, Field(offsetof(VURegs, fmacwritepos)));
		a.Add(w9, w9, 1);
		a.And(w9, w9, 3);
		a.Str(w9, Field(offsetof(VURegs, fmacwritepos)));
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
			if (i >= 7 && cycles > 0)
			{
				RetirementSchedule plan{static_cast<u8>(cycles)};
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
		// XGKICK may call C++ and change state during the prefix. Never promote
		// those blocks, even if the pending transfer finishes before the suffix.
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
		// No division/EFU/IALU or GIF work can arise inside a supported block
		// admitted by the readiness check. Broader game coverage still needs proper testing.
		EmitBackupCountdown(a, plan.cycles);
	}

	// A fully budgeted, callback-free suffix can keep its four FMAC flag
	// snapshots in q28..q31. Queue metadata and cycle stamps are compile-time
	// facts and only need materializing when returning to the dispatcher.
	void EmitDeferredSuffix(MacroAssembler& a, const Block& block, u32 first)
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
		for (u32 i = first; i < block.count; i++)
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
		const auto& last = block.instructions[block.count - 1];
		const u32 live = block.schedule[block.count - 1].remaining + HasFmac(last);
		a.Add(w9, w27, issued & 3);
		a.And(w9, w9, 3);
		a.Str(w9, Field(offsetof(VURegs, fmacwritepos)));
		a.Sub(w9, w9, live);
		a.And(w9, w9, 3);
		a.Str(w9, Field(offsetof(VURegs, fmacreadpos)));
		StoreWord(a, live, offsetof(VURegs, fmaccount));
		a.Str(w25, Field(VI(REG_STATUS_FLAG)));
		a.Str(w28, Field(VI(REG_MAC_FLAG)));
		StoreWord(a, last.pc + 8, VI(REG_TPC));
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
		for (u32 i = 0; i < MaxInstructions && pc + i * 8 < VU1_PROGSIZE; i++)
		{
			auto& ins = block->instructions[i];
			ins.pc = pc + i * 8;
			std::memcpy(&ins.lower, VU1.Micro + ins.pc, 4);
			std::memcpy(&ins.upper, VU1.Micro + ins.pc + 4, 4);
			block->words[i * 2] = ins.lower;
			block->words[i * 2 + 1] = ins.upper;
			if (DecodeUpper(ins.upper).op == Op::Unsupported ||
				(!(ins.upper & 0x80000000) && DecodeLower(ins.lower) == Lower::Unsupported))
				break;
			VU1.code = ins.upper;
			VU1regs_UPPER_OPCODE[ins.upper & 0x3f](&ins.uregs);
			if (!(ins.upper & 0x80000000))
			{
				VU1.code = ins.lower;
				VU1regs_LOWER_OPCODE[ins.lower >> 25](&ins.lregs);
			}
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
				s_pipeline = Arm64VU1::CompilePipeline(s_write, s_end - s_write);
				HostSys::EndCodeWrite();
				HostSys::FlushInstructionCache(s_write, static_cast<u32>(s_pipeline.size));
				s_write += (s_pipeline.size + 15) & ~size_t(15);
			}
			HostSys::BeginCodeWrite();
			MacroAssembler a(s_write, s_end - s_write);
			u32 suffix_start = block->count, suffix_cycles = 0;
			while (suffix_start > 7 && block->schedule[suffix_start - 1].cycles)
				suffix_cycles += block->schedule[--suffix_start].cycles;
			const bool deferred = block->count - suffix_start >= 8;
			Label exit, deferred_suffix;
			const int saved_size = deferred ? 96 : 80;
			const int frame_size = saved_size + (cache.count ? 64 : 0);
			a.Stp(x19, x20, MemOperand(sp, -frame_size, PreIndex));
			a.Stp(x21, x22, MemOperand(sp, 16));
			a.Stp(x23, x24, MemOperand(sp, 32));
			a.Stp(x25, x26, MemOperand(sp, 48));
			a.Str(lr, MemOperand(sp, 64));
			if (deferred)
				a.Stp(x27, x28, MemOperand(sp, 80));
			if (cache.count)
				for (u32 slot = 0; slot < 8; slot += 2)
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
			bool cycle_dirty = false;
			bool readiness_checked = false;
			for (u32 i = 0; i < block->count; i++)
			{
				const auto& ins = block->instructions[i];
				const bool schedule_pair = scheduled && block->schedule[i].cycles != 0;
				if (schedule_pair && (!readiness_checked || (deferred && i == suffix_start)))
				{
					EmitScheduleReadiness(a);
					readiness_checked = true;
				}
				if (deferred && i == suffix_start)
				{
					Label partial;
					a.Tbz(w25, 1, &partial);
					a.Sub(x9, x26, x20);
					a.Sub(x9, x21, x9);
					a.Cmp(x9, suffix_cycles);
					a.B(hs, &deferred_suffix);
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
				if (selected_prepare == shared_prepare)
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
				EmitFinish(a, ins);
				a.Sub(x9, x26, x20);
				a.Cmp(x9, x21);
				a.B(hs, &exit);
			}
			if (deferred)
			{
				a.B(&exit);
				a.Bind(&deferred_suffix);
				EmitDeferredSuffix(a, *block, suffix_start);
			}
			a.Bind(&exit);
			a.Str(x26, Field(offsetof(VURegs, cycle)));
			for (u32 slot = 0; slot < cache.count; slot++)
				a.Str(VRegister(8 + slot, 128), Field(cache.offsets[slot]));
			if (cache.count)
				for (u32 slot = 0; slot < 8; slot += 2)
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

	bool Matches(const Block& block, u32 pc)
	{
		return std::memcmp(block.words.data(), VU1.Micro + pc, std::max(1u, block.count) * 8) == 0;
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
	// Entries validate their complete source bytes before execution, including
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
	VU1.VI[REG_TPC].UL <<= 3;
	const u64 start = VU1.cycle;
	while (VU1.cycle - start < cycles)
	{
		if (!(VU0.VI[REG_VPU_STAT].UL & 0x100))
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
		Block* block = s_blocks[pc / 8].get();
		if (!block || !Matches(*block, pc))
			block = &Compile(pc);
		if (block->function)
		{
			// microVU transfers after the pair following XGKICK, including its
			// lower store. Exit after that pair so the complete architectural state
			// is published before GIF callbacks, even at a cycle-budget boundary.
			const bool pending = PacketXgkickPending();
			block->function(start, pending ? VU1.cycle - start + 1 : cycles);
			if (pending && PacketXgkickPending())
				_vuXGKICKTransfer(0, true);
		}
		else
			Step();
	}
	VU1.VI[REG_TPC].UL >>= 3;
	VU1.nextBlockCycles = (VU1.cycle - cpuRegs.cycle) + 1;
}
