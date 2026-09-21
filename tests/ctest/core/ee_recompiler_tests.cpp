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
	constexpr u32 Stop = 0x0000000c; // SYSCALL ends the block before its side effects.
	constexpr u32 Immediate[] = {9, 10, 11, 12, 13, 14, 15, 25};
	constexpr u32 Special[] = {0, 2, 3, 4, 6, 7, 10, 11, 20, 22, 23, 33, 35, 36,
		37, 38, 39, 42, 43, 45, 47, 56, 58, 59, 60, 62, 63};
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
			u32 native_cycles = 0xfffffff0;
			const EEBlockResult result = Arm64EE::TryExecute(native_cycles);
			ASSERT_TRUE(result);
			const cpuRegisters actual = cpuRegs;
			cpuRegs = initial;
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
			u32 native_cycles = 0xfffffff0;
			ASSERT_TRUE(Arm64EE::TryExecute(native_cycles));
			const cpuRegisters actual = cpuRegs;
			const VURegs actual_vu0 = VU0;
			cpuRegs = initial;
			VU0 = initial_vu0;
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
			const u32 address = Data + ((seed % 16) * 16) + ((op == 30 || op == 31) ? seed % 16 : 0);
			// Exercise negative displacements, 32-bit address addition and rt == rs.
			const s16 displacement = (seed & 1) ? -32768 : 32752;
			cpuRegs.GPR.r[1].UD[0] = 0xffffffff00000000ULL | u32(address - displacement);
			const u32 rt = seed % 4;
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
	for (u32 code : {Stop, 0x88220000u, 0x10220000u, 0x00221820u, 0x40026000u})
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
	for (u32 initial_t0 : {1u, 2u, 5u, 5000u}) // 5000 exercises kMaxSelfLoopIterations (4096)
	{
		SCOPED_TRACE(testing::Message() << "initial_t0=" << initial_t0);
		Init(0);
		program[0] = (9u << 26) | (1 << 21) | (1 << 16) | 0xffffu; // ADDIU $t0(r1), $t0, -1
		program[1] = (5u << 26) | (1 << 21) | (0 << 16) | 0xfffeu; // BNE $t0, $zero, Base (back to word 0)
		program[2] = (9u << 26) | (2 << 21) | (2 << 16) | 1u;      // delay slot: ADDIU $t1(r2), $t1, 1
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
		cpuRegs = initial;
		u32 expected_cycles = 0;
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
			}
			else
			{
				// Untaken: native's EmitPosition() (EECodeGenerator.cpp) leaves cpuRegs
				// at the delay slot's address/code, without executing it -- whoever runs
				// next (this test's own driver, or the real dispatcher) does that.
				cpuRegs.pc = Base + 8;
				cpuRegs.code = program[1];
				break;
			}
		}
		const cpuRegisters expected = cpuRegs;

		// Native, driven exactly like intExecuteWithBackend drives TryExecute().
		cpuRegs = initial;
		u32 native_cycles = 0;
		EEBlockResult result;
		u32 outer_calls = 0;
		do
		{
			result = Arm64EE::TryExecute(native_cycles);
			ASSERT_TRUE(result);
			if (result.exit == EEBlockExit::TakenBranch)
			{
				cpuRegs.branch = 1;
				cpuRegs.pc = result.target;
				cpuRegs.branch = 0;
			}
			ASSERT_LT(++outer_calls, 10u);
		} while (result.exit == EEBlockExit::TakenBranch && result.target == Base);

		EXPECT_EQ(native_cycles, expected_cycles);
		// cpuRegs.cycle itself isn't compared: intUpdateCPUCycles() (Interpreter.cpp)
		// updates it from its own file-static cpuBlockCycles, not from the
		// block_cycles reference TryExecute() was actually called with here -- only
		// correct when the caller aliases the two, which only intExecuteWithBackend's
		// real registration (intExecuteWithBackend(&Arm64EE::TryExecute), Interpreter.h)
		// guarantees. native_cycles above (this call's own block_cycles accumulator)
		// is the accounting this test can actually verify.
		cpuRegisters actual_no_cycle = cpuRegs;
		cpuRegisters expected_no_cycle = expected;
		actual_no_cycle.cycle = expected_no_cycle.cycle = 0;
		EXPECT_EQ(std::memcmp(&actual_no_cycle, &expected_no_cycle, sizeof(cpuRegisters)), 0);
		if (HasFailure())
			return;
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
			if (std::find(std::begin(PackedInstructions), std::end(PackedInstructions), code) != std::end(PackedInstructions))
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
	// DIV_S (function 3) is not in SupportsCOP1's Phase-1 whitelist; it must
	// keep falling back to the interpreter rather than being mis-decoded by
	// EmitCOP1's arithmetic default case.
	Init(0);
	InitFPU(0);
	program[0] = (17u << 26) | (16u << 21) | (2u << 16) | (1u << 11) | (3u << 6) | 3; // DIV.S f3, f1, f2
	const cpuRegisters before = cpuRegs;
	const fpuRegisters before_fpu = fpuRegs;
	u32 cycles = 123;
	EXPECT_FALSE(Arm64EE::TryExecute(cycles));
	EXPECT_EQ(cycles, 123u);
	EXPECT_EQ(std::memcmp(&before, &cpuRegs, sizeof(cpuRegs)), 0);
	EXPECT_EQ(std::memcmp(&before_fpu, &fpuRegs, sizeof(fpuRegs)), 0);
}
#endif
