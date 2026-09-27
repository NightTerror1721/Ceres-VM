#include "sdl_window.h"

#include <ceres/devices/input/gamepad.h>
#include <ceres/devices/input/mouse.h>

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <string>
#include <string_view>

namespace ceres::sdl
{
	namespace
	{
		u8 toButtons(Uint32 sdlState)
		{
			u8 buttons = 0;
			if (sdlState & SDL_BUTTON_LMASK) buttons |= devices::MouseDevice::ButtonLeft;
			if (sdlState & SDL_BUTTON_RMASK) buttons |= devices::MouseDevice::ButtonRight;
			if (sdlState & SDL_BUTTON_MMASK) buttons |= devices::MouseDevice::ButtonMiddle;
			return buttons;
		}

		u16 toGamepadButtons(SDL_Gamepad* gamepad)
		{
			using devices::GamepadDevice;
			u16 buttons = 0;
			if (SDL_GetGamepadButton(gamepad, SDL_GAMEPAD_BUTTON_SOUTH)) buttons |= GamepadDevice::ButtonSouth;
			if (SDL_GetGamepadButton(gamepad, SDL_GAMEPAD_BUTTON_EAST)) buttons |= GamepadDevice::ButtonEast;
			if (SDL_GetGamepadButton(gamepad, SDL_GAMEPAD_BUTTON_WEST)) buttons |= GamepadDevice::ButtonWest;
			if (SDL_GetGamepadButton(gamepad, SDL_GAMEPAD_BUTTON_NORTH)) buttons |= GamepadDevice::ButtonNorth;
			if (SDL_GetGamepadButton(gamepad, SDL_GAMEPAD_BUTTON_BACK)) buttons |= GamepadDevice::ButtonBack;
			if (SDL_GetGamepadButton(gamepad, SDL_GAMEPAD_BUTTON_GUIDE)) buttons |= GamepadDevice::ButtonGuide;
			if (SDL_GetGamepadButton(gamepad, SDL_GAMEPAD_BUTTON_START)) buttons |= GamepadDevice::ButtonStart;
			if (SDL_GetGamepadButton(gamepad, SDL_GAMEPAD_BUTTON_LEFT_STICK)) buttons |= GamepadDevice::ButtonLeftStick;
			if (SDL_GetGamepadButton(gamepad, SDL_GAMEPAD_BUTTON_RIGHT_STICK)) buttons |= GamepadDevice::ButtonRightStick;
			if (SDL_GetGamepadButton(gamepad, SDL_GAMEPAD_BUTTON_LEFT_SHOULDER)) buttons |= GamepadDevice::ButtonLeftShoulder;
			if (SDL_GetGamepadButton(gamepad, SDL_GAMEPAD_BUTTON_RIGHT_SHOULDER)) buttons |= GamepadDevice::ButtonRightShoulder;
			if (SDL_GetGamepadButton(gamepad, SDL_GAMEPAD_BUTTON_DPAD_UP)) buttons |= GamepadDevice::ButtonDpadUp;
			if (SDL_GetGamepadButton(gamepad, SDL_GAMEPAD_BUTTON_DPAD_DOWN)) buttons |= GamepadDevice::ButtonDpadDown;
			if (SDL_GetGamepadButton(gamepad, SDL_GAMEPAD_BUTTON_DPAD_LEFT)) buttons |= GamepadDevice::ButtonDpadLeft;
			if (SDL_GetGamepadButton(gamepad, SDL_GAMEPAD_BUTTON_DPAD_RIGHT)) buttons |= GamepadDevice::ButtonDpadRight;
			return buttons;
		}
	}

	SdlWindow::~SdlWindow()
	{
		if (_gamepad)
			SDL_CloseGamepad(_gamepad);
		if (_renderer)
			SDL_DestroyRenderer(_renderer);
		if (_window)
			SDL_DestroyWindow(_window);
		if (_videoStarted)
		{
			SDL_QuitSubSystem(SDL_INIT_VIDEO | SDL_INIT_GAMEPAD);
			if (SDL_WasInit(0) == 0)
				SDL_Quit();   // the last part of the host to let go of SDL shuts it down, as the one backend did
		}
	}

	bool SdlWindow::pump(driver::InputSink& input)
	{
		if (!_videoStarted)
			return true;   // no window yet, so nothing can have happened to it

		bool exposed = false;
		SDL_Event event;
		while (SDL_PollEvent(&event))
		{
			switch (event.type)
			{
				case SDL_EVENT_QUIT:
					return false;

				case SDL_EVENT_DROP_FILE:
					// A file dropped on the window is a medium plugged in. The path is UTF-8.
					if (_dropHandler && event.drop.data != nullptr)
					{
						const char* utf8 = event.drop.data;
						_dropHandler(std::filesystem::path(std::u8string(reinterpret_cast<const char8_t*>(utf8), reinterpret_cast<const char8_t*>(utf8) + std::strlen(utf8))));
					}
					break;

				case SDL_EVENT_KEY_DOWN:
					// Ctrl+Q closes the machine, and the window's own close button does too. Escape is
					// the program's: a menu cancels with it, so it must not end the run.
					if (event.key.scancode == SDL_SCANCODE_Q && (event.key.mod & SDL_KMOD_CTRL) != 0)
						return false;
					input.key(static_cast<u32>(event.key.scancode), true);
					break;

				case SDL_EVENT_KEY_UP:
					input.key(static_cast<u32>(event.key.scancode), false);
					break;

				case SDL_EVENT_TEXT_INPUT:
					input.text(std::string_view(event.text.text));
					break;

				case SDL_EVENT_MOUSE_MOTION:
					input.mouse(static_cast<i32>(event.motion.xrel), static_cast<i32>(event.motion.yrel), toButtons(event.motion.state), 0);
					break;

				case SDL_EVENT_MOUSE_BUTTON_DOWN:
				case SDL_EVENT_MOUSE_BUTTON_UP:
					_buttons = toButtons(SDL_GetMouseState(nullptr, nullptr));
					input.mouse(0, 0, _buttons, 0);
					break;

				case SDL_EVENT_MOUSE_WHEEL:
					input.mouse(0, 0, _buttons, static_cast<i8>(event.wheel.y));
					break;

				case SDL_EVENT_WINDOW_EXPOSED:
				case SDL_EVENT_WINDOW_PIXEL_SIZE_CHANGED:
					exposed = true;   // what the window shows is gone or the wrong size
					break;

				case SDL_EVENT_GAMEPAD_ADDED:
					if (!_gamepad)
						_gamepad = SDL_OpenGamepad(event.gdevice.which);
					break;

				case SDL_EVENT_GAMEPAD_REMOVED:
					if (_gamepad && SDL_GetGamepadID(_gamepad) == event.gdevice.which)
					{
						SDL_CloseGamepad(_gamepad);
						_gamepad = nullptr;
					}
					break;

				default:
					break;
			}
		}

		// A gamepad is state, not a queue of events, so it is polled every pump - and handed over only when it
		// changed, so a recording holds its moves rather than a copy of it every slice.
		if (_gamepad)
		{
			const std::array<i32, 7> state{ toGamepadButtons(_gamepad),
				SDL_GetGamepadAxis(_gamepad, SDL_GAMEPAD_AXIS_LEFTX),
				SDL_GetGamepadAxis(_gamepad, SDL_GAMEPAD_AXIS_LEFTY),
				SDL_GetGamepadAxis(_gamepad, SDL_GAMEPAD_AXIS_RIGHTX),
				SDL_GetGamepadAxis(_gamepad, SDL_GAMEPAD_AXIS_RIGHTY),
				SDL_GetGamepadAxis(_gamepad, SDL_GAMEPAD_AXIS_LEFT_TRIGGER),
				SDL_GetGamepadAxis(_gamepad, SDL_GAMEPAD_AXIS_RIGHT_TRIGGER) };
			if (!_padSent || state != _padState)
			{
				_padState = state;
				_padSent = true;
				input.gamepad(static_cast<u16>(state[0]), static_cast<i16>(state[1]), static_cast<i16>(state[2]),
					static_cast<i16>(state[3]), static_cast<i16>(state[4]), static_cast<u16>(state[5]), static_cast<u16>(state[6]));
			}
		}

		if (exposed && _exposed)
			_exposed();
		return true;
	}

	bool SdlWindow::open(int width, int height)
	{
		if (_window)
			return true;
		if (_videoFailed)
			return false;

		auto fail = [this](const char* what)
		{
			std::fprintf(stderr, "ceres: no window (%s: %s); showing text on the terminal instead\n", what, SDL_GetError());
			_videoFailed = true;
			if (_renderer) { SDL_DestroyRenderer(_renderer); _renderer = nullptr; }
			if (_window) { SDL_DestroyWindow(_window); _window = nullptr; }
			return false;
		};

		if (!SDL_InitSubSystem(SDL_INIT_VIDEO | SDL_INIT_GAMEPAD))
			return fail("SDL_Init");
		_videoStarted = true;

		const int scale = std::max(1, std::min(MaxWindowWidth / std::max(width, 1), MaxWindowHeight / std::max(height, 1)));
		_window = SDL_CreateWindow("Ceres", width * scale, height * scale, SDL_WINDOW_RESIZABLE);
		if (!_window)
			return fail("SDL_CreateWindow");
		_renderer = SDL_CreateRenderer(_window, nullptr);
		if (!_renderer)
			return fail("SDL_CreateRenderer");

		// Typed characters, as the layout made them, for the keyboard's text queue.
		SDL_StartTextInput(_window);
		return true;
	}

	void SdlWindow::fit(u32 width, u32 height)
	{
		if (!_window || width == 0 || height == 0)
			return;
		const int scale = std::max(1, std::min(MaxWindowWidth / static_cast<int>(width), MaxWindowHeight / static_cast<int>(height)));
		SDL_SetWindowSize(_window, static_cast<int>(width) * scale, static_cast<int>(height) * scale);
	}

	void SdlWindow::captureMouse()
	{
		if (_window && !_mouseCaptured)
		{
			SDL_SetWindowRelativeMouseMode(_window, true);
			_mouseCaptured = true;
		}
	}
}
