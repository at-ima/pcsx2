// SPDX-FileCopyrightText: 2026 PCSX2 Dev Team
// SPDX-License-Identifier: GPL-3.0+

#include "Common.h"
#include "MTVU.h"
#include "arm64/VU1Recompiler.h"
#include "arm64/VU1Pipeline.h"
#include "common/HostSys.h"
#include "vixl/aarch64/macro-assembler-aarch64.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <memory>
#include <optional>
#include <vector>

namespace
{
	using namespace vixl::aarch64;
	// Larger blocks amortize preparation, but increase generated code and validation work.
	constexpr u32 MaxInstructions = 256;
	// Free-space threshold before compiling both ordinary and deferred paths,
	// not a fixed allocation per block or an independent throughput setting.
	constexpr size_t MaxBlockBytes = MaxInstructions * 2048;

	using Arm64VU1::Instruction;

	constexpr size_t VectorOffset(u32 reg)
	{
		return reg == 32 ? offsetof(VURegs, ACC) : offsetof(VURegs, VF) + sizeof(VECTOR) * reg;
	}

	struct VectorCache
	{
		std::array<int, 33> slots;
		std::array<u32, 8> offsets;
		u32 count = 0;
		// Slots the block writes; exits store only these back.
		u8 dirty = 0;

		VectorCache()
		{
			slots.fill(-1);
			offsets.fill(~u32(0));
		}
		VRegister Host(u32 reg) const { return VRegister(8 + slots[reg], 128); }
	};

	enum FdivStatic : u8
	{
		FdivUnknown, // check at runtime
		FdivIdle, // nothing in the slot
		FdivBusy, // in the slot, not due yet
		FdivRetires, // due at this pair's cycle
	};

	struct RetirementSchedule
	{
		u8 cycles = 0; // Zero means that incoming timing is still unknown.
		u8 retired = 0;
		u8 remaining = 0;
		// Bit k: the k-th retired entry (oldest first) comes from a CLIP/FCSET,
		// which also writes back VI[REG_CLIP_FLAG] when it retires.
		u8 clip_retires = 0;
		// A divide issued earlier in the block may still be in the FDIV pipe here,
		// so this pair has to retire it itself instead of leaving that to the
		// generic per-pair preparation it is replacing.
		bool fdiv_pending = false;
		// What the FDIV slot does at this pair's start when the analysis knows
		// the cycles since the divide issued (FdivStatic). Deferred regions use it.
		u8 fdiv_static = 0;
		// The same for the EFU slot (ESADD..ERSQRT, retiring into P). An EFU op
		// or WAITP stalls until one cycle before the entry is due and retires
		// it (_vuTestEFUStalls), so there it goes as soon as it is enabled.
		bool efu_pending = false;
		u8 efu_static = 0;
		// Likewise for ILW/ILWR results in the IALU pipe. Only integer branches
		// stall on them, so every other pair keeps its timing and just drops
		// the entries that are due (VUPipeline::FlushIALU).
		bool ialu_pending = false;
	};

	// Pipeline state a block was entered with, captured by Execute() so that
	// the retirement analysis can start from known producer ages instead of
	// leaving the first seven pairs to the generic preparation. Most blocks
	// are entered a few cycles after the previous one exited, with two or
	// three FMAC results still in flight and often a divide.
	struct IncomingFmac
	{
		u32 regupper = 0, reglower = 0, xyzwupper = 0, xyzwlower = 0;
		bool flags = false; // writes the status flag (FSSET), which scheduling must not retire
		bool clip = false; // writes the clip flag (CLIP, FCSET), which a scheduled pair retires
		u32 age = 0; // cycles since issue, 3 meaning ready before the first pair
	};
	// Packed, since Execute() captures and compares one on every dispatch:
	// the entry count (3 bits) and the divide (6 bits), then 22 bits for each
	// FMAC entry, oldest first, two in each word; the IALU pipe in bits 53-56
	// and the EFU pipe in bits 57-62; the IALU registers in bits 44-59 of the
	// second word.
	struct IncomingProfile
	{
		u64 words[2] = {};
		bool operator==(const IncomingProfile&) const = default;
		u32 Count() const { return words[0] & 7; }
		// Pairs the incoming FDIV entry may still be pending; 0 if none.
		u32 Fdiv() const { return (words[0] >> 3) & 63; }
		// Pairs an incoming ILW/ILWR result may still be pending; 0 if the IALU
		// pipe is empty.
		u32 Ialu() const { return (words[0] >> 53) & 15; }
		// Pairs the incoming EFU entry may still be pending; 0 if none.
		u32 Efu() const { return (words[0] >> 57) & 63; }
		// The VI registers (bit n: VIn) the one ILW/ILWR still pending writes,
		// which stalls integer branches reading them until Ialu() - 1 cycles
		// after entry; AnyIaluRegs if more than one is pending.
		static constexpr u32 AnyIaluRegs = 0xffff;
		u32 IaluRegs() const { return (words[1] >> 44) & 0xffff; }
		void SetIaluRegs(u32 regs) { words[1] |= u64(regs & 0xffff) << 44; }
		static u32 Shift(u32 k) { return k < 2 ? 9 + 22 * k : 22 * (k - 2); }
		void Set(u32 count, u32 fdiv, u32 ialu, u32 efu) { words[0] |= count | (fdiv << 3) | (u64(ialu) << 53) | (u64(efu) << 57); }
		void SetFmac(u32 k, const IncomingFmac& e)
		{
			const u64 bits = e.age | (e.flags << 2) | (e.regupper << 3) | (e.reglower << 8) | (e.xyzwupper << 13) |
			                 (e.xyzwlower << 17) | (u64(e.clip) << 21);
			words[k / 2] |= bits << Shift(k);
		}
		IncomingFmac Fmac(u32 k) const
		{
			const u64 bits = words[k / 2] >> Shift(k);
			IncomingFmac e;
			e.age = bits & 3;
			e.flags = (bits >> 2) & 1;
			e.regupper = (bits >> 3) & 31;
			e.reglower = (bits >> 8) & 31;
			e.xyzwupper = (bits >> 13) & 15;
			e.xyzwlower = (bits >> 17) & 15;
			e.clip = (bits >> 21) & 1;
			return e;
		}
	};

	// An exit whose next PC is known links to the next block through one of
	// these: the generated exit jumps to `entry` without returning to
	// Execute(), provided the link is from the current generation, the target
	// passed source validation in this Execute() call, and the architectural
	// state lets Execute() enter a block at all. Execute() fills a slot the
	// first time the exit returns to it (see s_pending_link).
	struct LinkSlot
	{
		// One target per next PC seen. A subroutine's JR returns to every caller
		// through the same exit: SotC's transform loops call shared helpers, and
		// a single target sent ~1.4 exits per microprogram back to Execute().
		static constexpr u32 Ways = 4;
		struct Way
		{
			const void* entry = nullptr;
			const u32* epoch = nullptr; // the target's Block::validated_epoch
			u32 generation = 0;
			u32 pc = ~0u; // never a TPC
		};
		std::array<Way, Ways> ways{};
		u32 next_way = 0;
		// The exit state follows from the block's own incoming profile, so a
		// profiled target compiled for the state seen once always matches.
		bool deterministic = false;
	};

	struct Block
	{
		using Function = void (*)(u64 start, u64 cycles, u32 packet_pending);
		// What Execute() reads to choose a variant comes first; the arrays
		// below are tens of kilobytes.
		Function function = nullptr;
		u32 count = 0;
		bool has_branches = false;
		bool loops_to_entry = false;
		// Compiled for exactly this incoming state; Execute() only enters it
		// when the state matches. Such a block never loops natively: its loop
		// edge arrives with a different state and links to that one's variant.
		bool profiled = false;
		// Compiled for a profile that its loop edge could not keep, so it runs
		// unprofiled for every incoming state instead of being tried again.
		bool profile_rejected = false;
		IncomingProfile incoming;
		// The Execute() call in which this block last passed full source
		// validation; micro memory only changes between calls.
		u32 validated_epoch = 0;
		const void* linked_entry = nullptr;
		// Leading pairs whose cycle advance the analysis knows exactly.
		u32 known_prefix = 0;
		std::vector<LinkSlot> links;
		struct SourceRange
		{
			u32 pc, first, count;
		};
		std::vector<SourceRange> ranges;
		// Pairs that relaxed region exits looked ahead at (see ExitFlagsObserved),
		// validated with the block's own source.
		std::vector<SourceRange> guard_ranges;
		std::vector<u32> guard_words;
		std::array<Instruction, MaxInstructions> instructions{};
		std::array<RetirementSchedule, MaxInstructions> schedule{};
		// Per pair: which FMAC inputs (1: fs, 2: ft, 4: ACC) are known to be
		// clamped already in every lane the op uses (see AnalyzeClamps).
		std::array<u8, MaxInstructions> clamp_skip{};
		std::array<u32, MaxInstructions * 2> words{};
		std::array<u32, MaxInstructions> next_pc{};
		std::array<bool, MaxInstructions> delay{};
		VectorCache cache;
	};

	// Games often upload several microprograms to the same micro memory in turn
	// (Burnout 3 swaps them within every frame). One block per entry PC made each
	// swap back fail source validation and recompile, which took most of the VU1
	// thread's time. Keep a few variants per PC, most recently used first.
	// Profiled blocks add variants for the incoming states an entry PC sees,
	// usually one or two per program. Stop profiling a PC that keeps asking
	// for new ones rather than recompiling it indefinitely.
	constexpr size_t MaxVariants = 16;
	constexpr u8 MaxProfiledCompiles = 24;
	std::array<std::vector<std::unique_ptr<Block>>, VU1_PROGSIZE / 8> s_blocks;
	std::array<u8, VU1_PROGSIZE / 8> s_profiled_compiles;
	// Bumped whenever a compiled block may be freed or its code reused, which
	// drops every link. Slots start at generation 0.
	u32 s_link_generation = 1;
	// Blocks whose validated_epoch equals this passed source validation since
	// micro memory last changed. Advanced by the next Execute() after a Clear().
	u32 s_epoch = 1;
	std::atomic<bool> s_micro_changed{true};
	// Set by a linked exit that could not link, for Execute() to fill.
	LinkSlot* s_pending_link = nullptr;
	u64 s_dispatches = 0;
	u8* s_base = nullptr;
	u8* s_write = nullptr;
	u8* s_end = nullptr;
	u32 s_options = 0;
	Arm64VU1::PipelineCode s_pipeline;

	constexpr size_t VF(u32 reg) { return offsetof(VURegs, VF) + sizeof(VECTOR) * reg; }
	constexpr size_t VI(u32 reg) { return offsetof(VURegs, VI) + sizeof(REG_VI) * reg; }
	MemOperand Field(size_t offset) { return MemOperand(x19, offset); }

	u32 Options()
	{
		return (CHECK_VU_OVERFLOW(0) ? 1 : 0) | (CHECK_VU_OVERFLOW(1) ? 2 : 0) | (CHECK_VUADDSUBHACK ? 4 : 0) |
		       (EmuConfig.Cpu.VU1FPCR.GetDenormalsAreZero() ? 8 : 0) |
		       (CpuVU1 == &CpuArm64VU1 && !CHECK_XGKICKHACK ? 16 : 0) |
		       (THREAD_VU1 ? 32 : 0) | (EmuConfig.Speedhacks.vuFlagHack ? 64 : 0);
	}

	void InvalidateAll()
	{
		for (auto& variants : s_blocks)
			variants.clear();
		s_profiled_compiles.fill(0);
		s_link_generation++;
		s_write = s_base;
		s_pipeline = {};
		s_options = Options();
		s_micro_changed.store(true, std::memory_order_release);
	}


	enum class Op
	{
		None,
		Add,
		Sub,
		Mul,
		Madd,
		Msub,
		Max,
		Min,
		Abs,
		Itof,
		Ftoi,
		Clip,
		Opmula,
		Opmsub,
		Unsupported
	};
	struct Upper
	{
		Op op = Op::Unsupported;
		int broadcast = -1; // -1: vector, 0..3: lane, 4: I, 5: Q
		bool acc = false;
		u32 scale = 0;
	};

	Upper DecodeUpper(u32 code)
	{
		const u32 op = code & 0x3f;
		// D/T need the complete interpreter control path. An E pair is compiled as
		// the last pair of its trace; its delay slot and the program end stay
		// with the interpreter (see Compile()).
		if (code & 0x18000000)
			return {};
		if (op < 0x1c)
		{
			constexpr Op ops[] = {Op::Add, Op::Sub, Op::Madd, Op::Msub, Op::Max, Op::Min, Op::Mul};
			return {ops[op / 4], static_cast<int>(op % 4)};
		}
		if (op < 0x30)
		{
			constexpr Op ops[] = {Op::Mul, Op::Max, Op::Mul, Op::Min,
				Op::Add, Op::Madd, Op::Add, Op::Madd, Op::Sub, Op::Msub, Op::Sub, Op::Msub,
				Op::Add, Op::Madd, Op::Mul, Op::Max, Op::Sub, Op::Msub, Op::Opmsub, Op::Min};
			const int bc = op >= 0x28 ? -1 : (op == 0x1d || op == 0x1f || (op & 2)) ? 4 :
			                                                                          5;
			if (op == 0x22 && CHECK_VUADDSUBHACK)
				return {};
			return {ops[op - 0x1c], bc};
		}
		if (op < 0x3c)
			return {};
		const u32 sub = (code >> 6) & 31;
		const int lane = op & 3;
		if (sub < 4)
		{
			constexpr Op ops[] = {Op::Add, Op::Sub, Op::Madd, Op::Msub};
			return {ops[sub], lane, true};
		}
		if (sub == 4 || sub == 5)
		{
			constexpr u32 scales[] = {0, 4, 12, 15};
			return {sub == 4 ? Op::Itof : Op::Ftoi, -1, false, scales[lane]};
		}
		if (sub == 6)
			return {Op::Mul, lane, true};
		if (sub == 7 && lane == 0)
			return {Op::Mul, 5, true};
		if (sub == 7 && lane == 1)
			return {Op::Abs};
		if (sub == 7 && lane == 2)
			return {Op::Mul, 4, true};
		if (sub == 7 && lane == 3)
			return {Op::Clip};
		if (sub == 8 || sub == 9)
		{
			const bool ternary = lane & 1;
			return {sub == 8 ? (ternary ? Op::Madd : Op::Add) : (ternary ? Op::Msub : Op::Sub), lane < 2 ? 5 : 4, true};
		}
		if (sub == 10 && lane == 0)
			return {Op::Add, -1, true};
		if (sub == 10 && lane == 1)
			return {Op::Madd, -1, true};
		if (sub == 10 && lane == 2)
			return {Op::Mul, -1, true};
		if (sub == 11 && lane == 0)
			return {Op::Sub, -1, true};
		if (sub == 11 && lane == 1)
			return {Op::Msub, -1, true};
		if (sub == 11 && lane == 2)
			return {Op::Opmula, -1, true};
		if (sub == 11 && lane == 3)
			return {Op::None};
		return {};
	}

	enum class Lower
	{
		Lq,
		Sq,
		Lqi,
		Sqi,
		Lqd,
		Sqd,
		Iaddiu,
		Isubiu,
		Iadd,
		Isub,
		Iaddi,
		Iand,
		Ior,
		Move,
		Mr32,
		Mfir,
		Mfp,
		Mtir,
		Ilw,
		Ilwr,
		Isw,
		Fcand,
		Fceq,
		Fcor,
		Fcset,
		Fcget,
		Fseq,
		Fsset,
		Fsand,
		Fsor,
		Fmeq,
		Fmand,
		Fmor,
		Div,
		Sqrt,
		Rsqrt,
		Waitq,
		Esadd,
		Ersadd,
		Eleng,
		Erleng,
		Esum,
		Ercpr,
		Esqrt,
		Ersqrt,
		Waitp,
		Ibeq,
		Ibne,
		Ibgtz,
		Ibltz,
		Ibgez,
		Iblez,
		Xgkick,
		Branch,
		Bal,
		Jr,
		Jalr,
		Xtop,
		Xitop,
		Unsupported
	};
	Lower DecodeLower(u32 code)
	{
		switch (code >> 25)
		{
			case 0:
				return Lower::Lq;
			case 1:
				return Lower::Sq;
			case 4:
				return Lower::Ilw;
			case 5:
				return Lower::Isw;
			case 0x10:
				return Lower::Fceq;
			case 0x11:
				return Lower::Fcset;
			case 0x12:
				return Lower::Fcand;
			case 0x13:
				return Lower::Fcor;
			case 0x14:
				return Lower::Fseq;
			case 0x15:
				return Lower::Fsset;
			case 0x16:
				return Lower::Fsand;
			case 0x17:
				return Lower::Fsor;
			case 0x18:
				return Lower::Fmeq;
			case 0x1a:
				return Lower::Fmand;
			case 0x1b:
				return Lower::Fmor;
			case 0x1c:
				return Lower::Fcget;
			case 0x28:
				return Lower::Ibeq;
			case 0x29:
				return Lower::Ibne;
			case 0x2c:
				return Lower::Ibltz;
			case 0x2d:
				return Lower::Ibgtz;
			case 0x2e:
				return Lower::Iblez;
			case 0x2f:
				return Lower::Ibgez;
			case 8:
				return Lower::Iaddiu;
			case 9:
				return Lower::Isubiu;
			case 0x20:
				return Lower::Branch;
			case 0x21:
				return Lower::Bal;
			case 0x24:
				return Lower::Jr;
			case 0x25:
				return Lower::Jalr;
			case 0x40:
				switch (code & 0x3f)
				{
					case 0x30:
						return Lower::Iadd;
					case 0x31:
						return Lower::Isub;
					case 0x32:
						return Lower::Iaddi;
					case 0x34:
						return Lower::Iand;
					case 0x35:
						return Lower::Ior;
				}
				switch (code & 0x7ff)
				{
					case 0x37c:
						return Lower::Lqi;
					case 0x37d:
						return Lower::Sqi;
					case 0x37e:
						return Lower::Lqd;
					case 0x37f:
						return Lower::Sqd;
					case 0x33c:
						return Lower::Move;
					case 0x33d:
						return Lower::Mr32;
					case 0x3fd:
						return Lower::Mfir;
					case 0x67c:
						return Lower::Mfp;
					case 0x3fc:
						return Lower::Mtir;
					case 0x3bc:
						return Lower::Div;
					case 0x3bd:
						return Lower::Sqrt;
					case 0x3be:
						return Lower::Rsqrt;
					case 0x3bf:
						return Lower::Waitq;
					case 0x73c:
						return Lower::Esadd;
					case 0x73d:
						return Lower::Ersadd;
					case 0x73e:
						return Lower::Eleng;
					case 0x73f:
						return Lower::Erleng;
					case 0x77e:
						return Lower::Esum;
					case 0x7be:
						return Lower::Ercpr;
					case 0x7bc:
						return Lower::Esqrt;
					case 0x7bd:
						return Lower::Ersqrt;
					case 0x7bf:
						return Lower::Waitp;
					case 0x3fe:
						return Lower::Ilwr;
					case 0x6fc:
						return CpuVU1 == &CpuArm64VU1 && !CHECK_XGKICKHACK ? Lower::Xgkick : Lower::Unsupported;
					case 0x6bc:
						return Lower::Xtop;
					case 0x6bd:
						return Lower::Xitop;
				}
				break;
		}
		return Lower::Unsupported;
	}


	// The lower ops routed through the FDIV pipe, i.e. the ones the interpreter
	// sends into _vuTestFDIVStalls before executing the pair.
	bool IsFDIVPipe(Lower op)
	{
		return op == Lower::Div || op == Lower::Sqrt || op == Lower::Rsqrt || op == Lower::Waitq;
	}

	bool IsIntegerBranch(Lower op)
	{
		return op == Lower::Ibeq || op == Lower::Ibne || op == Lower::Ibgtz ||
		       op == Lower::Ibltz || op == Lower::Ibgez || op == Lower::Iblez;
	}

	// JR/JALR/BAL: always-taken branches with no compile-time-constant continuation
	// (JR/JALR's target is a register; BAL's is static but handled the same way for
	// one code path). EmitControlFlow, not EmitLower, generates these.
	bool IsRegisterBranch(Lower op)
	{
		return op == Lower::Jr || op == Lower::Jalr || op == Lower::Bal;
	}

	void StoreWord(MacroAssembler& a, u32 value, size_t offset)
	{
		a.Mov(w9, value);
		a.Str(w9, Field(offset));
	}

	// Stores the `mask` lanes (x = 8) of `value`: xy and zw as doublewords,
	// other lanes singly. v3 is scratch.
	void StoreMasked(MacroAssembler& a, VRegister value, MemOperand address, u32 mask)
	{
		if (!mask)
			return;
		if (mask == 15)
		{
			a.Str(value.Q(), address);
			return;
		}
		const Register base = address.GetBaseRegister();
		const s64 offset = address.GetOffset();
		for (u32 half = 0; half < 2; half++)
		{
			const u32 lanes = (mask >> (2 - half * 2)) & 3; // bit 1: the half's first lane
			if (lanes == 3)
			{
				if (half)
					a.Mov(d3, value.V2D(), 1);
				a.Str(half ? d3 : value.D(), MemOperand(base, offset + half * 8));
				continue;
			}
			for (u32 k = 0; k < 2; k++)
			{
				if (!(lanes & (2 >> k)))
					continue;
				const u32 lane = half * 2 + k;
				if (lane)
					a.Mov(s3, value.V4S(), lane);
				a.Str(lane ? s3 : value.S(), MemOperand(base, offset + lane * 4));
			}
		}
	}

	void LoadVector(MacroAssembler& a, const VectorCache& cache, VRegister value, u32 reg)
	{
		if (cache.slots[reg] >= 0)
			a.Mov(value.V16B(), cache.Host(reg).V16B());
		else
			a.Ldr(value.Q(), Field(VectorOffset(reg)));
	}

	// The cache register of `reg`, or `temp` loaded from memory. Read only.
	VRegister SourceVector(MacroAssembler& a, const VectorCache& cache, VRegister temp, u32 reg)
	{
		if (cache.slots[reg] >= 0)
			return cache.Host(reg);
		a.Ldr(temp.Q(), Field(VectorOffset(reg)));
		return temp;
	}

	// Copies the `mask` lanes of `value` into `dest`, xy and zw as doublewords.
	void MergeLanes(MacroAssembler& a, VRegister dest, VRegister value, u32 mask)
	{
		if (mask == 15)
		{
			if (!dest.Is(value))
				a.Mov(dest.V16B(), value.V16B());
			return;
		}
		for (u32 half = 0; half < 2; half++)
		{
			const u32 lanes = (mask >> (2 - half * 2)) & 3;
			if (lanes == 3)
				a.Ins(dest.V2D(), half, value.V2D(), half);
			else
				for (u32 k = 0; k < 2; k++)
					if (lanes & (2 >> k))
						a.Ins(dest.V4S(), half * 2 + k, value.V4S(), half * 2 + k);
		}
	}

	void StoreVector(MacroAssembler& a, const VectorCache& cache, VRegister value, u32 reg, u32 mask = 15)
	{
		if (cache.slots[reg] < 0)
			StoreMasked(a, value, Field(VectorOffset(reg)), mask);
		else
			MergeLanes(a, cache.Host(reg), value, mask);
	}

	// LQ and friends: loads the quadword at x1 + x0 * 16 into the `mask` lanes
	// of `reg`, straight into a fully written cache register.
	void LoadQuad(MacroAssembler& a, const VectorCache& cache, u32 reg, u32 mask)
	{
		const MemOperand address(x1, x0, LSL, 4);
		if (mask == 15 && cache.slots[reg] >= 0)
		{
			a.Ldr(cache.Host(reg).Q(), address);
			return;
		}
		a.Ldr(q0, address);
		StoreVector(a, cache, v0, reg, mask);
	}

	void AssignVectorCache(Block& block)
	{
		std::array<u32, 33> uses{};
		for (u32 i = 0; i < block.count; i++)
		{
			const auto& ins = block.instructions[i];
			for (const _VURegsNum* regs : {&ins.uregs, &ins.lregs})
			{
				if (regs->VFread0)
					uses[regs->VFread0]++;
				if (regs->VFread1)
					uses[regs->VFread1]++;
				if (regs->VFwrite)
					uses[regs->VFwrite]++;
				if (regs->VIread & (1 << REG_ACC_FLAG))
					uses[32]++;
				if (regs->VIwrite & (1 << REG_ACC_FLAG))
					uses[32]++;
			}
		}
		for (u32 slot = 0; slot < block.cache.offsets.size(); slot++)
		{
			const auto best = std::max_element(uses.begin(), uses.end());
			if (*best < 2)
				break;
			const u32 reg = best - uses.begin();
			block.cache.slots[reg] = slot;
			block.cache.offsets[slot] = VectorOffset(reg);
			block.cache.count++;
			*best = 0;
		}
		for (u32 i = 0; i < block.count; i++)
		{
			const auto& ins = block.instructions[i];
			for (const _VURegsNum* regs : {&ins.uregs, &ins.lregs})
			{
				if (regs == &ins.lregs && (ins.upper & 0x80000000))
					continue;
				if (regs->VFwrite && block.cache.slots[regs->VFwrite] >= 0)
					block.cache.dirty |= 1 << block.cache.slots[regs->VFwrite];
				if ((regs->VIwrite & (1 << REG_ACC_FLAG)) && block.cache.slots[32] >= 0)
					block.cache.dirty |= 1 << block.cache.slots[32];
			}
		}
	}

	// Unconditionally: matches vuDouble's denormal flush exactly, ignoring
	// VU1FPCR. Most callers want ClampInput below instead, which skips this
	// when hardware FZ makes it redundant -- but that shortcut only holds when
	// the clamped value is guaranteed to pass through more arithmetic before
	// being stored; a handful of EFU ops (ERCPR/ESQRT/ERSQRT) can store this
	// exact value completely unchanged, with no further instruction for
	// hardware FZ to act on.
	// v6/v7 hold the overflow clamp bounds (0x7f7fffff / 0xff7fffff), and v24
	// holds the FP exponent mask (0x7f800000), all for the whole block, loaded
	// once by EmitBlockConstants at block entry and after any call that may
	// reach the real _vuXGKICKTransfer (which is free to clobber caller-saved
	// v0-v7/v16-v31 under the AAPCS64 ABI, unlike our own hand-rolled
	// VU1Pipeline.cpp stubs, which never touch vector registers at all).
	// v8-v15 hold the VectorCache and v28-v31 are used by EmitDeferredRegion's
	// per-slot MAC/status/clip flag cache, so v6/v7/v24 are deliberately
	// outside both ranges. Never use s6/d6/s7/d7 as scratch either: a scalar
	// write zeroes the other lanes, which then clamps every later input's
	// y/z/w to 0 (the EFU reciprocals did this; Burnout 3's lighting broke).
	void ClampInputAlways(MacroAssembler& a, VRegister reg)
	{
		a.And(v17.V16B(), reg.V16B(), v24.V16B());
		a.Movi(v18.V4S(), 0x80000000);
		a.And(v18.V16B(), reg.V16B(), v18.V16B());
		a.Cmeq(v19.V4S(), v17.V4S(), 0);
		a.Bsl(v19.V16B(), v18.V16B(), reg.V16B());
		a.Mov(reg.V16B(), v19.V16B());
		if (CHECK_VU_OVERFLOW(0))
		{
			// Signed min clamps positive infinities/NaNs; unsigned min clamps
			// their negative encodings. Finite values and signed zeros are intact.
			a.Smin(reg.V4S(), reg.V4S(), v6.V4S());
			a.Umin(reg.V4S(), reg.V4S(), v7.V4S());
		}
	}

	void ClampInput(MacroAssembler& a, VRegister reg)
	{
		// Arithmetic flushes signed denormal inputs in hardware when FPCR.FZ is
		// enabled. Other FPCR modes still need the interpreter's explicit clamp.
		if (EmuConfig.Cpu.VU1FPCR.GetDenormalsAreZero())
		{
			if (CHECK_VU_OVERFLOW(0))
			{
				a.Smin(reg.V4S(), reg.V4S(), v6.V4S());
				a.Umin(reg.V4S(), reg.V4S(), v7.V4S());
			}
			return;
		}
		ClampInputAlways(a, reg);
	}

	// Clamped copies of whole VF registers in v26/v27, reused by later pairs of
	// a deferred region while the register is unchanged. Transform code reads
	// one vertex in four pairs (MULAx/MADDAy/MADDAz/MADDw), so its clamp was
	// repeated four times. Only deferred regions enable this; any C++ call
	// (see EmitBlockConstants), any VF write and any pair that is not plain
	// FMAC/IALU/load-store work forgets the copies. Needs proper testing
	// across more games.
	struct ClampedCopies
	{
		std::array<int, 2> reg{-1, -1};
		u32 next = 0;
		bool enabled = false;

		void Forget() { reg = {-1, -1}; }
		void Forget(u32 vf)
		{
			for (int& r : reg)
				if (r == static_cast<int>(vf))
					r = -1;
		}
	};
	ClampedCopies s_clamped_copies;

	// An FMAC input: VF `reg` (or ACC) with `lane` broadcast (-1: none), clamped
	// like ClampInput unless `clamped`. Returns the cache register itself when
	// nothing has to change, else `temp` (or a clamped copy); `copy` forces `temp`.
	VRegister FetchInput(MacroAssembler& a, const VectorCache& cache, VRegister temp, u32 reg, int lane, bool clamped, bool copy = false)
	{
		const bool daz = EmuConfig.Cpu.VU1FPCR.GetDenormalsAreZero();
		auto& copies = s_clamped_copies;
		if (!clamped && daz && CHECK_VU_OVERFLOW(0) && copies.enabled && lane < 0 && !copy && reg > 0 && reg < 32)
		{
			for (u32 k = 0; k < 2; k++)
			{
				if (copies.reg[k] == static_cast<int>(reg))
				{
					copies.next = k ^ 1;
					return VRegister(26 + k, 128);
				}
			}
			// Alternate, so the other input of the same pair keeps its copy.
			const u32 k = copies.next;
			copies.next ^= 1;
			const VRegister copy_reg(26 + k, 128);
			const VRegister value = SourceVector(a, cache, copy_reg, reg);
			a.Smin(copy_reg.V4S(), value.V4S(), v6.V4S());
			a.Umin(copy_reg.V4S(), copy_reg.V4S(), v7.V4S());
			copies.reg[k] = static_cast<int>(reg);
			return copy_reg;
		}
		VRegister value = SourceVector(a, cache, temp, reg);
		if (lane >= 0)
		{
			a.Dup(temp.V4S(), value.V4S(), lane);
			value = temp;
		}
		if (!clamped && daz && CHECK_VU_OVERFLOW(0))
		{
			a.Smin(temp.V4S(), value.V4S(), v6.V4S());
			a.Umin(temp.V4S(), temp.V4S(), v7.V4S());
			return temp;
		}
		if (!value.Is(temp) && (copy || (!clamped && !daz)))
		{
			a.Mov(temp.V16B(), value.V16B());
			value = temp;
		}
		if (!clamped && !daz)
			ClampInputAlways(a, temp);
		return value;
	}

	// Loads the shared per-block constants: the overflow-clamp bounds into
	// v6/v7 when this block's options actually need them, and the FP
	// exponent mask into v24 unconditionally (StoreMAC's MAC/status flag
	// classification always needs it). Call once at block entry, and again
	// after any Blr that might reach the real _vuXGKICKTransfer C++ function.
	// Z/S/U/O status bits from MAC bits: any lane of each group of four.
	void EmitMacToStatus(MacroAssembler& a, const Register& status, const Register& mac)
	{
		a.Orr(status, mac, Operand(mac, LSR, 1));
		a.Orr(status, status, Operand(status, LSR, 2));
		a.And(status, status, 0x1111);
		a.Orr(status, status, Operand(status, LSR, 3));
		a.Orr(status, status, Operand(status, LSR, 6));
		a.And(status, status, 15);
	}

	void EmitBlockConstants(MacroAssembler& a)
	{
		// Called after every C++ call, which may clobber v26/v27.
		s_clamped_copies.Forget();
		a.Movi(v24.V4S(), 0x7f800000);
		if (!CHECK_VU_OVERFLOW(0) && !CHECK_VU_OVERFLOW(1))
			return;
		a.Movi(v6.V4S(), 0x7f7fffff);
		a.Movi(v7.V4S(), 0xff7fffff);
	}

	// Set by EmitDeferredRegion around a pair whose flags nothing observes: StoreMAC
	// then leaves the weighted per-lane MAC bits in v(28 + slot) for the region's
	// sticky accumulator, instead of publishing the MAC/status scratch.
	int s_raw_flag_slot = -1;
	// Set instead under the VU flag hack (Speedhacks.vuFlagHack, on by default):
	// StoreMAC computes no flags at all, and the pair's sticky status bits are
	// dropped, as microVU does for status flags nothing reads (mVUsetFlags).
	bool s_discard_flags = false;
	// The current pair's Block::clamp_skip.
	u8 s_clamp_skip = 0;
	u32 s_store_mac_count = 0;

	// Clamps the result in v0 (v6/v7 hold the bounds for the whole block; see
	// EmitBlockConstants) and writes its `mask` lanes to `reg`, unless that is
	// VF0. A fully written cache register takes the last clamp step directly.
	void StoreResult(MacroAssembler& a, const VectorCache& cache, u32 reg, u32 mask)
	{
		if (CHECK_VU_OVERFLOW(1))
		{
			a.Smin(v0.V4S(), v0.V4S(), v6.V4S());
			if (reg && mask == 15 && cache.slots[reg] >= 0)
			{
				a.Umin(cache.Host(reg).V4S(), v0.V4S(), v7.V4S());
				return;
			}
			a.Umin(v0.V4S(), v0.V4S(), v7.V4S());
		}
		if (reg)
			StoreVector(a, cache, v0, reg, mask);
	}

	void StoreMAC(MacroAssembler& a, const VectorCache& cache, const Upper& op, u32 code, int mask_override = -1)
	{
		s_store_mac_count++;
		// OPMULA/OPMSUB always write xyz regardless of the encoded bits at this
		// position, which are not a destination mask for those two opcodes.
		const u32 mask = mask_override >= 0 ? static_cast<u32>(mask_override) : (code >> 21) & 15;
		const bool flush = EmuConfig.Cpu.VU1FPCR.GetFlushToZero();
		if (s_discard_flags)
		{
			// Only the result's own denormal flush and clamp remain.
			if (!flush)
			{
				a.And(v17.V16B(), v0.V16B(), v24.V16B());
				a.Cmeq(v19.V4S(), v17.V4S(), 0);
				a.Movi(v22.V4S(), 0x80000000);
				a.And(v22.V16B(), v0.V16B(), v22.V16B());
				a.Bsl(v19.V16B(), v22.V16B(), v0.V16B());
				a.Mov(v0.V16B(), v19.V16B());
			}
			StoreResult(a, cache, op.acc ? 32 : (code >> 6) & 31, mask);
			return;
		}
		// v0 is the result. Classify all lanes using the interpreter's FP zero test.
		// v24 holds the exponent mask for the whole block; see EmitBlockConstants.
		a.And(v17.V16B(), v0.V16B(), v24.V16B());
		a.Fcmeq(v18.V4S(), v0.V4S(), 0.0);
		if (!flush)
		{
			a.Cmeq(v19.V4S(), v17.V4S(), 0);
			a.Bic(v19.V16B(), v19.V16B(), v18.V16B()); // underflow
			a.Orr(v21.V16B(), v18.V16B(), v19.V16B());
		}
		a.Cmeq(v20.V4S(), v17.V4S(), v24.V4S()); // overflow
		// Weight each enabled lane by its architectural MAC bit before the
		// horizontal sum. The four bit groups do not overlap or carry.
		const auto weight = [mask](u32 lane) -> u64 { return mask & (8 >> lane); };
		a.Ldr(q22, weight(2) | (weight(3) << 32), weight(0) | (weight(1) << 32));
		a.And(v21.V16B(), flush ? v18.V16B() : v21.V16B(), v22.V16B());
		a.Sshr(v23.V4S(), v0.V4S(), 31);
		a.And(v23.V16B(), v23.V16B(), v22.V16B());
		a.Shl(v23.V4S(), v23.V4S(), 4);
		a.Orr(v21.V16B(), v21.V16B(), v23.V16B());
		if (!flush)
		{
			a.And(v23.V16B(), v19.V16B(), v22.V16B());
			a.Shl(v23.V4S(), v23.V4S(), 8);
			a.Orr(v21.V16B(), v21.V16B(), v23.V16B());
		}
		a.Shl(v22.V4S(), v22.V4S(), 12);
		a.And(v22.V16B(), v20.V16B(), v22.V16B());
		a.Orr(v21.V16B(), v21.V16B(), v22.V16B());
		if (s_raw_flag_slot >= 0)
			a.Mov(VRegister(28 + s_raw_flag_slot, 128).V16B(), v21.V16B());
		else
		{
			a.Addv(s22, v21.V4S());
			a.Fmov(w10, s22);
			a.Ldr(w9, Field(offsetof(VURegs, macflag)));
			a.And(w9, w9, 0xffff0000);
			a.Orr(w9, w9, w10);
			a.Str(w9, Field(offsetof(VURegs, macflag)));
			EmitMacToStatus(a, w11, w10);
			a.Str(w11, Field(offsetof(VURegs, statusflag)));
		}
		// FZ arithmetic has already produced signed zero for tiny results. The
		// reference FP comparison consequently reports zero, not underflow.
		if (!flush)
		{
			a.Movi(v22.V4S(), 0x80000000);
			a.And(v22.V16B(), v0.V16B(), v22.V16B());
			a.Bsl(v19.V16B(), v22.V16B(), v0.V16B());
			a.Mov(v0.V16B(), v19.V16B());
		}
		StoreResult(a, cache, op.acc ? 32 : (code >> 6) & 31, mask);
	}

	void EmitUpper(MacroAssembler& a, const VectorCache& cache, u32 code)
	{
		const Upper op = DecodeUpper(code);
		if (op.op == Op::None)
			return;
		const u32 fs = (code >> 11) & 31, ft = (code >> 16) & 31, fd = (code >> 6) & 31;
		const u32 mask = (code >> 21) & 15;
		if (op.op == Op::Add || op.op == Op::Sub || op.op == Op::Mul || op.op == Op::Madd || op.op == Op::Msub)
		{
			// Inputs are read from the vector cache in place where possible; the
			// result is computed into v0 (see StoreMAC).
			const bool fused = op.op == Op::Madd || op.op == Op::Msub;
			// FMUL/FMLA/FMLS read a broadcast operand's lane in place (`lane`);
			// only ADD/SUB need it duplicated first. Same per-lane arithmetic.
			const bool by_element = op.broadcast >= 0 && op.op != Op::Add && op.op != Op::Sub;
			int lane = -1;
			VRegister t;
			if (op.broadcast >= 4)
			{
				a.Ldr(s1, Field(VI(op.broadcast == 4 ? REG_I : REG_Q)));
				if (by_element)
					lane = 0;
				else
					a.Dup(v1.V4S(), v1.V4S(), 0);
				t = v1;
				if (!(s_clamp_skip & 2))
					ClampInput(a, v1);
			}
			else if (by_element)
			{
				// clamp_skip's ft bit covers just the broadcast lane.
				t = FetchInput(a, cache, v1, ft, -1, s_clamp_skip & 2);
				lane = op.broadcast;
			}
			else
				t = FetchInput(a, cache, v1, ft, op.broadcast, s_clamp_skip & 2);
			const VRegister s = FetchInput(a, cache, fused ? v2 : v0, fs, -1, s_clamp_skip & 1);
			switch (op.op)
			{
				case Op::Add:
					a.Fadd(v0.V4S(), s.V4S(), t.V4S());
					break;
				case Op::Sub:
					a.Fsub(v0.V4S(), s.V4S(), t.V4S());
					break;
				case Op::Mul:
					if (lane >= 0)
						a.Fmul(v0.V4S(), s.V4S(), t.S(), lane);
					else
						a.Fmul(v0.V4S(), s.V4S(), t.V4S());
					break;
				default:
					// Match the ARM64 interpreter's contracted multiply/add operations.
					FetchInput(a, cache, v0, 32, -1, s_clamp_skip & 4, true);
					if (op.op == Op::Madd)
					{
						if (lane >= 0)
							a.Fmla(v0.V4S(), s.V4S(), t.S(), lane);
						else
							a.Fmla(v0.V4S(), s.V4S(), t.V4S());
					}
					else if (lane >= 0)
						a.Fmls(v0.V4S(), s.V4S(), t.S(), lane);
					else
						a.Fmls(v0.V4S(), s.V4S(), t.V4S());
					break;
			}
			StoreMAC(a, cache, op, code);
			return;
		}
		// The other ops read fs (and ft) from the cache registers in place.
		if (op.op == Op::Clip)
		{
			// CLIP compares signed bit patterns, including non-finite inputs.
			// A denormal W uses the largest denormal threshold; FP compares differ.
			const VRegister fsv = SourceVector(a, cache, v0, fs);
			const VRegister ftv = SourceVector(a, cache, v1, ft);
			a.Umov(w0, ftv.V4S(), 3);
			a.And(w1, w0, 0x7fffffff);
			a.Mov(w2, 0x007fffff);
			a.Tst(w0, 0x7f800000);
			a.Csel(w1, w1, w2, ne);
			a.Dup(v1.V4S(), w1);
			a.Movi(v2.V4S(), 0x80000000);
			a.Eor(v2.V16B(), fsv.V16B(), v2.V16B());
			a.Cmgt(v0.V4S(), fsv.V4S(), v1.V4S());
			a.Cmgt(v2.V4S(), v2.V4S(), v1.V4S());
			// Pack +X/-X/+Y/-Y/+Z/-Z into the next six history bits.
			a.Mov(x0, 0x0000000400000001ull);
			a.Fmov(d3, x0);
			a.Mov(w0, 16);
			a.Ins(v3.V4S(), 2, w0);
			a.And(v0.V16B(), v0.V16B(), v3.V16B());
			a.Shl(v3.V4S(), v3.V4S(), 1);
			a.And(v2.V16B(), v2.V16B(), v3.V16B());
			a.Orr(v0.V16B(), v0.V16B(), v2.V16B());
			a.Addv(s0, v0.V4S());
			a.Fmov(w0, s0);
			a.Ldr(w1, Field(offsetof(VURegs, clipflag)));
			a.Orr(w0, w0, Operand(w1, LSL, 6));
			a.And(w0, w0, 0xffffff);
			a.Str(w0, Field(offsetof(VURegs, clipflag)));
			return;
		}
		if (op.op == Op::Abs || op.op == Op::Itof || op.op == Op::Ftoi)
		{
			if (!ft)
				return;
			const VRegister fsv = SourceVector(a, cache, v0, fs);
			VRegister result = v0;
			if (op.op == Op::Abs)
			{
				a.Movi(v1.V4S(), 0x7fffffff);
				a.And(v0.V16B(), fsv.V16B(), v1.V16B());
			}
			else if (op.op == Op::Itof)
			{
				a.Scvtf(v0.V4S(), fsv.V4S());
				if (op.scale)
				{
					a.Movi(v1.V4S(), 0x3f800000 - (op.scale << 23));
					a.Fmul(v0.V4S(), v0.V4S(), v1.V4S());
				}
			}
			else
			{
				VRegister x = fsv;
				if (op.scale)
				{
					a.Movi(v1.V4S(), 0x3f800000 + (op.scale << 23));
					a.Fmul(v0.V4S(), fsv.V4S(), v1.V4S());
					x = v0;
				}
				a.Movi(v1.V4S(), 0x7f800000);
				a.And(v2.V16B(), x.V16B(), v1.V16B());
				a.Movi(v1.V4S(), 0x4f000000);
				a.Cmhs(v2.V4S(), v2.V4S(), v1.V4S());
				a.Sshr(v4.V4S(), x.V4S(), 31);
				a.Movi(v1.V4S(), 0x7fffffff);
				a.Eor(v4.V16B(), v4.V16B(), v1.V16B());
				a.Fcvtzs(v0.V4S(), x.V4S());
				a.Bsl(v2.V16B(), v4.V16B(), v0.V16B());
				result = v2;
			}
			StoreVector(a, cache, result, ft, mask);
			return;
		}
		if (op.op == Op::Max || op.op == Op::Min)
		{
			if (!fd)
				return;
			const VRegister fsv = SourceVector(a, cache, v0, fs);
			VRegister tv = v1;
			if (op.broadcast < 0)
				tv = SourceVector(a, cache, v1, ft);
			else if (op.broadcast < 4)
				a.Dup(v1.V4S(), SourceVector(a, cache, v1, ft).V4S(), op.broadcast);
			else
			{
				a.Ldr(s1, Field(VI(op.broadcast == 4 ? REG_I : REG_Q)));
				a.Dup(v1.V4S(), v1.V4S(), 0);
			}
			a.And(v2.V16B(), fsv.V16B(), tv.V16B());
			a.Cmlt(v2.V4S(), v2.V4S(), 0);
			a.Smax(v4.V4S(), fsv.V4S(), tv.V4S());
			a.Smin(v0.V4S(), fsv.V4S(), tv.V4S());
			if (op.op == Op::Max)
				a.Bsl(v2.V16B(), v0.V16B(), v4.V16B());
			else
				a.Bsl(v2.V16B(), v4.V16B(), v0.V16B());
			StoreVector(a, cache, v2, fd, mask);
			return;
		}
		if (op.op == Op::Opmula || op.op == Op::Opmsub)
		{
			// Outer product: rotate Fs to yzx (into v2) and Ft to zxy (into v1)
			// before multiplying. EXT leaves the lane the rotation still needs
			// in w. The W lanes are never read (mask is fixed to xyz).
			const VRegister fsv = SourceVector(a, cache, v2, fs);
			a.Ext(v2.V16B(), fsv.V16B(), fsv.V16B(), 4);
			a.Ins(v2.V4S(), 2, v2.V4S(), 3);
			const VRegister ftv = SourceVector(a, cache, v1, ft);
			a.Ext(v1.V16B(), ftv.V16B(), ftv.V16B(), 12);
			a.Ins(v1.V4S(), 0, v1.V4S(), 3);
			if (!(s_clamp_skip & 1))
				ClampInput(a, v2);
			if (!(s_clamp_skip & 2))
				ClampInput(a, v1);
			if (op.op == Op::Opmula)
				a.Fmul(v0.V4S(), v2.V4S(), v1.V4S());
			else
			{
				// Match the ARM64 interpreter's contracted multiply/subtract.
				FetchInput(a, cache, v0, 32, -1, s_clamp_skip & 4, true);
				a.Fmls(v0.V4S(), v2.V4S(), v1.V4S());
			}
			StoreMAC(a, cache, op, code, 0xE);
		}
	}

	// The VI backup (_vuBackupVI) as a deferred region knows it at compile time.
	// Every pair's cycle count is known there, so the state is static after the
	// first backup in the region. (The incoming countdown is not bounded by two:
	// a restored state may hold anything.) Memory is brought up to date at the
	// region's exits.
	struct StaticViBackup
	{
		bool known = false;
		u32 left = 0;
		int reg = -1;
		bool reg_dirty = false;
	};
	StaticViBackup* s_vi_backup = nullptr;

	void BackupVI(MacroAssembler& a, u32 reg)
	{
		if (s_vi_backup && s_vi_backup->known)
		{
			auto& backup = *s_vi_backup;
			// Repeated writes keep the value from before the chain.
			if (!(backup.left && backup.reg == static_cast<int>(reg)))
			{
				a.Ldrh(w9, Field(VI(reg)));
				a.Str(w9, Field(offsetof(VURegs, VIOldValue)));
				backup.reg = static_cast<int>(reg);
				backup.reg_dirty = true;
			}
			backup.left = 2;
			return;
		}
		if (s_vi_backup)
			*s_vi_backup = {true, 2, static_cast<int>(reg), false};
		Label done;
		a.Ldrb(w9, Field(offsetof(VURegs, VIBackupCycles)));
		a.Ldr(w10, Field(offsetof(VURegs, VIRegNumber)));
		a.Cmp(w10, reg);
		a.Ccmp(w9, 0, ZFlag, eq);
		a.B(ne, &done);
		a.Ldrh(w9, Field(VI(reg)));
		a.Str(w9, Field(offsetof(VURegs, VIOldValue)));
		StoreWord(a, reg, offsetof(VURegs, VIRegNumber));
		a.Bind(&done);
		a.Mov(w9, 2);
		a.Strb(w9, Field(offsetof(VURegs, VIBackupCycles)));
	}

	// Shared by DIV/SQRT/RSQRT: a still-pending previous FDIV op stalls issue
	// (_vuTestFDIVStalls), same as the generic FMAC-read hazard the caller's
	// prepare stub covers. The interpreter retires the pipe (_vuTestPipes)
	// between that stall and the new op, so the old result must reach Q
	// before this one replaces it.
	// Set by EmitDeferredRegion around a divide whose status scratch was last
	// written by a flag instruction in the region: that leaves only Z/S/U/O
	// (VU_STAT_UPDATE), so FSSET's D/I sticky bits there are known clear.
	bool s_div_clean_scratch = false;

	// The status scratch bits a divide keeps: all but its own I/D.
	u32 DivScratchMask()
	{
		return s_div_clean_scratch ? 0xfffff3cf : 0xffffffcf;
	}

	// Set by EmitDeferredRegion around a pair whose FDIV slot is known to be
	// empty when its body runs (FdivStatic), so there is no stall to check.
	bool s_fdiv_idle = false;

	void EmitFDIVStall(MacroAssembler& a)
	{
		if (s_fdiv_idle)
			return;
		Label not_pending;
		a.Ldr(w9, Field(offsetof(VURegs, fdiv) + offsetof(fdivPipe, enable)));
		a.Cbz(w9, &not_pending);
		a.Ldr(x9, Field(offsetof(VURegs, fdiv) + offsetof(fdivPipe, sCycle)));
		a.Ldr(w10, Field(offsetof(VURegs, fdiv) + offsetof(fdivPipe, Cycle)));
		a.Add(x9, x9, x10);
		a.Cmp(x26, x9);
		a.Csel(x26, x9, x26, lo);
		// The caller only publishes its cached cycle for scheduled pairs, so
		// a stall taken here has to reach architectural state on its own.
		a.Str(x26, Field(offsetof(VURegs, cycle)));
		a.Ldr(w10, Field(offsetof(VURegs, fdiv) + offsetof(fdivPipe, reg)));
		a.Str(w10, Field(VI(REG_Q)));
		a.Ldr(w10, Field(VI(REG_STATUS_FLAG)));
		a.And(w10, w10, 0xfcf);
		a.Ldr(w11, Field(offsetof(VURegs, fdiv) + offsetof(fdivPipe, statusflag)));
		a.And(w11, w11, 0xc30);
		a.Orr(w10, w10, w11);
		a.Str(w10, Field(VI(REG_STATUS_FLAG)));
		// The forced advance above always reaches at least the entry's own
		// ready cycle, so it is fully retired now. DIV/SQRT/RSQRT immediately
		// re-arm the pipe below and would overwrite this either way, but
		// WAITQ issues nothing further and needs the pipe left empty, matching
		// _vuTestPipes retiring it once _vuTestFDIVStalls has advanced VU->cycle.
		a.Str(wzr, Field(offsetof(VURegs, fdiv) + offsetof(fdivPipe, enable)));
		// The ordinary per-pair prepare/retire already drained the FMAC ring
		// once, before this pair's own body could force the clock forward
		// above; entries that only became ready because of that forcing are
		// otherwise never drained (a run of unrelated pairs right after can
		// be swept into a deferred region that skips the runtime retire path
		// entirely, leaving them stuck until the block ends). This mirrors
		// _vuTestPipes running again, after _vuTestFDIVStalls, in the
		// interpreter's own per-instruction dispatch order.
		a.Mov(x16, reinterpret_cast<uintptr_t>(s_pipeline.retire_queues));
		a.Blr(x16);
		EmitBlockConstants(a);
		a.Bind(&not_pending);
	}

	// w0 = result bits, w1 = statusflag bits, both already computed by the caller.
	void EmitFDIVFinish(MacroAssembler& a, u32 cycles)
	{
		a.Str(w1, Field(offsetof(VURegs, statusflag)));
		// The op also leaves its result in the staging Q field itself, distinct
		// from the architectural VI(REG_Q) the FDIV pipe retires into later.
		a.Str(w0, Field(offsetof(VURegs, q)));
		a.Str(w0, Field(offsetof(VURegs, fdiv) + offsetof(fdivPipe, reg)));
		a.Str(w1, Field(offsetof(VURegs, fdiv) + offsetof(fdivPipe, statusflag)));
		a.Str(x26, Field(offsetof(VURegs, fdiv) + offsetof(fdivPipe, sCycle)));
		a.Mov(w9, cycles);
		a.Str(w9, Field(offsetof(VURegs, fdiv) + offsetof(fdivPipe, Cycle)));
		a.Mov(w9, 1);
		a.Str(w9, Field(offsetof(VURegs, fdiv) + offsetof(fdivPipe, enable)));
	}

	// Shared by every EFU-issuing op (ESADD..EEXP) and WAITP: a still-pending
	// previous EFU op stalls issue (_vuTestEFUStalls), mirroring EmitFDIVStall's
	// generic-hazard/pipe-retirement handling for Q, but for P. The interpreter
	// decrements the pending entry's own Cycle field by one before testing
	// readiness -- "the stall is released 1 cycle before P is updated"
	// (VUops.cpp) -- which only affects the forced VURegs::cycle advance below,
	// since every EFU-issuing op immediately overwrites the entry's Cycle right
	// after anyway (WAITP leaves it disabled instead); mutate efu.Cycle to
	// match exactly, needs proper testing right at the cycle-wrap boundary.
	// Set by EmitDeferredRegion: its pairs retire the EFU slot before the body.
	bool s_efu_idle = false;

	void EmitEFUStall(MacroAssembler& a)
	{
		if (s_efu_idle)
			return;
		Label not_pending;
		a.Ldr(w9, Field(offsetof(VURegs, efu) + offsetof(efuPipe, enable)));
		a.Cbz(w9, &not_pending);
		a.Ldr(w10, Field(offsetof(VURegs, efu) + offsetof(efuPipe, Cycle)));
		a.Sub(w10, w10, 1);
		a.Str(w10, Field(offsetof(VURegs, efu) + offsetof(efuPipe, Cycle)));
		a.Ldr(x9, Field(offsetof(VURegs, efu) + offsetof(efuPipe, sCycle)));
		a.Add(x9, x9, x10);
		a.Cmp(x26, x9);
		a.Csel(x26, x9, x26, lo);
		a.Str(x26, Field(offsetof(VURegs, cycle)));
		a.Ldr(w10, Field(offsetof(VURegs, efu) + offsetof(efuPipe, reg)));
		a.Str(w10, Field(VI(REG_P)));
		a.Str(wzr, Field(offsetof(VURegs, efu) + offsetof(efuPipe, enable)));
		a.Mov(x16, reinterpret_cast<uintptr_t>(s_pipeline.retire_queues));
		a.Blr(x16);
		EmitBlockConstants(a);
		a.Bind(&not_pending);
	}

	// w0 = result bits, already computed by the caller. EFU has no status-flag
	// interaction at all, unlike the FDIV pipe.
	void EmitEFUFinish(MacroAssembler& a, u32 cycles)
	{
		a.Str(w0, Field(offsetof(VURegs, p)));
		a.Str(w0, Field(offsetof(VURegs, efu) + offsetof(efuPipe, reg)));
		a.Str(x26, Field(offsetof(VURegs, efu) + offsetof(efuPipe, sCycle)));
		a.Mov(w9, cycles);
		a.Str(w9, Field(offsetof(VURegs, efu) + offsetof(efuPipe, Cycle)));
		a.Mov(w9, 1);
		a.Str(w9, Field(offsetof(VURegs, efu) + offsetof(efuPipe, enable)));
	}

	void EmitLower(MacroAssembler& a, const VectorCache& cache, u32 code)
	{
		const Lower op = DecodeLower(code);
		const u32 fs = (code >> 11) & 31, ft = (code >> 16) & 31, id = (code >> 6) & 15;
		const u32 is = fs & 15, it = ft & 15, mask = (code >> 21) & 15;
		if (op == Lower::Fcand || op == Lower::Fceq || op == Lower::Fcor)
		{
			// Read the retired flag instance, not the current CLIP accumulator.
			a.Ldr(w0, Field(VI(REG_CLIP_FLAG)));
			a.And(w0, w0, 0xffffff);
			a.Mov(w1, code & 0xffffff);
			if (op == Lower::Fcand)
				a.Tst(w0, w1);
			else if (op == Lower::Fceq)
				a.Cmp(w0, w1);
			else
			{
				a.Orr(w0, w0, w1);
				a.Mov(w1, 0xffffff);
				a.Cmp(w0, w1);
			}
			a.Cset(w0, op == Lower::Fcand ? ne : eq);
			a.Strh(w0, Field(VI(1)));
			return;
		}
		if (op == Lower::Fseq || op == Lower::Fsand || op == Lower::Fsor)
		{
			// Reads the retired status instance's low 12 bits, same as FCAND/FCEQ/FCOR
			// read the retired CLIP instance.
			if (!it)
				return;
			a.Ldr(w0, Field(VI(REG_STATUS_FLAG)));
			a.And(w0, w0, 0xfff);
			a.Mov(w1, (((code >> 21) & 1) << 11) | (code & 0x7ff));
			if (op == Lower::Fseq)
			{
				a.Cmp(w0, w1);
				a.Cset(w0, eq);
			}
			else if (op == Lower::Fsand)
				a.And(w0, w0, w1);
			else
				a.Orr(w0, w0, w1);
			a.Strh(w0, Field(VI(it)));
			return;
		}
		if (op == Lower::Fmeq || op == Lower::Fmand || op == Lower::Fmor)
		{
			if (!it)
				return;
			a.Ldr(w0, Field(VI(REG_MAC_FLAG)));
			a.And(w0, w0, 0xffff);
			a.Ldrh(w1, Field(VI(is)));
			if (op == Lower::Fmeq)
			{
				a.Cmp(w0, w1);
				a.Cset(w0, eq);
			}
			else if (op == Lower::Fmand)
				a.And(w0, w0, w1);
			else
				a.Orr(w0, w0, w1);
			a.Strh(w0, Field(VI(it)));
			return;
		}
		if (op == Lower::Fcget)
		{
			if (!it)
				return;
			a.Ldr(w0, Field(VI(REG_CLIP_FLAG)));
			a.And(w0, w0, 0xfff);
			a.Strh(w0, Field(VI(it)));
			return;
		}
		if (op == Lower::Fcset)
		{
			// Writes the staging clipflag, same as CLIP; the generic FMAC-pipe path
			// retires it into VI(REG_CLIP_FLAG) after four cycles.
			a.Mov(w0, code & 0xffffff);
			a.Str(w0, Field(offsetof(VURegs, clipflag)));
			return;
		}
		if (op == Lower::Fsset)
		{
			a.Ldr(w0, Field(offsetof(VURegs, statusflag)));
			a.And(w0, w0, 0x3f);
			a.Orr(w0, w0, (((code >> 21) & 1) << 11 | (code & 0x7ff)) & 0xfc0);
			a.Str(w0, Field(offsetof(VURegs, statusflag)));
			return;
		}
		if (op == Lower::Div)
		{
			// Mirrors _vuDIV/_vuFDIVAdd: compute now (matching the interpreter's
			// immediate statusflag write), then stage Q into the shared FDIV pipe
			// so the generic retirement code publishes it after 7 cycles, needs
			// proper testing against every division-by-zero/sign combination.
			EmitFDIVStall(a);
			const u32 fsf = (code >> 21) & 3, ftf = (code >> 23) & 3;
			LoadVector(a, cache, q0, fs);
			LoadVector(a, cache, q1, ft);
			a.Dup(v2.V4S(), v0.V4S(), fsf);
			a.Dup(v3.V4S(), v1.V4S(), ftf);
			ClampInput(a, v2); // fs, after vuDouble
			ClampInput(a, v3); // ft, after vuDouble
			a.Umov(w2, v2.V4S(), 0);
			a.Umov(w3, v3.V4S(), 0);
			Label is_zero, have_result, done;
			a.Fcmp(s3, 0.0);
			a.B(eq, &is_zero);
			a.Fdiv(s4, s2, s3);
			a.Dup(v4.V4S(), v4.V4S(), 0);
			ClampInput(a, v4); // result, after vuDouble
			a.Umov(w0, v4.V4S(), 0);
			a.Ldr(w1, Field(offsetof(VURegs, statusflag)));
			a.And(w1, w1, DivScratchMask());
			a.B(&have_result);
			a.Bind(&is_zero);
			{
				a.Ldr(w1, Field(offsetof(VURegs, statusflag)));
				a.And(w1, w1, DivScratchMask());
				a.Fcmp(s2, 0.0);
				Label fs_nonzero;
				a.B(ne, &fs_nonzero);
				a.Orr(w1, w1, 0x10); // fs == 0 && ft == 0: invalid (I flag)
				a.B(&done);
				a.Bind(&fs_nonzero);
				a.Orr(w1, w1, 0x20); // fs != 0 && ft == 0: divide-by-zero (D flag)
				a.Bind(&done);
			}
			// Signed max float, matching the sign of fs's numerator over ft's zero.
			a.Eor(w0, w2, w3);
			a.Mov(w4, 0x7f7fffff);
			a.Mov(w5, 0xff7fffff);
			a.Tst(w0, 0x80000000);
			a.Csel(w0, w5, w4, ne);
			a.Bind(&have_result);
			EmitFDIVFinish(a, 7);
			return;
		}
		if (op == Lower::Sqrt)
		{
			// Mirrors _vuSQRT: q = sqrt(fabs(ft)), needs proper testing against
			// every sign/denormal/non-finite combination.
			EmitFDIVStall(a);
			const u32 ftf = (code >> 23) & 3;
			LoadVector(a, cache, q1, ft);
			a.Dup(v3.V4S(), v1.V4S(), ftf);
			ClampInput(a, v3); // ft, after vuDouble
			a.Fabs(s4, s3);
			a.Fsqrt(s4, s4);
			a.Dup(v4.V4S(), v4.V4S(), 0);
			ClampInput(a, v4); // result, after vuDouble
			a.Umov(w0, v4.V4S(), 0);
			a.Ldr(w1, Field(offsetof(VURegs, statusflag)));
			a.And(w1, w1, DivScratchMask());
			// "pl" (N==0) matches a plain "ft < 0.0" comparison, including the
			// false-for-NaN case; "lt"/"ge" treat unordered operands as taken.
			a.Fcmp(s3, 0.0);
			Label not_negative;
			a.B(pl, &not_negative);
			a.Orr(w1, w1, 0x10); // I flag
			a.Bind(&not_negative);
			EmitFDIVFinish(a, 7);
			return;
		}
		if (op == Lower::Rsqrt)
		{
			// Mirrors _vuRSQRT: q = fs / sqrt(fabs(ft)), with ft == 0 branching
			// into a deeper zero-handling case than DIV's, needs proper testing
			// against every sign/zero/denormal/non-finite combination.
			EmitFDIVStall(a);
			const u32 fsf = (code >> 21) & 3, ftf = (code >> 23) & 3;
			LoadVector(a, cache, q0, fs);
			LoadVector(a, cache, q1, ft);
			a.Dup(v2.V4S(), v0.V4S(), fsf);
			a.Dup(v3.V4S(), v1.V4S(), ftf);
			ClampInput(a, v2); // fs, after vuDouble
			ClampInput(a, v3); // ft, after vuDouble
			a.Umov(w2, v2.V4S(), 0);
			a.Umov(w3, v3.V4S(), 0);
			Label ft_zero, have_result;
			a.Fcmp(s3, 0.0);
			a.B(eq, &ft_zero);
			a.Fabs(s5, s3);
			a.Fsqrt(s5, s5);
			a.Fdiv(s4, s2, s5);
			a.Dup(v4.V4S(), v4.V4S(), 0);
			ClampInput(a, v4); // result, after vuDouble
			a.Umov(w0, v4.V4S(), 0);
			a.Ldr(w1, Field(offsetof(VURegs, statusflag)));
			a.And(w1, w1, DivScratchMask());
			// See the SQRT case: "pl" is the correct false-for-NaN "ft < 0.0" test.
			a.Fcmp(s3, 0.0);
			a.B(pl, &have_result);
			a.Orr(w1, w1, 0x10); // I flag
			a.B(&have_result);
			a.Bind(&ft_zero);
			{
				a.Ldr(w1, Field(offsetof(VURegs, statusflag)));
				a.And(w1, w1, DivScratchMask());
				a.Orr(w1, w1, 0x20); // D flag, always: division by zero
				a.Eor(w0, w2, w3);
				a.And(w0, w0, 0x80000000);
				Label fs_zero;
				a.Fcmp(s2, 0.0);
				a.B(eq, &fs_zero);
				// fs != 0: signed max float, matching the sign of fs over ft's zero.
				a.Mov(w4, 0x7f7fffff);
				a.Orr(w0, w0, w4);
				a.B(&have_result);
				a.Bind(&fs_zero);
				// fs == 0 too: signed zero, and I joins D (both flags set).
				a.Orr(w1, w1, 0x10);
			}
			a.Bind(&have_result);
			EmitFDIVFinish(a, 13);
			return;
		}
		if (op == Lower::Waitq)
		{
			// _vuWAITQ's body is empty; its only effect is the pending-FDIV
			// stall every FDIV-pipe op takes before doing anything else. It
			// does not issue a new pipe entry, so there is nothing to finish.
			EmitFDIVStall(a);
			return;
		}
		if (op == Lower::Esadd || op == Lower::Ersadd || op == Lower::Eleng || op == Lower::Erleng)
		{
			// _vuESADD/_vuERSADD/_vuELENG/_vuERLENG all reduce fs.xyz to
			// p = fs.x^2 + fs.y^2 + fs.z^2, which clang contracts on ARM64 into
			// x*x followed by two fused multiply-adds; match that rounding.
			// They then diverge: ESADD stores it
			// directly, ERSADD takes its reciprocal (skipped when zero), ELENG
			// takes its square root (skipped when negative or NaN), ERLENG
			// chains both. None of the four clamp their output, matching the
			// interpreter (no vuDouble call on the result there), needs proper
			// testing against every sign/zero/denormal/non-finite combination.
			EmitEFUStall(a);
			LoadVector(a, cache, q0, fs);
			ClampInput(a, v0);
			a.Dup(v2.V4S(), v0.V4S(), 0);
			a.Dup(v3.V4S(), v0.V4S(), 1);
			a.Dup(v4.V4S(), v0.V4S(), 2);
			a.Fmul(s2, s2, s2);
			a.Fmadd(s5, s3, s3, s2);
			a.Fmadd(s5, s4, s4, s5);
			if (op == Lower::Ersadd)
			{
				Label done;
				a.Fcmp(s5, 0.0);
				a.B(eq, &done);
				a.Fmov(s1, 1.0f);
				a.Fdiv(s5, s1, s5);
				a.Bind(&done);
			}
			else if (op == Lower::Eleng || op == Lower::Erleng)
			{
				// "lt" is true for both a real negative sum and an unordered
				// (NaN) one, so this correctly skips the square root -- leaving
				// p as the original sum -- for exactly the cases where a plain
				// "p >= 0" test would be false in C.
				Label skip_sqrt;
				a.Fcmp(s5, 0.0);
				a.B(lt, &skip_sqrt);
				a.Fsqrt(s5, s5);
				if (op == Lower::Erleng)
				{
					Label done;
					a.Fcmp(s5, 0.0);
					a.B(eq, &done);
					a.Fmov(s1, 1.0f);
					a.Fdiv(s5, s1, s5);
					a.Bind(&done);
				}
				a.Bind(&skip_sqrt);
			}
			a.Fmov(w0, s5);
			u32 cycles = 11; // Esadd
			if (op == Lower::Ersadd || op == Lower::Eleng)
				cycles = 18;
			else if (op == Lower::Erleng)
				cycles = 24;
			EmitEFUFinish(a, cycles);
			return;
		}
		if (op == Lower::Esum)
		{
			// _vuESUM: p = fs.x + fs.y + fs.z + fs.w, left-to-right, no clamp.
			EmitEFUStall(a);
			LoadVector(a, cache, q0, fs);
			ClampInput(a, v0);
			a.Dup(v2.V4S(), v0.V4S(), 0);
			a.Dup(v3.V4S(), v0.V4S(), 1);
			a.Dup(v4.V4S(), v0.V4S(), 2);
			a.Dup(v5.V4S(), v0.V4S(), 3);
			a.Fadd(s2, s2, s3);
			a.Fadd(s2, s2, s4);
			a.Fadd(s2, s2, s5);
			a.Fmov(w0, s2);
			EmitEFUFinish(a, 12);
			return;
		}
		if (op == Lower::Ercpr || op == Lower::Esqrt || op == Lower::Ersqrt)
		{
			// _vuERCPR/_vuESQRT/_vuERSQRT read a single Fs[fsf] lane directly,
			// unlike the FDIV pipe's SQRT/RSQRT which take fabs() first: ERCPR
			// takes its reciprocal (skipped when zero); ESQRT/ERSQRT take its
			// square root, skipped entirely -- p keeps the original value --
			// when negative or NaN; ERSQRT chains a reciprocal after. No output
			// clamp, matching the interpreter, needs proper testing against
			// every sign/zero/denormal/non-finite combination.
			const u32 fsf = mask & 3;
			EmitEFUStall(a);
			LoadVector(a, cache, q0, fs);
			a.Dup(v2.V4S(), v0.V4S(), fsf);
			// The "skip" branch below can store this value completely
			// unchanged, with no arithmetic instruction for hardware FZ to act
			// on, so it needs the unconditional clamp (see ClampInputAlways).
			ClampInputAlways(a, v2);
			if (op == Lower::Ercpr)
			{
				// _vuERCPR divides "1.0" (a double literal, unlike ERSADD/
				// ERLENG/ERSQRT's "1.0f") by p, so the division itself happens
				// in double precision and only the final assignment back to
				// float rounds -- a different result than a native single-
				// precision division for some inputs. Widen, divide, narrow.
				Label done;
				a.Fcmp(s2, 0.0);
				a.B(eq, &done);
				a.Fcvt(d2, s2);
				a.Fmov(d1, 1.0);
				a.Fdiv(d2, d1, d2);
				a.Fcvt(s2, d2);
				a.Bind(&done);
			}
			else
			{
				Label skip_sqrt;
				a.Fcmp(s2, 0.0);
				a.B(lt, &skip_sqrt);
				a.Fsqrt(s2, s2);
				if (op == Lower::Ersqrt)
				{
					Label done;
					a.Fcmp(s2, 0.0);
					a.B(eq, &done);
					a.Fmov(s1, 1.0f);
					a.Fdiv(s2, s1, s2);
					a.Bind(&done);
				}
				a.Bind(&skip_sqrt);
			}
			a.Fmov(w0, s2);
			EmitEFUFinish(a, op == Lower::Ersqrt ? 18 : 12);
			return;
		}
		if (op == Lower::Waitp)
		{
			// _vuWAITP's body is empty; its only effect is the pending-EFU
			// stall every EFU-pipe op takes before doing anything else.
			EmitEFUStall(a);
			return;
		}
		if (op == Lower::Ilw || op == Lower::Ilwr)
		{
			if (!it || !mask)
				return;
			a.Ldrh(w0, Field(VI(is)));
			if (op == Lower::Ilw)
			{
				const s32 imm = static_cast<s32>(code << 21) >> 21;
				a.Add(w0, w0, imm);
			}
			// ILWR has no immediate: VI[Is] is already the quadword index.
			a.And(w0, w0, 0x3ff);
			a.Ldr(x1, Field(offsetof(VURegs, Mem)));
			a.Add(x1, x1, Operand(x0, LSL, 4));
			const u32 lane = (mask & 1) ? 3 : (mask & 2) ? 2 :
			                              (mask & 4)     ? 1 :
			                                               0;
			a.Ldrh(w0, MemOperand(x1, lane * 4));
			a.Strh(w0, Field(VI(it))); // Neither instruction creates an arithmetic VI backup.
			return;
		}
		if (op == Lower::Xtop || op == Lower::Xitop)
		{
			if (!it)
				return;
			// THREAD_VU1 (MTVU) redirects the VIF1 registers this pipe reads to the
			// snapshot the VU1 thread owns; baked in at compile time like the
			// Xgkick/CHECK_XGKICKHACK decode gate above, since the setting can't
			// change while this block is executing.
			VIFregisters& vifRegs = THREAD_VU1 ? vu1Thread.vifRegs : vif1Regs;
			const u32* const src = op == Lower::Xtop ? &vifRegs.top : &vifRegs.itop;
			a.Mov(x0, reinterpret_cast<uintptr_t>(src));
			a.Ldr(w0, MemOperand(x0));
			a.Strh(w0, Field(VI(it)));
			return;
		}
		if (op == Lower::Isw)
		{
			// Unlike ILW's single selected lane, ISW writes each masked lane
			// independently (X/Y/Z/W are separate addresses), so it still writes
			// when It == 0 and StoreMasked's usual broadcast-then-mask fits directly.
			const s32 imm = static_cast<s32>(code << 21) >> 21;
			a.Ldrh(w0, Field(VI(is)));
			a.Add(w0, w0, imm);
			a.And(w0, w0, 0x3ff);
			a.Ldr(x1, Field(offsetof(VURegs, Mem)));
			a.Add(x1, x1, Operand(x0, LSL, 4));
			a.Ldrh(w0, Field(VI(it)));
			a.Dup(v0.V4S(), w0);
			StoreMasked(a, v0, MemOperand(x1), mask);
			return;
		}
		if (op == Lower::Move || op == Lower::Mr32 || op == Lower::Mfir || op == Lower::Mfp)
		{
			if (!ft)
				return;
			if (op == Lower::Mfir)
			{
				a.Ldrsh(w0, Field(VI(is)));
				a.Dup(v0.V4S(), w0);
			}
			else if (op == Lower::Mfp)
			{
				// Reads P as it stands: MFP does not wait for a pending EFU op
				// (that is WAITP's job). Pairs reading P while one is pending
				// stay on the generic path, which retires it at the right cycle.
				a.Ldr(w0, Field(VI(REG_P)));
				a.Dup(v0.V4S(), w0);
			}
			else
			{
				const VRegister value = SourceVector(a, cache, v0, fs);
				if (op == Lower::Mr32)
					a.Ext(v0.V16B(), value.V16B(), value.V16B(), 4);
				else if (!value.Is(v0))
				{
					// MOVE between cached registers or out to memory: no copy.
					StoreVector(a, cache, value, ft, mask);
					return;
				}
			}
			StoreVector(a, cache, v0, ft, mask);
			return;
		}
		if (op == Lower::Lqi || op == Lower::Sqi || op == Lower::Lqd || op == Lower::Sqd)
		{
			const bool load = op == Lower::Lqi || op == Lower::Lqd;
			const bool decrement = op == Lower::Lqd || op == Lower::Sqd;
			const u32 base = load ? is : it;
			// Preserve the interpreter's guards, including the full encoded register
			// fields used by LQI/SQI/SQD. Even a suppressed update creates a backup.
			const bool update = (load ? (decrement ? is : fs) : ft) != 0;
			BackupVI(a, base);
			a.Ldrh(w2, Field(VI(base)));
			if (decrement && update)
				a.Sub(w2, w2, 1);
			if (!load || ft)
			{
				a.And(w0, w2, 0x3ff);
				a.Ldr(x1, Field(offsetof(VURegs, Mem)));
				if (load)
					LoadQuad(a, cache, ft, mask);
				else
				{
					a.Add(x1, x1, Operand(x0, LSL, 4));
					StoreMasked(a, SourceVector(a, cache, v0, fs), MemOperand(x1), mask);
				}
			}
			if (update)
			{
				if (!decrement)
					a.Add(w2, w2, 1);
				a.Strh(w2, Field(VI(base)));
			}
			return;
		}
		if (op == Lower::Lq || op == Lower::Sq)
		{
			if (op == Lower::Lq && !ft)
				return;
			const s32 imm = static_cast<s32>(code << 21) >> 21;
			a.Ldrh(w0, Field(VI(op == Lower::Lq ? is : it)));
			a.Add(w0, w0, imm);
			a.And(w0, w0, 0x3ff);
			a.Ldr(x1, Field(offsetof(VURegs, Mem)));
			if (op == Lower::Lq)
				LoadQuad(a, cache, ft, mask);
			else
			{
				a.Add(x1, x1, Operand(x0, LSL, 4));
				StoreMasked(a, SourceVector(a, cache, v0, fs), MemOperand(x1), mask);
			}
			return;
		}
		const bool immediate = op == Lower::Iaddiu || op == Lower::Isubiu || op == Lower::Iaddi || op == Lower::Mtir;
		const u32 dest = immediate ? it : id;
		if (!dest)
			return;
		BackupVI(a, dest);
		if (op == Lower::Mtir)
		{
			if (cache.slots[fs] >= 0)
				a.Umov(w0, cache.Host(fs).V8H(), ((code >> 21) & 3) * 2);
			else
				a.Ldrh(w0, Field(VF(fs) + ((code >> 21) & 3) * 4));
		}
		else
		{
			a.Ldrh(w0, Field(VI(is)));
			if (immediate)
			{
				const s32 imm = op == Lower::Iaddi ? static_cast<s32>(code << 21) >> 27 : ((code >> 10) & 0x7800) | (code & 0x7ff);
				if (op == Lower::Isubiu)
					a.Sub(w0, w0, imm);
				else
					a.Add(w0, w0, imm);
			}
			else
			{
				a.Ldrh(w1, Field(VI(it)));
				switch (op)
				{
					case Lower::Iadd:
						a.Add(w0, w0, w1);
						break;
					case Lower::Isub:
						a.Sub(w0, w0, w1);
						break;
					case Lower::Iand:
						a.And(w0, w0, w1);
						break;
					case Lower::Ior:
						a.Orr(w0, w0, w1);
						break;
					default:
						break;
				}
			}
		}
		a.Strh(w0, Field(VI(dest)));
	}

	// q28..q31 and x25..x28 are reserved for deferred pipeline state.
	void EmitPair(MacroAssembler& a, const VectorCache& cache, const Instruction& ins, bool publish_code)
	{
		const bool immediate = ins.upper & 0x80000000;
		const bool discard = !immediate && ins.uregs.VFwrite && ins.uregs.VFwrite == ins.lregs.VFwrite;
		const u32 backup = !immediate && !discard && ins.uregs.VFwrite &&
		                           (ins.uregs.VFwrite == ins.lregs.VFread0 || ins.uregs.VFwrite == ins.lregs.VFread1) ?
		                       ins.uregs.VFwrite :
		                       0;
		// The backup below uses v26/v27.
		const bool copies_enabled = s_clamped_copies.enabled;
		if (backup)
		{
			s_clamped_copies.Forget();
			s_clamped_copies.enabled = false;
			LoadVector(a, cache, q27, backup);
		}
		// Every FDIV-pipe op's stall-and-retire must land before the paired upper
		// instruction runs: VU1microInterp.cpp calls _vuTestLowerStalls/
		// _vuTestPipes ahead of _vu1ExecUpper, precisely so an upper op
		// broadcasting Q in the same pair observes the freshly retired value
		// instead of whatever was pending beforehand. This covers WAITQ and
		// DIV/SQRT/RSQRT alike -- a "MULq + DIV" pair reads the *previous*
		// divide's Q. EmitLower's own cases still call EmitFDIVStall afterwards,
		// but it is then a no-op (fdiv.enable is already clear by then).
		if (!immediate && IsFDIVPipe(DecodeLower(ins.lower)))
			EmitFDIVStall(a);
		if (publish_code)
			StoreWord(a, ins.upper, offsetof(VURegs, code));
		EmitUpper(a, cache, ins.upper);
		if (immediate)
			StoreWord(a, ins.lower, VI(REG_I));
		else if (!discard)
		{
			if (backup)
			{
				LoadVector(a, cache, q26, backup);
				StoreVector(a, cache, q27, backup);
			}
			if (publish_code)
				StoreWord(a, ins.lower, offsetof(VURegs, code));
			if (DecodeLower(ins.lower) != Lower::Branch && !IsIntegerBranch(DecodeLower(ins.lower)) &&
				DecodeLower(ins.lower) != Lower::Xgkick && !IsRegisterBranch(DecodeLower(ins.lower)))
				EmitLower(a, cache, ins.lower);
			if (backup)
				StoreVector(a, cache, q26, backup);
		}
		s_clamped_copies.enabled = copies_enabled;
		if (immediate || discard)
			s_clamped_copies.Forget(ins.uregs.VFwrite);
		else
		{
			s_clamped_copies.Forget(ins.uregs.VFwrite);
			s_clamped_copies.Forget(ins.lregs.VFwrite);
		}
	}

	void EmitControlFlow(MacroAssembler& a, const Block& block, u32 index)
	{
		const auto& ins = block.instructions[index];
		if (!(ins.upper & 0x80000000) && DecodeLower(ins.lower) == Lower::Branch)
		{
			const s32 displacement = (static_cast<s32>(ins.lower << 21) >> 21) * 8;
			StoreWord(a, (ins.pc + 8 + displacement) & VU1_PROGMASK, offsetof(VURegs, branchpc));
			StoreWord(a, 1, offsetof(VURegs, branch));
		}
		else if (!(ins.upper & 0x80000000) && IsIntegerBranch(DecodeLower(ins.lower)))
		{
			Label done;
			const auto load_operand = [&](const Register& dest, u32 reg) {
				if (s_vi_backup && s_vi_backup->known)
				{
					const bool old = s_vi_backup->left && s_vi_backup->reg == static_cast<int>(reg);
					a.Ldrsh(dest, Field(old ? offsetof(VURegs, VIOldValue) : VI(reg)));
					return;
				}
				Label current;
				a.Ldrsh(dest, Field(VI(reg)));
				a.Ldrb(w9, Field(offsetof(VURegs, VIBackupCycles)));
				a.Cbz(w9, &current);
				a.Ldr(w9, Field(offsetof(VURegs, VIRegNumber)));
				a.Cmp(w9, reg);
				a.B(ne, &current);
				a.Ldrsh(dest, Field(offsetof(VURegs, VIOldValue)));
				a.Bind(&current);
			};
			load_operand(w0, (ins.lower >> 11) & 15);
			const Lower op = DecodeLower(ins.lower);
			if (op == Lower::Ibgtz || op == Lower::Ibltz || op == Lower::Ibgez || op == Lower::Iblez)
			{
				// Skip (to done) on the condition opposite the one that takes the branch.
				a.Cmp(w0, 0);
				Condition skip;
				switch (op)
				{
					case Lower::Ibgtz:
						skip = le;
						break;
					case Lower::Ibltz:
						skip = ge;
						break;
					case Lower::Ibgez:
						skip = lt;
						break;
					default: // Iblez
						skip = gt;
						break;
				}
				a.B(skip, &done);
			}
			else
			{
				load_operand(w1, (ins.lower >> 16) & 15);
				a.Cmp(w0, w1);
				a.B(op == Lower::Ibeq ? ne : eq, &done);
			}
			const s32 displacement = (static_cast<s32>(ins.lower << 21) >> 21) * 8;
			StoreWord(a, (ins.pc + 8 + displacement) & VU1_PROGMASK, offsetof(VURegs, branchpc));
			StoreWord(a, 1, offsetof(VURegs, branch));
			a.Bind(&done);
		}
		else if (!(ins.upper & 0x80000000) && IsRegisterBranch(DecodeLower(ins.lower)))
		{
			const Lower op = DecodeLower(ins.lower);
			const u32 it_reg = (ins.lower >> 16) & 15;
			if (op == Lower::Bal)
			{
				const s32 displacement = (static_cast<s32>(ins.lower << 21) >> 21) * 8;
				StoreWord(a, (ins.pc + 8 + displacement) & VU1_PROGMASK, offsetof(VURegs, branchpc));
			}
			else
			{
				// JR/JALR: runtime target from VI[Is].US[0] * 8. Unlike the integer
				// branches above, _vuJR/_vuJALR read the live register directly with
				// no VI-backup bypass, so this doesn't apply one either.
				const u32 is_reg = (ins.lower >> 11) & 15;
				a.Ldrh(w0, Field(VI(is_reg)));
				a.Lsl(w0, w0, 3);
				a.And(w0, w0, VU1_PROGMASK);
				a.Str(w0, Field(offsetof(VURegs, branchpc)));
			}
			if (op != Lower::Jr && it_reg)
			{
				// Compile() never lets a JR/JALR/BAL land in a pending delay slot
				// (it bails out of the trace instead), so VU->branch==1 can't be true
				// here. _vuJALR/_vuBAL then link to (TPC+8)/8, but TPC has already
				// been advanced past this instruction by the time they read it, so
				// the return address is two pairs ahead: this instruction's own
				// address + 16, not + 8.
				a.Mov(w0, (ins.pc + 16) / 8);
				a.Strh(w0, Field(VI(it_reg)));
			}
			StoreWord(a, 1, offsetof(VURegs, branch));
		}
		else if (ins.upper & 0x40000000)
		{
			// What _vu1Exec() leaves after an E pair: ebit 2, counted down once.
			StoreWord(a, 1, offsetof(VURegs, ebit));
		}
		else if (block.delay[index])
		{
			const auto& prev = block.instructions[index - 1];
			const bool register_target = !(prev.upper & 0x80000000) && IsRegisterBranch(DecodeLower(prev.lower));
			StoreWord(a, 0, offsetof(VURegs, branch));
			if (register_target)
			{
				// The preceding JR/JALR/BAL already resolved branchpc itself (at
				// runtime for JR/JALR, statically for BAL); Compile() never gives this
				// delay slot a compile-time next_pc to fall back on, so copy it instead
				// of storing a literal.
				a.Ldr(w9, Field(offsetof(VURegs, branchpc)));
				a.Str(w9, Field(VI(REG_TPC)));
			}
			else
			{
				StoreWord(a, block.next_pc[index], VI(REG_TPC));
			}
		}
	}

	void EmitFmacMetadata(MacroAssembler& a, const Instruction& ins)
	{
		const bool upper = ins.uregs.pipe == VUPIPE_FMAC;
		const bool lower = ins.lregs.pipe == VUPIPE_FMAC;
		const u64 regs = (upper ? ins.uregs.VFwrite : 0) | (u64(lower ? ins.lregs.VFwrite : 0) << 32);
		const u64 flags = (upper ? ins.uregs.VIwrite : 0) | (lower ? ins.lregs.VIwrite : 0);
		a.Mov(x9, regs);
		a.Str(x9, MemOperand(x0, offsetof(fmacPipe, regupper)));
		a.Mov(x9, flags | (u64(upper ? ins.uregs.VFwxyzw : 0) << 32));
		a.Str(x9, MemOperand(x0, offsetof(fmacPipe, flagreg)));
		a.Mov(x9, lower ? ins.lregs.VFwxyzw : 0);
		a.Str(x9, MemOperand(x0, offsetof(fmacPipe, xyzwlower))); // also clear padding
	}

	void EmitFinish(MacroAssembler& a, const Instruction& ins)
	{
		static_assert(sizeof(fmacPipe) == 48 && offsetof(fmacPipe, reglower) == 4 &&
					  offsetof(fmacPipe, flagreg) == 8 && offsetof(fmacPipe, xyzwupper) == 12 &&
					  offsetof(fmacPipe, xyzwlower) == 16 && offsetof(fmacPipe, sCycle) == 24 &&
					  offsetof(fmacPipe, Cycle) == 32 && offsetof(fmacPipe, macflag) == 36 &&
					  offsetof(fmacPipe, statusflag) == 40 && offsetof(fmacPipe, clipflag) == 44);
		const bool upper = ins.uregs.pipe == VUPIPE_FMAC;
		const bool lower = ins.lregs.pipe == VUPIPE_FMAC;
		if (!upper && !lower)
			return;
		// The supported lower instructions either use FMAC or have zero IALU latency.
		// Emit the same queue entry as _vuClearFMAC + _vuAddUpper/LowerStalls.
		// w12 keeps the original fmacwritepos around so the increment below
		// doesn't need to reload what x0's address computation already consumed.
		a.Ldr(w12, Field(offsetof(VURegs, fmacwritepos)));
		a.Mov(w1, sizeof(fmacPipe));
		a.Madd(x0, x12, x1, x19);
		a.Add(x0, x0, offsetof(VURegs, fmac));
		EmitFmacMetadata(a, ins);
		a.Str(x26, MemOperand(x0, offsetof(fmacPipe, sCycle)));
		a.Mov(w9, 4);
		a.Ldr(w10, Field(offsetof(VURegs, macflag)));
		a.Stp(w9, w10, MemOperand(x0, offsetof(fmacPipe, Cycle)));
		a.Ldr(w9, Field(offsetof(VURegs, statusflag)));
		a.Ldr(w10, Field(offsetof(VURegs, clipflag)));
		a.Stp(w9, w10, MemOperand(x0, offsetof(fmacPipe, statusflag)));
		a.Ldr(w9, Field(offsetof(VURegs, fmaccount)));
		a.Add(w9, w9, 1);
		a.Str(w9, Field(offsetof(VURegs, fmaccount)));
		a.Add(w9, w12, 1);
		a.And(w9, w9, 3);
		a.Str(w9, Field(offsetof(VURegs, fmacwritepos)));
	}

	void EmitKick(MacroAssembler& a, u32 code)
	{
		Label fresh;
		a.Ldr(w9, Field(offsetof(VURegs, xgkickenable)));
		a.Cbz(w9, &fresh);
		a.Str(x26, Field(offsetof(VURegs, cycle)));
		a.Mov(x16, reinterpret_cast<uintptr_t>(s_pipeline.flush_kick));
		a.Blr(x16);
		a.Ldr(x26, Field(offsetof(VURegs, cycle)));
		EmitBlockConstants(a);
		a.Bind(&fresh);
		// Read VI after flushing, matching the reference lower-op ordering.
		a.Ldrh(w0, Field(VI((code >> 11) & 15)));
		a.And(w0, w0, 0x3ff);
		a.Lsl(w0, w0, 4);
		a.Str(w0, Field(offsetof(VURegs, xgkickaddr)));
		a.Mov(w9, VU1_MEMSIZE);
		a.Sub(w9, w9, w0);
		a.Str(w9, Field(offsetof(VURegs, xgkickdiff)));
		StoreWord(a, VURegs::XgkickPacket, offsetof(VURegs, xgkickenable));
		StoreWord(a, 0, offsetof(VURegs, xgkicksizeremaining));
		StoreWord(a, 0, offsetof(VURegs, xgkickendpacket));
		StoreWord(a, 1, offsetof(VURegs, xgkickcyclecount));
		a.Str(x26, Field(offsetof(VURegs, xgkicklastcycle)));
		// VPU_STAT's VGW bit is EE-thread state; the MTVU thread must not write it.
		if (!THREAD_VU1)
		{
			a.Mov(x0, reinterpret_cast<uintptr_t>(&VU0.VI[REG_VPU_STAT].UL));
			a.Ldr(w9, MemOperand(x0));
			a.Orr(w9, w9, 1 << 12);
			a.Str(w9, MemOperand(x0));
		}
	}

	void EmitIntegerIssue(MacroAssembler& a, const Instruction& ins)
	{
		if (ins.lregs.pipe != VUPIPE_IALU || !ins.lregs.cycles)
			return;
		a.Ldr(w0, Field(offsetof(VURegs, ialuwritepos)));
		a.Mov(w1, sizeof(ialuPipe));
		a.Madd(x1, x0, x1, x19);
		a.Add(x1, x1, offsetof(VURegs, ialu));
		a.Str(x26, MemOperand(x1, offsetof(ialuPipe, sCycle)));
		a.Mov(w9, ins.lregs.cycles);
		a.Str(w9, MemOperand(x1, offsetof(ialuPipe, Cycle)));
		a.Mov(w9, ins.lregs.VIwrite);
		a.Str(w9, MemOperand(x1, offsetof(ialuPipe, reg)));
		a.Add(w0, w0, 1);
		a.And(w0, w0, 3);
		a.Str(w0, Field(offsetof(VURegs, ialuwritepos)));
		a.Ldr(w0, Field(offsetof(VURegs, ialucount)));
		a.Add(w0, w0, 1);
		a.Str(w0, Field(offsetof(VURegs, ialucount)));
	}

	bool HasFmac(const Instruction& ins)
	{
		return ins.uregs.pipe == VUPIPE_FMAC || ins.lregs.pipe == VUPIPE_FMAC;
	}

	// Whether EmitUpper computes MAC/status flags for this upper instruction.
	bool UpdatesMacFlags(u32 upper)
	{
		switch (DecodeUpper(upper).op)
		{
			case Op::Add:
			case Op::Sub:
			case Op::Mul:
			case Op::Madd:
			case Op::Msub:
			case Op::Opmula:
			case Op::Opmsub:
				return true;
			default:
				return false;
		}
	}

	// Every MAC-updating FMAC result leaves StoreMAC clamped and free of
	// denormals, and clamping is idempotent, so an input lane last written
	// that way needs no input clamp. Tracks those lanes (x = 8 .. w = 1) of
	// VF0..VF31 and ACC (32) through the trace. Any other write forgets them,
	// including interpreter fallbacks (whose ACC writes are not described).
	// In Shadow of the Colossus the transform chains read ACC and results of
	// the previous pair, so about half of the input clamps go.
	void AnalyzeClamps(Block& block)
	{
		block.clamp_skip.fill(0);
		if (!CHECK_VU_OVERFLOW(1))
			return;
		std::array<u8, 33> clamped{};
		clamped[0] = 15; // VF0 is (0, 0, 0, 1)
		for (u32 i = 0; i < block.count; i++)
		{
			const auto& ins = block.instructions[i];
			const u32 code = ins.upper;
			const Upper op = DecodeUpper(code);
			const u32 fs = (code >> 11) & 31, ft = (code >> 16) & 31, fd = (code >> 6) & 31;
			const bool opm = op.op == Op::Opmula || op.op == Op::Opmsub;
			const u32 mask = opm ? 0xE : (code >> 21) & 15;
			const bool mac = UpdatesMacFlags(code);
			if (mac)
			{
				u8 skip = 0;
				if ((clamped[fs] & mask) == mask)
					skip |= 1;
				if (op.broadcast < 0 ? (clamped[ft] & mask) == mask : op.broadcast < 4 && (clamped[ft] & (8 >> op.broadcast)))
					skip |= 2;
				if ((clamped[32] & mask) == mask)
					skip |= 4;
				block.clamp_skip[i] = skip;
				const u32 dest = op.acc ? 32 : fd;
				if (dest)
					clamped[dest] |= mask;
			}
			else
			{
				if (ins.uregs.VFwrite)
					clamped[ins.uregs.VFwrite] &= ~ins.uregs.VFwxyzw;
				if (op.op == Op::Unsupported)
					clamped[32] = 0;
			}
			if (!(code & 0x80000000) && ins.lregs.VFwrite)
				clamped[ins.lregs.VFwrite] &= ~ins.lregs.VFwxyzw;
			clamped[0] = 15;
		}
	}

	void AnalyzeRetirement(Block& block)
	{
		// Ages saturate at four (already retired). -1 represents an age which
		// still depends on incoming timing. Every pair advances at least one cycle.
		std::array<int, MaxInstructions> ages{};
		ages.fill(-1);
		// A profiled block also knows the FMAC entries it was entered with. They
		// are older than every producer in the block, oldest first like the queue.
		const u32 phantoms = block.profiled ? block.incoming.Count() : 0;
		std::array<IncomingFmac, 4> incoming;
		std::array<int, 4> phantom_ages{};
		for (u32 k = 0; k < phantoms; k++)
		{
			incoming[k] = block.incoming.Fmac(k);
			phantom_ages[k] = incoming[k].age;
		}
		u32 integer_ready = block.profiled ? block.incoming.Ialu() : 0;
		// The block's own ILW/ILWR results: per VI register, the cycle (on the
		// `elapsed` scale) at which the latest one stops stalling integer
		// branches, or -1. Before `ialu_unknown`, incoming ones may be pending.
		// (Once `elapsed` is unknown it stays so, along with these.)
		std::array<int, 16> ialu_due;
		ialu_due.fill(-1);
		const u32 incoming_ialu_regs = block.profiled ? block.incoming.IaluRegs() : 0;
		const u32 ialu_unknown = !block.profiled ? 4 :
		                         incoming_ialu_regs == IncomingProfile::AnyIaluRegs ? block.incoming.Ialu() :
		                                                                              0;
		if (incoming_ialu_regs != IncomingProfile::AnyIaluRegs)
			for (u32 reg = 0; reg < 16; reg++)
				if (incoming_ialu_regs & (1 << reg))
					ialu_due[reg] = static_cast<int>(block.incoming.Ialu()) - 1;
		u32 efu_ready = block.profiled ? block.incoming.Efu() : 0;
		int efu_due = block.profiled && block.incoming.Efu() ? static_cast<int>(block.incoming.Efu()) - 1 : -1;
		bool efu_retired = false;
		u32 fdiv_ready = block.profiled ? block.incoming.Fdiv() : 0;
		// Cycles from block entry to the current pair, while every advance so
		// far is known, and the cycle on that scale at which the pending divide
		// comes due (-1: unknown). The profile records the incoming divide's
		// remaining cycles plus one.
		int elapsed = 0;
		int fdiv_due = block.profiled && block.incoming.Fdiv() ? static_cast<int>(block.incoming.Fdiv()) - 1 : -1;
		// Whether the pending divide has come due at a pair with a known cycle.
		bool fdiv_retired = false;
		block.known_prefix = block.count;
		for (u32 i = 0; i < block.count; i++)
		{
			const auto& ins = block.instructions[i];
			int cycles = -1;
			// A pair that reads no VF has nothing for the FMAC hazard scan to match,
			// so it advances exactly one cycle whatever the incoming pipeline holds.
			// That holds in the block's prologue too, where the dependency resolution
			// below cannot run yet (most prologue pairs read no VF). Leaving it
			// unknown there marks the ages of earlier producers unknown as well, and
			// a later pair whose own stall depends on one of them inherits that,
			// which can carry the unknown forward into the producers a scheduled
			// pair has to account for. The overrides further down still demote the
			// cases that stall on something other than the FMAC pipe.
			if (!ins.readsVF)
				cycles = 1;
			else if (block.profiled && i < 3)
			{
				// The newest conflicting producer decides the stall: every FMAC result
				// has the same latency, so older ones are ready no later. From the
				// fourth pair on the incoming entries are ready anyway and the
				// in-block dependency below applies unchanged.
				int age = 4;
				bool found = false;
				for (u32 j = i; j-- > 0;)
				{
					const auto& producer = block.instructions[j];
					if ((producer.uregs.pipe == VUPIPE_FMAC && (ins.readMasks[producer.uregs.VFwrite] & producer.uregs.VFwxyzw)) ||
						(producer.lregs.pipe == VUPIPE_FMAC && (ins.readMasks[producer.lregs.VFwrite] & producer.lregs.VFwxyzw)))
					{
						age = ages[j];
						found = true;
						break;
					}
				}
				for (u32 k = phantoms; !found && k-- > 0;)
				{
					const auto& entry = incoming[k];
					if ((ins.readMasks[entry.regupper] & entry.xyzwupper) || (ins.readMasks[entry.reglower] & entry.xyzwlower))
					{
						age = phantom_ages[k];
						found = true;
					}
				}
				if (age >= 0)
					cycles = std::max(1, 4 - age);
			}
			else if (i >= 3)
			{
				if (ins.dependency == 0)
					cycles = 1;
				else
				{
					int slots = ins.dependency;
					for (u32 distance = 1; distance <= 3; distance++)
					{
						if (HasFmac(block.instructions[i - distance]) && --slots == 0)
						{
							const int age = ages[i - distance];
							if (age >= 0)
								cycles = std::max(1, 4 - age);
							break;
						}
					}
				}
			}
			// VI waits and kick/transfer callbacks can change timing. Forget exact
			// producer ages until enough new pairs establish a known schedule.
			// An integer branch waits only for ILW/ILWR results in the IALU pipe.
			// Once the block's own loads have matured the branch advances like any
			// other pair: a profiled block knows its incoming entries, and an
			// unprofiled one's have retired after four pairs (ILW latency is four,
			// and every pair advances at least one cycle).
			const bool integer_branch = ins.lregs.pipe == VUPIPE_BRANCH && ins.lregs.VIread;
			bool integer_wait = integer_branch && !(i >= integer_ready && (block.profiled || i >= 4));
			// Within the latency of the block's own loads, the stall is known like
			// a divide's: _vuTestALUStalls advances the branch to the load's
			// stamp (the end of its pair) plus its latency, if that is later.
			int ialu_target = -1;
			if (integer_wait && i >= ialu_unknown && elapsed >= 0)
			{
				integer_wait = false;
				for (u32 reg = 0; reg < 16; reg++)
					if (ins.lregs.VIread & (1 << reg))
						ialu_target = std::max(ialu_target, ialu_due[reg]);
			}
			if (integer_wait || ins.lregs.pipe == VUPIPE_XGKICK ||
				(i && block.instructions[i - 1].lregs.pipe == VUPIPE_XGKICK))
				cycles = -1;
			// A divide/WAITQ or an EFU op/WAITP issued while an earlier entry is
			// still in its pipe stalls the pair until that entry retires
			// (_vuTestFDIVStalls/_vuTestEFUStalls), so VURegs::cycle jumps by an
			// amount nothing here can predict. Hand the age tracking below an
			// unknown advance instead. Reading Q or P does not stall: a scheduled
			// pair retires a pending divide itself (fdiv_pending) before it reads
			// Q, and waits for the EFU pipe to drain (efu_ready) before it can read
			// P. Read before this pair's own issue updates fdiv_ready/efu_ready,
			// since a pipe it arms itself cannot be what it stalls on.
			const bool fdiv_op = ins.lregs.pipe == VUPIPE_FDIV, efu_op = ins.lregs.pipe == VUPIPE_EFU;
			// With the divide's due cycle known, its stall is not a guess: the
			// pair advances to that cycle if it is later than where the FMAC
			// hazards alone would leave it. The pair itself stays generic (WAITQ
			// and divides write Q), but later pairs keep known producer ages.
			// This is SotC's transform loop, which waits on the previous
			// iteration's divide at its top. Needs proper testing across games.
			if (fdiv_op && i < fdiv_ready)
				cycles = (cycles > 0 && elapsed >= 0 && fdiv_due >= 0) ? std::max(cycles, fdiv_due - elapsed) : -1;
			if (efu_op && i < efu_ready)
				cycles = (cycles > 0 && elapsed >= 0 && efu_due >= 0) ? std::max(cycles, efu_due - 1 - elapsed) : -1;
			if (cycles > 0 && ialu_target >= 0)
				cycles = std::max(cycles, ialu_target - elapsed);
			// An unprofiled block may be entered with a divide (up to 13 cycles)
			// or an EFU operation (up to 54) in flight. Each pair advances at
			// least one cycle.
			if (!block.profiled && ((fdiv_op && i < 13) || (efu_op && i < 54)))
				cycles = -1;
			// The slot as this pair starts, before its own issue. A scheduled pair
			// with no divide of the block's (or the profile's) in reach finds it
			// empty: readiness admitted the schedule only with an idle slot.
			u8 fdiv_static = FdivUnknown;
			if (i >= fdiv_ready)
				fdiv_static = FdivIdle;
			else if (elapsed >= 0 && fdiv_due >= 0 && cycles > 0)
			{
				if (fdiv_retired)
					fdiv_static = FdivIdle;
				else if (elapsed + cycles >= fdiv_due)
				{
					fdiv_static = FdivRetires;
					fdiv_retired = true;
				}
				else
					fdiv_static = FdivBusy;
			}
			u8 efu_static = FdivUnknown;
			if (i >= efu_ready)
				efu_static = FdivIdle;
			else if (elapsed >= 0 && efu_due >= 0 && cycles > 0)
			{
				if (efu_retired)
					efu_static = FdivIdle;
				else if (elapsed + cycles >= efu_due - (efu_op ? 1 : 0))
				{
					efu_static = FdivRetires;
					efu_retired = true;
				}
				else
					efu_static = FdivBusy;
			}
			if (ins.lregs.pipe == VUPIPE_IALU && ins.lregs.cycles)
			{
				integer_ready = i + 5;
				const int due = (elapsed >= 0 && cycles > 0) ? elapsed + cycles + static_cast<int>(ins.lregs.cycles) : -1;
				for (u32 reg = 0; reg < 16; reg++)
					if (ins.lregs.VIwrite & (1 << reg))
						ialu_due[reg] = due;
			}
			// Divides are frequent enough in transform code that excluding their whole
			// latency from scheduling costs far more than retiring the pipe's single
			// slot inline: those pairs carry fdiv_pending instead and do it themselves
			// (see EmitScheduledPrepare and EmitDeferredRegion).
			if (ins.lregs.pipe == VUPIPE_FDIV && ins.lregs.cycles)
			{
				fdiv_retired = false;
				fdiv_ready = i + ins.lregs.cycles + 1;
				// _vuFDIVAdd stamps the cycle the pair ends on.
				fdiv_due = (elapsed >= 0 && cycles > 0) ? elapsed + cycles + static_cast<int>(ins.lregs.cycles) : -1;
			}
			// The EFU slot the same way: Burnout 3's lighting loops keep an EFU op in
			// flight nearly all the time. Needs proper testing across games.
			if (ins.lregs.pipe == VUPIPE_EFU && ins.lregs.cycles)
			{
				efu_retired = false;
				efu_ready = i + ins.lregs.cycles + 1;
				efu_due = (elapsed >= 0 && cycles > 0) ? elapsed + cycles + static_cast<int>(ins.lregs.cycles) : -1;
			}
			if (cycles <= 0 && block.known_prefix == block.count)
				block.known_prefix = i;
			// Without a profile the first seven pairs stay generic: three whose
			// stalls depend on the unknown incoming entries, then four for the
			// ages of their producers to become known.
			if ((block.profiled || i >= 7) && cycles > 0)
			{
				RetirementSchedule plan{static_cast<u8>(cycles)};
				plan.fdiv_pending = i < fdiv_ready;
				plan.fdiv_static = fdiv_static;
				// Before this pair's own issue moved efu_ready.
				plan.efu_pending = efu_static != FdivIdle;
				plan.efu_static = efu_static;
				plan.ialu_pending = i < integer_ready;
				for (u32 k = 0; k < phantoms && plan.cycles; k++)
				{
					if (phantom_ages[k] < 0)
						plan.cycles = 0;
					else if (phantom_ages[k] >= 4)
						continue;
					else if (phantom_ages[k] + cycles < 4)
						plan.remaining++;
					else
					{
						if (incoming[k].flags)
							plan.cycles = 0;
						// Like a CLIP issued in the block (see below).
						if (incoming[k].clip)
							plan.clip_retires |= 1 << plan.retired;
						plan.retired++;
					}
				}
				for (u32 j = i >= 4 ? i - 4 : 0; j < i; j++)
				{
					const auto& producer = block.instructions[j];
					if (!HasFmac(producer))
						continue;
					if (ages[j] < 0)
					{
						plan.cycles = 0;
						break;
					}
					if (ages[j] >= 4)
						continue;
					if (ages[j] + cycles < 4)
						plan.remaining++;
					else
					{
						// FSSET's status write stays on the generic path.
						const u32 flags = producer.uregs.VIwrite | producer.lregs.VIwrite;
						if (flags & (1 << REG_STATUS_FLAG))
							plan.cycles = 0;
						if (flags & (1 << REG_CLIP_FLAG))
							plan.clip_retires |= 1 << plan.retired;
						plan.retired++;
					}
				}
				// A scheduled pair retires FMAC entries into memory, so it can read
				// the flags. Q and P are final here (a pending divide is retired
				// above via fdiv_pending, and the EFU is idle). WAITQ only carries
				// the Q write as a tag (see Compile); with its stall known above,
				// the divide it waits on is due by the end of this pair's advance,
				// so fdiv_pending retires it before the body and the body's own
				// stall check finds the pipe empty. DIV/SQRT/RSQRT are the same
				// plus a new entry, stamped with the cycle this pair ends on, which
				// is where a scheduled pair has already moved x26. Splitting
				// regions at every divide cost SotC's transform loops ~10% of the
				// VU1 thread in region exits. EFU issues stay generic.
				const bool waitq = !(ins.upper & 0x80000000) && DecodeLower(ins.lower) == Lower::Waitq;
				const bool fdiv_issue = ins.lregs.pipe == VUPIPE_FDIV && ins.lregs.cycles;
				// EFU ops and WAITP likewise: the slot retires before the body.
				const bool efu_issue = ins.lregs.pipe == VUPIPE_EFU;
				if ((ins.lregs.VIwrite & ((1 << REG_Q) | (1 << REG_P))) && !waitq && !fdiv_issue && !efu_issue)
					plan.cycles = 0;
				// Flag, Q and P readers stay in a region, which publishes the
				// status and MAC flags it keeps in registers before them. An
				// untaken integer branch leaves it through its own exit.
				block.schedule[i] = plan;
			}
			for (u32 k = 0; k < phantoms; k++)
			{
				int& age = phantom_ages[k];
				if (cycles > 0 && age >= 0)
					age = std::min(4, age + cycles);
				else if (static_cast<int>(incoming[k].age + i) + std::max(1, cycles) >= 4 || (age >= 0 && age + std::max(1, cycles) >= 4))
					age = 4;
				else
					age = -1;
			}
			for (u32 j = 0; j < i; j++)
			{
				if (cycles > 0 && ages[j] >= 0)
					ages[j] = std::min(4, ages[j] + cycles);
				else if (static_cast<int>(i - j - 1) + std::max(1, cycles) >= 4 ||
						 (ages[j] >= 0 && ages[j] + std::max(1, cycles) >= 4))
					ages[j] = 4;
				else
					ages[j] = -1;
			}
			ages[i] = 0;
			elapsed = (elapsed >= 0 && cycles > 0) ? elapsed + cycles : -1;
		}
	}

	// `fdiv_pending`: the pair retires the FDIV slot itself (EmitFDIVSlotRetire
	// checks it at runtime), so a busy divide does not block it. The divide in
	// flight there is the block's own or the profiled incoming one: an in-block
	// divide stalls until any earlier one retires, and every later pair that
	// does not retire the slot is past that divide's latency.
	void EmitScheduleReadiness(MacroAssembler& a, bool fdiv_pending = false, bool ialu_pending = false, bool efu_pending = false)
	{
		// Bit 0 validates incoming FMAC timing and excludes callbacks. Bit 1
		// additionally permits scheduled execution once special queues drain.
		Label done;
		a.Cmp(w25, 1);
		a.B(ne, &done);
		for (size_t offset : {offsetof(VURegs, fdiv) + offsetof(fdivPipe, enable),
				 offsetof(VURegs, efu) + offsetof(efuPipe, enable), offsetof(VURegs, ialucount)})
		{
			if ((fdiv_pending && offset == offsetof(VURegs, fdiv) + offsetof(fdivPipe, enable)) ||
				(efu_pending && offset == offsetof(VURegs, efu) + offsetof(efuPipe, enable)) ||
				(ialu_pending && offset == offsetof(VURegs, ialucount)))
				continue;
			a.Ldr(w9, Field(offset));
			a.Cbnz(w9, &done);
		}
		a.Mov(w25, 3);
		a.Bind(&done);
	}

	void EmitScheduleGuard(MacroAssembler& a)
	{
		Label done, loop, ready;
		a.Mov(w25, 0);
		// XGKICK may call C++ and change state during the prefix. Promotion
		// requires a fresh guard after the explicit packet boundary.
		a.Ldr(w9, Field(offsetof(VURegs, xgkickenable)));
		a.Cbnz(w9, &done);
		a.Ldr(x9, Field(offsetof(VURegs, cycle)));
		// Each pair can advance four cycles, including dependency stalls.
		a.Cmn(x9, MaxInstructions * 4 + 4);
		a.B(hs, &done);
		a.Ldr(w10, Field(offsetof(VURegs, fmaccount)));
		a.Cmp(w10, 4);
		a.B(hi, &done);
		a.Ldr(w11, Field(offsetof(VURegs, fmacreadpos)));
		a.Cmp(w11, 3);
		a.B(hi, &done);
		a.Ldr(w12, Field(offsetof(VURegs, fmacwritepos)));
		a.Add(w13, w11, w10);
		a.And(w13, w13, 3);
		a.Cmp(w12, w13);
		a.B(ne, &done);
		a.Cbz(w10, &ready);
		a.Mov(x14, 0);
		a.Bind(&loop);
		a.Mov(w12, sizeof(fmacPipe));
		a.Madd(x12, x11, x12, x19);
		a.Add(x12, x12, offsetof(VURegs, fmac));
		a.Ldr(x13, MemOperand(x12, offsetof(fmacPipe, sCycle)));
		a.Cmp(x13, x9);
		a.B(hi, &done);
		// At most one entry is issued per cycle. This also prevents a queue
		// overflow in the generic prefix before incoming entries have matured.
		a.Cmp(x13, x14);
		a.B(lo, &done);
		a.Add(x14, x13, 1);
		a.Ldr(w13, MemOperand(x12, offsetof(fmacPipe, Cycle)));
		a.Cmp(w13, 4);
		a.B(hi, &done);
		a.Add(w11, w11, 1);
		a.And(w11, w11, 3);
		a.Subs(w10, w10, 1);
		a.B(ne, &loop);
		a.Bind(&ready);
		a.Mov(w25, 1);
		EmitScheduleReadiness(a);
		a.Bind(&done);
	}

	void EmitBackupCountdown(MacroAssembler& a, u32 cycles)
	{
		a.Ldrb(w9, Field(offsetof(VURegs, VIBackupCycles)));
		if (cycles == 1)
		{
			a.Cmp(w9, 0);
			a.Cset(w10, ne);
		}
		else
		{
			a.Mov(w10, cycles);
			a.Cmp(w9, w10);
			a.Csel(w10, w9, w10, lo);
		}
		a.Sub(w9, w9, w10);
		a.Strb(w9, Field(offsetof(VURegs, VIBackupCycles)));
	}

	// Retire the FDIV slot at cycle x26 if it is due. Cheap when nothing is
	// due, which is the common case even inside a divide's latency. `status`
	// holds the status flag; it is loaded and stored around the merge unless
	// the caller keeps it in that register (deferred regions keep it in w25).
	// Retires the EFU slot into P: when due at x26, or (`stall`) whenever it is
	// enabled, for the EFU op or WAITP that stalls on it; `known` skips the
	// checks for a slot known to be due.
	void EmitEFUSlotRetire(MacroAssembler& a, bool stall, bool known)
	{
		Label end;
		constexpr size_t offset = offsetof(VURegs, efu);
		if (!known)
		{
			a.Ldr(w11, Field(offset + offsetof(efuPipe, enable)));
			a.Cbz(w11, &end);
			if (!stall)
			{
				a.Ldr(x11, Field(offset + offsetof(efuPipe, sCycle)));
				a.Ldr(w12, Field(offset + offsetof(efuPipe, Cycle)));
				a.Sub(x11, x26, x11);
				a.Cmp(x11, x12);
				a.B(lo, &end);
			}
		}
		a.Str(wzr, Field(offset + offsetof(efuPipe, enable)));
		a.Ldr(w11, Field(offset + offsetof(efuPipe, reg)));
		a.Str(w11, Field(VI(REG_P)));
		a.Bind(&end);
	}

	// Retires the FDIV slot, known to be due: Q and the status flag's D/I bits.
	void EmitFDIVRetire(MacroAssembler& a, const Register& status, bool in_memory)
	{
		constexpr size_t offset = offsetof(VURegs, fdiv);
		a.Str(wzr, Field(offset + offsetof(fdivPipe, enable)));
		a.Ldr(w11, Field(offset + offsetof(fdivPipe, reg)));
		a.Str(w11, Field(VI(REG_Q)));
		if (in_memory)
			a.Ldr(status, Field(VI(REG_STATUS_FLAG)));
		a.And(status, status, 0xfcf);
		a.Ldr(w11, Field(offset + offsetof(fdivPipe, statusflag)));
		a.And(w11, w11, 0xc30);
		a.Orr(status, status, w11);
		if (in_memory)
			a.Str(status, Field(VI(REG_STATUS_FLAG)));
	}

	void EmitFDIVSlotRetire(MacroAssembler& a, const Register& status, bool in_memory)
	{
		Label end;
		constexpr size_t offset = offsetof(VURegs, fdiv);
		a.Ldr(w11, Field(offset + offsetof(fdivPipe, enable)));
		a.Cbz(w11, &end);
		a.Ldr(x11, Field(offset + offsetof(fdivPipe, sCycle)));
		a.Ldr(w12, Field(offset + offsetof(fdivPipe, Cycle)));
		a.Sub(x11, x26, x11);
		a.Cmp(x11, x12);
		a.B(lo, &end);
		a.Str(wzr, Field(offset + offsetof(fdivPipe, enable)));
		a.Ldr(w11, Field(offset + offsetof(fdivPipe, reg)));
		a.Str(w11, Field(VI(REG_Q)));
		if (in_memory)
			a.Ldr(status, Field(VI(REG_STATUS_FLAG)));
		a.And(status, status, 0xfcf);
		a.Ldr(w11, Field(offset + offsetof(fdivPipe, statusflag)));
		a.And(w11, w11, 0xc30);
		a.Orr(status, status, w11);
		if (in_memory)
			a.Str(status, Field(VI(REG_STATUS_FLAG)));
		a.Bind(&end);
	}

	// Drop the IALU entries that are due at cycle x26, like VUPipeline::FlushIALU.
	void EmitIALURetire(MacroAssembler& a)
	{
		Label loop, store, end;
		a.Ldr(w11, Field(offsetof(VURegs, ialucount)));
		a.Cbz(w11, &end);
		a.Ldr(w12, Field(offsetof(VURegs, ialureadpos)));
		a.Bind(&loop);
		a.Mov(w13, sizeof(ialuPipe));
		a.Madd(x13, x12, x13, x19);
		a.Add(x13, x13, offsetof(VURegs, ialu));
		a.Ldr(x14, MemOperand(x13, offsetof(ialuPipe, sCycle)));
		a.Ldr(w13, MemOperand(x13, offsetof(ialuPipe, Cycle)));
		a.Sub(x14, x26, x14);
		a.Cmp(x14, x13);
		a.B(lo, &store);
		a.Add(w12, w12, 1);
		a.And(w12, w12, 3);
		a.Subs(w11, w11, 1);
		a.B(ne, &loop);
		a.Bind(&store);
		a.Str(w12, Field(offsetof(VURegs, ialureadpos)));
		a.Str(w11, Field(offsetof(VURegs, ialucount)));
		a.Bind(&end);
	}

	void EmitScheduledPrepare(MacroAssembler& a, const Block& block, u32 index)
	{
		const auto& plan = block.schedule[index];
		a.Add(x26, x26, plan.cycles);
		StoreWord(a, block.instructions[index].pc + 8, VI(REG_TPC));
		if (plan.retired)
		{
			a.Ldr(w10, Field(offsetof(VURegs, fmacreadpos)));
			a.Ldr(w13, Field(VI(REG_STATUS_FLAG)));
			a.And(w13, w13, 0xff0);
			for (u32 i = 0; i < plan.retired; i++)
			{
				a.Mov(w11, sizeof(fmacPipe));
				a.Madd(x12, x10, x11, x19);
				a.Add(x12, x12, offsetof(VURegs, fmac));
				if (plan.clip_retires & (1 << i))
				{
					a.Ldr(w11, MemOperand(x12, offsetof(fmacPipe, clipflag)));
					a.Str(w11, Field(VI(REG_CLIP_FLAG)));
				}
				a.Ldr(w11, MemOperand(x12, offsetof(fmacPipe, statusflag)));
				a.And(w11, w11, 15);
				// All sticky bits survive, but only the last MAC/non-sticky flags
				// are observable after retirement. No flag readers run between slots.
				a.Orr(w13, w13, Operand(w11, LSL, 6));
				if (i + 1 == plan.retired)
				{
					a.Orr(w13, w13, w11);
					a.Str(w13, Field(VI(REG_STATUS_FLAG)));
					a.Ldr(w11, MemOperand(x12, offsetof(fmacPipe, macflag)));
					a.Str(w11, Field(VI(REG_MAC_FLAG)));
				}
				a.Add(w10, w10, 1);
				a.And(w10, w10, 3);
			}
			a.Str(w10, Field(offsetof(VURegs, fmacreadpos)));
			StoreWord(a, plan.remaining, offsetof(VURegs, fmaccount));
		}
		// The FDIV slot the generic preparation would have drained, in
		// VUPipeline::Retire's order: after the FMAC writeback above, because
		// both merge into VI[REG_STATUS_FLAG].
		if (plan.fdiv_pending)
			EmitFDIVSlotRetire(a, w10, true);
		if (plan.efu_pending)
			EmitEFUSlotRetire(a, block.instructions[index].lregs.pipe == VUPIPE_EFU, false);
		if (plan.ialu_pending)
			EmitIALURetire(a);
		// Needs proper testing across more games.
		EmitBackupCountdown(a, plan.cycles);
	}

	// A fully budgeted, callback-free region can keep its four FMAC flag
	// snapshots in q28..q31. Queue metadata and cycle stamps are compile-time
	// facts and only need materializing when returning to the dispatcher.
	// The FMAC slots a deferred region has written: which pair issued each and
	// when, relative to the region's start.
	struct RegionSlots
	{
		struct Slot
		{
			int writer = -1;
			u32 issue_cycle = 0;
			u32 order = 0;
			// Whether the slot register holds the entry's flags (Entry::Normal).
			bool exact = true;
		};
		std::array<Slot, 4> slots{};
		u32 issued = 0, elapsed = 0, backup_cycles = 0;
		// Cycles not yet added to x26, which only divides, IALU retirement and
		// exits read inside a region.
		u32 pending_cycles = 0;
		StaticViBackup vi;
		// Whether v25 may hold sticky status bits not yet in w25.
		bool raw_retired = false;
	};

	void FlushCycles(MacroAssembler& a, RegionSlots& state)
	{
		if (state.pending_cycles)
			a.Add(x26, x26, state.pending_cycles);
		state.pending_cycles = 0;
	}

	// Fold the sticky bits of retired unobserved entries (v25) into w25.
	void EmitStickyFold(MacroAssembler& a, const RegionSlots& state)
	{
		if (!state.raw_retired)
			return;
		// Each lane's MAC bits are disjoint, so the sum is their OR.
		a.Addv(vixl::aarch64::s16, v25.V4S());
		a.Fmov(w10, vixl::aarch64::s16);
		EmitMacToStatus(a, w11, w10);
		a.Orr(w25, w25, Operand(w11, LSL, 6));
	}

	// x12 = &VU1.fmac[0] and w13 = the slot size, for EmitRegionSlotAddress.
	void EmitRegionSlotBase(MacroAssembler& a)
	{
		a.Add(x12, x19, offsetof(VURegs, fmac));
		a.Mov(w13, sizeof(fmacPipe));
	}

	// x0 = the FMAC slot `relative` places after w27 (always 0..3).
	void EmitRegionSlotAddress(MacroAssembler& a, u32 relative)
	{
		if (relative & 3)
		{
			a.Add(w0, w27, relative & 3);
			a.And(w0, w0, 3);
			a.Umaddl(x0, w0, w13, x12);
		}
		else
			a.Umaddl(x0, w27, w13, x12);
	}

	// Publish what a deferred region keeps in registers, as of the end of pair
	// `last`: the live FMAC entries, queue positions, status/MAC flags, the
	// batched backup countdown and TPC.
	void EmitRegionExit(MacroAssembler& a, const Block& block, RegionSlots state, u32 last_index)
	{
		FlushCycles(a, state);
		if (state.vi.known)
		{
			a.Mov(w9, state.vi.left);
			a.Strb(w9, Field(offsetof(VURegs, VIBackupCycles)));
			if (state.vi.reg_dirty)
				StoreWord(a, state.vi.reg, offsetof(VURegs, VIRegNumber));
		}
		else if (state.backup_cycles)
			EmitBackupCountdown(a, std::min(state.backup_cycles, 255u));
		EmitStickyFold(a, state);
		const auto& last = block.instructions[last_index];
		const u32 live = block.schedule[last_index].remaining + HasFmac(last);
		// Only live entries are ever read again (queue walks start at fmacreadpos
		// and cover fmaccount entries). Retired slots keep stale contents.
		bool base = false;
		for (u32 slot = 0; slot < 4; slot++)
		{
			const auto& entry = state.slots[slot];
			if (entry.writer < 0 || entry.order + live < state.issued)
				continue;
			if (!base)
			{
				EmitRegionSlotBase(a);
				a.Movi(v17.V4S(), 4);
				base = true;
			}
			EmitRegionSlotAddress(a, slot);
			// The same fields as EmitFmacMetadata, two words per store.
			const auto& ins = block.instructions[entry.writer];
			const bool upper = ins.uregs.pipe == VUPIPE_FMAC;
			const bool lower = ins.lregs.pipe == VUPIPE_FMAC;
			const u64 flags = (upper ? ins.uregs.VIwrite : 0) | (lower ? ins.lregs.VIwrite : 0);
			a.Mov(x9, (upper ? ins.uregs.VFwrite : 0) | (u64(lower ? ins.lregs.VFwrite : 0) << 32));
			a.Mov(x10, flags | (u64(upper ? ins.uregs.VFwxyzw : 0) << 32));
			a.Stp(x9, x10, MemOperand(x0, offsetof(fmacPipe, regupper)));
			a.Mov(x9, lower ? ins.lregs.VFwxyzw : 0); // also clears the padding
			a.Sub(x10, x26, state.elapsed - entry.issue_cycle);
			a.Stp(x9, x10, MemOperand(x0, offsetof(fmacPipe, xyzwlower)));
			// Cycle = 4, then the MAC/status/clip lanes of the slot register.
			if (entry.exact)
				a.Ext(v16.V16B(), v17.V16B(), VRegister(28 + slot, 128).V16B(), 12);
			else
			{
				// Only at an exit whose flags nothing reads (ExitFlagsObserved).
				// Zero flags keep whatever the slot register holds out of the
				// sticky bits when the entry retires.
				a.Mov(w9, 4);
				a.Fmov(vixl::aarch64::s16, w9);
			}
			a.Str(q16, MemOperand(x0, offsetof(fmacPipe, Cycle)));
		}
		a.Add(w9, w27, state.issued & 3);
		a.And(w9, w9, 3);
		a.Str(w9, Field(offsetof(VURegs, fmacwritepos)));
		a.Sub(w9, w9, live);
		a.And(w9, w9, 3);
		a.Str(w9, Field(offsetof(VURegs, fmacreadpos)));
		StoreWord(a, live, offsetof(VURegs, fmaccount));
		a.Str(w25, Field(VI(REG_STATUS_FLAG)));
		a.Str(w28, Field(VI(REG_MAC_FLAG)));
		// A JR/JALR/BAL delay slot ending the region already published the right
		// TPC itself (EmitControlFlow's block.delay case, run above in this same
		// loop): its target isn't the compile-time-constant next_pc this generic
		// epilogue otherwise stores, so don't clobber it with that stale value.
		const bool last_is_register_branch_delay = block.delay[last_index] && last_index >= 1 &&
		                                           !(block.instructions[last_index - 1].upper & 0x80000000) &&
		                                           IsRegisterBranch(DecodeLower(block.instructions[last_index - 1].lower));
		if (!last_is_register_branch_delay)
			StoreWord(a, block.next_pc[last_index], VI(REG_TPC));
		// VURegs::code is decoding scratch that nothing reads after this point:
		// generated code and the interpreter set it before using it.
	}

	// An untaken integer branch inside a deferred region leaves through a stub
	// the block emits out of line: `label` publishes the region's state as of
	// pair `index` and continues at the branch's link exit.
	struct RegionBranchExit
	{
		Label label;
		RegionSlots state;
		u32 index;
	};

	// Whether code starting at `pc` can observe the status/MAC flags a region
	// exit publishes before a newer flag instruction's entry replaces them.
	// `pc` may be a delay slot, followed by after[0] (and after[1], for a
	// conditional branch either way). Every pair takes at least a cycle, so the
	// entry of a flag instruction four pairs back has retired, after every older
	// one. Flag reads, the status scratch (which FSSET and divides read and the
	// latest flag instruction stores), E/D/T bits, register branches and running
	// out of `budget` pairs count as observed. Appends the pairs it read to
	// `guards`: the answer holds only while they stay the same.
	bool FlagsObserved(u32 pc, std::array<u32, 2> after, u32 after_count, int since, u32& budget,
		std::vector<std::pair<u32, u64>>& guards)
	{
		for (;;)
		{
			if (since >= 4)
				return false;
			if (!budget)
				return true;
			budget--;
			pc &= VU1_PROGMASK;
			u32 lower, upper;
			std::memcpy(&lower, VU1.Micro + pc, 4);
			std::memcpy(&upper, VU1.Micro + pc + 4, 4);
			guards.emplace_back(pc, lower | (u64(upper) << 32));
			if (upper & 0x58000000)
				return true;
			const bool immediate = upper & 0x80000000;
			_VURegsNum uregs{}, lregs{};
			const u32 saved_code = VU1.code;
			VU1.code = upper;
			VU1regs_UPPER_OPCODE[upper & 0x3f](&uregs);
			if (!immediate)
			{
				VU1.code = lower;
				VU1regs_LOWER_OPCODE[lower >> 25](&lregs);
			}
			VU1.code = saved_code;
			if ((uregs.VIread | lregs.VIread) & ((1 << REG_STATUS_FLAG) | (1 << REG_MAC_FLAG)))
				return true;
			if (since < 1 && (((uregs.VIwrite | lregs.VIwrite) & (1 << REG_STATUS_FLAG)) ||
								 (lregs.pipe == VUPIPE_FDIV && lregs.cycles)))
				return true;
			const Lower op = immediate ? Lower::Unsupported : DecodeLower(lower);
			if (IsRegisterBranch(op))
				return true;
			const bool branch = op == Lower::Branch || IsIntegerBranch(op);
			if (branch && after_count)
				return true;
			if (since < 0 && uregs.pipe == VUPIPE_FMAC && UpdatesMacFlags(upper))
				since = 0;
			if (since >= 0)
				since++;
			if (after_count)
			{
				if (after_count == 2 && FlagsObserved(after[1], {}, 0, since, budget, guards))
					return true;
				pc = after[0];
				after_count = 0;
				continue;
			}
			if (branch)
			{
				const s32 displacement = (static_cast<s32>(lower << 21) >> 21) * 8;
				after = {(pc + 8 + displacement) & VU1_PROGMASK, pc + 16};
				after_count = op == Lower::Branch ? 1 : 2;
			}
			pc += 8;
		}
	}

	// Under the VU flag hack, whether the exits after pair `i` of a deferred
	// region have to publish exact flags: its untaken integer branch exit
	// and/or the region's end. If not, the block keeps the pairs that decided
	// it as guards. Needs proper testing across more games.
	bool ExitFlagsObserved(Block& block, u32 i, bool untaken, bool region_end)
	{
		constexpr u32 LookaheadPairs = 64;
		const auto& ins = block.instructions[i];
		if (ins.upper & 0x40000000)
			return true;
		std::vector<std::pair<u32, u64>> guards;
		u32 budget = LookaheadPairs;
		if (untaken && FlagsObserved(ins.pc + 8, {}, 0, -1, budget, guards))
			return true;
		if (region_end)
		{
			const Lower op = (ins.upper & 0x80000000) ? Lower::Unsupported : DecodeLower(ins.lower);
			std::array<u32, 2> after{};
			u32 after_count = 0;
			u32 next = block.next_pc[i];
			if (block.delay[i])
			{
				const auto& branch = block.instructions[i - 1];
				if (!(branch.upper & 0x80000000) && IsRegisterBranch(DecodeLower(branch.lower)))
					return true;
			}
			else if (IsRegisterBranch(op))
				return true;
			else if (op == Lower::Branch || IsIntegerBranch(op))
			{
				// The trace follows the taken edge; with no delay pair in the
				// block, both edges leave through this exit.
				const s32 displacement = (static_cast<s32>(ins.lower << 21) >> 21) * 8;
				after = {(ins.pc + 8 + displacement) & VU1_PROGMASK, ins.pc + 16};
				after_count = op == Lower::Branch || i + 1 < block.count ? 1 : 2;
			}
			budget = LookaheadPairs;
			if (FlagsObserved(next, after, after_count, -1, budget, guards))
				return true;
		}
		for (const auto& [pc, words] : guards)
		{
			auto& ranges = block.guard_ranges;
			if (!ranges.empty() && ranges.back().pc + ranges.back().count * 8 == pc)
				ranges.back().count++;
			else
				ranges.push_back({pc, static_cast<u32>(block.guard_words.size() / 2), 1});
			block.guard_words.push_back(static_cast<u32>(words));
			block.guard_words.push_back(static_cast<u32>(words >> 32));
		}
		return false;
	}

	void EmitDeferredRegion(MacroAssembler& a, Block& block, u32 first, u32 end,
		std::vector<std::unique_ptr<RegionBranchExit>>& branch_exits)
	{
		static_assert(offsetof(VURegs, statusflag) == offsetof(VURegs, macflag) + 4 &&
					  offsetof(VURegs, clipflag) == offsetof(VURegs, macflag) + 8);
		s_clamped_copies.Forget();
		a.Ldr(w27, Field(offsetof(VURegs, fmacwritepos)));
		a.Ldr(w25, Field(VI(REG_STATUS_FLAG)));
		a.Ldr(w28, Field(VI(REG_MAC_FLAG)));
		// Only the entries in flight at the region's first pair are retired from
		// these registers; the region's own entries overwrite their slots. Lane 3
		// (the next slot's first word) is never read for them.
		const u32 incoming = block.schedule[first].retired + block.schedule[first].remaining;
		if (incoming)
			EmitRegionSlotBase(a);
		for (u32 k = 1; k <= incoming; k++)
		{
			const u32 slot = (4 - k) & 3;
			EmitRegionSlotAddress(a, slot);
			a.Ldr(VRegister(28 + slot, 128), MemOperand(x0, offsetof(fmacPipe, macflag)));
		}
		// Which FMAC entries issued here can be observed. The status/MAC flags
		// and the flag scratch only become visible through flag readers and
		// region exits: the latest retired entry sets w25's low bits and w28,
		// live entries are written back, and the scratch fields are published.
		// An entry nothing observes only contributes its sticky status bits.
		// A flag instruction whose values nothing observes skips the scalar MAC/
		// status computation; its entry carries the per-lane MAC bits (Raw) and
		// ORs them into v25 when it retires. Other unobserved entries (Skip)
		// repeat the scratch of an earlier flag instruction, whose own entry
		// retires first and already contributed the same sticky bits.
		// Under the VU flag hack, a flag instruction that would be Raw computes no
		// flags at all (Dead) unless a status reader later in the region can see
		// its sticky bits. Sticky bits only reach the region's exit through the
		// entries that computed them, like microVU's. It also skips an exit's
		// flags when nothing after it reads them first (ExitFlagsObserved); live
		// entries whose sticky bits a status reader in the region still sees
		// keep them.
		// Needs proper testing across more games.
		enum class Entry : u8
		{
			Normal,
			Raw,
			Skip,
			Dead,
		};
		std::array<Entry, MaxInstructions> kinds{};
		std::array<bool, MaxInstructions> div_clean{};
		{
			std::array<int, MaxInstructions> source{};
			std::array<bool, MaxInstructions> flag_op{}, needed{}, scratch_full{};
			int scratch = -1, latest = -1;
			// Whether a flag instruction wrote the scratch after any FSSET.
			bool scratch_clean = false;
			u32 count = 0, sticky_seen = 0;
			const bool flag_hack = EmuConfig.Speedhacks.vuFlagHack;
			// [first, count) entries live at exits whose flags nothing reads.
			std::vector<std::pair<u32, u32>> relaxed_live;
			const auto observe_latest = [&]() {
				if (latest >= 0)
					needed[latest] = true;
			};
			for (u32 i = first; i < end; i++)
			{
				const auto& plan = block.schedule[i];
				const auto& ins = block.instructions[i];
				for (u32 j = 0; j < plan.retired; j++)
					latest = static_cast<int>(count - plan.remaining - plan.retired + j);
				if ((ins.uregs.VIread | ins.lregs.VIread) & ((1 << REG_STATUS_FLAG) | (1 << REG_MAC_FLAG)))
					observe_latest();
				if ((ins.uregs.VIread | ins.lregs.VIread) & (1 << REG_STATUS_FLAG))
					sticky_seen = count;
				if (HasFmac(ins))
				{
					flag_op[count] = UpdatesMacFlags(ins.upper);
					if (flag_op[count])
					{
						scratch = static_cast<int>(count);
						scratch_clean = true;
					}
					source[count] = scratch;
					// CLIP/FCSET retire the clip flag, FSSET reads the scratch.
					if ((ins.uregs.VIwrite | ins.lregs.VIwrite) & ((1 << REG_CLIP_FLAG) | (1 << REG_STATUS_FLAG)))
						needed[count] = true;
					count++;
				}
				if (!(ins.upper & 0x80000000) && DecodeLower(ins.lower) == Lower::Fsset)
					scratch_clean = false;
				// A divide passes on the scratch's sticky D/I bits and keeps the
				// rest. After a flag instruction in the region those bits are
				// clear, so it needs nothing from that instruction; otherwise the
				// latest one has to store the scratch.
				if (ins.lregs.pipe == VUPIPE_FDIV && ins.lregs.cycles && scratch >= 0)
				{
					if (scratch_clean)
						div_clean[i] = true;
					else
						scratch_full[scratch] = true;
				}
				const bool integer_branch = !(ins.upper & 0x80000000) && IsIntegerBranch(DecodeLower(ins.lower));
				const bool untaken = integer_branch && i + 1 < block.count;
				if (untaken || i + 1 == end)
				{
					const u32 live = plan.remaining + HasFmac(ins);
					if (flag_hack && !ExitFlagsObserved(block, i, untaken, i + 1 == end))
						relaxed_live.emplace_back(count >= live ? count - live : 0, count);
					else
					{
						observe_latest();
						for (u32 k = count >= live ? count - live : 0; k < count; k++)
							needed[k] = true;
						if (scratch >= 0)
							scratch_full[scratch] = true;
					}
				}
			}
			// A Raw entry retiring after the exit would take its sticky bits along.
			for (const auto& [from, to] : relaxed_live)
				for (u32 k = from; k < to; k++)
					if (flag_op[k] && k < sticky_seen)
						needed[k] = true;
			for (u32 k = 0; k < count; k++)
				if (needed[k] && source[k] >= 0)
					scratch_full[source[k]] = true;
			for (u32 k = 0; k < count; k++)
				kinds[k] = flag_op[k] ? (scratch_full[k] ? Entry::Normal : (flag_hack && k >= sticky_seen ? Entry::Dead : Entry::Raw)) :
				                        (needed[k] ? Entry::Normal : Entry::Skip);
		}
		a.Movi(v25.V4S(), 0);
		RegionSlots state;
		auto& slots = state.slots;
		u32& issued = state.issued;
		u32& elapsed = state.elapsed;
		u32& backup_cycles = state.backup_cycles;
		for (u32 i = first; i < end; i++)
		{
			const auto& plan = block.schedule[i];
			const auto& ins = block.instructions[i];
			const bool integer_branch = !(ins.upper & 0x80000000) && IsIntegerBranch(DecodeLower(ins.lower));
			elapsed += plan.cycles;
			state.pending_cycles += plan.cycles;
			int mac_from = -1;
			for (u32 j = 0; j < plan.retired; j++)
			{
				const int index = static_cast<int>(issued - plan.remaining - plan.retired + j);
				const Entry kind = index >= 0 ? kinds[index] : Entry::Normal;
				const u32 slot = static_cast<u32>(index) & 3;
				const VRegister flags(28 + slot, 128);
				if (kind == Entry::Raw)
				{
					a.Orr(v25.V16B(), v25.V16B(), flags.V16B());
					state.raw_retired = true;
					continue;
				}
				if (kind == Entry::Skip || kind == Entry::Dead)
					continue;
				if (plan.clip_retires & (1 << j))
				{
					a.Umov(w9, flags.V4S(), 2);
					a.Str(w9, Field(VI(REG_CLIP_FLAG)));
				}
				a.Umov(w9, flags.V4S(), 1);
				a.And(w9, w9, 15);
				a.And(w25, w25, 0xff0);
				a.Orr(w25, w25, w9);
				a.Orr(w25, w25, Operand(w9, LSL, 6));
				mac_from = static_cast<int>(slot);
			}
			if (mac_from >= 0)
				a.Umov(w28, VRegister(28 + mac_from, 128).V4S(), 0);
			const bool fdiv_check = plan.fdiv_pending && plan.fdiv_static == FdivUnknown;
			const bool efu_stall = ins.lregs.pipe == VUPIPE_EFU;
			const bool efu_check = plan.efu_pending && plan.efu_static == FdivUnknown && !efu_stall;
			if (fdiv_check || efu_check || plan.ialu_pending)
				FlushCycles(a, state);
			if (fdiv_check)
				EmitFDIVSlotRetire(a, w25, false);
			else if (plan.fdiv_pending && plan.fdiv_static == FdivRetires)
				EmitFDIVRetire(a, w25, false);
			if (plan.efu_pending && plan.efu_static != FdivBusy)
				EmitEFUSlotRetire(a, efu_stall, plan.efu_static == FdivRetires);
			if (plan.ialu_pending)
				EmitIALURetire(a);
			// Only an integer write or branch can inspect/reset the backup
			// countdown in a supported pair. Until the state is static, accumulate
			// time until that observer or the block exit.
			if (state.vi.known)
				state.vi.left -= std::min(state.vi.left, static_cast<u32>(plan.cycles));
			else
			{
				backup_cycles += plan.cycles;
				if ((ins.lregs.VIwrite & 0xffff) || integer_branch)
				{
					EmitBackupCountdown(a, std::min(backup_cycles, 255u));
					backup_cycles = 0;
				}
			}
			// Q/P are final in memory here, like the clip flag; status and MAC
			// live in w25/w28.
			const u32 reads = ins.uregs.VIread | ins.lregs.VIread;
			if (reads & (1 << REG_STATUS_FLAG))
			{
				EmitStickyFold(a, state);
				a.Str(w25, Field(VI(REG_STATUS_FLAG)));
			}
			if (reads & (1 << REG_MAC_FLAG))
				a.Str(w28, Field(VI(REG_MAC_FLAG)));
			const Entry kind = HasFmac(ins) ? kinds[issued] : Entry::Normal;
			const u32 macs = s_store_mac_count;
			s_raw_flag_slot = kind == Entry::Raw ? static_cast<int>(issued & 3) : -1;
			s_discard_flags = kind == Entry::Dead;
			s_vi_backup = &state.vi;
			s_clamp_skip = block.clamp_skip[i];
			// Divides (and WAITQ) stall on and record the cycle.
			if (ins.lregs.pipe != VUPIPE_FMAC && ins.lregs.pipe != VUPIPE_NONE && ins.lregs.pipe != VUPIPE_BRANCH)
				FlushCycles(a, state);
			s_fdiv_idle = plan.fdiv_static == FdivIdle || plan.fdiv_static == FdivRetires;
			// An EFU op or WAITP retired the slot above.
			s_efu_idle = true;
			s_div_clean_scratch = div_clean[i];
			// Clamped copies survive only plain FMAC/IALU/load-store pairs.
			const bool plain = (ins.upper & 0x80000000) ||
			                   ((ins.lregs.pipe == VUPIPE_NONE || ins.lregs.pipe == VUPIPE_FMAC || ins.lregs.pipe == VUPIPE_IALU) &&
								   DecodeLower(ins.lower) != Lower::Unsupported);
			if (!plain || DecodeUpper(ins.upper).op == Op::Unsupported)
				s_clamped_copies.Forget();
			s_clamped_copies.enabled = plain && DecodeUpper(ins.upper).op != Op::Unsupported;
			EmitPair(a, block.cache, ins, false);
			if (!s_clamped_copies.enabled)
				s_clamped_copies.Forget();
			s_clamped_copies.enabled = false;
			s_div_clean_scratch = false;
			s_fdiv_idle = false;
			s_efu_idle = false;
			s_clamp_skip = 0;
			s_raw_flag_slot = -1;
			s_discard_flags = false;
			pxAssertRel((s_store_mac_count != macs) == (HasFmac(ins) && UpdatesMacFlags(ins.upper)),
				"UpdatesMacFlags() disagrees with EmitUpper");
			EmitControlFlow(a, block, i);
			s_vi_backup = nullptr;
			// ILW/ILWR: the IALU entry goes to memory as on the generic path; the
			// cycles were flushed above. Later pairs retire it (ialu_pending).
			EmitIntegerIssue(a, ins);
			if (HasFmac(ins))
			{
				const u32 slot = issued++ & 3;
				// Only the first three lanes are used; the fourth is ignored.
				if (kind == Entry::Normal)
					a.Ldr(VRegister(28 + slot, 128), Field(offsetof(VURegs, macflag)));
				slots[slot].writer = i;
				slots[slot].issue_cycle = elapsed;
				slots[slot].order = issued - 1;
				slots[slot].exact = kind == Entry::Normal;
			}
			if (integer_branch && i + 1 < block.count)
			{
				// Not taken: the trace continues elsewhere.
				auto exit = std::make_unique<RegionBranchExit>();
				exit->state = state;
				exit->index = i;
				a.Ldr(w9, Field(offsetof(VURegs, branch)));
				a.Cbz(w9, &exit->label);
				branch_exits.push_back(std::move(exit));
			}
		}
		EmitRegionExit(a, block, state, end - 1);
	}

	Block& Compile(u32 pc, const IncomingProfile* profile)
	{
		if (static_cast<size_t>(s_end - s_write) < MaxBlockBytes)
			InvalidateAll();
		auto block = std::make_unique<Block>();
		const u32 saved_code = VU1.code;
		// Decode static B and taken integer-branch edges with one vector-cache and
		// pipeline schedule. Reachable source bytes are validated before entry.
		// A repeated entry can loop natively; other repeated PCs, unsupported
		// pairs and the size limit end the trace.
		u32 next_pc = pc, branch_target = 0;
		bool pending_branch = false;
		// JR/JALR/BAL: EmitControlFlow resolves branchpc itself (from the register
		// at runtime for JR/JALR) instead of relying on a precomputed
		// branch_target/next_pc. The trace follows BAL to its static target, and
		// JR/JALR when the register still holds a return address a BAL/JALR in
		// this trace linked, so subroutine calls and returns stay in one block.
		// Otherwise it ends once their delay slot has been emitted.
		bool pending_branch_terminal = false;
		// VI[0..15] values the trace linked itself and has not overwritten since
		// (in pairs), or -1. Neither JR nor JALR bypasses the VI backup.
		std::array<int, 16> linked;
		linked.fill(-1);
		std::array<bool, VU1_PROGSIZE / 8> visited{};
		for (u32 i = 0; i < MaxInstructions && next_pc < VU1_PROGSIZE; i++)
		{
			auto& ins = block->instructions[i];
			if (visited[next_pc / 8])
			{
				// Reenter only this trace's incoming-state path, with no unresolved
				// delay. Other internal targets need their own state contract.
				block->loops_to_entry = next_pc == pc && !pending_branch;
				break;
			}
			ins.pc = next_pc;
			std::memcpy(&ins.lower, VU1.Micro + ins.pc, 4);
			std::memcpy(&ins.upper, VU1.Micro + ins.pc + 4, 4);
			block->words[i * 2] = ins.lower;
			block->words[i * 2 + 1] = ins.upper;
			// What the preparation stubs publish, so they need neither the +8 nor
			// the upper/lower select at runtime. The interpreter leaves whichever
			// half it decoded last in VURegs::code, and it skips the lower half of
			// an E-bit pair.
			ins.tpc = ins.pc + 8;
			ins.code = (ins.upper & 0x80000000) ? ins.upper : ins.lower;
			if (DecodeUpper(ins.upper).op == Op::Unsupported ||
				(!(ins.upper & 0x80000000) && DecodeLower(ins.lower) == Lower::Unsupported))
				break;
			const bool conditional = !(ins.upper & 0x80000000) && IsIntegerBranch(DecodeLower(ins.lower));
			const bool ebit = ins.upper & 0x40000000;
			// The interpreter retires an E pair in a delay slot, or one that
			// branches itself, together with that branch.
			if (ebit && (pending_branch || conditional || (!(ins.upper & 0x80000000) &&
				(DecodeLower(ins.lower) == Lower::Branch || IsRegisterBranch(DecodeLower(ins.lower))))))
				break;
			// Follow the taken edge; the generated guard exits on the other path.
			if (conditional && pending_branch)
				break;
			const bool branch = !(ins.upper & 0x80000000) && DecodeLower(ins.lower) == Lower::Branch;
			if (branch && (pending_branch || i + 1 == MaxInstructions))
				break;
			const bool terminal_branch = !(ins.upper & 0x80000000) && IsRegisterBranch(DecodeLower(ins.lower));
			// A register/link branch found in a pending delay slot (branch-in-branch)
			// isn't natively resolved either, same as the nested-conditional case above.
			if (terminal_branch && (pending_branch || i + 1 == MaxInstructions))
				break;
			visited[ins.pc / 8] = true;
			block->delay[i] = pending_branch;
			const bool finishing_terminal_delay = pending_branch && pending_branch_terminal;
			// branch_target is meaningless for a terminal delay slot; EmitControlFlow
			// recognizes that case from the preceding instruction and ignores it.
			block->next_pc[i] = pending_branch ? branch_target : ins.pc + 8;
			if (pending_branch)
			{
				pending_branch = false;
				pending_branch_terminal = false;
			}
			if (branch || conditional)
			{
				const s32 displacement = (static_cast<s32>(ins.lower << 21) >> 21) * 8;
				branch_target = (ins.pc + 8 + displacement) & VU1_PROGMASK;
				pending_branch = true;
				block->has_branches = true;
			}
			else if (terminal_branch)
			{
				const Lower op = DecodeLower(ins.lower);
				const u32 is_reg = (ins.lower >> 11) & 15;
				int target = -1;
				if (op == Lower::Bal)
				{
					const s32 displacement = (static_cast<s32>(ins.lower << 21) >> 21) * 8;
					target = static_cast<int>((ins.pc + 8 + displacement) & VU1_PROGMASK);
				}
				else if (linked[is_reg] >= 0)
					target = static_cast<int>((static_cast<u32>(linked[is_reg]) * 8) & VU1_PROGMASK);
				pending_branch = true;
				pending_branch_terminal = target < 0;
				if (target >= 0)
					branch_target = static_cast<u32>(target);
				block->has_branches = true;
			}
			next_pc = block->next_pc[i];
			if (pending_branch)
				next_pc &= VU1_PROGMASK; // A delay pair may wrap micro memory.
			if (!block->ranges.empty() && block->ranges.back().pc + block->ranges.back().count * 8 == ins.pc)
				block->ranges.back().count++;
			else
				block->ranges.push_back({ins.pc, i, 1});
			VU1.code = ins.upper;
			VU1regs_UPPER_OPCODE[ins.upper & 0x3f](&ins.uregs);
			if (!(ins.upper & 0x80000000))
			{
				VU1.code = ins.lower;
				VU1regs_LOWER_OPCODE[ins.lower >> 25](&ins.lregs);
				// _vuRegsWAITQ (shared with the interpreter/x86) declares no
				// reads or writes at all, since its only effect is forcing the
				// pending FDIV entry to retire early. Without a VIwrite(Q) of
				// its own, nothing below treats it differently from a plain
				// 1-cycle op, so it can be scheduled or swept into a deferred
				// region as if its timing were fixed and it touched no
				// architectural state — neither is true. Tag it exactly like
				// DIV/SQRT/RSQRT so the existing Q/STATUS/MAC/CLIP exclusion
				// keeps it on the generic (non-deferred) path.
				if (DecodeLower(ins.lower) == Lower::Waitq)
					ins.lregs.VIwrite |= 1 << REG_Q;
				// _vuRegsWAITP has the same gap as _vuRegsWAITQ above, for P
				// instead of Q.
				if (DecodeLower(ins.lower) == Lower::Waitp)
					ins.lregs.VIwrite |= 1 << REG_P;
			}
			for (const _VURegsNum* regs : {&ins.uregs, &ins.lregs})
			{
				// FDIV (DIV/SQRT/RSQRT) and EFU (ESADD..WAITP) reads are also
				// FMAC-hazard checked by the interpreter's _vuTestLowerStalls
				// (_vuTestEFUStalls itself starts by calling _vuTestFMACStalls),
				// same as ordinary FMAC consumers.
				if (regs->pipe != VUPIPE_FMAC && regs->pipe != VUPIPE_FDIV && regs->pipe != VUPIPE_EFU)
					continue;
				if (regs->VFread0)
					ins.readMasks[regs->VFread0] |= regs->VFr0xyzw;
				if (regs->VFread1)
					ins.readMasks[regs->VFread1] |= regs->VFr1xyzw;
			}
			ins.readsVF = std::any_of(ins.readMasks.begin(), ins.readMasks.end(), [](u8 mask) { return mask != 0; });
			if (!(ins.upper & 0x80000000))
			{
				for (u32 reg = 1; reg < 16; reg++)
					if (ins.lregs.VIwrite & (1 << reg))
						linked[reg] = -1;
				const Lower op = DecodeLower(ins.lower);
				const u32 it_reg = (ins.lower >> 16) & 15;
				if ((op == Lower::Bal || op == Lower::Jalr) && it_reg)
					linked[it_reg] = static_cast<int>((ins.pc + 16) / 8);
			}
			if (i >= 3)
			{
				// Four cycles have elapsed since block entry, so incoming FMAC results
				// are ready. Only the preceding three pairs can still cause a stall.
				// All supported FMAC results have the same four-cycle latency: the
				// newest conflicting producer determines the wait for the whole pair.
				ins.dependency = 0;
				int8_t slots = 0;
				for (u32 distance = 1; distance <= 3; distance++)
				{
					const auto& previous = block->instructions[i - distance];
					if (previous.uregs.pipe != VUPIPE_FMAC && previous.lregs.pipe != VUPIPE_FMAC)
						continue;
					slots++;
					if ((previous.uregs.pipe == VUPIPE_FMAC && (ins.readMasks[previous.uregs.VFwrite] & previous.uregs.VFwxyzw)) ||
						(previous.lregs.pipe == VUPIPE_FMAC && (ins.readMasks[previous.lregs.VFwrite] & previous.lregs.VFwxyzw)))
					{
						ins.dependency = slots;
						break;
					}
				}
			}
			block->count++;
			if (finishing_terminal_delay)
				break; // JR/JALR/BAL's delay slot is native; the runtime target is not.
			if (ebit)
				break; // Execute() sees VU1.ebit and steps the delay slot and program end.
		}
		VU1.code = saved_code;
		if (block->count)
		{
			if (profile)
			{
				// A profiled loop leaves through its end exit and links back to a
				// variant for the state the loop edge arrives with. That needs the
				// edge's state to be the same every time; otherwise keep the native
				// loop and the generic prologue.
				block->profiled = true;
				block->incoming = *profile;
				const bool loops = block->loops_to_entry;
				block->loops_to_entry = false;
				AnalyzeRetirement(*block);
				if (loops && block->known_prefix != block->count)
				{
					block->profiled = false;
					block->profile_rejected = true;
					block->incoming = {};
					block->loops_to_entry = true;
					AnalyzeRetirement(*block);
				}
			}
			else
				AnalyzeRetirement(*block);
			AssignVectorCache(*block);
			AnalyzeClamps(*block);
			const VectorCache& cache = block->cache;
			if (!s_pipeline.prepare[0])
			{
				HostSys::BeginCodeWrite();
				s_pipeline = Arm64VU1::CompilePipeline(s_write, s_end - s_write, &_vuXGKICKTransfer, (s_options & 16) != 0);
				HostSys::EndCodeWrite();
				HostSys::FlushInstructionCache(s_write, static_cast<u32>(s_pipeline.size));
				s_write += (s_pipeline.size + 15) & ~size_t(15);
			}
			HostSys::BeginCodeWrite();
			MacroAssembler a(s_write, s_end - s_write);
			// Defer each sufficiently long known region separately. Conditional
			// exits must see materialized queues, without penalizing later regions.
			struct DeferredRegion
			{
				u32 first, end, cycles;
			};
			std::vector<DeferredRegion> regions;
			// A pair inside a divide's latency retires the FDIV slot into the
			// status flag a deferred region keeps in w25.
			const auto deferrable = [&](u32 i) { return block->schedule[i].cycles != 0; };
			// Unprofiled blocks never schedule their first seven pairs.
			const u32 first_scheduled = block->profiled ? 0 : 7;
			for (u32 i = first_scheduled; i < block->count;)
			{
				if (!deferrable(i))
				{
					i++;
					continue;
				}
				const u32 first = i;
				u32 cycles = 0;
				while (i < block->count && deferrable(i))
					cycles += block->schedule[i++].cycles;
				// Entering and leaving a region costs about as much as two pairs
				// of queue maintenance save.
				if (i - first >= 4)
					regions.push_back({first, i, cycles});
			}
			const bool deferred = !regions.empty();
			std::array<Label, MaxInstructions> deferred_entries, resumes;
			Label exit, loop_entry, finished, linked_entry, entry, common_entry, link_exit, link_request, restore;
			// Every block saves the same registers in the same frame, so a linked
			// exit can enter the next block in it and that block's exit returns
			// straight to Execute(). One slot per integer-branch exit plus the end.
			constexpr int saved_size = 96;
			constexpr int frame_size = saved_size + 64;
			block->links.resize(block->count + 1);
			u32 link_count = 0;
			std::array<Label, MaxInstructions + 1> link_sites;
			const auto add_link = [&](u32 /*pair*/) {
				// A profiled block is entered only in the state it was compiled for,
				// and everything after that follows from it: packet XGKICKs charge no
				// VU cycles, and the divide and IALU pipes are part of the profile.
				// known_prefix only bounds what the schedule can compute. Budget,
				// branch and pending-packet exits never take a link. Needs proper
				// testing across games.
				block->links[link_count].deterministic = block->profiled;
				return &link_sites[link_count++];
			};
			// Entered from another block's exit: the frame, start and budget are
			// already in place, and a linked exit never has a packet pending. So
			// are x19, the block constants (every block uses the same options, and
			// code that calls C++ reloads them) and x26, which the exit stored.
			a.Bind(&linked_entry);
			a.Str(wzr, MemOperand(sp, 72));
			a.B(&common_entry);
			a.Bind(&entry);
			a.Stp(x19, x20, MemOperand(sp, -frame_size, PreIndex));
			a.Stp(x21, x22, MemOperand(sp, 16));
			a.Stp(x23, x24, MemOperand(sp, 32));
			a.Stp(x25, x26, MemOperand(sp, 48));
			a.Str(lr, MemOperand(sp, 64));
			a.Str(w2, MemOperand(sp, 72));
			a.Stp(x27, x28, MemOperand(sp, 80));
			for (u32 slot = 0; slot < 8; slot += 2)
				a.Stp(VRegister(8 + slot, 64), VRegister(9 + slot, 64), MemOperand(sp, saved_size + slot * 8));
			a.Mov(x20, x0);
			a.Mov(x21, x1);
			a.Mov(x19, reinterpret_cast<uintptr_t>(&VU1));
			// Keep queue insertion and budget checks off the cycle store/load chain.
			a.Ldr(x26, Field(offsetof(VURegs, cycle)));
			EmitBlockConstants(a);
			a.Bind(&common_entry);
			u32 scheduled_pairs = 0;
			for (u32 i = first_scheduled; i < block->count; i++)
				scheduled_pairs += block->schedule[i].cycles != 0;
			// Amortize the entry guard over several scheduled pairs. A profiled
			// block needs none: Execute() checked the same conditions, and its
			// schedule already accounts for the one special entry it admits, a
			// pending divide.
			const bool scheduled = block->profiled ? scheduled_pairs != 0 : scheduled_pairs >= 4;
			// With the IALU pipe occupied, the first readiness check after the
			// generic pairs that drain it enables the schedule.
			if (block->profiled && scheduled)
				a.Mov(w25, block->incoming.Ialu() ? 1 : 3);
			else if (scheduled)
				EmitScheduleGuard(a);
			a.Mov(x24, reinterpret_cast<uintptr_t>(cache.offsets.data()));
			for (u32 slot = 0; slot < cache.count; slot++)
				a.Ldr(VRegister(8 + slot, 128), Field(cache.offsets[slot]));
			// Share the most frequent helper address in x22; keep preparation code
			// outside the emitted instruction stream to avoid instruction-cache growth.
			const auto& prepare = s_pipeline.prepare;
			std::array<u32, std::size(prepare)> uses{};
			for (u32 i = 0; i < block->count; i++)
			{
				const auto& ins = block->instructions[i];
				uses[ins.readsVF ? ins.dependency + 2 : 0]++;
			}
			const u32 shared_prepare = std::max_element(uses.begin(), uses.end()) - uses.begin();
			a.Mov(x22, reinterpret_cast<uintptr_t>(prepare[shared_prepare]));
			a.Mov(x23, reinterpret_cast<uintptr_t>(block->instructions.data()));
			a.Bind(&loop_entry);
			bool cycle_dirty = false;
			bool readiness_checked = false;
			u32 region_index = 0;
			for (u32 i = 0; i < block->count; i++)
			{
				const auto& ins = block->instructions[i];
				if (region_index < regions.size() && i == regions[region_index].end)
				{
					a.Bind(&resumes[region_index]);
					region_index++;
				}
				const bool region_start = region_index < regions.size() && i == regions[region_index].first;
				const bool schedule_pair = scheduled && block->schedule[i].cycles != 0;
				if (schedule_pair && (!readiness_checked || !block->schedule[i - 1].cycles || region_start))
				{
					EmitScheduleReadiness(a, block->schedule[i].fdiv_pending, block->schedule[i].ialu_pending, block->schedule[i].efu_pending);
					readiness_checked = true;
				}
				if (region_start)
				{
					Label partial;
					a.Tbz(w25, 1, &partial);
					a.Sub(x9, x26, x20);
					a.Sub(x9, x21, x9);
					a.Cmp(x9, regions[region_index].cycles);
					a.B(hs, &deferred_entries[region_index]);
					a.Bind(&partial);
				}
				Label generic_prepare, prepared;
				if (schedule_pair)
				{
					a.Tbz(w25, 1, &generic_prepare);
					EmitScheduledPrepare(a, *block, i);
					a.B(&prepared);
					a.Bind(&generic_prepare);
				}
				// Publish the cached cycle only when a preceding scheduled pair may
				// have changed it. Generic preparation/callbacks use architectural state.
				if (cycle_dirty)
					a.Str(x26, Field(offsetof(VURegs, cycle)));
				const u32 selected_prepare = ins.readsVF ? ins.dependency + 2 : 0;
				a.Add(x0, x23, i * sizeof(Instruction));
				const bool integer_branch = ins.lregs.pipe == VUPIPE_BRANCH && ins.lregs.VIread;
				if (integer_branch)
				{
					a.Mov(x16, reinterpret_cast<uintptr_t>(s_pipeline.branch_prepare));
					a.Blr(x16);
				}
				else if (selected_prepare == shared_prepare)
					a.Blr(x22);
				else
				{
					a.Mov(x16, reinterpret_cast<uintptr_t>(prepare[selected_prepare]));
					a.Blr(x16);
				}
				a.Ldr(x26, Field(offsetof(VURegs, cycle)));
				if (schedule_pair)
					a.Bind(&prepared);
				cycle_dirty = schedule_pair;
				s_clamp_skip = block->clamp_skip[i];
				EmitPair(a, cache, ins, true);
				s_clamp_skip = 0;
				const bool kick = ins.lregs.pipe == VUPIPE_XGKICK;
				if (kick)
				{
					EmitKick(a, ins.lower);
					if (scheduled)
						a.Mov(w25, 0);
				}
				EmitFinish(a, ins);
				EmitIntegerIssue(a, ins);
				EmitControlFlow(a, *block, i);
				if (!kick && (i == 0 || block->instructions[i - 1].lregs.pipe == VUPIPE_XGKICK))
				{
					Label no_packet;
					if (i == 0)
					{
						a.Ldr(w9, MemOperand(sp, 72));
						a.Cbz(w9, &no_packet);
					}
					// Commit the delayed pair before GIF observes VU state, then
					// continue with the same cache assignment and host frame.
					a.Str(x26, Field(offsetof(VURegs, cycle)));
					a.Mov(x16, reinterpret_cast<uintptr_t>(s_pipeline.finish_packet));
					a.Blr(x16);
					a.Ldr(x26, Field(offsetof(VURegs, cycle)));
					EmitBlockConstants(a);
					// Scheduling was disabled while the packet was pending. Recheck
					// after the callback; generic preparation rebuilds known timing.
					if (scheduled)
						EmitScheduleGuard(a);
					a.Bind(&no_packet);
				}
				if (integer_branch && i + 1 < block->count)
				{
					// The fallthrough path has no pending branch. Publish the common
					// cache at exit; only the taken path executes the connected delay.
					a.Ldr(w9, Field(offsetof(VURegs, branch)));
					a.Cbz(w9, add_link(i));
				}
				a.Sub(x9, x26, x20);
				a.Cmp(x9, x21);
				a.B(hs, &exit);
			}
			if (deferred)
				a.B(&finished);
			std::vector<std::unique_ptr<RegionBranchExit>> branch_exits;
			for (u32 r = 0; r < regions.size(); r++)
			{
				const auto& region = regions[r];
				a.Bind(&deferred_entries[r]);
				EmitDeferredRegion(a, *block, region.first, region.end, branch_exits);
				// The emitter borrows w25 for flags. Entry proved both guards and
				// no special work can be issued inside the region.
				a.Mov(w25, 3);
				if (region.end < block->count)
				{
					// Queues are complete; retain VF/ACC for the following region.
					// Exact budget exhaustion must exit before its first pair.
					a.Sub(x9, x26, x20);
					a.Cmp(x9, x21);
					a.B(lo, &resumes[r]);
				}
				a.B(region.end == block->count ? &finished : &exit);
			}
			for (const auto& branch_exit : branch_exits)
			{
				a.Bind(&branch_exit->label);
				EmitRegionExit(a, *block, branch_exit->state, branch_exit->index);
				a.B(add_link(branch_exit->index));
			}
			a.Bind(&finished);
			if (block->loops_to_entry)
			{
				a.Sub(x9, x26, x20);
				a.Cmp(x9, x21);
				a.B(hs, &exit);
				// Keep VF/ACC and the host frame, but start pipeline preparation
				// afresh. Every reachable source pair was validated before entry.
				a.Str(x26, Field(offsetof(VURegs, cycle)));
				if (scheduled)
					EmitScheduleGuard(a);
				a.Mov(w9, block->instructions[block->count - 1].lregs.pipe == VUPIPE_XGKICK ? 1 : 0);
				a.Str(w9, MemOperand(sp, 72));
				a.B(&loop_entry);
			}
			else if (block->next_pc[block->count - 1] == block->instructions[0].pc)
			{
				// The end exit continues at this block's own entry, typically a
				// loop whose closing branch has the entry pair as its delay slot
				// (profiled loops always leave this way). Once Execute() linked it
				// back to this very block, its first way enters it, and a running
				// block is current: re-enter at loop_entry with the cache, x22-x24
				// and the frame as they are, skipping the spill, the reload and
				// the way search. The other checks match link_exit's; failing one
				// leaves through `exit`, which spills. Needs proper testing.
				Label* site = add_link(block->count - 1);
				const LinkSlot& slot = block->links[link_count - 1];
				a.Mov(x0, reinterpret_cast<uintptr_t>(&slot));
				a.Ldr(x11, MemOperand(x0, offsetof(LinkSlot, ways) + offsetof(LinkSlot::Way, entry)));
				a.Adr(x10, &linked_entry);
				a.Cmp(x11, x10);
				a.B(ne, site);
				a.Ldr(w9, Field(VI(REG_TPC)));
				a.Ldr(w10, MemOperand(x0, offsetof(LinkSlot, ways) + offsetof(LinkSlot::Way, pc)));
				a.Cmp(w9, w10);
				a.B(ne, site);
				a.Sub(x9, x26, x20);
				a.Cmp(x9, x21);
				a.B(hs, &exit);
				if (s_options & 32)
				{
					a.Ldr(w9, Field(offsetof(VURegs, flags)));
					a.Tbz(w9, __builtin_ctz(VUFLAG_MTVURUNNING), &exit);
				}
				else
				{
					a.Mov(x16, reinterpret_cast<uintptr_t>(&VU0.VI[REG_VPU_STAT].UL));
					a.Ldr(w9, MemOperand(x16));
					a.Tbz(w9, 8, &exit);
				}
				a.Ldr(w9, Field(offsetof(VURegs, branch)));
				a.Ldr(w10, Field(offsetof(VURegs, ebit)));
				a.Orr(w9, w9, w10);
				a.Ldrb(w10, Field(offsetof(VURegs, takedelaybranch)));
				a.Orr(w9, w9, w10);
				a.Ldr(w10, Field(offsetof(VURegs, xgkickenable)));
				a.Orr(w9, w9, w10);
				a.Cbnz(w9, &exit);
				a.Str(x26, Field(offsetof(VURegs, cycle)));
				a.Str(wzr, MemOperand(sp, 72));
				if (block->profiled && scheduled)
					a.Mov(w25, block->incoming.Ialu() ? 1 : 3);
				else if (scheduled)
					EmitScheduleGuard(a);
				a.B(&loop_entry);
			}
			else
				a.B(add_link(block->count - 1));
			for (u32 k = 0; k < link_count; k++)
			{
				a.Bind(&link_sites[k]);
				a.Mov(x0, reinterpret_cast<uintptr_t>(&block->links[k]));
				a.B(&link_exit);
			}
			a.Bind(&exit);
			a.Str(x26, Field(offsetof(VURegs, cycle)));
			for (u32 slot = 0; slot < cache.count; slot++)
				if (cache.dirty & (1 << slot))
					a.Str(VRegister(8 + slot, 128), Field(cache.offsets[slot]));
			a.Bind(&restore);
			for (u32 slot = 0; slot < 8; slot += 2)
				a.Ldp(VRegister(8 + slot, 64), VRegister(9 + slot, 64), MemOperand(sp, saved_size + slot * 8));
			a.Ldp(x27, x28, MemOperand(sp, 80));
			a.Ldr(lr, MemOperand(sp, 64));
			a.Ldp(x25, x26, MemOperand(sp, 48));
			a.Ldp(x23, x24, MemOperand(sp, 32));
			a.Ldp(x21, x22, MemOperand(sp, 16));
			a.Ldp(x19, x20, MemOperand(sp, frame_size, PostIndex));
			a.Ret();
			// x0 = the exit's LinkSlot. Publish the state the next block (or
			// Execute()) reads, then take the link if Execute() would enter a
			// block at this point too and the slot has a current target for TPC.
			a.Bind(&link_exit);
			a.Str(x26, Field(offsetof(VURegs, cycle)));
			for (u32 slot = 0; slot < cache.count; slot++)
				if (cache.dirty & (1 << slot))
					a.Str(VRegister(8 + slot, 128), Field(cache.offsets[slot]));
			a.Sub(x9, x26, x20);
			a.Cmp(x9, x21);
			a.B(hs, &restore);
			if (s_options & 32)
			{
				a.Ldr(w9, Field(offsetof(VURegs, flags)));
				a.Tbz(w9, __builtin_ctz(VUFLAG_MTVURUNNING), &restore);
			}
			else
			{
				a.Mov(x16, reinterpret_cast<uintptr_t>(&VU0.VI[REG_VPU_STAT].UL));
				a.Ldr(w9, MemOperand(x16));
				a.Tbz(w9, 8, &restore);
			}
			a.Ldr(w9, Field(offsetof(VURegs, branch)));
			a.Ldr(w10, Field(offsetof(VURegs, ebit)));
			a.Orr(w9, w9, w10);
			a.Ldrb(w10, Field(offsetof(VURegs, takedelaybranch)));
			a.Orr(w9, w9, w10);
			a.Ldr(w10, Field(offsetof(VURegs, xgkickenable)));
			a.Orr(w9, w9, w10);
			a.Cbnz(w9, &restore);
			a.Ldr(w9, Field(VI(REG_TPC)));
			for (u32 way = 0; way < LinkSlot::Ways; way++)
			{
				const size_t base = offsetof(LinkSlot, ways) + way * sizeof(LinkSlot::Way);
				Label next_way;
				a.Ldr(w10, MemOperand(x0, base + offsetof(LinkSlot::Way, pc)));
				a.Cmp(w9, w10);
				a.B(ne, &next_way);
				a.Mov(x16, reinterpret_cast<uintptr_t>(&s_link_generation));
				a.Ldr(w12, MemOperand(x16));
				a.Ldr(w10, MemOperand(x0, base + offsetof(LinkSlot::Way, generation)));
				a.Cmp(w10, w12);
				a.B(ne, &link_request);
				a.Ldr(x12, MemOperand(x0, base + offsetof(LinkSlot::Way, epoch)));
				a.Ldr(w12, MemOperand(x12));
				a.Mov(x16, reinterpret_cast<uintptr_t>(&s_epoch));
				a.Ldr(w13, MemOperand(x16));
				a.Cmp(w12, w13);
				a.B(ne, &link_request);
				a.Ldr(x11, MemOperand(x0, base + offsetof(LinkSlot::Way, entry)));
				a.Br(x11);
				a.Bind(&next_way);
			}
			// A next PC this exit has not linked yet.
			a.Bind(&link_request);
			a.Mov(x16, reinterpret_cast<uintptr_t>(&s_pending_link));
			a.Str(x0, MemOperand(x16));
			a.B(&restore);
			a.FinalizeCode();
			const size_t size = a.GetSizeOfCodeGenerated();
			HostSys::EndCodeWrite();
			HostSys::FlushInstructionCache(s_write, static_cast<u32>(size));
			block->function = reinterpret_cast<Block::Function>(s_write + entry.GetLocation());
			block->linked_entry = s_write + linked_entry.GetLocation();
			s_write += (size + 15) & ~size_t(15);
		}
		// An evicted variant's code stays in the buffer until the next
		// InvalidateAll, like any other replaced block.
		auto& variants = s_blocks[pc / 8];
		if (variants.size() >= MaxVariants)
		{
			variants.pop_back();
			s_link_generation++;
		}
		variants.insert(variants.begin(), std::move(block));
		return *variants.front();
	}

	bool PacketXgkickPending()
	{
		return CpuVU1 == &CpuArm64VU1 && !CHECK_XGKICKHACK &&
		       VU1.xgkickenable == VURegs::XgkickPacket && VU1.xgkicksizeremaining == 0;
	}

	// The conditions EmitScheduleGuard checks at runtime, except that a pending
	// divide is admitted and described instead. Anything else stays unprofiled.
	bool CaptureProfile(IncomingProfile& profile)
	{
		if (VU1.xgkickenable)
			return false;
		const u64 cycle = VU1.cycle;
		if (cycle >= ~u64(0) - (MaxInstructions * 4 + 4))
			return false;
		u32 efu = 0;
		if (VU1.efu.enable)
		{
			if (VU1.efu.sCycle > cycle || VU1.efu.Cycle > 60)
				return false;
			const u64 ready = VU1.efu.sCycle + VU1.efu.Cycle;
			efu = static_cast<u32>((ready > cycle ? ready - cycle : 0) + 1);
		}
		u32 fdiv = 0;
		if (VU1.fdiv.enable)
		{
			if (VU1.fdiv.sCycle > cycle || VU1.fdiv.Cycle > 32)
				return false;
			const u64 ready = VU1.fdiv.sCycle + VU1.fdiv.Cycle;
			// Every pair advances at least one cycle, so the entry has retired
			// by the pair after this many.
			fdiv = static_cast<u32>((ready > cycle ? ready - cycle : 0) + 1);
		}
		// ILW/ILWR results only delay integer branches, and the generic
		// preparation drains the pipe; the schedule stays off until it has.
		u32 ialu = 0, ialu_regs = 0;
		if (VU1.ialucount)
		{
			if (VU1.ialucount > 4 || VU1.ialureadpos > 3 || VU1.ialuwritepos != ((VU1.ialureadpos + VU1.ialucount) & 3))
				return false;
			u64 ready = 0;
			for (u32 k = 0; k < VU1.ialucount; k++)
			{
				const ialuPipe& entry = VU1.ialu[(VU1.ialureadpos + k) & 3];
				if (entry.sCycle > cycle || entry.Cycle > 8)
					return false;
				ready = std::max<u64>(ready, entry.sCycle + entry.Cycle);
				// Due entries stall nothing.
				if (entry.sCycle + entry.Cycle > cycle)
					ialu_regs = ialu_regs ? IncomingProfile::AnyIaluRegs : (entry.reg & 0xffff);
			}
			ialu = static_cast<u32>((ready > cycle ? ready - cycle : 0) + 1);
		}
		const u32 count = VU1.fmaccount;
		if (count > 4 || VU1.fmacreadpos > 3 || VU1.fmacwritepos != ((VU1.fmacreadpos + count) & 3))
			return false;
		profile.Set(count, fdiv, ialu, efu);
		profile.SetIaluRegs(ialu_regs);
		u64 previous = 0;
		for (u32 k = 0; k < count; k++)
		{
			const fmacPipe& entry = VU1.fmac[(VU1.fmacreadpos + k) & 3];
			// At most one entry per cycle, in issue order, as the queue fills.
			if (entry.Cycle != 4 || entry.sCycle > cycle || (k && entry.sCycle <= previous))
				return false;
			previous = entry.sCycle;
			IncomingFmac fmac;
			fmac.flags = (entry.flagreg & (1 << REG_STATUS_FLAG)) != 0;
			fmac.clip = (entry.flagreg & (1 << REG_CLIP_FLAG)) != 0;
			// Three cycles old is ready at the first pair's cycle and cannot stall
			// it; the registers of such an entry no longer matter.
			fmac.age = static_cast<u32>(std::min<u64>(cycle - entry.sCycle, 3));
			if (fmac.age < 3)
			{
				if ((entry.regupper | entry.reglower) > 31 || (entry.xyzwupper | entry.xyzwlower) > 15)
					return false;
				fmac.regupper = entry.regupper;
				fmac.reglower = entry.reglower;
				fmac.xyzwupper = entry.xyzwupper;
				fmac.xyzwlower = entry.xyzwlower;
			}
			profile.SetFmac(k, fmac);
		}
		return true;
	}

	bool Matches(const Block& block, u32 pc, u64 max_pairs)
	{
		if (!block.count)
			return std::memcmp(block.words.data(), VU1.Micro + pc, 8) == 0;
		for (const auto& range : block.guard_ranges)
			if (std::memcmp(block.guard_words.data() + range.first * 2, VU1.Micro + range.pc, range.count * 8) != 0)
				return false;
		// Every pair advances at least one cycle. A short call cannot reach the
		// remainder of a connected trace; validate that remainder when it can.
		for (const auto& range : block.ranges)
		{
			const u32 count = static_cast<u32>(std::min<u64>(range.count, max_pairs));
			if (std::memcmp(block.words.data() + range.first * 2, VU1.Micro + range.pc, count * 8) != 0)
				return false;
			max_pairs -= count;
			if (!max_pairs)
				break;
		}
		return true;
	}
} // namespace

Arm64VU1Recompiler CpuArm64VU1;

Arm64VU1Recompiler::Arm64VU1Recompiler()
{
	m_Idx = 1;
	IsInterpreter = false;
}

void Arm64VU1Recompiler::Reserve()
{
	s_base = SysMemory::GetVU1Rec();
	s_end = SysMemory::GetVU1RecEnd();
	InvalidateAll();
}

void Arm64VU1Recompiler::Shutdown()
{
	InvalidateAll();
	s_base = s_write = s_end = nullptr;
}

void Arm64VU1Recompiler::Reset()
{
	CpuIntVU1.Reset();
	InvalidateAll();
}

void Arm64VU1Recompiler::SetStartPC(u32 pc) { VU1.start_pc = pc; }
void Arm64VU1Recompiler::Step()
{
	const bool pending = PacketXgkickPending();
	if (!pending)
	{
		CpuIntVU1.Step();
		return;
	}
	u32 words[2];
	std::memcpy(words, VU1.Micro + (VU1.VI[REG_TPC].UL & VU1_PROGMASK), sizeof(words));
	const bool new_kick = !(words[1] & 0x80000000) && (words[0] & 0xfe0007ff) == 0x800006fc;
	CpuIntVU1.Step();
	// A second XGKICK flushes the old request itself and starts a new delay.
	if (!new_kick && PacketXgkickPending())
		_vuXGKICKTransfer(0, true);
}
void Arm64VU1Recompiler::Clear(u32, u32)
{
	// Every micro memory write calls this first: MPG uploads (a wrapped one
	// clears only its first part, so the range is ignored), EE and debugger
	// writes through the memory map, and MTVU's ring. State loads go through
	// Reset(). Blocks are kept; the next Execute() revalidates their source
	// before entry, which avoids recompiling identical program uploads.
	s_micro_changed.store(true, std::memory_order_release);
}
size_t Arm64VU1Recompiler::GetCommittedCache() const { return s_base ? s_write - s_base : 0; }
u64 Arm64VU1Recompiler::GetDispatchCount() const { return s_dispatches; }

void Arm64VU1Recompiler::Execute(u32 cycles)
{
	if (!s_base)
		Reserve();
	if (s_options != Options())
		InvalidateAll();
	// Under MTVU this runs on the VU1 thread, where VU0.VI[REG_VPU_STAT] belongs to
	// the EE thread. Reading its busy bit here raced with the EE clearing it, and
	// writing it corrupted the EE's VIF1 stall handling. Use the VU1-local flag the
	// MTVU dispatcher sets instead; the interpreter clears it at the E-bit.
	const bool mtvu = THREAD_VU1;
	// Nothing else on the MTVU thread computes in floating point (its ring
	// carries unpacks, GIF packets and memory writes), so the VU1 mode stays set
	// between calls there. Writing FPCR waits for in-flight FP work: Shadow of
	// the Colossus makes ~500k calls a second, and the restore alone took ~9% of
	// the thread. Needs proper testing across games.
	std::optional<FPControlRegisterBackup> fpcr;
	if (!mtvu)
		fpcr.emplace(EmuConfig.Cpu.VU1FPCR);
	else if (FPControlRegister::GetCurrent() != EmuConfig.Cpu.VU1FPCR)
		FPControlRegister::SetCurrent(EmuConfig.Cpu.VU1FPCR);
	// Micro memory only changes between Execute() calls, through Clear(). A
	// block that passed source validation since then may be entered and linked
	// to without comparing its source again. Games that run many short
	// microprograms paid most of Execute()'s own time for that comparison.
	if (s_micro_changed.exchange(false, std::memory_order_acquire) && ++s_epoch == 0)
	{
		s_epoch = 1;
		s_link_generation++;
	}
	LinkSlot* pending_link = nullptr;
	u32 pending_generation = 0;
	VU1.VI[REG_TPC].UL <<= 3;
	const u64 start = VU1.cycle;
	// Pick the run gate once. Written as one conditional expression, clang
	// loaded both words every iteration and selected one; under MTVU the EE
	// keeps writing VU0's line, and that load was ~10% of the MTVU thread.
	const u32* const run_gate = mtvu ? &VU1.flags : &VU0.VI[REG_VPU_STAT].UL;
	const u32 run_mask = mtvu ? VUFLAG_MTVURUNNING : 0x100;
	while (VU1.cycle - start < cycles)
	{
		if (!(*run_gate & run_mask))
		{
			if (VU1.branch == 1)
			{
				VU1.VI[REG_TPC].UL = VU1.branchpc;
				VU1.branch = 0;
			}
			break;
		}
		const u32 pc = VU1.VI[REG_TPC].UL & VU1_PROGMASK;
		VU1.VI[REG_TPC].UL = pc;
		// Pending branch/E-bit delay slots must be retired by the original path.
		if (VU1.branch || VU1.ebit || (pc & 7))
		{
			Step();
			pending_link = nullptr;
			continue;
		}
		const bool pending = PacketXgkickPending();
		const u64 remaining = cycles - (VU1.cycle - start);
		auto& variants = s_blocks[pc / 8];
		IncomingProfile profile;
		const bool profilable = !pending && s_profiled_compiles[pc / 8] < MaxProfiledCompiles && CaptureProfile(profile);
		// Prefer a variant compiled for this incoming state. An unprofiled one
		// with the same source serves when the state cannot be profiled, and
		// always once profiling its loop was rejected.
		size_t found = variants.size(), unprofiled = variants.size();
		for (size_t i = 0; i < variants.size(); i++)
		{
			const Block& candidate = *variants[i];
			if (candidate.profiled && (!profilable || !(candidate.incoming == profile)))
				continue;
			const bool usable = candidate.profiled || !profilable || candidate.profile_rejected || !candidate.count;
			if (!usable && unprofiled != variants.size())
				continue;
			if (candidate.validated_epoch != s_epoch && !Matches(candidate, pc, remaining))
				continue;
			// A budget shorter than the block validated only a prefix.
			if (remaining >= candidate.count)
				variants[i]->validated_epoch = s_epoch;
			if (!usable)
			{
				unprofiled = i;
				continue;
			}
			found = i;
			break;
		}
		if (found == variants.size() && !profilable)
			found = unprofiled;
		Block* block;
		if (found != variants.size())
		{
			std::rotate(variants.begin(), variants.begin() + found, variants.begin() + found + 1);
			block = variants.front().get();
		}
		else if (profilable)
		{
			s_profiled_compiles[pc / 8]++;
			block = &Compile(pc, &profile);
		}
		else
			block = &Compile(pc, nullptr);
		if (found == variants.size())
			block->validated_epoch = s_epoch; // compiled from the current source
		// A restored chained-delay state still requires interpreter branch retirement.
		if (block->function && !(block->has_branches && VU1.takedelaybranch))
		{
			// Link the exit that just returned here, unless a compile since then
			// may have freed its block. Any exit may enter an unprofiled block,
			// whose entry guard checks the incoming state; a profiled one only
			// from an exit whose state is always the one seen now.
			if (pending_link && pending_generation == s_link_generation &&
				block->validated_epoch == s_epoch && (!block->profiled || pending_link->deterministic))
			{
				LinkSlot::Way* way = nullptr;
				for (auto& candidate : pending_link->ways)
				{
					if (candidate.pc == pc)
						way = &candidate;
				}
				if (!way)
					way = &pending_link->ways[pending_link->next_way++ % LinkSlot::Ways];
				way->entry = block->linked_entry;
				way->epoch = &block->validated_epoch;
				way->pc = pc;
				way->generation = s_link_generation;
			}
			s_pending_link = nullptr;
			pending_generation = s_link_generation;
			// The generated first-pair boundary publishes the delayed transfer,
			// including its lower store, even when that pair exhausts the budget.
			s_dispatches++;
			block->function(start, cycles, pending);
			pending_link = s_pending_link;
		}
		else
		{
			Step();
			pending_link = nullptr;
		}
	}
	VU1.VI[REG_TPC].UL >>= 3;
	// nextBlockCycles pairs VU1.cycle with the EE clock for the synchronous path.
	// Under MTVU neither operand is valid here: MTVU resets VU1.cycle to 0 per run,
	// and cpuRegs.cycle is owned by the EE thread.
	if (!mtvu)
		VU1.nextBlockCycles = (VU1.cycle - cpuRegs.cycle) + 1;
}
