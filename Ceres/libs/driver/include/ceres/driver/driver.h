#pragma once

#include "command.h"
#include "host_backend.h"
#include <functional>
#include <iosfwd>
#include <memory>

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

	// The driver never links SDL itself. A windowed host (the CLI) registers a factory that the
	// driver calls when `run --window` is asked for; empty (the default) means "built without a
	// windowed host", and `--window` then reports an error instead of doing nothing.
	using HostBackendFactory = std::function<std::unique_ptr<HostBackend>()>;

	int execute(const Command& command, HostServices services, HostBackendFactory windowBackend = {});
	int runCommandLine(int argc, char* const argv[], HostServices services, HostBackendFactory windowBackend = {});
}
