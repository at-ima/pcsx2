// SPDX-FileCopyrightText: 2026 PCSX2 Dev Team
// SPDX-License-Identifier: GPL-3.0+

#include "Common.h"

#if defined(ARCH_ARM64)
#include "arm64/VU1Recompiler.h"
#include <gtest/gtest.h>
#include <array>
#include <cstring>

namespace
{
	class VU1RecompilerTest : public testing::Test
	{
	protected:
		static void SetUpTestSuite() { ASSERT_TRUE(SysMemory::Allocate()); }
		static void TearDownTestSuite()
		{
			CpuArm64VU1.Shutdown();
			SysMemory::Release();
		}

		void SetUp() override
		{
			m_cpu = EmuConfig.Cpu;
			m_saved0 = VU0;
			m_saved1 = VU1;
			u8* micro = VU1.Micro;
			u8* mem = VU1.Mem;
			std::memset(&VU1, 0, sizeof(VU1));
			VU1.idx = 1;
			VU1.Micro = micro;
			VU1.Mem = mem;
			VU1.VF[0].f.w = 1.0f;
			for (u32 r = 1; r < 32; r++)
			{
				VU1.VI[r].UL = r * 713;
				for (u32 lane = 0; lane < 4; lane++)
					VU1.VF[r].F[lane] = (r + lane) * 0.125f;
			}
			VU1.ACC = VU1.VF[7];
			VU1.VI[REG_TPC].UL = 0;
			VU0.VI[REG_VPU_STAT].UL = 0x100;
			std::memset(VU1.Mem, 0x3f, VU1_MEMSIZE);
			for (u32 pc = 0; pc < VU1_PROGSIZE; pc += 8)
				Put(pc, 0x800002ff, 0x3f800000);
			CpuArm64VU1.Reserve();
		}

		void TearDown() override
		{
			CpuArm64VU1.Reset();
			VU0 = m_saved0;
			VU1 = m_saved1;
			EmuConfig.Cpu = m_cpu;
		}

		void Put(u32 pc, u32 upper, u32 lower)
		{
			std::memcpy(VU1.Micro + pc, &lower, 4);
			std::memcpy(VU1.Micro + pc + 4, &upper, 4);
		}

		void Compare(u32 cycles)
		{
			const VURegs initial0 = VU0, initial1 = VU1;
			const u32 vifstat = vif1Regs.stat._u32;
			std::array<u8, VU1_MEMSIZE> initial_memory;
			std::memcpy(initial_memory.data(), VU1.Mem, initial_memory.size());
			CpuIntVU1.Execute(cycles);
			const VURegs expected0 = VU0, expected1 = VU1;
			const u32 expected_vifstat = vif1Regs.stat._u32;
			std::array<u8, VU1_MEMSIZE> expected_memory;
			std::memcpy(expected_memory.data(), VU1.Mem, expected_memory.size());
			VU0 = initial0;
			VU1 = initial1;
			vif1Regs.stat._u32 = vifstat;
			std::memcpy(VU1.Mem, initial_memory.data(), initial_memory.size());
			CpuArm64VU1.Execute(cycles);
			ASSERT_EQ(VU1.cycle, expected1.cycle);
			ASSERT_EQ(VU1.VI[REG_TPC].UL, expected1.VI[REG_TPC].UL);
			ASSERT_EQ(std::memcmp(VU1.VF, expected1.VF, sizeof(VU1.VF)), 0);
			ASSERT_EQ(std::memcmp(VU1.VI, expected1.VI, sizeof(VU1.VI)), 0);
			ASSERT_EQ(std::memcmp(&VU1, &expected1, sizeof(VU1)), 0);
			ASSERT_EQ(std::memcmp(&VU0, &expected0, sizeof(VU0)), 0);
			ASSERT_EQ(std::memcmp(VU1.Mem, expected_memory.data(), expected_memory.size()), 0);
			ASSERT_EQ(vif1Regs.stat._u32, expected_vifstat);
		}

		Pcsx2Config::CpuOptions m_cpu;
		VURegs m_saved0, m_saved1;
	};
} // namespace

TEST_F(VU1RecompilerTest, ArithmeticTransfersAndIntegerOperations)
{
	constexpr u32 upper[] = {0x00, 0x04, 0x08, 0x0c, 0x10, 0x14, 0x18, 0x1c, 0x1d, 0x1e, 0x1f,
		0x20, 0x21, 0x22, 0x23, 0x24, 0x25, 0x26, 0x27, 0x28, 0x29, 0x2a, 0x2b, 0x2c, 0x2d, 0x2f,
		0x13c, 0x13d, 0x13e, 0x13f, 0x17c, 0x17d, 0x17e, 0x17f, 0x1fd, 0x2bc, 0x2bd, 0x2be, 0x2fc, 0x2fd, 0x2ff};
	constexpr u32 lower[] = {0x8000033c, 0x8000033d, 0x800003fc, 0x800003fd,
		0x80000030, 0x80000031, 0x80000032, 0x80000034, 0x80000035, 0x10000003, 0x12000003, 0, 0x02000000};
	for (u32 i = 0; i < 256; i++)
	{
		const u32 op = upper[i % std::size(upper)];
		const u32 dest = (op & 0x3f) >= 0x3c ? 0 : (i % 4) << 6;
		Put(i * 8, ((i % 16) << 21) | (2 << 16) | (1 << 11) | dest | op,
			((i % 16) << 21) | (3 << 16) | (1 << 11) | lower[i % std::size(lower)]);
	}
	for (u32 budget : {1u, 3u, 31u, 127u, 512u})
	{
		Compare(budget);
		if (HasFatalFailure())
			return;
	}
	EXPECT_GT(CpuArm64VU1.GetCommittedCache(), 0u);
}

TEST_F(VU1RecompilerTest, ConflictingUpperAndLowerAccesses)
{
	for (u32 i = 0; i < 32; i++)
	{
		const u32 fd = i % 3 + 1;
		const u32 ft = (i / 3) % 3 + 1;
		Put(i * 8, (15 << 21) | (2 << 16) | (1 << 11) | (fd << 6) | 0x28,
			(15 << 21) | (ft << 16) | (1 << 11) | 0x8000033c);
	}
	Compare(160);
}

TEST_F(VU1RecompilerTest, BranchEndBitAndSpecialInstructionFallback)
{
	Put(0, 0x2ff, 0x40000002); // B +2
	Put(8, 0x800002ff, 0x40000000); // Delay slot: I = 2.0
	Put(24, 0x2ff, 0x800003bc | (2 << 16) | (1 << 11)); // DIV
	Put(32, 0x400002ff, 0x8000033c); // E bit
	Put(40, 0x800002ff, 0x3f800000); // E-bit delay slot
	Compare(100);
	EXPECT_EQ(VU0.VI[REG_VPU_STAT].UL & 0x100, 0u);
}

TEST_F(VU1RecompilerTest, DetectsOverwrittenCodeAndMemoryRestoration)
{
	Compare(16);
	VU1.VI[REG_TPC].UL = 0;
	Put(0, (15 << 21) | (2 << 16) | (1 << 11) | (3 << 6) | 0x28, 0x8000033c);
	Compare(16);
	VU1.VI[REG_TPC].UL = 0;
	Put(0, 0x800002ff, 0x3f800000);
	Compare(16);
	CpuArm64VU1.Clear(0, 8);
	VU1.VI[REG_TPC].UL = 0;
	Put(0, 0x800002ff, 0x40000000);
	Compare(16);
}

TEST_F(VU1RecompilerTest, ProgramCounterAndCycleWrap)
{
	VU1.VI[REG_TPC].UL = (VU1_PROGSIZE - 8) / 8;
	VU1.cycle = ~u64(0) - 2;
	Compare(8);
}

TEST_F(VU1RecompilerTest, PendingFmacDependenciesAndLaneMasks)
{
	const VURegs initial = VU1;
	u32 random = 0x13579bdf;
	auto next = [&random]() { random = random * 1664525 + 1013904223; return random; };
	for (u64 cycle : {u64(0), u64(1), u64(100), ~u64(0) - 3, ~u64(0)})
	{
		for (u32 seed = 0; seed < 128; seed++)
		{
			SCOPED_TRACE(testing::Message() << "cycle=" << cycle << " seed=" << seed);
			VU1 = initial;
			VU1.cycle = cycle;
			VU1.fmaccount = seed % 5;
			VU1.fmacreadpos = (seed / 5) % 4;
			VU1.fmacwritepos = (VU1.fmacreadpos + VU1.fmaccount) & 3;
			VU1.VIBackupCycles = seed % 3;
			for (u32 n = 0; n < VU1.fmaccount; n++)
			{
				auto& pipe = VU1.fmac[(VU1.fmacreadpos + n) & 3];
				pipe.sCycle = cycle - (VU1.fmaccount - n - 1);
				pipe.Cycle = 4;
				pipe.regupper = (next() >> 16) % 4;
				pipe.reglower = (next() >> 16) % 4;
				pipe.xyzwupper = (next() >> 16) & 15;
				pipe.xyzwlower = (next() >> 16) & 15;
				pipe.flagreg = (1 << REG_STATUS_FLAG) | (1 << REG_CLIP_FLAG);
				pipe.statusflag = next() & 0xfff;
				pipe.macflag = next() & 0xffff;
				pipe.clipflag = next() & 0xffffff;
			}
			for (u32 i = 0; i < 8; i++)
			{
				const u32 mask = (next() >> 16) & 15;
				const u32 lowerMask = (next() >> 16) & 15;
				const u32 source = (next() >> 16) % 4;
				// Include two upper sources, a lower source, aliases and discarded writes.
				Put(i * 8, (mask << 21) | (2 << 16) | (1 << 11) | ((seed % 4) << 6) | 0x28,
					(lowerMask << 21) | (3 << 16) | (source << 11) | 0x8000033c);
			}
			Compare(seed % 8 + 1);
			if (HasFatalFailure())
				return;
		}
	}
}

TEST_F(VU1RecompilerTest, ScheduledDependenciesAcrossPipelineGapsAndBlockExits)
{
	const VURegs initial = VU1;
	u32 random = 0x87654321;
	auto next = [&random]() { random = random * 1664525 + 1013904223; return random; };
	for (u32 seed = 0; seed < 128; seed++)
	{
		SCOPED_TRACE(seed);
		VU1 = initial;
		VU1.cycle = (seed & 1) ? ~u64(0) - 8 : 100;
		for (u32 i = 0; i < 96; i++)
		{
			const u32 fields = next();
			const u32 upper = (fields & 1) ? 0x2ff :
			                                 (((fields >> 8) & 15) << 21) | (((fields >> 16) % 4) << 16) | (1 << 11) | (((fields >> 24) % 4) << 6) | 0x28;
			const u32 lower = (fields & 2) ? 0x80000030 | (1 << 11) | (2 << 16) | (3 << 6) :
			                                 0x8000033c | (((fields >> 12) & 15) << 21) | (3 << 16) | (((fields >> 20) % 4) << 11);
			Put(i * 8, upper, lower);
		}
		for (u32 budget : {1u, 7u, 17u, 64u, 127u})
		{
			Compare(budget);
			if (HasFailure())
				return;
		}
	}
}

TEST_F(VU1RecompilerTest, ScheduledPreparationRetiresMixedPipelines)
{
	const VURegs initial = VU1;
	for (u64 cycle : {u64(0), u64(100), ~u64(0) - 8})
	{
		for (u32 distance = 1; distance <= 4; distance++)
		{
			for (u32 budget : {1u, 3u, 8u, 32u, 128u})
			{
				SCOPED_TRACE(testing::Message() << "cycle=" << cycle << " distance=" << distance << " budget=" << budget);
				VU1 = initial;
				VU1.cycle = cycle;
				VU1.VIBackupCycles = 2;
				VU1.VI[REG_STATUS_FLAG].UL = 0xabc;
				VU1.fmaccount = 3;
				VU1.fmacreadpos = 3;
				VU1.fmacwritepos = 2;
				for (u32 n = 0; n < 3; n++)
				{
					auto& pipe = VU1.fmac[(3 + n) & 3];
					pipe.sCycle = cycle - (2 - n);
					pipe.Cycle = 4;
					pipe.regupper = 1;
					pipe.xyzwupper = 15;
					pipe.flagreg = n == 1 ? (1 << REG_CLIP_FLAG) | (1 << REG_STATUS_FLAG) : 0;
					pipe.statusflag = 0x481 + n;
					pipe.macflag = 0x1234 + n;
					pipe.clipflag = 0x54321 + n;
				}
				VU1.fdiv.enable = VU1.efu.enable = 1;
				VU1.fdiv.sCycle = VU1.efu.sCycle = cycle;
				VU1.fdiv.Cycle = 7;
				VU1.efu.Cycle = 11;
				VU1.fdiv.statusflag = 0x820;
				VU1.fdiv.reg.UL = 0x40000000;
				VU1.efu.reg.UL = 0x40400000;
				VU1.ialucount = 3;
				VU1.ialureadpos = 2;
				VU1.ialuwritepos = 1;
				for (u32 n = 0; n < 3; n++)
				{
					auto& pipe = VU1.ialu[(2 + n) & 3];
					pipe.sCycle = cycle;
					pipe.Cycle = 1 + n;
				}
				for (u32 i = 0; i < 96; i++)
				{
					// Repeated VF1 writes exercise each scheduled dependency distance.
					// MULq also observes FDIV's Q writeback during the native block.
					const u32 upper = i % distance == 0 ? (15 << 21) | (1 << 11) | (1 << 6) | 0x1c : 0x2ff;
					Put(i * 8, upper, 0x80000030 | (2 << 16) | (1 << 11) | (3 << 6));
				}
				Compare(budget);
				if (HasFailure())
					return;
			}
		}
	}
}

TEST_F(VU1RecompilerTest, RecompilesInputFlushWhenFpcrChanges)
{
	const VURegs initial = VU1;
	Put(0, 0x80000000 | (15 << 21) | (2 << 16) | (1 << 11) | (3 << 6) | 0x2a, 0x3f800000);
	for (bool flush : {true, false, true, false})
	{
		SCOPED_TRACE(flush);
		VU1 = initial;
		VU1.VF[1].UL[0] = 1;
		VU1.VF[1].UL[1] = 0x80000001;
		VU1.VF[1].UL[2] = 0x007fffff;
		VU1.VF[1].UL[3] = 0x807fffff;
		for (u32 lane = 0; lane < 4; lane++)
			VU1.VF[2].UL[lane] = 0x7f7fffff;
		EmuConfig.Cpu.VU1FPCR = FPControlRegister::GetDefault().DisableExceptions().SetFlushToZero(flush);
		// Reuse the same instruction bytes and cache entry. Without recompiling
		// after disabling FZ, denormal times max-finite produces a nonzero result.
		Compare(1);
		if (HasFailure())
			return;
	}
}

TEST_F(VU1RecompilerTest, SpecialFloatsAndChangedFloatingPointOptions)
{
	const VURegs initial = VU1;
	constexpr u32 edge[] = {0, 0x80000000, 1, 0x80000001, 0x007fffff, 0x00800000,
		0x3f800001, 0xbf800002, 0x7f7fffff, 0xff7fffff, 0x7f800000, 0xff800000,
		0x7fc12345, 0xff812345, 0x4f000000, 0xcf000000};
	u32 random = 12345;
	auto next = [&random]() { random = random * 1664525 + 1013904223; return random; };
	for (u32 options = 0; options < 32; options++)
	{
		EmuConfig.Cpu.VU1FPCR = FPControlRegister::GetDefault().DisableExceptions().SetRoundMode(static_cast<FPRoundMode>(options & 3)).SetFlushToZero(options & 4);
		EmuConfig.Cpu.Recompiler.vu0Overflow = (options & 8) != 0;
		EmuConfig.Cpu.Recompiler.vu1Overflow = (options & 16) != 0;
		for (u32 seed = 0; seed < 16; seed++)
		{
			SCOPED_TRACE(testing::Message() << "options=" << options << " seed=" << seed);
			VU1 = initial;
			for (u32 reg = 1; reg < 32; reg++)
				for (u32 lane = 0; lane < 4; lane++)
					VU1.VF[reg].UL[lane] = seed < 8 ? edge[(reg + lane + seed) % std::size(edge)] : next();
			VU1.ACC = VU1.VF[7];
			for (u32 i = 0; i < 64; i++)
			{
				u32 op = i % 0x30;
				const u32 mask = i < 32 ? 15 : i % 16;
				Put(i * 8, (mask << 21) | ((1 + (i % 3)) << 16) | (1 << 11) | ((i % 4) << 6) | op,
					0x8000033c | (mask << 21) | (4 << 16) | (3 << 11));
			}
			Compare(192);
			if (HasFatalFailure())
				return;
		}
	}
}
#endif
