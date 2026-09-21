// SPDX-FileCopyrightText: 2026 PCSX2 Dev Team
// SPDX-License-Identifier: GPL-3.0+

#include "Common.h"

#if defined(ARCH_ARM64)
#include "R3000A.h"
#include "IopMem.h"
#include "arm64/IopCodeGenerator.h"
#include "arm64/IopRecompiler.h"
#include <gtest/gtest.h>
#include <array>
#include <cstring>

namespace
{
	constexpr u32 Base = 0x10000;
	constexpr u32 Data = 0x20000;
	constexpr u32 Stop = (18u << 26); // COP2 (GTE): every encoding is unsupported and always falls back.
	constexpr u32 Immediate[] = {8, 9, 10, 11, 12, 13, 14, 15};
	constexpr u32 Special[] = {0, 2, 3, 4, 6, 7, 16, 17, 18, 19, 24, 25, 26, 27,
		32, 33, 34, 35, 36, 37, 38, 39, 42, 43};

	class IopRecompilerTest : public testing::Test
	{
	protected:
		static void SetUpTestSuite() { ASSERT_TRUE(SysMemory::Allocate()); }
		static void TearDownTestSuite()
		{
			Arm64IOP::Shutdown();
			SysMemory::Release();
		}
		void SetUp() override
		{
			m_regs = psxRegs;
			m_rlut = psxMemRLUT;
			m_wlut = psxMemWLUT;
			m_config = EmuConfig.Cpu;
			m_cpu = psxCpu;
			// iopMemWrite*() calls psxCpu->Clear() on every store (self-modifying-code
			// invalidation hook); point it at the plain interpreter's no-op Clear()
			// since nothing else in this test process sets the global up.
			psxCpu = &psxInt;
			program.fill(Stop);
			memory.fill(0x81abcdef);
			pages_r.fill(0);
			pages_w.fill(0);
			pages_r[Base >> 16] = reinterpret_cast<uptr>(program.data());
			pages_w[Base >> 16] = reinterpret_cast<uptr>(program.data());
			pages_r[Data >> 16] = reinterpret_cast<uptr>(memory.data());
			pages_w[Data >> 16] = reinterpret_cast<uptr>(memory.data());
			psxMemRLUT = pages_r.data();
			psxMemWLUT = pages_w.data();
			EmuConfig.Cpu.Recompiler.EnableIOP = true;
			Arm64IOP::Reset();
			Init(0);
		}
		void TearDown() override
		{
			Arm64IOP::Reset();
			psxRegs = m_regs;
			psxMemRLUT = m_rlut;
			psxMemWLUT = m_wlut;
			EmuConfig.Cpu = m_config;
			psxCpu = m_cpu;
		}
		void Init(u32 seed)
		{
			std::memset(&psxRegs, 0, sizeof(psxRegs));
			psxRegs.pc = Base;
			// Keeps a real taken branch's doBranch()-internal deadline poll (see
			// R3000AInterpreter.cpp) from ever firing iopEventTest() against this
			// otherwise-uninitialized unit test environment (DMA/counters/etc.).
			// The deadline check is an unsigned "cycle - iopNextEventCycle >= 0"
			// comparison, so an actual huge value like ~u64(0) wraps around and
			// reads as overdue; a merely-far-future value avoids that.
			psxRegs.iopNextEventCycle = 1'000'000'000ull;
			constexpr u32 edge[] = {0, 1, 0xffffffff, 0x80000000, 0x7fffffff, 0x12345678};
			for (u32 reg = 1; reg < 32; reg++)
				psxRegs.GPR.r[reg] = edge[(seed + reg) % std::size(edge)] ^ (seed * 2654435761u + reg);
		}

		// Runs one reference instruction exactly the way the plain interpreter's
		// execI() does: fetch, advance pc/cycle, dispatch through psxBSC. Real
		// branches recurse into the actual psxJ/psxBEQ/... functions, including
		// their doBranch() delay-slot and event-test handling.
		void Step()
		{
			psxRegs.code = iopMemFetch32(psxRegs.pc);
			psxRegs.pc += 4;
			psxRegs.cycle++;
			psxBSC[psxRegs.code >> 26]();
		}

		void Compare(u32 count)
		{
			const psxRegisters initial = psxRegs;
			const auto initial_memory = memory;
			const auto initial_program = program;
			const Arm64IOP::BlockResult result = Arm64IOP::TryExecute();
			ASSERT_TRUE(result);
			// TryExecute() itself never touches psxRegs.cycle (see IopCodeGenerator.h);
			// ExecuteBlock() is the one that charges it by the completed count.
			psxRegs.cycle += result.completed;
			const psxRegisters actual = psxRegs;
			const auto actual_memory = memory;
			const auto actual_program = program;
			memory = initial_memory;
			program = initial_program;
			psxRegs = initial;
			for (u32 i = 0; i < count; i++)
				Step();
			EXPECT_EQ(result.completed, count);
			EXPECT_EQ(actual.pc, psxRegs.pc);
			EXPECT_EQ(std::memcmp(&actual, &psxRegs, sizeof(psxRegs)), 0);
			EXPECT_EQ(actual_memory, memory);
			EXPECT_EQ(actual_program, program);
		}

		psxRegisters m_regs{};
		const uptr* m_rlut = nullptr;
		uptr* m_wlut = nullptr;
		R3000Acpu* m_cpu = nullptr;
		Pcsx2Config::CpuOptions m_config;
		alignas(16) std::array<u32, 1024> program;
		alignas(16) std::array<u32, 1024> memory;
		std::array<uptr, 0x2000> pages_r{};
		std::array<uptr, 0x2000> pages_w{};
	};
} // namespace

TEST_F(IopRecompilerTest, BasicAluOperationsMatchInterpreter)
{
	for (u32 seed = 0; seed < 64; seed++)
	{
		const u32 rs = seed % 4, rt = (seed / 4) % 4, rd = (seed / 16) % 4;
		for (u32 op : Special)
		{
			SCOPED_TRACE(testing::Message() << "special=" << op << " seed=" << seed);
			Init(seed);
			program[0] = (rs << 21) | (rt << 16) | (rd << 11) | ((seed % 32) << 6) | op;
			Compare(1);
			if (HasFailure())
				return;
		}
		for (u32 op : Immediate)
		{
			SCOPED_TRACE(testing::Message() << "immediate=" << op << " seed=" << seed);
			Init(seed);
			program[0] = (op << 26) | (rs << 21) | (rt << 16) | ((seed * 65521) & 0xffff);
			Compare(1);
			if (HasFailure())
				return;
		}
	}
}

TEST_F(IopRecompilerTest, DivisionByZeroAndOverflowMatchInterpreter)
{
	constexpr u32 lhs_values[] = {0, 1, 0xffffffff, 0x80000000, 0x7fffffff};
	constexpr u32 rhs_values[] = {0, 1, 0xffffffff, 2};
	for (u32 funct : {26u, 27u}) // DIV, DIVU
	{
		for (u32 lhs : lhs_values)
		{
			for (u32 rhs : rhs_values)
			{
				SCOPED_TRACE(testing::Message() << "funct=" << funct << " lhs=" << lhs << " rhs=" << rhs);
				Init(0);
				psxRegs.GPR.r[1] = lhs;
				psxRegs.GPR.r[2] = rhs;
				program[0] = (1 << 21) | (2 << 16) | funct;
				Compare(1);
				if (HasFailure())
					return;
			}
		}
	}
}

TEST_F(IopRecompilerTest, Cop0RegisterTransfersMatchInterpreter)
{
	for (u32 rs : {0u, 2u, 4u, 6u}) // MFC0, CFC0, MTC0, CTC0
	{
		SCOPED_TRACE(rs);
		Init(1);
		psxRegs.CP0.r[12] = 0x12345678; // Status
		program[0] = (16u << 26) | (rs << 21) | (2u << 16) | (12u << 11);
		Compare(1);
		if (HasFailure())
			return;
	}
}

TEST_F(IopRecompilerTest, LoadsAndStoresThroughLutFastPath)
{
	for (u32 op : {32u, 33u, 35u, 36u, 37u, 40u, 41u, 43u}) // LB,LH,LW,LBU,LHU,SB,SH,SW
	{
		for (u32 seed = 0; seed < 16; seed++)
		{
			SCOPED_TRACE(testing::Message() << "opcode=" << op << " seed=" << seed);
			Init(seed);
			const u32 address = Data + (seed % 16) * 4;
			psxRegs.GPR.r[1] = address - 8; // exercise a nonzero displacement
			const u32 rt = seed % 4;
			program[0] = (op << 26) | (1 << 21) | (rt << 16) | 8;
			Compare(1);
			if (HasFailure())
				return;
		}
	}
}

TEST_F(IopRecompilerTest, ConditionalBranchTakenAndUntakenPreserveDelaySlot)
{
	for (bool taken : {false, true})
	{
		SCOPED_TRACE(taken);
		Init(0);
		psxRegs.GPR.r[1] = taken ? 5 : 5;
		psxRegs.GPR.r[2] = taken ? 5 : 6;
		program[0] = (4u << 26) | (1 << 21) | (2 << 16) | 4; // BEQ r1, r2, +4
		program[1] = (9u << 26) | (3 << 16) | 77; // ADDIU r3, r0, 77 (delay slot; always executes)
		const psxRegisters initial = psxRegs;
		const Arm64IOP::BlockResult result = Arm64IOP::TryExecute();
		ASSERT_TRUE(result);
		psxRegs.cycle += result.completed;
		const psxRegisters actual = psxRegs;
		psxRegs = initial;
		Step(); // branch instruction itself
		// A real taken branch's doBranch() already executes the delay slot
		// internally (see R3000AInterpreter.cpp); only an untaken branch leaves
		// it as an ordinary next instruction that needs its own Step().
		if (!taken)
			Step();
		EXPECT_EQ(result.exit, taken ? Arm64IOP::BlockExit::TakenBranch : Arm64IOP::BlockExit::Continue);
		EXPECT_EQ(result.completed, 2u);
		EXPECT_EQ(actual.pc, psxRegs.pc);
		EXPECT_EQ(actual.GPR.r[3], 77u);
		EXPECT_EQ(std::memcmp(&actual, &psxRegs, sizeof(psxRegs)), 0);
	}
}

TEST_F(IopRecompilerTest, JumpAndLinkRegisterCapturesTargetBeforeDelaySlotWrite)
{
	Init(0);
	psxRegs.GPR.r[1] = Base + 0x100;
	program[0] = (1 << 21) | (31 << 11) | 9; // JALR r31, r1
	program[1] = (9u << 26) | (1 << 16) | 42; // ADDIU r1, r0, 42 (overwrites the jump source)
	const psxRegisters initial = psxRegs;
	const Arm64IOP::BlockResult result = Arm64IOP::TryExecute();
	ASSERT_TRUE(result);
	psxRegs.cycle += result.completed;
	const psxRegisters actual = psxRegs;
	psxRegs = initial;
	Step(); // JALR is unconditional; doBranch() executes the delay slot internally.
	EXPECT_EQ(result.exit, Arm64IOP::BlockExit::TakenBranch);
	EXPECT_EQ(actual.pc, Base + 0x100);
	EXPECT_EQ(actual.GPR.r[31], Base + 8);
	EXPECT_EQ(actual.GPR.r[1], 42u);
	EXPECT_EQ(std::memcmp(&actual, &psxRegs, sizeof(psxRegs)), 0);
}

TEST_F(IopRecompilerTest, UnsupportedGteOpcodeFallsBackWithoutStateChange)
{
	// program is pre-filled with Stop (a GTE COP2 opcode). Nothing in the block
	// is supported, so TryExecute must decline entirely and leave state alone.
	const psxRegisters before = psxRegs;
	for (u32 visit = 0; visit < 3; visit++)
	{
		const Arm64IOP::BlockResult result = Arm64IOP::TryExecute();
		EXPECT_FALSE(result);
		EXPECT_EQ(result.completed, 0u);
		EXPECT_EQ(std::memcmp(&before, &psxRegs, sizeof(psxRegs)), 0);
	}
}

TEST_F(IopRecompilerTest, UnalignedAccessOpcodesAndPlainJFallBack)
{
	for (u32 code : {(34u << 26), (38u << 26), (42u << 26), (46u << 26), (2u << 26)}) // LWL,LWR,SWL,SWR,J
	{
		SCOPED_TRACE(code);
		program[0] = code;
		const psxRegisters before = psxRegs;
		const Arm64IOP::BlockResult result = Arm64IOP::TryExecute();
		EXPECT_FALSE(result);
		EXPECT_EQ(std::memcmp(&before, &psxRegs, sizeof(psxRegs)), 0);
	}
}

TEST_F(IopRecompilerTest, SupportedIntegerCodeStaysNativeAcrossUnsupportedNeighbor)
{
	// A block boundary at an unsupported opcode must not corrupt state: the
	// supported prefix compiles and runs, then a second TryExecute() call
	// (as the driver would issue) falls back for the unsupported instruction.
	Init(2);
	program[0] = (9u << 26) | (1 << 16) | 11; // ADDIU r1, r0, 11
	program[1] = (9u << 26) | (1 << 21) | (2 << 16) | 22; // ADDIU r2, r1, 22
	program[2] = Stop;
	Compare(2);
	if (HasFailure())
		return;
	const psxRegisters before = psxRegs;
	const Arm64IOP::BlockResult result = Arm64IOP::TryExecute();
	EXPECT_FALSE(result);
	EXPECT_EQ(std::memcmp(&before, &psxRegs, sizeof(psxRegs)), 0);
}

TEST_F(IopRecompilerTest, SelfModifyingStoreInvalidatesCachedBlock)
{
	Init(0);
	program[0] = (9u << 26) | (1 << 16) | 5;
	Compare(1);
	psxRegs.pc = Base;
	program[0] = (9u << 26) | (1 << 16) | 9;
	Compare(1);
	EXPECT_EQ(psxRegs.GPR.r[1], 9u);
}
#endif
