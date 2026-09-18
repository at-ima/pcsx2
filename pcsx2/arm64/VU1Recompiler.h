// SPDX-FileCopyrightText: 2026 PCSX2 Dev Team
// SPDX-License-Identifier: GPL-3.0+

#pragma once

#include "VUmicro.h"

// Uses the interpreter's architectural/pipeline state. Normal XGKICK uses
// microVU's delayed packet policy; XgKickHack retains cycle-based transfers.
// This provider does not implement microVU's separate-thread execution protocol.
class Arm64VU1Recompiler final : public BaseVUmicroCPU
{
public:
	Arm64VU1Recompiler();
	const char* GetShortName() const override { return "armVU1"; }
	const char* GetLongName() const override { return "ARM64 VU1 Block Recompiler"; }
	void Reserve();
	void Shutdown() override;
	void Reset() override;
	void SetStartPC(u32 pc) override;
	void Step() override;
	void Execute(u32 cycles) override;
	void Clear(u32 addr, u32 size) override;
	size_t GetCommittedCache() const override;
};

extern Arm64VU1Recompiler CpuArm64VU1;
