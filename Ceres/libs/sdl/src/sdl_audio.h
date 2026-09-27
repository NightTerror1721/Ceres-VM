#pragma once

// The speakers: the audio device's tone and channels, synthesized on SDL's audio thread.

#include <ceres/driver/audio_output.h>

#define SDL_MAIN_HANDLED
#include <SDL3/SDL.h>

#include <mutex>

namespace ceres::sdl
{
	class SdlAudio final : public driver::AudioOutput
	{
	private:
		// The machine's thread describes a tone through the sink; SDL's audio thread turns it into samples.
		// Everything they share is behind the mutex, and so is the device pointer, so detachAudio() can be sure the
		// audio thread is done with it.
		static inline constexpr int SampleRate = 44100;
		SDL_AudioStream* _audioStream = nullptr;
		bool _audioStarted = false;
		bool _audioFailed = false;
		std::mutex _audioMutex;
		devices::AudioDevice* _audio = nullptr;
		devices::AudioDevice::Tone _tone{};
		bool _toneActive = false;
		i64 _samplesLeft = -1; // -1: play until stopped
		double _phase = 0.0;
		u32 _noise = 0x1234567u;

		// Opens the sound card for the first tone; false if there is none.
		bool ensureAudio();
		static void SDLCALL audioCallback(void* userdata, SDL_AudioStream* stream, int additionalAmount, int);
		// Fills the stream with the next stretch of the current tone (or silence).
		void synthesize(SDL_AudioStream* stream, int bytes);
		float wave(devices::AudioDevice::Waveform waveform, double phase);

	public:
		SdlAudio() = default;
		SdlAudio(const SdlAudio&) = delete;
		SdlAudio& operator=(const SdlAudio&) = delete;
		~SdlAudio() override;

		void attachAudio(devices::AudioDevice& audio) override;
		void detachAudio() override;
	};
}
