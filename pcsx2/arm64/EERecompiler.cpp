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
#include <unordered_map>

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
	};
	std::unordered_map<u32, Block> s_blocks;
	u8* s_write = nullptr;
	u8* s_write_limit = nullptr;
	bool s_goemon_tlb_hack = false;


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
		return remaining >= 2 && Arm64EE::CodeGenerator::SupportsDelaySlot(source[1]) ? 2 : 0;
	}

	// Allocation and compilation are cold. Keep their register/stack requirements
	// out of the dispatcher that runs for every cached block.
	__noinline Block& Compile(u32 pc, const u32* source)
	{
		Block& block = s_blocks[pc];
		block = {};
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
		HostSys::BeginCodeWrite();
		const size_t size = Arm64EE::CodeGenerator::Compile(s_write, SysMemory::GetEERecEnd() - s_write,
			pc, source, std::span(block.words.data(), block.word_count));
		HostSys::EndCodeWrite();
		HostSys::FlushInstructionCache(s_write, static_cast<u32>(size));
		block.function = reinterpret_cast<Block::Function>(s_write);
		s_write += (size + 15) & ~size_t(15);
		return block;
	}
} // namespace

__noinline void Arm64EE::Reset()
{
	s_blocks.clear();
	s_write = SysMemory::GetEERec();
	// The reserved buffer is stable until Shutdown; keep its compilation margin
	// out of the per-block memory-manager call path.
	s_write_limit = SysMemory::GetEERecEnd() - 16 * 1024;
	s_goemon_tlb_hack = EmuConfig.Gamefixes.GoemonTlbHack;
}

void Arm64EE::Shutdown()
{
	decltype(s_blocks){}.swap(s_blocks);
	s_write = nullptr;
	s_write_limit = nullptr;
}

EEBlockResult Arm64EE::TryExecute(u32& block_cycles)
{
	using namespace vtlb_private;
	const u32 pc = cpuRegs.pc;
	if (!CHECK_EEREC || cpuRegs.branch || (pc & 3))
		return {};
	const auto mapping = vtlbdata.vmap[pc >> VTLB_PAGE_BITS];
	// Never prefetch through MMIO or unmapped memory: the interpreter must
	// perform that read with its original exception PC and handler semantics.
	if (mapping.isHandler(pc))
		return {};
	const u32* source = reinterpret_cast<const u32*>(mapping.assumePtr(pc));
	if (!s_write || s_goemon_tlb_hack != EmuConfig.Gamefixes.GoemonTlbHack || s_write > s_write_limit)
		Reset();
	const auto it = s_blocks.find(pc);
	const Block* block = it != s_blocks.end() ? &it->second : nullptr;
	// Recheck the virtual mapping and all source words on every entry. This also
	// covers self-modifying code, DMA, TLB changes and state loads without Clear().
	if (!block || block->source != source || std::memcmp(source, block->words.data(), block->word_count * 4) != 0)
	{
		// Validated cache hits need no opcode decoding. Avoid allocating entries
		// for unsupported entry instructions on the interpreter fallback path.
		if (!CodeGenerator::Supports(source[0]))
			return {};
		block = &Compile(pc, source);
	}
	if (!block->function)
		return {};
	const u64 result = block->function(&cpuRegs);
	const u32 completed = static_cast<u32>(result) & CodeGenerator::CompletedMask;
	block_cycles += block->cycles[completed] * (2 - ((cpuRegs.CP0.n.Config >> 18) & 1));
	return {static_cast<EEBlockExit>((result >> CodeGenerator::ExitShift) & CodeGenerator::ExitMask),
		static_cast<u32>(result >> 32)};
}

size_t Arm64EE::GetCommittedCache()
{
	return s_write ? s_write - SysMemory::GetEERec() : 0;
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
		intExecuteWithBackend(&Arm64EE::TryExecute);
#endif
	}
	void ClearProvider(u32, u32)
	{
		// Blocks validate source bytes and the virtual mapping at every entry.
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
