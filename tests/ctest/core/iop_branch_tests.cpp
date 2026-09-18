// SPDX-FileCopyrightText: 2026 PCSX2 Dev Team
// SPDX-License-Identifier: GPL-3.0+

#include "Common.h"
#include "R3000A.h"
#include "IopHw.h"

#include <gtest/gtest.h>
#include <array>

namespace
{
	class IopBranchTest : public testing::Test
	{
	protected:
		static constexpr u32 Base = 0x10000;
		void SetUp() override
		{
			registers = psxRegs;
			mapping = psxMemRLUT;
			counter_start = psxNextStartCounter;
			counter_delta = psxNextDeltaCounter;
			action = iopEventAction;
			active = iopEventTestIsActive;
			delay = iopIsDelaySlot;
			for (size_t i = 0; i < addresses.size(); i++)
				hardware[i] = psxHu32(addresses[i]);
			pages[Base >> 16] = reinterpret_cast<uptr>(program.data());
			psxMemRLUT = pages.data();
		}
		void TearDown() override
		{
			psxRegs = registers;
			psxMemRLUT = mapping;
			psxNextStartCounter = counter_start;
			psxNextDeltaCounter = counter_delta;
			iopEventAction = action;
			iopEventTestIsActive = active;
			iopIsDelaySlot = delay;
			for (size_t i = 0; i < addresses.size(); i++)
				psxHu32(addresses[i]) = hardware[i];
		}
		void Init(u64 cycle, u64 deadline)
		{
			std::memset(&psxRegs, 0, sizeof(psxRegs));
			psxRegs.pc = Base;
			psxRegs.cycle = cycle;
			psxRegs.iopNextEventCycle = deadline;
			psxNextStartCounter = cycle;
			psxNextDeltaCounter = 100000;
			iopEventAction = iopEventTestIsActive = iopIsDelaySlot = false;
			for (u32 address : addresses)
				psxHu32(address) = 0;
			program[0] = (2u << 26) | (Base >> 2); // J to itself
			program[1] = 0; // NOP delay slot
		}
		psxRegisters registers{};
		const uptr* mapping = nullptr;
		u64 counter_start = 0;
		s32 counter_delta = 0;
		bool action = false, active = false, delay = false;
		static constexpr std::array<u32, 4> addresses{HW_ICFG, HW_ICTRL, HW_ISTAT, HW_IMASK};
		std::array<u32, 4> hardware{};
		std::array<uptr, 0x2000> pages{};
		std::array<u32, 2> program{};
	};
} // namespace

TEST_F(IopBranchTest, PollsDeadlineAfterDelaySlotAcrossCycleWrap)
{
	for (u64 cycle : {u64(10), u64(0xfffffffe), ~u64(0) - 1})
	{
		for (u32 distance : {0u, 1u, 2u, 3u, 100u})
		{
			SCOPED_TRACE(testing::Message() << cycle << ":" << distance);
			Init(cycle, cycle + distance);
			EXPECT_EQ(psxInt.ExecuteBlock(16), 0);
			EXPECT_EQ(psxRegs.cycle, cycle + 2);
			EXPECT_EQ(psxRegs.pc, Base);
			EXPECT_EQ(psxRegs.code, 0u);
			EXPECT_FALSE(iopIsDelaySlot);
			EXPECT_FALSE(iopEventAction);
			if (distance <= 2)
				EXPECT_EQ(psxRegs.iopNextEventCycle, cycle + 2 + 384);
			else
				EXPECT_EQ(psxRegs.iopNextEventCycle, cycle + distance);
		}
	}
}

TEST_F(IopBranchTest, AlreadyPendingInterruptIsNotDelayedUntilDeadline)
{
	for (u32 status : {0u, 1u, 0x400u, 0x401u, 0x801u, 0x400401u})
		for (u32 control : {0u, 1u})
			for (u32 mask : {0u, 1u, 2u})
			{
				Init(10, 1000);
				psxRegs.CP0.n.Status = status;
				psxHu32(HW_ICTRL) = control;
				psxHu32(HW_ISTAT) = 1;
				psxHu32(HW_IMASK) = mask;
				// Use the unmodified event handler as the interrupt oracle.
				iopEventTest();
				const u32 expected_pc = psxRegs.pc;
				const u32 expected_status = psxRegs.CP0.n.Status;
				const bool expected_action = iopEventAction;
				psxRegs.pc = Base;
				psxRegs.CP0.n.Status = status;
				psxRegs.iopNextEventCycle = 1000;
				iopEventAction = false;
				EXPECT_EQ(psxInt.ExecuteBlock(16), 0);
				EXPECT_EQ(psxRegs.pc, expected_pc);
				EXPECT_EQ(psxRegs.CP0.n.Status, expected_status);
				EXPECT_EQ(iopEventAction, expected_action);
				EXPECT_EQ(psxRegs.cycle, 12u);
			}
}

TEST_F(IopBranchTest, StatusEnabledInDelaySlotTakesPendingInterrupt)
{
	Init(10, 1000);
	psxHu32(HW_ICTRL) = psxHu32(HW_ISTAT) = psxHu32(HW_IMASK) = 1;
	psxRegs.GPR.r[1] = 0x401;
	program[1] = (16u << 26) | (4u << 21) | (1u << 16) | (12u << 11); // MTC0 r1, Status
	EXPECT_EQ(psxInt.ExecuteBlock(16), 0);
	EXPECT_EQ(psxRegs.pc, 0x80000080u);
	EXPECT_EQ(psxRegs.CP0.n.EPC, Base);
	EXPECT_TRUE(iopEventAction);
	EXPECT_EQ(psxRegs.cycle, 12u);
}

TEST_F(IopBranchTest, CounterDeadlineAndScheduledEventRemainPending)
{
	Init(10, 1000);
	psxNextDeltaCounter = 20;
	psxRegs.interrupt = 1u << IopEvt_Dma11;
	psxRegs.sCycle[IopEvt_Dma11] = 10;
	psxRegs.eCycle[IopEvt_Dma11] = 10;
	psxSetNextBranchDelta(10);
	EXPECT_EQ(psxInt.ExecuteBlock(16), 0);
	EXPECT_EQ(psxRegs.iopNextEventCycle, 20u);
	EXPECT_EQ(psxRegs.interrupt, 1u << IopEvt_Dma11);
	// A nearer counter deadline must still shorten the next branch test.
	psxSetNextBranch(psxNextStartCounter, 4);
	EXPECT_EQ(psxRegs.iopNextEventCycle, 14u);
	EXPECT_EQ(psxInt.ExecuteBlock(16), 0);
	EXPECT_EQ(psxRegs.cycle, 14u);
	EXPECT_EQ(psxRegs.iopNextEventCycle, 20u);
	EXPECT_EQ(psxRegs.interrupt, 1u << IopEvt_Dma11);
}

TEST_F(IopBranchTest, StatusRestoreAndDisableInDelaySlot)
{
	for (bool enable : {false, true})
	{
		Init(10, 1000);
		psxHu32(HW_ICTRL) = psxHu32(HW_ISTAT) = psxHu32(HW_IMASK) = 1;
		if (enable)
		{
			psxRegs.CP0.n.Status = 0x404;
			program[1] = 0x42000010; // RFE restores the interrupt-enable bit.
		}
		else
		{
			psxRegs.CP0.n.Status = 0x401;
			program[1] = (16u << 26) | (4u << 21) | (12u << 11); // MTC0 r0, Status
		}
		EXPECT_EQ(psxInt.ExecuteBlock(16), 0);
		EXPECT_EQ(psxRegs.pc, enable ? 0x80000080u : Base);
		EXPECT_EQ(iopEventAction, enable);
		EXPECT_EQ(psxRegs.cycle, 12u);
	}
}

TEST_F(IopBranchTest, Ps1ModeRetainsFractionalCycleAccounting)
{
	Init(10, 1000);
	psxHu32(HW_ICFG) = 8;
	psxRegs.iopCycleEECarry = 146;
	EXPECT_EQ(psxInt.ExecuteBlock(1), 1 - (2 * 1280 + 146) / 147);
	EXPECT_EQ(psxRegs.iopCycleEECarry, (2 * 1280 + 146) % 147);
	EXPECT_EQ(psxRegs.cycle, 12u);
	EXPECT_EQ(psxRegs.pc, Base);
	EXPECT_EQ(psxRegs.iopNextEventCycle, 1000u);
}
