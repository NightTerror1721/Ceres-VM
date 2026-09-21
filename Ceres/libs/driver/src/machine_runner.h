#pragma once

#include <ceres/driver/command.h>
#include <ceres/driver/machine.h>
#include <ceres/driver/host_backend.h>
#include <ceres/core/format/debug_info.h>
#include <iosfwd>

namespace ceres::driver
{
	struct HostServices;
	int runMachine(const fmt::Program& program, usize memorySize, const fmt::DebugInfo* profileInfo,
		const std::filesystem::path& diskImage, const std::vector<PortAttachment>& ports, HostServices services, HostBackend* backend = nullptr);
}
