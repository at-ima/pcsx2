// SPDX-FileCopyrightText: 2026 PCSX2 Dev Team
// SPDX-License-Identifier: GPL-3.0+

#include "Common.h"
#include "arm64/EECodeGenerator.h"
#include "R5900OpcodeTables.h"
#include "VU.h"
#include "vtlb.h"
#include "common/HostSys.h"
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

	// ADD/ADDI/SUB and their doubleword forms raise an overflow exception. The
	// native code leaves the block before one of them when it would overflow,
	// so the interpreter raises it; they are therefore not allowed in delay slots.
	bool IsTrapping(u32 code)
	{
		const u32 op = code >> 26, function = code & 63;
		return op == 8 || op == 24 || (op == 0 && (function == 32 || function == 34 || function == 44 || function == 46));
	}

	// SYNC and PREF do nothing in the interpreter.
	bool IsNop(u32 code)
	{
		return (code >> 26) == 51 || ((code >> 26) == 0 && (code & 63) == 15);
	}

	enum class MiscOp
	{
		None,
		MoveFromSA, // MFSA
		MoveToSA, // MTSA
		MoveToSAByte, // MTSAB
		MoveToSAHalf, // MTSAH
		ShiftLeftHalf, // PSLLH
		ShiftRightHalf, // PSRLH
		ShiftArithmeticHalf, // PSRAH
		ShiftLeftWord, // PSLLW
		ShiftRightWord, // PSRLW
		ShiftArithmeticWord, // PSRAW
		CopyHalf, // PCPYH
		MultiplyHalf, // PMULTH
		MoveFromCOP0, // MFC0
	};

	// Other instructions that cannot fault, so they may also sit in delay slots.
	MiscOp DecodeMisc(u32 code)
	{
		const u32 op = code >> 26, function = code & 63, selector = (code >> 6) & 31;
		if (op == 0 && function == 40)
			return MiscOp::MoveFromSA;
		if (op == 0 && function == 41)
			return MiscOp::MoveToSA;
		if (op == 1 && ((code >> 16) & 31) == 24)
			return MiscOp::MoveToSAByte;
		if (op == 1 && ((code >> 16) & 31) == 25)
			return MiscOp::MoveToSAHalf;
		if (op == 28)
		{
			switch (function)
			{
				case 52:
					return MiscOp::ShiftLeftHalf;
				case 54:
					return MiscOp::ShiftRightHalf;
				case 55:
					return MiscOp::ShiftArithmeticHalf;
				case 60:
					return MiscOp::ShiftLeftWord;
				case 62:
					return MiscOp::ShiftRightWord;
				case 63:
					return MiscOp::ShiftArithmeticWord;
				case 9:
					return selector == 28 ? MiscOp::MultiplyHalf : MiscOp::None;
				case 41:
					return selector == 27 ? MiscOp::CopyHalf : MiscOp::None;
			}
		}
		// MFC0 of PCCR/PCR0/PCR1 (register 25) updates performance counters.
		if (op == 16 && ((code >> 21) & 31) == 0 && ((code >> 11) & 31) != 25)
			return MiscOp::MoveFromCOP0;
		return MiscOp::None;
	}

	// QFSRV reads SA at run time; SA above 15 is left to the interpreter.
	bool IsQuadFunnelShift(u32 code)
	{
		return (code >> 26) == 28 && (code & 63) == 40 && ((code >> 6) & 31) == 27;
	}

	bool SupportsInteger(u32 code)
	{
		if (IsHiLo(code) || DecodePacked(code).op != PackedOp::None || IsNop(code) || DecodeMisc(code) != MiscOp::None)
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

	// Low 64 bits of guest GPRs this block has already loaded or written, kept
	// in x2-x8 until the end of the block. Write-through: every write still
	// stores to cpuRegs, so exits need nothing and a value only has to be
	// forgotten when something else writes its GPR. Needs proper testing across
	// more games.
	class GprCache
	{
	public:
		void Reset() { *this = {}; }
		// Registers handed out for one guest instruction are not evicted by it.
		void BeginInstruction() { m_pinned = 0; }
		// The cached register holding `reg`, loading it on a miss.
		Register Read(MacroAssembler& a, u32 reg)
		{
			if (!m_host[reg])
			{
				Map(reg, Allocate());
				a.Ldr(XRegister(m_host[reg]), GPR(reg));
			}
			return Use(reg);
		}
		// A register to compute `reg`'s new value in; the caller stores it.
		Register Write(u32 reg)
		{
			if (!m_host[reg])
				Map(reg, Allocate());
			return Use(reg);
		}
		void Invalidate(u32 reg)
		{
			if (m_host[reg])
				m_occupied &= ~(1u << m_host[reg]);
			m_host[reg] = 0;
		}
		void InvalidateAll() { *this = {}; }

	private:
		static constexpr u32 First = 2, Last = 8;
		u32 Allocate()
		{
			u32 best = 0;
			for (u32 h = First; h <= Last; h++)
			{
				if (!(m_occupied & (1u << h)))
					return h;
				if (!(m_pinned & (1u << h)) && (!best || m_used[h] < m_used[best]))
					best = h;
			}
			pxAssert(best);
			m_host[m_guest[best]] = 0;
			m_occupied &= ~(1u << best);
			return best;
		}
		void Map(u32 reg, u32 h)
		{
			m_host[reg] = static_cast<u8>(h);
			m_guest[h] = static_cast<u8>(reg);
			m_occupied |= 1u << h;
		}
		Register Use(u32 reg)
		{
			const u32 h = m_host[reg];
			m_used[h] = ++m_clock;
			m_pinned |= 1u << h;
			return XRegister(h);
		}
		std::array<u8, 32> m_host{}; // host register number, 0 when not cached
		std::array<u8, Last + 1> m_guest{};
		std::array<u32, Last + 1> m_used{};
		u32 m_occupied = 0, m_pinned = 0, m_clock = 0;
	} s_gpr;

	// A GPR store that does not go through s_gpr.
	MemOperand GPRWrite(u32 reg)
	{
		s_gpr.Invalidate(reg);
		return GPR(reg);
	}

	// fpuRegs is not a member of cpuRegisters: it is cpuRegs' sibling inside
	// cpuRegistersPack (R5900.h). x0 always holds &cpuRegs == &_cpuRegistersPack,
	// so fpuRegs is reachable at a fixed extra offset from x0 rather than
	// through offsetof(cpuRegisters, ...) the way GPR() works.
	MemOperand FPR(u32 reg)
	{
		return MemOperand(x0, offsetof(cpuRegistersPack, fpuRegs) + offsetof(fpuRegisters, fpr) + reg * sizeof(FPRreg));
	}

	MemOperand FACC()
	{
		return MemOperand(x0, offsetof(cpuRegistersPack, fpuRegs) + offsetof(fpuRegisters, ACC));
	}

	MemOperand FCR31()
	{
		return MemOperand(x0, offsetof(cpuRegistersPack, fpuRegs) + offsetof(fpuRegisters, fprc) + 31 * sizeof(u32));
	}

	constexpr u32 FPUflagC = 0x00800000;
	constexpr u32 FPUflagI = 0x00020000, FPUflagSI = 0x00000040;
	constexpr u32 FPUflagD = 0x00010000, FPUflagSD = 0x00000020;
	constexpr u32 FPUflagO = 0x00008000, FPUflagSO = 0x00000010;
	constexpr u32 FPUflagU = 0x00004000, FPUflagSU = 0x00000008;

	// Mirrors the interpreter's fpuDouble() (FPU.cpp): every FPU operand is
	// sanitized before use so PS2's non-IEEE FPU semantics (denormals flush to
	// signed zero, Inf/NaN bit patterns saturate to +/-Fmax) hold even though
	// the host FP unit is strictly IEEE-754. reg is clamped in place; w11/w12
	// are scratch.
	void EmitFpuClampOperand(MacroAssembler& a, const Register& reg)
	{
		Label flush, saturate, done;
		a.And(w11, reg, 0x7fffffff); // magnitude
		a.And(w12, reg, 0x80000000); // sign
		a.Cmp(w11, 0x00800000);
		a.B(lo, &flush); // magnitude < 0x00800000 -> exponent == 0 (denormal or zero)
		a.Cmp(w11, 0x7f800000);
		a.B(hs, &saturate); // magnitude >= 0x7f800000 -> exponent == 0xff (Inf; NaN can't
		                     // reach here since every operand was clamped before arithmetic)
		a.B(&done);
		a.Bind(&flush);
		a.Mov(reg, w12);
		a.B(&done);
		a.Bind(&saturate);
		a.Orr(reg, w12, 0x7f7fffff);
		a.Bind(&done);
	}

	// Mirrors the interpreter's checkOverflow()+checkUnderflow() (FPU.cpp) for
	// ADD_S/SUB_S/MUL_S: a result of exactly +/-Inf saturates to +/-Fmax and
	// sets O/SO; otherwise a true denormal flushes to signed zero and sets
	// U/SU, and either way O is cleared (checkOverflow's not-taken branch
	// always clears it for these ops, since they pass FPUflagO in cFlagsToSet).
	// w9 holds the raw result bits in/out; w11/w12 are scratch.
	void EmitFpuOutputFlags(MacroAssembler& a)
	{
		Label overflow, not_denormal, done;
		a.Ldr(w12, FCR31());
		a.And(w11, w9, 0x7fffffff);
		a.Cmp(w11, 0x7f800000);
		a.B(eq, &overflow);
		a.Bic(w12, w12, FPUflagO);
		a.Cmp(w11, 1);
		a.B(lo, &not_denormal); // magnitude == 0: exact zero, not a denormal
		a.Cmp(w11, 0x00800000);
		a.B(hs, &not_denormal); // magnitude in the normal range
		a.And(w9, w9, 0x80000000);
		a.Orr(w12, w12, FPUflagU | FPUflagSU);
		a.B(&done);
		a.Bind(&not_denormal);
		a.Bic(w12, w12, FPUflagU);
		a.B(&done);
		a.Bind(&overflow);
		a.And(w9, w9, 0x80000000);
		a.Orr(w9, w9, 0x7f7fffff);
		a.Orr(w12, w12, FPUflagO | FPUflagSO);
		a.Bind(&done);
		a.Str(w12, FCR31());
	}

	// checkOverflow()+checkUnderflow() with no flags (DIV.S): +/-Inf saturates
	// to +/-Fmax and a denormal flushes to signed zero. w9 in/out, w11 scratch.
	void EmitFpuSaturate(MacroAssembler& a)
	{
		Label overflow, done;
		a.And(w11, w9, 0x7fffffff);
		a.Cmp(w11, 0x7f800000);
		a.B(eq, &overflow);
		a.Tst(w9, 0x7f800000);
		a.B(ne, &done);
		a.And(w9, w9, 0x80000000);
		a.B(&done);
		a.Bind(&overflow);
		a.And(w9, w9, 0x80000000);
		a.Orr(w9, w9, 0x7f7fffff);
		a.Bind(&done);
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
		a.Str(q0, GPRWrite(rd));
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
				a.Str(x9, GPRWrite(rd));
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
			a.Str(x11, GPRWrite(rd));
	}

	void EmitTrapping(MacroAssembler& a, u32 code, Label* before)
	{
		const u32 op = code >> 26, rs = (code >> 21) & 31, rt = (code >> 16) & 31;
		const u32 dest = op ? rt : (code >> 11) & 31;
		const bool doubleword = op == 24 || (!op && (code & 63) >= 44);
		// The interpreter negates rt and then checks the addition, which differs
		// from a subtraction's overflow only for the most negative rt.
		const bool subtract = !op && ((code & 63) == 34 || (code & 63) == 46);
		a.Ldr(x9, GPR(rs));
		if (op)
			a.Mov(x10, static_cast<u64>(static_cast<s64>(static_cast<int16_t>(code))));
		else
			a.Ldr(x10, GPR(rt));
		if (doubleword)
		{
			if (subtract)
				a.Neg(x10, x10);
			a.Adds(x9, x9, x10);
			a.B(vs, before);
		}
		else
		{
			if (subtract)
				a.Neg(w10, w10);
			a.Adds(w9, w9, w10);
			a.B(vs, before);
			a.Sxtw(x9, w9);
		}
		if (dest)
			a.Str(x9, GPRWrite(dest));
	}

	MemOperand SA()
	{
		return MemOperand(x0, offsetof(cpuRegisters, sa));
	}

	MemOperand COP0(u32 reg)
	{
		return MemOperand(x0, offsetof(cpuRegisters, CP0) + reg * sizeof(u32));
	}

	void EmitMisc(MacroAssembler& a, u32 code, MiscOp op)
	{
		const u32 rs = (code >> 21) & 31, rt = (code >> 16) & 31, rd = (code >> 11) & 31, sa = (code >> 6) & 31;
		switch (op)
		{
			case MiscOp::MoveFromSA:
				if (rd)
				{
					a.Ldr(w9, SA());
					a.Str(x9, GPRWrite(rd));
				}
				return;
			case MiscOp::MoveToSA:
				a.Ldr(w9, GPR(rs));
				a.Str(w9, SA());
				return;
			case MiscOp::MoveToSAByte:
			case MiscOp::MoveToSAHalf:
			{
				const u32 mask = op == MiscOp::MoveToSAByte ? 0xf : 0x7;
				a.Ldr(w9, GPR(rs));
				a.And(w9, w9, mask);
				if (code & mask)
					a.Eor(w9, w9, code & mask);
				if (op == MiscOp::MoveToSAHalf)
					a.Lsl(w9, w9, 1);
				a.Str(w9, SA());
				return;
			}
			case MiscOp::MoveFromCOP0:
				if (rd == 9)
				{
					// Count catches up with the cycles committed so far, at least by one.
					a.Ldr(x9, MemOperand(x0, offsetof(cpuRegisters, cycle)));
					a.Ldr(x10, MemOperand(x0, offsetof(cpuRegisters, lastCOP0Cycle)));
					a.Subs(x10, x9, x10);
					a.Cinc(x10, x10, eq);
					a.Ldr(w11, COP0(9));
					a.Add(w11, w11, w10);
					a.Str(w11, COP0(9));
					a.Str(x9, MemOperand(x0, offsetof(cpuRegisters, lastCOP0Cycle)));
				}
				else if (rt && rd != 24) // register 24 only logs
				{
					a.Ldr(w11, COP0(rd));
					if (rd == 12)
						a.And(w11, w11, 0xf0c79c1f);
				}
				if (rt && rd != 24)
				{
					a.Sxtw(x11, w11);
					a.Str(x11, GPRWrite(rt));
				}
				return;
			default:
				break;
		}
		if (!rd)
			return;
		a.Ldr(q1, GPR(rt));
		switch (op)
		{
			case MiscOp::ShiftLeftHalf:
				a.Shl(v0.V8H(), v1.V8H(), sa & 15);
				break;
			case MiscOp::ShiftRightHalf:
			case MiscOp::ShiftArithmeticHalf:
				if (!(sa & 15))
					a.Mov(v0.V16B(), v1.V16B());
				else if (op == MiscOp::ShiftRightHalf)
					a.Ushr(v0.V8H(), v1.V8H(), sa & 15);
				else
					a.Sshr(v0.V8H(), v1.V8H(), sa & 15);
				break;
			case MiscOp::ShiftLeftWord:
				a.Shl(v0.V4S(), v1.V4S(), sa);
				break;
			case MiscOp::ShiftRightWord:
			case MiscOp::ShiftArithmeticWord:
				if (!sa)
					a.Mov(v0.V16B(), v1.V16B());
				else if (op == MiscOp::ShiftRightWord)
					a.Ushr(v0.V4S(), v1.V4S(), sa);
				else
					a.Sshr(v0.V4S(), v1.V4S(), sa);
				break;
			case MiscOp::CopyHalf:
				a.Dup(v0.V8H(), v1.V8H(), 0);
				a.Dup(v2.V8H(), v1.V8H(), 4);
				a.Mov(v0.V2D(), 1, v2.V2D(), 1);
				break;
			default:
				pxFailRel("Invalid EE instruction");
				break;
		}
		a.Str(q0, GPRWrite(rd));
	}

	// PMULTH: eight signed halfword products. LO gets products 0, 1, 4, 5,
	// HI gets 2, 3, 6, 7 and rd the even ones.
	void EmitMultiplyHalf(MacroAssembler& a, u32 code)
	{
		const u32 rs = (code >> 21) & 31, rt = (code >> 16) & 31, rd = (code >> 11) & 31;
		a.Ldr(q1, GPR(rs));
		a.Ldr(q2, GPR(rt));
		a.Smull(v3.V4S(), v1.V4H(), v2.V4H());
		a.Smull2(v4.V4S(), v1.V8H(), v2.V8H());
		a.Zip1(v5.V2D(), v3.V2D(), v4.V2D());
		a.Zip2(v6.V2D(), v3.V2D(), v4.V2D());
		a.Str(q5, MemOperand(x0, offsetof(cpuRegisters, LO)));
		a.Str(q6, MemOperand(x0, offsetof(cpuRegisters, HI)));
		if (rd)
		{
			a.Uzp1(v7.V4S(), v3.V4S(), v4.V4S());
			a.Str(q7, GPRWrite(rd));
		}
	}

	// QFSRV: bytes SA..SA+15 of rs:rt. Leaves the block first when SA > 15.
	void EmitQuadFunnelShift(MacroAssembler& a, u32 code, Label* before)
	{
		const u32 rs = (code >> 21) & 31, rt = (code >> 16) & 31, rd = (code >> 11) & 31;
		a.Ldr(w9, SA());
		a.Cmp(w9, 15);
		a.B(hi, before);
		if (!rd)
			return;
		a.Ldr(q1, GPR(rt));
		a.Ldr(q2, GPR(rs));
		a.Sub(sp, sp, 32);
		a.Str(q1, MemOperand(sp));
		a.Str(q2, MemOperand(sp, 16));
		a.Ldr(q0, MemOperand(sp, x9));
		a.Add(sp, sp, 32);
		a.Str(q0, GPRWrite(rd));
	}

	void Emit(MacroAssembler& a, u32 code)
	{
		if (IsNop(code))
			return;
		if (const MiscOp misc = DecodeMisc(code); misc != MiscOp::None)
		{
			if (misc == MiscOp::MultiplyHalf)
				EmitMultiplyHalf(a, code);
			else
				EmitMisc(a, code, misc);
			return;
		}
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
		s_gpr.BeginInstruction();
		const u32 function = code & 63;
		// LUI and the constant shifts do not read rs.
		const bool shift_by_sa = !op && (function == 0 || function == 2 || function == 3 || function >= 56);
		const Register rs_value = op == 15 || shift_by_sa ? Register(xzr) : s_gpr.Read(a, rs);
		const Register rt_value = op ? Register(xzr) : s_gpr.Read(a, rt);
		const Register rd_value = !op && (function == 10 || function == 11) ? s_gpr.Read(a, rd) : Register(xzr);
		const Register r = s_gpr.Write(dest);
		if (op)
		{
			a.Mov(x10, static_cast<u64>(static_cast<s64>(static_cast<int16_t>(code))));
			switch (op)
			{
				case 9: // ADDIU wraps at 32 bits, then sign extends to 64 bits.
					a.Add(r.W(), rs_value.W(), w10);
					a.Sxtw(r, r.W());
					break;
				case 25:
					a.Add(r, rs_value, x10);
					break;
				case 10:
				case 11:
					a.Cmp(rs_value, x10);
					a.Cset(r, op == 10 ? lt : lo);
					break;
				case 12:
				case 13:
				case 14:
					a.Mov(x10, code & 0xffff);
					if (op == 12)
						a.And(r, rs_value, x10);
					else if (op == 13)
						a.Orr(r, rs_value, x10);
					else
						a.Eor(r, rs_value, x10);
					break;
				case 15:
					a.Mov(r, static_cast<u64>(static_cast<s64>(static_cast<s32>(code << 16))));
					break;
			}
		}
		else
		{
			switch (function)
			{
				case 0:
					a.Lsl(r.W(), rt_value.W(), sa);
					a.Sxtw(r, r.W());
					break;
				case 2:
					a.Lsr(r.W(), rt_value.W(), sa);
					a.Sxtw(r, r.W());
					break;
				case 3:
					a.Asr(r.W(), rt_value.W(), sa);
					a.Sxtw(r, r.W());
					break;
				case 4:
					a.Lsl(r.W(), rt_value.W(), rs_value.W());
					a.Sxtw(r, r.W());
					break;
				case 6:
					a.Lsr(r.W(), rt_value.W(), rs_value.W());
					a.Sxtw(r, r.W());
					break;
				case 7:
					a.Asr(r.W(), rt_value.W(), rs_value.W());
					a.Sxtw(r, r.W());
					break;
				case 20:
					a.Lsl(r, rt_value, rs_value);
					break;
				case 22:
					a.Lsr(r, rt_value, rs_value);
					break;
				case 23:
					a.Asr(r, rt_value, rs_value);
					break;
				case 10:
				case 11:
					a.Cmp(rt_value, 0);
					a.Csel(r, rs_value, rd_value, function == 10 ? eq : ne);
					break;
				case 33:
					a.Add(r.W(), rs_value.W(), rt_value.W());
					a.Sxtw(r, r.W());
					break;
				case 35:
					a.Sub(r.W(), rs_value.W(), rt_value.W());
					a.Sxtw(r, r.W());
					break;
				case 36:
					a.And(r, rs_value, rt_value);
					break;
				case 37:
					a.Orr(r, rs_value, rt_value);
					break;
				case 38:
					a.Eor(r, rs_value, rt_value);
					break;
				case 39:
					a.Orr(r, rs_value, rt_value);
					a.Mvn(r, r);
					break;
				case 42:
				case 43:
					a.Cmp(rs_value, rt_value);
					a.Cset(r, function == 42 ? lt : lo);
					break;
				case 45:
					a.Add(r, rs_value, rt_value);
					break;
				case 47:
					a.Sub(r, rs_value, rt_value);
					break;
				case 56:
				case 60:
					a.Lsl(r, rt_value, sa + (function == 60 ? 32 : 0));
					break;
				case 58:
				case 62:
					a.Lsr(r, rt_value, sa + (function == 62 ? 32 : 0));
					break;
				case 59:
				case 63:
					a.Asr(r, rt_value, sa + (function == 63 ? 32 : 0));
					break;
			}
		}
		// EE integer instructions preserve the high 64 bits of each 128-bit GPR.
		a.Str(r, GPR(dest));
	}

	void EmitPosition(MacroAssembler& a, u32 pc, u32 code)
	{
		a.Mov(w9, pc);
		a.Str(w9, MemOperand(x0, offsetof(cpuRegisters, pc)));
		a.Mov(w9, code);
		a.Str(w9, MemOperand(x0, offsetof(cpuRegisters, code)));
	}

	// Register transfer (MFC1/CFC1/MTC1/CTC1), CVT_S, the S-format arithmetic
	// except RSQRT_S, CVT_W and the C.cond.S compare family. Anything else
	// falls back to the interpreter. needs proper testing across games.
	bool SupportsCOP1(u32 code)
	{
		const u32 rs = (code >> 21) & 31;
		if (rs == 0 || rs == 2 || rs == 4 || rs == 6) // MFC1/CFC1/MTC1/CTC1
			return true;
		if (rs == 20) // W format: only CVT_S.W exists
			return (code & 0x3f) == 32;
		if (rs == 16) // S format
		{
			switch (code & 0x3f)
			{
				case 0: // ADD_S
				case 1: // SUB_S
				case 2: // MUL_S
				case 3: // DIV_S
				case 4: // SQRT_S
				case 24: // ADDA_S
				case 25: // SUBA_S
				case 26: // MULA_S
				case 28: // MADD_S
				case 29: // MSUB_S
				case 30: // MADDA_S
				case 31: // MSUBA_S
				case 40: // MAX_S
				case 41: // MIN_S
				case 5: // ABS_S
				case 6: // MOV_S
				case 7: // NEG_S
				case 36: // CVT_W
				case 48: // C_F
				case 50: // C_EQ
				case 52: // C_LT
				case 54: // C_LE
					return true;
			}
		}
		return false;
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
		a.Mov(x1, reinterpret_cast<uintptr_t>(&Arm64EE::CodeGenerator::g_link_state));
		a.Mov(x14, reinterpret_cast<uintptr_t>(vtlb_private::vtlbdata.vmap));
		// The call clobbered x2-x8, and the handlers write GPRs.
		s_gpr.InvalidateAll();
	}

	// Natively compiles the COP1 (FPU) instructions accepted by SupportsCOP1().
	// Semantics are matched instruction-for-instruction against pcsx2/FPU.cpp;
	// see EmitFpuClampOperand/EmitFpuOutputFlags for the non-IEEE clamping this
	// mirrors. needs proper testing across games, especially the O/U flag
	// bookkeeping (FCR31), since nothing else in this codegen depends on it.
	// No EmitPosition() call is needed here (unlike EmitCOP2): every case below
	// decodes register fields from the already-known `code` value directly
	// rather than through cpuRegs.code/pc the way the interpreter's own COP1
	// handlers do, and none of these instructions can fault.
	void EmitCOP1(MacroAssembler& a, u32 code)
	{
		const u32 rs = (code >> 21) & 31;
		const u32 ft = (code >> 16) & 31; // GPR index for transfers, else FPR ft
		const u32 fs = (code >> 11) & 31;
		const u32 fd = (code >> 6) & 31;
		if (rs == 0) // MFC1
		{
			if (ft)
			{
				a.Ldrsw(x9, FPR(fs));
				a.Str(x9, GPRWrite(ft));
			}
			return;
		}
		if (rs == 2) // CFC1
		{
			if (!ft)
				return;
			if (fs == 31)
				a.Ldrsw(x9, FCR31());
			else
				a.Mov(x9, fs == 0 ? 0x2E00 : 0);
			a.Str(x9, GPRWrite(ft));
			return;
		}
		if (rs == 4) // MTC1
		{
			a.Ldr(w9, GPR(ft));
			a.Str(w9, FPR(fs));
			return;
		}
		if (rs == 6) // CTC1
		{
			if (fs == 31)
			{
				a.Ldr(w9, GPR(ft));
				a.Str(w9, FCR31());
			}
			return;
		}
		if (rs == 20) // CVT_S.W: FdValf = (float)FsValSl
		{
			a.Ldr(w9, FPR(fs));
			a.Scvtf(s0, w9);
			a.Str(s0, FPR(fd));
			return;
		}
		// rs == 16: S format.
		const u32 function = code & 0x3f;
		if (function == 48 || function == 50 || function == 52 || function == 54) // C.F/C.EQ/C.LT/C.LE
		{
			a.Ldr(w12, FCR31());
			if (function == 48) // C_F: unconditionally clears C
			{
				a.Bic(w12, w12, FPUflagC);
				a.Str(w12, FCR31());
				return;
			}
			a.Ldr(w9, FPR(fs));
			a.Ldr(w10, FPR(ft));
			EmitFpuClampOperand(a, w9);
			EmitFpuClampOperand(a, w10);
			a.Fmov(s0, w9);
			a.Fmov(s1, w10);
			a.Fcmp(s0, s1);
			// Operands are pre-clamped and can never be NaN, so the ordinary
			// (non-unordered-aware) condition mnemonics already match the
			// interpreter's plain C++ ==/</<= comparisons exactly.
			const Condition cond = function == 50 ? eq : function == 52 ? lt : le;
			a.Ldr(w12, FCR31()); // reload: EmitFpuClampOperand above clobbered w12 as scratch
			Label set_c, done_c;
			a.B(cond, &set_c);
			a.Bic(w12, w12, FPUflagC);
			a.B(&done_c);
			a.Bind(&set_c);
			a.Orr(w12, w12, FPUflagC);
			a.Bind(&done_c);
			a.Str(w12, FCR31());
			return;
		}
		a.Ldr(w9, FPR(fs));
		switch (function)
		{
			case 5: // ABS_S
				a.And(w9, w9, 0x7fffffff);
				a.Str(w9, FPR(fd));
				a.Ldr(w12, FCR31());
				a.Bic(w12, w12, FPUflagO | FPUflagU);
				a.Str(w12, FCR31());
				return;
			case 6: // MOV_S
				a.Str(w9, FPR(fd));
				return;
			case 7: // NEG_S
				a.Eor(w9, w9, 0x80000000);
				a.Str(w9, FPR(fd));
				a.Ldr(w12, FCR31());
				a.Bic(w12, w12, FPUflagO | FPUflagU);
				a.Str(w12, FCR31());
				return;
			case 36: // CVT_W
			{
				a.And(w10, w9, 0x7f800000); // exponent field of the original value
				a.And(w13, w9, 0x80000000); // sign of the original value
				Label sat, done_cvt;
				a.Cmp(w10, 0x4E800000);
				a.B(hi, &sat);
				a.Fmov(s0, w9);
				a.Fcvtzs(w9, s0); // round toward zero, matching the interpreter's (s32) cast
				a.B(&done_cvt);
				a.Bind(&sat);
				a.Mov(w9, 0x7fffffff);
				a.Mov(w11, 0x80000000);
				a.Cmp(w13, 0);
				a.Csel(w9, w9, w11, eq); // sign == 0 -> 0x7fffffff, else -> 0x80000000
				a.Bind(&done_cvt);
				a.Str(w9, FPR(fd));
				return;
			}
			case 3: // DIV_S
			{
				Label zero, done;
				a.Ldr(w10, FPR(ft));
				a.Tst(w10, 0x7f800000);
				a.B(eq, &zero); // checkDivideByZero(): denormal divisors count as zero
				EmitFpuClampOperand(a, w9);
				EmitFpuClampOperand(a, w10);
				a.Fmov(s0, w9);
				a.Fmov(s1, w10);
				a.Fdiv(s0, s0, s1);
				a.Fmov(w9, s0);
				EmitFpuSaturate(a);
				a.B(&done);
				a.Bind(&zero);
				a.Ldr(w12, FCR31());
				a.Mov(w13, FPUflagD | FPUflagSD);
				a.Mov(w11, FPUflagI | FPUflagSI);
				a.Tst(w9, 0x7f800000);
				a.Csel(w13, w11, w13, eq); // 0/0 is invalid rather than a division by zero
				a.Orr(w12, w12, w13);
				a.Str(w12, FCR31());
				a.Eor(w9, w9, w10);
				a.And(w9, w9, 0x80000000);
				a.Orr(w9, w9, 0x7f7fffff);
				a.Bind(&done);
				a.Str(w9, FPR(fd));
				return;
			}
			case 4: // SQRT_S: of ft, not fs
			{
				Label zero, positive, done;
				a.Ldr(w10, FPR(ft));
				a.Ldr(w12, FCR31());
				a.Bic(w12, w12, FPUflagI | FPUflagD);
				a.Tst(w10, 0x7f800000);
				a.B(eq, &zero);
				a.Tbz(w10, 31, &positive);
				a.Orr(w12, w12, FPUflagI | FPUflagSI);
				a.Bind(&positive);
				a.Str(w12, FCR31());
				a.And(w10, w10, 0x7fffffff);
				EmitFpuClampOperand(a, w10);
				a.Fmov(s0, w10);
				a.Fsqrt(s0, s0);
				a.Fmov(w9, s0);
				a.B(&done);
				a.Bind(&zero);
				a.Str(w12, FCR31());
				a.And(w9, w10, 0x80000000);
				a.Bind(&done);
				a.Str(w9, FPR(fd));
				return;
			}
			case 40: // MAX_S
			case 41: // MIN_S
				// fp_max()/fp_min(): signed integer order, reversed when both are negative.
				a.Ldr(w10, FPR(ft));
				a.Cmp(w9, w10);
				a.Csel(w12, w9, w10, gt);
				a.Csel(w13, w9, w10, lt);
				a.And(w11, w9, w10);
				a.Cmp(w11, 0);
				if (function == 40)
					a.Csel(w9, w13, w12, lt);
				else
					a.Csel(w9, w12, w13, lt);
				a.Str(w9, FPR(fd));
				a.Ldr(w12, FCR31());
				a.Bic(w12, w12, FPUflagO | FPUflagU);
				a.Str(w12, FCR31());
				return;
			case 28: // MADD_S
			case 29: // MSUB_S
				// The product is rounded and clamped before the accumulator is added.
				a.Ldr(w10, FPR(ft));
				EmitFpuClampOperand(a, w9);
				EmitFpuClampOperand(a, w10);
				a.Fmov(s0, w9);
				a.Fmov(s1, w10);
				a.Fmul(s0, s0, s1);
				a.Fmov(w10, s0);
				EmitFpuClampOperand(a, w10);
				a.Ldr(w9, FACC());
				EmitFpuClampOperand(a, w9);
				a.Fmov(s0, w9);
				a.Fmov(s1, w10);
				if (function == 28)
					a.Fadd(s0, s0, s1);
				else
					a.Fsub(s0, s0, s1);
				a.Fmov(w9, s0);
				EmitFpuOutputFlags(a);
				a.Str(w9, FPR(fd));
				return;
			case 30: // MADDA_S
			case 31: // MSUBA_S
				// The interpreter's `ACC += fs * ft` compiles to a fused multiply-add
				// on ARM64, with the accumulator unclamped; match it.
				a.Ldr(w10, FPR(ft));
				EmitFpuClampOperand(a, w9);
				EmitFpuClampOperand(a, w10);
				a.Fmov(s0, w9);
				a.Fmov(s1, w10);
				a.Ldr(s2, FACC());
				if (function == 30)
					a.Fmadd(s0, s0, s1, s2);
				else
					a.Fmsub(s0, s0, s1, s2);
				a.Fmov(w9, s0);
				EmitFpuOutputFlags(a);
				a.Str(w9, FACC());
				return;
			default: // ADD_S(0)/SUB_S(1)/MUL_S(2), and ADDA_S(24)/SUBA_S(25)/MULA_S(26) into ACC
			{
				a.Ldr(w10, FPR(ft));
				EmitFpuClampOperand(a, w9);
				EmitFpuClampOperand(a, w10);
				a.Fmov(s0, w9);
				a.Fmov(s1, w10);
				if ((function & 7) == 0)
					a.Fadd(s0, s0, s1);
				else if ((function & 7) == 1)
					a.Fsub(s0, s0, s1);
				else
					a.Fmul(s0, s0, s1);
				a.Fmov(w9, s0);
				EmitFpuOutputFlags(a);
				a.Str(w9, function >= 24 ? FACC() : FPR(fd));
				return;
			}
		}
	}

	u32 MemorySize(u32 code)
	{
		switch (code >> 26)
		{
			case 30:
			case 31:
			case 54:
			case 62:
				return 16; // LQ, SQ, LQC2, SQC2
			case 55:
			case 63:
				return 8; // LD, SD
			case 35:
			case 39:
			case 43:
			case 49:
			case 57:
				return 4; // LW, LWU, SW, LWC1, SWC1
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

	bool IsVU0Transfer(u32 code)
	{
		return (code >> 26) == 54 || (code >> 26) == 62; // LQC2, SQC2
	}

	// Leaves the host address of a RAM access in x12, or branches to `before`
	// when the interpreter has to perform it. Clobbers x9-x12 only.
	void EmitAddress(MacroAssembler& a, u32 code, Label* before)
	{
		const u32 op = code >> 26, rs = (code >> 21) & 31;
		const u32 size = MemorySize(code);
		const bool store = op == 31 || op == 40 || op == 41 || op == 43 || op == 63 || op == 57 || op == 62;
		if (IsVU0Transfer(code))
		{
			// vu0Sync(): let the interpreter catch a running microprogram up first.
			a.Mov(x10, reinterpret_cast<uintptr_t>(&VU0.VI[REG_VPU_STAT].UL));
			a.Ldr(w10, MemOperand(x10));
			a.Tbnz(w10, 0, before);
		}
		s_gpr.BeginInstruction();
		a.Add(w9, s_gpr.Read(a, rs).W(), static_cast<int16_t>(code));
		// LQ/SQ ignore the low address bits. LQC2/SQC2 pass them on to the
		// memory handlers, so the interpreter keeps the unaligned ones.
		if (size == 16 && !IsVU0Transfer(code))
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
	}

	// Performs the access at x12. A store overlapping this block's source
	// branches to `after` when one is given.
	void EmitAccess(MacroAssembler& a, u32 code, u32 pc, const u32* source, u32 source_bytes, Label* after)
	{
		const u32 op = code >> 26, rt = (code >> 16) & 31;
		const u32 size = MemorySize(code);
		const bool store = op == 31 || op == 40 || op == 41 || op == 43 || op == 63 || op == 57 || op == 62;
		const bool fpu = op == 49 || op == 57; // LWC1/SWC1 target fpuRegs.fpr, not a GPR
		// Match the architectural PC/code at the access, including host write faults.
		a.Mov(w13, pc + 4);
		a.Str(w13, MemOperand(x0, offsetof(cpuRegisters, pc)));
		a.Mov(w13, code);
		a.Str(w13, MemOperand(x0, offsetof(cpuRegisters, code)));
		if (store)
		{
			if (IsVU0Transfer(code))
			{
				a.Mov(x10, reinterpret_cast<uintptr_t>(&VU0.VF[rt]));
				a.Ldr(q0, MemOperand(x10));
				a.Str(q0, MemOperand(x12));
			}
			else if (size == 16)
			{
				a.Ldr(q0, GPR(rt));
				a.Str(q0, MemOperand(x12));
			}
			else if (fpu)
			{
				a.Ldr(w10, FPR(rt));
				a.Str(w10, MemOperand(x12));
			}
			else
			{
				// EmitAddress pinned rs for this instruction, so rt cannot evict it.
				const Register value = s_gpr.Read(a, rt);
				if (size == 8)
					a.Str(value, MemOperand(x12));
				else if (size == 4)
					a.Str(value.W(), MemOperand(x12));
				else if (size == 2)
					a.Strh(value.W(), MemOperand(x12));
				else
					a.Strb(value.W(), MemOperand(x12));
			}
			if (after)
			{
				// Compare host addresses so virtual aliases also detect self-modifying code.
				a.Mov(x13, reinterpret_cast<uintptr_t>(source) - (size - 1));
				a.Sub(x13, x12, x13);
				a.Cmp(x13, source_bytes + size - 1);
				a.B(lo, after);
			}
		}
		else
		{
			switch (op)
			{
				case 30:
				case 54:
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
				case 49:
					a.Ldr(w10, MemOperand(x12));
					break;
				case 55:
					a.Ldr(x10, MemOperand(x12));
					break;
			}
			if (IsVU0Transfer(code))
			{
				// LQC2 into vf0 reads and discards, like the interpreter.
				if (rt)
				{
					a.Mov(x10, reinterpret_cast<uintptr_t>(&VU0.VF[rt]));
					a.Str(q0, MemOperand(x10));
				}
			}
			else if (fpu)
			{
				// Unlike GPR r0, fpr[0] is a real writable register.
				a.Str(w10, FPR(rt));
			}
			else if (rt)
			{
				if (size == 16)
					a.Str(q0, GPRWrite(rt));
				else
					a.Str(x10, GPRWrite(rt));
			}
		}
	}

	void EmitMemory(MacroAssembler& a, u32 code, u32 pc, const u32* source, u32 source_bytes,
		Label* before, Label* after)
	{
		EmitAddress(a, code, before);
		EmitAccess(a, code, pc, source, source_bytes, after);
	}
	// Set by Compile() for the block being emitted.
	struct ExitInfo
	{
		bool linkable = false;
		std::span<const u32> cycles;
		u8* buffer = nullptr;
		// 2 - CP0.Config bit 18 when the block was compiled; the dispatcher drops
		// every block when that bit changes.
		u32 cycle_scale = 2;
	} s_exit;

	// While chaining, loads *block_cycles + cycles[completed] (scaled by the
	// same CP0.Config bit TryExecute uses) into w11, with x13 = block_cycles.
	// x1 holds &g_link_state for the whole block. Branches to `off` when
	// chaining is off. Clobbers x9-x13 only, so x0 and a taken branch's x15
	// survive.
	void EmitAddCycles(MacroAssembler& a, u32 completed, Label* off)
	{
		using namespace Arm64EE::CodeGenerator;
		a.Ldr(w9, MemOperand(x1, offsetof(LinkState, chaining)));
		a.Cbz(w9, off);
		a.Ldr(x13, MemOperand(x1, offsetof(LinkState, block_cycles)));
		a.Ldr(w11, MemOperand(x13));
		a.Add(w11, w11, s_exit.cycles[completed] * s_exit.cycle_scale);
	}

	// A returning exit: w0 = value, plus x15 (already shifted) as the target
	// when `with_target`. While chaining, a linkable block also adds its own
	// cycles and says so, since the caller may have entered a different block.
	void EmitReturn(MacroAssembler& a, u32 completed, u32 value, bool with_target)
	{
		using namespace Arm64EE::CodeGenerator;
		if (s_exit.linkable && completed)
		{
			Label off, done;
			EmitAddCycles(a, completed, &off);
			a.Str(w11, MemOperand(x13));
			a.Mov(x16, CyclesCommitted);
			a.B(&done);
			a.Bind(&off);
			a.Mov(x16, 0);
			a.Bind(&done);
			a.Mov(w0, value);
			a.Orr(x0, x0, x16);
		}
		else
		{
			a.Mov(w0, value);
		}
		if (with_target)
			a.Orr(x0, x0, x15);
		a.Ret();
	}

	enum class LinkKind
	{
		Taken, // the driver's intFinishBranch + intUpdateCPUCycles + deadline check
		EventTest, // deadline check only
		Continue, // nothing
	};

	// cpuRegs.code is decoding scratch: the interpreter and the COP2 handlers
	// set it before they read it. Exits store it only when they return to C++,
	// so it still names the last completed instruction there; a jump to the
	// next block leaves it for that block's own exits.
	void EmitCode(MacroAssembler& a, u32 code)
	{
		a.Mov(w9, code);
		a.Str(w9, MemOperand(x0, offsetof(cpuRegisters, code)));
	}

	// The chaining path of an exit whose next pc is known at compile time.
	// `code` is the last completed instruction. Jumps to `classic`, having
	// stored nothing, when chaining is off; otherwise stores pc, does the
	// driver's work for this exit and either jumps to the linked block or
	// returns to C++.
	void EmitLinkedExit(MacroAssembler& a, LinkKind kind, u32 completed, u32 next, u32 code, Label* classic)
	{
		using namespace Arm64EE::CodeGenerator;
		Label request, due;
		EmitAddCycles(a, completed, classic);
		a.Mov(w9, next);
		a.Str(w9, MemOperand(x0, offsetof(cpuRegisters, pc)));
		if (kind == LinkKind::Taken)
		{
			// intUpdateCPUCycles() at EECycleRate 0, which linkable blocks require:
			// cycle += max(block_cycles >> 3, 1); block_cycles &= 7.
			a.Lsr(w10, w11, 3);
			a.Cmp(w10, 1);
			a.Csinc(w10, w10, wzr, hs);
			a.And(w11, w11, 7);
			a.Ldr(x9, MemOperand(x0, offsetof(cpuRegisters, cycle)));
			a.Add(x9, x9, x10);
			a.Str(x9, MemOperand(x0, offsetof(cpuRegisters, cycle)));
		}
		a.Str(w11, MemOperand(x13));
		if (kind != LinkKind::Continue)
		{
			// EEBranchEventDue(): signed 64-bit distance to the deadline. A taken
			// exit still has the new cycle in x9.
			if (kind != LinkKind::Taken)
				a.Ldr(x9, MemOperand(x0, offsetof(cpuRegisters, cycle)));
			a.Ldr(x10, MemOperand(x0, offsetof(cpuRegisters, nextEventCycle)));
			a.Sub(x9, x9, x10);
			a.Tbz(x9, 63, &due);
		}
		// A link is only valid in the generation it was made in: any drop of
		// compiled blocks bumps it, including one from a write fault taken
		// inside the block that is running now.
		a.Ldr(w9, MemOperand(x1, offsetof(LinkState, generation)));
		u64 slot;
		{
			vixl::ExactAssemblyScope scope(&a, 5 * kInstructionSize);
			a.ldr(w10, 4); // the literal below, four instructions on
			a.cmp(w9, w10);
			a.b(&request, ne);
			slot = reinterpret_cast<uintptr_t>(s_exit.buffer + a.GetCursorOffset()) -
			       reinterpret_cast<uintptr_t>(SysMemory::GetEERec());
			a.b(&request); // PatchLink() retargets this to the next block
			a.dc32(0); // generation of the link; 0 is never current
		}
		a.Bind(&request);
		EmitCode(a, code);
		a.Mov(x0, (slot << 32) | LinkRequest | CyclesCommitted);
		a.Ret();
		if (kind != LinkKind::Continue)
		{
			a.Bind(&due);
			EmitCode(a, code);
			a.Mov(x0, EventDue | CyclesCommitted);
			a.Ret();
		}
	}

	// The chaining path of a JR/JALR, whose target is in w15: the driver's
	// work for a taken branch, then a jump through g_indirect when the target
	// block is there, or a return asking ExecuteChained() to look it up.
	void EmitIndirectExit(MacroAssembler& a, u32 completed, u32 code, Label* classic)
	{
		using namespace Arm64EE::CodeGenerator;
		static_assert(sizeof(IndirectEntry) == 16);
		Label due, miss;
		EmitAddCycles(a, completed, classic);
		a.Str(w15, MemOperand(x0, offsetof(cpuRegisters, pc)));
		a.Lsr(w10, w11, 3);
		a.Cmp(w10, 1);
		a.Csinc(w10, w10, wzr, hs);
		a.And(w11, w11, 7);
		a.Ldr(x9, MemOperand(x0, offsetof(cpuRegisters, cycle)));
		a.Add(x9, x9, x10);
		a.Str(x9, MemOperand(x0, offsetof(cpuRegisters, cycle)));
		a.Str(w11, MemOperand(x13));
		a.Ldr(x10, MemOperand(x0, offsetof(cpuRegisters, nextEventCycle)));
		a.Sub(x9, x9, x10);
		a.Tbz(x9, 63, &due);
		a.Mov(x10, reinterpret_cast<uintptr_t>(g_indirect.data()));
		a.Ubfx(w9, w15, 2, IndirectBits);
		a.Add(x10, x10, Operand(x9, LSL, 4));
		a.Ldp(w9, w11, MemOperand(x10));
		a.Cmp(w9, w15);
		a.B(ne, &miss);
		a.Ldr(w9, MemOperand(x1, offsetof(LinkState, generation)));
		a.Cmp(w11, w9);
		a.B(ne, &miss);
		a.Ldr(x16, MemOperand(x10, 8));
		a.Br(x16);
		a.Bind(&miss);
		EmitCode(a, code);
		a.Mov(x0, NextBlock | CyclesCommitted);
		a.Ret();
		a.Bind(&due);
		EmitCode(a, code);
		a.Mov(x0, EventDue | CyclesCommitted);
		a.Ret();
	}

	u32 BranchLink(u32 code)
	{
		const u32 op = code >> 26, rt = (code >> 16) & 31;
		return op == 3 || (op == 1 && (rt & 16)) ? 31 : op == 0 && (code & 63) == 9 ? (code >> 11) & 31 :
		                                                                              0;
	}

	// `before` exits ahead of the branch. A memory delay slot resolves its
	// address first, so an access the interpreter has to perform leaves before
	// the link register or anything else is written, and the interpreter then
	// runs the branch and its delay slot.
	void EmitBranch(MacroAssembler& a, u32 code, u32 delay, u32 pc, u32 preceding, const u32* source,
		u32 source_bytes, Label* before)
	{
		using namespace Arm64EE::CodeGenerator;
		const u32 op = code >> 26, rs = (code >> 21) & 31, rt = (code >> 16) & 31;
		const bool conditional = op != 0 && op != 2 && op != 3;
		const bool likely = (op >= 20 && op <= 23) || (op == 1 && (rt & 2)) || ((op == 16 || op == 17) && (rt & 2));
		const u32 link = BranchLink(code);
		const bool memory_delay = MemorySize(delay) != 0;
		if (memory_delay)
			EmitAddress(a, delay, before); // x12 stays live until EmitAccess below
		// Capture register targets before either the link or delay slot overwrites
		// their source. x15 is preserved by the nontrapping integer emitter.
		s_gpr.BeginInstruction();
		if (op == 0)
			a.Mov(w15, s_gpr.Read(a, rs).W());
		else if (op == 2 || op == 3)
			a.Mov(w15, ((pc + 4) & 0xf0000000u) | ((code & 0x03ffffffu) << 2));
		else
			a.Mov(w15, pc + 4 + static_cast<int16_t>(code) * 4);
		if (link)
		{
			a.Mov(w9, pc + 8);
			a.Str(x9, GPRWrite(link));
		}
		Label untaken;
		if (conditional)
		{
			Condition taken;
			if (op == 16) // BC0F/BC0T/BC0FL/BC0TL: CPCOND0, from the DMAC's STAT and PCR.
			{
				a.Mov(x9, reinterpret_cast<uintptr_t>(&psHu32(DMAC_PCR)));
				a.Ldr(w9, MemOperand(x9));
				a.Mov(x10, reinterpret_cast<uintptr_t>(&psHu32(DMAC_STAT)));
				a.Ldr(w10, MemOperand(x10));
				a.Orn(w9, w10, w9);
				a.And(w9, w9, 0x3ff);
				a.Cmp(w9, 0x3ff);
				taken = (rt & 1) ? eq : ne;
			}
			else if (op == 17) // BC1F/BC1T/BC1FL/BC1TL: branch on FCR31's C bit, not a GPR.
			{
				a.Ldr(w9, FCR31());
				a.Tst(w9, FPUflagC);
				taken = (rt & 1) ? ne : eq;
			}
			else
			{
				// REGIMM links are unconditional and precede the rs comparison in
				// the reference interpreter, including the rs == ra alias.
				s_gpr.BeginInstruction();
				const Register rs_value = s_gpr.Read(a, rs);
				if (op == 4 || op == 5 || op == 20 || op == 21)
				{
					a.Cmp(rs_value, s_gpr.Read(a, rt));
					taken = (op == 4 || op == 20) ? eq : ne;
				}
				else
				{
					a.Cmp(rs_value, 0);
					taken = op == 1 ? ((rt & 1) ? ge : lt) : (op == 6 || op == 22) ? le :
					                                                                 gt;
				}
			}
			a.B(InvertCondition(taken), &untaken);
		}
		// Nothing of this block runs after its delay slot, so a store there needs
		// no self-modification exit: a write fault drops every block and link.
		if (memory_delay)
			EmitAccess(a, delay, pc + 4, source, source_bytes, nullptr);
		else if ((delay >> 26) == 17)
			EmitCOP1(a, delay);
		else
			Emit(a, delay);
		if (s_exit.linkable && op != 0)
		{
			const u32 target = (op == 2 || op == 3) ? (((pc + 4) & 0xf0000000u) | ((code & 0x03ffffffu) << 2)) :
			                                          pc + 4 + static_cast<int16_t>(code) * 4;
			Label classic;
			EmitLinkedExit(a, LinkKind::Taken, preceding + 2, target, delay, &classic);
			a.Bind(&classic);
		}
		else if (s_exit.linkable)
		{
			// JR/JALR targets are only known at run time.
			Label classic;
			EmitIndirectExit(a, preceding + 2, delay, &classic);
			a.Bind(&classic);
		}
		EmitPosition(a, pc + 8, delay);
		a.Lsl(x15, x15, 32);
		EmitReturn(a, preceding + 2, (preceding + 2) | EncodeExit(EEBlockExit::TakenBranch), true);
		if (conditional)
		{
			a.Bind(&untaken);
			// BEQ/BNE and annulled likely branches test events without committing
			// cycles. Other untaken branches simply continue at the delay slot.
			const bool event_test = likely || op == 4 || op == 5;
			if (s_exit.linkable)
			{
				Label classic;
				EmitLinkedExit(a, event_test ? LinkKind::EventTest : LinkKind::Continue, preceding + 1, pc + (likely ? 8 : 4), code, &classic);
				a.Bind(&classic);
			}
			EmitPosition(a, pc + (likely ? 8 : 4), code);
			EmitReturn(a, preceding + 1, (preceding + 1) | EncodeExit(event_test ? EEBlockExit::EventTest : EEBlockExit::Continue), false);
		}
	}

} // namespace

bool Arm64EE::CodeGenerator::Supports(u32 code)
{
	return SupportsInteger(code) || IsTrapping(code) || IsQuadFunnelShift(code) || MemorySize(code) != 0 || IsBranch(code) ||
	       ((code >> 26) == 18 && SupportsCOP2(code)) || ((code >> 26) == 17 && SupportsCOP1(code));
}

bool Arm64EE::CodeGenerator::SupportsDelaySlot(u32 branch, u32 code)
{
	if (MemorySize(code))
	{
		// The address is computed before the branch writes its link register.
		const u32 link = BranchLink(branch);
		return !link || link != ((code >> 21) & 31);
	}
	return SupportsInteger(code) || ((code >> 26) == 17 && SupportsCOP1(code));
}

Arm64EE::CodeGenerator::LinkState Arm64EE::CodeGenerator::g_link_state;
std::array<Arm64EE::CodeGenerator::IndirectEntry, 1u << Arm64EE::CodeGenerator::IndirectBits> Arm64EE::CodeGenerator::g_indirect;

void Arm64EE::CodeGenerator::PatchLink(u8* slot, const void* target, u32 generation)
{
	// The code buffer is 64 MiB, well inside B's +-128 MiB range.
	const s64 delta = reinterpret_cast<const u8*>(target) - slot;
	const u32 b = 0x14000000u | (static_cast<u32>(delta >> 2) & 0x03ffffffu);
	HostSys::BeginCodeWrite();
	std::memcpy(slot + 4, &generation, sizeof(generation));
	std::memcpy(slot, &b, sizeof(b));
	HostSys::EndCodeWrite();
	HostSys::FlushInstructionCache(slot, 8);
}

size_t Arm64EE::CodeGenerator::Compile(u8* buffer, size_t capacity, u32 pc, const u32* source, std::span<const u32> words,
	std::span<const u32> cycles, bool linkable, bool self_check)
{
	MacroAssembler a(buffer, capacity);
	s_exit = {linkable, cycles, buffer, 2 - ((cpuRegs.CP0.n.Config >> 18) & 1)};
	s_gpr.Reset();
	std::array<Label, MaxInstructions + 1> exits;
	Label stale, copy;
	if (self_check)
	{
		// XOR the source with the copy compiled from, eight bytes at a time.
		// Links and g_indirect enter here too, so this replaces the dispatcher's
		// memcmp on every entry.
		const u32 bytes = static_cast<u32>(words.size_bytes());
		a.Mov(x16, reinterpret_cast<uintptr_t>(source));
		a.Adr(x17, &copy);
		for (u32 offset = 0; offset < bytes; offset += 8)
		{
			const bool word = bytes - offset == 4;
			const Register value = word ? Register(w9) : Register(x9), expected = word ? Register(w10) : Register(x10);
			a.Ldr(value, MemOperand(x16, offset));
			a.Ldr(expected, MemOperand(x17, offset));
			if (offset)
			{
				a.Eor(x9, x9, x10);
				a.Orr(x11, x11, x9);
			}
			else
				a.Eor(x11, x9, x10);
		}
		a.Cbnz(x11, &stale);
	}
	if (std::any_of(words.begin(), words.end(), [](u32 code) { return MemorySize(code) != 0; }))
		a.Mov(x14, reinterpret_cast<uintptr_t>(vtlb_private::vtlbdata.vmap));
	for (u32 i = 0; i < words.size(); i++)
	{
		if (IsBranch(words[i]))
		{
			EmitBranch(a, words[i], words[i + 1], pc + i * 4, i, source, words.size_bytes(), &exits[i]);
			break;
		}
		if (MemorySize(words[i]))
			EmitMemory(a, words[i], pc + i * 4, source, words.size_bytes(), &exits[i], &exits[i + 1]);
		else if (IsTrapping(words[i]))
			EmitTrapping(a, words[i], &exits[i]);
		else if (IsQuadFunnelShift(words[i]))
			EmitQuadFunnelShift(a, words[i], &exits[i]);
		else if ((words[i] >> 26) == 18)
			EmitCOP2(a, words[i], pc + i * 4);
		else if ((words[i] >> 26) == 17)
			EmitCOP1(a, words[i]);
		else
			Emit(a, words[i]);
	}
	// Completion and early exits share one contract: PC/code describe the last
	// completed instruction, and the caller charges exactly that prefix's cycles.
	for (u32 completed = words.size();; completed--)
	{
		a.Bind(&exits[completed]);
		// The end of the block falls through to the next pc. Earlier exits stop
		// before an access the interpreter has to perform, so they still return.
		if (linkable && completed && completed == words.size())
		{
			Label classic;
			EmitLinkedExit(a, LinkKind::Continue, completed, pc + completed * 4, words[completed - 1], &classic);
			a.Bind(&classic);
		}
		if (completed)
			EmitPosition(a, pc + completed * 4, words[completed - 1]);
		EmitReturn(a, completed, completed | EncodeExit(completed ? EEBlockExit::Continue : EEBlockExit::NotHandled), false);
		if (!completed)
			break;
	}
	if (self_check)
	{
		// pc is this block's entry and the exit that got here committed its
		// cycles, so the dispatcher only has to look the block up again. Outside
		// chaining the empty exit field reads as NotHandled.
		a.Bind(&stale);
		a.Mov(x0, NextBlock | CyclesCommitted);
		a.Ret();
		vixl::ExactAssemblyScope scope(&a, (words.size() + 2) * kInstructionSize);
		if (a.GetCursorOffset() & 7)
			a.nop();
		a.bind(&copy);
		for (u32 word : words)
			a.dc32(word);
	}
	a.FinalizeCode();
	return a.GetSizeOfCodeGenerated();
}
