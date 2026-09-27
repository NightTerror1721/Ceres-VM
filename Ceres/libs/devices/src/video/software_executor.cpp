#include <ceres/devices/video/software_executor.h>

#include <algorithm>

namespace ceres::devices::video
{
	void SoftwareExecutor::compose(const ScanoutState& state, const vm::Vram&, VideoFrame& frame)
	{
		frame.width = state.width;
		frame.height = state.height;
		frame.number = state.frameCounter;
		frame.pixels.assign(static_cast<usize>(state.width) * state.height, 0u);
		if (!state.displayOn)
			return;

		std::fill(frame.pixels.begin(), frame.pixels.end(), state.background & 0x00FFFFFFu);
	}
}
