// SPDX-FileCopyrightText: 2026 PCSX2 Dev Team
// SPDX-License-Identifier: GPL-3.0+

#pragma once

#include "Interpreter.h"

struct R5900cpu;
extern R5900cpu arm64Cpu;

namespace Arm64EE
{
	// Block compiler entry point, also used by differential tests. The CPU provider
	// owns selection/lifetime; the shared execution driver owns fallback and events.
	// Runs exactly one block.
	EEBlockResult TryExecute(u32& block_cycles);
	// What the provider registers with the driver: runs blocks back to back,
	// doing the driver's branch and event bookkeeping itself.
	EEBlockResult ExecuteChained(u32& block_cycles);
	void Reset();
	void Shutdown();
	size_t GetCommittedCache();
} // namespace Arm64EE
