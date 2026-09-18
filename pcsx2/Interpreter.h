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
// the driver commits the target and performs the original branch/event timing.
// EventTest requests an event test without committing accumulated cycles.
// The backend is never called during boot, single stepping or in a delay slot.
using EEBlockExecutor = EEBlockResult (*)(u32& block_cycles);
void intExecuteWithBackend(EEBlockExecutor execute_block);
