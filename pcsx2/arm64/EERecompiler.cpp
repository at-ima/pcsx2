// SPDX-FileCopyrightText: 2026 PCSX2 Dev Team
// SPDX-License-Identifier: GPL-3.0+

#include "Common.h"
#include "arm64/EERecompiler.h"
#include "R5900OpcodeTables.h"
#include "vtlb.h"
#include "common/HostSys.h"
#include "arm64/EECodeGenerator.h"

#include <algorithm>
#include <array>
#include <deque>

namespace
{
	constexpr u32 MaxInstructions = Arm64EE::CodeGenerator::MaxInstructions;

	struct Block
	{
		using Function = u64 (*)(cpuRegisters*);
		std::array<u32, MaxInstructions> words{};
		u32 word_count = 0;
		std::array<u32, MaxInstructions + 1> cycles{};
		const u32* source = nullptr;
		Function function = nullptr;
		// Source bytes are guaranteed unchanged while this block exists: its
		// page was write-protected (or is not RAM) when it was compiled, and
		// every way out of that state -- a write fault, a TLB remap, a
		// tracking reset -- drops all blocks. Decided once in Compile() so
		// the dispatcher does not query page protection on every entry.
		bool trusted = false;
	};
	// std::deque never invalidates references to existing elements when more
	// are appended (unlike std::vector), so pointers handed out into this
	// stay valid for as long as the deque itself lives -- exactly the
	// property s_lookup/s_last_dispatch_block below rely on.
	std::deque<Block> s_block_storage;
	// Backing index for s_block_storage, keyed by pc: fixed-size, power-of-two,
	// linear-probed open addressing. libc++'s std::unordered_map (used here
	// until this commit) picks a prime bucket count, which needs a hardware
	// integer division per probe to index -- measured live as ~15-22% of
	// total EE-thread time in a busy scene. A power-of-two table needs only a
	// mask. Entries are never individually removed (only Reset()/
	// ClearProvider() ever clear everything at once). A slot's generation
	// tag tells whether it's live without needing to physically wipe the
	// whole table on every clear: MapTLB() calls ClearProvider() very
	// frequently during a normal (non-savestate) boot -- once per TLB remap,
	// many times per boot stage -- so an O(BlockTableSize) fill() on every
	// one of those, regardless of how few blocks were actually live, made
	// boot itself pay for clearing hundreds of thousands of always-empty
	// slots over and over (measured live: this alone made normal boot appear
	// to hang indefinitely, while resuming a savestate -- which barely
	// touches MapTLB -- was unaffected). Bumping a generation counter
	// instead is O(1): a slot is live only if its tag matches the current
	// generation, so an old generation's entries are implicitly all "empty"
	// without visiting them.
	struct BlockTableSlot
	{
		u32 pc = 0;
		u32 generation = 0;
		Block* block = nullptr;
	};
	constexpr u32 BlockTableSize = 1u << 19; // 524288: generous vs. any realistic live block-pc count
	std::array<BlockTableSlot, BlockTableSize> s_block_table{};
	u32 s_block_table_count = 0;
	u32 s_block_table_generation = 1; // 0 is never used, so a default-constructed slot starts "empty"
	u32 HashBlockPc(u32 pc) { return ((pc >> 2) * 0x9E3779B1u) >> (32 - 19); } // Fibonacci hashing, top 19 bits
	// Existing entry for pc, or nullptr. Linear-probes from the hashed slot;
	// bounded by BlockTableSize, though a real miss resolves in O(1) average
	// since ClearBlockTable() keeps the load factor low (see its call site).
	Block* FindBlock(u32 pc)
	{
		for (u32 index = HashBlockPc(pc);; index = (index + 1) & (BlockTableSize - 1))
		{
			const BlockTableSlot& slot = s_block_table[index];
			if (slot.generation != s_block_table_generation)
				return nullptr;
			if (slot.pc == pc)
				return slot.block;
		}
	}
	// Always allocates a fresh Block (even when pc already has one -- e.g. a
	// self-modifying-code recompile): callers immediately overwrite every
	// cached pointer to the old one, so nothing needs it to stay reachable,
	// and reusing the old slot in place would only complicate this for no
	// benefit. The orphaned old Block just lingers in s_block_storage until
	// the next wholesale clear, exactly like s_write's own cache-exhaustion
	// philosophy already documented at its declaration below.
	Block& InsertBlock(u32 pc)
	{
		for (u32 index = HashBlockPc(pc);; index = (index + 1) & (BlockTableSize - 1))
		{
			BlockTableSlot& slot = s_block_table[index];
			const bool empty = slot.generation != s_block_table_generation;
			if (empty || slot.pc == pc)
			{
				s_block_table_count += empty;
				s_block_storage.emplace_back();
				slot = {pc, s_block_table_generation, &s_block_storage.back()};
				return *slot.block;
			}
		}
	}
	void ClearBlockTable()
	{
		s_block_table_generation++;
		// Invalidates every native link made so far (see LinkState).
		Arm64EE::CodeGenerator::g_link_state.generation = s_block_table_generation;
		s_block_table_count = 0;
		std::deque<Block>{}.swap(s_block_storage);
	}
	struct LookupEntry
	{
		u32 pc = 0;
		u32 rejected_word = 0;
		const Block* block = nullptr;
		const u32* rejected_source = nullptr;
	};
	// Sized well past any game's live working set of unique block PCs so this
	// direct-mapped cache absorbs almost every lookup before it would ever
	// need to fall back to FindBlock() above.
	std::array<LookupEntry, 65536> s_lookup{};
	u8* s_write = nullptr;
	u8* s_write_limit = nullptr;
	bool s_goemon_tlb_hack = false;
	// Native links inline intUpdateCPUCycles() for the default cycle rate only.
	s8 s_cycle_rate = 0;
	bool Linkable() { return s_cycle_rate == 0; }
	// 1-entry "most recently dispatched" cache, checked before s_lookup. A
	// branch that loops back to its own containing block's entry pc (a very
	// common delay/poll-loop shape) makes intExecuteWithBackend call
	// TryExecute() again immediately for the same pc, back-to-back, with
	// nothing else running in between -- so this hits every single iteration
	// of such a loop, skipping s_lookup's indexing and (formerly) the
	// division-costing unordered_map fallback entirely (measured live: that
	// fallback can otherwise cost ~15% of total EE-thread time in a busy
	// scene with such a loop running millions of times/sec). This only
	// changes which path *finds* the block to run -- TryExecute() still runs
	// exactly one block per call, same as always, so callers that invoke it
	// directly (e.g. differential tests) see no change in behavior.
	u32 s_last_dispatch_pc = 0;
	const Block* s_last_dispatch_block = nullptr;
	u64 s_dispatches = 0;
	bool s_trust_untracked_for_testing = false;


	u32 GetSupportedInstructionCount(const u32* source, u32 remaining)
	{
		const u32 code = source[0];
		if (!Arm64EE::CodeGenerator::Supports(code))
			return 0;
		if (!Arm64EE::CodeGenerator::IsBranch(code))
			return 1;
		// Preserve the interpreter's TLB callbacks when the Goemon fix is enabled.
		if (EmuConfig.Gamefixes.GoemonTlbHack && ((code >> 26) == 3 || ((code >> 26) == 0 && (code & 63) == 8)))
			return 0;
		return remaining >= 2 && Arm64EE::CodeGenerator::SupportsDelaySlot(code, source[1]) ? 2 : 0;
	}

	// Allocation and compilation are cold. Keep their register/stack requirements
	// out of the dispatcher that runs for every cached block.
	__noinline Block& Compile(u32 pc, const u32* source, vtlb_ProtectionMode page_type, bool tracked)
	{
		// Keep the table's load factor low so a genuine miss (an unseen pc)
		// still resolves in ~O(1): matches s_write's own "just wipe everything
		// and start over" cache-exhaustion philosophy rather than growing.
		if (s_block_table_count * 4 >= BlockTableSize * 3)
			Arm64EE::Reset();
		Block& block = InsertBlock(pc);
		block.source = source;
		const u32 limit = std::min(MaxInstructions, (4096 - (pc & 4095)) / 4);
		while (block.word_count < limit)
		{
			const u32 count = GetSupportedInstructionCount(source + block.word_count, limit - block.word_count);
			if (!count)
				break;
			for (u32 i = 0; i < count; i++)
			{
				const u32 word = source[block.word_count];
				block.words[block.word_count++] = word;
				block.cycles[block.word_count] = block.cycles[block.word_count - 1] + R5900::GetInstruction(word).cycles;
			}
			if (count == 2)
				break;
		}
		if (!block.word_count)
		{
			// Cache rejected branch/delay pairs too. Revalidate both words so a
			// patched delay slot becomes eligible without recompiling every visit.
			block.word_count = std::min(2u, limit);
			std::copy_n(source, block.word_count, block.words.data());
			return block;
		}
		// None and Write both mean "not currently known to self-modify": write-protect
		// the page so TryExecute can trust the cache without a memcmp on every entry.
		// Manual pages already faulted at least once; leave them alone; re-protecting
		// on every recompile would just re-fault immediately (see the vtlb.cpp comment
		// on ProtMode_Manual). NotRequired (non-RAM, e.g. BIOS ROM) needs no protection:
		// mmap_MarkCountedRamPage() has no bounds check against non-RAM pointers.
		// Never mark when untracked: pc's physical mapping doesn't correspond to
		// source, so mmap_MarkCountedRamPage() would protect an unrelated page.
		if (tracked && (page_type == ProtMode_None || page_type == ProtMode_Write))
			mmap_MarkCountedRamPage(pc);
		block.trusted = tracked ? page_type != ProtMode_Manual : s_trust_untracked_for_testing;
		HostSys::BeginCodeWrite();
		const size_t size = Arm64EE::CodeGenerator::Compile(s_write, SysMemory::GetEERecEnd() - s_write,
			pc, source, std::span(block.words.data(), block.word_count),
			std::span(block.cycles.data(), block.word_count + 1), Linkable());
		HostSys::EndCodeWrite();
		HostSys::FlushInstructionCache(s_write, static_cast<u32>(size));
		block.function = reinterpret_cast<Block::Function>(s_write);
		s_write += (size + 15) & ~size_t(15);
		return block;
	}
} // namespace

__noinline void Arm64EE::Reset()
{
	s_lookup.fill({});
	ClearBlockTable();
	s_last_dispatch_pc = 0;
	s_last_dispatch_block = nullptr;
	s_write = SysMemory::GetEERec();
	// The reserved buffer is stable until Shutdown; keep its compilation margin
	// out of the per-block memory-manager call path.
	s_write_limit = SysMemory::GetEERecEnd() - 16 * 1024;
	s_goemon_tlb_hack = EmuConfig.Gamefixes.GoemonTlbHack;
	s_cycle_rate = EmuConfig.Speedhacks.EECycleRate;
}

void Arm64EE::Shutdown()
{
	s_lookup.fill({});
	ClearBlockTable();
	s_last_dispatch_pc = 0;
	s_last_dispatch_block = nullptr;
	s_write = nullptr;
	s_write_limit = nullptr;
}

namespace
{
	// Finds (compiling if needed) and validates the block at cpuRegs.pc, or
	// returns nullptr when the interpreter has to run the next instruction.
	const Block* LookupBlock()
	{
		using namespace vtlb_private;
		const u32 pc = cpuRegs.pc;
		if (!CHECK_EEREC || cpuRegs.branch || (pc & 3))
			return nullptr;
		const auto mapping = vtlbdata.vmap[pc >> VTLB_PAGE_BITS];
		// Never prefetch through MMIO or unmapped memory: the interpreter must
		// perform that read with its original exception PC and handler semantics.
		if (mapping.isHandler(pc))
			return nullptr;
		const u32* source = reinterpret_cast<const u32*>(mapping.assumePtr(pc));
		if (!s_write || s_goemon_tlb_hack != EmuConfig.Gamefixes.GoemonTlbHack || s_cycle_rate != EmuConfig.Speedhacks.EECycleRate ||
			s_write > s_write_limit)
			Arm64EE::Reset();
		// Checked before s_lookup: see the comment on its declaration. Reset() /
		// ClearProvider() invalidate it alongside s_lookup and the block table.
		// s_lookup is only touched on a miss here; it is large enough that reading
		// it unconditionally cost a cache miss on most dispatches.
		const Block* block = s_last_dispatch_pc == pc ? s_last_dispatch_block : nullptr;
		LookupEntry* lookup = nullptr;
		// Mix page and instruction bits to avoid concentrating same-offset blocks
		// in one slot. The full PC tag keeps virtual aliases distinct.
		const auto get_lookup = [pc]() { return &s_lookup[((pc >> 2) ^ (pc >> 12)) & (s_lookup.size() - 1)]; };
		if (!block)
		{
			lookup = get_lookup();
			block = lookup->pc == pc ? lookup->block : nullptr;
			if (!block)
			{
				// Only opcode-level rejection is cached here. Branch/delay rejection
				// still uses a Block and validates both instruction words below.
				if (lookup->pc == pc && lookup->rejected_source == source && lookup->rejected_word == source[0])
					return nullptr;
				block = FindBlock(pc);
				*lookup = {pc, 0, block, nullptr};
			}
		}
		// Trusted blocks need no per-entry recheck (see Block::trusted). Others --
		// compiled on a page that already faulted (ProtMode_Manual), or whose
		// physical mapping does not match source -- compare every entry.
		if (!block || block->source != source ||
			(!block->trusted && std::memcmp(source, block->words.data(), block->word_count * 4) != 0))
		{
			if (!lookup)
				lookup = get_lookup();
			// Validated cache hits need no opcode decoding. Avoid allocating entries
			// for unsupported entry instructions on the interpreter fallback path.
			if (!Arm64EE::CodeGenerator::Supports(source[0]))
			{
				*lookup = {pc, source[0], nullptr, source};
				return nullptr;
			}
			// mmap_GetRamPageInfo()/mmap_MarkCountedRamPage() key off pc through the
			// separate *physical* (pmap) mapping, not the vmap lookup that produced
			// source above; they normally agree for real PS2 RAM, but nothing
			// guarantees it (e.g. a vmap override onto host memory pmap knows nothing
			// about -- exactly what the recompiler unit tests do to inject synthetic
			// code buffers). Only use write-protection tracking when the two
			// mappings actually resolve to the same byte.
			const bool tracked = source == reinterpret_cast<const u32*>(PSM(pc));
			const vtlb_ProtectionMode page_type = tracked ? mmap_GetRamPageInfo(pc) : ProtMode_None;
			block = &Compile(pc, source, page_type, tracked);
			*lookup = {pc, 0, block, nullptr};
		}
		if (!block->function)
			return nullptr;
		s_last_dispatch_pc = pc;
		s_last_dispatch_block = block;
		return block;
	}

	// Runs one block (or, while chaining, a run of linked ones) and adds the
	// cycles the generated code did not already add.
	u64 RunBlock(const Block* block, u32& block_cycles)
	{
		const u64 result = block->function(&cpuRegs);
		if (!(result & Arm64EE::CodeGenerator::CyclesCommitted))
		{
			const u32 completed = static_cast<u32>(result) & Arm64EE::CodeGenerator::CompletedMask;
			block_cycles += block->cycles[completed] * (2 - ((cpuRegs.CP0.n.Config >> 18) & 1));
		}
		return result;
	}

	EEBlockResult DecodeResult(u64 result)
	{
		using namespace Arm64EE::CodeGenerator;
		return {static_cast<EEBlockExit>((result >> ExitShift) & ExitMask), static_cast<u32>(result >> 32)};
	}
} // namespace

EEBlockResult Arm64EE::TryExecute(u32& block_cycles)
{
	CodeGenerator::g_link_state.chaining = 0;
	const Block* block = LookupBlock();
	return block ? DecodeResult(RunBlock(block, block_cycles)) : EEBlockResult{};
}

EEBlockResult Arm64EE::ExecuteChained(u32& block_cycles)
{
	// Keeps running blocks here instead of returning to intExecuteWithBackend
	// after each one, and lets linkable blocks jump straight to each other
	// (see CodeGenerator::LinkState). In Burnout 3 most blocks are about three
	// instructions ending in a branch, and a C++ round trip per block took
	// about a third of the EE thread.
	// A Continue exit needs nothing from the driver, and an EventTest exit only
	// the event-deadline check. A taken branch needs intFinishBranch's pc
	// commit, the cycle commit and the event-deadline check, done below (or by
	// the generated code) exactly as the driver does them. Anything else goes
	// back to the driver. Native links only return at an event deadline, so a
	// pending exit request waits for the next event, as with the x86 recompiler.
	// A JR/JALR does the same bookkeeping in generated code and enters its target
	// through CodeGenerator::g_indirect, which this loop fills with every trusted
	// block it dispatches; on a miss it returns NextBlock.
	// needs proper testing across a wider range of games.
	using namespace CodeGenerator;
	g_link_state.chaining = 1;
	g_link_state.block_cycles = &block_cycles;
	constexpr u32 kMaxChainedBlocks = 4096;
	u8* pending_slot = nullptr;
	u32 pending_generation = 0;
	for (u32 blocks = 1;; blocks++)
	{
		s_dispatches++;
		const u32 pc = cpuRegs.pc;
		const Block* block = LookupBlock();
		if (!block)
			return {};
		if (block->trusted)
			g_indirect[IndirectIndex(pc)] = {pc, g_link_state.generation, reinterpret_cast<const void*>(block->function)};
		// Link the exit that asked for it, unless blocks were dropped since (a
		// write fault in the block, or a Reset() while looking this one up,
		// which also rewinds the code buffer the slot lives in).
		if (pending_slot && pending_generation == g_link_state.generation && block->trusted)
			PatchLink(pending_slot, reinterpret_cast<const void*>(block->function), pending_generation);
		pending_slot = nullptr;
		const u32 generation = g_link_state.generation;
		const u64 raw = RunBlock(block, block_cycles);
		if (raw & LinkRequest)
		{
			pending_slot = SysMemory::GetEERec() + (raw >> 32);
			pending_generation = generation;
			if (blocks >= kMaxChainedBlocks)
				return {EEBlockExit::Continue, 0};
			continue;
		}
		if (raw & NextBlock)
		{
			if (blocks >= kMaxChainedBlocks)
				return {EEBlockExit::Continue, 0};
			continue;
		}
		if (raw & EventDue)
		{
			// pc/cycles are already committed, so returning TakenBranch would make
			// the driver commit them a second time.
			intEventTest(); // may fastjmp out of this function entirely
			return {EEBlockExit::Continue, 0};
		}
		const EEBlockResult result = DecodeResult(raw);
		if (blocks >= kMaxChainedBlocks)
			return result;
		if (result.exit == EEBlockExit::Continue)
			continue;
		if (result.exit == EEBlockExit::EventTest)
		{
			// The driver's intBranchEventTest(): no cycle commit, only the deadline.
			if (EEBranchEventDue(/*backend_active=*/true, /*exit_requested=*/false, cpuRegs.cycle, cpuRegs.nextEventCycle))
			{
				intEventTest(); // may fastjmp out of this function entirely
				return {EEBlockExit::Continue, 0};
			}
			continue;
		}
		if (result.exit != EEBlockExit::TakenBranch)
			return result;
		// Replicates intFinishBranch()'s effect on this (non-interpreter-
		// execution) path -- its WaitLoop speedhack branch is gated on
		// Cpu->usesInterpreterExecution, which is false here, so nothing else
		// from intFinishBranch applies. branch2 (Interpreter.cpp) is written
		// but never read there, so it needs no equivalent here.
		cpuRegs.branch = 1;
		cpuRegs.pc = result.target;
		cpuRegs.branch = 0;
		if (Linkable())
		{
			// intUpdateCPUCycles() at the default cycle rate, applied to the
			// counter this call was given -- the same one in production -- so it
			// matches what linked exits do.
			cpuRegs.cycle += std::max(block_cycles >> 3, 1u);
			block_cycles &= 7;
		}
		else
			intUpdateCPUCycles();
		if (EEBranchEventDue(/*backend_active=*/true, /*exit_requested=*/false, cpuRegs.cycle, cpuRegs.nextEventCycle))
		{
			intEventTest(); // may fastjmp out of this function entirely
			return {EEBlockExit::Continue, 0};
		}
	}
}

size_t Arm64EE::GetCommittedCache()
{
	return s_write ? s_write - SysMemory::GetEERec() : 0;
}

u64 Arm64EE::GetDispatchCount()
{
	return s_dispatches;
}

void Arm64EE::SetTrustUntrackedForTesting(bool trust)
{
	s_trust_untracked_for_testing = trust;
	Reset();
}

namespace
{
	void ResetProvider()
	{
		intCpu.Reset();
		Arm64EE::Reset();
	}
	void ExecuteProvider()
	{
#if defined(PCSX2_DEVBUILD)
		// Keep the interpreter's instruction-level debugger checks in development builds.
		intCpu.Execute();
#else
		intExecuteWithBackend(&Arm64EE::ExecuteChained);
#endif
	}
	void ClearProvider(u32, u32)
	{
		// Called for a write to a write-protected page (via vtlb.cpp's page fault
		// handler, now that Compile() calls mmap_MarkCountedRamPage) or a TLB
		// remap. Both are rare next to block execution, so unconditionally
		// dropping every cached block is simpler -- and safer -- than working out
		// which ones the given range actually overlaps. s_write is left alone;
		// its space is reclaimed the same way an ordinary memcmp-miss recompile
		// already leaks it, on the next Reset().
		s_lookup.fill({});
		ClearBlockTable();
		s_last_dispatch_pc = 0;
		s_last_dispatch_block = nullptr;
	}
} // namespace

R5900cpu arm64Cpu = {
	Arm64EE::Reset,
	Arm64EE::Shutdown,
	ResetProvider,
	[]() { intCpu.Step(); },
	ExecuteProvider,
	[]() { intCpu.ExitExecution(); },
	[]() { intCpu.CancelInstruction(); },
	ClearProvider,
	true,
};
