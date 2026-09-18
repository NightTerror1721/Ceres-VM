#include <ceres/driver/driver.h>
#include <iostream>

#ifdef CERES_HAS_SDL
#include <ceres/sdl/sdl_backend.h>
#endif

int main(int argc, char** argv)
{
	ceres::driver::HostBackendFactory window;
#ifdef CERES_HAS_SDL
	window = &ceres::sdl::createSdlBackend;
#endif
	return ceres::driver::runCommandLine(argc, argv, { &std::cin, &std::cout, &std::cerr }, std::move(window));
}
