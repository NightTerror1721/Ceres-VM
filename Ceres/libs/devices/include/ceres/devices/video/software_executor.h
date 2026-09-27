#pragma once

// The GPU's reference executor: the scanout in plain C++, line by line, in the fixed order of plan/v2 SPEC 7.2 -
// the background colour, then the bitmap plane, then the text plane. What it produces is what every other executor
// has to match.

#include <ceres/devices/video/gpu_executor.h>

namespace ceres::devices::video
{
	class SoftwareExecutor final : public GpuExecutor
	{
	public:
		void compose(const ScanoutState& state, const vm::Vram& vram, VideoFrame& frame) override;
	};
}
