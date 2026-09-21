// SPDX-FileCopyrightText: 2026 PCSX2 Dev Team
// SPDX-License-Identifier: GPL-3.0+

#pragma once

#include "Interpreter.h"
#include <span>

namespace Arm64EE::CodeGenerator
{
	constexpr u32 MaxInstructions = 32;
	bool Supports(u32 code);
	constexpr bool IsBranch(u32 code)
	{
		const u32 op = code >> 26;
		if (op == 0)
			return (code & 63) == 8 || (code & 63) == 9;
		if (op == 1)
		{
			switch ((code >> 16) & 31)
			{
				case 0:
				case 1:
				case 2:
				case 3:
				case 16:
				case 17:
				case 18:
				case 19:
					return true;
				default:
					return false;
			}
		}
		// COP1's BC1F/BC1T/BC1FL/BC1TL live under rs == 8; the other COP1 rs
		// values (register transfer, S/W-format arithmetic) are not branches.
		if (op == 17)
			return ((code >> 21) & 31) == 8;
		return (op >= 2 && op <= 7) || (op >= 20 && op <= 23);
	}
	bool SupportsDelaySlot(u32 code);
	// Packed native return value: completed prefix in bits 0-7, exit action in
	// bits 8-9 and the taken target in bits 32-63. No events run in generated code.
	constexpr u32 CompletedMask = 0xff;
	constexpr u32 ExitShift = 8;
	constexpr u32 ExitMask = 3;
	constexpr u32 EncodeExit(EEBlockExit exit) { return static_cast<u32>(exit) << ExitShift; }
	// Generated functions return the completed prefix and exit action. On an
	// unsupported memory access they leave PC at that instruction for fallback.
	// Stores overlapping this block's source exit immediately for revalidation.
	size_t Compile(u8* buffer, size_t capacity, u32 pc, const u32* source, std::span<const u32> words);
} // namespace Arm64EE::CodeGenerator
