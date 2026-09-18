#include <ceres/sdl/sdl_backend.h>

#define SDL_MAIN_HANDLED
#include <SDL3/SDL.h>

#include <stdexcept>
#include <string>

namespace ceres::sdl
{
	namespace
	{
		class SdlBackend final : public driver::HostBackend
		{
		private:
			SDL_Window* _window = nullptr;
			SDL_Renderer* _renderer = nullptr;
			SDL_Texture* _texture = nullptr;
			u32 _textureWidth = 0;
			u32 _textureHeight = 0;
			u8 _buttons = 0; // Wheel events carry no button mask, so the last known one is kept.
			SDL_Gamepad* _gamepad = nullptr;

		public:
			SdlBackend()
			{
				if (!SDL_Init(SDL_INIT_VIDEO | SDL_INIT_GAMEPAD))
					throw std::runtime_error(std::string("SDL_Init failed: ") + SDL_GetError());

				_window = SDL_CreateWindow("Ceres", 1280, 720, SDL_WINDOW_RESIZABLE);
				if (!_window)
					throw std::runtime_error(std::string("SDL_CreateWindow failed: ") + SDL_GetError());

				_renderer = SDL_CreateRenderer(_window, nullptr);
				if (!_renderer)
					throw std::runtime_error(std::string("SDL_CreateRenderer failed: ") + SDL_GetError());

				// A game wants unbounded deltas, not a cursor that stops at the window edge.
				SDL_SetWindowRelativeMouseMode(_window, true);
			}

			~SdlBackend() override
			{
				if (_gamepad)
					SDL_CloseGamepad(_gamepad);
				if (_texture)
					SDL_DestroyTexture(_texture);
				if (_renderer)
					SDL_DestroyRenderer(_renderer);
				if (_window)
					SDL_DestroyWindow(_window);
				SDL_Quit();
			}

			bool pump(devices::KeyboardDevice& keyboard, devices::MouseDevice& mouse, devices::GamepadDevice& gamepad) override
			{
				SDL_Event event;
				while (SDL_PollEvent(&event))
				{
					switch (event.type)
					{
						case SDL_EVENT_QUIT:
							return false;

						case SDL_EVENT_KEY_DOWN:
							keyboard.pushKey(static_cast<u32>(event.key.scancode), true);
							if (event.key.scancode == SDL_SCANCODE_ESCAPE)
								return false;
							break;

						case SDL_EVENT_KEY_UP:
							keyboard.pushKey(static_cast<u32>(event.key.scancode), false);
							break;

						case SDL_EVENT_MOUSE_MOTION:
							mouse.pushMotion(static_cast<i32>(event.motion.xrel), static_cast<i32>(event.motion.yrel), toButtons(event.motion.state), 0);
							break;

						case SDL_EVENT_MOUSE_BUTTON_DOWN:
						case SDL_EVENT_MOUSE_BUTTON_UP:
							_buttons = toButtons(SDL_GetMouseState(nullptr, nullptr));
							mouse.pushMotion(0, 0, _buttons, 0);
							break;

						case SDL_EVENT_MOUSE_WHEEL:
							mouse.pushMotion(0, 0, _buttons, static_cast<i8>(event.wheel.y));
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

				// A gamepad is state, not a queue of events, so it is polled every pump. pushState
				// only raises the interrupt when something actually changed.
				if (_gamepad)
				{
					gamepad.pushState(toGamepadButtons(_gamepad),
						SDL_GetGamepadAxis(_gamepad, SDL_GAMEPAD_AXIS_LEFTX),
						SDL_GetGamepadAxis(_gamepad, SDL_GAMEPAD_AXIS_LEFTY),
						SDL_GetGamepadAxis(_gamepad, SDL_GAMEPAD_AXIS_RIGHTX),
						SDL_GetGamepadAxis(_gamepad, SDL_GAMEPAD_AXIS_RIGHTY),
						static_cast<u16>(SDL_GetGamepadAxis(_gamepad, SDL_GAMEPAD_AXIS_LEFT_TRIGGER)),
						static_cast<u16>(SDL_GetGamepadAxis(_gamepad, SDL_GAMEPAD_AXIS_RIGHT_TRIGGER)));
				}

				return true;
			}

			void present(const devices::DisplayDevice& display) override
			{
				const u32 width = display.width();
				const u32 height = display.height();
				const auto pixels = display.pixels();

				if (width == 0 || height == 0 || pixels.empty())
					return;

				if (width != _textureWidth || height != _textureHeight)
				{
					if (_texture)
						SDL_DestroyTexture(_texture);
					_texture = SDL_CreateTexture(_renderer, SDL_PIXELFORMAT_BGRX8888, SDL_TEXTUREACCESS_STREAMING, static_cast<int>(width), static_cast<int>(height));
					_textureWidth = width;
					_textureHeight = height;
				}

				if (!_texture)
					return;

				// The display stores 0x00RRGGBB per pixel, which in memory is B,G,R,0 - exactly
				// SDL_PIXELFORMAT_BGRX8888.
				SDL_UpdateTexture(_texture, nullptr, pixels.data(), static_cast<int>(width * sizeof(u32)));
				SDL_RenderClear(_renderer);
				SDL_RenderTexture(_renderer, _texture, nullptr, nullptr);
				SDL_RenderPresent(_renderer);
			}

		private:
			static u8 toButtons(Uint32 sdlState)
			{
				u8 buttons = 0;
				if (sdlState & SDL_BUTTON_LMASK) buttons |= devices::MouseDevice::ButtonLeft;
				if (sdlState & SDL_BUTTON_RMASK) buttons |= devices::MouseDevice::ButtonRight;
				if (sdlState & SDL_BUTTON_MMASK) buttons |= devices::MouseDevice::ButtonMiddle;
				return buttons;
			}

			static u16 toGamepadButtons(SDL_Gamepad* gamepad)
			{
				u16 buttons = 0;
				if (SDL_GetGamepadButton(gamepad, SDL_GAMEPAD_BUTTON_SOUTH)) buttons |= devices::GamepadDevice::ButtonSouth;
				if (SDL_GetGamepadButton(gamepad, SDL_GAMEPAD_BUTTON_EAST)) buttons |= devices::GamepadDevice::ButtonEast;
				if (SDL_GetGamepadButton(gamepad, SDL_GAMEPAD_BUTTON_WEST)) buttons |= devices::GamepadDevice::ButtonWest;
				if (SDL_GetGamepadButton(gamepad, SDL_GAMEPAD_BUTTON_NORTH)) buttons |= devices::GamepadDevice::ButtonNorth;
				if (SDL_GetGamepadButton(gamepad, SDL_GAMEPAD_BUTTON_BACK)) buttons |= devices::GamepadDevice::ButtonBack;
				if (SDL_GetGamepadButton(gamepad, SDL_GAMEPAD_BUTTON_GUIDE)) buttons |= devices::GamepadDevice::ButtonGuide;
				if (SDL_GetGamepadButton(gamepad, SDL_GAMEPAD_BUTTON_START)) buttons |= devices::GamepadDevice::ButtonStart;
				if (SDL_GetGamepadButton(gamepad, SDL_GAMEPAD_BUTTON_LEFT_STICK)) buttons |= devices::GamepadDevice::ButtonLeftStick;
				if (SDL_GetGamepadButton(gamepad, SDL_GAMEPAD_BUTTON_RIGHT_STICK)) buttons |= devices::GamepadDevice::ButtonRightStick;
				if (SDL_GetGamepadButton(gamepad, SDL_GAMEPAD_BUTTON_LEFT_SHOULDER)) buttons |= devices::GamepadDevice::ButtonLeftShoulder;
				if (SDL_GetGamepadButton(gamepad, SDL_GAMEPAD_BUTTON_RIGHT_SHOULDER)) buttons |= devices::GamepadDevice::ButtonRightShoulder;
				if (SDL_GetGamepadButton(gamepad, SDL_GAMEPAD_BUTTON_DPAD_UP)) buttons |= devices::GamepadDevice::ButtonDpadUp;
				if (SDL_GetGamepadButton(gamepad, SDL_GAMEPAD_BUTTON_DPAD_DOWN)) buttons |= devices::GamepadDevice::ButtonDpadDown;
				if (SDL_GetGamepadButton(gamepad, SDL_GAMEPAD_BUTTON_DPAD_LEFT)) buttons |= devices::GamepadDevice::ButtonDpadLeft;
				if (SDL_GetGamepadButton(gamepad, SDL_GAMEPAD_BUTTON_DPAD_RIGHT)) buttons |= devices::GamepadDevice::ButtonDpadRight;
				return buttons;
			}
		};
	}

	std::unique_ptr<driver::HostBackend> createSdlBackend()
	{
		return std::make_unique<SdlBackend>();
	}
}
