#include <ceres/devices/audio/audio.h>
#include <cmath>

namespace ceres::devices
{
	namespace
	{
		// Every register of the device (plan/v2 SPEC 5.3), in offset order.
		constexpr RegisterInfo Registers[] = {
			{ 0x00, "Status",            RegisterAccess::Read,      0x0, false, "Bit 0 = a tone is playing." },
			{ 0x04, "Frequency",         RegisterAccess::ReadWrite, 0x0, false, "Hz." },
			{ 0x08, "Duration",          RegisterAccess::ReadWrite, 0x0, false, "Milliseconds, 0 = until stopped." },
			{ 0x0C, "Volume",            RegisterAccess::ReadWrite, 0x0, false, "0..255." },
			{ 0x10, "Waveform",          RegisterAccess::ReadWrite, 0x0, false, "A Waveform." },
			{ 0x14, "Command",           RegisterAccess::Write,     0x0, false, "1 = play, 2 = stop." },
			{ 0x20, "ChannelCommand",    RegisterAccess::Write,     0x0, false, "Channel << 8 | 1 key on, 2 key off, 3 stop." },
			{ 0x24, "ChannelStatus",     RegisterAccess::Read,      0x0, false, "Bit n = channel n is sounding." },
			{ 0x28, "ChannelCount",      RegisterAccess::Read,      0x4, false, "How many channels (4)" },
			{ 0x40, "Channel0Frequency", RegisterAccess::ReadWrite, 0x0, false, "Channel 0. Hz." },
			{ 0x44, "Channel0Volume",    RegisterAccess::ReadWrite, 0x0, false, "Channel 0. 0..255." },
			{ 0x48, "Channel0Waveform",  RegisterAccess::ReadWrite, 0x0, false, "Channel 0. A Waveform." },
			{ 0x4C, "Channel0Duty",      RegisterAccess::ReadWrite, 0x0, false, "Channel 0. A square wave's high part, 1..255 of 256 (128: half)." },
			{ 0x50, "Channel0Attack",    RegisterAccess::ReadWrite, 0x0, false, "Channel 0. Milliseconds to reach full level." },
			{ 0x54, "Channel0Decay",     RegisterAccess::ReadWrite, 0x0, false, "Channel 0. Milliseconds to fall to the sustain level." },
			{ 0x58, "Channel0Sustain",   RegisterAccess::ReadWrite, 0x0, false, "Channel 0. 0..255, the level while the key is held." },
			{ 0x5C, "Channel0Release",   RegisterAccess::ReadWrite, 0x0, false, "Channel 0. Milliseconds to fall silent after key off." },
			{ 0x60, "Channel1Frequency", RegisterAccess::ReadWrite, 0x0, false, "Channel 1. Hz." },
			{ 0x64, "Channel1Volume",    RegisterAccess::ReadWrite, 0x0, false, "Channel 1. 0..255." },
			{ 0x68, "Channel1Waveform",  RegisterAccess::ReadWrite, 0x0, false, "Channel 1. A Waveform." },
			{ 0x6C, "Channel1Duty",      RegisterAccess::ReadWrite, 0x0, false, "Channel 1. A square wave's high part, 1..255 of 256 (128: half)." },
			{ 0x70, "Channel1Attack",    RegisterAccess::ReadWrite, 0x0, false, "Channel 1. Milliseconds to reach full level." },
			{ 0x74, "Channel1Decay",     RegisterAccess::ReadWrite, 0x0, false, "Channel 1. Milliseconds to fall to the sustain level." },
			{ 0x78, "Channel1Sustain",   RegisterAccess::ReadWrite, 0x0, false, "Channel 1. 0..255, the level while the key is held." },
			{ 0x7C, "Channel1Release",   RegisterAccess::ReadWrite, 0x0, false, "Channel 1. Milliseconds to fall silent after key off." },
			{ 0x80, "Channel2Frequency", RegisterAccess::ReadWrite, 0x0, false, "Channel 2. Hz." },
			{ 0x84, "Channel2Volume",    RegisterAccess::ReadWrite, 0x0, false, "Channel 2. 0..255." },
			{ 0x88, "Channel2Waveform",  RegisterAccess::ReadWrite, 0x0, false, "Channel 2. A Waveform." },
			{ 0x8C, "Channel2Duty",      RegisterAccess::ReadWrite, 0x0, false, "Channel 2. A square wave's high part, 1..255 of 256 (128: half)." },
			{ 0x90, "Channel2Attack",    RegisterAccess::ReadWrite, 0x0, false, "Channel 2. Milliseconds to reach full level." },
			{ 0x94, "Channel2Decay",     RegisterAccess::ReadWrite, 0x0, false, "Channel 2. Milliseconds to fall to the sustain level." },
			{ 0x98, "Channel2Sustain",   RegisterAccess::ReadWrite, 0x0, false, "Channel 2. 0..255, the level while the key is held." },
			{ 0x9C, "Channel2Release",   RegisterAccess::ReadWrite, 0x0, false, "Channel 2. Milliseconds to fall silent after key off." },
			{ 0xA0, "Channel3Frequency", RegisterAccess::ReadWrite, 0x0, false, "Channel 3. Hz." },
			{ 0xA4, "Channel3Volume",    RegisterAccess::ReadWrite, 0x0, false, "Channel 3. 0..255." },
			{ 0xA8, "Channel3Waveform",  RegisterAccess::ReadWrite, 0x0, false, "Channel 3. A Waveform." },
			{ 0xAC, "Channel3Duty",      RegisterAccess::ReadWrite, 0x0, false, "Channel 3. A square wave's high part, 1..255 of 256 (128: half)." },
			{ 0xB0, "Channel3Attack",    RegisterAccess::ReadWrite, 0x0, false, "Channel 3. Milliseconds to reach full level." },
			{ 0xB4, "Channel3Decay",     RegisterAccess::ReadWrite, 0x0, false, "Channel 3. Milliseconds to fall to the sustain level." },
			{ 0xB8, "Channel3Sustain",   RegisterAccess::ReadWrite, 0x0, false, "Channel 3. 0..255, the level while the key is held." },
			{ 0xBC, "Channel3Release",   RegisterAccess::ReadWrite, 0x0, false, "Channel 3. Milliseconds to fall silent after key off." },
		};
	}

	void AudioDevice::renderChannels(float* out, usize frames, u32 sampleRate)
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

	AudioDevice::Channel AudioDevice::channel(u32 index) const
	{
		const std::lock_guard lock{ _channelMutex };
		return index < ChannelCount ? _channels[index] : Channel{};
	}

	void AudioDevice::toneFinished()
	{
		if (_busy.exchange(false, std::memory_order_acq_rel))
			raiseInterrupt(Interrupt);
	}

	void AudioDevice::reset()
	{
		if (_busy.load(std::memory_order_acquire))
			stop();
		const std::lock_guard lock{ _channelMutex };
		for (Channel& c : _channels)
			c = Channel{};
	}

	void AudioDevice::play()
	{
		// Without anyone to play it the tone is not started at all, so a program waiting for the
		// busy bit to clear never waits for a host that is not there. A tone with no duration
		// runs until it is stopped, and is busy until then.
		if (!_sink)
			return;

		_busy.store(true, std::memory_order_release);
		_sink(_tone);
	}

	void AudioDevice::stop()
	{
		_busy.store(false, std::memory_order_release);
		if (_sink)
			_sink(std::nullopt);
	}

	float AudioDevice::sample(Channel& c)
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

	void AudioDevice::channelCommand(u32 value)
	{
		const u32 index = (value >> 8) & 0xFFu;
		const u32 command = value & 0xFFu;
		if (index >= ChannelCount)
			return;
		// Asked before the lock is taken: a host starting its audio may wait for its callback, which takes it.
		const bool rendered = command == ChannelKeyOn && _wake && _wake();
		{
			const std::lock_guard lock{ _channelMutex };
			Channel& c = _channels[index];
			if (command == ChannelKeyOn)
			{
				if (rendered)
					c.stage = Channel::Attack;           // from the level it has: a retrigger does not click
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
	}

	bool AudioDevice::channelRegister(Address offset, u32*& field, Channel*& c)
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

	u32 AudioDevice::read(Address offset)
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

	void AudioDevice::write(Address offset, u32 value)
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

	const RegisterMap& AudioDevice::registers() const
	{
		static constexpr RegisterMap map{ "audio", Registers };
		return map;
	}
}
