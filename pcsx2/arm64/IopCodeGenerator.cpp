// SPDX-FileCopyrightText: 2026 PCSX2 Dev Team
// SPDX-License-Identifier: GPL-3.0+

#include "Common.h"
#include "arm64/IopCodeGenerator.h"
#include "IopMem.h"
#include "vixl/aarch64/macro-assembler-aarch64.h"

#include <array>

namespace
{
	using namespace vixl::aarch64;
	using Arm64IOP::CodeGenerator::EncodeExit;
	using Arm64IOP::CodeGenerator::Exit;
	using Arm64IOP::CodeGenerator::IsBranch;
	using Arm64IOP::CodeGenerator::MaxInstructions;

	bool SupportsInteger(u32 code)
	{
		const u32 op = code >> 26;
		switch (op)
		{
			case 8:
			case 9:
			case 10:
			case 11:
			case 12:
			case 13:
			case 14:
			case 15:
				return true;
			case 16: // COP0: MFC0/CFC0/MTC0/CTC0. RFE (rs 16) is left to the interpreter.
			{
				const u32 rs = (code >> 21) & 31;
				return rs == 0 || rs == 2 || rs == 4 || rs == 6;
			}
			case 0:
				switch (code & 63)
				{
					case 0:
					case 2:
					case 3:
					case 4:
					case 6:
					case 7:
					case 16:
					case 17:
					case 18:
					case 19:
					case 24:
					case 25:
					case 26:
					case 27:
					case 32:
					case 33:
					case 34:
					case 35:
					case 36:
					case 37:
					case 38:
					case 39:
					case 42:
					case 43:
						return true;
					default:
						return false;
				}
			default:
				return false;
		}
	}

	// LB/LBU/LH/LHU/LW: bits 0-1 encode the sign/width. SB/SH/SW mirror them.
	u32 MemorySize(u32 code)
	{
		switch (code >> 26)
		{
			case 32:
			case 36:
			case 40:
				return 1; // LB, LBU, SB
			case 33:
			case 37:
			case 41:
				return 2; // LH, LHU, SH
			case 35:
			case 43:
				return 4; // LW, SW
			default:
				return 0;
		}
	}

	MemOperand GPR(u32 reg) { return MemOperand(x0, offsetof(psxRegisters, GPR) + reg * 4); }
	MemOperand CP0(u32 reg) { return MemOperand(x0, offsetof(psxRegisters, CP0) + reg * 4); }

	void EmitPosition(MacroAssembler& a, u32 pc, u32 code)
	{
		a.Mov(w9, pc);
		a.Str(w9, MemOperand(x0, offsetof(psxRegisters, pc)));
		a.Mov(w9, code);
		a.Str(w9, MemOperand(x0, offsetof(psxRegisters, code)));
	}

	// Straight-line ALU/shift/mult/div/COP0-register-transfer codegen for one
	// non-branch, non-memory instruction. Mirrors R3000AInterpreter.cpp's
	// per-opcode functions exactly, including ADD/ADDI having no overflow trap
	// (treated the same as ADDU/ADDIU there) and DIV/DIVU's zero-divisor and
	// INT_MIN/-1 overflow results (the latter falls out of AArch64 SDIV's
	// defined wraparound behavior without any special-casing, same reasoning
	// as EECodeGenerator.cpp's EmitHiLo divide path).
	void Emit(MacroAssembler& a, u32 code)
	{
		const u32 op = code >> 26, rs = (code >> 21) & 31, rt = (code >> 16) & 31;
		const u32 rd = (code >> 11) & 31, sa = (code >> 6) & 31, funct = code & 63;

		if (op == 16) // COP0
		{
			if (rs == 4 || rs == 6) // MTC0/CTC0
			{
				a.Ldr(w9, GPR(rt));
				a.Str(w9, CP0(rd));
			}
			else if (rt) // MFC0/CFC0
			{
				a.Ldr(w9, CP0(rd));
				a.Str(w9, GPR(rt));
			}
			return;
		}

		if (op) // Immediate-form ops: destination is always rt.
		{
			if (!rt)
				return;
			a.Ldr(w9, GPR(rs));
			switch (op)
			{
				case 8: // ADDI: no overflow trap, matches psxADDI treating it as ADDIU.
				case 9: // ADDIU
					a.Add(w9, w9, static_cast<int16_t>(code));
					break;
				case 10: // SLTI
				case 11: // SLTIU
					a.Cmp(w9, static_cast<int16_t>(code));
					a.Cset(w9, op == 10 ? lt : lo);
					break;
				case 12: // ANDI
				case 13: // ORI
				case 14: // XORI
					a.Mov(w10, code & 0xffff);
					if (op == 12)
						a.And(w9, w9, w10);
					else if (op == 13)
						a.Orr(w9, w9, w10);
					else
						a.Eor(w9, w9, w10);
					break;
				case 15: // LUI ignores rs.
					a.Mov(w9, code << 16);
					break;
			}
			a.Str(w9, GPR(rt));
			return;
		}

		// SPECIAL. rd is the destination for every op reached here except MTHI/MTLO.
		switch (funct)
		{
			case 0: // SLL
				if (!rd)
					return;
				a.Ldr(w9, GPR(rt));
				a.Lsl(w9, w9, sa);
				break;
			case 2: // SRL
				if (!rd)
					return;
				a.Ldr(w9, GPR(rt));
				a.Lsr(w9, w9, sa);
				break;
			case 3: // SRA
				if (!rd)
					return;
				a.Ldr(w9, GPR(rt));
				a.Asr(w9, w9, sa);
				break;
			case 4: // SLLV
				if (!rd)
					return;
				a.Ldr(w9, GPR(rt));
				a.Ldr(w10, GPR(rs));
				a.Lsl(w9, w9, w10);
				break;
			case 6: // SRLV
				if (!rd)
					return;
				a.Ldr(w9, GPR(rt));
				a.Ldr(w10, GPR(rs));
				a.Lsr(w9, w9, w10);
				break;
			case 7: // SRAV
				if (!rd)
					return;
				a.Ldr(w9, GPR(rt));
				a.Ldr(w10, GPR(rs));
				a.Asr(w9, w9, w10);
				break;
			case 16: // MFHI
				if (!rd)
					return;
				a.Ldr(w9, GPR(32));
				break;
			case 17: // MTHI
				a.Ldr(w9, GPR(rs));
				a.Str(w9, GPR(32));
				return;
			case 18: // MFLO
				if (!rd)
					return;
				a.Ldr(w9, GPR(33));
				break;
			case 19: // MTLO
				a.Ldr(w9, GPR(rs));
				a.Str(w9, GPR(33));
				return;
			case 24: // MULT
			case 25: // MULTU
			{
				a.Ldr(w9, GPR(rs));
				a.Ldr(w10, GPR(rt));
				if (funct == 24)
					a.Smull(x9, w9, w10);
				else
					a.Umull(x9, w9, w10);
				a.Str(w9, GPR(33)); // LO = low 32 bits
				a.Lsr(x10, x9, 32);
				a.Str(w10, GPR(32)); // HI = high 32 bits
				return;
			}
			case 26: // DIV
			case 27: // DIVU
			{
				const bool unsigned_op = funct == 27;
				a.Ldr(w9, GPR(rs));
				a.Ldr(w10, GPR(rt));
				if (unsigned_op)
					a.Udiv(w11, w9, w10);
				else
					a.Sdiv(w11, w9, w10);
				a.Msub(w12, w11, w10, w9);
				a.Mov(w13, 0xffffffffu);
				if (!unsigned_op)
				{
					a.Cmp(w9, 0);
					a.Cneg(w13, w13, lt);
				}
				a.Cmp(w10, 0);
				a.Csel(w11, w11, w13, ne); // LO: quotient, or the zero-divisor override
				a.Csel(w12, w12, w9, ne); // HI: remainder, or the dividend on zero-divisor
				a.Str(w12, GPR(32));
				a.Str(w11, GPR(33));
				return;
			}
			case 32: // ADD: no overflow trap, matches psxADD treating it as ADDU.
			case 33: // ADDU
				if (!rd)
					return;
				a.Ldr(w9, GPR(rs));
				a.Ldr(w10, GPR(rt));
				a.Add(w9, w9, w10);
				break;
			case 34: // SUB
			case 35: // SUBU
				if (!rd)
					return;
				a.Ldr(w9, GPR(rs));
				a.Ldr(w10, GPR(rt));
				a.Sub(w9, w9, w10);
				break;
			case 36: // AND
				if (!rd)
					return;
				a.Ldr(w9, GPR(rs));
				a.Ldr(w10, GPR(rt));
				a.And(w9, w9, w10);
				break;
			case 37: // OR
				if (!rd)
					return;
				a.Ldr(w9, GPR(rs));
				a.Ldr(w10, GPR(rt));
				a.Orr(w9, w9, w10);
				break;
			case 38: // XOR
				if (!rd)
					return;
				a.Ldr(w9, GPR(rs));
				a.Ldr(w10, GPR(rt));
				a.Eor(w9, w9, w10);
				break;
			case 39: // NOR
				if (!rd)
					return;
				a.Ldr(w9, GPR(rs));
				a.Ldr(w10, GPR(rt));
				a.Orr(w9, w9, w10);
				a.Mvn(w9, w9);
				break;
			case 42: // SLT
			case 43: // SLTU
				if (!rd)
					return;
				a.Ldr(w9, GPR(rs));
				a.Ldr(w10, GPR(rt));
				a.Cmp(w9, w10);
				a.Cset(w9, funct == 42 ? lt : lo);
				break;
			default:
				return;
		}
		a.Str(w9, GPR(rd));
	}

	// JR/JALR (SPECIAL), BLTZ/BGEZ/BLTZAL/BGEZAL (REGIMM) and JAL/BEQ/BNE/BLEZ/
	// BGTZ. Unlike EE there is no branch-likely: the delay slot always executes
	// exactly once regardless of whether the branch is taken, matching how
	// R3000AInterpreter.cpp's outer while(!branch2) execI() loop naturally runs
	// the next instruction in sequence when a conditional branch isn't taken
	// (doBranch(), and therefore also its event-test poll, only runs when the
	// branch actually is taken).
	void EmitBranch(MacroAssembler& a, u32 code, u32 delay, u32 pc, u32 preceding)
	{
		const u32 op = code >> 26, rs = (code >> 21) & 31, rt = (code >> 16) & 31, funct = code & 63;
		const bool conditional = op != 0 && op != 3;
		const u32 link = (op == 0 && funct == 9) ? ((code >> 11) & 31) : op == 3 ? 31 :
		                                          (op == 1 && (rt & 16))         ? 31 :
		                                                                           0;

		if (op == 0)
			a.Ldr(w15, GPR(rs)); // JR/JALR
		else if (op == 3)
			a.Mov(w15, ((pc + 4) & 0xf0000000u) | ((code & 0x03ffffffu) << 2)); // JAL
		else
			a.Mov(w15, pc + 4 + static_cast<int16_t>(code) * 4); // REGIMM/BEQ/BNE/BLEZ/BGTZ

		if (link)
		{
			a.Mov(w9, pc + 8);
			a.Str(w9, GPR(link));
		}

		if (conditional)
		{
			// Snapshot the outcome into w14 before the delay slot's own codegen can
			// clobber the condition flags (e.g. an SLT delay-slot instruction).
			a.Ldr(w9, GPR(rs));
			Condition taken;
			if (op == 4 || op == 5)
			{
				a.Ldr(w10, GPR(rt));
				a.Cmp(w9, w10);
				taken = op == 4 ? eq : ne;
			}
			else if (op == 6 || op == 7)
			{
				a.Cmp(w9, 0);
				taken = op == 6 ? le : gt;
			}
			else
			{
				a.Cmp(w9, 0);
				taken = (rt & 1) ? ge : lt;
			}
			a.Cset(w14, taken);
		}

		Emit(a, delay); // Always executes, taken or not.
		a.Mov(w13, delay);
		a.Str(w13, MemOperand(x0, offsetof(psxRegisters, code)));

		if (conditional)
		{
			Label untaken;
			a.Cbz(w14, &untaken);
			a.Str(w15, MemOperand(x0, offsetof(psxRegisters, pc)));
			a.Mov(w0, (preceding + 2) | EncodeExit(Exit::TakenBranch));
			a.Ret();
			a.Bind(&untaken);
			a.Mov(w9, pc + 8);
			a.Str(w9, MemOperand(x0, offsetof(psxRegisters, pc)));
			a.Mov(w0, (preceding + 2) | EncodeExit(Exit::Continue));
			a.Ret();
		}
		else
		{
			a.Str(w15, MemOperand(x0, offsetof(psxRegisters, pc)));
			a.Mov(w0, (preceding + 2) | EncodeExit(Exit::TakenBranch));
			a.Ret();
		}
	}

	// Inlines only the RAM-and-mirrors fast path (physical address below the
	// 8MB window iopMemFetch32() itself trusts), gated on the LUT slot being
	// non-null. Anything else -- MMIO pages, the SIF/hardware register windows
	// that alias into the same 64KB-granule LUT slot despite needing dispatch,
	// unmapped addresses -- falls back to the interpreter for this one
	// instruction, same as EECodeGenerator.cpp's EmitMemory does for its own
	// handler/unmapped cases. Needs proper testing against real IOP DMA/SPU2
	// register access patterns straddling this boundary.
	void EmitMemory(MacroAssembler& a, u32 code, u32 pc, const u32* source, u32 source_bytes, Label* before, Label* after)
	{
		const u32 op = code >> 26, rs = (code >> 21) & 31, rt = (code >> 16) & 31;
		const u32 size = MemorySize(code);
		const bool store = op == 40 || op == 41 || op == 43;

		a.Ldr(w9, GPR(rs));
		a.Add(w9, w9, static_cast<int16_t>(code));
		a.And(w9, w9, 0x1fffffff);
		a.Cmp(w9, 0x00800000);
		a.B(hs, before);

		// Bakes in the current LUT base pointer rather than indirecting through the
		// global psxMemWLUT/psxMemRLUT variable at runtime: iopMemAlloc()/Release()
		// only (re)allocate it across a full VM shutdown/startup, which already
		// clears this block cache (see Arm64IOP::Reset()/Shutdown()), so a cached
		// block never outlives the LUT array it was compiled against.
		a.Mov(x14, reinterpret_cast<uintptr_t>(store ? static_cast<const void*>(psxMemWLUT) : static_cast<const void*>(psxMemRLUT)));
		a.Lsr(w11, w9, 16);
		a.Ldr(x12, MemOperand(x14, x11, LSL, 3));
		a.Cbz(x12, before);
		a.And(w13, w9, 0xffff);
		a.Add(x12, x12, x13);

		EmitPosition(a, pc + 4, code);

		if (store)
		{
			a.Ldr(w10, GPR(rt));
			if (size == 4)
				a.Str(w10, MemOperand(x12));
			else if (size == 2)
				a.Strh(w10, MemOperand(x12));
			else
				a.Strb(w10, MemOperand(x12));
			// Compare host addresses so virtual mirrors also detect self-modifying code.
			a.Mov(x15, reinterpret_cast<uintptr_t>(source));
			a.Sub(x15, x12, x15);
			a.Cmp(x15, source_bytes);
			a.B(lo, after);
		}
		else
		{
			switch (op)
			{
				case 32:
					a.Ldrsb(w10, MemOperand(x12));
					break;
				case 33:
					a.Ldrsh(w10, MemOperand(x12));
					break;
				case 35:
					a.Ldr(w10, MemOperand(x12));
					break;
				case 36:
					a.Ldrb(w10, MemOperand(x12));
					break;
				case 37:
					a.Ldrh(w10, MemOperand(x12));
					break;
			}
			if (rt)
				a.Str(w10, GPR(rt));
		}
	}
} // namespace

bool Arm64IOP::CodeGenerator::Supports(u32 code)
{
	return SupportsInteger(code) || MemorySize(code) != 0 || IsBranch(code);
}

bool Arm64IOP::CodeGenerator::SupportsDelaySlot(u32 code)
{
	return SupportsInteger(code);
}

size_t Arm64IOP::CodeGenerator::Compile(u8* buffer, size_t capacity, u32 pc, const u32* source, std::span<const u32> words)
{
	MacroAssembler a(buffer, capacity);
	std::array<Label, MaxInstructions + 1> exits;
	for (u32 i = 0; i < words.size(); i++)
	{
		if (IsBranch(words[i]))
		{
			EmitBranch(a, words[i], words[i + 1], pc + i * 4, i);
			break;
		}
		if (MemorySize(words[i]))
			EmitMemory(a, words[i], pc + i * 4, source, static_cast<u32>(words.size_bytes()), &exits[i], &exits[i + 1]);
		else
			Emit(a, words[i]);
	}
	// Every completion count shares one contract: pc/code describe the last
	// completed instruction, and the caller charges exactly that many cycles.
	// Unreachable when the block ends in a branch (EmitBranch always returns
	// directly), same as EECodeGenerator.cpp's equivalent trailer.
	for (u32 completed = static_cast<u32>(words.size());; completed--)
	{
		a.Bind(&exits[completed]);
		if (completed)
			EmitPosition(a, pc + completed * 4, words[completed - 1]);
		a.Mov(w0, completed | EncodeExit(completed ? Exit::Continue : Exit::NotHandled));
		a.Ret();
		if (!completed)
			break;
	}
	a.FinalizeCode();
	return a.GetSizeOfCodeGenerated();
}
