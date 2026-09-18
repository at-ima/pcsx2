// SPDX-FileCopyrightText: 2002-2026 PCSX2 Dev Team
// SPDX-License-Identifier: GPL-3.0+

#pragma once

#include "VUops.h"
#include "DebugTools/Debug.h"

// Shared retirement rules, visible to the ARM64 preparation helpers so the
// compiler can specialize accesses to VU1 without duplicating timing semantics.
namespace VUPipeline
{
	__forceinline_odr bool FlushFMAC(VURegs* VU)
	{
		bool didflush = false;

		VUM_LOG("Flushing FMACs");

		for (int i = VU->fmacreadpos; VU->fmaccount > 0; i = (i + 1) & 3)
		{
			if ((VU->cycle - VU->fmac[i].sCycle) < VU->fmac[i].Cycle)
			{
				VUM_LOG("Not flushing FMAC pipe[%d] (macflag=%x clipflag=%x statusflag=%x) r %d w %d", i, VU->fmac[i].macflag, VU->fmac[i].clipflag, VU->fmac[i].statusflag, VU->fmacreadpos, VU->fmacwritepos);
				return didflush;
			}

			VUM_LOG("flushing FMAC pipe[%d] (macflag=%x clipflag=%x statusflag=%x) r %d w %d", i, VU->fmac[i].macflag, VU->fmac[i].clipflag, VU->fmac[i].statusflag, VU->fmacreadpos, VU->fmacwritepos);

			// Clip flags (Affected by CLIP instruction)
			if (VU->fmac[i].flagreg & (1 << REG_CLIP_FLAG))
				VU->VI[REG_CLIP_FLAG].UL = VU->fmac[i].clipflag;

			// Normal FMAC instructoins only affectx Z/S/I/O, D/I are modified only by FDIV instructions
			// Sticky flags (Affected by FSSET)
			if (VU->fmac[i].flagreg & (1 << REG_STATUS_FLAG))
				VU->VI[REG_STATUS_FLAG].UL = (VU->VI[REG_STATUS_FLAG].UL & 0x30) | (VU->fmac[i].statusflag & 0xFC0) | (VU->fmac[i].statusflag & 0xF);
			else
				VU->VI[REG_STATUS_FLAG].UL = (VU->VI[REG_STATUS_FLAG].UL & 0xFF0) | (VU->fmac[i].statusflag & 0xF) | ((VU->fmac[i].statusflag & 0xF) << 6);
			VU->VI[REG_MAC_FLAG].UL = VU->fmac[i].macflag;

			VU->fmacreadpos = (VU->fmacreadpos + 1) & 3;
			VU->fmaccount--;

			didflush = true;
		}

		return didflush;
	}

	__forceinline_odr bool FlushIALU(VURegs* VU)
	{
		bool didflush = false;

		VUM_LOG("Flushing ALU stalls");

		for (int i = VU->ialureadpos; VU->ialucount > 0; i = (i + 1) & 3)
		{
			if ((VU->cycle - VU->ialu[i].sCycle) < VU->ialu[i].Cycle)
				return didflush;

			VU->ialureadpos = (VU->ialureadpos + 1) & 3;
			VU->ialucount--;
			didflush = true;
		}
		return didflush;
	}

	__forceinline_odr bool FlushFDIV(VURegs* VU)
	{
		if (VU->fdiv.enable == 0)
			return false;

		if ((VU->cycle - VU->fdiv.sCycle) >= VU->fdiv.Cycle)
		{
			VUM_LOG("flushing FDIV pipe");

			VU->fdiv.enable = 0;
			VU->VI[REG_Q].UL = VU->fdiv.reg.UL;
			// FDIV only affects D/I
			VU->VI[REG_STATUS_FLAG].UL = (VU->VI[REG_STATUS_FLAG].UL & 0xFCF) | (VU->fdiv.statusflag & 0xC30);

			return true;
		}
		return false;
	}

	__forceinline_odr bool FlushEFU(VURegs* VU)
	{
		if (VU->efu.enable == 0)
			return false;

		if ((VU->cycle - VU->efu.sCycle) >= VU->efu.Cycle)
		{
			VUM_LOG("flushing EFU pipe");

			VU->efu.enable = 0;
			VU->VI[REG_P].UL = VU->efu.reg.UL;

			return true;
		}

		return false;
	}

	__forceinline_odr void Retire(VURegs* VU)
	{
		// Each flush retires all ready entries without advancing the cycle or adding
		// work to another pipeline. Keep the flag writeback order, but only visit once.
		FlushFMAC(VU);
		FlushFDIV(VU);
		FlushEFU(VU);
		FlushIALU(VU);

		if (VU == &VU1)
		{
			if (VU1.xgkickenable)
			{
				_vuXGKICKTransfer((VU1.cycle - VU1.xgkicklastcycle) - 1, false);
			}
		}
	}

} // namespace VUPipeline
