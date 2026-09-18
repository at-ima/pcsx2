// SPDX-FileCopyrightText: 2026 PCSX2 Dev Team
// SPDX-License-Identifier: GPL-3.0+

#pragma once

#include "R3000A.h"

// PC/cycle updates and debugger hooks belong to the execution driver. Dispatch
// directly to the existing leaf handler, avoiding a second indirect branch for
// grouped opcodes. Keep the tables as the single source of opcode semantics.
static __fi void psxExecuteOpcode()
{
	const u32 code = psxRegs.code;
	if (code == 0) // SLL r0, r0, 0 has no handler side effects.
		return;

	switch (code >> 26)
	{
		case 0:
			psxSPC[code & 63]();
			break;
		case 1:
			psxREG[(code >> 16) & 31]();
			break;
		case 16:
			psxCP0[(code >> 21) & 31]();
			break;
		case 18:
			if ((code & 63) == 0)
				psxCP2BSC[(code >> 21) & 31]();
			else
				psxCP2[code & 63]();
			break;
		default:
			psxBSC[code >> 26]();
			break;
	}
}
