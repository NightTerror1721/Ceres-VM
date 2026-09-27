#include "sdl_audio.h"

#include <cmath>
#include <optional>
#include <vector>

namespace ceres::sdl
{
	SdlAudio::~SdlAudio()
	{
		detachAudio();
		if (_audioStarted)
			SDL_QuitSubSystem(SDL_INIT_AUDIO);
	}

	void SdlAudio::attachAudio(devices::AudioDevice& audio)
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
		audio.setChannelWake([this] { return ensureAudio(); });   // the channels are mixed in synthesize()
	}

	void SdlAudio::detachAudio()
	{
		SDL_AudioStream* stream = nullptr;
		{
			const std::lock_guard lock{ _audioMutex };
			if (_audio)
			{
				_audio->clearToneSink();
				_audio->setChannelWake({});
			}
			_audio = nullptr;
			_toneActive = false;
			stream = _audioStream;
			_audioStream = nullptr;
		}
		// Destroyed outside the lock: it waits for a callback in flight, and that callback wants the lock.
		if (stream)
			SDL_DestroyAudioStream(stream);
	}

	bool SdlAudio::ensureAudio()
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
		SDL_AudioStream* stream = SDL_OpenAudioDeviceStream(SDL_AUDIO_DEVICE_DEFAULT_PLAYBACK, &spec, &SdlAudio::audioCallback, this);
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

	void SDLCALL SdlAudio::audioCallback(void* userdata, SDL_AudioStream* stream, int additionalAmount, int)
	{
		static_cast<SdlAudio*>(userdata)->synthesize(stream, additionalAmount);
	}

	void SdlAudio::synthesize(SDL_AudioStream* stream, int bytes)
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
			if (_audio)
				_audio->renderChannels(samples.data(), samples.size(), SampleRate);
		}
		SDL_PutAudioStreamData(stream, samples.data(), frames * static_cast<int>(sizeof(float)));
	}

	float SdlAudio::wave(devices::AudioDevice::Waveform waveform, double phase)
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
}
