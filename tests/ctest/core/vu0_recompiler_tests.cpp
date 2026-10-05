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

	// DIV Q, VFs[fsf], VFt[ftf].
	constexpr u32 MakeDiv(u32 fs, u32 fsf, u32 ft, u32 ftf)
	{
		return (0x40u << 25) | (ftf << 23) | (fsf << 21) | (ft << 16) | (fs << 11) | 0x3bc;
	}

	// Lower branch: target is pc + 8 + (sign-extended imm11 * 8).
	constexpr u32 MakeBranch(u32 op, u32 is, u32 it, u32 pc, u32 target)
	{
		const u32 imm = ((target - pc - 8) / 8) & 0x7ff;
		return (op << 25) | (it << 16) | (is << 11) | imm;
	}

	// IADDIU vit, vis, imm -- used to seed the loop counters the branches test.
	constexpr u32 MakeIaddiu(u32 it, u32 is, u32 imm)
	{
		return (8u << 25) | ((imm & 0x7800) << 10) | (it << 16) | (is << 11) | (imm & 0x7ff);
	}

	// ISUBIU vit, vis, imm.
	constexpr u32 MakeIsubiu(u32 it, u32 is, u32 imm)
	{
		return (9u << 25) | ((imm & 0x7800) << 10) | (it << 16) | (is << 11) | (imm & 0x7ff);
	}

	// ILW.x vit, offset(vis) -- an integer load, which issues into the IALU pipe
	// and only lands in vit some cycles later.
	constexpr u32 MakeIlw(u32 it, u32 is, u32 offset)
	{
		return (4u << 25) | (1u << 21) | (it << 16) | (is << 11) | (offset & 0x7ff);
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
			m_flag_hack = EmuConfig.Speedhacks.vuFlagHack;
			// Exact comparisons need every flag; the flag hack tests turn it on.
			EmuConfig.Speedhacks.vuFlagHack = false;
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
			EmuConfig.Speedhacks.vuFlagHack = m_flag_hack;
		}

		// Rewinds to a coherent starting state. Resetting VU0.cycle without also
		// emptying the pipe queues would leave entries whose sCycle sits in the
		// future relative to the rewound clock -- a state the emulator never
		// reaches, and one the two execution paths resolve differently.
		void Rewind()
		{
			VU0.VI[REG_TPC].UL = 0;
			VU0.cycle = 0;
			std::memset(VU0.fmac, 0, sizeof(VU0.fmac));
			VU0.fmacreadpos = VU0.fmacwritepos = VU0.fmaccount = 0;
			std::memset(VU0.ialu, 0, sizeof(VU0.ialu));
			VU0.ialureadpos = VU0.ialuwritepos = VU0.ialucount = 0;
			std::memset(&VU0.fdiv, 0, sizeof(VU0.fdiv));
			std::memset(&VU0.efu, 0, sizeof(VU0.efu));
			VU0.VIBackupCycles = 0;
			VU0.branch = VU0.ebit = 0;
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

		// Runs the microprogram through its E bit in slices of `cycles`.
		static void RunToEnd(BaseVUmicroCPU& cpu, u32 cycles)
		{
			for (u32 slice = 0; slice < 4096 && (VU0.VI[REG_VPU_STAT].UL & 1); slice++)
				cpu.Execute(cycles);
			ASSERT_FALSE(VU0.VI[REG_VPU_STAT].UL & 1);
		}

		// Under the flag hack, the program's results and the flags it reads
		// match the interpreter. Sticky status bits of the ops whose flags
		// nothing saw may be missing, and so may the queue entries those ops
		// left behind.
		void CompareWithFlagHack(u32 cycles)
		{
			VU0.VI[REG_VPU_STAT].UL |= 1;
			const VURegs initial = VU0;
			EmuConfig.Speedhacks.vuFlagHack = true;
			RunToEnd(CpuIntVU0, cycles);
			const VURegs expected = VU0;
			VU0 = initial;
			RunToEnd(CpuArm64VU0, cycles);
			EmuConfig.Speedhacks.vuFlagHack = false;

			ASSERT_EQ(std::memcmp(VU0.VF, expected.VF, sizeof(VU0.VF)), 0);
			for (u32 reg = 0; reg < 32; reg++)
			{
				SCOPED_TRACE(testing::Message() << "vi" << reg);
				const u32 actual = VU0.VI[reg].UL, wanted = expected.VI[reg].UL;
				if (reg == REG_STATUS_FLAG)
				{
					EXPECT_EQ(actual & 0x3f, wanted & 0x3f);
					EXPECT_EQ(actual & ~wanted, 0u);
				}
				else
					EXPECT_EQ(actual, wanted);
			}
			EXPECT_EQ(VU0.cycle, expected.cycle);
		}

		Pcsx2Config::CpuOptions m_cpu;
		bool m_flag_hack;
		VURegs m_saved0;
	};

	constexpr u32 kEbit = 0x40000000;

	// FMAND vit, vis.
	constexpr u32 MakeFmand(u32 it, u32 is)
	{
		return (0x1au << 25) | (it << 16) | (is << 11);
	}
} // namespace

// A divide stages its result in the FDIV pipe, and the block's own cycle-driven
// retire step has to flush that pipe as it advances the cycle, or a Q consumer
// past the divide's latency reads a stale value -- the shape of the Ridge Racer
// V regression VU0Pipeline.cpp's FDIV retirement fixed. Back-to-back divides
// additionally make the second one stall on the first.
TEST_F(VU0RecompilerTest, BackToBackDividesKeepQInSyncWithTheInterpreter)
{
	constexpr u32 kMulQ = MakeUpper(0x1c, 15, 5, 6, 0);

	for (u32 budget : {4u, 8u, 16u, 24u, 40u, 64u})
	{
		SCOPED_TRACE(testing::Message() << "budget=" << budget);
		Rewind();
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

// Games swap microprograms at the same VU0 address, so the cache keeps more
// than one block per PC. Once both programs are compiled, alternating between
// them must reuse their code instead of recompiling on every switch.
TEST_F(VU0RecompilerTest, AlternatingMicroprogramsAtOneAddressKeepTheirCode)
{
	constexpr u32 kAdd = MakeUpper(0x28, 15, 5, 6, 7);
	constexpr u32 kSub = MakeUpper(0x2c, 15, 5, 6, 7);
	size_t committed = 0;
	for (u32 round = 0; round < 6; round++)
	{
		SCOPED_TRACE(testing::Message() << "round=" << round);
		Rewind();
		Put(0, (round & 1) ? kSub : kAdd, kNopLower);
		Compare(16);
		if (round == 1)
			committed = CpuArm64VU0.GetCommittedCache();
		else if (round > 1)
			EXPECT_EQ(CpuArm64VU0.GetCommittedCache(), committed);
	}
	EXPECT_GT(committed, 0u);
}

// An integer load lands in its destination register some cycles after it
// issues, so a branch testing that register has to wait for the load to retire
// rather than reading the stale value. The compiled branch gets that wait from
// the pipeline's branch entry (VU0Pipeline.cpp), not from its own codegen.
TEST_F(VU0RecompilerTest, BranchesWaitForAPendingIntegerLoad)
{
	for (u32 budget : {4u, 8u, 16u, 32u, 64u})
	{
		SCOPED_TRACE(testing::Message() << "budget=" << budget);
		Rewind();
		for (u32 pc = 0; pc < VU0_PROGSIZE; pc += 8)
			Put(pc, kNopUpper, kNopLower);
		Put(0, kNopUpper, MakeIaddiu(3, 0, 0)); // vi3 = 0 (load address)
		Put(8, kNopUpper, MakeIaddiu(2, 0, 0)); // vi2 = 0 (compare operand)
		Put(16, kNopUpper, MakeIlw(1, 3, 0)); // vi1 = mem[vi3], still in flight
		Put(24, kNopUpper, MakeBranch(0x28, 1, 2, 24, 48)); // IBEQ vi1, vi2 -> 48
		Put(32, kNopUpper, kNopLower); // delay slot
		Put(40, kNopUpper, MakeIaddiu(4, 0, 1));
		Put(48, kNopUpper, MakeIaddiu(5, 0, 1));

		Compare(budget);
	}
}

// A backward integer-conditional branch is the shape a VU0 microprogram loop
// actually takes, and the one the recompiler has to get right to keep a trace
// going instead of handing every branch plus its delay slot to the interpreter.
// The branch's own register read must observe the interpreter's one-pair integer
// write delay (VIBackupCycles), so the counter is updated in the pair right
// before the branch on purpose.
TEST_F(VU0RecompilerTest, ConditionalLoopBranchesMatchTheInterpreter)
{
	constexpr u32 kMul = MakeUpper(0x2a, 15, 9, 5, 6);

	for (u32 budget : {4u, 8u, 16u, 32u, 64u, 128u})
	{
		SCOPED_TRACE(testing::Message() << "budget=" << budget);
		Rewind();
		for (u32 pc = 0; pc < VU0_PROGSIZE; pc += 8)
			Put(pc, kNopUpper, kNopLower);
		Put(0, kNopUpper, MakeIaddiu(1, 0, 4)); // vi1 = 4
		Put(8, kNopUpper, MakeIaddiu(2, 0, 0)); // vi2 = 0
		// loop:
		Put(16, kMul, kNopLower);
		Put(24, kNopUpper, MakeIsubiu(1, 1, 1)); // vi1 -= 1, read by the branch below
		Put(32, kMul, MakeBranch(0x29, 1, 2, 32, 16)); // IBNE vi1, vi2 -> loop
		Put(40, kMul, kNopLower); // delay slot
		// fallthrough
		Put(48, kMul, kNopLower);
		Put(56, kNopUpper, MakeBranch(0x20, 0, 0, 56, 72)); // B -> 72
		Put(64, kMul, kNopLower); // delay slot
		Put(72, kMul, kNopLower);

		Compare(budget);
	}
}

// DIV/SQRT/RSQRT are the remaining reason VU0 traces were cut short, so assert
// that a block starting on one is actually compiled rather than handed back to
// the interpreter a pair at a time.
TEST_F(VU0RecompilerTest, EmitsNativeCodeForABlockStartingOnADivide)
{
	ASSERT_EQ(CpuArm64VU0.GetCommittedCache(), 0u);
	Rewind();
	Put(0, kNopUpper, MakeDiv(1, 0, 2, 1));
	CpuArm64VU0.Execute(64);
	EXPECT_GT(CpuArm64VU0.GetCommittedCache(), 0u);
}

// The idiom that broke Ridge Racer V's car rendering: WAITQ paired with an
// upper op that broadcasts Q. The interpreter runs the lower op's stall and the
// pipe retirement before executing the upper (VU0microInterp.cpp calls
// _vuTestLowerStalls/_vuTestPipes ahead of _vu0ExecUpper), so the multiply sees
// the divide's freshly retired result. Emitting the pair in source order
// instead makes it multiply by the previous Q.
TEST_F(VU0RecompilerTest, WaitqRetiresQBeforeThePairedUpperReadsIt)
{
	constexpr u32 kMulQ = MakeUpper(0x1c, 15, 5, 6, 0);
	constexpr u32 kWaitq = (0x40u << 25) | 0x3bf;

	for (u32 budget : {2u, 3u, 4u, 8u, 12u, 16u, 32u})
	{
		SCOPED_TRACE(testing::Message() << "budget=" << budget);
		Rewind();
		for (u32 pc = 0; pc < VU0_PROGSIZE; pc += 8)
			Put(pc, kNopUpper, kNopLower);
		Put(0, kNopUpper, MakeDiv(1, 0, 2, 1));
		Put(8, kMulQ, kWaitq); // waits for the divide, then multiplies by its Q
		Put(16, kMulQ, kNopLower);

		Compare(budget);
	}
}

TEST_F(VU0RecompilerTest, ADivideRetiresThePreviousQBeforeThePairedUpperReadsIt)
{
	// Same hazard as the WAITQ test above, but through the far more common shape:
	// a divide whose own FDIV-pipe stall retires the *previous* divide's Q, paired
	// with an upper op that broadcasts Q in that very pair.
	constexpr u32 kMulQ = MakeUpper(0x1c, 15, 5, 6, 0);

	for (u32 budget : {2u, 3u, 4u, 8u, 12u, 16u, 32u})
	{
		SCOPED_TRACE(testing::Message() << "budget=" << budget);
		Rewind();
		for (u32 pc = 0; pc < VU0_PROGSIZE; pc += 8)
			Put(pc, kNopUpper, kNopLower);
		Put(0, kNopUpper, MakeDiv(1, 0, 2, 1));
		Put(8, kMulQ, MakeDiv(3, 0, 4, 1)); // reads the first divide's Q, issues a second
		Put(16, kMulQ, kNopLower);

		Compare(budget);
	}
}
// Under the VU flag hack, an FMAC op whose flags a later one replaces before
// anything reads them computes none. Run in slices, the program still leaves
// every result and the flags it read as the interpreter does, including when
// a slice ends between a hidden op and the one that replaces its flags.
TEST_F(VU0RecompilerTest, FlagHackKeepsResultsAndTheFlagsThatAreRead)
{
	for (u32 budget : {1u, 2u, 3u, 5u, 8u, 16u, 64u})
	{
		SCOPED_TRACE(testing::Message() << "budget=" << budget);
		Rewind();
		for (u32 pc = 0; pc < VU0_PROGSIZE; pc += 8)
			Put(pc, kNopUpper, kNopLower);
		VU0.VF[7].F[1] = -3.0f;
		Put(0, MakeUpper(0x2a, 15, 9, 5, 6), kNopLower); // MUL
		Put(8, MakeUpper(0x2c, 15, 10, 0, 7), kNopLower); // SUB, negative lanes
		Put(16, MakeUpper(0x28, 15, 11, 3, 4), kNopLower); // ADD
		Put(24, MakeUpper(0x2a, 15, 12, 1, 0), kNopLower); // MUL
		Put(32, MakeUpper(0x2c, 15, 13, 5, 5), kNopLower); // SUB, zero
		Put(64, kNopUpper, MakeFmand(1, 2));
		Put(72, MakeUpper(0x28, 15, 14, 6, 7), kNopLower); // ADD
		Put(80, MakeUpper(0x2c, 15, 15, 0, 7), kNopLower); // SUB, negative lanes
		Put(120, kNopUpper | kEbit, kNopLower);
		VU0.VI[2].UL = 0xffff;
		CompareWithFlagHack(budget);
	}
}

// An op without flags still flushes a denormal result to its sign when the
// FPCR does not flush it.
TEST_F(VU0RecompilerTest, FlagHackFlushesDenormalResultsLikeTheInterpreter)
{
	for (bool flush : {false, true})
	{
		SCOPED_TRACE(testing::Message() << "flush=" << flush);
		EmuConfig.Cpu.VU0FPCR = FPControlRegister::GetDefault().DisableExceptions().SetFlushToZero(flush);
		CpuArm64VU0.Reset();
		Rewind();
		for (u32 pc = 0; pc < VU0_PROGSIZE; pc += 8)
			Put(pc, kNopUpper, kNopLower);
		VU0.VF[5] = {};
		VU0.VF[5].F[0] = 1.5e-38f;
		VU0.VF[5].F[1] = -1.5e-38f;
		VU0.VF[5].F[2] = 0.0f;
		VU0.VF[5].F[3] = 3.0f;
		VU0.VF[6].F[0] = VU0.VF[6].F[1] = VU0.VF[6].F[2] = VU0.VF[6].F[3] = 0.25f;
		Put(0, MakeUpper(0x2a, 15, 9, 5, 6), kNopLower); // MUL, denormal lanes
		Put(8, MakeUpper(0x2a, 15, 10, 5, 6), kNopLower);
		Put(16, MakeUpper(0x2a, 15, 11, 5, 6), kNopLower);
		Put(64, kNopUpper | kEbit, kNopLower);
		CompareWithFlagHack(64);
	}
}

// A flag test that runs after an op's result retires but before the next
// op's does reads that op's flags, so they are still computed.
TEST_F(VU0RecompilerTest, FlagHackComputesFlagsATestCanRead)
{
	Rewind();
	VU0.VF[7].F[2] = -2.0f;
	Put(0, MakeUpper(0x2c, 15, 10, 0, 7), kNopLower); // SUB, a negative lane
	Put(8, MakeUpper(0x2a, 15, 9, 5, 6), kNopLower); // MUL
	Put(32, kNopUpper, MakeFmand(1, 2));
	Put(64, kNopUpper | kEbit, kNopLower);
	VU0.VI[2].UL = 0xffff;
	CompareWithFlagHack(64);
	EXPECT_NE(VU0.VI[1].UL, 0u);
}

// The E-bit pair runs on the interpreter after the block, and it may test the
// flags of an op whose replacement has not retired yet.
TEST_F(VU0RecompilerTest, FlagHackComputesFlagsCodeAfterTheBlockCanRead)
{
	Rewind();
	VU0.VF[7].F[2] = -2.0f;
	Put(0, MakeUpper(0x2c, 15, 10, 0, 7), kNopLower); // SUB, a negative lane
	Put(8, MakeUpper(0x2a, 15, 9, 5, 6), kNopLower); // MUL
	Put(32, kNopUpper | kEbit, MakeFmand(1, 2));
	VU0.VI[2].UL = 0xffff;
	CompareWithFlagHack(64);
	EXPECT_NE(VU0.VI[1].UL, 0u);
}

// The trace follows a branch's taken edge, so the fallthrough leaves for code
// the block did not see, which may test the flags.
TEST_F(VU0RecompilerTest, FlagHackComputesFlagsTheFallthroughCanRead)
{
	Rewind();
	VU0.VF[7].F[2] = -2.0f;
	Put(0, MakeUpper(0x2c, 15, 10, 0, 7), kNopLower); // SUB, a negative lane
	Put(8, kNopUpper, MakeBranch(0x29, 0, 0, 8, 40)); // IBNE vi0, vi0: never taken
	Put(32, kNopUpper, MakeFmand(1, 2));
	Put(40, MakeUpper(0x2a, 15, 9, 5, 6), kNopLower); // MUL on the taken edge
	Put(80, kNopUpper | kEbit, kNopLower);
	VU0.VI[2].UL = 0xffff;
	CompareWithFlagHack(64);
	EXPECT_NE(VU0.VI[1].UL, 0u);
}

// The flag hack drops the flag computation from the block, not just its use.
TEST_F(VU0RecompilerTest, FlagHackEmitsLessCode)
{
	const auto committed = [this](bool hack) {
		CpuArm64VU0.Reset();
		Rewind();
		for (u32 pc = 0; pc < 64; pc += 8)
			Put(pc, MakeUpper(0x2a, 15, 9 + pc / 8, 5, 6), kNopLower);
		Put(96, kNopUpper | kEbit, kNopLower);
		EmuConfig.Speedhacks.vuFlagHack = hack;
		VU0.VI[REG_VPU_STAT].UL |= 1;
		CpuArm64VU0.Execute(16);
		EmuConfig.Speedhacks.vuFlagHack = false;
		return CpuArm64VU0.GetCommittedCache();
	};
	const size_t exact = committed(false);
	EXPECT_LT(committed(true), exact);
}
#endif
