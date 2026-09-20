// SPDX-FileCopyrightText: 2026 PCSX2 Dev Team
// SPDX-License-Identifier: GPL-3.0+

#include "Common.h"
#include "arm64/EECodeGenerator.h"
#include "R5900OpcodeTables.h"
#include "vtlb.h"
#include "vixl/aarch64/macro-assembler-aarch64.h"

#include <algorithm>
#include <array>

namespace
{
	using namespace vixl::aarch64;
	bool IsHiLo(u32 code)
	{
		const u32 op = code >> 26, function = code & 63;
		if (op != 0 && op != 28)
			return false;
		return (function >= 16 && function <= 19) || (function >= 24 && function <= 27) ||
		       (op == 28 && (function == 0 || function == 1 || function == 32 || function == 33));
	}

	enum class PackedOp
	{
		None,
		Add,
		Subtract,
		Greater,
		Equal,
		Maximum,
		Minimum,
		InterleaveLower,
		InterleaveUpper,
		PackEven,
		CopyUpper,
		And,
		Or,
		Xor,
		Nor,
	};

	struct PackedInstruction
	{
		PackedOp op = PackedOp::None;
		u32 lane_bytes = 1;
	};

	PackedInstruction DecodePacked(u32 code)
	{
		if ((code >> 26) != 28)
			return {};
		const u32 function = code & 63, selector = (code >> 6) & 31;
		if (function == 8) // MMI0: wrapping arithmetic, signed comparisons, packing
		{
			if (selector <= 10)
			{
				constexpr PackedOp ops[] = {PackedOp::Add, PackedOp::Subtract, PackedOp::Greater, PackedOp::Maximum};
				return {ops[selector & 3], 4u >> (selector / 4)};
			}
			if (selector == 18 || selector == 22 || selector == 26)
				return {PackedOp::InterleaveLower, 4u >> ((selector - 18) / 4)};
			if (selector == 19 || selector == 23 || selector == 27)
				return {PackedOp::PackEven, 4u >> ((selector - 19) / 4)};
		}
		else if (function == 40) // MMI1
		{
			if (selector == 2 || selector == 6 || selector == 10)
				return {PackedOp::Equal, 4u >> ((selector - 2) / 4)};
			if (selector == 3 || selector == 7)
				return {PackedOp::Minimum, selector == 3 ? 4u : 2u};
			if (selector == 18 || selector == 22 || selector == 26)
				return {PackedOp::InterleaveUpper, 4u >> ((selector - 18) / 4)};
		}
		else if (function == 9 || function == 41) // MMI2/MMI3
		{
			if (selector == 14)
				return {function == 9 ? PackedOp::InterleaveLower : PackedOp::CopyUpper, 8};
			if (selector == 18)
				return {function == 9 ? PackedOp::And : PackedOp::Or};
			if (selector == 19)
				return {function == 9 ? PackedOp::Xor : PackedOp::Nor};
		}
		return {};
	}

	bool SupportsInteger(u32 code)
	{
		if (IsHiLo(code) || DecodePacked(code).op != PackedOp::None)
			return true;
		switch (code >> 26)
		{
			case 9:
			case 10:
			case 11:
			case 12:
			case 13:
			case 14:
			case 15:
			case 25:
				return true;
			case 0:
				switch (code & 63)
				{
					case 0:
					case 2:
					case 3:
					case 4:
					case 6:
					case 7:
					case 10:
					case 11:
					case 20:
					case 22:
					case 23:
					case 33:
					case 35:
					case 36:
					case 37:
					case 38:
					case 39:
					case 42:
					case 43:
					case 45:
					case 47:
					case 56:
					case 58:
					case 59:
					case 60:
					case 62:
					case 63:
						return true;
				}
		}
		return false;
	}

	MemOperand GPR(u32 reg)
	{
		return MemOperand(x0, offsetof(cpuRegisters, GPR) + reg * sizeof(GPR_reg));
	}

	void EmitPacked(MacroAssembler& a, u32 code, PackedInstruction instruction)
	{
		const u32 rs = (code >> 21) & 31, rt = (code >> 16) & 31, rd = (code >> 11) & 31;
		if (!rd)
			return;
		// Read both full operands before writing rd, including rs/rt/rd aliases.
		// Non-saturating integer NEON operations leave the host FPSR unchanged.
		a.Ldr(q1, GPR(rs));
		a.Ldr(q2, GPR(rt));
		auto format = [&](VRegister reg) {
			switch (instruction.lane_bytes)
			{
				case 8:
					return reg.V2D();
				case 4:
					return reg.V4S();
				case 2:
					return reg.V8H();
				default:
					return reg.V16B();
			}
		};
		const VRegister dst = format(v0), lhs = format(v1), rhs = format(v2);
		switch (instruction.op)
		{
			case PackedOp::Add:
				a.Add(dst, lhs, rhs);
				break;
			case PackedOp::Subtract:
				a.Sub(dst, lhs, rhs);
				break;
			case PackedOp::Greater:
				a.Cmgt(dst, lhs, rhs);
				break;
			case PackedOp::Equal:
				a.Cmeq(dst, lhs, rhs);
				break;
			case PackedOp::Maximum:
				a.Smax(dst, lhs, rhs);
				break;
			case PackedOp::Minimum:
				a.Smin(dst, lhs, rhs);
				break;
			// PEXT/PPAC place rt's lanes before rs's lanes. PCPYUD reverses
			// that order when selecting the upper 64 bits of both sources.
			case PackedOp::InterleaveLower:
				a.Zip1(dst, rhs, lhs);
				break;
			case PackedOp::InterleaveUpper:
				a.Zip2(dst, rhs, lhs);
				break;
			case PackedOp::PackEven:
				a.Uzp1(dst, rhs, lhs);
				break;
			case PackedOp::CopyUpper:
				a.Zip2(dst, lhs, rhs);
				break;
			case PackedOp::And:
				a.And(dst, lhs, rhs);
				break;
			case PackedOp::Or:
				a.Orr(dst, lhs, rhs);
				break;
			case PackedOp::Xor:
				a.Eor(dst, lhs, rhs);
				break;
			case PackedOp::Nor:
				a.Orr(dst, lhs, rhs);
				a.Mvn(dst, dst);
				break;
			case PackedOp::None:
				pxFailRel("Invalid packed integer instruction");
				break;
		}
		a.Str(q0, GPR(rd));
	}

	void EmitHiLo(MacroAssembler& a, u32 code)
	{
		const u32 op = code >> 26, function = code & 63;
		const u32 rs = (code >> 21) & 31, rt = (code >> 16) & 31, rd = (code >> 11) & 31;
		const u32 bank = op == 28 && function >= 16 ? 1 : 0;
		const MemOperand hi(x0, offsetof(cpuRegisters, HI) + bank * sizeof(u64));
		const MemOperand lo(x0, offsetof(cpuRegisters, LO) + bank * sizeof(u64));
		if (function >= 16 && function <= 19)
		{
			const MemOperand& special = (function & 2) ? lo : hi;
			if (function & 1) // MTHI/MTLO, including the second bank
			{
				a.Ldr(x9, GPR(rs));
				a.Str(x9, special);
			}
			else if (rd) // MFHI/MFLO copy all 64 bits without extending again.
			{
				a.Ldr(x9, special);
				a.Str(x9, GPR(rd));
			}
			return;
		}

		a.Ldr(w9, GPR(rs));
		a.Ldr(w10, GPR(rt));
		const bool unsigned_op = function & 1;
		const bool divide = function == 26 || function == 27;
		if (divide)
		{
			if (unsigned_op)
				a.Udiv(w11, w9, w10);
			else
				a.Sdiv(w11, w9, w10);
			a.Msub(w12, w11, w10, w9);
			// ARM's signed overflow result already matches EE. Zero division needs
			// an EE quotient of -1 (or +1 for a negative signed dividend), while
			// the remainder above retains the dividend because the divisor is zero.
			a.Mov(w13, 0xffffffffu);
			if (!unsigned_op)
			{
				a.Cmp(w9, 0);
				a.Cneg(w13, w13, lt);
			}
			a.Cmp(w10, 0);
			a.Csel(w11, w11, w13, ne);
			a.Sxtw(x11, w11);
			a.Sxtw(x12, w12);
		}
		else
		{
			if (unsigned_op)
				a.Umull(x11, w9, w10);
			else
				a.Smull(x11, w9, w10);
			if (op == 28 && (function == 0 || function == 1 || function == 32 || function == 33))
			{
				// MADD concatenates the low 32 bits of HI and LO, not their 64-bit
				// sign extensions. Addition wraps at 64 bits without a host trap.
				a.Ldr(w12, lo);
				a.Ldr(w13, hi);
				a.Orr(x12, x12, Operand(x13, LSL, 32));
				a.Add(x11, x11, x12);
			}
			// Even unsigned operations sign-extend each 32-bit result separately.
			a.Asr(x12, x11, 32);
			a.Sxtw(x11, w11);
		}
		a.Str(x11, lo);
		a.Str(x12, hi);
		if (!divide && rd)
			a.Str(x11, GPR(rd));
	}

	void Emit(MacroAssembler& a, u32 code)
	{
		const PackedInstruction packed = DecodePacked(code);
		if (packed.op != PackedOp::None)
		{
			EmitPacked(a, code, packed);
			return;
		}
		if (IsHiLo(code))
		{
			EmitHiLo(a, code);
			return;
		}
		const u32 op = code >> 26, rs = (code >> 21) & 31, rt = (code >> 16) & 31;
		const u32 rd = (code >> 11) & 31, sa = (code >> 6) & 31;
		const u32 dest = op ? rt : rd;
		if (!dest)
			return;
		a.Ldr(x9, GPR(rs));
		if (op)
		{
			a.Mov(x10, static_cast<u64>(static_cast<s64>(static_cast<int16_t>(code))));
			switch (op)
			{
				case 9: // ADDIU wraps at 32 bits, then sign extends to 64 bits.
					a.Add(w9, w9, w10);
					a.Sxtw(x9, w9);
					break;
				case 25:
					a.Add(x9, x9, x10);
					break;
				case 10:
				case 11:
					a.Cmp(x9, x10);
					a.Cset(x9, op == 10 ? lt : lo);
					break;
				case 12:
				case 13:
				case 14:
					a.Mov(x10, code & 0xffff);
					if (op == 12)
						a.And(x9, x9, x10);
					else if (op == 13)
						a.Orr(x9, x9, x10);
					else
						a.Eor(x9, x9, x10);
					break;
				case 15:
					a.Mov(x9, static_cast<u64>(static_cast<s64>(static_cast<s32>(code << 16))));
					break;
			}
		}
		else
		{
			a.Ldr(x10, GPR(rt));
			const u32 function = code & 63;
			switch (function)
			{
				case 0:
					a.Lsl(w9, w10, sa);
					a.Sxtw(x9, w9);
					break;
				case 2:
					a.Lsr(w9, w10, sa);
					a.Sxtw(x9, w9);
					break;
				case 3:
					a.Asr(w9, w10, sa);
					a.Sxtw(x9, w9);
					break;
				case 4:
					a.Lsl(w9, w10, w9);
					a.Sxtw(x9, w9);
					break;
				case 6:
					a.Lsr(w9, w10, w9);
					a.Sxtw(x9, w9);
					break;
				case 7:
					a.Asr(w9, w10, w9);
					a.Sxtw(x9, w9);
					break;
				case 20:
					a.Lsl(x9, x10, x9);
					break;
				case 22:
					a.Lsr(x9, x10, x9);
					break;
				case 23:
					a.Asr(x9, x10, x9);
					break;
				case 10:
				case 11:
					a.Ldr(x11, GPR(rd));
					a.Cmp(x10, 0);
					a.Csel(x9, x9, x11, function == 10 ? eq : ne);
					break;
				case 33:
					a.Add(w9, w9, w10);
					a.Sxtw(x9, w9);
					break;
				case 35:
					a.Sub(w9, w9, w10);
					a.Sxtw(x9, w9);
					break;
				case 36:
					a.And(x9, x9, x10);
					break;
				case 37:
					a.Orr(x9, x9, x10);
					break;
				case 38:
					a.Eor(x9, x9, x10);
					break;
				case 39:
					a.Orr(x9, x9, x10);
					a.Mvn(x9, x9);
					break;
				case 42:
				case 43:
					a.Cmp(x9, x10);
					a.Cset(x9, function == 42 ? lt : lo);
					break;
				case 45:
					a.Add(x9, x9, x10);
					break;
				case 47:
					a.Sub(x9, x9, x10);
					break;
				case 56:
				case 60:
					a.Lsl(x9, x10, sa + (function == 60 ? 32 : 0));
					break;
				case 58:
				case 62:
					a.Lsr(x9, x10, sa + (function == 62 ? 32 : 0));
					break;
				case 59:
				case 63:
					a.Asr(x9, x10, sa + (function == 63 ? 32 : 0));
					break;
			}
		}
		// EE integer instructions preserve the high 64 bits of each 128-bit GPR.
		a.Str(x9, GPR(dest));
	}

	void EmitPosition(MacroAssembler& a, u32 pc, u32 code)
	{
		a.Mov(w9, pc);
		a.Str(w9, MemOperand(x0, offsetof(cpuRegisters, pc)));
		a.Mov(w9, code);
		a.Str(w9, MemOperand(x0, offsetof(cpuRegisters, code)));
	}

	// Only the plain register-transfer ops (QMFC2/CFC2/QMTC2/CTC2, selected by
	// rs) and the "CO" macro-mode arithmetic ops (rs bit 4 set, dispatched by
	// COP2_SPECIAL exactly as the interpreter's own top-level table does) are
	// recognized. BC2 (rs == 8) still ends the native block and falls back to
	// the interpreter, since it is a branch this codegen does not model.
	bool SupportsCOP2(u32 code)
	{
		const u32 rs = (code >> 21) & 31;
		return rs == 1 || rs == 2 || rs == 5 || rs == 6 || (rs & 16) != 0;
	}

	// COP2 macro-mode ops are left as plain interpreter calls rather than
	// reimplemented in native code: their VU0 pipeline/flag/sync semantics
	// (see the TODOs atop VU0.cpp) are delicate, and every one of them already
	// operates on cpuRegs/VU0 globals with no arguments, so calling the exact
	// same, already-validated handler costs one Blr while still letting the
	// surrounding integer/branch code stay natively compiled instead of the
	// whole block dropping to the interpreter at the first COP2 instruction.
	void EmitCOP2(MacroAssembler& a, u32 code, u32 pc)
	{
		const u32 rs = (code >> 21) & 31;
		EmitPosition(a, pc + 4, code);
		void (*handler)() = (rs & 16) ? &COP2_SPECIAL : rs == 1 ? &QMFC2 : rs == 2 ? &CFC2 : rs == 5 ? &QMTC2 : &CTC2;
		// A generated block is a leaf function as far as its caller is concerned:
		// its own trailing Ret() relies on lr still holding the return address
		// TryExecute's call left there. Blr overwrites lr with this call site, so
		// it must be saved/restored around the call, matching VU1Pipeline.cpp's
		// stub calls (x15 just keeps sp's mandatory 16-byte alignment here; this
		// codegen never keeps anything live in it across instructions).
		a.Stp(x15, lr, MemOperand(sp, -16, PreIndex));
		a.Mov(x16, reinterpret_cast<uintptr_t>(handler));
		a.Blr(x16);
		a.Ldp(x15, lr, MemOperand(sp, 16, PostIndex));
		// COP2 handlers are ordinary C++ functions under the AAPCS64 ABI and are
		// free to clobber every caller-saved register; x0 (the cpuRegisters*
		// base every subsequent field access assumes stays live for the whole
		// compiled function) and x14 (the vtlb map base, cached once per block
		// when the block needs it at all) must be restored afterward.
		a.Mov(x0, reinterpret_cast<uintptr_t>(&cpuRegs));
		a.Mov(x14, reinterpret_cast<uintptr_t>(vtlb_private::vtlbdata.vmap));
	}

	void EmitBranch(MacroAssembler& a, u32 code, u32 delay, u32 pc, u32 preceding)
	{
		using namespace Arm64EE::CodeGenerator;
		const u32 op = code >> 26, rs = (code >> 21) & 31, rt = (code >> 16) & 31;
		const bool conditional = op != 0 && op != 2 && op != 3;
		const bool likely = (op >= 20 && op <= 23) || (op == 1 && (rt & 2));
		const u32 link = op == 3 || (op == 1 && (rt & 16)) ? 31 : op == 0 && (code & 63) == 9 ? (code >> 11) & 31 :
		                                                                                        0;
		// Capture register targets before either the link or delay slot overwrites
		// their source. x15 is preserved by the nontrapping integer emitter.
		if (op == 0)
			a.Ldr(w15, GPR(rs));
		else if (op == 2 || op == 3)
			a.Mov(w15, ((pc + 4) & 0xf0000000u) | ((code & 0x03ffffffu) << 2));
		else
			a.Mov(w15, pc + 4 + static_cast<int16_t>(code) * 4);
		if (link)
		{
			a.Mov(w9, pc + 8);
			a.Str(x9, GPR(link));
		}
		Label untaken;
		if (conditional)
		{
			// REGIMM links are unconditional and precede the rs comparison in the
			// reference interpreter, including the rs == ra alias.
			a.Ldr(x9, GPR(rs));
			Condition taken;
			if (op == 4 || op == 5 || op == 20 || op == 21)
			{
				a.Ldr(x10, GPR(rt));
				a.Cmp(x9, x10);
				taken = (op == 4 || op == 20) ? eq : ne;
			}
			else
			{
				a.Cmp(x9, 0);
				taken = op == 1 ? ((rt & 1) ? ge : lt) : (op == 6 || op == 22) ? le :
				                                                                 gt;
			}
			a.B(InvertCondition(taken), &untaken);
		}
		Emit(a, delay);
		EmitPosition(a, pc + 8, delay);
		a.Lsl(x15, x15, 32);
		a.Mov(w0, (preceding + 2) | EncodeExit(EEBlockExit::TakenBranch));
		a.Orr(x0, x0, x15);
		a.Ret();
		if (conditional)
		{
			a.Bind(&untaken);
			EmitPosition(a, pc + (likely ? 8 : 4), code);
			// BEQ/BNE and annulled likely branches test events without committing
			// cycles. Other untaken branches simply continue at the delay slot.
			a.Mov(w0, (preceding + 1) | EncodeExit((likely || op == 4 || op == 5) ? EEBlockExit::EventTest : EEBlockExit::Continue));
			a.Ret();
		}
	}

	u32 MemorySize(u32 code)
	{
		switch (code >> 26)
		{
			case 30:
			case 31:
				return 16; // LQ, SQ
			case 55:
			case 63:
				return 8; // LD, SD
			case 35:
			case 39:
			case 43:
				return 4; // LW, LWU, SW
			case 33:
			case 37:
			case 41:
				return 2; // LH, LHU, SH
			case 32:
			case 36:
			case 40:
				return 1; // LB, LBU, SB
			default:
				return 0;
		}
	}

	void EmitMemory(MacroAssembler& a, u32 code, u32 pc, const u32* source, u32 source_bytes,
		Label* before, Label* after)
	{
		const u32 op = code >> 26, rs = (code >> 21) & 31, rt = (code >> 16) & 31;
		const u32 size = MemorySize(code);
		const bool store = op == 31 || op == 40 || op == 41 || op == 43 || op == 63;
		a.Ldr(w9, GPR(rs));
		a.Add(w9, w9, static_cast<int16_t>(code));
		if (size == 16)
			a.And(w9, w9, 0xfffffff0);
		else if (size > 1)
		{
			a.Tst(w9, size - 1);
			a.B(ne, before); // interpreter raises the original address error
		}
		if (!store && size <= 4)
		{
			// Counter reads can run event tests even when mapped directly.
			a.Lsr(w11, w9, 13);
			a.Cmp(w11, 0x8000);
			a.B(eq, before);
		}
		a.Lsr(w11, w9, vtlb_private::VTLB_PAGE_BITS);
		a.Ldr(x12, MemOperand(x14, x11, LSL, 3));
		a.Add(x12, x12, x9);
		a.Tbnz(x12, 63, before); // MMIO and unmapped accesses stay in the interpreter
		// Match the architectural PC/code at the access, including host write faults.
		a.Mov(w13, pc + 4);
		a.Str(w13, MemOperand(x0, offsetof(cpuRegisters, pc)));
		a.Mov(w13, code);
		a.Str(w13, MemOperand(x0, offsetof(cpuRegisters, code)));
		if (store)
		{
			if (size == 16)
			{
				a.Ldr(q0, GPR(rt));
				a.Str(q0, MemOperand(x12));
			}
			else
			{
				a.Ldr(x10, GPR(rt));
				if (size == 8)
					a.Str(x10, MemOperand(x12));
				else if (size == 4)
					a.Str(w10, MemOperand(x12));
				else if (size == 2)
					a.Strh(w10, MemOperand(x12));
				else
					a.Strb(w10, MemOperand(x12));
			}
			// Compare host addresses so virtual aliases also detect self-modifying code.
			a.Mov(x13, reinterpret_cast<uintptr_t>(source) - (size - 1));
			a.Sub(x13, x12, x13);
			a.Cmp(x13, source_bytes + size - 1);
			a.B(lo, after);
		}
		else
		{
			switch (op)
			{
				case 30:
					a.Ldr(q0, MemOperand(x12));
					break;
				case 32:
					a.Ldrsb(x10, MemOperand(x12));
					break;
				case 33:
					a.Ldrsh(x10, MemOperand(x12));
					break;
				case 35:
					a.Ldrsw(x10, MemOperand(x12));
					break;
				case 36:
					a.Ldrb(w10, MemOperand(x12));
					break;
				case 37:
					a.Ldrh(w10, MemOperand(x12));
					break;
				case 39:
					a.Ldr(w10, MemOperand(x12));
					break;
				case 55:
					a.Ldr(x10, MemOperand(x12));
					break;
			}
			if (rt)
			{
				if (size == 16)
					a.Str(q0, GPR(rt));
				else
					a.Str(x10, GPR(rt));
			}
		}
	}
} // namespace

bool Arm64EE::CodeGenerator::Supports(u32 code)
{
	return SupportsInteger(code) || MemorySize(code) != 0 || IsBranch(code) ||
	       ((code >> 26) == 18 && SupportsCOP2(code));
}

bool Arm64EE::CodeGenerator::SupportsDelaySlot(u32 code)
{
	return SupportsInteger(code);
}

size_t Arm64EE::CodeGenerator::Compile(u8* buffer, size_t capacity, u32 pc, const u32* source, std::span<const u32> words)
{
	MacroAssembler a(buffer, capacity);
	std::array<Label, MaxInstructions + 1> exits;
	if (std::any_of(words.begin(), words.end(), [](u32 code) { return MemorySize(code) != 0; }))
		a.Mov(x14, reinterpret_cast<uintptr_t>(vtlb_private::vtlbdata.vmap));
	for (u32 i = 0; i < words.size(); i++)
	{
		if (IsBranch(words[i]))
		{
			EmitBranch(a, words[i], words[i + 1], pc + i * 4, i);
			break;
		}
		if (MemorySize(words[i]))
			EmitMemory(a, words[i], pc + i * 4, source, words.size_bytes(), &exits[i], &exits[i + 1]);
		else if ((words[i] >> 26) == 18)
			EmitCOP2(a, words[i], pc + i * 4);
		else
			Emit(a, words[i]);
	}
	// Completion and early exits share one contract: PC/code describe the last
	// completed instruction, and the caller charges exactly that prefix's cycles.
	for (u32 completed = words.size();; completed--)
	{
		a.Bind(&exits[completed]);
		if (completed)
		{
			EmitPosition(a, pc + completed * 4, words[completed - 1]);
		}
		a.Mov(w0, completed | EncodeExit(completed ? EEBlockExit::Continue : EEBlockExit::NotHandled));
		a.Ret();
		if (!completed)
			break;
	}
	a.FinalizeCode();
	return a.GetSizeOfCodeGenerated();
}
