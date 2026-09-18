// SPDX-FileCopyrightText: 2002-2026 PCSX2 Dev Team
// SPDX-License-Identifier: GPL-3.0+

#include "VUmicro.h"

#include <gtest/gtest.h>

#include <array>
#include <cstring>

namespace
{
	constexpr u32 NOP = 0x2ff;
	constexpr u32 IBit = 0x80000000;

	constexpr u32 Add(u32 dest, u32 lhs, u32 rhs)
	{
		return (15 << 21) | (rhs << 16) | (lhs << 11) | (dest << 6) | 0x28;
	}

	constexpr u32 Move(u32 dest, u32 src)
	{
		return 0x8000033c | (15 << 21) | (dest << 16) | (src << 11);
	}

	class VU1InterpreterTest : public testing::Test
	{
	protected:
		void SetUp() override
		{
			m_saved = VU1;
			std::memset(&VU1, 0, sizeof(VU1));
			VU1.idx = 1;
			VU1.Micro = reinterpret_cast<u8*>(m_program.data());
			VU1.Mem = reinterpret_cast<u8*>(m_memory.data());
			VU1.VF[0].f.w = 1.0f;
			for (u32 lane = 0; lane < 4; lane++)
			{
				VU1.VF[1].F[lane] = 2.0f;
				VU1.VF[2].F[lane] = 3.0f;
				VU1.VF[3].F[lane] = 5.0f;
				m_memory[lane] = 0x41400000; // 12.0f
			}
		}

		void TearDown() override { VU1 = m_saved; }

		void Run(u32 upper, u32 lower, u32 pc = 0)
		{
			m_program[pc / 4] = lower;
			m_program[pc / 4 + 1] = upper;
			VU1.VI[REG_TPC].UL = pc;
			CpuIntVU1.Step();
		}

		void ExpectVector(u32 reg, float value)
		{
			for (float lane : VU1.VF[reg].F)
				EXPECT_EQ(lane, value);
		}

		VURegs m_saved;
		alignas(16) std::array<u32, VU1_PROGSIZE / 4> m_program{};
		alignas(16) std::array<u32, 0x4000 / 4> m_memory{};
	};
} // namespace

TEST_F(VU1InterpreterTest, RepeatedInstructionUsesCurrentRegisterValues)
{
	Run(Add(4, 1, 2), Move(5, 3));
	ExpectVector(4, 5.0f);
	ExpectVector(5, 5.0f);
	for (float& lane : VU1.VF[1].F)
		lane = 7.0f;
	Run(Add(4, 1, 2), Move(5, 3));
	ExpectVector(4, 10.0f);
}

TEST_F(VU1InterpreterTest, ChangedUpperInstructionUpdatesWriteConflict)
{
	const u32 loadVF1 = (15 << 21) | (1 << 16); // LQ.xyzw vf1, 0(vi0)
	Run(Add(1, 2, 3), loadVF1);
	ExpectVector(1, 8.0f); // The upper write discards the lower write.
	Run(Add(4, 2, 3), loadVF1);
	ExpectVector(1, 12.0f);
	ExpectVector(4, 8.0f);
}

TEST_F(VU1InterpreterTest, ChangedLowerInstructionUpdatesReadConflict)
{
	Run(Add(1, 2, 3), Move(4, 2));
	ExpectVector(4, 3.0f);
	for (float& lane : VU1.VF[1].F)
		lane = 2.0f;
	Run(Add(1, 2, 3), Move(4, 1));
	ExpectVector(1, 8.0f);
	ExpectVector(4, 2.0f); // Lower reads the value from before the upper write.
}

TEST_F(VU1InterpreterTest, ImmediateBitChangesLowerInstructionInterpretation)
{
	const u32 lower = Move(4, 1);
	Run(NOP | IBit, lower);
	EXPECT_EQ(VU1.VI[REG_I].UL, lower);
	ExpectVector(4, 0.0f);
	Run(NOP, lower);
	ExpectVector(4, 2.0f);
	Run(NOP | IBit, 0x3f800000);
	EXPECT_EQ(VU1.VI[REG_I].UL, 0x3f800000u);
	ExpectVector(4, 2.0f);
}

TEST_F(VU1InterpreterTest, RestoredProgramDoesNotReuseStaleDependencies)
{
	Run(Add(1, 2, 3), Move(4, 2));
	const VURegs saved = VU1;
	const auto program = m_program;
	Run(Add(4, 1, 2), Move(5, 3));
	VU1 = saved;
	m_program = program;
	// Simulate a state load, which restores micro memory without calling Clear().
	VU1.VI[REG_TPC].UL = 0;
	CpuIntVU1.Step();
	ExpectVector(1, 8.0f);
	ExpectVector(4, 3.0f);
}

TEST_F(VU1InterpreterTest, ChangedDivisionInstructionUpdatesLatency)
{
	Run(NOP, 0x800003bc | (2 << 16) | (1 << 11)); // DIV Q, vf1.x, vf2.x
	EXPECT_EQ(VU1.fdiv.Cycle, 7u);
	Run(NOP, 0x800003be | (2 << 16) | (1 << 11)); // RSQRT Q, vf1.x, vf2.x
	EXPECT_EQ(VU1.fdiv.Cycle, 13u);
}

TEST_F(VU1InterpreterTest, ProgramCounterWrapUsesCorrectInstruction)
{
	Run(Add(4, 1, 2), Move(5, 3), VU1_PROGSIZE - 8);
	ExpectVector(4, 5.0f);
	EXPECT_EQ(VU1.VI[REG_TPC].UL, VU1_PROGSIZE);
	m_program[0] = Move(5, 1);
	m_program[1] = Add(4, 2, 3);
	CpuIntVU1.Step();
	ExpectVector(4, 8.0f);
	ExpectVector(5, 2.0f);
	EXPECT_EQ(VU1.VI[REG_TPC].UL, 8u);
}
