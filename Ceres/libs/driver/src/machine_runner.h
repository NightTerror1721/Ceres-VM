#pragma once

#include <ceres/driver/command.h>
#include <ceres/driver/machine.h>
#include <ceres/driver/host_backend.h>
#include <ceres/core/format/debug_info.h>
#include <ceres/vm/ceresvm.h>
#include <iosfwd>

namespace ceres::driver
{
	struct HostServices;
	int runMachine(const fmt::Program& program, usize memorySize, const fmt::DebugInfo* profileInfo,
		const std::filesystem::path& diskImage, const std::vector<PortAttachment>& ports, vm::ProgramArguments arguments,
		const std::filesystem::path& hostDirectory, HostServices services, HostBackend* backend = nullptr);
}
