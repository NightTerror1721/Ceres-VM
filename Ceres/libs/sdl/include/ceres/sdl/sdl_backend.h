#pragma once

// The SDL3 windowed host, hidden behind a plain factory so no SDL type leaks into a header that
// a non-SDL build would include. createSdlBackend() either returns a ready backend or throws
// std::runtime_error with what SDL reported.

#include <ceres/driver/host_backend.h>
#include <memory>

namespace ceres::sdl
{
	std::unique_ptr<driver::HostBackend> createSdlBackend();
}
