// SPDX-FileCopyrightText: 2026 PCSX2 Dev Team
// SPDX-License-Identifier: GPL-3.0+

#pragma once

#include "R3000A.h"

extern R3000Acpu arm64IopCpu;

namespace Arm64IOP
{
	enum class BlockExit
	{
		NotHandled,
		Continue,
		TakenBranch,
	};

	struct BlockResult
	{
		BlockExit exit = BlockExit::NotHandled;
		u32 completed = 0;
		explicit operator bool() const { return exit != BlockExit::NotHandled; }
	};

	// Block compiler entry point, also usable directly by differential tests
	// (see EERecompiler.h's TryExecute for the analogous EE entry point).
	BlockResult TryExecute();
	void Reset();
	void Shutdown();
	size_t GetCommittedCache();
} // namespace Arm64IOP
