#pragma once

// The SDL3 windowed host, hidden behind a plain factory so no SDL type leaks into a header that a non-SDL build
// would include. createSdlHost() returns its three parts - the window's input, the presenter that draws in the
// window, and the speakers - or throws std::runtime_error with what SDL reported.

#include <ceres/driver/driver.h>

namespace ceres::sdl
{
	driver::WindowHost createSdlHost();
}
