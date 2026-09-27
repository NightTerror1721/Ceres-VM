#pragma once

// What the window shows: the GPU's frames, drawn with the window's renderer (sdl_window.h) at the largest whole scale
// that fits, black bars round what is left (plan/v2 F5.5).

#include "sdl_window.h"

#include <ceres/driver/video_output.h>

#include <memory>

namespace ceres::sdl
{
	class SdlPresenter final : public driver::VideoOutput
	{
	private:
		std::shared_ptr<SdlWindow> _window;
		SDL_Texture* _texture = nullptr;   // streaming, the frame's size
		u32 _textureWidth = 0;
		u32 _textureHeight = 0;
		bool _hasFrame = false;

		// Draws the texture as it is, at the largest whole scale that fits the window.
		void draw();

	public:
		explicit SdlPresenter(std::shared_ptr<SdlWindow> window);
		SdlPresenter(const SdlPresenter&) = delete;
		SdlPresenter& operator=(const SdlPresenter&) = delete;
		~SdlPresenter() override;

		bool openWindow(u32 width, u32 height) override;
		void present(const devices::video::VideoFrame& frame) override;
		bool windowOpen() const noexcept override { return _window->isOpen(); }
		void setFullscreen(bool fullscreen) override { _window->setFullscreen(fullscreen); }
		void setStatus(const driver::HostStatus& status) override;
	};
}
