#pragma once

// What the window shows: the machine's frames, drawn with the window's renderer (sdl_window.h).

#include "sdl_window.h"

#include <ceres/driver/video_output.h>

#include <memory>
#include <vector>

namespace ceres::sdl
{
	class SdlPresenter final : public driver::VideoOutput
	{
	private:
		std::shared_ptr<SdlWindow> _window;
		SDL_Texture* _texture = nullptr;      // the pixel display
		u32 _textureWidth = 0;
		u32 _textureHeight = 0;
		SDL_Texture* _textTexture = nullptr;  // the text framebuffer, drawn by devices::TextRenderer
		u32 _textWidth = 0;
		u32 _textHeight = 0;
		std::vector<u32> _textPixels;

		// Which of the two the window shows: the one the program presented most recently. The pixel display
		// is shown live (every slice, from its buffer) once it is in use: from its first presented frame, or
		// from the start when the window was asked for by name, as it always has been.
		enum class Shown { Nothing, Text, Pixels };
		Shown _shown = Shown::Nothing;
		bool _displayLive = false;
		u64 _displayPresents = 0;

		// Draws the text texture as it is, letterboxed at whatever size the window has.
		void drawText();

	public:
		explicit SdlPresenter(std::shared_ptr<SdlWindow> window);
		SdlPresenter(const SdlPresenter&) = delete;
		SdlPresenter& operator=(const SdlPresenter&) = delete;
		~SdlPresenter() override;

		void present(const devices::DisplayDevice& display) override;
		bool showsText() const noexcept override { return true; }
		bool presentText(const devices::FramebufferDevice::Frame& frame) override;
		bool openWindow() override;
		bool windowOpen() const noexcept override { return _window->isOpen(); }
	};
}
