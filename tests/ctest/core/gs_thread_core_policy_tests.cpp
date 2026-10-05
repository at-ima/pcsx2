// SPDX-FileCopyrightText: 2026 PCSX2 Dev Team
// SPDX-License-Identifier: GPL-3.0+

#include "GS/GSThreadCorePolicy.h"
#include <gtest/gtest.h>

namespace
{
	// Runs windows at `load` until the policy moves to the efficiency cores.
	unsigned UntilEfficiency(GSThreadCorePolicy& policy, double load)
	{
		for (unsigned i = 1; i < 1000; i++)
			if (policy.Update(load, false))
				return i;
		return 0;
	}
} // namespace

TEST(GSThreadCorePolicyTest, MovesToEfficiencyCoresOnlyAfterCalmWindows)
{
	GSThreadCorePolicy policy;
	// 0.1 times the assumed slowdown of 2.5 is well under DemoteLoad.
	EXPECT_EQ(UntilEfficiency(policy, 0.1), GSThreadCorePolicy::CalmWindows);
	// 0.4 * 2.5 is not.
	GSThreadCorePolicy busy;
	for (unsigned i = 0; i < 100; i++)
		EXPECT_FALSE(busy.Update(0.4, false));
}

TEST(GSThreadCorePolicyTest, OverloadMovesBackAndHoldsLongerEachTime)
{
	GSThreadCorePolicy policy;
	UntilEfficiency(policy, 0.2);
	ASSERT_TRUE(policy.Efficiency());
	// Measured three times slower there: 0.6 against 0.2.
	EXPECT_TRUE(policy.Update(0.6, false));
	EXPECT_NEAR(policy.Slowdown(), 2.5 + (3.0 - 2.5) * 0.3, 1e-9);
	EXPECT_TRUE(policy.Update(GSThreadCorePolicy::PromoteLoad, false));
	EXPECT_FALSE(policy.Update(0.95, false));
	// Held for HoldWindows however idle it gets, then calm windows.
	EXPECT_EQ(UntilEfficiency(policy, 0.01), GSThreadCorePolicy::HoldWindows + GSThreadCorePolicy::CalmWindows);
	// Falling behind again soon doubles the hold.
	EXPECT_FALSE(policy.Update(0.95, false));
	EXPECT_EQ(policy.HoldTime(), 2 * GSThreadCorePolicy::HoldWindows);
	EXPECT_EQ(UntilEfficiency(policy, 0.01), 2 * GSThreadCorePolicy::HoldWindows + GSThreadCorePolicy::CalmWindows);
	// A settled stint resets it.
	for (unsigned i = 0; i < GSThreadCorePolicy::SettledWindows; i++)
		EXPECT_TRUE(policy.Update(0.3, false));
	EXPECT_FALSE(policy.Update(GSThreadCorePolicy::PromoteLoad + 0.01, false));
	EXPECT_EQ(policy.HoldTime(), GSThreadCorePolicy::HoldWindows);
}

TEST(GSThreadCorePolicyTest, BelowFullSpeedKeepsPerformanceCores)
{
	GSThreadCorePolicy policy;
	// Idle but slow (GPU-bound, say): never moves.
	for (unsigned i = 0; i < 100; i++)
		EXPECT_FALSE(policy.Update(0.05, true));
	// On the efficiency cores, a slow window moves it back.
	EXPECT_EQ(UntilEfficiency(policy, 0.05), GSThreadCorePolicy::CalmWindows);
	EXPECT_FALSE(policy.Update(0.2, true));
}

TEST(GSThreadCorePolicyTest, AverageLoadDecidesNotOneQuietWindow)
{
	GSThreadCorePolicy policy;
	// Steady 0.4 (times 2.5 is over DemoteLoad) with one quiet window: the
	// average barely moves, so it stays.
	for (unsigned i = 0; i < 10; i++)
		EXPECT_FALSE(policy.Update(0.4, false));
	EXPECT_FALSE(policy.Update(0.05, false));
	for (unsigned i = 0; i < 10; i++)
		EXPECT_FALSE(policy.Update(0.4, false));
}
