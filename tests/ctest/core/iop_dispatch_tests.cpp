// SPDX-FileCopyrightText: 2026 PCSX2 Dev Team
// SPDX-License-Identifier: GPL-3.0+

#include "Common.h"
#include "R3000AInterpreter.h"

#include <gtest/gtest.h>
#include <algorithm>
#include <array>
#include <utility>

namespace
{
	using Handler = void (*)();
	Handler selected;
	template <size_t Index>
	void Probe()
	{
		selected = &Probe<Index>;
	}

	template <size_t Offset, size_t... Indices>
	constexpr auto Probes(std::index_sequence<Indices...>)
	{
		return std::array<Handler, sizeof...(Indices)>{&Probe<Offset + Indices>...};
	}

	class IopDispatchTest : public testing::Test
	{
	protected:
		void SetUp() override
		{
			registers = psxRegs;
			std::copy_n(psxBSC, 64, basic.begin());
			std::copy_n(psxSPC, 64, special.begin());
			std::copy_n(psxREG, 32, regimm.begin());
			std::copy_n(psxCP0, 32, cp0.begin());
			std::copy_n(psxCP2, 64, cp2.begin());
			std::copy_n(psxCP2BSC, 32, cp2basic.begin());
		}
		void TearDown() override
		{
			std::copy(basic.begin(), basic.end(), psxBSC);
			std::copy(special.begin(), special.end(), psxSPC);
			std::copy(regimm.begin(), regimm.end(), psxREG);
			std::copy(cp0.begin(), cp0.end(), psxCP0);
			std::copy(cp2.begin(), cp2.end(), psxCP2);
			std::copy(cp2basic.begin(), cp2basic.end(), psxCP2BSC);
			psxRegs = registers;
		}
		psxRegisters registers{};
		std::array<Handler, 64> basic{}, special{}, cp2{};
		std::array<Handler, 32> regimm{}, cp0{}, cp2basic{};
	};
} // namespace

TEST_F(IopDispatchTest, MatchesLegacyLeafForAllSelectors)
{
	constexpr auto probes = Probes<0>(std::make_index_sequence<288>{});
	std::copy_n(probes.begin(), 64, psxBSC);
	std::copy_n(probes.begin() + 64, 64, psxSPC);
	std::copy_n(probes.begin() + 128, 32, psxREG);
	std::copy_n(probes.begin() + 160, 32, psxCP0);
	std::copy_n(probes.begin() + 192, 64, psxCP2);
	std::copy_n(probes.begin() + 256, 32, psxCP2BSC);
	// Retain the original grouping handlers as the reference decoder.
	for (u32 op : {0u, 1u, 16u, 18u})
		psxBSC[op] = basic[op];
	psxCP2[0] = cp2[0];

	for (u32 op = 0; op < 64; op++)
		for (u32 function = 0; function < 64; function++)
			for (u32 reg = 0; reg < 32; reg++)
			{
				// Nonzero low operand bits keep SPECIAL/SLL distinct from exact NOP.
				psxRegs.code = (op << 26) | (reg << 21) | ((31 - reg) << 16) | 0x7fc0 | function;
				selected = nullptr;
				psxBSC[op]();
				const Handler expected = selected;
				ASSERT_NE(expected, nullptr);
				selected = nullptr;
				psxExecuteOpcode();
				ASSERT_EQ(selected, expected) << "opcode " << psxRegs.code;
			}
}

TEST_F(IopDispatchTest, NopPreservesCompleteArchitecturalState)
{
	// A nonzero backing value for r0 must also be left alone, as by psxSLL.
	std::memset(&psxRegs, 0xa5, sizeof(psxRegs));
	psxRegs.code = 0;
	std::array<u8, sizeof(psxRegs)> before;
	std::memcpy(before.data(), &psxRegs, sizeof(psxRegs));
	psxBSC[0]();
	EXPECT_EQ(std::memcmp(&psxRegs, before.data(), sizeof(psxRegs)), 0);
	psxExecuteOpcode();
	EXPECT_EQ(std::memcmp(&psxRegs, before.data(), sizeof(psxRegs)), 0);
}
