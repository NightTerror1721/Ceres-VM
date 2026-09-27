#pragma once

// The window itself and everything that comes in through it: keys, text, the mouse, the gamepad, dropped files.
// It is the host's input (HostInput); the presenter (sdl_presenter.h) draws on it through its renderer.

#include <ceres/driver/host_input.h>

#define SDL_MAIN_HANDLED
#include <SDL3/SDL.h>

#include <array>
#include <functional>

namespace ceres::sdl
{
	class SdlWindow final : public driver::HostInput
	{
	public:
		// The window's starting size, and the largest it is allowed to grow to when it follows what it shows.
		static inline constexpr int MaxWindowWidth = 1280;
		static inline constexpr int MaxWindowHeight = 720;

	private:
		// Nothing of SDL is started until it is needed: a machine with a screen opens its window when the
		// program first shows a frame, and a program that never does opens none (see open()).
		bool _videoStarted = false;
		bool _videoFailed = false;
		SDL_Window* _window = nullptr;
		SDL_Renderer* _renderer = nullptr;
		bool _mouseCaptured = false;
		u8 _buttons = 0;                  // wheel events carry no button mask, so the last known one is kept
		std::array<i32, 7> _padState{};   // the gamepad as last handed over: only a change goes to the machine
		bool _padSent = false;
		SDL_Gamepad* _gamepad = nullptr;
		std::function<void(const std::filesystem::path&)> _dropHandler;
		std::function<void()> _exposed;

	public:
		SdlWindow() = default;
		SdlWindow(const SdlWindow&) = delete;
		SdlWindow& operator=(const SdlWindow&) = delete;
		~SdlWindow() override;

		bool pump(driver::InputSink& input) override;
		void setFileDropHandler(std::function<void(const std::filesystem::path&)> handler) override { _dropHandler = std::move(handler); }

		// Starts SDL and opens the window, once. `width` and `height` are what the window will show, so it opens at
		// the size it will have. If that cannot be done (no display: a server, a terminal session) it says so once
		// on standard error and never tries again.
		bool open(int width, int height);
		bool isOpen() const noexcept { return _window != nullptr; }

		SDL_Window* window() const noexcept { return _window; }
		SDL_Renderer* renderer() const noexcept { return _renderer; }

		// Sizes the window to show width x height at the largest whole scale that fits the starting size.
		void fit(u32 width, u32 height);

		// A game wants unbounded mouse deltas, not a cursor that stops at the window edge.
		void captureMouse();

		// Called from pump() when what the window shows is gone or the wrong size, so it is drawn again.
		void setExposedHandler(std::function<void()> handler) { _exposed = std::move(handler); }
	};
}
