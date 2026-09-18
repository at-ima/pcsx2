// SPDX-FileCopyrightText: 2026 PCSX2 Dev Team
// SPDX-License-Identifier: GPL-3.0+

#include "Common.h"

#if defined(ARCH_ARM64)
#include "arm64/VU1Recompiler.h"
#include "arm64/VU1Pipeline.h"
#include "Gif_Unit.h"
#include "common/HostSys.h"
#include "vixl/aarch64/macro-assembler-aarch64.h"
#include <gtest/gtest.h>
#include <array>
#include <cstring>

namespace
{
	std::array<VECTOR, 8> s_seen_vectors;
	s32 s_transfer_cycles;
	bool s_transfer_flush;
	u32 s_transfer_calls;

	void ObservePipelineCallout(s32 cycles, bool flush)
	{
		s_transfer_cycles = cycles;
		s_transfer_flush = flush;
		s_transfer_calls++;
		for (u32 slot = 0; slot < 8; slot++)
		{
			VECTOR& value = slot == 7 ? VU1.ACC : VU1.VF[1 + slot];
			s_seen_vectors[slot] = value;
			for (u32 lane = 0; lane < 4; lane++)
				value.UL[lane] = 0xa5000000 | (slot << 8) | lane;
		}
		VU1.cycle += 257;
		// AAPCS64 preserves only the low halves of v8..v15. Exercise the upper
		// half clobbers which the generated call boundary must handle itself.
		asm volatile("movi v8.16b, #17\n movi v9.16b, #17\n movi v10.16b, #17\n movi v11.16b, #17\n"
					 "movi v12.16b, #17\n movi v13.16b, #17\n movi v14.16b, #17\n movi v15.16b, #17"
			: : : "v8", "v9", "v10", "v11", "v12", "v13", "v14", "v15");
	}

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

TEST_F(VU1RecompilerTest, BlockBoundaryPreservesHostVectorRegisters)
{
	using namespace vixl::aarch64;
	using Wrapper = void (*)(u64*, u32);
	u8* const base = SysMemory::GetVU0Rec();
	HostSys::BeginCodeWrite();
	MacroAssembler a(base, 4096);
	a.Stp(x19, lr, MemOperand(sp, -80, PreIndex));
	for (u32 slot = 0; slot < 8; slot += 2)
		a.Stp(VRegister(8 + slot, 64), VRegister(9 + slot, 64), MemOperand(sp, 16 + slot * 8));
	a.Mov(x19, x0);
	for (u32 slot = 0; slot < 8; slot++)
	{
		a.Mov(x9, 0x0123456789abcde0ULL + slot);
		a.Fmov(VRegister(8 + slot, 64), x9);
	}
	a.Mov(w0, w1);
	a.Mov(x16, reinterpret_cast<uintptr_t>(+[](u32 cycles) { CpuArm64VU1.Execute(cycles); }));
	a.Blr(x16);
	for (u32 slot = 0; slot < 8; slot++)
		a.Str(VRegister(8 + slot, 64), MemOperand(x19, slot * 8));
	for (u32 slot = 0; slot < 8; slot += 2)
		a.Ldp(VRegister(8 + slot, 64), VRegister(9 + slot, 64), MemOperand(sp, 16 + slot * 8));
	a.Ldp(x19, lr, MemOperand(sp, 80, PostIndex));
	a.Ret();
	a.FinalizeCode();
	HostSys::EndCodeWrite();
	HostSys::FlushInstructionCache(base, static_cast<u32>(a.GetSizeOfCodeGenerated()));
	const VURegs initial = VU1, initial0 = VU0;
	for (u32 count = 0; count <= 8; count++)
	{
		for (u32 i = 0; i < 16; i++)
		{
			const u32 reg = count ? 1 + i % count : 0;
			Put(i * 8, count ? 0x80000000 | (15 << 21) | (reg << 16) | (reg << 11) | (reg << 6) | 0x28 : 0x800002ff,
				0x3f800000);
		}
		Put(128, 0xc00002ff, 0x3f800000);
		Put(136, 0x800002ff, 0x3f800000);
		for (u32 budget : {1u, 16u, 64u})
		{
			SCOPED_TRACE(testing::Message() << "count=" << count << " budget=" << budget);
			VU0 = initial0;
			VU1 = initial;
			std::array<u64, 8> output{};
			reinterpret_cast<Wrapper>(base)(output.data(), budget);
			for (u32 slot = 0; slot < 8; slot++)
				EXPECT_EQ(output[slot], 0x0123456789abcde0ULL + slot);
		}
	}
}

TEST_F(VU1RecompilerTest, IntegerLoadsAndPositiveBranchesMatchPipelineTiming)
{
	const VURegs initial = VU1, initial0 = VU0;
	for (u32 mask = 0; mask < 16; mask++)
	{
		for (u32 dest : {0u, 1u, 2u})
		{
			for (u32 budget = 1; budget <= 12; budget++)
			{
				SCOPED_TRACE(testing::Message() << mask << "/" << dest << "/" << budget);
				VU0 = initial0;
				VU1 = initial;
				VU1.VI[1].UL = 0xabcdffff;
				VU1.ialureadpos = VU1.ialuwritepos = 3;
				std::memset(VU1.ialu, 0xa5, sizeof(VU1.ialu));
				VU1.VIBackupCycles = 3;
				VU1.VIRegNumber = dest;
				VU1.VIOldValue = 0xffff;
				const u32 data[] = {1u, 0x8000u, 0xffffu, 0u};
				std::memcpy(VU1.Mem + 0x3fe0, data, sizeof(data));
				Put(0, 0x2ff, 0x08000000 | (mask << 21) | (dest << 16) | (1 << 11) | 0x7ff);
				Put(8, 0x2ff, 0x5a000000 | (dest << 11) | 2); // IBGTZ, with ILW dependency.
				Put(16, 0x800002ff, 0x40000000);
				Put(24, 0xc00002ff, 0x40400000);
				Put(32, 0x800002ff, 0x40800000);
				Put(40, 0xc00002ff, 0x41000000);
				Put(48, 0x800002ff, 0x3f800000);
				Compare(budget);
				if (HasFatalFailure())
					return;
			}
		}
	}
}

TEST_F(VU1RecompilerTest, IntegerBranchUsesBackupAndIncomingHazards)
{
	const VURegs initial = VU1, initial0 = VU0;
	Put(0, 0x2ff, 0x5a000000 | (2 << 11) | 2);
	Put(8, 0x800002ff, 0x40000000);
	for (u32 value : {0u, 1u, 0x7fffu, 0x8000u, 0xffffu})
		for (u32 latency : {0u, 1u, 2u, 4u, 16u, 260u})
			for (bool wrap : {false, true})
			{
				SCOPED_TRACE(testing::Message() << value << "/" << latency << "/" << wrap);
				VU0 = initial0;
				VU1 = initial;
				VU1.cycle = wrap ? ~u64(0) - 2 : 100;
				VU1.VI[2].UL = 0xffff0000 | value;
				VU1.VIRegNumber = 2;
				VU1.VIOldValue = value ? 0 : 1;
				VU1.VIBackupCycles = 2;
				if (latency)
				{
					VU1.ialucount = 1;
					VU1.ialuwritepos = 1;
					VU1.ialu[0].sCycle = VU1.cycle;
					VU1.ialu[0].Cycle = latency;
					VU1.ialu[0].reg = 1 << 2;
				}
				Compare(1);
				if (HasFatalFailure())
					return;
			}
}

TEST_F(VU1RecompilerTest, IntegerBranchCombinesFmacAndIaluWaits)
{
	const VURegs initial = VU1, initial0 = VU0;
	Put(0, (15 << 21) | (2 << 16) | (1 << 11) | (3 << 6) | 0x28, 0x5a000000 | (2 << 11) | 2);
	for (u32 latency : {1u, 2u, 4u, 8u})
		for (u32 match : {1u, 2u})
			for (u32 budget : {1u, 4u, 8u})
			{
				VU0 = initial0;
				VU1 = initial;
				VU1.cycle = 100;
				VU1.fmaccount = 1;
				VU1.fmacwritepos = 1;
				VU1.fmac[0].sCycle = 100;
				VU1.fmac[0].Cycle = 4;
				VU1.fmac[0].regupper = 1;
				VU1.fmac[0].xyzwupper = 15;
				VU1.ialucount = 1;
				VU1.ialuwritepos = 1;
				VU1.ialu[0].sCycle = 100;
				VU1.ialu[0].Cycle = latency;
				VU1.ialu[0].reg = 1 << match;
				Compare(budget);
				if (HasFatalFailure())
					return;
			}
}

TEST_F(VU1RecompilerTest, IntegerLoadsResumeScheduledSuffix)
{
	const VURegs initial = VU1, initial0 = VU0;
	for (u32 load_at : {0u, 7u, 16u, 30u})
	{
		for (u32 i = 0; i < 64; i++)
			Put(i * 8, 0x80000000 | (15 << 21) | (2 << 16) | (3 << 11) | (3 << 6) | 0x28, 0x3f800000);
		for (u32 i = load_at; i < load_at + 4; i++)
			Put(i * 8, 0x2ff, 0x08000000 | (15 << 21) | (2 << 16) | (1 << 11));
		Put(512, 0xc00002ff, 0x3f800000);
		Put(520, 0x800002ff, 0x3f800000);
		for (u32 budget : {1u, 7u, 16u, 31u, 64u, 128u, 256u})
		{
			VU0 = initial0;
			VU1 = initial;
			Compare(budget);
			if (HasFatalFailure())
				return;
		}
	}
}

TEST_F(VU1RecompilerTest, ConditionalTailPreservesEveryBudgetExit)
{
	const VURegs initial = VU1, initial0 = VU0;
	for (u32 length : {16u, 32u})
		for (u32 load_at : {0u, 8u, length - 1})
			for (u32 value : {0u, 1u, 0xffffu})
			{
				for (u32 i = 0; i < length; i++)
					Put(i * 8, 0x80000000 | (15 << 21) | (2 << 16) | (3 << 11) | (3 << 6) | 0x28, 0x3f800000);
				Put(load_at * 8, 0x2ff, 0x08000000 | (8 << 21) | (2 << 16) | (1 << 11));
				Put(length * 8, 0x2ff, 0x5a000000 | (2 << 11) | 3);
				Put((length + 1) * 8, 0x800002ff, 0x40000000);
				Put((length + 2) * 8, 0xc00002ff, 0x40400000);
				Put((length + 3) * 8, 0x800002ff, 0x40800000);
				Put((length + 4) * 8, 0xc00002ff, 0x41000000);
				Put((length + 5) * 8, 0x800002ff, 0x3f800000);
				for (u32 budget = 1; budget <= length * 4 + 8; budget++)
				{
					SCOPED_TRACE(testing::Message() << length << "/" << load_at << "/" << value << "/" << budget);
					VU0 = initial0;
					VU1 = initial;
					VU1.VI[1].UL = 0;
					std::memcpy(VU1.Mem, &value, sizeof(value));
					Compare(budget);
					if (HasFatalFailure())
						return;
				}
			}
}

TEST_F(VU1RecompilerTest, ConditionalSuccessorsPreserveBudgetsAndSourceEdits)
{
	const VURegs initial = VU1, initial0 = VU0;
	const u32 add = (15 << 21) | (2 << 16) | (3 << 11) | (3 << 6) | 0x28;
	for (u32 i = 0; i < 96; i++)
		Put(i * 8, 0x80000000 | add, 0x3f800000);
	Put(24 * 8, add, 0x5a000000 | (2 << 11) | (48 - 25));
	Put(64 * 8, add, 0x5a000000 | (3 << 11) | (80 - 65));
	Put(88 * 8, add, 0x40000000 | ((8 - 89) & 0x7ff));
	for (u32 pass = 0; pass < 4; pass++)
	{
		if (pass == 1)
			Put(52 * 8, 0x80000000 | ((add & ~63u) | 0x2c), 0x41000000);
		else if (pass == 2)
			Put(24 * 8, add, 0x5a000000 | (2 << 11) | (50 - 25));
		else if (pass == 3)
			Put(25 * 8, 0x2ff, 0x5a000000 | (3 << 11) | 2); // Nested delay branch.
		for (u32 value : {0u, 1u, 0xffffu})
			for (u32 latency : {0u, 104u, 260u})
				for (u32 budget = 1; budget <= 340; budget++)
				{
					SCOPED_TRACE(testing::Message() << pass << "/" << value << "/" << latency << "/" << budget);
					VU0 = initial0;
					VU1 = initial;
					VU1.cycle = 100;
					VU1.VI[2].UL = value;
					VU1.VI[3].UL = pass & 1;
					if (latency)
					{
						VU1.ialucount = 1;
						VU1.ialuwritepos = 1;
						VU1.ialu[0].sCycle = VU1.cycle;
						VU1.ialu[0].Cycle = latency;
						VU1.ialu[0].reg = 1 << 2;
					}
					Compare(budget);
					if (HasFatalFailure())
						return;
				}
	}
}

TEST_F(VU1RecompilerTest, BudgetLimitedValidationChecksLaterCodeBeforeExecution)
{
	const VURegs initial = VU1, initial0 = VU0;
	Put(0, 0x800002ff, 0x3f800000);
	Put(8, 0x2ff, 0x5a000000 | (2 << 11) | 30); // Taken target at byte 256.
	Put(16, 0x800002ff, 0x40000000);
	Put(384, 0xc00002ff, 0x3f800000);
	for (u32 pass = 0; pass < 4; pass++)
	{
		if (pass == 1)
			Put(280, 0x800002ff, 0x40400000); // Later target region.
		else if (pass == 2)
			Put(16, 0x800002ff, 0x40800000); // Conditional delay.
		else if (pass == 3)
			Put(8, 0x2ff, 0x5a000000 | (2 << 11) | 34); // Retarget the edge.
		for (u32 budget : {1u, 2u, 3u, 4u, 8u, 32u})
		{
			VU0 = initial0;
			VU1 = initial;
			VU1.VI[2].UL = 1;
			Compare(budget);
			if (HasFatalFailure())
				return;
		}
	}
}

TEST_F(VU1RecompilerTest, ConditionalDelayWrapAndTraceLimit)
{
	const VURegs initial = VU1, initial0 = VU0;
	for (u32 start : {0u, VU1_PROGSIZE - 8})
		for (u32 length : {1u, 255u, 256u})
		{
			for (u32 pc = 0; pc < VU1_PROGSIZE; pc += 8)
				Put(pc, 0x800002ff, 0x3f800000);
			const u32 pc = (start + (length - 1) * 8) & VU1_PROGMASK;
			Put(pc, 0x2ff, 0x5a000000 | (2 << 11) | 3);
			Put((pc + 8) & VU1_PROGMASK, 0x800002ff, 0x40000000);
			Put((pc + 32) & VU1_PROGMASK, 0xc00002ff, 0x40800000);
			for (u32 value : {0u, 1u})
				for (u32 budget : {length, length + 1, length + 2, length + 8})
				{
					SCOPED_TRACE(testing::Message() << start << "/" << length << "/" << value << "/" << budget);
					VU0 = initial0;
					VU1 = initial;
					VU1.VI[REG_TPC].UL = start / 8;
					VU1.VI[2].UL = value;
					Compare(budget);
					if (HasFatalFailure())
						return;
				}
		}
}

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

TEST_F(VU1RecompilerTest, ConnectedBranchRegionsPreserveEveryBudgetAndSourceEdit)
{
	const VURegs initial = VU1, initial0 = VU0;
	const u32 add = (15 << 21) | (2 << 16) | (3 << 11) | (3 << 6) | 0x28;
	for (u32 i = 0; i < 24; i++)
		Put(i * 8, 0x80000000 | add, 0x3f800000);
	Put(24 * 8, add, 0x40000000 | (64 - 25)); // Connect to pair 64 after the delay.
	Put(25 * 8, 0x80000000 | add, 0x40000000);
	for (u32 i = 64; i < 80; i++)
		Put(i * 8, 0x80000000 | add, 0x40400000);
	Put(80 * 8, add, 0x40000000 | ((8 - 81) & 0x7ff)); // A back edge ends the trace.
	Put(81 * 8, 0x80000000 | add, 0x40800000);
	for (u32 budget = 1; budget <= 192; budget++)
	{
		SCOPED_TRACE(budget);
		VU0 = initial0;
		VU1 = initial;
		VU1.cycle = 100;
		Compare(budget);
		if (HasFatalFailure())
			return;
	}
	// Both a destination edit and a changed edge must invalidate a cached trace.
	for (u32 pass = 0; pass < 3; pass++)
	{
		VU0 = initial0;
		VU1 = initial;
		if (pass == 0)
			Put(70 * 8, 0x80000000 | ((add & ~63u) | 0x2c), 0x41000000);
		else if (pass == 1)
			Put(24 * 8, add, 0x40000000 | (68 - 25));
		else
			Put(25 * 8, 0x2ff, 0x40000002); // A nested branch restores the fallback path.
		Compare(192);
		if (HasFatalFailure())
			return;
	}
}

TEST_F(VU1RecompilerTest, ConnectedBranchDelayWrapsMicroMemory)
{
	const VURegs initial = VU1, initial0 = VU0;
	Put(VU1_PROGSIZE - 8, 0x2ff, 0x40000002);
	Put(0, 0x800002ff, 0x40000000);
	Put(16, 0xc00002ff, 0x3f800000);
	Put(24, 0x800002ff, 0x3f800000);
	for (u32 budget : {1u, 2u, 3u, 4u, 16u})
	{
		VU0 = initial0;
		VU1 = initial;
		VU1.VI[REG_TPC].UL = (VU1_PROGSIZE - 8) / 8;
		Compare(budget);
		if (HasFatalFailure())
			return;
	}
}

TEST_F(VU1RecompilerTest, TerminalBranchPreservesEveryBudgetPrefix)
{
	const VURegs initial = VU1, initial0 = VU0;
	for (u32 length : {1u, 8u, 24u, 256u})
	{
		for (s32 displacement : {-1024, -2, -1, 0, 3, 1023})
		{
			for (u32 i = 0; i < length + 2; i++)
				Put(i * 8, 0x800002ff, 0x3f800000);
			Put((length - 1) * 8, (15 << 21) | (2 << 16) | (1 << 11) | (3 << 6) | 0x28,
				0x40000000 | (static_cast<u32>(displacement) & 0x7ff));
			for (u32 budget : {length, length + 1, length + 2, length + 8})
			{
				SCOPED_TRACE(testing::Message() << "length=" << length << " displacement=" << displacement << " budget=" << budget);
				VU0 = initial0;
				VU1 = initial;
				Compare(budget);
				if (HasFatalFailure())
					return;
			}
		}
	}
}

TEST_F(VU1RecompilerTest, BranchDelayPreservesPipelineAndChainedTargets)
{
	const VURegs initial = VU1, initial0 = VU0;
	for (u32 variant = 0; variant < 7; variant++)
	{
		for (u32 budget : {1u, 2u, 3u, 4u, 8u, 32u})
		{
			for (bool wrap : {false, true})
			{
				SCOPED_TRACE(testing::Message() << "variant=" << variant << " budget=" << budget << " wrap=" << wrap);
				VU0 = initial0;
				VU1 = initial;
				VU1.cycle = wrap ? ~u64(0) - 2 : 100;
				VU1.branch = variant == 5 ? 2 : 1;
				VU1.branchpc = 64;
				VU1.takedelaybranch = variant == 3;
				VU1.delaybranchpc = 96;
				VU1.VIBackupCycles = 2;
				VU1.ebit = variant == 6 ? 1 : 0;
				Put(0, (15 << 21) | (2 << 16) | (1 << 11) | (3 << 6) | 0x28,
					0x02000000 | (15 << 21) | (3 << 11) | (2 << 16)); // SQ observes old VF3.
				Put(8, 0x800002ff, 0x40000000);
				Put(64, 0x800002ff, 0x40400000);
				Put(96, 0xc00002ff, 0x3f800000);
				Put(104, 0x800002ff, 0x3f800000);
				if (variant == 1 || variant == 2)
				{
					VU1.fmaccount = 1;
					VU1.fmacwritepos = 1;
					VU1.fmac[0].sCycle = VU1.cycle;
					VU1.fmac[0].Cycle = 4;
					VU1.fmac[0].regupper = 1;
					VU1.fmac[0].xyzwupper = 15;
				}
				if (variant == 2)
				{
					VU1.fdiv.enable = 1;
					VU1.fdiv.sCycle = VU1.cycle;
					VU1.fdiv.Cycle = 2;
					VU1.fdiv.reg.UL = 0x40000000;
				}
				if (variant == 4)
					Put(0, 0x2ff, 0x40000002); // Branch in delay slot must retain fallback.
				Compare(budget);
				if (HasFatalFailure())
					return;
			}
		}
	}
}

TEST_F(VU1RecompilerTest, BranchDelayValidationDoesNotHideEditedTail)
{
	const VURegs initial = VU1, initial0 = VU0;
	for (u32 pass = 0; pass < 4; pass++)
	{
		VU0 = initial0;
		VU1 = initial;
		VU1.branch = pass == 3 ? 0 : 1;
		VU1.branchpc = 64;
		if (pass == 1)
			Put(8, 0x80000000 | (15 << 21) | (2 << 16) | (1 << 11) | (3 << 6) | 0x28, 0x40000000);
		if (pass == 2)
			Put(0, 0x800002ff, 0x40400000); // The executed first pair must always be checked.
		Compare(pass == 3 ? 16 : 1);
		if (HasFatalFailure())
			return;
	}
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

TEST_F(VU1RecompilerTest, CachedVectorsAndAccumulatorAtEveryPrefixExit)
{
	const VURegs initial = VU1;
	for (u32 i = 0; i < 64; i++)
	{
		const u32 mask = i % 16;
		const u32 dest = 1 + i % 7;
		const u32 source = 1 + (i + 1) % 7;
		const u32 upper = (mask << 21) | (source << 16) | (dest << 11) |
		                  (i % 3 == 0 ? 0x2bd : (dest << 6) | 0x29); // MADDA / MADD
		u32 lower;
		if (i % 3 == 0)
			lower = (mask << 21) | (source << 11) | 0x02000000; // SQ
		else if (i % 3 == 1)
			lower = (mask << 21) | (source << 16) | (dest << 11) | 0x8000033c; // MOVE
		else
			lower = ((i % 4) << 21) | (3 << 16) | (dest << 11) | 0x800003fc; // MTIR
		Put(i * 8, upper, lower);
	}
	for (u32 budget = 1; budget <= 160; budget++)
	{
		SCOPED_TRACE(budget);
		VU1 = initial;
		Compare(budget);
		if (HasFailure())
			return;
	}
}

TEST_F(VU1RecompilerTest, NativePreparationRetiresRandomizedIncomingPipelines)
{
	const VURegs initial = VU1;
	u32 random = 0xa5b467c9;
	auto next = [&random]() { random = random * 1664525 + 1013904223; return random; };
	for (u64 cycle : {u64(0), u64(0xffffffff), u64(0x100000000), ~u64(0) - 1, ~u64(0)})
	{
		for (u32 seed = 0; seed < 128; seed++)
		{
			SCOPED_TRACE(testing::Message() << "cycle=" << cycle << " seed=" << seed);
			VU1 = initial;
			VU1.cycle = cycle;
			VU1.VIBackupCycles = next() & 255;
			VU1.VI[REG_STATUS_FLAG].UL = next();
			VU1.VI[REG_CLIP_FLAG].UL = next();
			VU1.fmaccount = seed % 5;
			VU1.ialucount = (seed / 5) % 5;
			VU1.fmacreadpos = VU1.ialureadpos = seed % 4;
			VU1.fmacwritepos = (VU1.fmacreadpos + VU1.fmaccount) & 3;
			VU1.ialuwritepos = (VU1.ialureadpos + VU1.ialucount) & 3;
			for (u32 n = 0; n < 4; n++)
			{
				auto& fmac = VU1.fmac[(VU1.fmacreadpos + n) & 3];
				fmac.sCycle = cycle - (next() % 8);
				fmac.Cycle = 1 + next() % 8;
				fmac.flagreg = next();
				fmac.statusflag = next();
				fmac.macflag = next();
				fmac.clipflag = next();
				auto& ialu = VU1.ialu[(VU1.ialureadpos + n) & 3];
				ialu.sCycle = cycle - (next() % 8);
				ialu.Cycle = 1 + next() % 8;
			}
			VU1.fdiv.enable = seed & 1;
			VU1.efu.enable = seed & 2;
			VU1.fdiv.sCycle = cycle - next() % 8;
			VU1.efu.sCycle = cycle - next() % 8;
			VU1.fdiv.Cycle = 1 + next() % 8;
			VU1.efu.Cycle = 1 + next() % 8;
			VU1.fdiv.reg.UL = next();
			VU1.efu.reg.UL = next();
			VU1.fdiv.statusflag = next();
			// I-bit NOP creates no new pipe entry: isolate arbitrary incoming queues.
			Compare(1);
			if (HasFailure())
				return;
		}
	}
}


TEST_F(VU1RecompilerTest, ScheduledRetirementPreservesEveryBudgetPrefix)
{
	const float lhs[] = {-2.0f, 0.0f, 1.0f, 0.0f};
	const float rhs[] = {1.0f, 0.0f, -2.0f, 1.0f};
	std::memcpy(VU1.VF[1].F, lhs, sizeof(lhs));
	std::memcpy(VU1.VF[2].F, rhs, sizeof(rhs));
	const VURegs initial = VU1;
	for (u32 pattern = 0; pattern < 7; pattern++)
	{
		for (u32 i = 0; i < 64; i++)
		{
			u32 upper = 0x80000000 | (15 << 21) | (2 << 16) | (1 << 11) | ((3 + i % 5) << 6) | 0x28;
			u32 lower = 0x3f800000;
			if (pattern == 1 && i % 3 == 0)
				upper = 0x800002ff; // Gaps in the FMAC queue.
			if (pattern == 2)
			{
				upper = 0x2ff;
				lower = 0x8000033c | (15 << 21) | ((3 + i % 5) << 16) | (1 << 11);
			}
			if (pattern == 3 && i % 16 == 11)
				upper = (upper & ~(31 << 11)) | ((3 + (i - 1) % 5) << 11);
			if (pattern == 4)
			{
				upper &= ~0x80000000u;
				lower = 0x10000003 | ((2 + i % 3) << 16) | (1 << 11); // IADDIU
			}
			if (pattern == 5)
				upper = 0x80000000 | (15 << 21) | (2 << 16) | (3 << 11) | (3 << 6) | 0x28;
			if (pattern == 6)
			{
				const bool stall = i % 8 == 7;
				const u32 mask = stall ? 15 : 1 << (i % 4);
				const u32 source = stall ? 3 + (i - 1) % 5 : 1;
				upper = 0x80000000 | (mask << 21) | (2 << 16) | (source << 11) | ((3 + i % 5) << 6) | 0x28;
			}
			Put(i * 8, upper, lower);
		}
		for (u32 budget = 1; budget <= 64; budget++)
		{
			SCOPED_TRACE(testing::Message() << "pattern=" << pattern << " budget=" << budget);
			VU1 = initial;
			VU1.cycle = 100;
			VU1.VIBackupCycles = 2;
			VU1.fmacreadpos = 3;
			VU1.fmacwritepos = 2;
			VU1.fmaccount = 3;
			for (u32 j = 0; j < 3; j++)
			{
				auto& pipe = VU1.fmac[(3 + j) & 3];
				pipe.sCycle = 97 + j;
				pipe.Cycle = 4;
				pipe.regupper = 1;
				pipe.xyzwupper = 15;
				pipe.flagreg = j == 0 ? (1 << REG_CLIP_FLAG) : (1 << REG_STATUS_FLAG);
				pipe.statusflag = 0x135 + j;
				pipe.macflag = 0x2468 + j;
				pipe.clipflag = 0xabc + j;
			}
			Compare(budget);
			if (HasFatalFailure())
				return;
		}
	}
}

TEST_F(VU1RecompilerTest, DeferredSuffixMaterializesCompleteQueues)
{
	const VURegs initial = VU1, initial0 = VU0;
	for (u32 length : {15u, 24u, 32u, 63u, 64u, 65u, 96u, 128u, 255u, 256u, 257u, 511u, 512u, 513u})
	{
		for (u32 pattern = 0; pattern < 8; pattern++)
		{
			for (u32 i = 0; i < length; i++)
			{
				u32 upper = 0x80000000 | (15 << 21) | (2 << 16) | (1 << 11) | ((3 + i % 5) << 6) | 0x28;
				u32 lower = 0x3f800000;
				if (pattern <= 3 && i + pattern < length)
					upper = 0x800002ff; // Zero to three writes: retain untouched inactive slots.
				if (pattern == 4 && i % 3 == 0)
					upper = 0x800002ff;
				if (pattern == 5)
					upper = 0x80000000 | (15 << 21) | (2 << 16) | (3 << 11) | (3 << 6) | 0x28;
				if (pattern == 6)
				{
					upper &= ~0x80000000u;
					lower = 0x8000033c | (15 << 21) | (9 << 16) | ((3 + i % 5) << 11); // Paired old-value read.
				}
				if (pattern == 7)
				{
					upper = (upper & ~0x8000003fu) | (i % 2 ? 0x2b : 0x2c); // MAX/SUB flag preservation.
					lower = 0x10000003 | (2 << 16) | (1 << 11);
				}
				Put(i * 8, upper, lower);
			}
			Put(length * 8, 0xc00002ff, 0x3f800000); // Stop after a real E-bit delay pair.
			Put((length + 1) * 8, 0x800002ff, 0x3f800000);
			for (u32 ring = 0; ring < 4; ring++)
			{
				for (u32 budget : {length - 1, length, length + 1, 4 * length - 1, 4 * length, 4 * length + 1})
				{
					SCOPED_TRACE(testing::Message() << "length=" << length << " pattern=" << pattern << " ring=" << ring << " budget=" << budget);
					VU0 = initial0;
					VU1 = initial;
					VU1.cycle = 0xffffffffULL - 16;
					VU1.fmacreadpos = VU1.fmacwritepos = ring;
					VU1.fmaccount = 0;
					std::memset(VU1.fmac, 0xa5, sizeof(VU1.fmac));
					VU1.VI[REG_STATUS_FLAG].UL = 0xfedcba98;
					VU1.VI[REG_MAC_FLAG].UL = 0x12345678;
					VU1.macflag = 0xa5a51234;
					VU1.statusflag = 0x65432109;
					VU1.clipflag = 0x89abcdef;
					VU1.VIBackupCycles = ring == 3 ? 255 : 3;
					Compare(budget);
					if (HasFatalFailure())
						return;
				}
			}
		}
	}
}

TEST_F(VU1RecompilerTest, DeferredSuffixMixedInstructionsAndResume)
{
	const VURegs initial = VU1, initial0 = VU0;
	constexpr u32 ops[] = {0x00, 0x04, 0x08, 0x0c, 0x10, 0x14, 0x18,
		0x1c, 0x1d, 0x20, 0x21, 0x28, 0x29, 0x2a, 0x2b, 0x2c, 0x2d, 0x2f,
		0x13c, 0x17d, 0x1fd, 0x2bc, 0x2bd, 0x2ff};
	constexpr u32 lowers[] = {0x8000033c, 0x8000033d, 0x800003fc, 0x800003fd,
		0x10000003, 0x12000003, 0x80000030, 0x80000034, 0, 0x02000000};
	u32 random = 0x31415926;
	auto next = [&]() { random = random * 1664525 + 1013904223; return random >> 16; };
	for (u32 seed = 0; seed < 128; seed++)
	{
		SCOPED_TRACE(seed);
		VU0 = initial0;
		VU1 = initial;
		VU1.cycle = 100;
		VU1.fmacreadpos = VU1.fmacwritepos = seed & 3;
		std::memset(VU1.Mem, 0x3f, VU1_MEMSIZE);
		for (u32 i = 0; i < 128; i++)
		{
			const u32 op = ops[next() % std::size(ops)];
			const u32 dest = (op & 63) >= 60 ? 0 : (next() % 10) << 6;
			u32 upper = (next() % 16) << 21 | (next() % 10) << 16 | (next() % 10) << 11 | dest | op;
			u32 lower = (next() % 16) << 21 | (next() % 10) << 16 | (next() % 10) << 11 | lowers[next() % std::size(lowers)];
			if (next() & 1)
			{
				upper |= 0x80000000;
				lower = 0x3f800000;
			}
			Put(i * 8, upper, lower);
		}
		Put(128 * 8, 0xc00002ff, 0x3f800000);
		Put(129 * 8, 0x800002ff, 0x3f800000);
		// Partial execution followed by a full suffix, then continuation into
		// another block and the E-bit fallback must expose identical state.
		for (u32 budget : {1u, 7u, 31u, 128u, 512u})
		{
			Compare(budget);
			if (HasFatalFailure())
				return;
		}
	}
}

TEST_F(VU1RecompilerTest, LongBlockWrapAndLateCodeModification)
{
	const VURegs initial = VU1, initial0 = VU0;
	for (u32 i = 0; i < 512; i++)
		Put(i * 8, 0x80000000 | (15 << 21) | (2 << 16) | (3 << 11) | (3 << 6) | 0x28, 0x3f800000);
	Put(512 * 8, 0xc00002ff, 0x3f800000);
	Put(513 * 8, 0x800002ff, 0x3f800000);
	for (u64 distance : {129u, 255u, 259u, 260u, 511u, 515u, 516u, 1023u, 1027u, 1028u, 2047u, 2051u, 2052u})
	{
		for (u32 budget : {63u, 64u, 65u, 127u, 128u, 129u, 255u, 256u, 257u, 511u, 512u, 513u, 1023u, 1024u, 1025u, 2047u, 2048u, 2049u})
		{
			SCOPED_TRACE(testing::Message() << "distance=" << distance << " budget=" << budget);
			VU0 = initial0;
			VU1 = initial;
			VU1.cycle = ~u64(0) - distance;
			Compare(budget);
			if (HasFatalFailure())
				return;
		}
	}
	// Re-enter cached code whose first 128 pairs are unchanged.
	for (u32 pass = 0; pass < 3; pass++)
	{
		VU0 = initial0;
		VU1 = initial;
		VU1.cycle = 100;
		if (pass)
			Put((pass == 1 ? 191 : 447) * 8, 0x80000000 | (15 << 21) | (2 << 16) | (3 << 11) | (3 << 6) | 0x2a, 0x40000000);
		Compare(2048);
		if (HasFatalFailure())
			return;
	}
	for (u32 i = 0; i < 64; i++)
		Put(VU1_PROGSIZE - 512 + i * 8, 0x80000000 | (15 << 21) | (2 << 16) | (1 << 11) | (3 << 6) | 0x28, 0x3f800000);
	for (u32 budget : {63u, 64u, 65u, 128u})
	{
		VU0 = initial0;
		VU1 = initial;
		VU1.VI[REG_TPC].UL = (VU1_PROGSIZE - 512) / 8;
		Compare(budget);
		if (HasFatalFailure())
			return;
	}
}

TEST_F(VU1RecompilerTest, ScheduledReadinessAfterIncomingQueuesDrain)
{
	const VURegs initial = VU1, initial0 = VU0;
	for (u32 i = 0; i < 64; i++)
		Put(i * 8, (15 << 21) | (1 << 11) | (3 << 6) | 0x20, // ADDq observes division completion.
			0x10000003 | (2 << 16) | (1 << 11));
	Put(64 * 8, 0xc00002ff, 0x3f800000);
	Put(65 * 8, 0x800002ff, 0x3f800000);
	for (u32 variant = 0; variant < 6; variant++)
	{
		for (u32 latency : {1u, 2u, 6u, 7u, 8u, 16u})
		{
			for (u32 budget : {6u, 7u, 8u, 15u, 63u, 64u, 65u, 96u})
			{
				SCOPED_TRACE(testing::Message() << "variant=" << variant << " latency=" << latency << " budget=" << budget);
				VU0 = initial0;
				VU1 = initial;
				VU1.cycle = variant == 5 ? ~u64(0) - 200 : 100;
				VU1.VI[REG_Q].UL = 0x3f800000;
				if (variant == 0 || variant >= 3)
				{
					VU1.ialucount = 1;
					VU1.ialureadpos = 3;
					VU1.ialuwritepos = 0;
					VU1.ialu[3].sCycle = VU1.cycle;
					VU1.ialu[3].Cycle = latency;
					VU1.ialu[3].reg = 1 << 2;
				}
				if (variant == 1 || variant == 3)
				{
					VU1.fdiv.enable = 1;
					VU1.fdiv.sCycle = VU1.cycle;
					VU1.fdiv.Cycle = latency;
					VU1.fdiv.reg.UL = 0x40000000;
					VU1.fdiv.statusflag = 0xc30;
				}
				if (variant == 2 || variant == 3)
				{
					VU1.efu.enable = 1;
					VU1.efu.sCycle = VU1.cycle;
					VU1.efu.Cycle = latency;
					VU1.efu.reg.UL = 0x40400000;
				}
				if (variant == 4)
				{
					// Drained special work must not override invalid incoming FMAC timing.
					VU1.fmaccount = 1;
					VU1.fmacwritepos = 1;
					VU1.fmac[0].sCycle = VU1.cycle;
					VU1.fmac[0].Cycle = 8;
					VU1.fmac[0].regupper = 1;
					VU1.fmac[0].xyzwupper = 15;
				}
				Compare(budget);
				if (HasFatalFailure())
					return;
			}
		}
	}
}

TEST_F(VU1RecompilerTest, ScheduledRetirementHandlesSpecialAndIncomingState)
{
	const VURegs initial = VU1;
	for (u32 i = 0; i < 64; i++)
		Put(i * 8, 0x80000000 | (15 << 21) | (2 << 16) | (1 << 11) | (3 << 6) | 0x28, 0x3f800000);
	for (u32 variant = 0; variant < 7; variant++)
	{
		for (u32 budget : {8u, 9u, 16u, 31u, 40u})
		{
			SCOPED_TRACE(testing::Message() << "variant=" << variant << " budget=" << budget);
			VU1 = initial;
			VU1.cycle = variant == 5 ? ~u64(0) - 16 : 100;
			if (variant == 0)
			{
				VU1.fdiv.enable = 1;
				VU1.fdiv.sCycle = 100;
				VU1.fdiv.Cycle = 12;
				VU1.fdiv.statusflag = 0xc30;
			}
			else if (variant == 1)
			{
				VU1.efu.enable = 1;
				VU1.efu.sCycle = 100;
				VU1.efu.Cycle = 20;
			}
			else if (variant == 2)
			{
				VU1.ialucount = 1;
				VU1.ialuwritepos = 1;
				VU1.ialu[0].sCycle = 100;
				VU1.ialu[0].Cycle = 2;
			}
			else if (variant == 3 || variant == 4)
			{
				VU1.fmaccount = 1;
				VU1.fmacwritepos = 1;
				VU1.fmac[0].sCycle = variant == 3 ? 100 : 102;
				VU1.fmac[0].Cycle = variant == 3 ? 8 : 4;
				VU1.fmac[0].regupper = 1;
				VU1.fmac[0].xyzwupper = 15;
			}
			if (variant == 6)
			{
				VU1.fmaccount = 2;
				VU1.fmacwritepos = 2;
				for (u32 i = 0; i < 2; i++)
				{
					VU1.fmac[i].sCycle = 100;
					VU1.fmac[i].Cycle = 4;
				}
			}
			Compare(budget);
			if (HasFatalFailure())
				return;
		}
	}
}

TEST_F(VU1RecompilerTest, XgkickCreditTicksPreserveCycleWrapAndCachedValues)
{
	const VURegs initial = VU1;
	for (u32 i = 0; i < 32; i++)
		Put(i * 8, 0x80000000 | (15 << 21) | (2 << 16) | (1 << 11) | ((3 + i % 4) << 6) | 0x28, 0x3f800000);
	for (u64 cycle : {u64(0), u64(100), u64(0xffffffff), ~u64(0) - 1, ~u64(0)})
	{
		for (u32 ahead : {0u, 1u})
		{
			SCOPED_TRACE(testing::Message() << "cycle=" << cycle << " ahead=" << ahead);
			VU1 = initial;
			VU1.cycle = cycle;
			VU1.VIBackupCycles = 2;
			VU1.xgkickenable = 1;
			VU1.xgkicklastcycle = cycle + ahead;
			VU1.xgkickcyclecount = ahead;
			// Never accumulate two transfer cycles; no GIF packet is required.
			Compare(2);
			if (HasFailure())
				return;
			EXPECT_EQ(VU1.xgkickcyclecount, 1u);
			EXPECT_EQ(VU1.xgkicklastcycle, cycle + 1);
		}
	}
}

TEST_F(VU1RecompilerTest, PipelineCalloutPublishesAndReloadsVectorCache)
{
	using namespace vixl::aarch64;
	using Wrapper = void (*)(VURegs*, const Arm64VU1::Instruction*, const u32*, const VECTOR*, VECTOR*);
	std::array<Wrapper, 9> wrappers;
	u8* const base = SysMemory::GetVU1Rec();
	HostSys::BeginCodeWrite();
	const auto pipeline = Arm64VU1::CompilePipeline(base, SysMemory::GetVU1RecEnd() - base, &ObservePipelineCallout);
	u8* write = base + ((pipeline.size + 15) & ~size_t(15));
	for (u32 entry = 0; entry < wrappers.size(); entry++)
	{
		wrappers[entry] = reinterpret_cast<Wrapper>(write);
		MacroAssembler a(write, SysMemory::GetVU1RecEnd() - write);
		a.Stp(x19, x24, MemOperand(sp, -96, PreIndex));
		a.Stp(x25, lr, MemOperand(sp, 16));
		for (u32 slot = 0; slot < 8; slot += 2)
			a.Stp(VRegister(8 + slot, 64), VRegister(9 + slot, 64), MemOperand(sp, 32 + slot * 8));
		a.Mov(x19, x0);
		a.Mov(x0, x1);
		a.Mov(x24, x2);
		a.Mov(x25, x4);
		for (u32 slot = 0; slot < 8; slot++)
			a.Ldr(VRegister(8 + slot, 128), MemOperand(x3, sizeof(VECTOR) * slot));
		const void* target = entry == 8 ? pipeline.flush_kick : pipeline.branch_prepare;
		if (entry == 7)
			target = pipeline.finish_packet;
		if (entry < pipeline.prepare.size())
			target = pipeline.prepare[entry];
		a.Mov(x16, reinterpret_cast<uintptr_t>(target));
		a.Blr(x16);
		for (u32 slot = 0; slot < 8; slot++)
			a.Str(VRegister(8 + slot, 128), MemOperand(x25, sizeof(VECTOR) * slot));
		for (u32 slot = 0; slot < 8; slot += 2)
			a.Ldp(VRegister(8 + slot, 64), VRegister(9 + slot, 64), MemOperand(sp, 32 + slot * 8));
		a.Ldp(x25, lr, MemOperand(sp, 16));
		a.Ldp(x19, x24, MemOperand(sp, 96, PostIndex));
		a.Ret();
		a.FinalizeCode();
		write += (a.GetSizeOfCodeGenerated() + 15) & ~size_t(15);
	}
	HostSys::EndCodeWrite();
	HostSys::FlushInstructionCache(base, static_cast<u32>(write - base));

	const VURegs initial = VU1;
	Arm64VU1::Instruction ins{};
	ins.pc = 40;
	ins.upper = 0x800002ff;
	ins.lower = 0x3f800000;
	ins.lregs.VIread = 1 << 2;
	for (u32 entry = 0; entry < wrappers.size(); entry++)
	{
		for (u32 count : {0u, 1u, 8u})
		{
			SCOPED_TRACE(testing::Message() << "entry=" << entry << " count=" << count);
			VU1 = initial;
			VU1.cycle = 100;
			VU1.VIBackupCycles = 3;
			VU1.xgkickenable = entry == 7 ? VURegs::XgkickPacket : 1;
			VU1.xgkicklastcycle = 98;
			s_transfer_calls = 0;
			std::array<u32, 8> offsets;
			offsets.fill(~u32(0));
			std::array<VECTOR, 8> cached{}, output{};
			for (u32 slot = 0; slot < 8; slot++)
			{
				if (slot < count)
					offsets[slot] = slot == 7 ? offsetof(VURegs, ACC) : offsetof(VURegs, VF) + sizeof(VECTOR) * (1 + slot);
				for (u32 lane = 0; lane < 4; lane++)
					cached[slot].UL[lane] = 0x5a000000 | (slot << 8) | lane;
			}
			wrappers[entry](&VU1, &ins, offsets.data(), cached.data(), output.data());
			ASSERT_EQ(s_transfer_calls, 1u);
			EXPECT_EQ(s_transfer_cycles, entry >= 7 ? 0 : 2);
			EXPECT_EQ(s_transfer_flush, entry >= 7);
			EXPECT_EQ(VU1.cycle, entry >= 7 ? 357u : 358u);
			EXPECT_EQ(VU1.VIBackupCycles, entry >= 7 ? 3u : 1u);
			EXPECT_EQ(VU1.VI[REG_TPC].UL, entry >= 7 ? initial.VI[REG_TPC].UL : 48u);
			EXPECT_EQ(VU1.code, entry >= 7 ? initial.code : ins.upper);
			for (u32 slot = 0; slot < count; slot++)
			{
				EXPECT_EQ(std::memcmp(&s_seen_vectors[slot], &cached[slot], sizeof(VECTOR)), 0);
				for (u32 lane = 0; lane < 4; lane++)
					EXPECT_EQ(output[slot].UL[lane], 0xa5000000u | (slot << 8) | lane);
			}
		}
	}
}

TEST_F(VU1RecompilerTest, DeferredProducerFlagsSurviveNonArithmeticPairs)
{
	const VURegs initial = VU1, initial0 = VU0;
	for (u32 mask = 0; mask < 16; mask++)
		for (u32 pattern = 0; pattern < 4; pattern++)
		{
			for (u32 i = 0; i < 64; i++)
			{
				const bool arithmetic = pattern == 3 || (pattern == 1 && i == 0) || (pattern == 2 && i % 5 == 0);
				const u32 upper = arithmetic ? (mask << 21) | (2 << 16) | (1 << 11) | (3 << 6) | 0x2a : 0x2ff;
				Put(i * 8, upper, 0x8000033c | (mask << 21) | (4 << 16) | (3 << 11));
			}
			Put(64 * 8, 0xc00002ff, 0x3f800000);
			for (u32 budget : {7u, 8u, 15u, 16u, 31u, 32u, 63u, 64u, 65u, 256u})
			{
				SCOPED_TRACE(testing::Message() << mask << "/" << pattern << "/" << budget);
				VU1 = initial;
				VU0 = initial0;
				VU1.macflag = 0xa5a5ffff;
				VU1.statusflag = 0x89abcdef;
				VU1.clipflag = 0x76543210;
				const u32 values[] = {0, 0x80000000, 0x7f7fffff, 0x80800000};
				const u32 factors[] = {0x3f800000, 0x40000000, 0x40000000, 0x3f000000};
				std::memcpy(&VU1.VF[1], values, sizeof(values));
				std::memcpy(&VU1.VF[2], factors, sizeof(factors));
				Compare(budget);
				if (HasFatalFailure())
					return;
			}
		}
}

TEST_F(VU1RecompilerTest, MacFlagLaneWeightsMatchEveryMask)
{
	const VURegs initial = VU1, initial0 = VU0;
	constexpr u32 inputs[] = {0, 0x80000000, 0x00800000, 0x80800000,
		0x7f7fffff, 0xff7fffff, 0x7fc12345, 0xff800000};
	constexpr u32 factors[] = {0x3f000000, 0x40000000, 0xbf800000, 0};
	for (u32 options = 0; options < 32; options++)
	{
		EmuConfig.Cpu.VU1FPCR = FPControlRegister::GetDefault().DisableExceptions().SetRoundMode(static_cast<FPRoundMode>(options & 3)).SetFlushToZero(options & 4);
		EmuConfig.Cpu.Recompiler.vu0Overflow = (options & 8) != 0;
		EmuConfig.Cpu.Recompiler.vu1Overflow = (options & 16) != 0;
		for (u32 mask = 0; mask < 16; mask++)
			for (u32 rotation = 0; rotation < std::size(inputs); rotation++)
				for (u32 budget : {1u, 8u})
				{
					SCOPED_TRACE(testing::Message() << options << "/" << mask << "/" << rotation << "/" << budget);
					VU1 = initial;
					VU0 = initial0;
					VU1.macflag = 0xa5a5ffff;
					VU1.statusflag = 0xffffffff;
					for (u32 lane = 0; lane < 4; lane++)
					{
						VU1.VF[1].UL[lane] = inputs[(rotation + lane) % std::size(inputs)];
						VU1.VF[2].UL[lane] = factors[lane];
					}
					Put(0, 0x80000000 | (mask << 21) | (2 << 16) | (1 << 11) | (3 << 6) | 0x2a, 0x3f800000);
					Compare(budget); // Inspect both freshly issued and retired flag records.
					if (HasFatalFailure())
						return;
				}
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

namespace
{
	class VU1PacketXgkickTest : public VU1RecompilerTest
	{
	protected:
		void SetUp() override
		{
			VU1RecompilerTest::SetUp();
			m_provider = CpuVU1;
			m_gamefixes = EmuConfig.Gamefixes;
			m_cpu_regs = cpuRegs;
			m_vifstat = vif1Regs.stat._u32;
			CpuVU1 = &CpuArm64VU1;
			EmuConfig.Gamefixes.XgKickHack = false;
			vif1Regs.stat.VGW = false;
			gifUnit.Reset();
			// Buffer packets while the GIF is paused, avoiding an MTGS thread.
			gifRegs.ctrl.PSE = 1;
		}

		void TearDown() override
		{
			vif1Regs.stat.VGW = false;
			gifUnit.Reset();
			vif1Regs.stat._u32 = m_vifstat;
			cpuRegs = m_cpu_regs;
			EmuConfig.Gamefixes = m_gamefixes;
			CpuVU1 = m_provider;
			VU1RecompilerTest::TearDown();
		}

		void Tag(u32 addr, u32 loops, bool eop)
		{
			Gif_Tag::HW_Gif_Tag tag{};
			tag.NLOOP = loops;
			tag.EOP = eop;
			tag.FLG = GIF_FLG_PACKED;
			tag.NREG = 1;
			tag.REGS[0] = GIF_REG_RGBA;
			std::memcpy(VU1.Mem + addr, &tag, sizeof(tag));
		}

		void Kick(u32 addr = 0)
		{
			VU1.VI[1].UL = addr / 16;
			Put(0, 0x2ff, 0x800006fc | (1 << 11));
			CpuVU1->Execute(1);
			ASSERT_TRUE(VU1.xgkickenable);
			ASSERT_EQ(gifUnit.gifPath[0].curSize, 0u);
		}

		BaseVUmicroCPU* m_provider;
		Pcsx2Config::GamefixOptions m_gamefixes;
		cpuRegisters m_cpu_regs;
		u32 m_vifstat;
	};
} // namespace

TEST_F(VU1PacketXgkickTest, DelaysThroughNextInstructionAndObservesItsStore)
{
	Tag(0, 2, true);
	Kick();
	// The instruction immediately after XGKICK can still populate its data.
	VU1.VI[2].UL = 1;
	Put(8, 0x2ff, 0x02000000 | (15 << 21) | (2 << 16) | (3 << 11));
	vif1Regs.stat.VGW = true;
	const u64 cycle = VU1.cycle;
	CpuArm64VU1.Execute(1);
	EXPECT_EQ(VU1.cycle, cycle + 1);
	EXPECT_FALSE(VU1.xgkickenable);
	EXPECT_EQ(VU0.VI[REG_VPU_STAT].UL & (1 << 12), 0u);
	EXPECT_FALSE(vif1Regs.stat.VGW);
	EXPECT_NE(cpuRegs.interrupt & (1 << DMAC_VIF1), 0u);
	EXPECT_EQ(gifUnit.gifPath[0].curSize, 48u);
	EXPECT_EQ(std::memcmp(gifUnit.gifPath[0].buffer, VU1.Mem, 48), 0);
	EXPECT_EQ(std::memcmp(gifUnit.gifPath[0].buffer + 16, &VU1.VF[3], 16), 0);
}

TEST_F(VU1PacketXgkickTest, BranchDelayStoresBeforePendingPacketTransfer)
{
	Tag(0, 2, true);
	Kick();
	VU1.VI[2].UL = 1;
	VU1.branch = 1;
	VU1.branchpc = 64;
	Put(8, 0x2ff, 0x02000000 | (15 << 21) | (2 << 16) | (3 << 11));
	CpuArm64VU1.Execute(1);
	EXPECT_EQ(VU1.branch, 0u);
	EXPECT_EQ(VU1.VI[REG_TPC].UL, 64u / 8);
	EXPECT_FALSE(VU1.xgkickenable);
	ASSERT_EQ(gifUnit.gifPath[0].curSize, 48u);
	EXPECT_EQ(std::memcmp(gifUnit.gifPath[0].buffer + 16, &VU1.VF[3], 16), 0);
}

TEST_F(VU1PacketXgkickTest, ConnectedBranchDoesNotRunDelayBeforePendingTransfer)
{
	Tag(0, 2, true);
	Kick();
	VU1.VI[2].UL = 1;
	Put(8, 0x2ff, 0x40000006); // B to byte 64.
	Put(16, 0x2ff, 0x02000000 | (15 << 21) | (2 << 16) | (3 << 11));
	std::array<u8, 16> previous{};
	std::memcpy(previous.data(), VU1.Mem + 16, previous.size());
	CpuArm64VU1.Execute(1);
	ASSERT_EQ(gifUnit.gifPath[0].curSize, 48u);
	EXPECT_FALSE(VU1.xgkickenable);
	EXPECT_EQ(VU1.branch, 1u);
	EXPECT_EQ(std::memcmp(gifUnit.gifPath[0].buffer + 16, previous.data(), previous.size()), 0);
	CpuArm64VU1.Execute(1);
	EXPECT_EQ(VU1.branch, 0u);
	EXPECT_EQ(VU1.VI[REG_TPC].UL, 64u / 8);
	EXPECT_EQ(std::memcmp(VU1.Mem + 16, &VU1.VF[3], 16), 0);
	EXPECT_EQ(std::memcmp(gifUnit.gifPath[0].buffer + 16, previous.data(), previous.size()), 0);
}

TEST_F(VU1PacketXgkickTest, ConditionalConnectionPublishesBeforePendingTransfer)
{
	Tag(0, 2, true);
	Kick();
	VU1.VI[2].UL = 1;
	Put(8, 0x2ff, 0x5a000000 | (2 << 11) | 6);
	Put(16, 0x2ff, 0x02000000 | (15 << 21) | (2 << 16) | (3 << 11));
	std::array<u8, 16> previous{};
	std::memcpy(previous.data(), VU1.Mem + 16, previous.size());
	// A larger caller budget must still publish the branch pair and complete
	// the pending transfer before allowing the connected delay store.
	CpuArm64VU1.Execute(2);
	ASSERT_EQ(gifUnit.gifPath[0].curSize, 48u);
	EXPECT_FALSE(VU1.xgkickenable);
	EXPECT_EQ(VU1.branch, 0u);
	EXPECT_EQ(VU1.VI[REG_TPC].UL, 64u / 8);
	EXPECT_EQ(std::memcmp(VU1.Mem + 16, &VU1.VF[3], 16), 0);
	EXPECT_EQ(std::memcmp(gifUnit.gifPath[0].buffer + 16, previous.data(), previous.size()), 0);
}

TEST_F(VU1PacketXgkickTest, NativeKickMatchesReferenceStepsAcrossEveryBudget)
{
	const VURegs initial = VU1, initial0 = VU0;
	std::array<u8, VU1_MEMSIZE> memory;
	std::memcpy(memory.data(), VU1.Mem, memory.size());
	const u32 add = (15 << 21) | (2 << 16) | (3 << 11) | (3 << 6) | 0x28;
	const u32 kick = 0x800006fc | (1 << 11);
	constexpr u32 budgets[] = {240, 240, 240, 1040, 512, 512};
	for (bool gamefix : {false, true})
		for (u32 variant = 0; variant < std::size(budgets); variant++)
			for (u32 old_mode : {0u, 1u, VURegs::XgkickPacket})
			{
				EmuConfig.Gamefixes.XgKickHack = gamefix;
				for (u32 i = 0; i < 260; i++)
					Put(i * 8, 0x80000000 | add, 0x3f800000);
				Put(0, add, kick);
				Put(8, add, 0x02000000 | (15 << 21) | (2 << 16) | (3 << 11));
				Put(16, add, variant == 0 ? kick : 0x40000000 | (48 - 3));
				Put(24, add, kick); // Consecutive kick or branch-delay kick.
				Put(52 * 8, add, kick);
				if (variant == 2)
					Put(53 * 8, 0x2ff, 0x5a000000 | (2 << 11) | 2);
				Put(80 * 8, 0xc00002ff, 0x3f800000);
				if (variant == 3)
				{
					for (u32 pc : {0u, 8u, 16u, 24u, 52u * 8, 80u * 8})
						Put(pc, 0x80000000 | add, 0x3f800000);
					Put(255 * 8, add, kick); // Last pair of the bounded trace.
					Put(256 * 8, add, 0x02000000 | (15 << 21) | (2 << 16) | (3 << 11));
					Put(258 * 8, 0xc00002ff, 0x3f800000);
				}
				if (variant >= 4)
				{
					for (u32 pc : {16u, 24u})
						Put(pc, 0x80000000 | add, 0x3f800000);
					Put(30 * 8, add, 0x12000001 | (4 << 16) | (4 << 11));
					Put(32 * 8, add, 0x5a000000 | (4 << 11) | ((-33) & 0x7ff));
					Put(36 * 8, 0xc00002ff, 0x3f800000);
					if (variant == 5)
					{
						Put(0, add, 0x02000000 | (15 << 21) | (2 << 16) | (3 << 11));
						Put(33 * 8, add, kick); // Carry this request across the native back edge.
					}
				}
				auto reset = [&]() {
					VU1 = initial;
					VU0 = initial0;
					VU1.VI[1].UL = 0;
					VU1.VI[2].UL = 1;
					VU1.VI[4].UL = 3;
					std::memcpy(VU1.Mem, memory.data(), memory.size());
					Tag(0, 2, true);
					Tag(64, 1, true);
					if (old_mode)
					{
						VU1.xgkickenable = old_mode;
						VU1.xgkickaddr = 64;
						VU1.xgkickdiff = VU1_MEMSIZE - 64;
					}
					gifUnit.Reset();
					gifRegs.ctrl.PSE = 1;
					vif1Regs.stat.VGW = false;
				};
				for (u32 budget = 1; budget <= budgets[variant]; budget++)
				{
					SCOPED_TRACE(testing::Message() << gamefix << "/" << variant << "/" << old_mode << "/" << budget);
					reset();
					const u64 start = VU1.cycle;
					while (VU1.cycle - start < budget)
					{
						if (!(VU0.VI[REG_VPU_STAT].UL & 0x100))
							break;
						CpuArm64VU1.Step(); // Original opcode interpreter, with native packet policy.
					}
					VU1.VI[REG_TPC].UL >>= 3;
					VU1.nextBlockCycles = (VU1.cycle - cpuRegs.cycle) + 1;
					const VURegs expected = VU1, expected0 = VU0;
					std::array<u8, VU1_MEMSIZE> expected_memory;
					std::memcpy(expected_memory.data(), VU1.Mem, expected_memory.size());
					const u32 size = gifUnit.gifPath[0].curSize;
					std::array<u8, 4096> packet;
					ASSERT_LE(size, packet.size());
					std::memcpy(packet.data(), gifUnit.gifPath[0].buffer, size);
					reset();
					CpuArm64VU1.Execute(budget);
					ASSERT_EQ(std::memcmp(&VU1, &expected, sizeof(VU1)), 0);
					ASSERT_EQ(std::memcmp(&VU0, &expected0, sizeof(VU0)), 0);
					ASSERT_EQ(std::memcmp(VU1.Mem, expected_memory.data(), expected_memory.size()), 0);
					ASSERT_EQ(gifUnit.gifPath[0].curSize, size);
					ASSERT_EQ(std::memcmp(gifUnit.gifPath[0].buffer, packet.data(), size), 0);
				}
			}
}

TEST_F(VU1PacketXgkickTest, NativePacketContinuationMatchesSplitExecution)
{
	Tag(0, 2, true);
	Kick();
	VU1.VI[2].UL = 1;
	const u32 add = (15 << 21) | (2 << 16) | (3 << 11) | (3 << 6) | 0x28;
	for (u32 i = 1; i < 80; i++)
		Put(i * 8, 0x80000000 | add, 0x3f800000);
	Put(8, add, 0x02000000 | (15 << 21) | (2 << 16) | (3 << 11));
	Put(16, add, 0x02000000 | (15 << 21) | (2 << 16) | (4 << 11));
	Put(24 * 8, add, 0x5a000000 | (2 << 11) | (48 - 25));
	Put(72 * 8, 0xc00002ff, 0x3f800000);
	const VURegs initial = VU1, initial0 = VU0;
	std::array<u8, VU1_MEMSIZE> memory;
	std::memcpy(memory.data(), VU1.Mem, memory.size());
	auto reset = [&]() {
		VU1 = initial;
		VU0 = initial0;
		std::memcpy(VU1.Mem, memory.data(), memory.size());
		gifUnit.Reset();
		gifRegs.ctrl.PSE = 1;
		vif1Regs.stat.VGW = false;
	};
	for (u32 budget = 1; budget <= 260; budget++)
	{
		SCOPED_TRACE(budget);
		reset();
		CpuArm64VU1.Execute(1);
		const u64 elapsed = VU1.cycle - initial.cycle;
		if (elapsed < budget)
			CpuArm64VU1.Execute(budget - elapsed);
		const VURegs expected = VU1, expected0 = VU0;
		std::array<u8, VU1_MEMSIZE> expected_memory;
		std::memcpy(expected_memory.data(), VU1.Mem, expected_memory.size());
		ASSERT_EQ(gifUnit.gifPath[0].curSize, 48u);
		std::array<u8, 48> packet;
		std::memcpy(packet.data(), gifUnit.gifPath[0].buffer, packet.size());
		reset();
		CpuArm64VU1.Execute(budget);
		EXPECT_EQ(std::memcmp(&VU1, &expected, sizeof(VU1)), 0);
		EXPECT_EQ(std::memcmp(&VU0, &expected0, sizeof(VU0)), 0);
		EXPECT_EQ(std::memcmp(VU1.Mem, expected_memory.data(), expected_memory.size()), 0);
		ASSERT_EQ(gifUnit.gifPath[0].curSize, packet.size());
		EXPECT_EQ(std::memcmp(gifUnit.gifPath[0].buffer, packet.data(), packet.size()), 0);
	}
}

TEST_F(VU1PacketXgkickTest, WrapsMemoryAndTransfersAllTagsThroughEop)
{
	Tag(0x3ff0, 1, false);
	Tag(16, 2, true);
	Kick(0x3ff0);
	CpuArm64VU1.Execute(2);
	ASSERT_EQ(gifUnit.gifPath[0].curSize, 80u);
	EXPECT_EQ(std::memcmp(gifUnit.gifPath[0].buffer, VU1.Mem + 0x3ff0, 16), 0);
	EXPECT_EQ(std::memcmp(gifUnit.gifPath[0].buffer + 16, VU1.Mem, 64), 0);
	EXPECT_EQ(VU1.xgkickaddr, 64u);
	EXPECT_FALSE(VU1.xgkickenable);
}

TEST_F(VU1PacketXgkickTest, FlushDoesNotChargePerQwordVuCycles)
{
	Tag(0, 32, true);
	Kick();
	const u64 cycle = VU1.cycle;
	_vuXGKICKTransfer(0, true);
	EXPECT_EQ(VU1.cycle, cycle);
	EXPECT_EQ(gifUnit.gifPath[0].curSize, 33u * 16);
	EXPECT_FALSE(VU1.xgkickenable);
}

TEST_F(VU1PacketXgkickTest, InterpreterAndGamefixRetainIncrementalTransfers)
{
	for (bool interpreter : {false, true})
	{
		SCOPED_TRACE(interpreter);
		VU1.VI[REG_TPC].UL = 0;
		gifUnit.Reset();
		gifRegs.ctrl.PSE = 1;
		CpuVU1 = interpreter ? static_cast<BaseVUmicroCPU*>(&CpuIntVU1) : &CpuArm64VU1;
		EmuConfig.Gamefixes.XgKickHack = !interpreter;
		Tag(0, 2, true);
		Kick();
		CpuVU1->Execute(2);
		EXPECT_EQ(gifUnit.gifPath[0].curSize, 16u);
		EXPECT_EQ(VU1.xgkicksizeremaining, 32u);
		EXPECT_TRUE(VU1.xgkickenable);
		_vuXGKICKTransfer(0, true);
	}
}

TEST_F(VU1PacketXgkickTest, RestoredPartialTagFinishesWithoutReparsingPayload)
{
	Tag(0, 2, true);
	Kick();
	EmuConfig.Gamefixes.XgKickHack = true;
	_vuXGKICKTransfer(1, false);
	ASSERT_EQ(gifUnit.gifPath[0].curSize, 16u);
	ASSERT_EQ(VU1.xgkicksizeremaining, 32u);
	EmuConfig.Gamefixes.XgKickHack = false;
	_vuXGKICKTransfer(2, false);
	EXPECT_EQ(gifUnit.gifPath[0].curSize, 32u);
	EXPECT_TRUE(VU1.xgkickenable);
	_vuXGKICKTransfer(2, false);
	EXPECT_EQ(gifUnit.gifPath[0].curSize, 48u);
	EXPECT_FALSE(VU1.xgkickenable);
	EXPECT_EQ(std::memcmp(gifUnit.gifPath[0].buffer, VU1.Mem, 48), 0);
}

TEST_F(VU1PacketXgkickTest, OversizedPacketCancelsWithoutCopying)
{
	Tag(0, 1023, true);
	Kick();
	CpuArm64VU1.Execute(2);
	EXPECT_FALSE(VU1.xgkickenable);
	EXPECT_EQ(gifUnit.gifPath[0].curSize, 0u);
	EXPECT_EQ(VU0.VI[REG_VPU_STAT].UL & (1 << 12), 0u);
}


TEST_F(VU1PacketXgkickTest, StalledFollowingStoreCompletesBeforePacketCopy)
{
	Tag(0, 1, true);
	Kick();
	VU1.fmac[0] = {};
	VU1.fmac[0].regupper = 3;
	VU1.fmac[0].xyzwupper = 15;
	VU1.fmac[0].sCycle = VU1.cycle;
	VU1.fmac[0].Cycle = 4;
	VU1.fmacreadpos = 0;
	VU1.fmacwritepos = 1;
	VU1.fmaccount = 1;
	VU1.VI[2].UL = 1;
	Put(8, 0x2ff, 0x02000000 | (15 << 21) | (2 << 16) | (3 << 11));
	const u64 cycle = VU1.cycle;
	CpuArm64VU1.Execute(1);
	EXPECT_EQ(VU1.cycle, cycle + 4);
	ASSERT_EQ(gifUnit.gifPath[0].curSize, 32u);
	EXPECT_EQ(std::memcmp(gifUnit.gifPath[0].buffer + 16, &VU1.VF[3], 16), 0);
}

TEST_F(VU1PacketXgkickTest, FollowingStoreInInterpreterFallbackCompletesBeforeCopy)
{
	Tag(0, 1, true);
	Kick();
	VU1.VI[2].UL = 1;
	Put(8, 0x400002ff, 0x02000000 | (15 << 21) | (2 << 16) | (3 << 11));
	CpuArm64VU1.Execute(1); // E-bit pair is interpreted.
	ASSERT_EQ(gifUnit.gifPath[0].curSize, 32u);
	EXPECT_EQ(std::memcmp(gifUnit.gifPath[0].buffer + 16, &VU1.VF[3], 16), 0);
	EXPECT_FALSE(VU1.xgkickenable);
}

TEST_F(VU1PacketXgkickTest, ConsecutiveKicksFlushOldRequestAndDelayNewRequest)
{
	Tag(0, 1, true);
	Tag(64, 2, true);
	Kick();
	VU1.VI[2].UL = 4;
	Put(8, 0x2ff, 0x800006fc | (2 << 11));
	CpuArm64VU1.Execute(1);
	EXPECT_EQ(gifUnit.gifPath[0].curSize, 32u);
	EXPECT_TRUE(VU1.xgkickenable);
	EXPECT_EQ(VU1.xgkickaddr, 64u);
	CpuArm64VU1.Execute(1);
	ASSERT_EQ(gifUnit.gifPath[0].curSize, 80u);
	EXPECT_EQ(std::memcmp(gifUnit.gifPath[0].buffer + 32, VU1.Mem + 64, 48), 0);
	EXPECT_FALSE(VU1.xgkickenable);
}

TEST_F(VU1PacketXgkickTest, PendingDelaySurvivesCycleCounterWrap)
{
	for (u64 cycle : {u64(0xffffffff) - 1, ~u64(0) - 1})
	{
		SCOPED_TRACE(cycle);
		gifUnit.Reset();
		gifRegs.ctrl.PSE = 1;
		VU1.VI[REG_TPC].UL = 0;
		VU1.cycle = cycle;
		Tag(0, 1, true);
		Kick();
		CpuArm64VU1.Execute(1);
		EXPECT_EQ(gifUnit.gifPath[0].curSize, 32u);
		EXPECT_FALSE(VU1.xgkickenable);
		EXPECT_EQ(VU1.cycle, cycle + 2);
	}
}


TEST_F(VU1PacketXgkickTest, EndBitDelayFlushesPacketWithoutChargingTransferCycles)
{
	Tag(0, 16, true);
	VU1.VI[1].UL = 0;
	Put(0, 0x400002ff, 0x800006fc | (1 << 11));
	CpuArm64VU1.Execute(1);
	ASSERT_TRUE(VU1.xgkickenable);
	ASSERT_EQ(gifUnit.gifPath[0].curSize, 0u);
	const u64 cycle = VU1.cycle;
	CpuArm64VU1.Execute(1);
	EXPECT_EQ(VU1.cycle, cycle + 1);
	EXPECT_EQ(gifUnit.gifPath[0].curSize, 17u * 16);
	EXPECT_FALSE(VU1.xgkickenable);
	EXPECT_EQ(VU0.VI[REG_VPU_STAT].UL & 0x1100, 0u);
}


TEST_F(VU1PacketXgkickTest, RestoredIncrementalRequestStaysIncrementalBetweenTags)
{
	Tag(0, 1, false);
	Tag(32, 2, true);
	Kick();
	// Old save states use enable=1, including at a tag boundary with no
	// remaining bytes. Such a request must not become a delayed native kick.
	VU1.xgkickenable = 1;
	_vuXGKICKTransfer(3, false);
	VU1.cycle += 3;
	ASSERT_EQ(gifUnit.gifPath[0].curSize, 32u);
	ASSERT_EQ(VU1.xgkicksizeremaining, 0u);
	CpuArm64VU1.Execute(1);
	EXPECT_EQ(gifUnit.gifPath[0].curSize, 32u);
	EXPECT_EQ(VU1.xgkickenable, 1u);
	_vuXGKICKTransfer(2, false);
	EXPECT_EQ(gifUnit.gifPath[0].curSize, 48u);
	EXPECT_EQ(VU1.xgkicksizeremaining, 32u);
	_vuXGKICKTransfer(0, true);
	EXPECT_EQ(gifUnit.gifPath[0].curSize, 80u);
	EXPECT_FALSE(VU1.xgkickenable);
}

#endif
