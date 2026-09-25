#pragma once

// A tone generator: one voice that plays a note of a given frequency, duration, volume and
// waveform. It is the audio half of the "consola retro" - a beeper with a choice of timbre rather
// than a sample player - and it is host-driven the way the display is: the device holds what the
// program asked for, and a host that has speakers installs a sink to play it. Without a sink the
// machine is silent, which is also what a headless run and a test want.
//
// Beside it, four CHANNELS for music, in the manner of the 8-bit consoles: each with a frequency, a volume, a
// waveform (a square of any duty, triangle, sawtooth, sine or noise) and an ADSR envelope - attack, decay,
// sustain, release. A program keys a channel on (the attack starts) and off (the release starts); the device
// mixes the four itself, in renderChannels(), which the host's audio thread calls for as many samples as it
// needs, so every host sounds the same and a test can listen without speakers.

#include <ceres/vm/mmio_bus.h>
#include <array>
#include <atomic>
#include <cmath>
#include <functional>
#include <mutex>
#include <optional>

namespace ceres::devices
{
	using namespace vm;

	class AudioDevice final : public IODevice
	{
	public:
		static inline constexpr Address StatusRegister = Address(0x00);    // Read: bit 0 = a tone is playing
		static inline constexpr Address FrequencyRegister = Address(0x04); // Read/write: Hz
		static inline constexpr Address DurationRegister = Address(0x08);  // Read/write: milliseconds, 0 = until stopped
		static inline constexpr Address VolumeRegister = Address(0x0C);    // Read/write: 0..255
		static inline constexpr Address WaveformRegister = Address(0x10);  // Read/write: a Waveform
		static inline constexpr Address CommandRegister = Address(0x14);   // Write: 1 = play, 2 = stop

		// The channels. ChannelCommand takes channel << 8 | command; the rest are per channel, at ChannelBase +
		// channel * ChannelStride.
		static inline constexpr Address ChannelCommandRegister = Address(0x20);   // Write: channel << 8 | 1 key on, 2 key off, 3 stop
		static inline constexpr Address ChannelStatusRegister = Address(0x24);    // Read: bit n = channel n is sounding
		static inline constexpr Address ChannelCountRegister = Address(0x28);     // Read: how many channels (4)
		static inline constexpr u32 ChannelBase = 0x40;
		static inline constexpr u32 ChannelStride = 0x20;
		static inline constexpr u32 ChannelFrequency = 0x00;   // Hz
		static inline constexpr u32 ChannelVolume = 0x04;      // 0..255
		static inline constexpr u32 ChannelWaveform = 0x08;    // a Waveform
		static inline constexpr u32 ChannelDuty = 0x0C;        // a square's high part, 1..255 of 256 (128: half)
		static inline constexpr u32 ChannelAttack = 0x10;      // ms to reach full level
		static inline constexpr u32 ChannelDecay = 0x14;       // ms to fall to the sustain level
		static inline constexpr u32 ChannelSustain = 0x18;     // 0..255, the level while the key is held
		static inline constexpr u32 ChannelRelease = 0x1C;     // ms to fall silent after key off
		static inline constexpr u32 ChannelCount = 4;
		static inline constexpr u32 ChannelKeyOn = 1;
		static inline constexpr u32 ChannelKeyOff = 2;
		static inline constexpr u32 ChannelStop = 3;

		static inline constexpr u32 CommandPlay = 1;
		static inline constexpr u32 CommandStop = 2;

		static inline constexpr u32 StatusBusy = 1u << 0;

		enum Waveform : u32
		{
			Square = 0,
			Triangle = 1,
			Sawtooth = 2,
			Sine = 3,
			Noise = 4,
		};
		static inline constexpr u32 WaveformCount = 5;

		static inline constexpr u32 MinFrequency = 20;
		static inline constexpr u32 MaxFrequency = 20000;

		// Seventh user interrupt (the timer, terminal, DMA controller, keyboard, mouse and gamepad
		// have 0-5): a tone that ran its full duration has finished. A tone that was stopped, or
		// replaced, does not raise it.
		static inline constexpr InterruptNumber Interrupt = InterruptNumber::UserInterrupt6;

		struct Tone
		{
			u32 frequency = 440;
			u32 durationMs = 0;
			u32 volume = 128;
			Waveform waveform = Square;
		};

		// What the host is asked to do: play this, or (an empty value) fall silent.
		using ToneSink = std::function<void(const std::optional<Tone>&)>;
		// Called when a channel starts to sound: the host makes sure its audio is running (renderChannels).
		using ChannelWake = std::function<void()>;

		struct Channel
		{
			u32 frequency = 440, volume = 160, waveform = Square, duty = 128;
			u32 attackMs = 5, decayMs = 60, sustain = 180, releaseMs = 120;
			enum Stage : u8 { Off, Attack, Decay, Sustain, Release } stage = Off;
			float level = 0.0f;          // the envelope, 0..1
			float releaseFrom = 0.0f;
			double phase = 0.0;
			u32 noise = 0x2545F491u;
		};

	private:
		Tone _tone{};
		std::atomic<bool> _busy{ false };
		ToneSink _sink;
		ChannelWake _wake;
		mutable std::mutex _channelMutex;   // the machine writes the registers, the host's audio thread renders
		std::array<Channel, ChannelCount> _channels{};

	public:
		AudioDevice() = default;
		AudioDevice(const AudioDevice&) = delete;
		AudioDevice(AudioDevice&&) = delete;
		~AudioDevice() override = default;

		AudioDevice& operator=(const AudioDevice&) = delete;
		AudioDevice& operator=(AudioDevice&&) = delete;

	public:
		void attachTo(MmioBus& bus)
		{
			bus.attach(default_mmio::Audio, *this);
		}

		void detachFrom(MmioBus& bus)
		{
			bus.detach(default_mmio::Audio);
		}

		void setToneSink(ToneSink sink) { _sink = std::move(sink); }
		void clearToneSink() { _sink = nullptr; }
		void setChannelWake(ChannelWake wake) { _wake = std::move(wake); }

		// Adds the four channels to out (mono floats, about -1..1 each channel at full volume, scaled down to
		// leave room for the rest), frames samples at sampleRate, moving their envelopes on.
		void renderChannels(float* out, usize frames, u32 sampleRate)
		{
			if (sampleRate == 0)
				return;
			const std::lock_guard lock{ _channelMutex };
			for (Channel& c : _channels)
			{
				if (c.stage == Channel::Off)
					continue;
				const double step = static_cast<double>(c.frequency) / sampleRate;
				const float gain = static_cast<float>(c.volume) / 255.0f * 0.2f;
				const float sustain = static_cast<float>(c.sustain) / 255.0f;
				auto perSample = [&](u32 ms, float span) { return ms == 0 ? 1.0f : span * 1000.0f / (static_cast<float>(ms) * sampleRate); };
				for (usize i = 0; i < frames && c.stage != Channel::Off; ++i)
				{
					switch (c.stage)
					{
					case Channel::Attack:
						c.level += perSample(c.attackMs, 1.0f);
						if (c.level >= 1.0f) { c.level = 1.0f; c.stage = Channel::Decay; }
						break;
					case Channel::Decay:
						c.level -= perSample(c.decayMs, 1.0f - sustain);
						if (c.level <= sustain) { c.level = sustain; c.stage = Channel::Sustain; }
						break;
					case Channel::Release:
						c.level -= perSample(c.releaseMs, c.releaseFrom);
						if (c.level <= 0.0f) { c.level = 0.0f; c.stage = Channel::Off; }
						break;
					default:
						break;
					}
					out[i] += gain * c.level * sample(c);
					c.phase += step;
					c.phase -= std::floor(c.phase);
				}
			}
		}

		// A channel as it stands, for a test or a debugger.
		Channel channel(u32 index) const
		{
			const std::lock_guard lock{ _channelMutex };
			return index < ChannelCount ? _channels[index] : Channel{};
		}

		bool isBusy() const noexcept { return _busy.load(std::memory_order_acquire); }
		const Tone& tone() const noexcept { return _tone; }

		// Called by the host, from whatever thread plays the sound, when a tone has run its whole
		// duration. Clears the busy bit and raises the interrupt so a program can wait for it with
		// sti/halt instead of polling.
		void toneFinished()
		{
			if (_busy.exchange(false, std::memory_order_acq_rel))
				raiseInterrupt(Interrupt);
		}

		// A reset silences a tone the last program left playing, and every channel.
		void reset() override
		{
			if (_busy.load(std::memory_order_acquire))
				stop();
			const std::lock_guard lock{ _channelMutex };
			for (Channel& c : _channels)
				c = Channel{};
		}

	private:
		void play()
		{
			// Without anyone to play it the tone is not started at all, so a program waiting for the
			// busy bit to clear never waits for a host that is not there. A tone with no duration
			// runs until it is stopped, and is busy until then.
			if (!_sink)
				return;

			_busy.store(true, std::memory_order_release);
			_sink(_tone);
		}

		void stop()
		{
			_busy.store(false, std::memory_order_release);
			if (_sink)
				_sink(std::nullopt);
		}

		static u32 clamp(u32 value, u32 low, u32 high) { return value < low ? low : (value > high ? high : value); }

		static float sample(Channel& c)
		{
			switch (c.waveform)
			{
			case Square: return c.phase * 256.0 < c.duty ? 1.0f : -1.0f;
			case Triangle: return static_cast<float>(4.0 * std::abs(c.phase - 0.5) - 1.0);
			case Sawtooth: return static_cast<float>(2.0 * c.phase - 1.0);
			case Sine: return static_cast<float>(std::sin(6.283185307179586 * c.phase));
			default:
				c.noise ^= c.noise << 13; c.noise ^= c.noise >> 17; c.noise ^= c.noise << 5;
				return static_cast<float>(static_cast<i32>(c.noise)) / 2147483648.0f;
			}
		}

		void channelCommand(u32 value)
		{
			const u32 index = (value >> 8) & 0xFFu;
			const u32 command = value & 0xFFu;
			if (index >= ChannelCount)
				return;
			bool woke = false;
			{
				const std::lock_guard lock{ _channelMutex };
				Channel& c = _channels[index];
				if (command == ChannelKeyOn)
				{
					c.stage = Channel::Attack;               // from the level it has: a retrigger does not click
					woke = true;
				}
				else if (command == ChannelKeyOff && c.stage != Channel::Off)
				{
					c.releaseFrom = c.level;
					c.stage = Channel::Release;
				}
				else if (command == ChannelStop)
				{
					c.stage = Channel::Off;
					c.level = 0.0f;
				}
			}
			if (woke && _wake)
				_wake();
		}

		bool channelRegister(Address offset, u32*& field, Channel*& c)
		{
			const u32 at = offset.value();
			if (at < ChannelBase || at >= ChannelBase + ChannelCount * ChannelStride)
				return false;
			c = &_channels[(at - ChannelBase) / ChannelStride];
			switch ((at - ChannelBase) % ChannelStride)
			{
			case ChannelFrequency: field = &c->frequency; return true;
			case ChannelVolume: field = &c->volume; return true;
			case ChannelWaveform: field = &c->waveform; return true;
			case ChannelDuty: field = &c->duty; return true;
			case ChannelAttack: field = &c->attackMs; return true;
			case ChannelDecay: field = &c->decayMs; return true;
			case ChannelSustain: field = &c->sustain; return true;
			case ChannelRelease: field = &c->releaseMs; return true;
			default: return false;
			}
		}

	public:
		u32 readUnsignedWord(Address offset) override
		{
			if (offset == StatusRegister) return isBusy() ? StatusBusy : 0;
			if (offset == FrequencyRegister) return _tone.frequency;
			if (offset == DurationRegister) return _tone.durationMs;
			if (offset == VolumeRegister) return _tone.volume;
			if (offset == WaveformRegister) return static_cast<u32>(_tone.waveform);
			if (offset == ChannelCountRegister) return ChannelCount;
			const std::lock_guard lock{ _channelMutex };
			if (offset == ChannelStatusRegister)
			{
				u32 bits = 0;
				for (u32 i = 0; i < ChannelCount; ++i)
					if (_channels[i].stage != Channel::Off)
						bits |= 1u << i;
				return bits;
			}
			u32* field = nullptr;
			Channel* c = nullptr;
			if (channelRegister(offset, field, c))
				return *field;
			return 0;
		}
		u8 readUnsignedByte(Address offset) override { return static_cast<u8>(readUnsignedWord(offset)); }
		i8 readSignedByte(Address offset) override { return static_cast<i8>(readUnsignedByte(offset)); }
		u16 readUnsignedHalfword(Address offset) override { return static_cast<u16>(readUnsignedWord(offset)); }
		i16 readSignedHalfword(Address offset) override { return static_cast<i16>(readUnsignedHalfword(offset)); }

		void writeWord(Address offset, u32 value) override
		{
			if (offset == FrequencyRegister) { _tone.frequency = clamp(value, MinFrequency, MaxFrequency); return; }
			if (offset == DurationRegister) { _tone.durationMs = value; return; }
			if (offset == VolumeRegister) { _tone.volume = clamp(value, 0, 255); return; }
			if (offset == WaveformRegister)
			{
				// An unknown timbre is a typo, not a request: keep the one that was set.
				if (value < WaveformCount)
					_tone.waveform = static_cast<Waveform>(value);
				return;
			}
			if (offset == CommandRegister)
			{
				if (value == CommandPlay)
					play();
				else if (value == CommandStop)
					stop();
				return;
			}
			if (offset == ChannelCommandRegister)
			{
				channelCommand(value);
				return;
			}
			const std::lock_guard lock{ _channelMutex };
			u32* field = nullptr;
			Channel* c = nullptr;
			if (!channelRegister(offset, field, c))
				return;
			if (field == &c->frequency) value = clamp(value, MinFrequency, MaxFrequency);
			else if (field == &c->volume || field == &c->sustain) value = clamp(value, 0, 255);
			else if (field == &c->duty) value = clamp(value, 1, 255);
			else if (field == &c->waveform && value >= WaveformCount) return;   // a typo keeps the one there
			*field = value;
		}
		void writeByte(Address offset, u8 value) override { writeWord(offset, value); }
		void writeHalfword(Address offset, u16 value) override { writeWord(offset, value); }
	};
}
