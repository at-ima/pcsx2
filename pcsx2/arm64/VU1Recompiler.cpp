// SPDX-FileCopyrightText: 2026 PCSX2 Dev Team
// SPDX-License-Identifier: GPL-3.0+

#include "Common.h"
#include "arm64/VU1Recompiler.h"
#include "VUPipeline.h"
#include "common/HostSys.h"
#include "vixl/aarch64/macro-assembler-aarch64.h"

#include <algorithm>
#include <array>
#include <memory>

namespace
{
	using namespace vixl::aarch64;
	constexpr u32 MaxInstructions = 32;
	constexpr size_t MaxBlockBytes = 64 * 1024;

	struct Instruction
	{
		u32 pc;
		u32 lower;
		u32 upper;
		_VURegsNum uregs{};
		_VURegsNum lregs{};
		std::array<u8, 32> readMasks{};
		bool readsVF = false;
		// -1: inspect incoming pipeline; 0: no dependency; 1..3: FMAC slots back.
		int8_t dependency = -1;
	};

	struct Block
	{
		using Function = void (*)(u64 start, u64 cycles);
		std::array<Instruction, MaxInstructions> instructions{};
		std::array<u32, MaxInstructions * 2> words{};
		u32 count = 0;
		Function function = nullptr;
	};

	std::array<std::unique_ptr<Block>, VU1_PROGSIZE / 8> s_blocks;
	u8* s_base = nullptr;
	u8* s_write = nullptr;
	u8* s_end = nullptr;
	u32 s_options = 0;

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
		s_options = Options();
	}

	// Keep pipeline retirement shared with the interpreter until static scheduling
	// has differential coverage. Game-level XGKICK timing still needs proper testing.
	// -2 skips VF hazards; -1 scans incoming entries; 0..3 are scheduled.
	template <int Dependency>
	void PrepareInstruction(const Instruction* ins)
	{
		VU1.cycle++;
		VU1.VI[REG_TPC].UL = ins->pc + 8;
		const u32 before = VU1.cycle - 1;
		if constexpr (Dependency != -2)
		{
			if (Dependency >= 0 && VU1.cycle < ~u64(0) - 4)
			{
				if constexpr (Dependency > 0)
				{
					const fmacPipe& pipe = VU1.fmac[(VU1.fmacwritepos - Dependency) & 3];
					if ((VU1.cycle - pipe.sCycle) < pipe.Cycle)
						VU1.cycle = std::max(VU1.cycle, pipe.sCycle + pipe.Cycle);
				}
			}
			else
			{
				// Both halves read the same pending FMAC results. Merge their read masks
				// at compile time instead of scanning the queue once per source register.
				for (u32 n = 0, pos = VU1.fmacreadpos; n < VU1.fmaccount; n++, pos = (pos + 1) & 3)
				{
					const fmacPipe& pipe = VU1.fmac[pos];
					if ((VU1.cycle - pipe.sCycle) < pipe.Cycle &&
						((ins->readMasks[pipe.regupper] & pipe.xyzwupper) ||
							(ins->readMasks[pipe.reglower] & pipe.xyzwlower)))
					{
						const u64 ready = pipe.sCycle + pipe.Cycle;
						if (ready > VU1.cycle)
							VU1.cycle = ready;
					}
				}
			}
		}
		VU1.code = (ins->upper & 0x80000000) ? ins->upper : ins->lower;
		VUPipeline::Retire(&VU1);
		if (VU1.VIBackupCycles)
			VU1.VIBackupCycles -= std::min(static_cast<u8>(VU1.cycle - before), VU1.VIBackupCycles);
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

	void StoreMAC(MacroAssembler& a, const Upper& op, u32 code)
	{
		const u32 mask = (code >> 21) & 15;
		// v0 is the result. Classify all lanes using the interpreter's FP zero test.
		a.Movi(v16.V4S(), 0x7f800000);
		a.And(v17.V16B(), v0.V16B(), v16.V16B());
		a.Fcmeq(v18.V4S(), v0.V4S(), 0.0);
		a.Cmeq(v19.V4S(), v17.V4S(), 0);
		a.Bic(v19.V16B(), v19.V16B(), v18.V16B()); // underflow
		a.Cmeq(v20.V4S(), v17.V4S(), v16.V4S()); // overflow
		a.Bic(v20.V16B(), v20.V16B(), v18.V16B());
		a.Orr(v21.V16B(), v18.V16B(), v19.V16B());
		a.Movi(v22.V4S(), 1);
		a.And(v21.V16B(), v21.V16B(), v22.V16B());
		a.Ushr(v23.V4S(), v0.V4S(), 31);
		a.Shl(v23.V4S(), v23.V4S(), 4);
		a.Orr(v21.V16B(), v21.V16B(), v23.V16B());
		a.Movi(v22.V4S(), 0x100);
		a.And(v22.V16B(), v19.V16B(), v22.V16B());
		a.Orr(v21.V16B(), v21.V16B(), v22.V16B());
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
		a.Movi(v22.V4S(), 0x80000000);
		a.And(v22.V16B(), v0.V16B(), v22.V16B());
		a.Bsl(v19.V16B(), v22.V16B(), v0.V16B());
		a.Mov(v0.V16B(), v19.V16B());
		if (CHECK_VU_OVERFLOW(1))
		{
			a.Movi(v23.V4S(), 0x7f7fffff);
			a.Orr(v22.V16B(), v22.V16B(), v23.V16B());
			a.Bsl(v20.V16B(), v22.V16B(), v0.V16B());
			a.Mov(v0.V16B(), v20.V16B());
		}
		const u32 fd = (code >> 6) & 31;
		if (op.acc || fd)
			StoreMasked(a, v0, Field(op.acc ? offsetof(VURegs, ACC) : VF(fd)), mask);
	}

	void EmitUpper(MacroAssembler& a, u32 code)
	{
		const Upper op = DecodeUpper(code);
		if (op.op == Op::None)
			return;
		const u32 fs = (code >> 11) & 31, ft = (code >> 16) & 31, fd = (code >> 6) & 31;
		const u32 mask = (code >> 21) & 15;
		a.Ldr(q0, Field(VF(fs)));
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
			StoreMasked(a, v0, Field(VF(ft)), mask);
			return;
		}
		if (op.broadcast < 4)
		{
			a.Ldr(q1, Field(VF(ft)));
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
			StoreMasked(a, v2, Field(VF(fd)), mask);
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
				a.Ldr(q2, Field(offsetof(VURegs, ACC)));
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
		StoreMAC(a, op, code);
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

	void EmitLower(MacroAssembler& a, u32 code)
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
				a.Ldr(q0, Field(VF(fs)));
				if (op == Lower::Mr32)
					a.Ext(v0.V16B(), v0.V16B(), v0.V16B(), 4);
			}
			StoreMasked(a, v0, Field(VF(ft)), mask);
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
				StoreMasked(a, v0, Field(VF(ft)), mask);
			}
			else
			{
				a.Ldr(q0, Field(VF(fs)));
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
			a.Ldrh(w0, Field(VF(fs) + ((code >> 21) & 3) * 4));
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
		const u64 regs = (upper ? ins.uregs.VFwrite : 0) | (u64(lower ? ins.lregs.VFwrite : 0) << 32);
		const u64 flags = (upper ? ins.uregs.VIwrite : 0) | (lower ? ins.lregs.VIwrite : 0);
		a.Mov(x9, regs);
		a.Str(x9, MemOperand(x0, offsetof(fmacPipe, regupper)));
		a.Mov(x9, flags | (u64(upper ? ins.uregs.VFwxyzw : 0) << 32));
		a.Str(x9, MemOperand(x0, offsetof(fmacPipe, flagreg)));
		a.Mov(x9, lower ? ins.lregs.VFwxyzw : 0);
		a.Str(x9, MemOperand(x0, offsetof(fmacPipe, xyzwlower))); // also clear padding
		a.Ldr(x9, Field(offsetof(VURegs, cycle)));
		a.Str(x9, MemOperand(x0, offsetof(fmacPipe, sCycle)));
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
			HostSys::BeginCodeWrite();
			MacroAssembler a(s_write, s_end - s_write);
			Label exit;
			a.Stp(x19, x20, MemOperand(sp, -48, PreIndex));
			a.Stp(x21, x22, MemOperand(sp, 16));
			a.Stp(x23, lr, MemOperand(sp, 32));
			a.Mov(x19, reinterpret_cast<uintptr_t>(&VU1));
			a.Mov(x20, x0);
			a.Mov(x21, x1);
			// Share the most frequent helper address in x22; keep preparation code
			// outside the emitted instruction stream to avoid instruction-cache growth.
			using PrepareFunction = void (*)(const Instruction*);
			constexpr PrepareFunction prepare[] = {&PrepareInstruction<-2>, &PrepareInstruction<-1>,
				&PrepareInstruction<0>, &PrepareInstruction<1>, &PrepareInstruction<2>, &PrepareInstruction<3>};
			std::array<u32, std::size(prepare)> uses{};
			for (u32 i = 0; i < block->count; i++)
			{
				const auto& ins = block->instructions[i];
				uses[ins.readsVF ? ins.dependency + 2 : 0]++;
			}
			const u32 shared_prepare = std::max_element(uses.begin(), uses.end()) - uses.begin();
			a.Mov(x22, reinterpret_cast<uintptr_t>(prepare[shared_prepare]));
			a.Mov(x23, reinterpret_cast<uintptr_t>(block->instructions.data()));
			for (u32 i = 0; i < block->count; i++)
			{
				const auto& ins = block->instructions[i];
				const u32 selected_prepare = ins.readsVF ? ins.dependency + 2 : 0;
				a.Add(x0, x23, i * sizeof(Instruction));
				if (selected_prepare == shared_prepare)
					a.Blr(x22);
				else
				{
					a.Mov(x16, reinterpret_cast<uintptr_t>(prepare[selected_prepare]));
					a.Blr(x16);
				}
				const bool immediate = ins.upper & 0x80000000;
				const bool discard = !immediate && ins.uregs.VFwrite && ins.uregs.VFwrite == ins.lregs.VFwrite;
				const u32 backup = !immediate && !discard && ins.uregs.VFwrite &&
				                           (ins.uregs.VFwrite == ins.lregs.VFread0 || ins.uregs.VFwrite == ins.lregs.VFread1) ?
				                       ins.uregs.VFwrite :
				                       0;
				if (backup)
					a.Ldr(q27, Field(VF(backup)));
				StoreWord(a, ins.upper, offsetof(VURegs, code));
				EmitUpper(a, ins.upper);
				if (immediate)
					StoreWord(a, ins.lower, VI(REG_I));
				else if (!discard)
				{
					if (backup)
					{
						a.Ldr(q26, Field(VF(backup)));
						a.Str(q27, Field(VF(backup)));
					}
					StoreWord(a, ins.lower, offsetof(VURegs, code));
					EmitLower(a, ins.lower);
					if (backup)
						a.Str(q26, Field(VF(backup)));
				}
				EmitFinish(a, ins);
				a.Ldr(x9, Field(offsetof(VURegs, cycle)));
				a.Sub(x9, x9, x20);
				a.Cmp(x9, x21);
				a.B(hs, &exit);
			}
			a.Bind(&exit);
			a.Ldp(x23, lr, MemOperand(sp, 32));
			a.Ldp(x21, x22, MemOperand(sp, 16));
			a.Ldp(x19, x20, MemOperand(sp, 48, PostIndex));
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
void Arm64VU1Recompiler::Step() { CpuIntVU1.Step(); }
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
			CpuIntVU1.Step();
			continue;
		}
		Block* block = s_blocks[pc / 8].get();
		if (!block || !Matches(*block, pc))
			block = &Compile(pc);
		if (block->function)
			block->function(start, cycles);
		else
			CpuIntVU1.Step();
	}
	VU1.VI[REG_TPC].UL >>= 3;
	VU1.nextBlockCycles = (VU1.cycle - cpuRegs.cycle) + 1;
}
