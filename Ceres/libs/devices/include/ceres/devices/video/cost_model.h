#pragma once

// What the GPU's work costs, in GPU cycles, and how long that is on the CPU's clock (plan/v2 SPEC 3.1). The
// scanout composes the screen for free (SPEC 7.2); the engines are what pay. Integer arithmetic, rounded up, so a
// run lands on the same cycle on every host.

#include <ceres/core/base/types.h>

namespace ceres::devices::video
{
	struct CostModel
	{
		// The copy engine: a fixed start, then a 32-bit word a cycle for a copy (it reads and writes) and two for a fill.
		static inline constexpr u64 CopySetup = 16;
		static inline constexpr u64 CopyBytesPerCycle = 4;
		static inline constexpr u64 FillBytesPerCycle = 8;

		static constexpr u64 copyCycles(u64 bytes) noexcept { return CopySetup + (bytes + CopyBytesPerCycle - 1) / CopyBytesPerCycle; }
		static constexpr u64 fillCycles(u64 bytes) noexcept { return CopySetup + (bytes + FillBytesPerCycle - 1) / FillBytesPerCycle; }

		// GPU cycles as CPU cycles, rounded up: the work is done on the first CPU cycle at or after it ends.
		static constexpr u64 toCpuCycles(u64 gpuCycles, u64 cpuHz, u64 gpuHz) noexcept
		{
			if (gpuHz == 0)
				return gpuCycles;
			// In two parts, so the product never overflows: whole GPU seconds, then the rest.
			const u64 seconds = gpuCycles / gpuHz;
			const u64 rest = gpuCycles % gpuHz;
			return seconds * cpuHz + (rest * cpuHz + gpuHz - 1) / gpuHz;
		}
	};
}
