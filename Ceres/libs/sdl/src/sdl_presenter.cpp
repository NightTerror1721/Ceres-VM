#include "sdl_presenter.h"

#include <ceres/devices/video/text_renderer.h>

namespace ceres::sdl
{
	SdlPresenter::SdlPresenter(std::shared_ptr<SdlWindow> window) : _window(std::move(window))
	{
		_window->setExposedHandler([this]
		{
			if (_shown == Shown::Text)
				drawText();
		});
	}

	SdlPresenter::~SdlPresenter()
	{
		_window->setExposedHandler({});
		if (_texture)
			SDL_DestroyTexture(_texture);
		if (_textTexture)
			SDL_DestroyTexture(_textTexture);
	}

	bool SdlPresenter::openWindow()
	{
		// Asked for by name: the pixel display shows from the start, and the window is the size it always was.
		_displayLive = true;
		return _window->open(SdlWindow::MaxWindowWidth, SdlWindow::MaxWindowHeight);
	}

	void SdlPresenter::present(const devices::DisplayDevice& display)
	{
		// The display is in use from the moment the program presents a frame of it - and then it is the
		// one shown, until a frame of text is presented after it.
		if (display.presentCount() != _displayPresents)
		{
			_displayPresents = display.presentCount();
			_displayLive = true;
			_shown = Shown::Pixels;
		}
		if (!_displayLive || _shown == Shown::Text)
			return;

		const u32 width = display.width();
		const u32 height = display.height();
		const auto pixels = display.frame();

		if (width == 0 || height == 0 || pixels.empty())
			return;
		if (!_window->open(SdlWindow::MaxWindowWidth, SdlWindow::MaxWindowHeight))
			return;

		// Only a program that draws pixels is taken to be a game: a text interface needs its pointer to close
		// the window with.
		_window->captureMouse();
		_shown = Shown::Pixels;

		SDL_Renderer* renderer = _window->renderer();
		if (width != _textureWidth || height != _textureHeight)
		{
			if (_texture)
				SDL_DestroyTexture(_texture);
			_texture = SDL_CreateTexture(renderer, SDL_PIXELFORMAT_XRGB8888, SDL_TEXTUREACCESS_STREAMING, static_cast<int>(width), static_cast<int>(height));
			_textureWidth = width;
			_textureHeight = height;

			// Follow the display: size the window to the display's resolution at the largest integer scale that
			// still fits, so the pixels stay crisp and the window never outgrows its starting size.
			_window->fit(width, height);
		}

		if (!_texture)
			return;
		SDL_SetRenderLogicalPresentation(renderer, 0, 0, SDL_LOGICAL_PRESENTATION_DISABLED);

		// The display stores 0x00RRGGBB per pixel: SDL calls that packed order SDL_PIXELFORMAT_XRGB8888.
		SDL_UpdateTexture(_texture, nullptr, pixels.data(), static_cast<int>(width * sizeof(u32)));
		SDL_RenderClear(renderer);
		SDL_RenderTexture(renderer, _texture, nullptr, nullptr);
		SDL_RenderPresent(renderer);
	}

	bool SdlPresenter::presentText(const devices::FramebufferDevice::Frame& frame)
	{
		const u32 width = devices::TextRenderer::imageWidth(frame);
		const u32 height = devices::TextRenderer::imageHeight(frame);
		if (width == 0 || height == 0)
			return true;   // nothing to show is shown
		if (!_window->open(static_cast<int>(width), static_cast<int>(height)))
			return false;

		devices::TextRenderer::render(frame, _textPixels);

		if (width != _textWidth || height != _textHeight)
		{
			if (_textTexture)
				SDL_DestroyTexture(_textTexture);
			_textTexture = SDL_CreateTexture(_window->renderer(), SDL_PIXELFORMAT_XRGB8888, SDL_TEXTUREACCESS_STREAMING, static_cast<int>(width), static_cast<int>(height));
			if (_textTexture)
				SDL_SetTextureScaleMode(_textTexture, SDL_SCALEMODE_NEAREST);   // crisp cells, at any window size
			_textWidth = width;
			_textHeight = height;
			_window->fit(width, height);
		}
		if (!_textTexture)
			return false;

		SDL_UpdateTexture(_textTexture, nullptr, _textPixels.data(), static_cast<int>(width * sizeof(u32)));
		_shown = Shown::Text;
		drawText();
		return true;
	}

	void SdlPresenter::drawText()
	{
		SDL_Renderer* renderer = _window->renderer();
		if (!_textTexture || !renderer)
			return;
		SDL_SetRenderLogicalPresentation(renderer, static_cast<int>(_textWidth), static_cast<int>(_textHeight), SDL_LOGICAL_PRESENTATION_LETTERBOX);
		SDL_SetRenderDrawColor(renderer, 0, 0, 0, 255);
		SDL_RenderClear(renderer);
		SDL_RenderTexture(renderer, _textTexture, nullptr, nullptr);
		SDL_RenderPresent(renderer);
	}
}
