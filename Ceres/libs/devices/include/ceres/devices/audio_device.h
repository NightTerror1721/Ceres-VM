#pragma once

// A tone generator: one voice that plays a note of a given frequency, duration, volume and
// waveform. It is the audio half of the "consola retro" - a beeper with a choice of timbre rather
// than a sample player - and it is host-driven the way the display is: the device holds what the
// program asked for, and a host that has speakers installs a sink to play it. Without a sink the
// machine is silent, which is also what a headless run and a test want.

#include <ceres/vm/mmio_bus.h>
#include <atomic>
#include <functional>
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

	private:
		Tone _tone{};
		std::atomic<bool> _busy{ false };
		ToneSink _sink;

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

		// A reset silences a tone the last program left playing.
		void reset() override
		{
			if (_busy.load(std::memory_order_acquire))
				stop();
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

	public:
		u32 readUnsignedWord(Address offset) override
		{
			if (offset == StatusRegister) return isBusy() ? StatusBusy : 0;
			if (offset == FrequencyRegister) return _tone.frequency;
			if (offset == DurationRegister) return _tone.durationMs;
			if (offset == VolumeRegister) return _tone.volume;
			if (offset == WaveformRegister) return static_cast<u32>(_tone.waveform);
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
			}
		}
		void writeByte(Address offset, u8 value) override { writeWord(offset, value); }
		void writeHalfword(Address offset, u16 value) override { writeWord(offset, value); }
	};
}
