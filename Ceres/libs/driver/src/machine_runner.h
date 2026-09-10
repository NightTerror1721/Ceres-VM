#pragma once

#include <ceres/driver/machine.h>
#include <ceres/core/format/debug_info.h>
#include <iosfwd>

namespace ceres::driver
{
	struct HostServices;
	int runMachine(const fmt::Program& program, usize memorySize, const fmt::DebugInfo* profileInfo,
		const std::filesystem::path& diskImage, HostServices services);
}
