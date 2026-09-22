// SPDX-FileCopyrightText: 2026 PCSX2 Dev Team
// SPDX-License-Identifier: GPL-3.0+

#include "arm64/VU0Pipeline.h"
#include "vixl/aarch64/macro-assembler-aarch64.h"

// Mirrors arm64/VU1Pipeline.cpp's CompilePipeline, minus the branch-wait
// entry and the XGKICK/VIBackupCycles-skipping standalone retire_queues
// entry point VU1 needs. VU0Recompiler.cpp never compiles a branch or an
// XGKICK/FDIV/EFU-pipe op into a block (see its Compile()), so those parts
// of VU1's pipeline stub have no VU0 use yet.
namespace Arm64VU0
{
	using namespace vixl::aarch64;

	PipelineCode CompilePipeline(u8* code, size_t capacity)
	{
		MacroAssembler a(code, capacity);
		PipelineCode result;
		Label scan, retire, retire_mid;
		auto field = [](size_t offset) { return MemOperand(x19, offset); };
		auto vi = [&](u32 reg) { return field(offsetof(VURegs, VI) + sizeof(REG_VI) * reg); };

		// Six FMAC entry stubs and an integer-branch entry share retirement. Keep
		// this code outside the guest blocks, where repeating it would inflate
		// the I-cache.
		for (int entry = 0; entry < 7; entry++)
		{
			const int dependency = entry == 6 ? -1 : entry - 2;
			if (entry == 6)
				result.branch_prepare = code + a.GetCursorOffset();
			else
				result.prepare[entry] = code + a.GetCursorOffset();
			// Entries 0 and 2-5 leave w2 unset here: their common path jumps straight to
			// retire_mid, which never reads w2, and their rare wrap fallback sets w2=0
			// itself right before rejoining the scan/retire path that does.
			if (entry == 6)
				a.Ldr(w2, MemOperand(x0, offsetof(Instruction, lregs) + offsetof(_VURegsNum, VIread)));
			else if (dependency == -1)
				a.Mov(w2, 0); // Entry 1 always falls through the scan path's real w2 check below.
			a.Ldr(x9, field(offsetof(VURegs, cycle)));
			a.Mov(w15, w9); // Interpreter truncates cyclesBeforeOp to u32.
			a.Add(x9, x9, 1);
			a.Ldr(w10, MemOperand(x0, offsetof(Instruction, pc)));
			a.Add(w10, w10, 8);
			a.Str(w10, vi(REG_TPC));
			a.Ldr(w10, MemOperand(x0, offsetof(Instruction, upper)));
			a.Ldr(w11, MemOperand(x0, offsetof(Instruction, lower)));
			a.Tst(w10, 0x80000000);
			a.Csel(w10, w10, w11, ne);
			a.Str(w10, field(offsetof(VURegs, code)));
			if (dependency == -1)
			{
				a.B(&scan);
				continue;
			}
			if (dependency >= 0)
			{
				Label skip_check;
				// Retain the reference scan at the cycle-wrap boundary: below this,
				// a wrapped subtraction from the producer's sCycle could look ready
				// when it is not.
				a.Cmn(x9, 5);
				a.B(hs, &skip_check);
				if (dependency)
				{
					a.Ldr(w10, field(offsetof(VURegs, fmacwritepos)));
					a.Sub(w10, w10, dependency);
					a.And(w10, w10, 3);
					a.Mov(w11, sizeof(fmacPipe));
					a.Madd(x10, x10, x11, x19);
					a.Add(x10, x10, offsetof(VURegs, fmac));
					a.Ldr(x11, MemOperand(x10, offsetof(fmacPipe, sCycle)));
					a.Ldr(w12, MemOperand(x10, offsetof(fmacPipe, Cycle)));
					a.Sub(x13, x9, x11);
					a.Cmp(x13, x12);
					a.Add(x11, x11, x12);
					a.Csel(x11, x11, x9, lo);
					a.Cmp(x9, x11);
					a.Csel(x9, x9, x11, hs);
				}
				a.Bind(&skip_check);
			}
			a.B(&retire_mid);
		}

		// Inspect incoming FMAC entries, merging the upper/lower lane hazards.
		a.Bind(&scan);
		a.Ldr(w10, field(offsetof(VURegs, fmaccount)));
		a.Cbz(w10, &retire);
		a.Ldr(w11, field(offsetof(VURegs, fmacreadpos)));
		a.Add(x0, x0, offsetof(Instruction, readMasks));
		{
			Label loop, next;
			a.Bind(&loop);
			a.Mov(w12, sizeof(fmacPipe));
			a.Madd(x12, x11, x12, x19);
			a.Add(x12, x12, offsetof(VURegs, fmac));
			a.Ldr(x13, MemOperand(x12, offsetof(fmacPipe, sCycle)));
			a.Ldr(w14, MemOperand(x12, offsetof(fmacPipe, Cycle)));
			a.Sub(x16, x9, x13);
			a.Cmp(x16, x14);
			a.B(hs, &next);
			a.Ldr(w16, MemOperand(x12, offsetof(fmacPipe, regupper)));
			a.Ldrb(w16, MemOperand(x0, x16));
			a.Ldr(w17, MemOperand(x12, offsetof(fmacPipe, xyzwupper)));
			a.And(w16, w16, w17);
			a.Ldr(w1, MemOperand(x12, offsetof(fmacPipe, reglower)));
			a.Ldrb(w1, MemOperand(x0, x1));
			a.Ldr(w17, MemOperand(x12, offsetof(fmacPipe, xyzwlower)));
			a.And(w1, w1, w17);
			a.Orr(w16, w16, w1);
			a.Cbz(w16, &next);
			a.Add(x13, x13, x14);
			a.Cmp(x9, x13);
			a.Csel(x9, x9, x13, hs);
			a.Bind(&next);
			a.Add(w11, w11, 1);
			a.And(w11, w11, 3);
			a.Subs(w10, w10, 1);
			a.B(ne, &loop);
		}
		a.B(&retire);

		// Drains FMAC/FDIV/EFU/IALU entries ready at cycle x9, in
		// VUPipeline::Retire()'s order. FDIV/EFU must be drained even for a block
		// that contains no DIV/SQRT/RSQRT/EFU op itself, because it does contain
		// the ops that *read* their results (Q via an op.broadcast == 5 upper, P
		// via MFP); leaving those pipes un-retired made a compiled block read a
		// stale Q/P once it advanced the cycle past the pipe's latency. No XGKICK
		// handling: VU0 has no GIF path at all.
		// Shared by the ordinary retire flow and the standalone retire_queues
		// entry point. Drains FMAC/FDIV/EFU/IALU entries ready at cycle x9; the
		// integer-branch wait uses w2 (zero outside branch_prepare). Deliberately
		// excludes VIBackupCycles, which must run at most once per pair, so a
		// caller invoking this a second time for the same pair (see
		// PipelineCode::retire_queues) does not decrement it twice.
		// `mid`, when non-null, is bound right after the integer-branch wait, for
		// callers that already know w2 == 0 and can skip that dead check.
		auto emit_retire_queues = [&](Label* mid = nullptr) {
			{
				// An integer-conditional branch waits for a pending integer load that
				// writes a register it tests, after the upper FMAC stalls above. w2 is
				// the branch's lregs.VIread set, and zero for every other entry, which
				// is why they can jump straight to retire_mid and skip this.
				Label loop, next, end;
				a.Cbz(w2, &end);
				a.Ldr(w10, field(offsetof(VURegs, ialucount)));
				a.Cbz(w10, &end);
				a.Ldr(w11, field(offsetof(VURegs, ialureadpos)));
				a.Bind(&loop);
				a.Mov(w12, sizeof(ialuPipe));
				a.Madd(x12, x11, x12, x19);
				a.Add(x12, x12, offsetof(VURegs, ialu));
				a.Ldr(x13, MemOperand(x12, offsetof(ialuPipe, sCycle)));
				a.Ldr(w14, MemOperand(x12, offsetof(ialuPipe, Cycle)));
				a.Sub(x16, x9, x13);
				a.Cmp(x16, x14);
				a.B(hs, &next);
				a.Ldr(w17, MemOperand(x12, offsetof(ialuPipe, reg)));
				a.Tst(w17, w2);
				a.B(eq, &next);
				a.Add(x13, x13, x14);
				a.Cmp(x9, x13);
				a.Csel(x9, x9, x13, hs);
				a.Bind(&next);
				a.Add(w11, w11, 1);
				a.And(w11, w11, 3);
				a.Subs(w10, w10, 1);
				a.B(ne, &loop);
				a.Bind(&end);
			}
			if (mid)
				a.Bind(mid);
			a.Str(x9, field(offsetof(VURegs, cycle)));
			{
				Label loop, end, no_clip, normal_flags, store_flags;
				a.Ldr(w10, field(offsetof(VURegs, fmaccount)));
				a.Cbz(w10, &end);
				a.Ldr(w11, field(offsetof(VURegs, fmacreadpos)));
				a.Bind(&loop);
				a.Mov(w12, sizeof(fmacPipe));
				a.Madd(x12, x11, x12, x19);
				a.Add(x12, x12, offsetof(VURegs, fmac));
				a.Ldr(x13, MemOperand(x12, offsetof(fmacPipe, sCycle)));
				a.Ldr(w14, MemOperand(x12, offsetof(fmacPipe, Cycle)));
				a.Sub(x13, x9, x13);
				a.Cmp(x13, x14);
				a.B(lo, &end);
				a.Ldr(w13, MemOperand(x12, offsetof(fmacPipe, flagreg)));
				a.Tbz(w13, REG_CLIP_FLAG, &no_clip);
				a.Ldr(w14, MemOperand(x12, offsetof(fmacPipe, clipflag)));
				a.Str(w14, vi(REG_CLIP_FLAG));
				a.Bind(&no_clip);
				a.Ldr(w14, vi(REG_STATUS_FLAG));
				a.Ldr(w16, MemOperand(x12, offsetof(fmacPipe, statusflag)));
				a.Tbz(w13, REG_STATUS_FLAG, &normal_flags);
				a.And(w14, w14, 0x30);
				a.And(w13, w16, 0xfc0);
				a.Orr(w14, w14, w13);
				a.And(w16, w16, 0xf);
				a.Orr(w14, w14, w16);
				a.B(&store_flags);
				a.Bind(&normal_flags);
				a.And(w14, w14, 0xff0);
				a.And(w16, w16, 0xf);
				a.Orr(w14, w14, w16);
				a.Orr(w14, w14, Operand(w16, LSL, 6));
				a.Bind(&store_flags);
				a.Str(w14, vi(REG_STATUS_FLAG));
				a.Ldr(w14, MemOperand(x12, offsetof(fmacPipe, macflag)));
				a.Str(w14, vi(REG_MAC_FLAG));
				a.Add(w11, w11, 1);
				a.And(w11, w11, 3);
				a.Sub(w10, w10, 1);
				a.Str(w11, field(offsetof(VURegs, fmacreadpos)));
				a.Str(w10, field(offsetof(VURegs, fmaccount)));
				a.Cbnz(w10, &loop);
				a.Bind(&end);
			}
			{
				Label end;
				constexpr size_t offset = offsetof(VURegs, fdiv);
				a.Ldr(w10, field(offset + offsetof(fdivPipe, enable)));
				a.Cbz(w10, &end);
				a.Ldr(x10, field(offset + offsetof(fdivPipe, sCycle)));
				a.Ldr(w11, field(offset + offsetof(fdivPipe, Cycle)));
				a.Sub(x10, x9, x10);
				a.Cmp(x10, x11);
				a.B(lo, &end);
				a.Str(wzr, field(offset + offsetof(fdivPipe, enable)));
				a.Ldr(w10, field(offset + offsetof(fdivPipe, reg)));
				a.Str(w10, vi(REG_Q));
				a.Ldr(w10, vi(REG_STATUS_FLAG));
				a.And(w10, w10, 0xfcf);
				a.Ldr(w11, field(offset + offsetof(fdivPipe, statusflag)));
				a.And(w11, w11, 0xc30);
				a.Orr(w10, w10, w11);
				a.Str(w10, vi(REG_STATUS_FLAG));
				a.Bind(&end);
			}
			{
				Label end;
				constexpr size_t offset = offsetof(VURegs, efu);
				a.Ldr(w10, field(offset + offsetof(efuPipe, enable)));
				a.Cbz(w10, &end);
				a.Ldr(x10, field(offset + offsetof(efuPipe, sCycle)));
				a.Ldr(w11, field(offset + offsetof(efuPipe, Cycle)));
				a.Sub(x10, x9, x10);
				a.Cmp(x10, x11);
				a.B(lo, &end);
				a.Str(wzr, field(offset + offsetof(efuPipe, enable)));
				a.Ldr(w10, field(offset + offsetof(efuPipe, reg)));
				a.Str(w10, vi(REG_P));
				a.Bind(&end);
			}
			{
				Label loop, end;
				a.Ldr(w10, field(offsetof(VURegs, ialucount)));
				a.Cbz(w10, &end);
				a.Ldr(w11, field(offsetof(VURegs, ialureadpos)));
				a.Bind(&loop);
				a.Mov(w12, sizeof(ialuPipe));
				a.Madd(x12, x11, x12, x19);
				a.Add(x12, x12, offsetof(VURegs, ialu));
				a.Ldr(x13, MemOperand(x12, offsetof(ialuPipe, sCycle)));
				a.Ldr(w14, MemOperand(x12, offsetof(ialuPipe, Cycle)));
				a.Sub(x13, x9, x13);
				a.Cmp(x13, x14);
				a.B(lo, &end);
				a.Add(w11, w11, 1);
				a.And(w11, w11, 3);
				a.Sub(w10, w10, 1);
				a.Str(w11, field(offsetof(VURegs, ialureadpos)));
				a.Str(w10, field(offsetof(VURegs, ialucount)));
				a.Cbnz(w10, &loop);
				a.Bind(&end);
			}
		};

		// Standalone entry point: no branch wait (w2 = 0), drain the queues and
		// return, deliberately skipping VIBackupCycles (see above). DIV/SQRT/RSQRT
		// call this after their own stall has forced the clock forward, matching
		// the interpreter running _vuTestPipes again after _vuTestFDIVStalls.
		result.retire_queues = code + a.GetCursorOffset();
		a.Mov(w2, 0);
		a.Ldr(x9, field(offsetof(VURegs, cycle)));
		emit_retire_queues();
		a.Ret();

		a.Bind(&retire);
		emit_retire_queues(&retire_mid);
		{
			Label done;
			a.Ldrb(w10, field(offsetof(VURegs, VIBackupCycles)));
			a.Cbz(w10, &done);
			a.Ldr(w9, field(offsetof(VURegs, cycle)));
			a.Sub(w9, w9, w15);
			a.Uxtb(w9, w9);
			a.Cmp(w9, w10);
			a.Csel(w9, w9, w10, lo);
			a.Sub(w10, w10, w9);
			a.Strb(w10, field(offsetof(VURegs, VIBackupCycles)));
			a.Bind(&done);
		}
		a.Ret();
		a.FinalizeCode();
		result.size = a.GetSizeOfCodeGenerated();
		return result;
	}
} // namespace Arm64VU0
