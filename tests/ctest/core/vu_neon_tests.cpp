// SPDX-FileCopyrightText: 2002-2026 PCSX2 Dev Team
// SPDX-License-Identifier: GPL-3.0+

#include "Common.h"
#include "VUops.h"

#include <gtest/gtest.h>

#include <array>
#include <cstring>

#if defined(ARCH_ARM64)
namespace
{
	class VUNEONTest : public testing::Test
	{
	protected:
		void SetUp() override
		{
			m_savedVU0 = VU0;
			m_savedVU1 = VU1;
			m_savedCPU = EmuConfig.Cpu;
		}

		void TearDown() override
		{
			VU0 = m_savedVU0;
			VU1 = m_savedVU1;
			EmuConfig.Cpu = m_savedCPU;
		}

		void Compare(u32 code, u32 seed)
		{
			std::memset(&VU0, 0, sizeof(VU0));
			constexpr std::array<u32, 16> edge = {0, 0x80000000, 1, 0x80000001,
				0x007fffff, 0x807fffff, 0x00800000, 0x80800000,
				0x3f800000, 0xbf800000, 0x7f7fffff, 0xff7fffff,
				0x7f800000, 0xff800000, 0x7fc12345, 0xff812345};
			u32 random = seed + 1;
			auto next = [&random]() {
				random = random * 1664525 + 1013904223;
				return random;
			};
			for (u32 reg = 0; reg < 32; reg++)
			{
				VU0.VI[reg].UL = seed < 32 ? edge[(reg + seed) % edge.size()] : next();
				for (u32 lane = 0; lane < 4; lane++)
					VU0.VF[reg].UL[lane] = seed < 32 ? edge[(reg + lane + seed) % edge.size()] : next();
			}
			VU0.ACC = VU0.VF[7];
			VU0.macflag = 0xabcd1234;
			VU0.statusflag = 0xdef;
			VU0.code = code;
			VU1 = VU0;
			// VU0 keeps the scalar path; compare actual opcode execution, including flags.
			VU0_UPPER_OPCODE[code & 0x3f]();
			VU1_UPPER_OPCODE[code & 0x3f]();
			ASSERT_EQ(std::memcmp(VU0.VF, VU1.VF, sizeof(VU0.VF)), 0) << "code=" << code << " seed=" << seed;
			ASSERT_EQ(std::memcmp(&VU0.ACC, &VU1.ACC, sizeof(VU0.ACC)), 0) << "code=" << code << " seed=" << seed;
			ASSERT_EQ(VU0.macflag, VU1.macflag) << "code=" << code << " seed=" << seed;
			ASSERT_EQ(VU0.statusflag, VU1.statusflag) << "code=" << code << " seed=" << seed;
		}

		VURegs m_savedVU0;
		VURegs m_savedVU1;
		Pcsx2Config::CpuOptions m_savedCPU;
	};
} // namespace

TEST_F(VUNEONTest, ArithmeticMatchesScalarWithRoundingClampingAndAliasing)
{
	// Full and partial masks, destination/source aliases, VF0, and accumulator forms.
	// Game-level floating-point compatibility still needs proper testing.
	constexpr std::array<u32, 45> opcodes = {
		0x00, 0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07,
		0x08, 0x09, 0x0a, 0x0b, 0x0c, 0x0d, 0x0e, 0x0f,
		0x18, 0x19, 0x1a, 0x1b, 0x1c, 0x1e,
		0x20, 0x21, 0x22, 0x23, 0x24, 0x25, 0x26, 0x27,
		0x28, 0x29, 0x2a, 0x2c, 0x2d,
		0x2bc, 0x2fc, 0x2be, 0x2bd, 0x2fd,
		0x03c, 0x0bc, 0x0fc, 0x1bc, 0x1fc};
	for (u32 round = 0; round < 4; round++)
	{
		for (bool flush : {false, true})
		{
			const FPControlRegisterBackup fpcr(FPControlRegister::GetDefault().DisableExceptions().SetRoundMode(static_cast<FPRoundMode>(round)).SetFlushToZero(flush));
			for (bool clamp : {false, true})
			{
				EmuConfig.Cpu.Recompiler.vu0Overflow = clamp;
				EmuConfig.Cpu.Recompiler.vu1Overflow = clamp;
				for (u32 opcode : opcodes)
				{
					for (u32 sample = 0; sample < 128; sample++)
					{
						const u32 mask = sample < 96 ? 15 : sample % 16;
						const u32 dest = (opcode & 0x3f) >= 0x3c ? 0 : sample % 4;
						const u32 code = (mask << 21) | (2 << 16) | (1 << 11) | (dest << 6) | opcode;
						Compare(code, sample);
						if (HasFatalFailure())
							return;
					}
				}
			}
		}
	}
}

TEST_F(VUNEONTest, MultiplyAddMatchesScalarAtCancellationBoundary)
{
	for (u32 round = 0; round < 4; round++)
	{
		const FPControlRegisterBackup fpcr(FPControlRegister::GetDefault().DisableExceptions().SetRoundMode(static_cast<FPRoundMode>(round)));
		for (u32 opcode : {0x29u, 0x2du, 0x2bdu, 0x2fdu})
		{
			std::memset(&VU0, 0, sizeof(VU0));
			const u32 dest = opcode < 0x3c ? 3 << 6 : 0;
			VU0.code = (15 << 21) | (2 << 16) | (1 << 11) | dest | opcode;
			for (u32 lane = 0; lane < 4; lane++)
			{
				VU0.VF[1].UL[lane] = 0x3f800001;
				VU0.VF[2].UL[lane] = 0x3f800001;
				VU0.ACC.UL[lane] = (opcode == 0x29 || opcode == 0x2bd) ? 0xbf800002 : 0x3f800002;
			}
			VU1 = VU0;
			VU0_UPPER_OPCODE[VU0.code & 0x3f]();
			VU1_UPPER_OPCODE[VU1.code & 0x3f]();
			EXPECT_EQ(std::memcmp(VU0.VF, VU1.VF, sizeof(VU0.VF)), 0);
			EXPECT_EQ(std::memcmp(&VU0.ACC, &VU1.ACC, sizeof(VU0.ACC)), 0);
			EXPECT_EQ(VU0.macflag, VU1.macflag);
			EXPECT_EQ(VU0.statusflag, VU1.statusflag);
		}
	}
}
#endif
