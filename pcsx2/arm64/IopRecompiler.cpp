// SPDX-FileCopyrightText: 2026 PCSX2 Dev Team
// SPDX-License-Identifier: GPL-3.0+

#include "Common.h"
#include "arm64/IopRecompiler.h"
#include "arm64/IopCodeGenerator.h"
#include "IopHw.h"
#include "IopMem.h"
#include "common/HostSys.h"

#include <algorithm>
#include <array>
#include <cstring>
#include <unordered_map>
#include <vector>

namespace
{
	using Arm64IOP::BlockExit;
	using Arm64IOP::BlockResult;
	constexpr u32 MaxInstructions = Arm64IOP::CodeGenerator::MaxInstructions;

	struct Block
	{
		using Function = u32 (*)(psxRegisters*);
		std::array<u32, MaxInstructions> words{};
		u32 word_count = 0;
		const u32* source = nullptr;
		Function function = nullptr;
	};
	std::unordered_map<u32, Block> s_blocks;
	// Word-indexed reverse map over the compilable window (physical < 8MB),
	// so Clear() can find which blocks a store overlaps in O(bytes written)
	// instead of scanning every live block. Mirrors the x86 recompiler's
	// PSX_GETBLOCK table (iR3000A.cpp) for the same reason: Clear() runs on
	// every single IOP store through the psxMemWLUT fast path (see
	// IopMem.cpp), so its cost must not scale with the cache's size. Entries
	// use the block's raw (segment-bit-including) pc as the "occupied" marker
	// (word-aligned pcs are never all-ones); a stale/overlapping entry left
	// behind here is only a missed invalidation opportunity, never a
	// correctness gap, since TryExecuteImpl() independently memcmp-revalidates
	// every block before running it regardless of what this index says.
	constexpr u32 CompileWindowWords = 0x00800000u / 4;
	constexpr u32 EmptyWord = ~0u;
	std::vector<u32> s_word_pc;

	void IndexBlockWords(const Block& block, u32 pc, bool present)
	{
		const u32 start_word = (pc & 0x1fffffffu) >> 2;
		const u32 end_word = std::min(start_word + block.word_count, CompileWindowWords);
		for (u32 w = start_word; w < end_word; w++)
			s_word_pc[w] = present ? pc : EmptyWord;
	}
	struct LookupEntry
	{
		u32 pc = 0;
		u32 rejected_word = 0;
		const Block* block = nullptr;
		const u32* rejected_source = nullptr;
	};
	// unordered_map preserves element addresses across rehash. Reset/Shutdown
	// clear this non-owning cache before destroying the backing blocks.
	std::array<LookupEntry, 1024> s_lookup{};
	u8* s_write = nullptr;
	u8* s_write_limit = nullptr;

	u32 GetSupportedInstructionCount(const u32* source, u32 remaining)
	{
		const u32 code = source[0];
		if (!Arm64IOP::CodeGenerator::Supports(code))
			return 0;
		if (!Arm64IOP::CodeGenerator::IsBranch(code))
			return 1;
		return remaining >= 2 && Arm64IOP::CodeGenerator::SupportsDelaySlot(source[1]) ? 2 : 0;
	}

	// Allocation and compilation are cold. Keep their register/stack requirements
	// out of the dispatcher that runs for every cached block.
	__noinline Block& Compile(u32 pc, const u32* source)
	{
		{
			const auto existing = s_blocks.find(pc);
			if (existing != s_blocks.end())
			{
				// Word range may shrink/change on recompile; drop the old
				// mapping before word_count is overwritten below.
				IndexBlockWords(existing->second, pc, false);
			}
		}
		Block& block = s_blocks[pc];
		block = {};
		block.source = source;
		// IOP RAM is only 2MB (mirrored up to 8MB); a block never needs to reason
		// about crossing a "page" the way EE's write-protection tracking does, but
		// still cap at the buffer/array size and 8MB main-RAM window like TryExecute.
		const u32 limit = std::min(MaxInstructions, (0x00800000u - (pc & 0x1fffffffu)) / 4);
		while (block.word_count < limit)
		{
			const u32 count = GetSupportedInstructionCount(source + block.word_count, limit - block.word_count);
			if (!count)
				break;
			for (u32 i = 0; i < count; i++)
				block.words[block.word_count++] = source[block.word_count];
			if (count == 2)
				break;
		}
		if (!block.word_count)
		{
			// Cache rejected branch/delay pairs too. Revalidate both words so a
			// patched delay slot becomes eligible without recompiling every visit.
			block.word_count = std::min(2u, limit);
			std::copy_n(source, block.word_count, block.words.data());
			IndexBlockWords(block, pc, true);
			return block;
		}
		HostSys::BeginCodeWrite();
		// Capacity is measured against the buffer's true end, not s_write_limit
		// (which sits 16KB short of it purely to decide when TryExecuteImpl()
		// should Reset() before compiling). Passing s_write_limit - s_write here
		// would shrink the declared capacity to near zero as s_write approaches
		// it, making vixl's MacroAssembler believe the fixed external buffer is
		// full and try to Grow() it via realloc -- corrupting the heap, since
		// this buffer was never malloc'd. Matches EERecompiler.cpp's Compile().
		const size_t size = Arm64IOP::CodeGenerator::Compile(s_write, SysMemory::GetIOPRecEnd() - s_write,
			pc, source, std::span(block.words.data(), block.word_count));
		HostSys::EndCodeWrite();
		HostSys::FlushInstructionCache(s_write, static_cast<u32>(size));
		block.function = reinterpret_cast<Block::Function>(s_write);
		s_write += (size + 15) & ~size_t(15);
		IndexBlockWords(block, pc, true);
		return block;
	}

	// The IOP has no TLB/write-protection tracking at all (see IopMem.h/IopMem.cpp):
	// every cached entry is revalidated with a plain memcmp, unlike EE's recompiler
	// which can skip that check for pages it has write-protected.
	Arm64IOP::BlockResult TryExecuteImpl()
	{
		const u32 pc = psxRegs.pc;
		if (!CHECK_IOPREC || (pc & 3))
			return {};
		const u32 physical = pc & 0x1fffffff;
		// Only main RAM and its mirrors (< 8MB) are ever compiled, matching the
		// same restriction IopMem.h's iopMemFetch32() applies to instruction
		// fetch -- it keeps this path out of the 0x1f80xxxx/0x1d00xxxx MMIO
		// windows entirely, even though those regions also have non-null LUT
		// entries that would otherwise look "safe" to dereference directly.
		if (physical >= 0x00800000)
			return {};
		const uptr page = psxMemRLUT[physical >> 16];
		if (!page)
			return {};
		const u32* source = reinterpret_cast<const u32*>(page + (physical & 0xffff));
		if (!s_write || s_write > s_write_limit)
			Arm64IOP::Reset();
		LookupEntry& lookup = s_lookup[((pc >> 2) ^ (pc >> 12)) & (s_lookup.size() - 1)];
		const Block* block = lookup.pc == pc ? lookup.block : nullptr;
		if (!block)
		{
			if (lookup.pc == pc && lookup.rejected_source == source && lookup.rejected_word == source[0])
				return {};
			const auto it = s_blocks.find(pc);
			block = it != s_blocks.end() ? &it->second : nullptr;
			lookup = {pc, 0, block, nullptr};
		}
		if (!block || block->source != source ||
			std::memcmp(source, block->words.data(), block->word_count * 4) != 0)
		{
			if (!Arm64IOP::CodeGenerator::Supports(source[0]))
			{
				lookup = {pc, source[0], nullptr, source};
				return {};
			}
			block = &Compile(pc, source);
			lookup = {pc, 0, block, nullptr};
		}
		if (!block->function)
			return {};
		const u32 result = block->function(&psxRegs);
		const u32 completed = result & Arm64IOP::CodeGenerator::CompletedMask;
		const auto exit = static_cast<Arm64IOP::CodeGenerator::Exit>((result >> Arm64IOP::CodeGenerator::ExitShift) & Arm64IOP::CodeGenerator::ExitMask);
		return {static_cast<BlockExit>(exit), completed};
	}

	// Mirrors R3000AInterpreter.cpp's intExecuteBlock() outer cycle-budget loop
	// exactly (it is not exported, so this is necessarily a parallel copy --
	// keep the two in sync if that loop's HW_ICFG/PS1-mode accounting changes).
	// The inner step either runs a compiled block or falls back to exactly one
	// interpreter instruction via the same process-global psxBSC/... tables the
	// plain interpreter dispatches through, so any actual branch taken on the
	// fallback path (including its doBranch()-internal delay slot and event
	// test) is bit-identical to the plain interpreter -- we are calling the
	// very same function pointers, not reimplementing them.
	s32 ExecuteBlock(s32 eeCycles)
	{
		psxRegs.iopBreak = 0;
		psxRegs.iopCycleEE = eeCycles;
		u64 lastIOPCycle = 0;

		while (psxRegs.iopCycleEE > 0)
		{
			lastIOPCycle = psxRegs.cycle;
			if ((psxHu32(HW_ICFG) & 8) && ((psxRegs.pc & 0x1fffffffU) == 0xa0 || (psxRegs.pc & 0x1fffffffU) == 0xb0 || (psxRegs.pc & 0x1fffffffU) == 0xc0))
				psxBiosCall();

			bool taken = false;
			while (!taken)
			{
				const BlockResult result = TryExecuteImpl();
				if (result.exit == BlockExit::NotHandled)
				{
					// Run a full interpreter burst here -- straight through until an
					// actually-taken branch -- instead of one instruction at a time.
					// A compiled block can only ever start at a pc TryExecuteImpl()
					// is invoked with in the first place (that's the only place
					// Compile() is reached from), so re-checking it after every
					// single fallback instruction, including plain sequential ALU
					// code between branches, can never find anything new -- it just
					// pays the lookup's LUT/hash/array-access cost once per
					// instruction for no benefit. That per-instruction repricing is
					// what made this wrapper ~33% slower than the plain interpreter
					// even with zero native code ever compiled (see the comment on
					// psxCpu's assignment in VMManager.cpp for how that was
					// measured). `taken` is detected precisely by comparing pc to
					// the fallthrough address, matching doBranch()'s branch2
					// semantics exactly -- stricter than the IsBranch(code)
					// heuristic this loop used to exit on for every branch-class
					// opcode whether or not it actually branched.
					for (;;)
					{
						const u32 code = iopMemFetch32(psxRegs.pc);
						psxRegs.code = code;
						const u32 fallthrough_pc = psxRegs.pc + 4;
						psxRegs.pc = fallthrough_pc;
						psxRegs.cycle++;
						psxBSC[code >> 26]();
						if (psxRegs.pc != fallthrough_pc)
							break;
					}
					taken = true;
				}
				else
				{
					psxRegs.cycle += result.completed;
					if (result.exit == BlockExit::TakenBranch)
					{
						taken = true;
						// Replicates doBranch()'s post-branch deadline/pending-IRQ poll,
						// since no events run inside generated code (see IopCodeGenerator.h).
						if (static_cast<s64>(psxRegs.cycle - psxRegs.iopNextEventCycle) >= 0 ||
							((psxRegs.CP0.n.Status & 0xFE01) >= 0x401 && psxHu32(HW_ICTRL) != 0 &&
								(psxHu32(HW_ISTAT) & psxHu32(HW_IMASK)) != 0))
						{
							iopEventTest();
						}
					}
					// Exit::Continue (untaken conditional branch inside a compiled
					// block): not a real burst boundary, loop back and retry
					// TryExecuteImpl() at the new pc, exactly like the plain
					// interpreter falling through past an untaken branch.
				}
			}

			if ((psxHu32(HW_ICFG) & (1 << 3)))
			{
				// F = gcd(PS2CLK, PSXCLK) = 230400
				const u32 cnum = 1280; // PS2CLK / F
				const u32 cdenom = 147; // PSXCLK / F
				const u32 t = ((cnum * (psxRegs.cycle - lastIOPCycle)) + psxRegs.iopCycleEECarry);
				psxRegs.iopCycleEE -= t / cdenom;
				psxRegs.iopCycleEECarry = t % cdenom;
			}
			else
			{
				psxRegs.iopCycleEE -= (psxRegs.cycle - lastIOPCycle) * 8;
			}
		}

		return psxRegs.iopBreak + psxRegs.iopCycleEE;
	}

	// No write-protection/dirty tracking exists for IOP memory, unlike EE's
	// vtlb-based scheme, so this can't know ahead of time whether Addr/Size
	// actually overlaps compiled code. But TryExecuteImpl() already
	// revalidates every cached block with a memcmp before running it (see
	// above), so an imprecise overlap test here is only a missed optimization,
	// never a correctness gap -- a stale block that slips past this check is
	// still caught on its next visit. This matters because every IOP store
	// through the psxMemWLUT fast path calls Clear() unconditionally (see
	// IopMem.cpp), including ordinary data writes (stack, heap, DMA buffers)
	// that have nothing to do with code. Dropping the *entire* cache on every
	// one of those, as an earlier version of this function did, was measured
	// recompiling thousands of blocks per second for a working set of only a
	// handful of blocks, making the cache useless in practice. Needs proper testing against
	// real self-modifying-code IOP homebrew/IRX loaders, and against mirrored
	// RAM writes (a store to one 2MB mirror doesn't share a physical address
	// with a block compiled from another mirror of the same underlying byte,
	// but the memcmp safety net still catches that case on next execution).
	void Clear(u32 Addr, u32 Size)
	{
		// s_word_pc turns this into O(words written) instead of O(live blocks):
		// with Clear() called on every single IOP store (see the comment above),
		// scanning the whole block map here -- as an earlier version of this
		// function did -- reintroduced the same per-store cost problem the
		// narrow-invalidation rewrite was meant to fix, just shifted from "wipe
		// everything" to "walk everything," which is just as unaffordable at
		// this call frequency.
		if (s_word_pc.empty()) // Not yet Reset() -- nothing has been compiled.
			return;
		const u32 start_word = (Addr & 0x1fffffff) >> 2;
		const u32 end_word = std::min(start_word + std::max<u32>(Size, 1), CompileWindowWords);
		bool removed = false;
		for (u32 w = start_word; w < end_word; w++)
		{
			const u32 pc = s_word_pc[w];
			if (pc == EmptyWord)
				continue;
			const auto it = s_blocks.find(pc);
			if (it != s_blocks.end())
			{
				IndexBlockWords(it->second, pc, false);
				s_blocks.erase(it);
				removed = true;
			}
			else
				s_word_pc[w] = EmptyWord; // Stale entry with no backing block; drop it.
		}
		// Entries in s_blocks keep stable addresses across erase() of other
		// elements, but s_lookup caches raw pointers to erased entries too --
		// only safe to skip clearing it when nothing was actually removed.
		if (removed)
			s_lookup.fill({});
	}

	void Reserve()
	{
		// The code buffer itself comes from the shared SysMemory allocation
		// (SysMemory::GetIOPRec()); Reset() (called before first use, and again
		// on VM reset) is what actually sets up s_write.
	}
} // namespace

Arm64IOP::BlockResult Arm64IOP::TryExecute()
{
	return TryExecuteImpl();
}

void Arm64IOP::Reset()
{
	s_lookup.fill({});
	s_blocks.clear();
	s_word_pc.assign(CompileWindowWords, EmptyWord);
	s_write = SysMemory::GetIOPRec();
	s_write_limit = SysMemory::GetIOPRecEnd() - 16 * 1024;
}

void Arm64IOP::Shutdown()
{
	s_lookup.fill({});
	decltype(s_blocks){}.swap(s_blocks);
	decltype(s_word_pc){}.swap(s_word_pc);
	s_write = nullptr;
	s_write_limit = nullptr;
}

size_t Arm64IOP::GetCommittedCache()
{
	return s_write ? s_write - SysMemory::GetIOPRec() : 0;
}

R3000Acpu arm64IopCpu = {
	Reserve,
	Arm64IOP::Reset,
	ExecuteBlock,
	Clear,
	Arm64IOP::Shutdown,
};
