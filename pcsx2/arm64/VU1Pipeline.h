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
		// What the preparation stubs publish for this pair, precomputed and kept
		// adjacent so one Ldp replaces three loads and the upper/lower select on
		// a path that runs once per executed pair. `tpc` is pc + 8; `code` is the
		// word the interpreter would have left in VURegs::code.
		u32 tpc;
		u32 code;
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
		// Same ABI; also waits for lregs.VIread in the incoming IALU queue.
		const void* branch_prepare = nullptr;
		// Drains FMAC/FDIV/EFU/IALU entries ready at the current VURegs::cycle,
		// then XGKICK (crediting or transferring, safe to run again for the
		// same pair), and returns — no +1/TPC/code publish, no incoming-hazard
		// scan, and deliberately no VIBackupCycles handling (it uses the
		// pre-pair cycle snapshot and must run at most once per pair; the
		// ordinary prepare stubs above already do so). For a pair whose own
		// body forces VURegs::cycle forward after that ordinary prepare/retire
		// already ran once for it (DIV/SQRT/RSQRT/WAITQ stalling on an
		// outstanding FDIV entry): entries that only become ready because of
		// that forcing are otherwise never drained, and a run of unrelated
		// pairs right after can be swept into a deferred region that skips
		// the runtime retire path entirely, leaving them stuck until the
		// block ends. Mirrors _vuTestPipes running again, after
		// _vuTestFDIVStalls, in the interpreter's own per-instruction order.
		const void* retire_queues = nullptr;
		// Same cache ABI; finish a delayed packet after the pair has committed.
		const void* finish_packet = nullptr;
		// Flush any old transfer before issuing another XGKICK.
		const void* flush_kick = nullptr;
		size_t size = 0;
	};

	using XgkickTransfer = void (*)(s32 cycles, bool flush);
	PipelineCode CompilePipeline(u8* code, size_t capacity, XgkickTransfer transfer = &_vuXGKICKTransfer, bool packet_mode = true);
} // namespace Arm64VU1
