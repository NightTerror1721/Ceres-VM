#pragma once

#include <ceres/core/format/debug_info.h>
#include <ceres/core/format/program.h>
#include <ceres/core/base/types.h>

#include <filesystem>
#include <iosfwd>

namespace ceres::driver
{
	struct HostServices;

	// The common machine assembly used by terminal and future graphical hosts. The device layout is
	// deliberately owned here so front ends differ only in how they provide input and consume output.
	int runMachine(const fmt::Program& program, usize memorySize, const fmt::DebugInfo* profileInfo,
		const std::filesystem::path& diskImage, HostServices services);
}
