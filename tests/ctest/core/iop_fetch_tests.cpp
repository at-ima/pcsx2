// SPDX-FileCopyrightText: 2026 PCSX2 Dev Team
// SPDX-License-Identifier: GPL-3.0+

#include "Common.h"
#include "IopMem.h"

#include <gtest/gtest.h>
#include <array>

namespace
{
	class IopFetchTest : public testing::Test
	{
	protected:
		void SetUp() override
		{
			previous = psxMemRLUT;
			psxMemRLUT = pages.data();
			for (u32 i = 0; i < 0x80; i++)
				pages[i] = reinterpret_cast<uptr>(memory[i % 2].data());
			for (u32 i = 0; i < 0x4000; i++)
			{
				memory[0][i] = 0x24020000 | i;
				memory[1][i] = 0x34030000 | i;
			}
		}
		void TearDown() override { psxMemRLUT = previous; }
		const uptr* previous = nullptr;
		std::array<uptr, 0x2000> pages{};
		std::array<std::array<u32, 0x4000>, 2> memory{};
	};
} // namespace

TEST_F(IopFetchTest, RamMirrorsAliasesAndPageBoundaries)
{
	for (u32 alias : {0u, 0x80000000u, 0xa0000000u})
	{
		for (u32 page = 0; page < 0x80; page++)
		{
			for (u32 offset : {0u, 4u, 0x7ffcu, 0xfff8u, 0xfffcu})
			{
				const u32 address = alias | (page << 16) | offset;
				EXPECT_EQ(iopMemFetch32(address), iopMemRead32(address));
			}
		}
	}
}

TEST_F(IopFetchTest, FetchObservesCodeWritesAndMappingChanges)
{
	EXPECT_EQ(iopMemFetch32(0x80010004), memory[1][1]);
	memory[1][1] = 0x03e00008;
	EXPECT_EQ(iopMemFetch32(0xa0010004), 0x03e00008u);
	pages[1] = pages[0];
	EXPECT_EQ(iopMemFetch32(0x80010004), memory[0][1]);
	pages[1] = 0;
	EXPECT_EQ(iopMemFetch32(0x80010004), 0u);
}

TEST_F(IopFetchTest, NonRamFetchUsesExistingReadSemantics)
{
	// ROM and expansion mappings, unmapped addresses, and the RAM window end.
	for (u32 address : {0x00800000u, 0x1f000000u, 0x1fc00000u, 0xbfc00000u, 0x1e000000u})
	{
		EXPECT_EQ(iopMemFetch32(address), iopMemRead32(address));
		pages[(address & 0x1fffffff) >> 16] = pages[0];
		EXPECT_EQ(iopMemFetch32(address), memory[0][0]);
	}

	// This page has a nonzero RLUT entry but reads must use the hardware path.
	const u32 saved = psxHu32(0x1f800100);
	psxHu32(0x1f800100) = 0xabcdef01;
	pages[0x1f80] = pages[0];
	EXPECT_EQ(iopMemFetch32(0xbf800100), 0xabcdef01u);
	psxHu32(0x1f800100) = saved;
}
