// SPDX-FileCopyrightText: 2026 PCSX2 Dev Team
// SPDX-License-Identifier: GPL-3.0+

#pragma once

#include "common/Pcsx2Types.h"

enum class EEBlockExit
{
	NotHandled,
	Continue,
	TakenBranch,
	EventTest,
};

struct EEBlockResult
{
	EEBlockExit exit = EEBlockExit::NotHandled;
	u32 target = 0;
	explicit operator bool() const { return exit != EEBlockExit::NotHandled; }
};

// Shared EE execution driver: boot hooks, events, exception recovery and fallback.
// The backend accumulates cycles in the interpreter's 3-bit fixed-point units.
// TakenBranch completes a nontrapping delay slot and leaves PC just past it;
// the driver commits the target and cycles, then polls the event deadline.
// EventTest polls the deadline without committing accumulated cycles.
// Forced CP0/MMIO event tests are independent of this branch-polling policy.
// The backend is never called during boot, single stepping or in a delay slot.
using EEBlockExecutor = EEBlockResult (*)(u32& block_cycles);
void intExecuteWithBackend(EEBlockExecutor execute_block);

// Use the same signed 64-bit cycle difference as the x86 dispatcher. Preserve
// immediate interpreter polling and requested execution exits before the deadline.
constexpr bool EEBranchEventDue(bool backend_active, bool exit_requested, u64 cycle, u64 next_event_cycle)
{
	return !backend_active || exit_requested || static_cast<s64>(cycle - next_event_cycle) >= 0;
}
