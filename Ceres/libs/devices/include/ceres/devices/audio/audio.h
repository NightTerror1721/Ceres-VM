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
		// Called when a channel is keyed on: the host makes sure its audio is running (renderChannels), and says
		// whether it is. Without a host that renders them - none attached, or its sound failed to open - a channel
		// keyed on stays silent, so its status bit never has a program waiting on an envelope nobody runs.
		using ChannelWake = std::function<bool()>;

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
		void renderChannels(float* out, usize frames, u32 sampleRate);

		// A channel as it stands, for a test or a debugger.
		Channel channel(u32 index) const;

		bool isBusy() const noexcept { return _busy.load(std::memory_order_acquire); }
		const Tone& tone() const noexcept { return _tone; }

		// Called by the host, from whatever thread plays the sound, when a tone has run its whole
		// duration. Clears the busy bit and raises the interrupt so a program can wait for it with
		// sti/halt instead of polling.
		void toneFinished();

		// A reset silences a tone the last program left playing, and every channel.
		void reset() override;

	private:
		void play();

		void stop();

		static u32 clamp(u32 value, u32 low, u32 high) { return value < low ? low : (value > high ? high : value); }

		static float sample(Channel& c);

		void channelCommand(u32 value);

		bool channelRegister(Address offset, u32*& field, Channel*& c);

	public:
		u32 read(Address offset) override;
		void write(Address offset, u32 value) override;
		const RegisterMap& registers() const override;
	};
}
