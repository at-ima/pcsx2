// SPDX-FileCopyrightText: 2002-2026 PCSX2 Dev Team
// SPDX-License-Identifier: GPL-3.0

#include "common/Console.h"
#include "MTVU.h"
#include "SaveState.h"
#include "vtlb.h"

#include "common/Assertions.h"

bool SaveStateBase::vuJITFreeze()
{
	if(IsSaving())
		vu1Thread.WaitVU();

	Console.Warning("recompiler state is stubbed in arm64!");

	// HACK!!

	// size of microRegInfo structure
	std::array<u8,96> empty_data{};
	Freeze(empty_data);
	Freeze(empty_data);
	return true;
}
