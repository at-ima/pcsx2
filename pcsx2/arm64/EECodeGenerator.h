// SPDX-FileCopyrightText: 2026 PCSX2 Dev Team
// SPDX-License-Identifier: GPL-3.0+

#pragma once

#include "Interpreter.h"
#include <array>
#include <span>
#include <utility>

namespace Arm64EE::CodeGenerator
{
	constexpr u32 MaxInstructions = 32;
	bool Supports(u32 code);
	constexpr bool IsBranch(u32 code)
	{
		const u32 op = code >> 26;
		if (op == 0)
			return (code & 63) == 8 || (code & 63) == 9;
		if (op == 1)
		{
			switch ((code >> 16) & 31)
			{
				case 0:
				case 1:
				case 2:
				case 3:
				case 16:
				case 17:
				case 18:
				case 19:
					return true;
				default:
					return false;
			}
		}
		// COP1's BC1F/BC1T/BC1FL/BC1TL live under rs == 8; the other COP1 rs
		// values (register transfer, S/W-format arithmetic) are not branches.
		if (op == 17)
			return ((code >> 21) & 31) == 8;
		// COP0's BC0F/BC0T/BC0FL/BC0TL.
		if (op == 16)
			return ((code >> 21) & 31) == 8 && ((code >> 16) & 31) < 4;
		return (op >= 2 && op <= 7) || (op >= 20 && op <= 23);
	}
	// Run by calling the interpreter's handler from inside the block, which
	// saves the dispatcher round trip, block lookup and execI() that a
	// fallback costs: SYSCALL, CACHE and COP0's MTC0/ERET/EI/DI. SotC's kernel
	// runs SYSCALL/MTC0/ERET about 200k times a second each.
	constexpr bool IsInterpreterCall(u32 code)
	{
		const u32 op = code >> 26;
		if (op == 0)
			return (code & 63) == 12; // SYSCALL
		if (op == 47)
			return true; // CACHE
		if (op == 16)
		{
			const u32 rs = (code >> 21) & 31;
			const u32 function = code & 63;
			return rs == 4 || (rs == 16 && (function == 0x18 || function == 0x38 || function == 0x39));
		}
		return false;
	}
	// The block ends after an interpreter call that can change pc (SYSCALL,
	// ERET), the cycle scale (MTC0 Config) or memory (CACHE write-backs).
	// MTC0 of other registers, EI and DI change only COP0 state; like execI()
	// they only schedule an event test, so the block goes on.
	constexpr bool EndsBlockAfterCall(u32 code)
	{
		const u32 op = code >> 26;
		if (op != 16)
			return true;
		if (((code >> 21) & 31) == 16)
			return (code & 63) == 0x18;
		return ((code >> 11) & 31) == 16;
	}
	bool SupportsDelaySlot(u32 branch, u32 code);
	// A conditional branch that is not "likely" and can fall through: when it is
	// not taken, its delay slot runs as an ordinary instruction at pc + 4, so a
	// linkable block goes on past it instead of ending there.
	constexpr bool ContinuesAfterBranch(u32 code)
	{
		const u32 op = code >> 26, rs = (code >> 21) & 31, rt = (code >> 16) & 31;
		if (!IsBranch(code) || op == 0 || op == 2 || op == 3 || (op >= 20 && op <= 23))
			return false;
		if ((op == 1 || op == 16 || op == 17) && (rt & 2))
			return false; // REGIMM, COP0 and COP1 likely forms
		return !(op == 4 && rs == 0 && rt == 0); // BEQ $0, $0 is always taken
	}
	// Packed native return value: completed prefix in bits 0-7, exit action in
	// bits 8-9 and the taken target in bits 32-63. No events run in generated code.
	constexpr u32 CompletedMask = 0xff;
	constexpr u32 ExitShift = 8;
	constexpr u32 ExitMask = 3;
	constexpr u32 EncodeExit(EEBlockExit exit) { return static_cast<u32>(exit) << ExitShift; }

	// Block linking. While `chaining` is set (only by Arm64EE::ExecuteChained),
	// every exit of a linkable block adds its own completed-prefix cycles to
	// *block_cycles, and exits with a compile-time-known next pc (a static
	// taken branch, an untaken branch, the end of the block) also do the
	// driver's work for that exit themselves -- pc commit, cycle commit, event
	// deadline -- and then jump straight to the next block through a patchable
	// B. Dropping compiled blocks points every patched B back at its exit.
	// Otherwise they return to C++ with the flags below.
	struct LinkState
	{
		u32 chaining = 0;
		u32 generation = 0; // bumped whenever compiled blocks are dropped
		u32* block_cycles = nullptr;
	};
	extern LinkState g_link_state;
	constexpr u64 CyclesCommitted = 1u << 10; // the exit already added its cycles
	constexpr u64 EventDue = 1u << 11; // pc and cycles committed; the event deadline passed
	constexpr u64 LinkRequest = 1u << 12; // pc and cycles committed; bits 32-63 = slot offset from the code base
	constexpr u64 NextBlock = 1u << 13; // pc and cycles committed, nothing due; look up the next block

	// JR/JALR targets while chaining. ExecuteChained() records every trusted
	// block it dispatches here by pc; a register jump whose target matches an
	// entry of the current generation enters that block directly.
	struct IndirectEntry
	{
		u32 pc;
		u32 generation;
		const void* code;
	};
	constexpr u32 IndirectBits = 12;
	constexpr u32 IndirectIndex(u32 pc) { return (pc >> 2) & ((1u << IndirectBits) - 1); }
	extern std::array<IndirectEntry, 1u << IndirectBits> g_indirect;
	// A link slot is a B instruction. PatchLink() returns the B it replaced,
	// which UnpatchLinks() writes back when blocks are dropped.
	u32 PatchLink(u8* slot, const void* target);
	void UnpatchLinks(std::span<const std::pair<u8*, u32>> links);

	// Blocks run with the cycle bookkeeping in callee-saved registers, so a
	// chain of linked blocks never round-trips it through memory: x19 =
	// cpuRegs.cycle (also stored at every commit, so memory stays current),
	// w20 = *block_cycles, x21 = cpuRegs.nextEventCycle (reloaded after any
	// call into C++), x22 = the LinkState, x23 = vtlbdata.vmap for memory
	// accesses. EmitEnter() writes the stub C++
	// calls blocks through: Enter(&cpuRegs, &g_link_state, block) loads them,
	// runs the block and writes w20 back.
	using Enter = u64 (*)(cpuRegisters*, LinkState*, const void*);
	size_t EmitEnter(u8* buffer, size_t capacity);

	// Generated functions return the completed prefix and exit action. On an
	// unsupported memory access they leave PC at that instruction for fallback.
	// Stores overlapping this block's source exit immediately for revalidation.
	// `cycles[n]` is the fixed-point cost of the first n instructions; a
	// linkable block needs it to commit cycles itself while chaining.
	// `self_check` makes the block compare its source with `words` on entry
	// and return NextBlock | CyclesCommitted, having run nothing, when they
	// differ, so a block on a page that is not write-protected can be linked.
	size_t Compile(u8* buffer, size_t capacity, u32 pc, const u32* source, std::span<const u32> words,
		std::span<const u32> cycles, bool linkable, bool self_check);
} // namespace Arm64EE::CodeGenerator
