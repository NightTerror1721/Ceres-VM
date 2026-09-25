// The audio (AudioDevice): the device on its own, and a program reaching it through its registers.
#include "device_test_machine.h"

// --- A tone generator --------------------------------------------------------------------------

TEST(audio, without_a_host_to_play_it_a_tone_is_never_busy)
{
	AudioDevice audio{};
	audio.write(AudioDevice::FrequencyRegister, 440);
	audio.write(AudioDevice::DurationRegister, 100);
	audio.write(AudioDevice::CommandRegister, AudioDevice::CommandPlay);

	// A program waiting for the busy bit to clear must not wait for speakers that are not there.
	CHECK_EQ(audio.read(AudioDevice::StatusRegister), 0u);
}

TEST(audio, a_tone_reaches_the_host_with_what_the_program_set)
{
	AudioDevice audio{};
	std::optional<AudioDevice::Tone> heard;
	audio.setToneSink([&](const std::optional<AudioDevice::Tone>& tone) { heard = tone; });

	audio.write(AudioDevice::FrequencyRegister, 880);
	audio.write(AudioDevice::DurationRegister, 250);
	audio.write(AudioDevice::VolumeRegister, 200);
	audio.write(AudioDevice::WaveformRegister, AudioDevice::Triangle);
	CHECK(!heard.has_value()); // Nothing plays until the command

	audio.write(AudioDevice::CommandRegister, AudioDevice::CommandPlay);

	CHECK(heard.has_value());
	CHECK_EQ(heard->frequency, 880u);
	CHECK_EQ(heard->durationMs, 250u);
	CHECK_EQ(heard->volume, 200u);
	CHECK_EQ(static_cast<u32>(heard->waveform), static_cast<u32>(AudioDevice::Triangle));
	CHECK_EQ(audio.read(AudioDevice::StatusRegister), AudioDevice::StatusBusy);
}

TEST(audio, stop_silences_the_host_and_clears_the_busy_bit_without_an_interrupt)
{
	Machine m{ Instruction::STI(), Instruction::NOP(), Instruction::NOP() };

	AudioDevice audio{};
	audio.attachTo(m.vm().io());
	bool silenced = false;
	audio.setToneSink([&](const std::optional<AudioDevice::Tone>& tone) { silenced = !tone.has_value(); });
	m.installHandler(AudioDevice::Interrupt, Address(0x800), { Instruction::LI(9, 0x77), Instruction::IRET() });

	audio.write(AudioDevice::CommandRegister, AudioDevice::CommandPlay);
	audio.write(AudioDevice::CommandRegister, AudioDevice::CommandStop);
	m.step(3);

	CHECK(silenced);
	CHECK(!audio.isBusy());
	CHECK_EQ(m.reg(9), 0u);
}

TEST(audio, a_tone_that_ends_clears_busy_and_raises_the_interrupt_once)
{
	Machine m{ Instruction::STI(), Instruction::HALT(), Instruction::LI(10, 0x42) };

	AudioDevice audio{};
	audio.attachTo(m.vm().io());
	audio.setToneSink([](const std::optional<AudioDevice::Tone>&) {});
	m.installHandler(AudioDevice::Interrupt, Address(0x800), { Instruction::ADDI(9, 9, 1), Instruction::IRET() });

	audio.write(AudioDevice::CommandRegister, AudioDevice::CommandPlay);
	CHECK(audio.isBusy());

	m.step(2);
	CHECK(m.flags().halting());

	audio.toneFinished();
	audio.toneFinished(); // The host reporting it twice must not raise it twice
	m.step(6);

	CHECK(!audio.isBusy());
	CHECK_EQ(m.reg(9), 1u);
	CHECK_EQ(m.reg(10), 0x42u);
}

TEST(audio, a_channel_rises_holds_and_falls_through_its_envelope)
{
	AudioDevice audio{};
	using A = AudioDevice;
	const u32 base = A::ChannelBase + 2 * A::ChannelStride;   // channel 2
	audio.write(Address(base + A::ChannelFrequency), 1000);
	audio.write(Address(base + A::ChannelVolume), 255);
	audio.write(Address(base + A::ChannelWaveform), A::Square);
	audio.write(Address(base + A::ChannelAttack), 10);     // 10 ms at 10 kHz: 100 samples
	audio.write(Address(base + A::ChannelDecay), 10);
	audio.write(Address(base + A::ChannelSustain), 128);
	audio.write(Address(base + A::ChannelRelease), 10);
	CHECK_EQ(audio.read(A::ChannelCountRegister), 4u);
	CHECK_EQ(audio.read(A::ChannelStatusRegister), 0u);
	audio.write(A::ChannelCommandRegister, (2u << 8) | A::ChannelKeyOn);
	CHECK_EQ(audio.read(A::ChannelStatusRegister), 0u);   // no host renders it: nothing to wait for
	bool asked = false;
	audio.setChannelWake([&] { asked = true; return false; });                 // a host whose sound failed to open
	audio.write(A::ChannelCommandRegister, (2u << 8) | A::ChannelKeyOn);
	CHECK(asked);
	CHECK_EQ(audio.read(A::ChannelStatusRegister), 0u);   // asked, and told nobody renders it
	bool woken = false;
	audio.setChannelWake([&] { woken = true; return true; });
	audio.write(A::ChannelCommandRegister, (2u << 8) | A::ChannelKeyOn);
	CHECK(woken);
	CHECK_EQ(audio.read(A::ChannelStatusRegister), 4u);   // bit 2

	std::vector<float> out(1000, 0.0f);
	audio.renderChannels(out.data(), 50, 10000);                 // halfway up the attack
	CHECK(std::abs(audio.channel(2).level - 0.5f) < 0.05f);
	std::fill(out.begin(), out.end(), 0.0f);                      // it adds to what is there
	audio.renderChannels(out.data(), 300, 10000);                 // attack and decay done: sustaining
	CHECK(audio.channel(2).stage == A::Channel::Sustain);
	CHECK(std::abs(audio.channel(2).level - 128.0f / 255.0f) < 0.01f);
	float peak = 0.0f;
	for (float s : out) peak = std::max(peak, std::abs(s));
	CHECK(peak > 0.1f && peak <= 0.21f);                          // a square at full volume, with headroom

	audio.write(A::ChannelCommandRegister, (2u << 8) | A::ChannelKeyOff);
	audio.renderChannels(out.data(), 200, 10000);                 // the release runs out
	CHECK(audio.channel(2).stage == A::Channel::Off);
	CHECK_EQ(audio.read(A::ChannelStatusRegister), 0u);

	audio.write(Address(base + A::ChannelDuty), 0);          // clamped to 1
	CHECK_EQ(audio.read(Address(base + A::ChannelDuty)), 1u);
	audio.write(Address(base + A::ChannelWaveform), 99);      // a typo keeps the one there
	CHECK_EQ(audio.read(Address(base + A::ChannelWaveform)), static_cast<u32>(A::Square));
	audio.write(A::ChannelCommandRegister, (2u << 8) | A::ChannelKeyOn);
	audio.reset();                                                // a reset silences every channel
	CHECK_EQ(audio.read(A::ChannelStatusRegister), 0u);
}

TEST(audio, the_tone_registers_are_clamped_and_a_typo_waveform_is_ignored)
{
	AudioDevice audio{};
	audio.write(AudioDevice::FrequencyRegister, 5);
	CHECK_EQ(audio.read(AudioDevice::FrequencyRegister), AudioDevice::MinFrequency);
	audio.write(AudioDevice::FrequencyRegister, 99999);
	CHECK_EQ(audio.read(AudioDevice::FrequencyRegister), AudioDevice::MaxFrequency);
	audio.write(AudioDevice::VolumeRegister, 1000);
	CHECK_EQ(audio.read(AudioDevice::VolumeRegister), 255u);

	audio.write(AudioDevice::WaveformRegister, AudioDevice::Sine);
	audio.write(AudioDevice::WaveformRegister, 77);
	CHECK_EQ(audio.read(AudioDevice::WaveformRegister), static_cast<u32>(AudioDevice::Sine));
}

TEST(audio, an_audio_slot_nobody_attached_reads_all_ones_so_a_program_can_tell)
{
	Machine m{
		LoadBase(default_mmio::Audio), LoadBaseLow(default_mmio::Audio),
		Instruction::LDR(1, Base, Off(AudioDevice::StatusRegister)),
	};
	m.step(3);
	CHECK_EQ(m.reg(1), 0xFFFFFFFFu);

	Machine attached{
		LoadBase(default_mmio::Audio), LoadBaseLow(default_mmio::Audio),
		Instruction::LDR(1, Base, Off(AudioDevice::StatusRegister)),
	};
	AudioDevice audio{};
	audio.attachTo(attached.vm().io());
	attached.step(3);
	CHECK_EQ(attached.reg(1), 0u);
}
