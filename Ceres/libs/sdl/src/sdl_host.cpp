#include <ceres/sdl/sdl_host.h>

#include "sdl_audio.h"
#include "sdl_presenter.h"
#include "sdl_window.h"

namespace ceres::sdl
{
	driver::WindowHost createSdlHost()
	{
		auto window = std::make_shared<SdlWindow>();
		driver::WindowHost host;
		host.video = std::make_shared<SdlPresenter>(window);
		host.input = std::move(window);
		host.audio = std::make_shared<SdlAudio>();
		return host;
	}
}
