// SPDX-FileCopyrightText: 2026 PCSX2 Dev Team
// SPDX-License-Identifier: GPL-3.0+

#pragma once

#include "VUops.h"

#include <array>
#include <cstddef>

// Mirrors arm64/VU1Pipeline.h, reduced for VU0's simpler execution model: VU0
// has no XGKICK/GIF path, so nothing here drives the GIF or waits on a packet.
namespace Arm64VU0
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
		// x19..x29 and SP.
		std::array<const void*, 6> prepare{};
		// Entry for an integer-conditional branch pair, which has to wait for a
		// pending integer load feeding the register it tests. It reads that
		// register set from the Instruction's lregs.VIread rather than taking a
		// compile-time dependency slot, so it does not fit prepare[] above.
		const void* branch_prepare = nullptr;
		// Drains the pipe queues on its own, for an op whose body forces the cycle
		// forward mid-pair (DIV/SQRT/RSQRT waiting on a pending divide) and so has
		// to re-run the retirement its own prepare call already did. Skips
		// VIBackupCycles, which must not be decremented twice for one pair.
		const void* retire_queues = nullptr;
		size_t size = 0;
	};

	PipelineCode CompilePipeline(u8* code, size_t capacity);
} // namespace Arm64VU0
