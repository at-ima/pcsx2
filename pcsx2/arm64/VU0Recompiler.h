// SPDX-FileCopyrightText: 2026 PCSX2 Dev Team
// SPDX-License-Identifier: GPL-3.0+

#pragma once

#include "VUmicro.h"

// VU0 always runs synchronously on the EE thread, so this provider carries
// none of Arm64VU1Recompiler's MTVU/XGKICK-packet handling. It reuses the
// interpreter's architectural/pipeline state, matching that provider.
class Arm64VU0Recompiler final : public BaseVUmicroCPU
{
public:
	Arm64VU0Recompiler();
	const char* GetShortName() const override { return "armVU0"; }
	const char* GetLongName() const override { return "ARM64 VU0 Block Recompiler"; }
	void Reserve();
	void Shutdown() override;
	void Reset() override;
	void SetStartPC(u32 pc) override;
	void Step() override;
	void Execute(u32 cycles) override;
	void Clear(u32 addr, u32 size) override;
	size_t GetCommittedCache() const override;
};

extern Arm64VU0Recompiler CpuArm64VU0;
