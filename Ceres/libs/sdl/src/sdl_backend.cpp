#include <ceres/sdl/sdl_backend.h>

#include <ceres/devices/text_renderer.h>

#define SDL_MAIN_HANDLED
#include <SDL3/SDL.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
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

			// Nothing of SDL is started until it is needed: a machine with a screen opens its window when the
			// program first shows a frame, and a program that never does opens none (see ensureVideo()).
			bool _videoStarted = false;
			bool _videoFailed = false;
			SDL_Window* _window = nullptr;
			SDL_Renderer* _renderer = nullptr;
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
			bool _mouseCaptured = false;
			bool _needsRedraw = false;
			u8 _buttons = 0; // Wheel events carry no button mask, so the last known one is kept.
			SDL_Gamepad* _gamepad = nullptr;

			// The tone generator. The machine's thread describes a tone through the sink; SDL's audio
			// thread turns it into samples. Everything they share is behind the mutex, and so is the
			// device pointer, so detachAudio() can be sure the audio thread is done with it.
			static inline constexpr int SampleRate = 44100;
			SDL_AudioStream* _audioStream = nullptr;
			bool _audioStarted = false;
			bool _audioFailed = false;
			std::mutex _audioMutex;
			devices::AudioDevice* _audio = nullptr;
			std::function<void(const std::filesystem::path&)> _dropHandler;
			devices::AudioDevice::Tone _tone{};
			bool _toneActive = false;
			i64 _samplesLeft = -1; // -1: play until stopped
			double _phase = 0.0;
			u32 _noise = 0x1234567u;

		public:
			SdlBackend() = default;

			~SdlBackend() override
			{
				detachAudio();
				if (_gamepad)
					SDL_CloseGamepad(_gamepad);
				if (_texture)
					SDL_DestroyTexture(_texture);
				if (_textTexture)
					SDL_DestroyTexture(_textTexture);
				if (_renderer)
					SDL_DestroyRenderer(_renderer);
				if (_window)
					SDL_DestroyWindow(_window);
				if (_videoStarted || _audioStarted)
					SDL_Quit();
			}

			bool showsText() const noexcept override { return true; }

			void setFileDropHandler(std::function<void(const std::filesystem::path&)> handler) override { _dropHandler = std::move(handler); }

			bool openWindow() override
			{
				// Asked for by name: the pixel display shows from the start, and the window is the size it always was.
				_displayLive = true;
				return ensureVideo(MaxWindowWidth, MaxWindowHeight);
			}

			bool pump(devices::KeyboardDevice& keyboard, devices::MouseDevice& mouse, devices::GamepadDevice& gamepad) override
			{
				if (!_videoStarted)
					return true;   // no window yet, so nothing can have happened to it

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
							keyboard.pushKey(static_cast<u32>(event.key.scancode), true);
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

						case SDL_EVENT_WINDOW_EXPOSED:
						case SDL_EVENT_WINDOW_PIXEL_SIZE_CHANGED:
							_needsRedraw = true;   // what the window shows is gone or the wrong size
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

				if (_needsRedraw && _shown == Shown::Text)
					drawText();
				_needsRedraw = false;

				return true;
			}

			void attachAudio(devices::AudioDevice& audio) override
			{
				{
					const std::lock_guard lock{ _audioMutex };
					_audio = &audio;
					_toneActive = false;
				}

				// The sound card is opened when the first tone is asked for, not for every run of a program that
				// never makes a sound. No sound card is not an error: the machine just stays silent, and every
				// tone is over as soon as it is asked for.
				audio.setToneSink([this](const std::optional<devices::AudioDevice::Tone>& tone)
				{
					if (tone && !ensureAudio())
					{
						if (_audio)
							_audio->toneFinished();
						return;
					}
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
				if (!ensureVideo(MaxWindowWidth, MaxWindowHeight))
					return;

				// A game wants unbounded deltas, not a cursor that stops at the window edge. Only a program that
				// draws pixels is taken to be one: a text interface needs its pointer to close the window with.
				if (!_mouseCaptured)
				{
					SDL_SetWindowRelativeMouseMode(_window, true);
					_mouseCaptured = true;
				}
				_shown = Shown::Pixels;

				if (width != _textureWidth || height != _textureHeight)
				{
					if (_texture)
						SDL_DestroyTexture(_texture);
					_texture = SDL_CreateTexture(_renderer, SDL_PIXELFORMAT_XRGB8888, SDL_TEXTUREACCESS_STREAMING, static_cast<int>(width), static_cast<int>(height));
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
				SDL_SetRenderLogicalPresentation(_renderer, 0, 0, SDL_LOGICAL_PRESENTATION_DISABLED);

				// The display stores 0x00RRGGBB per pixel: SDL calls that packed order
				// SDL_PIXELFORMAT_XRGB8888.
				SDL_UpdateTexture(_texture, nullptr, pixels.data(), static_cast<int>(width * sizeof(u32)));
				SDL_RenderClear(_renderer);
				SDL_RenderTexture(_renderer, _texture, nullptr, nullptr);
				SDL_RenderPresent(_renderer);
			}

			bool presentText(const devices::FramebufferDevice::Frame& frame) override
			{
				const u32 width = devices::TextRenderer::imageWidth(frame);
				const u32 height = devices::TextRenderer::imageHeight(frame);
				if (width == 0 || height == 0)
					return true;   // nothing to show is shown
				if (!ensureVideo(static_cast<int>(width), static_cast<int>(height)))
					return false;

				devices::TextRenderer::render(frame, _textPixels);

				if (width != _textWidth || height != _textHeight)
				{
					if (_textTexture)
						SDL_DestroyTexture(_textTexture);
					_textTexture = SDL_CreateTexture(_renderer, SDL_PIXELFORMAT_XRGB8888, SDL_TEXTUREACCESS_STREAMING, static_cast<int>(width), static_cast<int>(height));
					if (_textTexture)
						SDL_SetTextureScaleMode(_textTexture, SDL_SCALEMODE_NEAREST);   // crisp cells, at any window size
					_textWidth = width;
					_textHeight = height;
					fitWindow(width, height);
				}
				if (!_textTexture)
					return false;

				SDL_UpdateTexture(_textTexture, nullptr, _textPixels.data(), static_cast<int>(width * sizeof(u32)));
				_shown = Shown::Text;
				drawText();
				return true;
			}

		private:
			// Starts SDL and opens the window, once. `width` and `height` are what the window will show, so it
			// opens at the size it will have. If that cannot be done (no display: a server, a terminal session)
			// it says so once on standard error and never tries again, and the text goes to the terminal.
			bool ensureVideo(int width, int height)
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

				if (!SDL_Init(SDL_INIT_VIDEO | SDL_INIT_GAMEPAD))
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

			// The window is the text's size at the largest whole scale that fits, as it is for the pixel display.
			void fitWindow(u32 width, u32 height)
			{
				const int scale = std::max(1, std::min(MaxWindowWidth / static_cast<int>(width),
					MaxWindowHeight / static_cast<int>(height)));
				SDL_SetWindowSize(_window, static_cast<int>(width) * scale, static_cast<int>(height) * scale);
			}

			// Draws the text texture as it is, letterboxed at whatever size the window has.
			void drawText()
			{
				if (!_textTexture || !_renderer)
					return;
				SDL_SetRenderLogicalPresentation(_renderer, static_cast<int>(_textWidth), static_cast<int>(_textHeight), SDL_LOGICAL_PRESENTATION_LETTERBOX);
				SDL_SetRenderDrawColor(_renderer, 0, 0, 0, 255);
				SDL_RenderClear(_renderer);
				SDL_RenderTexture(_renderer, _textTexture, nullptr, nullptr);
				SDL_RenderPresent(_renderer);
			}

			// Opens the sound card for the first tone; false if there is none.
			bool ensureAudio()
			{
				if (_audioStream)
					return true;
				if (_audioFailed)
					return false;
				if (!SDL_InitSubSystem(SDL_INIT_AUDIO))
				{
					_audioFailed = true;
					return false;
				}
				_audioStarted = true;

				const SDL_AudioSpec spec{ SDL_AUDIO_F32, 1, SampleRate };
				SDL_AudioStream* stream = SDL_OpenAudioDeviceStream(SDL_AUDIO_DEVICE_DEFAULT_PLAYBACK, &spec, &SdlBackend::audioCallback, this);
				if (!stream)
				{
					_audioFailed = true;
					return false;
				}
				{
					const std::lock_guard lock{ _audioMutex };
					_audioStream = stream;
				}
				SDL_ResumeAudioStreamDevice(stream);
				return true;
			}

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
