#include "sdl_presenter.h"

#include <cmath>
#include <format>

namespace ceres::sdl
{
	SdlPresenter::SdlPresenter(std::shared_ptr<SdlWindow> window) : _window(std::move(window))
	{
		// What the window showed is gone (uncovered, resized): draw the last frame again.
		_window->setExposedHandler([this] { if (_hasFrame) draw(); });
	}

	SdlPresenter::~SdlPresenter()
	{
		_window->setExposedHandler({});
		if (_texture)
			SDL_DestroyTexture(_texture);
	}

	bool SdlPresenter::openWindow(u32 width, u32 height)
	{
		return _window->open(static_cast<int>(width), static_cast<int>(height));
	}

	void SdlPresenter::present(const devices::video::VideoFrame& frame)
	{
		if (frame.width == 0 || frame.height == 0 || frame.pixels.size() < static_cast<usize>(frame.width) * frame.height)
			return;
		if (!_window->open(static_cast<int>(frame.width), static_cast<int>(frame.height)))
			return;

		SDL_Renderer* renderer = _window->renderer();
		if (frame.width != _textureWidth || frame.height != _textureHeight)
		{
			if (_texture)
				SDL_DestroyTexture(_texture);
			// The frame is 0x00RRGGBB per pixel: SDL calls that packed order SDL_PIXELFORMAT_XRGB8888.
			_texture = SDL_CreateTexture(renderer, SDL_PIXELFORMAT_XRGB8888, SDL_TEXTUREACCESS_STREAMING, static_cast<int>(frame.width), static_cast<int>(frame.height));
			if (_texture)
				SDL_SetTextureScaleMode(_texture, SDL_SCALEMODE_NEAREST);   // crisp pixels at any scale
			_textureWidth = frame.width;
			_textureHeight = frame.height;
			// A new resolution: the window follows it, at the largest whole scale that fits its starting size.
			_window->fit(frame.width, frame.height);
		}
		if (!_texture)
			return;

		SDL_UpdateTexture(_texture, nullptr, frame.pixels.data(), static_cast<int>(frame.width * sizeof(u32)));
		_hasFrame = true;
		draw();
	}

	void SdlPresenter::draw()
	{
		SDL_Renderer* renderer = _window->renderer();
		if (!_texture || !renderer)
			return;
		SDL_SetRenderLogicalPresentation(renderer, static_cast<int>(_textureWidth), static_cast<int>(_textureHeight), SDL_LOGICAL_PRESENTATION_INTEGER_SCALE);
		SDL_SetRenderDrawColor(renderer, 0, 0, 0, 255);
		SDL_RenderClear(renderer);
		SDL_RenderTexture(renderer, _texture, nullptr, nullptr);
		SDL_RenderPresent(renderer);
	}

	// The status bar is the window's title: "Ceres - standard - 100%", and once the program is done, how it ended.
	void SdlPresenter::setStatus(const driver::HostStatus& status)
	{
		std::string title = "Ceres - " + status.profile;
		if (status.exitCode)
			title += std::format(" - [terminado: c\xC3\xB3" "digo {}]", *status.exitCode);
		else if (status.speed > 0.0)
			title += std::format(" - {}%", static_cast<long long>(std::lround(status.speed * 100.0)));
		_window->setTitle(title);
	}
}
