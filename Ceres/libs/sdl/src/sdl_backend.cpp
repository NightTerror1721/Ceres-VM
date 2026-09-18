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

		public:
			SdlBackend()
			{
				if (!SDL_Init(SDL_INIT_VIDEO))
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
				if (_texture)
					SDL_DestroyTexture(_texture);
				if (_renderer)
					SDL_DestroyRenderer(_renderer);
				if (_window)
					SDL_DestroyWindow(_window);
				SDL_Quit();
			}

			bool pump(devices::KeyboardDevice& keyboard, devices::MouseDevice& mouse) override
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

						default:
							break;
					}
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
		};
	}

	std::unique_ptr<driver::HostBackend> createSdlBackend()
	{
		return std::make_unique<SdlBackend>();
	}
}
