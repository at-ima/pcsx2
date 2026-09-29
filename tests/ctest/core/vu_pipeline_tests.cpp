// SPDX-FileCopyrightText: 2002-2026 PCSX2 Dev Team
// SPDX-License-Identifier: GPL-3.0+

#include "VUops.h"

#include <gtest/gtest.h>

#include <cstring>

namespace
{
	class VUPipelineTest : public testing::TestWithParam<int>
	{
	protected:
		VURegs& Regs() { return vuRegs[GetParam()]; }

		void SetUp() override
		{
			m_saved = Regs();
			// XGKICK/game-level timing still needs proper testing; isolate pipeline retirement here.
			std::memset(&Regs(), 0, sizeof(VURegs));
			Regs().cycle = 100;
		}

		void TearDown() override { Regs() = m_saved; }

	private:
		VURegs m_saved;
	};
} // namespace

TEST_P(VUPipelineTest, EmptyPipelinesPreserveRegistersAndCycle)
{
	VURegs& vu = Regs();
	vu.VI[REG_STATUS_FLAG].UL = 0xabc;
	vu.VI[REG_MAC_FLAG].UL = 0x1234;
	vu.VI[REG_CLIP_FLAG].UL = 0x5678;
	vu.VI[REG_Q].UL = 0x3f800000;
	vu.VI[REG_P].UL = 0x40000000;
	const VURegs before = vu;

	_vuTestPipes(&vu);

	EXPECT_EQ(std::memcmp(&before, &vu, sizeof(vu)), 0);
}

TEST_P(VUPipelineTest, PendingResultsAreNotPublishedEarly)
{
	VURegs& vu = Regs();
	vu.fmaccount = vu.ialucount = 1;
	vu.fmac[0].sCycle = vu.ialu[0].sCycle = 97;
	vu.fmac[0].Cycle = vu.ialu[0].Cycle = 4;
	vu.fdiv.enable = vu.efu.enable = 1;
	vu.fdiv.sCycle = vu.efu.sCycle = 90;
	vu.fdiv.Cycle = vu.efu.Cycle = 11;
	vu.fdiv.reg.UL = 0x3f800000;
	vu.efu.reg.UL = 0x40000000;
	const VURegs before = vu;

	_vuTestPipes(&vu);

	EXPECT_EQ(std::memcmp(&before, &vu, sizeof(vu)), 0);
}

TEST_P(VUPipelineTest, ReadyResultsPreserveFlagWritebackOrder)
{
	VURegs& vu = Regs();
	vu.VI[REG_STATUS_FLAG].UL = 0x30;
	vu.fmaccount = 2;
	vu.fmac[0].sCycle = 95;
	vu.fmac[0].Cycle = 4;
	vu.fmac[0].statusflag = 3;
	vu.fmac[1].sCycle = 96;
	vu.fmac[1].Cycle = 4;
	vu.fmac[1].flagreg = (1 << REG_STATUS_FLAG) | (1 << REG_CLIP_FLAG);
	vu.fmac[1].statusflag = 0x489;
	vu.fmac[1].macflag = 0x1234;
	vu.fmac[1].clipflag = 0x5678;
	vu.fdiv.enable = vu.efu.enable = 1;
	vu.fdiv.sCycle = vu.efu.sCycle = 90;
	vu.fdiv.Cycle = vu.efu.Cycle = 10;
	vu.fdiv.statusflag = 0x820;
	vu.fdiv.reg.UL = 0x3f800000;
	vu.efu.reg.UL = 0x40000000;
	vu.ialucount = 1;
	vu.ialu[0].sCycle = 98;
	vu.ialu[0].Cycle = 2;

	_vuTestPipes(&vu);

	EXPECT_EQ(vu.cycle, 100u);
	EXPECT_EQ(vu.fmaccount, 0u);
	EXPECT_EQ(vu.fmacreadpos, 2u);
	EXPECT_EQ(vu.ialucount, 0u);
	EXPECT_EQ(vu.ialureadpos, 1u);
	EXPECT_EQ(vu.fdiv.enable, 0);
	EXPECT_EQ(vu.efu.enable, 0);
	EXPECT_EQ(vu.VI[REG_STATUS_FLAG].UL, 0xca9u);
	EXPECT_EQ(vu.VI[REG_MAC_FLAG].UL, 0x1234u);
	EXPECT_EQ(vu.VI[REG_CLIP_FLAG].UL, 0x5678u);
	EXPECT_EQ(vu.VI[REG_Q].UL, 0x3f800000u);
	EXPECT_EQ(vu.VI[REG_P].UL, 0x40000000u);
	const VURegs after = vu;
	_vuTestPipes(&vu);
	EXPECT_EQ(std::memcmp(&after, &vu, sizeof(vu)), 0);
}

TEST_P(VUPipelineTest, WrappedQueuesStopAtFirstPendingResult)
{
	VURegs& vu = Regs();
	vu.fmacreadpos = 3;
	vu.fmaccount = 3;
	vu.fmac[3].sCycle = 96;
	vu.fmac[3].Cycle = 4;
	vu.fmac[3].macflag = 0x1234;
	vu.fmac[0].sCycle = 97;
	vu.fmac[0].Cycle = 4;
	vu.fmac[1].macflag = 0xffff;
	vu.ialureadpos = 2;
	vu.ialucount = 3;
	vu.ialu[2].sCycle = 98;
	vu.ialu[2].Cycle = 2;
	vu.ialu[3].sCycle = 99;
	vu.ialu[3].Cycle = 2;

	_vuTestPipes(&vu);

	EXPECT_EQ(vu.cycle, 100u);
	EXPECT_EQ(vu.fmaccount, 2u);
	EXPECT_EQ(vu.fmacreadpos, 0u);
	EXPECT_EQ(vu.ialucount, 2u);
	EXPECT_EQ(vu.ialureadpos, 3u);
	EXPECT_EQ(vu.VI[REG_MAC_FLAG].UL, 0x1234u);

	vu.cycle = 101;
	_vuTestPipes(&vu);
	EXPECT_EQ(vu.cycle, 101u);
	EXPECT_EQ(vu.fmaccount, 0u);
	EXPECT_EQ(vu.fmacreadpos, 2u);
	EXPECT_EQ(vu.ialucount, 0u);
	EXPECT_EQ(vu.ialureadpos, 1u);
	EXPECT_EQ(vu.VI[REG_MAC_FLAG].UL, 0xffffu);
}

INSTANTIATE_TEST_SUITE_P(VU0AndVU1, VUPipelineTest, testing::Values(0, 1));
