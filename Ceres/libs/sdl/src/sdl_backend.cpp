#include <ceres/sdl/sdl_backend.h>

#define SDL_MAIN_HANDLED
#include <SDL3/SDL.h>

#include <algorithm>
#include <cmath>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace ceres::sdl
{
	namespace
	{
		class SdlBackend final : public driver::HostBackend
		{
		private:
			// The window's starting size, and the largest it is allowed to grow to when it follows
			// the display (see present()).
			static inline constexpr int MaxWindowWidth = 1280;
			static inline constexpr int MaxWindowHeight = 720;

			SDL_Window* _window = nullptr;
			SDL_Renderer* _renderer = nullptr;
			SDL_Texture* _texture = nullptr;
			u32 _textureWidth = 0;
			u32 _textureHeight = 0;
			u8 _buttons = 0; // Wheel events carry no button mask, so the last known one is kept.
			SDL_Gamepad* _gamepad = nullptr;

			// The tone generator. The machine's thread describes a tone through the sink; SDL's audio
			// thread turns it into samples. Everything they share is behind the mutex, and so is the
			// device pointer, so detachAudio() can be sure the audio thread is done with it.
			static inline constexpr int SampleRate = 44100;
			SDL_AudioStream* _audioStream = nullptr;
			std::mutex _audioMutex;
			devices::AudioDevice* _audio = nullptr;
			devices::AudioDevice::Tone _tone{};
			bool _toneActive = false;
			i64 _samplesLeft = -1; // -1: play until stopped
			double _phase = 0.0;
			u32 _noise = 0x1234567u;

		public:
			SdlBackend()
			{
				if (!SDL_Init(SDL_INIT_VIDEO | SDL_INIT_GAMEPAD | SDL_INIT_AUDIO))
					throw std::runtime_error(std::string("SDL_Init failed: ") + SDL_GetError());

				_window = SDL_CreateWindow("Ceres", MaxWindowWidth, MaxWindowHeight, SDL_WINDOW_RESIZABLE);
				if (!_window)
					throw std::runtime_error(std::string("SDL_CreateWindow failed: ") + SDL_GetError());

				_renderer = SDL_CreateRenderer(_window, nullptr);
				if (!_renderer)
					throw std::runtime_error(std::string("SDL_CreateRenderer failed: ") + SDL_GetError());

				// A game wants unbounded deltas, not a cursor that stops at the window edge.
				SDL_SetWindowRelativeMouseMode(_window, true);

				// Typed characters, as the layout made them, for the keyboard's text queue.
				SDL_StartTextInput(_window);
			}

			~SdlBackend() override
			{
				detachAudio();
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

						case SDL_EVENT_TEXT_INPUT:
							keyboard.pushText(std::string_view(event.text.text));
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

			void attachAudio(devices::AudioDevice& audio) override
			{
				// No sound card is not an error: the machine just stays silent.
				const SDL_AudioSpec spec{ SDL_AUDIO_F32, 1, SampleRate };
				SDL_AudioStream* stream = SDL_OpenAudioDeviceStream(SDL_AUDIO_DEVICE_DEFAULT_PLAYBACK, &spec, &SdlBackend::audioCallback, this);
				if (!stream)
					return;

				{
					const std::lock_guard lock{ _audioMutex };
					_audio = &audio;
					_audioStream = stream;
					_toneActive = false;
				}

				audio.setToneSink([this](const std::optional<devices::AudioDevice::Tone>& tone)
				{
					const std::lock_guard lock{ _audioMutex };
					if (!tone)
					{
						_toneActive = false;
						return;
					}
					_tone = *tone;
					_phase = 0.0;
					_samplesLeft = tone->durationMs == 0 ? -1 : static_cast<i64>(tone->durationMs) * SampleRate / 1000;
					_toneActive = true;
				});
				SDL_ResumeAudioStreamDevice(stream);
			}

			void detachAudio() override
			{
				SDL_AudioStream* stream = nullptr;
				{
					const std::lock_guard lock{ _audioMutex };
					if (_audio)
						_audio->clearToneSink();
					_audio = nullptr;
					_toneActive = false;
					stream = _audioStream;
					_audioStream = nullptr;
				}
				// Destroyed outside the lock: it waits for a callback in flight, and that callback wants the lock.
				if (stream)
					SDL_DestroyAudioStream(stream);
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

					// Follow the display: size the window to the display's resolution at the largest
					// integer scale that still fits, so the pixels stay crisp and the window never
					// outgrows its starting size. A later change to the display re-sizes it again.
					const int scale = std::max(1, std::min(MaxWindowWidth / static_cast<int>(width),
						MaxWindowHeight / static_cast<int>(height)));
					SDL_SetWindowSize(_window, static_cast<int>(width) * scale, static_cast<int>(height) * scale);
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
			static void SDLCALL audioCallback(void* userdata, SDL_AudioStream* stream, int additionalAmount, int)
			{
				static_cast<SdlBackend*>(userdata)->synthesize(stream, additionalAmount);
			}

			// Fills the stream with the next stretch of the current tone (or silence).
			void synthesize(SDL_AudioStream* stream, int bytes)
			{
				const int frames = bytes / static_cast<int>(sizeof(float));
				if (frames <= 0)
					return;

				std::vector<float> samples(static_cast<usize>(frames), 0.0f);
				{
					const std::lock_guard lock{ _audioMutex };
					if (_toneActive)
					{
						const double step = static_cast<double>(_tone.frequency) / SampleRate;
						const float gain = static_cast<float>(_tone.volume) / 255.0f * 0.25f; // headroom: it is a beeper, not a speaker test
						for (int i = 0; i < frames && _toneActive; ++i)
						{
							samples[static_cast<usize>(i)] = gain * wave(_tone.waveform, _phase);
							_phase += step;
							_phase -= std::floor(_phase);

							if (_samplesLeft > 0 && --_samplesLeft == 0)
							{
								_toneActive = false;
								if (_audio)
									_audio->toneFinished();
							}
						}
					}
				}
				SDL_PutAudioStreamData(stream, samples.data(), frames * static_cast<int>(sizeof(float)));
			}

			float wave(devices::AudioDevice::Waveform waveform, double phase)
			{
				switch (waveform)
				{
					case devices::AudioDevice::Square: return phase < 0.5 ? 1.0f : -1.0f;
					case devices::AudioDevice::Triangle: return static_cast<float>(4.0 * std::abs(phase - 0.5) - 1.0);
					case devices::AudioDevice::Sawtooth: return static_cast<float>(2.0 * phase - 1.0);
					case devices::AudioDevice::Sine: return static_cast<float>(std::sin(6.283185307179586 * phase));
					case devices::AudioDevice::Noise:
						_noise ^= _noise << 13; _noise ^= _noise >> 17; _noise ^= _noise << 5;
						return static_cast<float>(static_cast<i32>(_noise)) / 2147483648.0f;
				}
				return 0.0f;
			}

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
