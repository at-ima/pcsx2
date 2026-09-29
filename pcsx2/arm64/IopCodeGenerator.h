// SPDX-FileCopyrightText: 2026 PCSX2 Dev Team
// SPDX-License-Identifier: GPL-3.0+

#pragma once

#include "R3000A.h"
#include <span>

namespace Arm64IOP::CodeGenerator
{
	constexpr u32 MaxInstructions = 32;

	// Generated functions return the completed-instruction prefix (bits 0-7) and
	// the exit action (bits 8-9). psxRegs.pc/code always reflect the last
	// completed instruction on every exit path, matching what the interpreter's
	// own execI()/doBranch() leave behind. No cycle accounting, event testing or
	// exception raising happens inside generated code -- the caller (IopRecompiler's
	// ExecuteBlock) adds CompletedMask cycles and drives events/fallback between
	// native block executions, mirroring EECodeGenerator.h's contract.
	enum class Exit : u32
	{
		NotHandled,
		Continue,
		TakenBranch,
	};
	constexpr u32 CompletedMask = 0xff;
	constexpr u32 ExitShift = 8;
	constexpr u32 ExitMask = 3;
	constexpr u32 EncodeExit(Exit exit) { return static_cast<u32>(exit) << ExitShift; }

	// R3000A has no branch-likely and only JR/JALR (SPECIAL), BLTZ/BGEZ/BLTZAL/
	// BGEZAL (REGIMM) and JAL/BEQ/BNE/BLEZ/BGTZ. Plain J is deliberately excluded:
	// the interpreter's psxJ() checks the delay slot for an IRX import-table
	// magic marker and can redirect execution via irxImportExec() before ever
	// reaching doBranch(); reproducing that in native code isn't worth the risk
	// for an MVP, so J always falls back to the interpreter instead.
	constexpr bool IsBranch(u32 code)
	{
		const u32 op = code >> 26;
		if (op == 0)
			return (code & 63) == 8 || (code & 63) == 9;
		if (op == 1)
		{
			const u32 rt = (code >> 16) & 31;
			return rt == 0 || rt == 1 || rt == 16 || rt == 17;
		}
		return op == 3 || (op >= 4 && op <= 7);
	}

	// True for opcodes this codegen inlines directly: ALU/shift/mult/div,
	// MFC0/CFC0/MTC0/CTC0 (RFE is not included) and LB/LBU/LH/LHU/LW/SB/SH/SW
	// through the LUT fast path. GTE/COP2 (all of it), LWL/LWR/SWL/SWR,
	// SYSCALL/BREAK and plain J always return false here and fall back to the
	// interpreter, which raises exceptions / does unaligned merges exactly as
	// before.
	bool Supports(u32 code);
	// A delay slot instruction is only inlined if it is a supported non-memory,
	// non-branch, non-COP integer op -- kept conservative on purpose.
	bool SupportsDelaySlot(u32 code);

	size_t Compile(u8* buffer, size_t capacity, u32 pc, const u32* source, std::span<const u32> words);
} // namespace Arm64IOP::CodeGenerator
