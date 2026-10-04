// SPDX-FileCopyrightText: 2026 PCSX2 Dev Team
// SPDX-License-Identifier: GPL-3.0+

#include "Common.h"

#if defined(ARCH_ARM64)
#include "R5900OpcodeTables.h"
#include "VU.h"
#include "arm64/EERecompiler.h"
#include "vtlb.h"
#include <gtest/gtest.h>
#include <array>
#include <algorithm>

// Deadline polling must remain correct across both the old 32-bit cycle boundary
// and the full counter wrap, without postponing an execution-exit request.
TEST(EEBranchPollingTest, DeadlinesWrapAndForcedInterpreterPolling)
{
	struct Case
	{
		u64 cycle;
		u64 deadline;
		bool due;
	};
	constexpr Case cases[] = {
		{99, 100, false}, {100, 100, true}, {101, 100, true},
		{0xffffffff, 0x100000000, false}, {0x100000000, 0xffffffff, true},
		{~u64(0), 0, false}, {0, ~u64(0), true},
		{~u64(0) - 2, 2, false}, {2, ~u64(0) - 2, true}};
	for (const auto& test : cases)
	{
		SCOPED_TRACE(testing::Message() << "cycle=" << test.cycle << " deadline=" << test.deadline);
		EXPECT_EQ(EEBranchEventDue(true, false, test.cycle, test.deadline), test.due);
		EXPECT_TRUE(EEBranchEventDue(true, true, test.cycle, test.deadline));
		EXPECT_TRUE(EEBranchEventDue(false, false, test.cycle, test.deadline));
	}
}

namespace
{
	constexpr u32 Base = 0x10000;
	constexpr u32 Data = 0x20000;
	constexpr u32 Alias = Base + 0x4000;
	constexpr u32 Stop = 0x0000000d; // BREAK ends the block before its side effects.
	constexpr u32 Immediate[] = {9, 10, 11, 12, 13, 14, 15, 25, 51}; // 51 = PREF
	constexpr u32 Special[] = {0, 2, 3, 4, 6, 7, 10, 11, 15, 20, 22, 23, 33, 35, 36,
		37, 38, 39, 42, 43, 45, 47, 56, 58, 59, 60, 62, 63}; // 15 = SYNC
	constexpr u32 HiLoInstructions[] = {
		16, 17, 18, 19, 24, 25, 26, 27,
		(28u << 26) | 16, (28u << 26) | 17, (28u << 26) | 18, (28u << 26) | 19,
		(28u << 26) | 24, (28u << 26) | 25, (28u << 26) | 26, (28u << 26) | 27,
		(28u << 26), (28u << 26) | 1, (28u << 26) | 32, (28u << 26) | 33};

	constexpr u32 PackedCode(u32 function, u32 selector)
	{
		return (28u << 26) | (selector << 6) | function;
	}
	constexpr u32 PackedInstructions[] = {
		PackedCode(8, 0), PackedCode(8, 1), PackedCode(8, 2), PackedCode(8, 3), // PADDW, PSUBW, PCGTW, PMAXW
		PackedCode(8, 4), PackedCode(8, 5), PackedCode(8, 6), PackedCode(8, 7), // PADDH, PSUBH, PCGTH, PMAXH
		PackedCode(8, 8), PackedCode(8, 9), PackedCode(8, 10), // PADDB, PSUBB, PCGTB
		PackedCode(8, 18), PackedCode(8, 22), PackedCode(8, 26), // PEXTLW/H/B
		PackedCode(8, 19), PackedCode(8, 23), PackedCode(8, 27), // PPACW/H/B
		PackedCode(40, 2), PackedCode(40, 6), PackedCode(40, 10), // PCEQW/H/B
		PackedCode(40, 3), PackedCode(40, 7), // PMINW/H
		PackedCode(40, 18), PackedCode(40, 22), PackedCode(40, 26), // PEXTUW/H/B
		PackedCode(9, 14), PackedCode(41, 14), // PCPYLD/UD
		PackedCode(9, 18), PackedCode(9, 19), PackedCode(41, 18), PackedCode(41, 19)}; // PAND, PXOR, POR, PNOR

	class EERecompilerTest : public testing::Test
	{
	protected:
		static void SetUpTestSuite() { ASSERT_TRUE(SysMemory::Allocate()); }
		static void TearDownTestSuite()
		{
			Arm64EE::Shutdown();
			SysMemory::Release();
		}
		void SetUp() override
		{
			m_cpu = cpuRegs;
			m_vu0 = VU0;
			m_fpu = fpuRegs;
			m_config = EmuConfig.Cpu;
			m_goemon = EmuConfig.Gamefixes.GoemonTlbHack;
			m_addsub = EmuConfig.Gamefixes.VuAddSubHack;
			EmuConfig.Gamefixes.GoemonTlbHack = false;
			m_mapping = vtlb_private::vtlbdata.vmap[Base >> 12];
			m_last_mapping = vtlb_private::vtlbdata.vmap[0xfffff];
			m_data_mapping = vtlb_private::vtlbdata.vmap[Data >> 12];
			m_alias_mapping = vtlb_private::vtlbdata.vmap[Alias >> 12];
			program.fill(Stop);
			memory.fill(0x81abcdef);
			Map(Base, program.data());
			Map(Data, memory.data());
			EmuConfig.Cpu.Recompiler.EnableEE = true;
			Arm64EE::Reset();
			Init(0);
		}
		void TearDown() override
		{
			Arm64EE::Reset();
			vtlb_private::vtlbdata.vmap[Base >> 12] = m_mapping;
			vtlb_private::vtlbdata.vmap[0xfffff] = m_last_mapping;
			vtlb_private::vtlbdata.vmap[Data >> 12] = m_data_mapping;
			vtlb_private::vtlbdata.vmap[Alias >> 12] = m_alias_mapping;
			cpuRegs = m_cpu;
			VU0 = m_vu0;
			fpuRegs = m_fpu;
			EmuConfig.Cpu = m_config;
			EmuConfig.Gamefixes.GoemonTlbHack = m_goemon;
			EmuConfig.Gamefixes.VuAddSubHack = m_addsub;
		}
		void Map(u32 address, u32* buffer)
		{
			vtlb_private::vtlbdata.vmap[address >> 12] = vtlb_private::VTLBVirtual::fromPointer(reinterpret_cast<uptr>(buffer), address);
		}
		void Init(u32 seed)
		{
			std::memset(&cpuRegs, 0, sizeof(cpuRegs));
			cpuRegs.pc = Base;
			cpuRegs.CP0.n.Config = (seed & 1) << 18;
			constexpr u64 edge[] = {0, 1, ~u64(0), 0x80000000, 0x7fffffff,
				0x8000000000000000, 0x7fffffffffffffff, 0xffffffff00000000};
			for (u32 reg = 1; reg < 32; reg++)
			{
				cpuRegs.GPR.r[reg].UD[0] = edge[(seed + reg) % std::size(edge)];
				cpuRegs.GPR.r[reg].UD[1] = 0x1234567812345678ULL ^ (u64(seed) << 32) ^ reg;
			}
		}
		void InitHiLo(u32 seed)
		{
			Init(seed);
			cpuRegs.HI.UD[0] = 0x89abcdef01234567ULL ^ (u64(seed) << 32);
			cpuRegs.LO.UD[0] = 0x76543210fedcba98ULL ^ seed;
			cpuRegs.HI.UD[1] = 0xa5a5a5a580000001ULL ^ seed;
			cpuRegs.LO.UD[1] = 0x5a5a5a5affffffffULL ^ (u64(seed) << 32);
		}

		void InitPacked(u32 seed)
		{
			InitHiLo(seed);
			constexpr u32 values[] = {0, 1, 0xffffffff, 0x7fffffff, 0x80000000,
				0x7fff8000, 0x80007fff, 0x007f80ff, 0xff807f00, 0x01020304,
				0x7fa12345, 0xffc54321};
			u32 random = seed;
			for (u32 reg = 1; reg < 32; reg++)
			{
				for (u32 lane = 0; lane < 4; lane++)
				{
					random = random * 1664525 + 1013904223;
					cpuRegs.GPR.r[reg].UL[lane] = seed < 64 ? values[(seed + reg * 5 + lane * 7) % std::size(values)] : random;
				}
			}
		}

		void Compare(u32 count)
		{
			const cpuRegisters initial = cpuRegs;
			const auto initial_memory = memory;
			const auto initial_program = program;
			u32 native_cycles = 0xfffffff0;
			ASSERT_TRUE(Arm64EE::TryExecute(native_cycles));
			const cpuRegisters actual = cpuRegs;
			const auto actual_memory = memory;
			const auto actual_program = program;
			memory = initial_memory;
			program = initial_program;
			cpuRegs = initial;
			u32 expected_cycles = 0xfffffff0;
			for (u32 i = 0; i < count; i++)
			{
				const auto mapping = vtlb_private::vtlbdata.vmap[cpuRegs.pc >> 12];
				cpuRegs.code = *reinterpret_cast<const u32*>(mapping.assumePtr(cpuRegs.pc));
				cpuRegs.pc += 4;
				const auto& opcode = R5900::GetCurrentInstruction();
				expected_cycles += opcode.cycles * (2 - ((cpuRegs.CP0.n.Config >> 18) & 1));
				opcode.interpret();
			}
			EXPECT_EQ(native_cycles, expected_cycles);
			EXPECT_EQ(actual.pc, cpuRegs.pc);
			EXPECT_EQ(std::memcmp(&actual, &cpuRegs, sizeof(cpuRegs)), 0);
			EXPECT_EQ(actual_memory, memory);
			EXPECT_EQ(actual_program, program);
		}
		void CompareBranch(u32 prefix, bool taken, bool likely, bool event, u32 target, u32 link = 0)
		{
			const cpuRegisters initial = cpuRegs;
			const VURegs initial_vu0 = VU0;
			const fpuRegisters initial_fpu = fpuRegs;
			const auto initial_memory = memory;
			u32 native_cycles = 0xfffffff0;
			const EEBlockResult result = Arm64EE::TryExecute(native_cycles);
			ASSERT_TRUE(result);
			const cpuRegisters actual = cpuRegs;
			const VURegs actual_vu0 = VU0;
			const fpuRegisters actual_fpu = fpuRegs;
			const auto actual_memory = memory;
			cpuRegs = initial;
			VU0 = initial_vu0;
			fpuRegs = initial_fpu;
			memory = initial_memory;
			u32 expected_cycles = 0xfffffff0;
			auto step = [&](bool interpret) {
				const auto mapping = vtlb_private::vtlbdata.vmap[cpuRegs.pc >> 12];
				cpuRegs.code = *reinterpret_cast<const u32*>(mapping.assumePtr(cpuRegs.pc));
				cpuRegs.pc += 4;
				const auto& opcode = R5900::GetCurrentInstruction();
				expected_cycles += opcode.cycles * (2 - ((cpuRegs.CP0.n.Config >> 18) & 1));
				if (interpret)
					opcode.interpret();
			};
			for (u32 i = 0; i < prefix; i++)
				step(true);
			step(false); // branch state before the shared driver's event processing
			if (link)
				cpuRegs.GPR.r[link].UD[0] = cpuRegs.pc + 4;
			if (taken)
				step(true);
			else if (likely)
				cpuRegs.pc += 4;
			EXPECT_EQ(result.exit, taken ? EEBlockExit::TakenBranch : event ? EEBlockExit::EventTest :
																			  EEBlockExit::Continue);
			EXPECT_EQ(result.target, taken ? target : 0u);
			EXPECT_EQ(native_cycles, expected_cycles);
			EXPECT_EQ(actual.pc, cpuRegs.pc);
			EXPECT_EQ(std::memcmp(&actual, &cpuRegs, sizeof(cpuRegs)), 0);
			EXPECT_EQ(std::memcmp(&actual_vu0, &VU0, sizeof(VU0)), 0);
			EXPECT_EQ(std::memcmp(&actual_fpu, &fpuRegs, sizeof(fpuRegs)), 0);
			EXPECT_EQ(actual_memory, memory);
		}
		// The block at pc must fall back to the interpreter without side effects.
		void ExpectRejected()
		{
			const cpuRegisters before = cpuRegs;
			const VURegs before_vu0 = VU0;
			const auto before_memory = memory;
			u32 cycles = 123;
			EXPECT_FALSE(Arm64EE::TryExecute(cycles));
			EXPECT_EQ(cycles, 123u);
			EXPECT_EQ(std::memcmp(&before, &cpuRegs, sizeof(cpuRegs)), 0);
			EXPECT_EQ(std::memcmp(&before_vu0, &VU0, sizeof(VU0)), 0);
			EXPECT_EQ(before_memory, memory);
		}

		// VU0 macro-mode (COP2) ops execute via a direct interpreter call from the
		// native block (see EmitCOP2 in arm64/EECodeGenerator.cpp), so besides the
		// usual cpuRegs comparison, the reference run below also needs VU0's own
		// state (VF/VI registers, clip flag, etc.) compared, and VPU_STAT must stay
		// zero so vu0Sync()/_vu0FinishMicro() short-circuit instead of touching the
		// (uninitialized, in this test) VU0 micro-mode program.
		void InitVU0(u32 seed)
		{
			std::memset(&VU0, 0, sizeof(VU0));
			u32 random = seed * 2654435761u + 1;
			auto next = [&random]() { random = random * 1664525 + 1013904223; return random; };
			for (u32 reg = 1; reg < 32; reg++)
			{
				for (u32 lane = 0; lane < 4; lane++)
					VU0.VF[reg].UL[lane] = next();
				VU0.VI[reg].UL = next();
			}
			VU0.VI[REG_VPU_STAT].UL = 0;
		}
		void CompareWithVU0(u32 count)
		{
			const cpuRegisters initial = cpuRegs;
			const VURegs initial_vu0 = VU0;
			const auto initial_memory = memory;
			u32 native_cycles = 0xfffffff0;
			ASSERT_TRUE(Arm64EE::TryExecute(native_cycles));
			const cpuRegisters actual = cpuRegs;
			const VURegs actual_vu0 = VU0;
			const auto actual_memory = memory;
			cpuRegs = initial;
			VU0 = initial_vu0;
			memory = initial_memory;
			u32 expected_cycles = 0xfffffff0;
			for (u32 i = 0; i < count; i++)
			{
				const auto mapping = vtlb_private::vtlbdata.vmap[cpuRegs.pc >> 12];
				cpuRegs.code = *reinterpret_cast<const u32*>(mapping.assumePtr(cpuRegs.pc));
				cpuRegs.pc += 4;
				const auto& opcode = R5900::GetCurrentInstruction();
				expected_cycles += opcode.cycles * (2 - ((cpuRegs.CP0.n.Config >> 18) & 1));
				opcode.interpret();
			}
			EXPECT_EQ(native_cycles, expected_cycles);
			EXPECT_EQ(actual.pc, cpuRegs.pc);
			EXPECT_EQ(std::memcmp(&actual, &cpuRegs, sizeof(cpuRegs)), 0);
			EXPECT_EQ(std::memcmp(&actual_vu0, &VU0, sizeof(VU0)), 0);
			EXPECT_EQ(actual_memory, memory);
		}

		// COP1 (FPU) native codegen (see EmitCOP1 in arm64/EECodeGenerator.cpp)
		// writes fpuRegs directly, which is not part of cpuRegisters, so it needs
		// its own init/compare pair alongside the usual cpuRegs comparison.
		void InitFPU(u32 seed)
		{
			std::memset(&fpuRegs, 0, sizeof(fpuRegs));
			// A mix of ordinary values and the PS2 FPU's non-IEEE edge cases:
			// +/-0, +/-1, +/-Fmax, +/-Inf, NaN, smallest/largest +/-denormals.
			constexpr u32 edge[] = {
				0x00000000, 0x80000000,
				0x3f800000, 0xbf800000,
				0x7f7fffff, 0xff7fffff,
				0x7f800000, 0xff800000,
				0x7fc00000, 0xffc00000,
				0x00000001, 0x80000001,
				0x007fffff, 0x807fffff,
				0x3f000000, 0x40490fdb,
			};
			u32 random = seed * 2654435761u + 1;
			auto next = [&random]() { random = random * 1664525 + 1013904223; return random; };
			for (u32 reg = 0; reg < 32; reg++)
				fpuRegs.fpr[reg].UL = seed < 64 ? edge[(seed + reg) % std::size(edge)] : next();
		}
		void CompareWithFPU(u32 count)
		{
			const cpuRegisters initial = cpuRegs;
			const fpuRegisters initial_fpu = fpuRegs;
			const auto initial_memory = memory;
			u32 native_cycles = 0xfffffff0;
			ASSERT_TRUE(Arm64EE::TryExecute(native_cycles));
			const cpuRegisters actual = cpuRegs;
			const fpuRegisters actual_fpu = fpuRegs;
			const auto actual_memory = memory;
			cpuRegs = initial;
			fpuRegs = initial_fpu;
			memory = initial_memory;
			u32 expected_cycles = 0xfffffff0;
			for (u32 i = 0; i < count; i++)
			{
				const auto mapping = vtlb_private::vtlbdata.vmap[cpuRegs.pc >> 12];
				cpuRegs.code = *reinterpret_cast<const u32*>(mapping.assumePtr(cpuRegs.pc));
				cpuRegs.pc += 4;
				const auto& opcode = R5900::GetCurrentInstruction();
				expected_cycles += opcode.cycles * (2 - ((cpuRegs.CP0.n.Config >> 18) & 1));
				opcode.interpret();
			}
			EXPECT_EQ(native_cycles, expected_cycles);
			EXPECT_EQ(actual.pc, cpuRegs.pc);
			EXPECT_EQ(std::memcmp(&actual, &cpuRegs, sizeof(cpuRegs)), 0);
			EXPECT_EQ(std::memcmp(&actual_fpu, &fpuRegs, sizeof(fpuRegs)), 0);
			EXPECT_EQ(actual_memory, memory);
		}

		alignas(16) std::array<u32, 1024> program;
		alignas(16) std::array<u32, 1024> memory;
		cpuRegisters m_cpu;
		VURegs m_vu0;
		fpuRegisters m_fpu;
		Pcsx2Config::CpuOptions m_config;
		bool m_goemon;
		bool m_addsub;
		vtlb_private::VTLBVirtual m_mapping, m_last_mapping, m_data_mapping, m_alias_mapping;
	};
} // namespace

TEST_F(EERecompilerTest, AllIntegerOperationsAndAliasedRegisters)
{
	for (u32 seed = 0; seed < 128; seed++)
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

TEST_F(EERecompilerTest, MixedBlocksAndModifiedCode)
{
	u32 random = 12345;
	auto next = [&random]() { random = random * 1664525 + 1013904223; return random; };
	for (u32 seed = 0; seed < 128; seed++)
	{
		SCOPED_TRACE(seed);
		Init(seed);
		for (u32 reg = 1; reg < 32; reg++)
			cpuRegs.GPR.r[reg].UD[0] = (u64(next()) << 32) | next();
		for (u32 i = 0; i < 32; i++)
		{
			const u32 fields = next() & 0x03ffffff;
			program[i] = (i & 1) ? (fields & ~63u) | Special[(next() >> 16) % std::size(Special)] :
			                       fields | (Immediate[(next() >> 16) % std::size(Immediate)] << 26);
		}
		Compare(32);
		if (HasFailure())
			return;
		cpuRegs.pc = Base;
		program[15] = (15 << 26) | (3 << 16) | 0xabcd; // change a cached block's interior
		Compare(32);
		if (HasFailure())
			return;
	}
}

TEST_F(EERecompilerTest, CachedRegistersFollowEveryWriter)
{
	// Four guest registers shared by integer ops (which keep GPRs in host
	// registers within a block), loads, HI/LO transfers, MULT and packed ops
	// (which write GPRs directly), so every cached value is reused, evicted
	// and overwritten behind the cache's back. r5 holds the data address.
	u32 random = 777;
	auto next = [&random]() { random = random * 1664525 + 1013904223; return random >> 8; };
	for (u32 seed = 0; seed < 256; seed++)
	{
		SCOPED_TRACE(seed);
		InitHiLo(seed);
		for (u32 reg = 1; reg <= 4; reg++)
			cpuRegs.GPR.r[reg].UD[0] = (u64(next()) << 40) ^ (u64(next()) << 16) ^ next();
		cpuRegs.GPR.r[5].UD[0] = Data;
		for (u32 i = 0; i < 32; i++)
		{
			const u32 rs = 1 + next() % 4, rt = 1 + next() % 4, rd = 1 + next() % 4, sa = next() % 32;
			const u32 offset = (next() % 16) * 16, imm = next() & 0xffff;
			const u32 r = (rs << 21) | (rt << 16) | (rd << 11);
			const u32 choices[] = {
				r | 33, // ADDU
				r | 37, // OR
				(rt << 16) | (rd << 11) | (sa << 6), // SLL
				r | 45, // DADDU
				r | 10, // MOVZ
				r | 11, // MOVN
				r | 42, // SLT
				(9u << 26) | (rs << 21) | (rt << 16) | imm, // ADDIU
				(15u << 26) | (rt << 16) | imm, // LUI
				(13u << 26) | (rs << 21) | (rt << 16) | imm, // ORI
				(35u << 26) | (5 << 21) | (rt << 16) | offset, // LW
				(55u << 26) | (5 << 21) | (rt << 16) | offset, // LD
				(43u << 26) | (5 << 21) | (rt << 16) | offset, // SW
				(63u << 26) | (5 << 21) | (rt << 16) | offset, // SD
				(30u << 26) | (5 << 21) | (rt << 16) | offset, // LQ
				(31u << 26) | (5 << 21) | (rt << 16) | offset, // SQ
				r | 24, // MULT rd
				(rd << 11) | 18, // MFLO
				(rs << 21) | 19, // MTLO
				r | PackedCode(8, 0), // PADDW
			};
			program[i] = choices[next() % std::size(choices)];
		}
		Compare(32);
		if (HasFailure())
			return;
	}
}

TEST_F(EERecompilerTest, MappingChangesAndPageBoundaries)
{
	program[0] = (9 << 26) | (1 << 16) | 17;
	Compare(1);
	alignas(16) std::array<u32, 1024> other;
	other.fill(Stop);
	other[0] = (9 << 26) | (1 << 16) | 42;
	Map(Base, other.data());
	cpuRegs.pc = Base;
	Compare(1);
	other[1023] = (9 << 26) | (1 << 16) | 123;
	cpuRegs.pc = Base + 4092;
	Compare(1);
	Map(0xfffff000, other.data());
	cpuRegs.pc = 0xfffffffc;
	Compare(1);
}

TEST_F(EERecompilerTest, BlocksWithMatchingPageOffsetsRemainCached)
{
	program[0] = (9u << 26) | (1 << 16) | 123;
	Map(Alias, program.data());
	Compare(1);
	cpuRegs.pc = Alias;
	Compare(1);
	const size_t warmed_size = Arm64EE::GetCommittedCache();
	ASSERT_GT(warmed_size, 0u);
	for (u32 i = 0; i < 128; i++)
	{
		cpuRegs.pc = (i & 1) ? Alias : Base;
		Compare(1);
		if (HasFailure())
			return;
	}
	EXPECT_EQ(Arm64EE::GetCommittedCache(), warmed_size);
}

TEST_F(EERecompilerTest, RamLoadsStoresAndAddressWrapping)
{
	for (u32 op : {30u, 31u, 32u, 33u, 35u, 36u, 37u, 39u, 40u, 41u, 43u, 55u, 63u})
	{
		for (u32 seed = 0; seed < 64; seed++)
		{
			SCOPED_TRACE(testing::Message() << "opcode=" << op << " seed=" << seed);
			Init(seed);
			// Compare() leaves the stores' results behind; a zeroed word would hide
			// a load that wrongly writes r0.
			memory.fill(0x81abcdef);
			const u32 address = Data + ((seed % 16) * 16) + ((op == 30 || op == 31) ? seed % 16 : 0);
			// Exercise negative displacements, 32-bit address addition and rt == rs.
			const s16 displacement = (seed & 1) ? -32768 : 32752;
			cpuRegs.GPR.r[1].UD[0] = 0xffffffff00000000ULL | u32(address - displacement);
			const u32 rt = seed % 4;
			// The interpreter's LD writes r0 (R5900OpcodeImpl.cpp); native code keeps
			// it zero, as the hardware does.
			if (op == 55 && rt == 0)
				continue;
			program[0] = (op << 26) | (1 << 21) | (rt << 16) | static_cast<u16>(displacement);
			Compare(1);
			if (HasFailure())
				return;
		}
	}
}

TEST_F(EERecompilerTest, LookupCollisionsRemappingAndCacheLifetime)
{
	// These PCs collide in the front cache while naming different instructions.
	constexpr u32 other = Alias + 16;
	Map(Alias, memory.data());
	memory.fill(Stop);
	program[0] = (9u << 26) | (1 << 16) | 11;
	memory[4] = (9u << 26) | (1 << 16) | 22;
	auto run = [&](u32 pc, u32 expected) {
		cpuRegs.pc = pc;
		u32 cycles = 0;
		EXPECT_TRUE(Arm64EE::TryExecute(cycles));
		EXPECT_EQ(cpuRegs.GPR.r[1].UD[0], expected);
		EXPECT_EQ(cpuRegs.pc, pc + 4);
	};
	run(Base, 11);
	run(other, 22);
	const size_t warmed = Arm64EE::GetCommittedCache();
	for (u32 i = 0; i < 32; i++)
	{
		run(Base, 11);
		run(other, 22);
	}
	EXPECT_EQ(Arm64EE::GetCommittedCache(), warmed);
	// A cached PC still validates its mapping and every source word.
	memory[0] = (9u << 26) | (1 << 16) | 33;
	Map(Base, memory.data());
	run(Base, 33);
	memory[0] = (9u << 26) | (1 << 16) | 44;
	run(Base, 44);
	Arm64EE::Reset();
	run(Base, 44);
	Arm64EE::Shutdown();
	run(Base, 44);
}

TEST_F(EERecompilerTest, RejectedLookupRechecksCodeMappingAndCollidingPCs)
{
	Map(Alias, memory.data());
	memory.fill(Stop);
	constexpr u32 other = Alias + 16;
	auto reject = [&](u32 pc) {
		cpuRegs.pc = pc;
		const cpuRegisters before = cpuRegs;
		u32 cycles = 123;
		for (u32 i = 0; i < 3; i++)
		{
			EXPECT_FALSE(Arm64EE::TryExecute(cycles));
			EXPECT_EQ(cycles, 123u);
			EXPECT_EQ(std::memcmp(&before, &cpuRegs, sizeof(before)), 0);
		}
	};
	reject(Base);
	reject(other);
	reject(Base);
	program[0] = (9u << 26) | (1 << 16) | 77;
	cpuRegs.pc = Base;
	Compare(1);
	program[0] = Stop;
	reject(Base);
	memory[0] = (9u << 26) | (1 << 16) | 88;
	Map(Base, memory.data());
	cpuRegs.pc = Base;
	Compare(1);
	EXPECT_EQ(cpuRegs.GPR.r[1].UD[0], 88u);
}

TEST_F(EERecompilerTest, LookupPointersSurviveBackingMapGrowth)
{
	for (u32 i = 0; i < 256; i++)
		program[i * 2] = (9u << 26) | (1 << 16) | (i + 1);
	for (u32 i = 0; i < 256; i++)
	{
		cpuRegs.pc = Base + i * 8;
		u32 cycles = 0;
		ASSERT_TRUE(Arm64EE::TryExecute(cycles));
		EXPECT_EQ(cpuRegs.GPR.r[1].UD[0], i + 1);
	}
	const size_t warmed = Arm64EE::GetCommittedCache();
	cpuRegs.pc = Base;
	u32 cycles = 0;
	ASSERT_TRUE(Arm64EE::TryExecute(cycles));
	EXPECT_EQ(cpuRegs.GPR.r[1].UD[0], 1u);
	EXPECT_EQ(Arm64EE::GetCommittedCache(), warmed);
}

TEST_F(EERecompilerTest, MixedMemoryBlocksAndSelfModifyingAliases)
{
	cpuRegs.GPR.r[1].UD[0] = Data;
	for (u32 i = 0; i < 32; i += 4)
	{
		program[i] = (35u << 26) | (1 << 21) | (2 << 16) | (i * 4); // LW
		program[i + 1] = (9u << 26) | (2 << 21) | (2 << 16) | 17; // ADDIU
		program[i + 2] = (43u << 26) | (1 << 21) | (2 << 16) | (i * 4); // SW
		program[i + 3] = (2 << 21) | (3 << 11) | 37; // OR
	}
	Compare(32);
	// Make a data-address alias of the code page. A store into the next opcode
	// must leave the block before the stale native instruction can execute.
	Map(Data, program.data());
	program.fill(Stop);
	program[0] = (43u << 26) | (1 << 21) | (2 << 16) | 4;
	program[1] = (9u << 26) | (3 << 16) | 7;
	cpuRegs.pc = Base;
	cpuRegs.GPR.r[2].UD[0] = (9u << 26) | (3 << 16) | 42;
	Compare(1);
	Compare(1);
	EXPECT_EQ(cpuRegs.GPR.r[3].UD[0], 42u);
}

TEST_F(EERecompilerTest, MemoryExitsPreserveUnexecutedInstructionAndCycleCharge)
{
	for (u32 scenario = 0; scenario < 3; scenario++)
	{
		Init(0);
		// Unmapped/handler, misaligned RAM, and counter register access.
		const u32 address = scenario == 0 ? Data : scenario == 1 ? Data + 1 :
		                                                           0x10000000;
		if (scenario == 0)
			vtlb_private::vtlbdata.vmap[Data >> 12] = vtlb_private::VTLBVirtual(vtlb_private::VTLBPhysical::fromHandler(0), Data, Data);
		else
			Map(Data, memory.data());
		cpuRegs.GPR.r[1].UD[0] = address;
		program[0] = (9u << 26) | (2 << 16) | 123;
		program[1] = (35u << 26) | (1 << 21) | (3 << 16);
		program[2] = (9u << 26) | (4 << 16) | 456;
		Compare(1);
		const cpuRegisters before = cpuRegs;
		u32 cycles = 123;
		EXPECT_FALSE(Arm64EE::TryExecute(cycles));
		EXPECT_EQ(cycles, 123u);
		EXPECT_EQ(std::memcmp(&before, &cpuRegs, sizeof(cpuRegs)), 0);
	}
}

TEST_F(EERecompilerTest, UnsupportedAndUnsafeEntriesDoNotChangeState)
{
	for (u32 code : {Stop, 0x88220000u, 0x10220000u, 0x46020016u, 0x4002c800u}) // ..., RSQRT.S, MFC0 $25
	{
		program[0] = code;
		const cpuRegisters before = cpuRegs;
		u32 cycles = 123;
		EXPECT_FALSE(Arm64EE::TryExecute(cycles));
		EXPECT_EQ(cycles, 123u);
		EXPECT_EQ(std::memcmp(&before, &cpuRegs, sizeof(cpuRegs)), 0);
	}
	program[0] = 0;
	for (u32 scenario = 0; scenario < 4; scenario++)
	{
		Init(0);
		EmuConfig.Cpu.Recompiler.EnableEE = scenario != 0;
		if (scenario == 1)
			cpuRegs.branch = 1;
		if (scenario == 2)
			cpuRegs.pc++;
		if (scenario == 3)
			vtlb_private::vtlbdata.vmap[Base >> 12] = vtlb_private::VTLBVirtual(vtlb_private::VTLBPhysical::fromHandler(0), Base, Base);
		const cpuRegisters before = cpuRegs;
		u32 cycles = 123;
		EXPECT_FALSE(Arm64EE::TryExecute(cycles));
		EXPECT_EQ(cycles, 123u);
		EXPECT_EQ(std::memcmp(&before, &cpuRegs, sizeof(cpuRegs)), 0);
	}
}

TEST_F(EERecompilerTest, ConditionalBranchesPreserveDelayAndEventBoundaries)
{
	constexpr u64 values[] = {0, 1, ~u64(0), 0x8000000000000000, 0x7fffffffffffffff, 0xffffffff00000000};
	for (u32 op : {4u, 5u, 6u, 7u, 20u, 21u, 22u, 23u})
	{
		for (u32 seed = 0; seed < 72; seed++)
		{
			SCOPED_TRACE(testing::Message() << "opcode=" << op << " seed=" << seed);
			Init(seed);
			program.fill(Stop);
			const u32 prefix = seed % 4;
			for (u32 i = 0; i < prefix; i++)
				program[i] = (9u << 26) | (5 << 21) | (5 << 16) | 1;
			const u32 rt = seed & 1 ? 1 : 2;
			cpuRegs.GPR.r[1].UD[0] = values[seed % std::size(values)];
			cpuRegs.GPR.r[2].UD[0] = values[(seed / std::size(values)) % std::size(values)];
			const s64 lhs = cpuRegs.GPR.r[1].SD[0], rhs = cpuRegs.GPR.r[rt].SD[0];
			const u32 kind = op & 7;
			const bool taken = kind == 4 ? lhs == rhs : kind == 5 ? lhs != rhs :
			                                        kind == 6     ? lhs <= 0 :
			                                                        lhs > 0;
			const s16 displacement = seed & 1 ? -32768 : 32767;
			program[prefix] = (op << 26) | (1 << 21) | (rt << 16) | static_cast<u16>(displacement);
			// Overwrite a condition source in the delay slot after testing it.
			program[prefix + 1] = (9u << 26) | (1 << 16) | 77;
			CompareBranch(prefix, taken, op >= 20, op >= 20 || op == 4 || op == 5,
				Base + prefix * 4 + 4 + displacement * 4);
			if (HasFailure())
				return;
		}
	}
}

// A BNE back to its own containing block's entry pc (ADDIU counter decrement;
// BNE; delay-slot ADDIU) is fast-pathed inside TryExecute() itself -- see the
// loop comment above TryExecute() in EERecompiler.cpp. Drive it the same way
// intExecuteWithBackend does (repeatedly call TryExecute(), manually applying
// intFinishBranch's cpuRegs.branch/pc commit on a TakenBranch exit) and check
// the loop still lands on the exact final state/cycle count a fully manual,
// one-MIPS-instruction-at-a-time replay predicts -- this is what would catch a
// double-counted cycle or a stale/skipped pc commit from the new internal loop.
// needs proper testing across a wider range of games.
TEST_F(EERecompilerTest, SelfLoopingBranchMatchesManualReplay)
{
	for (u32 initial_t0 : {1u, 2u, 5u, 5000u}) // 5000 exercises kMaxChainedBlocks (4096)
	{
		SCOPED_TRACE(testing::Message() << "initial_t0=" << initial_t0);
		Init(0);
		program[0] = (9u << 26) | (1 << 21) | (1 << 16) | 0xffffu; // ADDIU $t0(r1), $t0, -1
		program[1] = (5u << 26) | (1 << 21) | (0 << 16) | 0xfffeu; // BNE $t0, $zero, Base (back to word 0)
		program[2] = (9u << 26) | (2 << 21) | (2 << 16) | 1u;      // delay slot: ADDIU $t1(r2), $t1, 1
		program[3] = Stop; // ExecuteChained runs on past the loop until an unsupported instruction
		cpuRegs.GPR.r[1].UD[0] = initial_t0;
		// Keep the event deadline far away so the internal loop never takes its
		// intEventTest() exit: that path calls into counters/IOP/VMManager state
		// this narrow unit test harness doesn't set up. The event-due exit itself
		// is exercised only by inspection/live testing for now -- needs proper
		// testing with a harness that can run intEventTest() safely. Note
		// EEBranchEventDue() treats the gap as a wrapping s64 difference (see
		// EEBranchPollingTest above), so ~u64(0) reads as "already due", not
		// "far away" -- a merely large value is what actually stays not-due.
		cpuRegs.nextEventCycle = u64(1) << 40;
		const cpuRegisters initial = cpuRegs;
		const u32 scale = 2 - ((cpuRegs.CP0.n.Config >> 18) & 1);
		const u32 addiu_cycles = R5900::GetInstruction(program[0]).cycles;
		const u32 bne_cycles = R5900::GetInstruction(program[1]).cycles;
		const u32 delay_cycles = R5900::GetInstruction(program[2]).cycles;

		// Manual, one-instruction-at-a-time ground truth: every decrement but the
		// last is taken (loops back to Base); the one that brings $t0 to 0 is not.
		// Each taken branch also commits the driver's way (intUpdateCPUCycles() at
		// the default cycle rate): cycle += max(acc >> 3, 1), acc &= 7.
		cpuRegs = initial;
		u32 expected_cycles = 0;
		auto commit = [&expected_cycles]() {
			cpuRegs.cycle += std::max(expected_cycles >> 3, 1u);
			expected_cycles &= 7;
		};
		for (u32 t0 = initial_t0;;)
		{
			cpuRegs.code = program[0];
			R5900::GetCurrentInstruction().interpret(); // ADDIU $t0, $t0, -1
			expected_cycles += addiu_cycles * scale;
			t0--;
			expected_cycles += bne_cycles * scale;
			if (t0 != 0)
			{
				cpuRegs.code = program[2];
				R5900::GetCurrentInstruction().interpret(); // delay slot
				expected_cycles += delay_cycles * scale;
				commit();
			}
			else
			{
				// Untaken: the branch leaves the delay slot's word for the next
				// block, which ExecuteChained runs as an ordinary instruction before
				// stopping at the SYSCALL.
				cpuRegs.code = program[2];
				R5900::GetCurrentInstruction().interpret();
				expected_cycles += delay_cycles * scale;
				cpuRegs.pc = Base + 12;
				break;
			}
		}
		const cpuRegisters expected = cpuRegs;

		// Native, driven exactly like intExecuteWithBackend drives the provider.
		cpuRegs = initial;
		u32 native_cycles = 0;
		EEBlockResult result;
		u32 outer_calls = 0;
		do
		{
			result = Arm64EE::ExecuteChained(native_cycles);
			if (result.exit == EEBlockExit::TakenBranch)
			{
				cpuRegs.branch = 1;
				cpuRegs.pc = result.target;
				cpuRegs.branch = 0;
			}
			ASSERT_LT(++outer_calls, 10u);
		} while (result);

		// ExecuteChained commits cycles into the counter it was given (in
		// production the driver's cpuBlockCycles), both in C++ and in linked
		// native exits, so cpuRegs.cycle is comparable here too.
		EXPECT_EQ(native_cycles, expected_cycles);
		EXPECT_EQ(cpuRegs.cycle, expected.cycle);
		EXPECT_EQ(std::memcmp(&cpuRegs, &expected, sizeof(cpuRegisters)), 0);
		if (HasFailure())
			return;
	}
}

// ExecuteChained runs blocks back to back, doing the driver's branch commit
// itself. Two blocks that jump to each other must end exactly where running
// one block per TryExecute() call with that commit in between ends.
TEST_F(EERecompilerTest, ChainedBlocksMatchOneBlockPerCall)
{
	for (u32 initial_t0 : {1u, 3u, 3000u}) // 3000 round trips cross kMaxChainedBlocks
	{
		SCOPED_TRACE(testing::Message() << "initial_t0=" << initial_t0);
		Init(0);
		program[0] = (9u << 26) | (1 << 21) | (1 << 16) | 0xffffu; // ADDIU $1, $1, -1
		program[1] = (2u << 26) | ((Base + 32) >> 2); // J Base+32
		program[2] = (9u << 26) | (2 << 21) | (2 << 16) | 1u; // delay: ADDIU $2, $2, 1
		program[8] = (9u << 26) | (3 << 21) | (3 << 16) | 2u; // ADDIU $3, $3, 2
		program[9] = (5u << 26) | (1 << 21) | (0 << 16) | 0xfff6u; // BNE $1, $0, Base
		program[10] = (9u << 26) | (4 << 21) | (4 << 16) | 1u; // delay: ADDIU $4, $4, 1
		program[3] = Stop;
		program[11] = Stop; // both runs end here, after the untaken BNE's next word
		cpuRegs.GPR.r[1].UD[0] = initial_t0;
		// Keep the event deadline away, as in SelfLoopingBranchMatchesManualReplay.
		cpuRegs.nextEventCycle = u64(1) << 40;
		const cpuRegisters initial = cpuRegs;

		auto run = [&](bool chained, u32& cycles) {
			cpuRegs = initial;
			cycles = 0;
			EEBlockResult result;
			u32 calls = 0;
			do
			{
				result = chained ? Arm64EE::ExecuteChained(cycles) : Arm64EE::TryExecute(cycles);
				if (result.exit == EEBlockExit::TakenBranch)
				{
					cpuRegs.branch = 1;
					cpuRegs.pc = result.target;
					cpuRegs.branch = 0;
					// The driver's intUpdateCPUCycles() at the default cycle rate.
					cpuRegs.cycle += std::max(cycles >> 3, 1u);
					cycles &= 7;
				}
				ASSERT_LT(++calls, 10000u);
			} while (result);
			if (!chained)
				ASSERT_GT(calls, 2 * initial_t0 - 1);
		};
		u32 stepped_cycles, chained_cycles;
		run(false, stepped_cycles);
		if (HasFatalFailure())
			return;
		cpuRegisters stepped = cpuRegs;
		run(true, chained_cycles);
		if (HasFatalFailure())
			return;
		EXPECT_EQ(chained_cycles, stepped_cycles);
		EXPECT_EQ(cpuRegs.GPR.r[3].UL[0], initial.GPR.r[3].UL[0] + 2 * initial_t0); // ran every round trip
		EXPECT_EQ(cpuRegs.pc, Base + 44); // stopped at the SYSCALL
		EXPECT_EQ(cpuRegs.cycle, stepped.cycle);
		EXPECT_EQ(std::memcmp(&cpuRegs, &stepped, sizeof(cpuRegisters)), 0);
	}
}

TEST_F(EERecompilerTest, RegimmBranchesAndLinkSourceAliasing)
{
	for (u32 rt : {0u, 1u, 2u, 3u, 16u, 17u, 18u, 19u})
	{
		for (u32 seed = 0; seed < 1; seed++)
		{
			SCOPED_TRACE(testing::Message() << "regimm=" << rt << " seed=" << seed);
			Init(seed);
			const u32 rs = seed & 1 ? 31 : 1;
			const u32 link = rt & 16 ? 31 : 0;
			const s64 value = link && rs == 31 ? Base + 8 : cpuRegs.GPR.r[rs].SD[0];
			const bool taken = rt & 1 ? value >= 0 : value < 0;
			program[0] = (1u << 26) | (rs << 21) | (rt << 16) | 0xfffc;
			program[1] = (25u << 26) | (31 << 21) | (31 << 16) | 1;
			CompareBranch(0, taken, rt & 2, rt & 2, Base + 4 - 16, link);
			if (HasFailure())
				return;
		}
	}
}

TEST_F(EERecompilerTest, JumpTargetsLinksAndAliasedSources)
{
	for (u32 seed = 0; seed < 1; seed++)
	{
		for (u32 function : {8u, 9u})
		{
			for (u32 rd : {0u, 1u, 31u})
			{
				Init(seed);
				const u32 target = cpuRegs.GPR.r[1].UL[0];
				program[0] = (1 << 21) | (rd << 11) | function;
				program[1] = (25u << 26) | (1 << 21) | (1 << 16) | 7;
				CompareBranch(0, true, false, false, target, function == 9 ? rd : 0);
				if (HasFailure())
					return;
			}
		}
	}
	for (u32 op : {2u, 3u})
	{
		for (u32 pc : {Base, 0xfffffff8u})
		{
			Init(0);
			cpuRegs.pc = pc;
			Map(0xfffff000, program.data());
			const u32 index = (pc & 4095) / 4;
			program[index] = (op << 26) | (0x01234560 >> 2);
			program[index + 1] = (25u << 26) | (31 << 21) | (1 << 16) | 3;
			CompareBranch(0, true, false, false, ((pc + 4) & 0xf0000000u) | 0x01234560, op == 3 ? 31 : 0);
			if (HasFailure())
				return;
		}
	}
}

TEST_F(EERecompilerTest, EveryIntegerDelaySlotPreservesJumpTarget)
{
	for (u32 seed = 0; seed < 1; seed++)
	{
		for (u32 op : Special)
		{
			Init(seed);
			program[0] = (1 << 21) | 8; // JR r1
			program[1] = (1 << 21) | (2 << 16) | (1 << 11) | (seed << 6) | op;
			CompareBranch(0, true, false, false, cpuRegs.GPR.r[1].UL[0]);
			if (HasFailure())
				return;
		}
		for (u32 op : Immediate)
		{
			Init(seed);
			program[0] = (1 << 21) | 8;
			program[1] = (op << 26) | (1 << 21) | (1 << 16) | 0xabcd;
			CompareBranch(0, true, false, false, cpuRegs.GPR.r[1].UL[0]);
			if (HasFailure())
				return;
		}
	}
}

TEST_F(EERecompilerTest, BranchDelaySourceChangesAndFallback)
{
	program[0] = (3u << 26) | ((Base + 128) >> 2);
	for (u32 delay : {Stop, 0x8c220000u, 0x10000000u, 0x00221820u})
	{
		Init(0);
		program[1] = delay;
		const cpuRegisters before = cpuRegs;
		u32 cycles = 123;
		for (u32 visit = 0; visit < 2; visit++)
		{
			EXPECT_FALSE(Arm64EE::TryExecute(cycles));
			EXPECT_EQ(cycles, 123u);
			EXPECT_EQ(std::memcmp(&before, &cpuRegs, sizeof(cpuRegs)), 0);
		}
		// Previously rejected blocks must become eligible after code is patched.
		program[1] = (9u << 26) | (1 << 16) | 77;
		CompareBranch(0, true, false, false, Base + 128, 31);
		program[1] = (9u << 26) | (1 << 16) | 99;
		cpuRegs.pc = Base;
		CompareBranch(0, true, false, false, Base + 128, 31);
	}
}

TEST_F(EERecompilerTest, BranchAndDelayMustFitBlockAndPage)
{
	program.fill(0);
	program[31] = (2u << 26) | (Base >> 2);
	Compare(31);
	u32 cycles = 0;
	EXPECT_EQ(Arm64EE::TryExecute(cycles).exit, EEBlockExit::TakenBranch);
	program[30] = program[31];
	program[31] = 0;
	cpuRegs.pc = Base;
	CompareBranch(30, true, false, false, Base);
	program[1023] = program[30];
	cpuRegs.pc = Base + 4092;
	const cpuRegisters before = cpuRegs;
	cycles = 123;
	EXPECT_FALSE(Arm64EE::TryExecute(cycles));
	EXPECT_EQ(cycles, 123u);
	EXPECT_EQ(std::memcmp(&before, &cpuRegs, sizeof(cpuRegs)), 0);
}

TEST_F(EERecompilerTest, GoemonSettingInvalidatesCachedBranchBlocks)
{
	for (u32 branch : {(3u << 26) | (0x3563b8 >> 2), (1u << 21) | 8})
	{
		Init(0);
		program.fill(Stop);
		program[0] = 0; // exercise a cached branch beyond the entry instruction
		program[1] = branch;
		program[2] = 0;
		cpuRegs.GPR.r[1].UD[0] = 0x33ad48;
		EmuConfig.Gamefixes.GoemonTlbHack = false;
		u32 cycles = 0;
		ASSERT_EQ(Arm64EE::TryExecute(cycles).exit, EEBlockExit::TakenBranch);
		EmuConfig.Gamefixes.GoemonTlbHack = true;
		cpuRegs.pc = Base;
		Compare(1);
		const cpuRegisters before = cpuRegs;
		EXPECT_FALSE(Arm64EE::TryExecute(cycles));
		EXPECT_EQ(std::memcmp(&before, &cpuRegs, sizeof(cpuRegs)), 0);
		EmuConfig.Gamefixes.GoemonTlbHack = false;
		EXPECT_EQ(Arm64EE::TryExecute(cycles).exit, EEBlockExit::TakenBranch);
	}
}

TEST_F(EERecompilerTest, MemoryExitsBeforeBranchDoNotExecuteLinkOrDelay)
{
	program[0] = (35u << 26) | (1 << 21) | (2 << 16);
	program[1] = (3u << 26) | (Base >> 2);
	program[2] = (9u << 26) | (3 << 16) | 77;
	cpuRegs.GPR.r[1].UD[0] = Data + 1; // misaligned LW exits before the whole block
	const cpuRegisters before = cpuRegs;
	u32 cycles = 123;
	EXPECT_FALSE(Arm64EE::TryExecute(cycles));
	EXPECT_EQ(cycles, 123u);
	EXPECT_EQ(std::memcmp(&before, &cpuRegs, sizeof(cpuRegs)), 0);
	cpuRegs.GPR.r[1].UD[0] = Data;
	CompareBranch(1, true, false, false, Base, 31);
}

TEST_F(EERecompilerTest, SelfModifyingStoreInvalidatesCompiledDelaySlot)
{
	Map(Data, program.data());
	program[0] = (43u << 26) | (1 << 21) | (2 << 16) | 8;
	program[1] = (3u << 26) | (Base >> 2);
	program[2] = (9u << 26) | (3 << 16) | 77;
	cpuRegs.GPR.r[1].UD[0] = Data;
	cpuRegs.GPR.r[2].UD[0] = (9u << 26) | (3 << 16) | 99;
	Compare(1); // store must stop before the compiled branch or stale delay slot
	CompareBranch(0, true, false, false, Base, 31);
	EXPECT_EQ(cpuRegs.GPR.r[3].UD[0], 99u);
}

TEST_F(EERecompilerTest, CachedEntryPatchedToUnsupportedInstruction)
{
	program[0] = (9u << 26) | (1 << 16) | 77;
	Compare(1);
	cpuRegs.pc = Base;
	program[0] = Stop;
	const cpuRegisters before = cpuRegs;
	u32 cycles = 123;
	for (u32 visit = 0; visit < 2; visit++)
	{
		EXPECT_FALSE(Arm64EE::TryExecute(cycles));
		EXPECT_EQ(cycles, 123u);
		EXPECT_EQ(std::memcmp(&before, &cpuRegs, sizeof(cpuRegs)), 0);
	}
	program[0] = (9u << 26) | (1 << 16) | 99;
	Compare(1);
}

TEST_F(EERecompilerTest, HiLoOperationsMatchInterpreterAtEdgesAndAliases)
{
	constexpr u64 values[] = {0, 1, 2, ~u64(0), 0x80000000, 0x7fffffff,
		0xffffffff, 0x1234567800000000, 0xdeadbeef00000001,
		0xffffffff80000000, 0x55555555, 0xaaaaaaaa};
	for (u32 code : HiLoInstructions)
	{
		for (u32 lhs = 0; lhs < std::size(values); lhs++)
		{
			for (u32 rhs = 0; rhs < std::size(values); rhs++)
			{
				for (u32 alias = 0; alias < 4; alias++)
				{
					SCOPED_TRACE(testing::Message() << "code=" << code << " lhs=" << lhs << " rhs=" << rhs << " alias=" << alias);
					InitHiLo(lhs + rhs);
					const u32 rs = alias == 3 ? 0 : 1, rt = alias == 2 ? rs : 2;
					const u32 rd = alias == 0 ? 0 : alias == 1 ? rs :
					                                             rt;
					if (rs)
						cpuRegs.GPR.r[rs].UD[0] = values[lhs];
					if (rt)
						cpuRegs.GPR.r[rt].UD[0] = values[rhs];
					program[0] = code | (rs << 21) | (rt << 16) | (rd << 11);
					Compare(1);
					if (HasFailure())
						return;
				}
			}
		}
	}
}

TEST_F(EERecompilerTest, MultiplyAccumulateWrapsAndUsesOnlyLowAccumulatorWords)
{
	constexpr u64 accumulators[] = {0, 1, ~u64(0), 0x7fffffffffffffff, 0x8000000000000000, 0xffffffff00000000};
	constexpr u32 values[] = {0, 1, 2, 0xffffffff, 0x80000000, 0x7fffffff};
	for (u32 function : {0u, 1u, 32u, 33u})
	{
		const u32 bank = function >= 32 ? 1 : 0;
		for (u64 accumulator : accumulators)
		{
			for (u32 lhs : values)
			{
				for (u32 rhs : values)
				{
					SCOPED_TRACE(testing::Message() << "function=" << function << " accumulator=" << accumulator << " lhs=" << lhs << " rhs=" << rhs);
					InitHiLo(lhs ^ rhs);
					cpuRegs.HI.UD[bank] = 0xabcdef0100000000ULL | (accumulator >> 32);
					cpuRegs.LO.UD[bank] = 0x1234567800000000ULL | static_cast<u32>(accumulator);
					cpuRegs.GPR.r[1].UD[0] = lhs;
					cpuRegs.GPR.r[2].UD[0] = rhs;
					const u64 product = function & 1 ? u64(lhs) * rhs :
					                                   static_cast<u64>(s64(static_cast<s32>(lhs)) * static_cast<s32>(rhs));
					const u64 expected = accumulator + product; // defined unsigned wrap
					program[0] = (28u << 26) | (1 << 21) | (2 << 16) | (3 << 11) | function;
					Compare(1);
					EXPECT_EQ(cpuRegs.LO.SD[bank], static_cast<s32>(expected));
					EXPECT_EQ(cpuRegs.HI.SD[bank], static_cast<s32>(expected >> 32));
					EXPECT_EQ(cpuRegs.GPR.r[3].UD[0], cpuRegs.LO.UD[bank]);
					if (HasFailure())
						return;
				}
			}
		}
	}
}

TEST_F(EERecompilerTest, HiLoDependenciesAcrossMixedBlocksAndMemoryExit)
{
	for (u32 seed = 0; seed < 128; seed++)
	{
		SCOPED_TRACE(seed);
		InitHiLo(seed);
		for (u32 i = 0; i < 32; i++)
		{
			const u32 rs = 1 + (i % 3), rt = 1 + ((i + 1) % 3), rd = (i + 2) % 4;
			program[i] = HiLoInstructions[(i + seed) % std::size(HiLoInstructions)] | (rs << 21) | (rt << 16) | (rd << 11);
			if (i % 5 == 4)
				program[i] = (9u << 26) | (rs << 21) | (rt << 16) | 0x8765;
		}
		Compare(32);
		if (HasFailure())
			return;
	}
	InitHiLo(0);
	program.fill(Stop);
	program[0] = (1 << 21) | (2 << 16) | 24; // MULT with rd == zero still updates HI/LO
	program[1] = (28u << 26) | (1 << 21) | (2 << 16) | 24; // MULT1 preserves bank zero
	program[2] = (35u << 26) | (4 << 21) | (3 << 16); // misaligned load exits after both multiplies
	cpuRegs.GPR.r[4].UD[0] = Data + 1;
	Compare(2);
	program[2] = (3 << 11) | 18; // MFLO resumes with bank-zero result
	Compare(1);
}

TEST_F(EERecompilerTest, HiLoDelaySlotsPreserveTargetsLinksAndAnnulment)
{
	for (u32 code : HiLoInstructions)
	{
		for (u32 seed = 0; seed < 1; seed++)
		{
			SCOPED_TRACE(testing::Message() << "code=" << code << " seed=" << seed);
			InitHiLo(seed);
			program[0] = (1 << 21) | (1 << 11) | 9; // JALR captures target before link/slot writes
			program[1] = code | (1 << 21) | (2 << 16) | (1 << 11);
			CompareBranch(0, true, false, false, cpuRegs.GPR.r[1].UL[0], 1);
			if (HasFailure())
				return;
			InitHiLo(seed);
			cpuRegs.GPR.r[1].UD[0] = 1;
			program[0] = (20u << 26) | (1 << 16); // BEQL zero, r1 annuls the HI/LO instruction
			CompareBranch(0, false, true, true, Base + 4);
			if (HasFailure())
				return;
		}
	}
}

TEST_F(EERecompilerTest, PackedIntegersMatchInterpreterWithFullWidthAliases)
{
	constexpr u32 operands[][3] = {{1, 2, 3}, {1, 2, 1}, {1, 2, 2}, {1, 1, 1},
		{0, 2, 3}, {1, 0, 3}, {1, 2, 0}, {0, 0, 31}};
	for (u32 code : PackedInstructions)
	{
		for (const auto& regs : operands)
		{
			program[0] = code | (regs[0] << 21) | (regs[1] << 16) | (regs[2] << 11);
			for (u32 seed = 0; seed < 128; seed++)
			{
				SCOPED_TRACE(testing::Message() << "code=" << program[0] << " seed=" << seed);
				InitPacked(seed);
				Compare(1);
				if (HasFailure())
					return;
			}
		}
	}
}

TEST_F(EERecompilerTest, PackedIntegerDependenciesAndQuadwordTransfers)
{
	for (u32 seed = 0; seed < 128; seed++)
	{
		InitPacked(seed);
		for (u32 i = 0; i < 32; i++)
		{
			const u32 rs = 1 + i % 3, rt = 1 + (i + 1) % 3, rd = 1 + (i + 2) % 3;
			program[i] = PackedInstructions[(i + seed) % std::size(PackedInstructions)] | (rs << 21) | (rt << 16) | (rd << 11);
			if (i % 5 == 4)
				program[i] = (9u << 26) | (rs << 21) | (rd << 16) | 0xffef;
		}
		Compare(32);
		if (HasFailure())
			return;
	}
	for (u32 code : PackedInstructions)
	{
		InitPacked(100);
		program.fill(Stop);
		cpuRegs.GPR.r[4].UD[0] = Data;
		std::memcpy(memory.data(), &cpuRegs.GPR.r[1], sizeof(GPR_reg) * 2);
		program[0] = (30u << 26) | (4 << 21) | (1 << 16) | 3; // LQ aligns down
		program[1] = (30u << 26) | (4 << 21) | (2 << 16) | 16;
		program[2] = code | (1 << 21) | (2 << 16) | (1 << 11);
		program[3] = (31u << 26) | (4 << 21) | (1 << 16) | 32; // SQ stores full packed result
		Compare(4);
		if (HasFailure())
			return;
	}
}

TEST_F(EERecompilerTest, PackedDelaySlotsPreserveRegisterTargetsAndAnnulment)
{
	for (u32 code : PackedInstructions)
	{
		for (u32 seed = 0; seed < 64; seed++)
		{
			SCOPED_TRACE(testing::Message() << "code=" << code << " seed=" << seed);
			InitPacked(seed);
			program[0] = (1 << 21) | (1 << 11) | 9; // JALR must capture target before full-width slot write
			program[1] = code | (1 << 21) | (2 << 16) | (1 << 11);
			CompareBranch(0, true, false, false, cpuRegs.GPR.r[1].UL[0], 1);
			if (HasFailure())
				return;
			InitPacked(seed);
			cpuRegs.GPR.r[1].UD[0] = 1;
			program[0] = (20u << 26) | (1 << 16); // untaken BEQL annuls the packed operation
			CompareBranch(0, false, true, true, Base + 4);
			if (HasFailure())
				return;
		}
	}
}

TEST_F(EERecompilerTest, UnsupportedPackedSelectorsRemainInterpreted)
{
	for (u32 function : {8u, 9u, 40u, 41u})
	{
		for (u32 selector = 0; selector < 32; selector++)
		{
			const u32 code = PackedCode(function, selector);
			if (std::find(std::begin(PackedInstructions), std::end(PackedInstructions), code) != std::end(PackedInstructions) ||
				code == PackedCode(40, 27) || code == PackedCode(9, 28) || code == PackedCode(41, 27)) // QFSRV, PMULTH, PCPYH
				continue;
			SCOPED_TRACE(testing::Message() << "function=" << function << " selector=" << selector);
			InitPacked(0);
			program[0] = code | (1 << 21) | (2 << 16) | (3 << 11);
			const cpuRegisters before = cpuRegs;
			u32 cycles = 123;
			EXPECT_FALSE(Arm64EE::TryExecute(cycles));
			EXPECT_EQ(cycles, 123u);
			EXPECT_EQ(std::memcmp(&before, &cpuRegs, sizeof(cpuRegs)), 0);
		}
	}
}

TEST_F(EERecompilerTest, PackedNativeExecutionPreservesHostFloatingPointStatus)
{
	for (u32 code : PackedInstructions)
	{
		InitPacked(0);
		program[0] = code | (1 << 21) | (2 << 16) | (3 << 11);
		u32 cycles = 0;
		ASSERT_TRUE(Arm64EE::TryExecute(cycles)); // compile before setting host status
		for (u64 status : {0ULL, 0x0800009fULL})
		{
			cpuRegs.pc = Base;
			u64 saved, actual;
			asm volatile("mrs %0, fpsr" : "=r"(saved));
			asm volatile("msr fpsr, %0" : : "r"(status));
			const EEBlockResult result = Arm64EE::TryExecute(cycles);
			asm volatile("mrs %0, fpsr" : "=r"(actual));
			asm volatile("msr fpsr, %0" : : "r"(saved));
			EXPECT_TRUE(result);
			EXPECT_EQ(actual, status);
		}
	}
}

TEST_F(EERecompilerTest, COP2RegisterTransfersMatchInterpreter)
{
	// rs selects QMFC2(1)/CFC2(2)/QMTC2(5)/CTC2(6); bit 0 of the code word only
	// gates a vu0Sync()/_vu0FinishMicro() wait that InitVU0's cleared VPU_STAT
	// already makes a no-op, so both settings are exercised for coverage.
	constexpr u32 rs_values[] = {1, 2, 5, 6};
	for (u32 seed = 0; seed < 32; seed++)
	{
		const u32 rt = 1 + seed % 4, fs = 1 + (seed / 4) % 4;
		for (u32 rs : rs_values)
		{
			for (u32 bit0 : {0u, 1u})
			{
				SCOPED_TRACE(testing::Message() << "rs=" << rs << " seed=" << seed << " bit0=" << bit0);
				Init(seed);
				InitVU0(seed);
				program[0] = (18u << 26) | (rs << 21) | (rt << 16) | (fs << 11) | bit0;
				CompareWithVU0(1);
				if (HasFailure())
					return;
			}
		}
	}
}

TEST_F(EERecompilerTest, COP2MacroArithmeticMatchesInterpreter)
{
	// rs bit 4 (i.e. rs >= 16) reaches COP2_SPECIAL; funct 39 is VADD, an
	// ordinary full-vector add with no broadcast-lane selector bits.
	constexpr u32 funct = 39;
	for (u32 seed = 0; seed < 1; seed++)
	{
		const u32 fd = 1 + seed % 4, fs = 1 + (seed / 4) % 4, ft = 1 + (seed / 16) % 4;
		SCOPED_TRACE(testing::Message() << "seed=" << seed);
		Init(seed);
		InitVU0(seed);
		program[0] = (18u << 26) | (16u << 21) | (ft << 16) | (fs << 11) | (fd << 6) | funct;
		CompareWithVU0(1);
		if (HasFailure())
			return;
	}
}

TEST_F(EERecompilerTest, COP2MacroMultiplyAccumulateMatchesInterpreter)
{
	// Every V{ADD,SUB,MUL,MADD,MSUB}[A][bc|i|q] runs natively while VU0 is idle.
	// Operands include signed zeros, denormals, Inf/NaN, Fmax and values whose
	// sums/products overflow or underflow, under both overflow clamp settings
	// and the FPCR modes the interpreter also runs with.
	constexpr u32 ops[] = {0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 24, 25, 26, 27, 28, 30,
		32, 33, 34, 35, 36, 37, 38, 39, 40, 41, 42, 44, 45};
	constexpr u32 values[] = {0x00000000, 0x80000000, 0x00000001, 0x807fffff, 0x00800000, 0x3f800000,
		0xbf800000, 0x7f7fffff, 0xff7fffff, 0x7f800000, 0xff800000, 0x7fc00000, 0x1f800000, 0x5f800000,
		0xdf000000, 0x40490fdb};
	constexpr u32 masks[] = {15, 0, 8, 1, 7, 10};
	constexpr u64 fpcrs[] = {0, 1u << 24, (1u << 24) | (3u << 22)};
	u64 saved_fpcr;
	asm volatile("mrs %0, fpcr" : "=r"(saved_fpcr));
	for (u64 fpcr : fpcrs)
	{
		asm volatile("msr fpcr, %0" : : "r"(fpcr));
		for (bool clamp : {true, false})
		{
			EmuConfig.Cpu.Recompiler.vu0Overflow = clamp;
			for (bool accumulator : {false, true})
			{
				for (u32 op : ops)
				{
					for (u32 seed = 0; seed < 12; seed++)
					{
						const u32 mask = masks[seed % std::size(masks)];
						// fd 0, fs == ft and fd aliasing a source all appear.
						const u32 fd = seed % 5, fs = 1 + seed % 3, ft = seed % 4 == 3 ? fs : 1 + (seed / 3) % 4;
						SCOPED_TRACE(testing::Message() << "op=" << op << " acc=" << accumulator << " mask=" << mask
														<< " clamp=" << clamp << " fpcr=" << fpcr << " seed=" << seed);
						Init(seed);
						InitVU0(seed);
						for (u32 reg = 1; reg < 32; reg++)
							for (u32 lane = 0; lane < 4; lane++)
								if (seed < 8)
									VU0.VF[reg].UL[lane] = values[(seed * 7 + reg * 5 + lane * 3) % std::size(values)];
						for (u32 lane = 0; lane < 4; lane++)
							VU0.ACC.UL[lane] = values[(seed * 3 + lane * 5 + 1) % std::size(values)];
						VU0.VI[REG_I].UL = values[(seed + 5) % std::size(values)];
						VU0.VI[REG_Q].UL = values[(seed * 11 + 2) % std::size(values)];
						VU0.macflag = 0xa5a50000 | seed;
						VU0.VI[REG_STATUS_FLAG].UL = 0xfff;
						const u32 operation = accumulator ? ((op >> 2) << 6) | 0x3c | (op & 3) : (fd << 6) | op;
						program[0] = (9u << 26) | (1u << 21) | (2u << 16) | 5; // ADDIU r2, r1, 5 (cached)
						program[1] = (18u << 26) | (1u << 25) | (mask << 21) | (ft << 16) | (fs << 11) | operation;
						program[2] = (33u << 0) | (2u << 21) | (1u << 16) | (4u << 11); // ADDU r4, r2, r1
						CompareWithVU0(3);
						if (HasFailure())
						{
							asm volatile("msr fpcr, %0" : : "r"(saved_fpcr));
							return;
						}
					}
				}
			}
		}
	}
	asm volatile("msr fpcr, %0" : : "r"(saved_fpcr));
}

TEST_F(EERecompilerTest, COP2InterpreterCallsKeepCachedRegisters)
{
	// Ops left to the interpreter are called with the block's registers saved,
	// so cached GPRs survive them; QMFC2/CFC2 write rt, which must be reloaded.
	const u32 cop2[] = {
		(18u << 26) | (1u << 25) | (15u << 21) | (3u << 16) | (2u << 11) | (1u << 6) | 43, // VMAX
		(18u << 26) | (1u << 25) | (14u << 21) | (3u << 16) | (2u << 11) | (12u << 6) | 0x3c, // VMOVE
		(18u << 26) | (1u << 25) | (15u << 21) | (3u << 16) | (2u << 11) | (7u << 6) | 0x3c | 3, // VCLIPw
		(18u << 26) | (1u << 21) | (2u << 16) | (3u << 11), // QMFC2 r2, vf3
		(18u << 26) | (2u << 21) | (2u << 16) | (5u << 11), // CFC2 r2, vi5
		(18u << 26) | (5u << 21) | (2u << 16) | (3u << 11), // QMTC2 r2, vf3
		(18u << 26) | (6u << 21) | (2u << 16) | (5u << 11), // CTC2 r2, vi5
	};
	for (u32 code : cop2)
	{
		for (u32 seed = 0; seed < 4; seed++)
		{
			SCOPED_TRACE(testing::Message() << "code=" << std::hex << code << " seed=" << seed);
			Init(seed);
			InitVU0(seed);
			program[0] = (9u << 26) | (1u << 21) | (2u << 16) | 5; // ADDIU r2, r1, 5
			program[1] = (9u << 26) | (3u << 21) | (4u << 16) | 7; // ADDIU r4, r3, 7
			program[2] = code;
			program[3] = (33u << 0) | (2u << 21) | (4u << 16) | (5u << 11); // ADDU r5, r2, r4
			program[4] = (33u << 0) | (1u << 21) | (3u << 16) | (6u << 11); // ADDU r6, r1, r3
			CompareWithVU0(5);
			if (HasFailure())
				return;
		}
	}
	// The Tri-Ace fix keeps VADDi on the interpreter.
	EmuConfig.Gamefixes.VuAddSubHack = true;
	Init(0);
	InitVU0(0);
	program[0] = (9u << 26) | (1u << 21) | (2u << 16) | 5;
	program[1] = (18u << 26) | (1u << 25) | (15u << 21) | (2u << 11) | (1u << 6) | 34; // VADDi
	program[2] = (33u << 0) | (2u << 21) | (1u << 16) | (4u << 11);
	program[3] = Stop;
	CompareWithVU0(3);
}

TEST_F(EERecompilerTest, InterpreterCallsMatchInterpreter)
{
	// SYSCALL, CACHE and COP0's MTC0/ERET/EI/DI call the interpreter handler
	// from inside the block. MTC0 (except Config), EI and DI let the block go
	// on with its cached GPRs; the rest end it with the pc the handler left.
	constexpr u32 addiu_r2 = (9u << 26) | (1u << 21) | (2u << 16) | 5; // ADDIU r2, r1, 5
	constexpr u32 addu_r4 = (33u << 0) | (2u << 21) | (1u << 16) | (4u << 11); // ADDU r4, r2, r1
	auto mtc0 = [](u32 rt, u32 rd) { return (16u << 26) | (4u << 21) | (rt << 16) | (rd << 11); };
	constexpr u32 eret = 0x42000018, ei = 0x42000038, di = 0x42000039, syscall = 0x0000000c;
	constexpr u32 cache = (47u << 26) | (1u << 21) | (0x1a << 16) | 0x40; // CACHE DHIN, 0x40(r1)
	struct Case
	{
		std::array<u32, 6> words;
		u32 count;
	};
	const Case cases[] = {
		{{addiu_r2, mtc0(2, 14), ei, di, mtc0(3, 9), addu_r4}, 6}, // EPC, EI, DI, Count
		{{addiu_r2, mtc0(2, 12), addu_r4, mtc0(5, 12), addu_r4, Stop}, 5}, // Status
		{{addiu_r2, mtc0(2, 16), addu_r4, Stop}, 2}, // Config ends the block
		{{addiu_r2, syscall, addu_r4, Stop}, 2},
		{{addiu_r2, eret, addu_r4, Stop}, 2},
		{{addiu_r2, cache, addu_r4, Stop}, 2},
		{{mtc0(0, 25), mtc0(6, 25) | 1, mtc0(6, 25) | 3, addu_r4, Stop}, 4}, // MTPS, MTPC0, MTPC1
	};
	for (u32 index = 0; index < std::size(cases); index++)
	{
		for (u32 seed = 0; seed < 8; seed++)
		{
			SCOPED_TRACE(testing::Message() << "case=" << index << " seed=" << seed);
			Init(seed);
			cpuRegs.GPR.r[1].UD[0] = Data;
			cpuRegs.GPR.n.v1.UD[0] = seed & 1 ? 0x05 : 0x7e; // RFU005 or an unknown call: plain exceptions
			cpuRegs.CP0.n.Status.val = (seed & 2) ? 0x70030c13 : 0x00000002; // EXL set; ERL too for odd pairs
			if (seed & 4)
				cpuRegs.CP0.n.Status.val |= 4; // ERL
			cpuRegs.CP0.n.EPC = Base + 0x40;
			cpuRegs.CP0.n.ErrorEPC = Base + 0x80;
			cpuRegs.CP0.n.Config = (seed & 1) << 18;
			cpuRegs.GPR.r[5].UD[0] = 0x70030c13 ^ seed;
			cpuRegs.GPR.r[6].UD[0] = 0x80000000 | seed;
			std::copy(cases[index].words.begin(), cases[index].words.end(), program.begin());
			Compare(cases[index].count);
			if (HasFailure())
				return;
			program.fill(Stop);
		}
	}
}

TEST_F(EERecompilerTest, COP2SurroundingIntegerCodeStaysNative)
{
	// A COP2 instruction no longer has to end the native block: integer code
	// before and after it should still execute without dropping to the
	// interpreter for the whole remainder of the block.
	Init(0);
	InitVU0(0);
	program[0] = (9u << 26) | (1u << 21) | (2u << 16) | 5; // ADDIU r2, r1, 5
	program[1] = (18u << 26) | (5u << 21) | (2u << 16) | (3u << 11); // QMTC2 r2, vf3
	program[2] = (9u << 26) | (2u << 21) | (4u << 16) | 7; // ADDIU r4, r2, 7
	CompareWithVU0(3);
}

TEST_F(EERecompilerTest, COP2BranchRemainsInterpreted)
{
	// BC2 (rs == 8) is a branch this codegen does not model; it must keep
	// ending the native block rather than being treated as ordinary COP2.
	Init(0);
	InitVU0(0);
	program[0] = (18u << 26) | (8u << 21); // BC2F, offset 0
	program[1] = 0; // SLL r0, r0, 0 (NOP) delay slot
	const cpuRegisters before = cpuRegs;
	u32 cycles = 123;
	EXPECT_FALSE(Arm64EE::TryExecute(cycles));
	EXPECT_EQ(cycles, 123u);
	EXPECT_EQ(std::memcmp(&before, &cpuRegs, sizeof(cpuRegs)), 0);
}

TEST_F(EERecompilerTest, COP1RegisterTransfersMatchInterpreter)
{
	// rs selects MFC1(0)/CFC1(2)/MTC1(4)/CTC1(6). CFC1/CTC1 only special-case
	// fs==31 (FCR31) and, for CFC1 only, fs==0 (FCR0); everything else reads
	// as zero and non-31 CTC1 writes are dropped, so fs is swept across all
	// three cases (0, 31, and an arbitrary other register).
	constexpr u32 rs_values[] = {0, 2, 4, 6};
	constexpr u32 fs_values[] = {0, 5, 31};
	for (u32 seed = 0; seed < 16; seed++)
	{
		const u32 rt = 1 + seed % 4;
		for (u32 rs : rs_values)
		{
			for (u32 fs : fs_values)
			{
				SCOPED_TRACE(testing::Message() << "rs=" << rs << " fs=" << fs << " seed=" << seed);
				Init(seed);
				InitFPU(seed);
				fpuRegs.fprc[31] = 0x0083c078 ^ seed; // arbitrary FCR31 content for CFC1 coverage
				program[0] = (17u << 26) | (rs << 21) | (rt << 16) | (fs << 11);
				CompareWithFPU(1);
				if (HasFailure())
					return;
			}
		}
	}
}

TEST_F(EERecompilerTest, COP1ArithmeticMatchesInterpreter)
{
	// function selects ADD_S(0)/SUB_S(1)/MUL_S(2)/ABS_S(5)/MOV_S(6)/NEG_S(7).
	// Operands sweep InitFPU's edge-case table (zeros, Inf, NaN, denormals,
	// Fmax) so the non-IEEE clamping in EmitFpuClampOperand/EmitFpuOutputFlags
	// gets exercised, not just ordinary finite values.
	constexpr u32 functions[] = {0, 1, 2, 5, 6, 7};
	for (u32 seed = 0; seed < 24; seed++)
	{
		const u32 fd = 1 + seed % 4, fs = 1 + (seed / 4) % 4, ft = 1 + (seed / 16) % 4;
		for (u32 function : functions)
		{
			SCOPED_TRACE(testing::Message() << "function=" << function << " seed=" << seed);
			Init(seed);
			InitFPU(seed);
			program[0] = (17u << 26) | (16u << 21) | (ft << 16) | (fs << 11) | (fd << 6) | function;
			CompareWithFPU(1);
			if (HasFailure())
				return;
		}
	}
}

TEST_F(EERecompilerTest, COP1CompareAndBranchMatchInterpreter)
{
	// function selects C_F(48)/C_EQ(50)/C_LT(52)/C_LE(54); the FCR31 C bit it
	// writes is then consumed by a BC1[F/T][L] in the following instruction.
	constexpr u32 functions[] = {48, 50, 52, 54};
	for (u32 seed = 0; seed < 16; seed++)
	{
		const u32 fs = 1 + seed % 4, ft = 1 + (seed / 4) % 4;
		for (u32 function : functions)
		{
			SCOPED_TRACE(testing::Message() << "function=" << function << " seed=" << seed);
			Init(seed);
			InitFPU(seed);
			program[0] = (17u << 26) | (16u << 21) | (ft << 16) | (fs << 11) | function;
			CompareWithFPU(1);
			if (HasFailure())
				return;
		}
	}
}

TEST_F(EERecompilerTest, COP1BranchFollowsCompareResult)
{
	// BC1F/BC1T/BC1FL/BC1TL (rt selects which) branching off a preceding
	// C_LT.S, covering both the taken and untaken/annulled paths.
	for (u32 seed = 0; seed < 8; seed++)
	{
		const u32 fs = 1 + seed % 4, ft = 1 + (seed / 4) % 4;
		for (u32 rt = 0; rt < 4; rt++)
		{
			SCOPED_TRACE(testing::Message() << "rt=" << rt << " seed=" << seed);
			Init(seed);
			InitFPU(seed);
			program[0] = (17u << 26) | (16u << 21) | (ft << 16) | (fs << 11) | 52; // C_LT.S
			program[1] = (17u << 26) | (8u << 21) | (rt << 16) | 2; // BC1[F/T][L], offset +2
			program[2] = 0; // SLL r0, r0, 0 (NOP) delay slot

			// Predict C_LT.S's outcome by interpreting it against a scratch copy,
			// without disturbing the state CompareBranch below needs pristine.
			cpuRegisters cpu_copy = cpuRegs;
			fpuRegisters fpu_copy = fpuRegs;
			cpu_copy.code = program[0];
			const auto& opcode = R5900::GetInstruction(program[0]);
			std::swap(cpuRegs, cpu_copy);
			std::swap(fpuRegs, fpu_copy);
			opcode.interpret();
			const bool c = (fpuRegs.fprc[31] & 0x00800000) != 0;
			std::swap(cpuRegs, cpu_copy);
			std::swap(fpuRegs, fpu_copy);

			const bool taken = c == ((rt & 1) != 0);
			const bool likely = (rt & 2) != 0;
			CompareBranch(1, taken, likely, likely, Base + 16); // branch at Base+4, target = +4+offset*4
			if (HasFailure())
				return;
		}
	}
}

TEST_F(EERecompilerTest, COP1ConvertMatchesInterpreter)
{
	// CVT_S.W (rs==20) converts an integer bit pattern to float; CVT_W.S
	// (rs==16, function 36) converts back with PS2's saturate-instead-of-trap
	// behavior for out-of-range magnitudes, so InitFPU's edge cases (which
	// include values well past the +/-2^30 cutoff) matter here too.
	for (u32 seed = 0; seed < 16; seed++)
	{
		const u32 fd = 1 + seed % 4, fs = 1 + (seed / 4) % 4;
		SCOPED_TRACE(testing::Message() << "CVT_S seed=" << seed);
		Init(seed);
		InitFPU(seed);
		program[0] = (17u << 26) | (20u << 21) | (fs << 11) | (fd << 6) | 32;
		CompareWithFPU(1);
		if (HasFailure())
			return;
		SCOPED_TRACE(testing::Message() << "CVT_W seed=" << seed);
		Init(seed);
		InitFPU(seed);
		program[0] = (17u << 26) | (16u << 21) | (fs << 11) | (fd << 6) | 36;
		CompareWithFPU(1);
		if (HasFailure())
			return;
	}
}

TEST_F(EERecompilerTest, COP1MemoryTransfersMatchInterpreter)
{
	Init(0);
	InitFPU(0);
	cpuRegs.GPR.r[1].UD[0] = Data;
	program[0] = (49u << 26) | (1u << 21) | (3u << 16) | 4; // LWC1 f3, 4(r1)
	CompareWithFPU(1);
	if (HasFailure())
		return;
	Init(0);
	InitFPU(0);
	cpuRegs.GPR.r[1].UD[0] = Data;
	program[0] = (57u << 26) | (1u << 21) | (3u << 16) | 8; // SWC1 f3, 8(r1)
	CompareWithFPU(1);
}

TEST_F(EERecompilerTest, COP1SurroundingIntegerCodeStaysNative)
{
	// Like COP2SurroundingIntegerCodeStaysNative: a COP1 instruction should
	// not force the rest of the block back to the interpreter.
	Init(0);
	InitFPU(0);
	program[0] = (9u << 26) | (1u << 21) | (2u << 16) | 5; // ADDIU r2, r1, 5
	program[1] = (17u << 26) | (16u << 21) | (2u << 16) | (1u << 11) | (3u << 6) | 0; // ADD.S f3, f1, f2
	program[2] = (9u << 26) | (2u << 21) | (4u << 16) | 7; // ADDIU r4, r2, 7
	CompareWithFPU(3);
}

TEST_F(EERecompilerTest, COP1UnsupportedRemainsInterpreted)
{
	// RSQRT_S (function 22) is not in SupportsCOP1's whitelist; it must keep
	// falling back to the interpreter rather than being mis-decoded by
	// EmitCOP1's arithmetic default case.
	Init(0);
	InitFPU(0);
	program[0] = (17u << 26) | (16u << 21) | (2u << 16) | (1u << 11) | (3u << 6) | 22; // RSQRT.S f3, f1, f2
	const cpuRegisters before = cpuRegs;
	const fpuRegisters before_fpu = fpuRegs;
	u32 cycles = 123;
	EXPECT_FALSE(Arm64EE::TryExecute(cycles));
	EXPECT_EQ(cycles, 123u);
	EXPECT_EQ(std::memcmp(&before, &cpuRegs, sizeof(cpuRegs)), 0);
	EXPECT_EQ(std::memcmp(&before_fpu, &fpuRegs, sizeof(fpuRegs)), 0);
}

TEST_F(EERecompilerTest, TrappingArithmeticLeavesOverflowToInterpreter)
{
	// ADDI, DADDI, then ADD, SUB, DADD, DSUB. An overflowing instruction must
	// stop the block before any of its effects so the interpreter raises it.
	constexpr u32 ops[] = {8u << 26, 24u << 26, 32, 34, 44, 46};
	constexpr u64 values[] = {0, 1, ~u64(0), 0x7fffffff, 0x80000000, 0xffffffff80000000ULL,
		0x7fffffffffffffffULL, 0x8000000000000000ULL, 0x40000000, 0x123456789abcdefULL};
	constexpr s16 immediates[] = {0, 1, -1, 0x7fff, -0x8000};
	for (u32 op : ops)
	{
		for (u32 i = 0; i < std::size(values); i++)
		{
			for (u32 j = 0; j < std::size(values); j++)
			{
				for (u32 rd : {0u, 3u, 1u})
				{
					SCOPED_TRACE(testing::Message() << "op=" << op << " i=" << i << " j=" << j << " rd=" << rd);
					Init(i + j);
					cpuRegs.GPR.r[1].UD[0] = values[i];
					cpuRegs.GPR.r[2].UD[0] = values[j];
					const bool immediate = op > 63;
					const s16 imm = immediates[j % std::size(immediates)];
					program[0] = (9u << 26) | (4 << 16) | 5; // ADDIU r4, r0, 5 keeps a prefix
					program[1] = immediate ? op | (1 << 21) | (rd << 16) | static_cast<u16>(imm) :
					                         op | (1 << 21) | (2 << 16) | (rd << 11);
					const bool doubleword = op == (24u << 26) || op == 44 || op == 46;
					const s64 lhs = doubleword ? static_cast<s64>(values[i]) : static_cast<s32>(values[i]);
					s64 rhs = doubleword ? static_cast<s64>(values[j]) : static_cast<s32>(values[j]);
					if (immediate)
						rhs = imm;
					else if (op == 34)
						rhs = static_cast<s32>(-static_cast<s64>(values[j])); // the interpreter's truncated negation
					else if (op == 46)
						rhs = static_cast<s64>(0 - values[j]);
					bool overflow;
					if (doubleword)
					{
						s64 sum;
						overflow = __builtin_add_overflow(lhs, rhs, &sum);
					}
					else
						overflow = (lhs + rhs) != static_cast<s32>(lhs + rhs);
					Compare(overflow ? 1 : 2);
					if (overflow)
						ExpectRejected();
					if (HasFailure())
						return;
				}
			}
		}
	}
	// Trapping instructions stay out of delay slots.
	Init(0);
	program[0] = (2u << 26) | ((Base + 64) >> 2); // J
	program[1] = 0x00221820u; // ADD r3, r1, r2
	ExpectRejected();
}

TEST_F(EERecompilerTest, VU0QuadwordTransfersMatchInterpreter)
{
	for (u32 op : {54u, 62u}) // LQC2, SQC2
	{
		for (u32 seed = 0; seed < 32; seed++)
		{
			SCOPED_TRACE(testing::Message() << "op=" << op << " seed=" << seed);
			Init(seed);
			InitVU0(seed);
			const u32 ft = seed % 4; // includes vf0
			const s16 displacement = (seed & 1) ? -32768 : 32752;
			cpuRegs.GPR.r[1].UD[0] = 0xffffffff00000000ULL | u32(Data + (seed % 8) * 16 - displacement);
			program[0] = (op << 26) | (1 << 21) | (ft << 16) | static_cast<u16>(displacement);
			program[1] = (9u << 26) | (1 << 21) | (5 << 16) | 3; // ADDIU after the transfer
			CompareWithVU0(2);
			if (HasFailure())
				return;
		}
		// Unaligned addresses and a running microprogram belong to the interpreter.
		for (u32 scenario = 0; scenario < 2; scenario++)
		{
			Init(0);
			InitVU0(1);
			cpuRegs.GPR.r[1].UD[0] = Data + (scenario == 0 ? 4 : 0);
			if (scenario == 1)
				VU0.VI[REG_VPU_STAT].UL = 1;
			program[0] = (op << 26) | (1 << 21) | (2 << 16);
			ExpectRejected();
		}
	}
}

TEST_F(EERecompilerTest, MemoryAndFpuDelaySlotsMatchInterpreter)
{
	struct Case
	{
		u32 branch;
		bool taken, likely, event;
		u32 link, target;
	};
	// r5 == r5 and r5 != r6 hold for every seed below; r7 holds Base + 256.
	const Case cases[] = {
		{(2u << 26) | ((Base + 64) >> 2), true, false, false, 0, Base + 64}, // J
		{(3u << 26) | ((Base + 64) >> 2), true, false, false, 31, Base + 64}, // JAL
		{(4u << 26) | (5 << 21) | (5 << 16) | 15, true, false, false, 0, Base + 64}, // BEQ taken
		{(4u << 26) | (5 << 21) | (6 << 16) | 15, false, false, true, 0, 0}, // BEQ untaken
		{(21u << 26) | (5 << 21) | (6 << 16) | 15, true, true, false, 0, Base + 64}, // BNEL taken
		{(21u << 26) | (5 << 21) | (5 << 16) | 15, false, true, true, 0, 0}, // BNEL annulled
		{(7 << 21) | (31 << 11) | 9, true, false, false, 31, Base + 256}, // JALR r7
	};
	const u32 delays[] = {
		(35u << 26) | (1 << 21) | (2 << 16) | 8, // LW r2, 8(r1)
		(35u << 26) | (1 << 21) | (31 << 16) | 8, // LW ra, 8(r1)
		(43u << 26) | (1 << 21) | (31 << 16) | 4, // SW ra, 4(r1)
		(30u << 26) | (1 << 21) | (7 << 16) | 16, // LQ r7, 16(r1)
		(31u << 26) | (1 << 21) | (5 << 16) | 32, // SQ r5, 32(r1)
		(54u << 26) | (1 << 21) | (3 << 16) | 48, // LQC2 vf3, 48(r1)
		(62u << 26) | (1 << 21) | (4 << 16) | 64, // SQC2 vf4, 64(r1)
		(57u << 26) | (1 << 21) | (6 << 16) | 12, // SWC1 f6, 12(r1)
		(49u << 26) | (1 << 21) | (6 << 16) | 12, // LWC1 f6, 12(r1)
		(17u << 26) | (16u << 21) | (2 << 16) | (1 << 11) | (3 << 6), // ADD.S f3, f1, f2
	};
	for (u32 seed = 0; seed < 4; seed++)
	{
		for (const Case& c : cases)
		{
			for (u32 delay : delays)
			{
				SCOPED_TRACE(testing::Message() << std::hex << "branch=" << c.branch << " delay=" << delay << " seed=" << seed);
				Init(seed);
				InitVU0(seed);
				InitFPU(seed);
				for (u32 i = 0; i < memory.size(); i++)
					memory[i] = i * 0x9e3779b9u + seed;
				cpuRegs.GPR.r[1].UD[0] = Data + seed * 16;
				cpuRegs.GPR.r[5].UD[0] = 0x1234;
				cpuRegs.GPR.r[6].UD[0] = 0x5678;
				cpuRegs.GPR.r[7].UD[0] = Base + 256;
				program[0] = c.branch;
				program[1] = delay;
				CompareBranch(0, c.taken, c.likely, c.event, c.target, c.link);
				if (HasFailure())
					return;
			}
		}
	}
	// A delay-slot access the interpreter must perform leaves before the
	// branch writes its link register.
	// Misaligned, counter register, and handler-mapped addresses.
	constexpr u32 rejected[] = {Data + 2, 0x10000000, Data};
	for (u32 scenario = 0; scenario < 3; scenario++)
	{
		Init(0);
		InitVU0(0);
		cpuRegs.GPR.r[1].UD[0] = rejected[scenario];
		if (scenario == 2)
			vtlb_private::vtlbdata.vmap[Data >> 12] = vtlb_private::VTLBVirtual(vtlb_private::VTLBPhysical::fromHandler(0), Data, Data);
		program[0] = (3u << 26) | ((Base + 64) >> 2); // JAL
		program[1] = (35u << 26) | (1 << 21) | (2 << 16); // LW r2, 0(r1)
		ExpectRejected();
		Map(Data, memory.data());
	}
	// The address is computed before the link write, so a base of the link
	// register cannot be compiled.
	Init(0);
	cpuRegs.GPR.r[31].UD[0] = Data;
	program[0] = (3u << 26) | ((Base + 64) >> 2); // JAL
	program[1] = (35u << 26) | (31 << 21) | (2 << 16); // LW r2, 0(ra)
	ExpectRejected();
	// A delay-slot store into the block's own code still takes effect.
	Init(0);
	Map(Data, program.data());
	program[0] = (2u << 26) | ((Base + 64) >> 2); // J
	program[1] = (43u << 26) | (1 << 21) | (2 << 16) | 4; // SW r2, 4(r1): overwrites itself
	cpuRegs.GPR.r[1].UD[0] = Data;
	cpuRegs.GPR.r[2].UD[0] = 0x24030007; // ADDIU r3, r0, 7
	u32 cycles = 0;
	EXPECT_EQ(Arm64EE::TryExecute(cycles).exit, EEBlockExit::TakenBranch);
	EXPECT_EQ(program[1], 0x24030007u);
	cpuRegs.pc = Base;
	program[1] = 0x24030007;
	CompareBranch(0, true, false, false, Base + 64);
	EXPECT_EQ(cpuRegs.GPR.r[3].UD[0], 7u);
}

TEST_F(EERecompilerTest, COP1DivideAccumulateAndMinMaxMatchInterpreter)
{
	// DIV_S, SQRT_S, ADDA/SUBA/MULA, MADD/MSUB, MADDA/MSUBA, MAX/MIN. Seeds
	// below 64 take InitFPU's edge values, the rest random bit patterns.
	constexpr u32 functions[] = {3, 4, 24, 25, 26, 28, 29, 30, 31, 40, 41};
	constexpr u32 accumulators[] = {0x00000000, 0x80000000, 0x3f800000, 0xff7fffff, 0x7f800000, 0x00400000, 0xc2c80000};
	for (u32 seed = 0; seed < 160; seed++)
	{
		const u32 fd = seed % 5, fs = 1 + (seed / 5) % 4, ft = 1 + (seed / 20) % 4; // fd may alias
		for (u32 function : functions)
		{
			SCOPED_TRACE(testing::Message() << "function=" << function << " seed=" << seed);
			Init(seed);
			InitFPU(seed);
			fpuRegs.ACC.UL = seed < 64 ? accumulators[seed % std::size(accumulators)] : fpuRegs.fpr[(seed + 7) % 32].UL;
			fpuRegs.fprc[31] = 0x0003c078 ^ (seed << 3);
			program[0] = (17u << 26) | (16u << 21) | (ft << 16) | (fs << 11) | (fd << 6) | function;
			CompareWithFPU(1);
			if (HasFailure())
				return;
		}
	}
}

TEST_F(EERecompilerTest, ShiftAmountAndMiscMultimediaMatchInterpreter)
{
	for (u32 seed = 0; seed < 96; seed++)
	{
		const u32 rs = 1 + seed % 4, rt = 1 + (seed / 4) % 4, rd = (seed / 16) % 4, sa = (seed * 7) % 32;
		const u32 codes[] = {
			PackedCode(52, sa) | (rt << 16) | (rd << 11), // PSLLH
			PackedCode(54, sa) | (rt << 16) | (rd << 11), // PSRLH
			PackedCode(55, sa) | (rt << 16) | (rd << 11), // PSRAH
			PackedCode(60, sa) | (rt << 16) | (rd << 11), // PSLLW
			PackedCode(62, sa) | (rt << 16) | (rd << 11), // PSRLW
			PackedCode(63, sa) | (rt << 16) | (rd << 11), // PSRAW
			PackedCode(41, 27) | (rt << 16) | (rd << 11), // PCPYH
			PackedCode(9, 28) | (rs << 21) | (rt << 16) | (rd << 11), // PMULTH
			(1u << 26) | (rs << 21) | (24 << 16) | ((seed * 40503) & 0xffff), // MTSAB
			(1u << 26) | (rs << 21) | (25 << 16) | ((seed * 40503) & 0xffff), // MTSAH
			(rs << 21) | 41, // MTSA
			(rd << 11) | 40, // MFSA
		};
		for (u32 code : codes)
		{
			SCOPED_TRACE(testing::Message() << std::hex << "code=" << code << " seed=" << seed);
			InitPacked(seed);
			cpuRegs.sa = seed * 0x9e3779b9u;
			program[0] = code;
			program[1] = Stop;
			Compare(1);
			if (HasFailure())
				return;
		}
		// QFSRV after MTSAB, which leaves SA in 0..15.
		SCOPED_TRACE(testing::Message() << "QFSRV seed=" << seed);
		InitPacked(seed);
		program[0] = (1u << 26) | (rs << 21) | (24 << 16) | seed; // MTSAB
		program[1] = PackedCode(40, 27) | (rs << 21) | (rt << 16) | (rd << 11); // QFSRV
		Compare(2);
		if (HasFailure())
			return;
	}
	// A larger SA (only MTSA can set one) leaves QFSRV to the interpreter.
	InitPacked(0);
	cpuRegs.sa = 16;
	program[0] = (9u << 26) | (4 << 16) | 5; // ADDIU r4, r0, 5
	program[1] = PackedCode(40, 27) | (1 << 21) | (2 << 16) | (3 << 11);
	Compare(1);
	ExpectRejected();
}

TEST_F(EERecompilerTest, MoveFromCOP0MatchesInterpreter)
{
	for (u32 rd = 0; rd < 32; rd++)
	{
		if (rd == 25)
			continue; // performance counters stay interpreted
		for (u32 seed = 0; seed < 6; seed++)
		{
			SCOPED_TRACE(testing::Message() << "rd=" << rd << " seed=" << seed);
			Init(seed);
			for (u32 reg = 0; reg < 32; reg++)
				cpuRegs.CP0.r[reg] = 0x89abcdefu * (reg + seed + 1);
			cpuRegs.CP0.n.Config = (seed & 1) << 18;
			cpuRegs.cycle = 0x123456789ULL * (seed + 1);
			cpuRegs.lastCOP0Cycle = seed == 0 ? cpuRegs.cycle : cpuRegs.cycle - seed * 1000;
			const u32 rt = seed % 3; // rt 0 still updates Count
			program[0] = (16u << 26) | (rt << 16) | (rd << 11) | (seed & 7);
			program[1] = (16u << 26) | (1 << 16) | (9 << 11); // a second Count read
			Compare(2);
			if (HasFailure())
				return;
		}
	}
	Init(0);
	program[0] = 0x4002c800u; // MFC0 r2, $25
	ExpectRejected();
}

TEST_F(EERecompilerTest, COP0BranchesFollowDmacCondition)
{
	const u32 stat = psHu32(DMAC_STAT), pcr = psHu32(DMAC_PCR);
	constexpr u32 values[][2] = {{0x3ff, 0x3ff}, {0x000, 0x000}, {0x3fe, 0x3ff}, {0x001, 0x3fe}, {0x3ff0000, 0x0}, {0x155, 0x2aa}};
	for (const auto& [s, p] : values)
	{
		for (u32 rt = 0; rt < 4; rt++)
		{
			for (u32 delay : {0u, (35u << 26) | (1 << 21) | (2 << 16) | 4})
			{
				SCOPED_TRACE(testing::Message() << std::hex << "stat=" << s << " pcr=" << p << " rt=" << rt << " delay=" << delay);
				Init(0);
				cpuRegs.GPR.r[1].UD[0] = Data;
				psHu32(DMAC_STAT) = s;
				psHu32(DMAC_PCR) = p;
				program[0] = (16u << 26) | (8u << 21) | (rt << 16) | 3; // BC0x +3
				program[1] = delay;
				const bool condition = ((s | ~p) & 0x3ff) == 0x3ff;
				const bool taken = condition == ((rt & 1) != 0), likely = (rt & 2) != 0;
				CompareBranch(0, taken, likely, likely, Base + 16);
			}
		}
	}
	psHu32(DMAC_STAT) = stat;
	psHu32(DMAC_PCR) = pcr;
}

TEST_F(EERecompilerTest, LinkedAndRegisterJumpsMatchSteppedExecution)
{
	// A loop that calls a function with JAL, returns with JR ra, and calls a
	// second one through JALR. With untracked blocks trusted, static exits link
	// and register jumps go through the indirect table, so the whole loop runs
	// with a handful of C++ lookups.
	struct TrustGuard
	{
		TrustGuard() { Arm64EE::SetTrustUntrackedForTesting(true); }
		~TrustGuard() { Arm64EE::SetTrustUntrackedForTesting(false); }
	} guard;
	constexpr u32 iterations = 3000; // crosses kMaxChainedBlocks
	auto build = [&](u32 increment) {
		program.fill(Stop);
		program[0] = (3u << 26) | ((Base + 64) >> 2); // JAL f
		program[1] = (9u << 26) | (1 << 21) | (1 << 16) | 0xffffu; // delay: ADDIU $1, $1, -1
		program[2] = (5 << 21) | (31 << 11) | 9; // JALR $5 (g)
		program[3] = 0; // delay: NOP
		program[4] = (5u << 26) | (1 << 21) | 0xfffbu; // BNE $1, $0, Base
		program[5] = (9u << 26) | (4 << 21) | (4 << 16) | 1u; // delay: ADDIU $4, $4, 1
		program[16] = (9u << 26) | (2 << 21) | (2 << 16) | increment; // f: ADDIU $2, $2, increment
		program[17] = (31 << 21) | 8; // JR ra
		program[18] = (9u << 26) | (3 << 21) | (3 << 16) | 3u; // delay: ADDIU $3, $3, 3
		program[32] = (31 << 21) | 8; // g: JR ra
		program[33] = (0x19u << 26) | (6 << 21) | (6 << 16) | 5u; // delay: DADDIU $6, $6, 5
		for (u32 i = 0; i < 8; i++)
			program[48 + i] = (9u << 26) | (7 << 21) | (7 << 16) | (increment + i); // filler: ADDIU $7, $7, n
	};
	auto prepare = [&]() {
		Init(0);
		cpuRegs.GPR.r[1].UD[0] = iterations;
		cpuRegs.GPR.r[5].UD[0] = Base + 128;
		cpuRegs.nextEventCycle = u64(1) << 40; // keep events away, as in ChainedBlocksMatchOneBlockPerCall
	};
	auto run = [&](bool chained, u32& cycles) {
		cycles = 0;
		EEBlockResult result;
		u32 calls = 0;
		do
		{
			result = chained ? Arm64EE::ExecuteChained(cycles) : Arm64EE::TryExecute(cycles);
			if (result.exit == EEBlockExit::TakenBranch)
			{
				cpuRegs.branch = 1;
				cpuRegs.pc = result.target;
				cpuRegs.branch = 0;
				cpuRegs.cycle += std::max(cycles >> 3, 1u);
				cycles &= 7;
			}
			ASSERT_LT(++calls, 100000u);
		} while (result);
	};
	for (u32 increment : {7u, 11u})
	{
		SCOPED_TRACE(testing::Message() << "increment=" << increment);
		build(increment);
		Arm64EE::Reset(); // the code changed; trusted blocks need an explicit drop
		if (increment != 7)
		{
			// Compile something else first, so the loop's blocks land elsewhere
			// in the code buffer than in the previous round.
			prepare();
			cpuRegs.pc = Base + 192;
			u32 filler_cycles = 0;
			ASSERT_TRUE(Arm64EE::TryExecute(filler_cycles));
		}
		prepare();
		const u32 initial_r2 = cpuRegs.GPR.r[2].UL[0];
		u32 stepped_cycles, chained_cycles;
		run(false, stepped_cycles);
		if (HasFatalFailure())
			return;
		const cpuRegisters stepped = cpuRegs;
		EXPECT_EQ(stepped.GPR.r[2].UL[0], initial_r2 + increment * iterations); // ran every call
		prepare();
		const u64 dispatches = Arm64EE::GetDispatchCount();
		run(true, chained_cycles);
		if (HasFatalFailure())
			return;
		EXPECT_LT(Arm64EE::GetDispatchCount() - dispatches, 16u);
		EXPECT_EQ(chained_cycles, stepped_cycles);
		EXPECT_EQ(std::memcmp(&cpuRegs, &stepped, sizeof(cpuRegisters)), 0);
	}
}

TEST_F(EERecompilerTest, LinkedBlockLeavingBeforeItsFirstInstructionStoresItsPc)
{
	// Links do not store pc. A linked block whose first instruction is an access
	// the interpreter has to perform (here an unaligned LW) leaves before running
	// anything, and has to name its own entry for the interpreter.
	program.fill(Stop);
	program[0] = (9u << 26) | (2 << 21) | (2 << 16) | 1u; // ADDIU $2, $2, 1
	program[1] = (2u << 26) | ((Base + 64) >> 2); // J Base + 64
	program[2] = 0; // delay: NOP
	program[16] = (35u << 26) | (1 << 21) | (3 << 16) | 1u; // LW $3, 1($1)
	Arm64EE::Reset();
	// The first round links the jump; the second runs through the link.
	for (u32 round = 0; round < 2; round++)
	{
		SCOPED_TRACE(testing::Message() << "round=" << round);
		Init(0);
		cpuRegs.GPR.r[1].UD[0] = Data;
		cpuRegs.nextEventCycle = u64(1) << 40;
		const u64 initial_r2 = cpuRegs.GPR.r[2].UD[0];
		u32 cycles = 0;
		EXPECT_FALSE(Arm64EE::ExecuteChained(cycles));
		EXPECT_EQ(cpuRegs.pc, Base + 64);
		EXPECT_EQ(cpuRegs.GPR.r[2].UD[0], initial_r2 + 1);
	}
}

TEST_F(EERecompilerTest, UntrustedBlocksLinkAndCatchChangedSource)
{
	// The same kind of loop on a page that is not write-protected (untracked,
	// as in every test here): its blocks check their own source on entry, so
	// they link too. Then the called function changes without any Reset() or
	// Clear(); the links already made lead to the old code, whose check has to
	// send execution back to the dispatcher to recompile it.
	constexpr u32 iterations = 3000;
	program.fill(Stop);
	program[0] = (3u << 26) | ((Base + 64) >> 2); // JAL f
	program[1] = (9u << 26) | (1 << 21) | (1 << 16) | 0xffffu; // delay: ADDIU $1, $1, -1
	program[2] = (5u << 26) | (1 << 21) | 0xfffdu; // BNE $1, $0, Base
	program[3] = (9u << 26) | (4 << 21) | (4 << 16) | 1u; // delay: ADDIU $4, $4, 1
	program[16] = (9u << 26) | (2 << 21) | (2 << 16) | 7u; // f: ADDIU $2, $2, 7
	program[17] = (9u << 26) | (3 << 21) | (3 << 16) | 3u; // ADDIU $3, $3, 3
	program[18] = (31 << 21) | 8; // JR ra
	program[19] = 0; // delay: NOP
	Arm64EE::Reset();
	auto prepare = [&]() {
		Init(0);
		cpuRegs.GPR.r[1].UD[0] = iterations;
		cpuRegs.nextEventCycle = u64(1) << 40;
	};
	auto run = [&](bool chained, u32& cycles) {
		cycles = 0;
		EEBlockResult result;
		u32 calls = 0;
		do
		{
			result = chained ? Arm64EE::ExecuteChained(cycles) : Arm64EE::TryExecute(cycles);
			if (result.exit == EEBlockExit::TakenBranch)
			{
				cpuRegs.branch = 1;
				cpuRegs.pc = result.target;
				cpuRegs.branch = 0;
				cpuRegs.cycle += std::max(cycles >> 3, 1u);
				cycles &= 7;
			}
			ASSERT_LT(++calls, 100000u);
		} while (result);
	};
	for (u32 round = 0; round < 2; round++)
	{
		SCOPED_TRACE(testing::Message() << "round=" << round);
		if (round)
			program[16] = (9u << 26) | (2 << 21) | (2 << 16) | 11u; // f: ADDIU $2, $2, 11
		// Chained first, so the second round runs through the first round's links.
		prepare();
		const u32 initial_r2 = cpuRegs.GPR.r[2].UL[0];
		const u64 dispatches = Arm64EE::GetDispatchCount();
		u32 chained_cycles, stepped_cycles;
		run(true, chained_cycles);
		if (HasFatalFailure())
			return;
		if (!round)
			EXPECT_LT(Arm64EE::GetDispatchCount() - dispatches, 16u);
		const cpuRegisters chained = cpuRegs;
		EXPECT_EQ(chained.GPR.r[2].UL[0], initial_r2 + (round ? 11 : 7) * iterations);
		prepare();
		run(false, stepped_cycles);
		if (HasFatalFailure())
			return;
		EXPECT_EQ(chained_cycles, stepped_cycles);
		EXPECT_EQ(std::memcmp(&cpuRegs, &chained, sizeof(cpuRegisters)), 0);
	}
}
#endif
