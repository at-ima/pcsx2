// SPDX-FileCopyrightText: 2026 PCSX2 Dev Team
// SPDX-License-Identifier: GPL-3.0+

#pragma once

#include "VUops.h"

#include <array>
#include <cstddef>

namespace Arm64VU1
{
	struct Instruction
	{
		u32 pc;
		u32 lower;
		u32 upper;
		_VURegsNum uregs{};
		_VURegsNum lregs{};
		std::array<u8, 32> readMasks{};
		bool readsVF = false;
		// -2: no VF reads; -1: inspect incoming pipeline; 0..3: FMAC slots back.
		int8_t dependency = -1;
	};

	struct PipelineCode
	{
		// Private JIT ABI: x0 = Instruction*, x19 = VURegs*. Entries correspond
		// to read-free, incoming, and scheduled dependencies 0..3. Preserve
		// x19..x29 and SP. x24 points to eight VF/ACC cache offsets (unused
		// slots are ~0u); q8..q15 hold those cached values and survive preparation.
		std::array<const void*, 6> prepare{};
		size_t size = 0;
	};

	using XgkickTransfer = void (*)(s32 cycles, bool flush);
	PipelineCode CompilePipeline(u8* code, size_t capacity, XgkickTransfer transfer = &_vuXGKICKTransfer);
} // namespace Arm64VU1
