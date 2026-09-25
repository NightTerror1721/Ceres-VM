#include <ceres/driver/driver.h>
#include <iostream>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

#ifdef CERES_HAS_SDL
#include <ceres/sdl/sdl_backend.h>
#endif

#ifdef _WIN32
namespace
{
	// A program's text is UTF-8 (the standard library's, and the text framebuffer's frames), so a Windows console
	// shows it as UTF-8 while the machine runs, and gets back the code page it had.
	struct Utf8Console
	{
		UINT previous = GetConsoleOutputCP();
		Utf8Console() { if (previous != 0) SetConsoleOutputCP(CP_UTF8); }
		~Utf8Console() { if (previous != 0) SetConsoleOutputCP(previous); }
		Utf8Console(const Utf8Console&) = delete;
		Utf8Console& operator=(const Utf8Console&) = delete;
	};
}
#endif

int main(int argc, char** argv)
{
#ifdef _WIN32
	const Utf8Console console;
#endif
	ceres::driver::HostBackendFactory window;
#ifdef CERES_HAS_SDL
	window = &ceres::sdl::createSdlBackend;
#endif
	return ceres::driver::runCommandLine(argc, argv, { &std::cin, &std::cout, &std::cerr }, std::move(window));
}
