#pragma once

#include "command.h"
#include <functional>
#include <iosfwd>

namespace ceres::driver
{
	// Host-owned streams keep the driver usable by terminals, GUIs and tests alike.
	struct HostServices
	{
		std::istream* input = nullptr;
		std::ostream* output = nullptr;
		std::ostream* diagnostics = nullptr;
	};

	int execute(const Command& command, HostServices services);
	int runCommandLine(int argc, char* const argv[], HostServices services);
}
