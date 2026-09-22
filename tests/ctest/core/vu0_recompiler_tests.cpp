// SPDX-FileCopyrightText: 2026 PCSX2 Dev Team
// SPDX-License-Identifier: GPL-3.0+

#include "Common.h"

#if defined(ARCH_ARM64)
#include "arm64/VU0Recompiler.h"
#include "common/HostSys.h"
#include <gtest/gtest.h>
#include <array>
#include <cstring>

namespace
{
	// An upper op that decodes to Op::None, paired with a MOVE whose masks are
	// empty, gives a pair that is compiled but architecturally inert.
	constexpr u32 kNopUpper = 0x000002ff;
	constexpr u32 kNopLower = 0x8000033c;

	constexpr u32 MakeUpper(u32 op, u32 dest, u32 fd, u32 fs, u32 ft)
	{
		return (dest << 21) | (ft << 16) | (fs << 11) | (fd << 6) | op;
	}

	// DIV Q, VFs[fsf], VFt[ftf]. VU0Recompiler.cpp never compiles this, so it
	// always runs through the interpreter and leaves its result in the FDIV pipe.
	constexpr u32 MakeDiv(u32 fs, u32 fsf, u32 ft, u32 ftf)
	{
		return (0x40u << 25) | (ftf << 23) | (fsf << 21) | (ft << 16) | (fs << 11) | 0x3bc;
	}

	class VU0RecompilerTest : public testing::Test
	{
	protected:
		static void SetUpTestSuite() { ASSERT_TRUE(SysMemory::Allocate()); }
		static void TearDownTestSuite()
		{
			CpuArm64VU0.Shutdown();
			SysMemory::Release();
		}

		void SetUp() override
		{
			m_cpu = EmuConfig.Cpu;
			m_saved0 = VU0;
			u8* micro = VU0.Micro;
			u8* mem = VU0.Mem;
			std::memset(&VU0, 0, sizeof(VU0));
			VU0.idx = 0;
			VU0.Micro = micro;
			VU0.Mem = mem;
			VU0.VF[0].f.w = 1.0f;
			for (u32 r = 1; r < 32; r++)
			{
				VU0.VI[r].UL = r * 713;
				for (u32 lane = 0; lane < 4; lane++)
					VU0.VF[r].F[lane] = (r + lane) * 0.125f;
			}
			VU0.ACC = VU0.VF[7];
			VU0.VI[REG_TPC].UL = 0;
			VU0.VI[REG_Q].UL = 0x3f800000;
			VU0.VI[REG_VPU_STAT].UL = 0x1;
			std::memset(VU0.Mem, 0x3f, VU0_MEMSIZE);
			for (u32 pc = 0; pc < VU0_PROGSIZE; pc += 8)
				Put(pc, kNopUpper, kNopLower);
			CpuArm64VU0.Reserve();
		}

		void TearDown() override
		{
			CpuArm64VU0.Reset();
			VU0 = m_saved0;
			EmuConfig.Cpu = m_cpu;
		}

		void Put(u32 pc, u32 upper, u32 lower)
		{
			std::memcpy(VU0.Micro + pc, &lower, 4);
			std::memcpy(VU0.Micro + pc + 4, &upper, 4);
		}

		// Runs the interpreter, rewinds, runs the recompiler, and requires the
		// resulting VU0 state to match bit for bit.
		void Compare(u32 cycles)
		{
			const VURegs initial = VU0;
			std::array<u8, VU0_MEMSIZE> initial_memory;
			std::memcpy(initial_memory.data(), VU0.Mem, initial_memory.size());

			CpuIntVU0.Execute(cycles);
			const VURegs expected = VU0;
			std::array<u8, VU0_MEMSIZE> expected_memory;
			std::memcpy(expected_memory.data(), VU0.Mem, expected_memory.size());

			VU0 = initial;
			std::memcpy(VU0.Mem, initial_memory.data(), initial_memory.size());
			CpuArm64VU0.Execute(cycles);

			ASSERT_EQ(VU0.VI[REG_Q].UL, expected.VI[REG_Q].UL);
			ASSERT_EQ(VU0.cycle, expected.cycle);
			ASSERT_EQ(VU0.VI[REG_TPC].UL, expected.VI[REG_TPC].UL);
			ASSERT_EQ(std::memcmp(VU0.VF, expected.VF, sizeof(VU0.VF)), 0);
			ASSERT_EQ(std::memcmp(VU0.VI, expected.VI, sizeof(VU0.VI)), 0);
			ASSERT_EQ(std::memcmp(&VU0, &expected, sizeof(VU0)), 0);
			ASSERT_EQ(std::memcmp(VU0.Mem, expected_memory.data(), expected_memory.size()), 0);
		}

		Pcsx2Config::CpuOptions m_cpu;
		VURegs m_saved0;
	};
} // namespace

// A compiled block never contains DIV itself (VU0Recompiler.cpp's Compile()
// stops the block right before it, same as any other unsupported lower op), but
// it does compile the ops that read Q afterward. The block's own cycle-driven
// retire step therefore has to flush the FDIV pipe as it advances the cycle, or
// a Q consumer past the divide's latency reads a stale value -- reproducing the
// shape of the Ridge Racer V regression this fixes (see VU0Pipeline.cpp).
TEST_F(VU0RecompilerTest, BackToBackDividesKeepQInSyncWithTheInterpreter)
{
	constexpr u32 kMulQ = MakeUpper(0x1c, 15, 5, 6, 0);

	for (u32 budget : {4u, 8u, 16u, 24u, 40u, 64u})
	{
		SCOPED_TRACE(testing::Message() << "budget=" << budget);
		VU0.VI[REG_TPC].UL = 0;
		VU0.cycle = 0;
		for (u32 pc = 0; pc < VU0_PROGSIZE; pc += 8)
			Put(pc, kNopUpper, kNopLower);
		Put(0, kNopUpper, MakeDiv(1, 0, 2, 1));
		Put(32, kMulQ, kNopLower);
		Put(40, kMulQ, kNopLower);
		Put(48, kNopUpper, MakeDiv(3, 2, 4, 3));
		Put(80, kMulQ, kNopLower);
		Put(88, kMulQ, kNopLower);

		Compare(budget);
	}
}

// Every other test here compares against the interpreter, which still passes if
// the recompiler quietly compiles nothing and steps every pair instead -- the
// results are identical either way. Assert that native code was actually
// emitted, so that silent degradation to the interpreter is a test failure.
TEST_F(VU0RecompilerTest, EmitsNativeCodeForASupportedBlock)
{
	ASSERT_EQ(CpuArm64VU0.GetCommittedCache(), 0u);
	VU0.VI[REG_TPC].UL = 0;
	VU0.cycle = 0;
	CpuArm64VU0.Execute(64);
	EXPECT_GT(CpuArm64VU0.GetCommittedCache(), 0u);
}
#endif
