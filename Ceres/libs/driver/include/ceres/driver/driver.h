#pragma once

#include "command.h"
#include <iosfwd>

namespace ceres::driver
{
	// Host-owned streams keep the command adapter usable by terminals and tests. Native GUI hosts
	// should invoke execute() with their own stream adapters or use the lower-level VM libraries.
	struct HostServices
	{
		std::istream* input = nullptr;
		std::ostream* output = nullptr;
		std::ostream* diagnostics = nullptr;
	};

	int execute(const Command& command, HostServices services);
	int runCommandLine(int argc, char* const argv[], HostServices services);
}
