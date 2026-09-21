// What the standard library asked of the machine: the end-of-input flag, an exit status, an
// STI that takes effect a step late, the capacities a program could not learn, a millisecond
// clock, colour in the text grid, an optional fault on division by zero, typed characters and a
// tone generator. Every one of them is opt-in or an addition: nothing here changes what a program
// written before them does.

#include "framework.h"
#include <ceres/vm/ceresvm.h>
#include <ceres/devices/devices.h>
#include <ceres/devices/storage_devices.h>
#include <ceres/devices/input_devices.h>
#include <ceres/devices/audio_device.h>
#include <ceres/vm/bios.h>
#include <ceres/core/format/memory_map.h>
#include <optional>
#include <string>
#include <string_view>

using namespace ceres;
using namespace ceres::vm;
using namespace ceres::devices;
using namespace ceres::fmt;
using namespace ceres::testing;

namespace
{
	class Machine
	{
	private:
		CeresVM _vm;

	public:
		explicit Machine(std::initializer_list<Instruction> program)
		{
			const Address entry = Memory::UnrestrictedSegmentStart;

			usize offset = 0;
			for (Instruction instruction : program)
			{
				_vm.memory().writeUnchecked<u32>(entry + Address(static_cast<u32>(offset)), instruction.raw());
				offset += Instruction::Size;
			}

			BIOS bios{};
			bios.initializeMemory(_vm.memory());
			_vm.memory().writeUnchecked<u32>(0_addr, entry.value());
			_vm.engine().reset();
		}

		void step(usize count = 1)
		{
			for (usize i = 0; i < count; ++i)
				_vm.engine().step();
		}

		CeresVM& vm() noexcept { return _vm; }
		u32 reg(usize index) const { return _vm.engine().registers().getValue(index); }
		const FlagRegister& flags() const { return _vm.engine().flags(); }
		Memory& memory() { return _vm.memory(); }

		void installHandler(InterruptNumber number, Address at, std::initializer_list<Instruction> handler)
		{
			usize offset = 0;
			for (Instruction instruction : handler)
			{
				_vm.memory().writeUnchecked<u32>(at + Address(static_cast<u32>(offset)), instruction.raw());
				offset += Instruction::Size;
			}
			_vm.memory().writeUnchecked<u32>(Address(static_cast<u32>(number) * Address::Size), at.value());
		}
	};

	constexpr u8 Base = 13;
	constexpr u16 Hi(Address address) noexcept { return static_cast<u16>(address.value() >> 16); }
	constexpr u16 Lo(Address address) noexcept { return static_cast<u16>(address.value() & 0xFFFF); }
	Instruction LoadBase(Address address) noexcept { return Instruction::LUI(Base, Hi(address)); }
	Instruction LoadBaseLow(Address address) noexcept { return Instruction::ORI(Base, Base, Lo(address)); }
	u16 Off(Address registerOffset) noexcept { return static_cast<u16>(registerOffset.value()); }

	constexpr u32 SourceBuffer = 0x1000;

	void fill(Memory& memory, u32 address, std::string_view bytes)
	{
		for (u32 i = 0; i < bytes.size(); ++i)
			memory.writeUnchecked<u8>(Address(address + i), static_cast<u8>(bytes[i]));
	}
}

// --- The terminal reports the end of its input --------------------------------------------------

TEST(device_extras, an_open_terminal_never_reports_the_end_of_input)
{
	TerminalDevice terminal{};
	CHECK_EQ(terminal.readUnsignedByte(TerminalDevice::StatusRegister) & TerminalDevice::StatusEndOfInput, 0u);
	CHECK(!terminal.isInputClosed());
}

TEST(device_extras, a_closed_terminal_reports_the_end_only_once_its_input_is_drained)
{
	TerminalDevice terminal{};
	terminal.pushInput("ab");
	terminal.closeInput();

	// Data first: the program still has bytes to read, so this is not the end yet.
	u32 status = terminal.readUnsignedByte(TerminalDevice::StatusRegister);
	CHECK_EQ(status & TerminalDevice::StatusInputAvailable, TerminalDevice::StatusInputAvailable);
	CHECK_EQ(status & TerminalDevice::StatusEndOfInput, 0u);

	CHECK_EQ(terminal.readUnsignedByte(TerminalDevice::InputRegister), u8{ 'a' });
	CHECK_EQ(terminal.readUnsignedByte(TerminalDevice::InputRegister), u8{ 'b' });

	status = terminal.readUnsignedByte(TerminalDevice::StatusRegister);
	CHECK_EQ(status & TerminalDevice::StatusInputAvailable, 0u);
	CHECK_EQ(status & TerminalDevice::StatusEndOfInput, TerminalDevice::StatusEndOfInput);
	// Output stays ready: closing the input says nothing about the screen.
	CHECK_EQ(status & TerminalDevice::StatusOutputReady, TerminalDevice::StatusOutputReady);
}

TEST(device_extras, closing_the_input_wakes_a_halted_machine)
{
	Machine m{ Instruction::STI(), Instruction::HALT(), Instruction::LI(10, 0x21) };

	TerminalDevice terminal{};
	terminal.attachTo(m.vm().io());
	m.installHandler(TerminalDevice::Interrupt, Address(0x800), {
		Instruction::LI(9, 0x99),
		Instruction::IRET(),
	});

	m.step(2);
	CHECK(m.flags().halting());

	terminal.closeInput();
	m.step(4);

	CHECK_EQ(m.reg(9), 0x99u);
	CHECK_EQ(m.reg(10), 0x21u);
}

TEST(device_extras, a_snapshot_of_the_terminal_remembers_that_its_input_was_closed)
{
	TerminalDevice terminal{};
	terminal.closeInput();
	const auto state = terminal.captureState();
	CHECK(state.closed);

	TerminalDevice other{};
	other.restoreState(state);
	CHECK(other.isInputClosed());
}

// --- An exit status ----------------------------------------------------------------------------

TEST(device_extras, a_word_written_to_the_control_register_carries_the_exit_status)
{
	Machine m{
		LoadBase(default_mmio::SystemControl), LoadBaseLow(default_mmio::SystemControl),
		Instruction::LI(1, 0x0501),
		Instruction::STR(Base, 1, Off(SystemControlDevice::CommandRegister)),
	};

	bool shutDown = false;
	SystemControlDevice control{ [&] { shutDown = true; }, {} };
	control.attachTo(m.vm().io());
	m.step(4);

	CHECK(shutDown);
	CHECK_EQ(control.exitCode(), u8{ 5 });
}

TEST(device_extras, a_plain_byte_shutdown_is_still_status_zero)
{
	Machine m{
		LoadBase(default_mmio::SystemControl), LoadBaseLow(default_mmio::SystemControl),
		Instruction::LI(1, 1),
		Instruction::STRB(Base, 1, Off(SystemControlDevice::CommandRegister)),
	};

	bool shutDown = false;
	SystemControlDevice control{ [&] { shutDown = true; }, {} };
	control.attachTo(m.vm().io());
	m.step(4);

	CHECK(shutDown);
	CHECK_EQ(control.exitCode(), u8{ 0 });
}

TEST(device_extras, a_halfword_shutdown_carries_the_status_too)
{
	SystemControlDevice control{ [] {}, {} };
	control.writeHalfword(SystemControlDevice::CommandRegister, 0x2A01);
	CHECK_EQ(control.exitCode(), u8{ 42 });
}

TEST(device_extras, a_reset_does_not_change_the_status)
{
	bool reset = false;
	SystemControlDevice control{ [] {}, [&] { reset = true; } };
	control.writeWord(SystemControlDevice::CommandRegister, 0x0702);
	CHECK(reset);
	CHECK_EQ(control.exitCode(), u8{ 0 });
}

// --- What the machine has ----------------------------------------------------------------------

TEST(device_extras, the_control_device_reports_how_much_ram_there_is)
{
	Machine m{
		LoadBase(default_mmio::SystemControl), LoadBaseLow(default_mmio::SystemControl),
		Instruction::LDR(1, Base, Off(SystemControlDevice::MemorySizeRegister)),
	};

	SystemControlDevice control{};
	control.attachTo(m.vm().io());
	m.step(3);

	CHECK_EQ(m.reg(1), static_cast<u32>(m.memory().size()));
}

TEST(device_extras, the_control_register_stays_unreadable_and_unknown_offsets_read_all_ones)
{
	SystemControlDevice control{};
	CHECK_EQ(control.readUnsignedWord(SystemControlDevice::CommandRegister), 0xFFFFFFFFu);
	CHECK_EQ(control.readUnsignedWord(Address(0x40)), 0xFFFFFFFFu);
	CHECK_EQ(control.readUnsignedByte(Address(0x40)), u8{ 0xFF });
}

TEST(device_extras, the_disk_reports_how_many_sectors_it_has)
{
	DiskDevice disk{ 10 };
	CHECK_EQ(disk.readUnsignedWord(DiskDevice::SectorCountRegister), 10u);

	DiskDevice big{ 1000 };
	CHECK_EQ(big.readUnsignedWord(DiskDevice::SectorCountRegister), 1000u);
}

TEST(device_extras, the_features_register_holds_what_was_written_and_tells_the_host)
{
	SystemControlDevice control{};
	u32 seen = 0;
	control.setFeaturesCallback([&](u32 features) { seen = features; });

	CHECK_EQ(control.features(), 0u);
	control.writeWord(SystemControlDevice::FeaturesRegister, SystemControlDevice::FeatureDivisionFault);

	CHECK_EQ(seen, SystemControlDevice::FeatureDivisionFault);
	CHECK_EQ(control.readUnsignedWord(SystemControlDevice::FeaturesRegister), SystemControlDevice::FeatureDivisionFault);
}

// --- STI takes effect one instruction late ------------------------------------------------------

TEST(device_extras, sti_then_halt_cannot_lose_an_interrupt_that_arrives_between_them)
{
	// The timer expires on the very step that runs STI. Before, the interrupt was serviced before
	// the HALT ran, the handler returned to the HALT, and the machine slept with nothing left to
	// wake it.
	Machine m{
		Instruction::STI(),
		Instruction::HALT(),
		Instruction::LI(10, 0x33),
	};

	TimerDevice timer{};
	timer.attachTo(m.vm().io());
	timer.arm(1);

	m.installHandler(TimerDevice::Interrupt, Address(0x800), {
		Instruction::LI(9, 0x5A),
		Instruction::IRET(),
	});

	m.step(5);

	CHECK_EQ(m.reg(9), 0x5Au);
	CHECK_EQ(m.reg(10), 0x33u); // Woke, and went on past the HALT
	CHECK(!m.flags().halting());
}

TEST(device_extras, the_instruction_after_sti_runs_before_a_pending_interrupt_is_delivered)
{
	Machine m{
		Instruction::STI(),
		Instruction::LI(1, 1),
		Instruction::LI(2, 2),
	};

	TimerDevice timer{};
	timer.attachTo(m.vm().io());
	timer.arm(1);

	m.installHandler(TimerDevice::Interrupt, Address(0x800), {
		Instruction::LI(9, 0x5A),
		Instruction::IRET(),
	});

	m.step(2);
	CHECK_EQ(m.reg(1), 1u);
	CHECK_EQ(m.reg(9), 0u); // Not yet

	m.step(1);
	CHECK_EQ(m.reg(9), 0x5Au); // Delivered on the step after
}

TEST(device_extras, an_interrupt_already_enabled_is_not_delayed)
{
	// Only the STI itself opens the window: with the flag set already, delivery is immediate.
	Machine m{
		Instruction::STI(),
		Instruction::NOP(),
		Instruction::NOP(),
		Instruction::NOP(),
		Instruction::NOP(),
	};

	TimerDevice timer{};
	timer.attachTo(m.vm().io());

	m.installHandler(TimerDevice::Interrupt, Address(0x800), {
		Instruction::LI(9, 0x5A),
		Instruction::IRET(),
	});

	m.step(3);
	timer.arm(1);
	m.step(2);

	CHECK_EQ(m.reg(9), 0x5Au);
}

// --- Division by zero, as an option ------------------------------------------------------------

TEST(device_extras, division_by_zero_only_sets_the_trap_flag_unless_asked_for_more)
{
	Machine m{
		Instruction::LI(1, 7),
		Instruction::LI(2, 0),
		Instruction::LI(3, 99),
		Instruction::DIV(3, 1, 2),
		Instruction::LI(4, 4),
	};

	m.installHandler(InterruptNumber::DivisionByZero, Address(0x800), {
		Instruction::LI(8, 0xD),
		Instruction::IRET(),
	});

	m.step(6);

	CHECK(m.flags().trap());
	CHECK_EQ(m.reg(3), 99u);
	CHECK_EQ(m.reg(8), 0u);
	CHECK_EQ(m.reg(4), 4u);
}

TEST(device_extras, division_by_zero_can_raise_its_own_interrupt)
{
	Machine m{
		Instruction::LI(1, 7),
		Instruction::LI(2, 0),
		Instruction::LI(3, 99),
		Instruction::DIV(3, 1, 2),
		Instruction::LI(4, 4),
	};

	m.vm().engine().setDivisionFaults(true);
	m.installHandler(InterruptNumber::DivisionByZero, Address(0x800), {
		Instruction::LI(8, 0xD),
		Instruction::IRET(),
	});

	m.step(8);

	CHECK_EQ(m.reg(8), 0xDu);  // The handler ran
	CHECK_EQ(m.reg(3), 99u);   // The destination was left alone
	CHECK_EQ(m.reg(4), 4u);    // And it came back to the instruction after the division
	CHECK(!m.flags().trap());
}

TEST(device_extras, every_division_shaped_instruction_honours_the_option)
{
	// A remainder and a float division take the same road as DIV.
	Machine m{
		Instruction::LI(1, 7),
		Instruction::LI(2, 0),
		Instruction::MOD(3, 1, 2),
		Instruction::ITOF(0, 1),
		Instruction::ITOF(1, 2),
		Instruction::FDIV(2, 0, 1),
		Instruction::LI(4, 4),
	};

	m.vm().engine().setDivisionFaults(true);
	m.installHandler(InterruptNumber::DivisionByZero, Address(0x800), {
		Instruction::ADDI(8, 8, 1),
		Instruction::IRET(),
	});

	m.step(12);

	CHECK_EQ(m.reg(8), 2u);
	CHECK_EQ(m.reg(4), 4u);
}

TEST(device_extras, a_program_switches_the_option_on_through_the_control_device)
{
	Machine m{
		LoadBase(default_mmio::SystemControl), LoadBaseLow(default_mmio::SystemControl),
		Instruction::LI(1, static_cast<u16>(SystemControlDevice::FeatureDivisionFault)),
		Instruction::STR(Base, 1, Off(SystemControlDevice::FeaturesRegister)),
	};

	SystemControlDevice control{};
	control.setFeaturesCallback([&](u32 features)
	{
		m.vm().engine().setDivisionFaults((features & SystemControlDevice::FeatureDivisionFault) != 0);
	});
	control.attachTo(m.vm().io());
	m.step(4);

	CHECK(m.vm().engine().divisionFaults());
}

// --- A millisecond clock -----------------------------------------------------------------------

TEST(device_extras, the_millisecond_register_never_goes_backwards)
{
	TimerDevice timer{};
	const u32 first = timer.readUnsignedWord(TimerDevice::MillisRegister);
	const u32 second = timer.readUnsignedWord(TimerDevice::MillisRegister);
	CHECK(second >= first);
	CHECK(first < 60000u); // Counts from the machine's start, not the epoch
}

TEST(device_extras, the_millisecond_clock_can_be_replaced_for_a_replay)
{
	TimerDevice timer{};
	timer.setMillisSource([] { return u32{ 1234 }; });
	CHECK_EQ(timer.readUnsignedWord(TimerDevice::MillisRegister), 1234u);

	timer.clearMillisSource();
	CHECK(timer.readUnsignedWord(TimerDevice::MillisRegister) != 1234u);
}

// --- Colour in the text grid -------------------------------------------------------------------

TEST(device_extras, a_grid_without_colour_is_shown_as_plain_text)
{
	FramebufferDevice framebuffer{};
	framebuffer.writeWord(FramebufferDevice::WidthRegister, 3);
	framebuffer.writeWord(FramebufferDevice::HeightRegister, 1);
	framebuffer.writeWord(FramebufferDevice::CommandRegister, FramebufferDevice::CommandClear);
	for (char c : std::string_view{ "abc" })
		framebuffer.writeWord(FramebufferDevice::DataRegister, static_cast<u8>(c));

	std::string shown;
	framebuffer.setPresentSink([&](std::string_view frame) { shown = frame; });
	framebuffer.writeWord(FramebufferDevice::CommandRegister, FramebufferDevice::CommandPresent);

	CHECK_EQ(shown, std::string{ "abc\n" });
	CHECK(!framebuffer.hasAttributes());
}

TEST(device_extras, a_cells_attribute_sits_above_its_character_in_the_data_word)
{
	FramebufferDevice framebuffer{};
	framebuffer.writeWord(FramebufferDevice::WidthRegister, 3);
	framebuffer.writeWord(FramebufferDevice::HeightRegister, 1);
	framebuffer.writeWord(FramebufferDevice::CommandRegister, FramebufferDevice::CommandClear);

	// Blue on red, blue on red, then the terminal's own colours.
	framebuffer.writeWord(FramebufferDevice::DataRegister, (0x14u << FramebufferDevice::AttributeShift) | 'a');
	framebuffer.writeWord(FramebufferDevice::DataRegister, (0x14u << FramebufferDevice::AttributeShift) | 'b');
	framebuffer.writeWord(FramebufferDevice::DataRegister, 'c');

	std::string shown;
	framebuffer.setPresentSink([&](std::string_view frame) { shown = frame; });
	framebuffer.writeWord(FramebufferDevice::CommandRegister, FramebufferDevice::CommandPresent);

	CHECK_EQ(shown, std::string{ "\x1b[34;41mab\x1b[0mc\n" });
	CHECK(framebuffer.hasAttributes());
}

TEST(device_extras, a_row_never_leaves_the_terminal_painted_and_bright_colours_use_the_high_codes)
{
	FramebufferDevice framebuffer{};
	framebuffer.writeWord(FramebufferDevice::WidthRegister, 2);
	framebuffer.writeWord(FramebufferDevice::HeightRegister, 2);
	framebuffer.writeWord(FramebufferDevice::CommandRegister, FramebufferDevice::CommandClear);

	framebuffer.writeWord(FramebufferDevice::DataRegister, (0xF9u << FramebufferDevice::AttributeShift) | 'x'); // bright red on bright white
	framebuffer.writeWord(FramebufferDevice::DataRegister, (0x02u << FramebufferDevice::AttributeShift) | 'y'); // green on black
	framebuffer.writeWord(FramebufferDevice::DataRegister, 'z');
	framebuffer.writeWord(FramebufferDevice::DataRegister, 'w');

	CHECK_EQ(framebuffer.toAnsiText(), std::string{ "\x1b[91;107mx\x1b[32;40my\x1b[0m\nzw\n" });
}

TEST(device_extras, attributes_can_be_blitted_in_one_trigger_and_clearing_removes_them)
{
	Machine m{
		LoadBase(default_mmio::Framebuffer), LoadBaseLow(default_mmio::Framebuffer),
		Instruction::LI(1, 2),
		Instruction::STR(Base, 1, Off(FramebufferDevice::WidthRegister)),
		Instruction::LI(1, 1),
		Instruction::STR(Base, 1, Off(FramebufferDevice::HeightRegister)),
		Instruction::LI(1, FramebufferDevice::CommandClear),
		Instruction::STR(Base, 1, Off(FramebufferDevice::CommandRegister)),
		Instruction::LI(2, static_cast<u16>(SourceBuffer)),
		Instruction::LI(3, 2),
		Instruction::STR(Base, 2, Off(FramebufferDevice::BlockAddressRegister)),
		Instruction::STR(Base, 3, Off(FramebufferDevice::BlockLengthRegister)),
		Instruction::LI(4, FramebufferDevice::BlockCommandWrite),
		Instruction::STR(Base, 4, Off(FramebufferDevice::BlockCommandRegister)),
		Instruction::LI(2, static_cast<u16>(SourceBuffer + 16)),
		Instruction::STR(Base, 2, Off(FramebufferDevice::BlockAddressRegister)),
		Instruction::LI(4, FramebufferDevice::BlockCommandWriteAttributes),
		Instruction::STR(Base, 4, Off(FramebufferDevice::BlockCommandRegister)),
	};

	FramebufferDevice framebuffer{};
	framebuffer.attachTo(m.vm().io());

	fill(m.memory(), SourceBuffer, "hi");
	m.memory().writeUnchecked<u8>(Address(SourceBuffer + 16), 0x21);
	m.memory().writeUnchecked<u8>(Address(SourceBuffer + 17), 0x00);
	m.step(18);

	CHECK_EQ(framebuffer.toAnsiText(), std::string{ "\x1b[31;42mh\x1b[0mi\n" });
	CHECK_EQ(framebuffer.attributes()[0], u8{ 0x21 });

	framebuffer.writeWord(FramebufferDevice::CommandRegister, FramebufferDevice::CommandClear);
	CHECK(!framebuffer.hasAttributes());
}

// --- Typed characters --------------------------------------------------------------------------

TEST(device_extras, typed_text_arrives_as_code_points_apart_from_the_key_events)
{
	KeyboardDevice keyboard{};
	keyboard.pushKey(4, true); // the physical A key
	keyboard.pushText(std::string_view{ "A" });

	const u32 status = keyboard.readUnsignedWord(KeyboardDevice::StatusRegister);
	CHECK_EQ(status & KeyboardDevice::StatusDataReady, KeyboardDevice::StatusDataReady);
	CHECK_EQ(status & KeyboardDevice::StatusTextReady, KeyboardDevice::StatusTextReady);

	CHECK_EQ(keyboard.readUnsignedWord(KeyboardDevice::TextRegister), u32{ 'A' });
	CHECK_EQ(keyboard.readUnsignedWord(KeyboardDevice::StatusRegister) & KeyboardDevice::StatusTextReady, 0u);
	CHECK_EQ(keyboard.readUnsignedWord(KeyboardDevice::TextRegister), 0u); // Empty reads 0

	// The event queue was not touched by the text queue.
	CHECK_EQ(keyboard.readUnsignedWord(KeyboardDevice::EventRegister), (4u | KeyboardDevice::EventPressed));
}

TEST(device_extras, a_utf8_string_is_decoded_into_code_points)
{
	KeyboardDevice keyboard{};
	keyboard.pushText(std::string_view{ "a\xC3\xA9\xE2\x82\xAC\xF0\x9F\x98\x80" }); // a é € 😀

	CHECK_EQ(keyboard.readUnsignedWord(KeyboardDevice::TextRegister), 0x61u);
	CHECK_EQ(keyboard.readUnsignedWord(KeyboardDevice::TextRegister), 0xE9u);
	CHECK_EQ(keyboard.readUnsignedWord(KeyboardDevice::TextRegister), 0x20ACu);
	CHECK_EQ(keyboard.readUnsignedWord(KeyboardDevice::TextRegister), 0x1F600u);
	CHECK_EQ(keyboard.availableText(), usize{ 0 });
}

TEST(device_extras, a_malformed_utf8_sequence_is_skipped_not_guessed_at)
{
	KeyboardDevice keyboard{};
	// A stray continuation byte, a lead byte cut short by a plain one, then a good character.
	keyboard.pushText(std::string_view{ "\x80" "\xC3" "x" "y" });

	CHECK_EQ(keyboard.readUnsignedWord(KeyboardDevice::TextRegister), u32{ 'x' });
	CHECK_EQ(keyboard.readUnsignedWord(KeyboardDevice::TextRegister), u32{ 'y' });
	CHECK_EQ(keyboard.availableText(), usize{ 0 });
}

TEST(device_extras, typed_text_raises_the_keyboards_interrupt_and_a_full_queue_drops_it)
{
	Machine m{ Instruction::STI(), Instruction::NOP(), Instruction::NOP(), Instruction::NOP() };

	KeyboardDevice keyboard{};
	keyboard.attachTo(m.vm().io());
	m.installHandler(KeyboardDevice::Interrupt, Address(0x800), {
		Instruction::LI(9, 0x6B),
		Instruction::IRET(),
	});

	keyboard.pushText(std::string_view{ "q" });
	m.step(4);
	CHECK_EQ(m.reg(9), 0x6Bu);

	for (usize i = 0; i < KeyboardDevice::EventBufferCapacity + 10; ++i)
		keyboard.pushText(u32{ 'z' });
	CHECK_EQ(keyboard.availableText(), KeyboardDevice::EventBufferCapacity - 1);
	CHECK(keyboard.droppedEvents() > 0);
}

// --- A tone generator --------------------------------------------------------------------------

TEST(device_extras, without_a_host_to_play_it_a_tone_is_never_busy)
{
	AudioDevice audio{};
	audio.writeWord(AudioDevice::FrequencyRegister, 440);
	audio.writeWord(AudioDevice::DurationRegister, 100);
	audio.writeWord(AudioDevice::CommandRegister, AudioDevice::CommandPlay);

	// A program waiting for the busy bit to clear must not wait for speakers that are not there.
	CHECK_EQ(audio.readUnsignedWord(AudioDevice::StatusRegister), 0u);
}

TEST(device_extras, a_tone_reaches_the_host_with_what_the_program_set)
{
	AudioDevice audio{};
	std::optional<AudioDevice::Tone> heard;
	audio.setToneSink([&](const std::optional<AudioDevice::Tone>& tone) { heard = tone; });

	audio.writeWord(AudioDevice::FrequencyRegister, 880);
	audio.writeWord(AudioDevice::DurationRegister, 250);
	audio.writeWord(AudioDevice::VolumeRegister, 200);
	audio.writeWord(AudioDevice::WaveformRegister, AudioDevice::Triangle);
	CHECK(!heard.has_value()); // Nothing plays until the command

	audio.writeWord(AudioDevice::CommandRegister, AudioDevice::CommandPlay);

	CHECK(heard.has_value());
	CHECK_EQ(heard->frequency, 880u);
	CHECK_EQ(heard->durationMs, 250u);
	CHECK_EQ(heard->volume, 200u);
	CHECK_EQ(static_cast<u32>(heard->waveform), static_cast<u32>(AudioDevice::Triangle));
	CHECK_EQ(audio.readUnsignedWord(AudioDevice::StatusRegister), AudioDevice::StatusBusy);
}

TEST(device_extras, stop_silences_the_host_and_clears_the_busy_bit_without_an_interrupt)
{
	Machine m{ Instruction::STI(), Instruction::NOP(), Instruction::NOP() };

	AudioDevice audio{};
	audio.attachTo(m.vm().io());
	bool silenced = false;
	audio.setToneSink([&](const std::optional<AudioDevice::Tone>& tone) { silenced = !tone.has_value(); });
	m.installHandler(AudioDevice::Interrupt, Address(0x800), { Instruction::LI(9, 0x77), Instruction::IRET() });

	audio.writeWord(AudioDevice::CommandRegister, AudioDevice::CommandPlay);
	audio.writeWord(AudioDevice::CommandRegister, AudioDevice::CommandStop);
	m.step(3);

	CHECK(silenced);
	CHECK(!audio.isBusy());
	CHECK_EQ(m.reg(9), 0u);
}

TEST(device_extras, a_tone_that_ends_clears_busy_and_raises_the_interrupt_once)
{
	Machine m{ Instruction::STI(), Instruction::HALT(), Instruction::LI(10, 0x42) };

	AudioDevice audio{};
	audio.attachTo(m.vm().io());
	audio.setToneSink([](const std::optional<AudioDevice::Tone>&) {});
	m.installHandler(AudioDevice::Interrupt, Address(0x800), { Instruction::ADDI(9, 9, 1), Instruction::IRET() });

	audio.writeWord(AudioDevice::CommandRegister, AudioDevice::CommandPlay);
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

TEST(device_extras, the_tone_registers_are_clamped_and_a_typo_waveform_is_ignored)
{
	AudioDevice audio{};
	audio.writeWord(AudioDevice::FrequencyRegister, 5);
	CHECK_EQ(audio.readUnsignedWord(AudioDevice::FrequencyRegister), AudioDevice::MinFrequency);
	audio.writeWord(AudioDevice::FrequencyRegister, 99999);
	CHECK_EQ(audio.readUnsignedWord(AudioDevice::FrequencyRegister), AudioDevice::MaxFrequency);
	audio.writeWord(AudioDevice::VolumeRegister, 1000);
	CHECK_EQ(audio.readUnsignedWord(AudioDevice::VolumeRegister), 255u);

	audio.writeWord(AudioDevice::WaveformRegister, AudioDevice::Sine);
	audio.writeWord(AudioDevice::WaveformRegister, 77);
	CHECK_EQ(audio.readUnsignedWord(AudioDevice::WaveformRegister), static_cast<u32>(AudioDevice::Sine));
}

TEST(device_extras, an_audio_slot_nobody_attached_reads_all_ones_so_a_program_can_tell)
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

// --- Keystrokes, in the order they were typed --------------------------------------------------

TEST(device_extras, keystrokes_keep_the_order_the_text_and_the_named_keys_were_typed_in)
{
	KeyboardDevice keyboard{};
	keyboard.pushText(u32{ 'a' });
	keyboard.pushKey(scancode::Return, true);
	keyboard.pushText(u32{ 'b' });
	keyboard.pushKey(scancode::Up, true);

	CHECK_EQ(keyboard.readUnsignedWord(KeyboardDevice::StatusRegister) & KeyboardDevice::StatusKeyReady, KeyboardDevice::StatusKeyReady);
	CHECK_EQ(keyboard.readUnsignedWord(KeyboardDevice::KeyRegister), u32{ 'a' });
	CHECK_EQ(keyboard.readUnsignedWord(KeyboardDevice::KeyRegister), KeyboardDevice::KeyNamed | scancode::Return);
	CHECK_EQ(keyboard.readUnsignedWord(KeyboardDevice::KeyRegister), u32{ 'b' });
	CHECK_EQ(keyboard.readUnsignedWord(KeyboardDevice::KeyRegister), KeyboardDevice::KeyNamed | scancode::Up);
	CHECK_EQ(keyboard.readUnsignedWord(KeyboardDevice::StatusRegister) & KeyboardDevice::StatusKeyReady, 0u);
	CHECK_EQ(keyboard.readUnsignedWord(KeyboardDevice::KeyRegister), 0u);   // empty reads 0
}

TEST(device_extras, only_a_press_of_a_key_with_no_character_is_a_keystroke)
{
	KeyboardDevice keyboard{};
	keyboard.pushKey(4, true);                    // the A key: its character comes as text, not from here
	keyboard.pushKey(4, false);
	keyboard.pushKey(scancode::Escape, false);    // a release is not a keystroke
	CHECK_EQ(keyboard.availableKeys(), usize{ 0 });

	keyboard.pushKey(scancode::Escape, true);
	keyboard.pushKey(scancode::F1, true);
	keyboard.pushKey(scancode::F12, true);
	keyboard.pushKey(scancode::Delete, true);
	CHECK_EQ(keyboard.readUnsignedWord(KeyboardDevice::KeyRegister), KeyboardDevice::KeyNamed | scancode::Escape);
	CHECK_EQ(keyboard.readUnsignedWord(KeyboardDevice::KeyRegister), KeyboardDevice::KeyNamed | scancode::F1);
	CHECK_EQ(keyboard.readUnsignedWord(KeyboardDevice::KeyRegister), KeyboardDevice::KeyNamed | scancode::F12);
	CHECK_EQ(keyboard.readUnsignedWord(KeyboardDevice::KeyRegister), KeyboardDevice::KeyNamed | scancode::Delete);
}

TEST(device_extras, a_control_code_typed_as_text_is_not_a_keystroke)
{
	KeyboardDevice keyboard{};
	keyboard.pushText(u32{ 7 });
	keyboard.pushText(u32{ 127 });
	CHECK_EQ(keyboard.availableKeys(), usize{ 0 });
	keyboard.pushText(u32{ 0x20AC });
	CHECK_EQ(keyboard.readUnsignedWord(KeyboardDevice::KeyRegister), 0x20ACu);
}

TEST(device_extras, the_keystroke_sink_hears_each_keystroke_and_a_full_queue_drops_but_still_reports)
{
	KeyboardDevice keyboard{};
	std::string heard;
	keyboard.setKeystrokeSink([&](u32 keystroke) { heard += keystrokeToTerminalBytes(keystroke); });
	keyboard.pushText(std::string_view{ "hi" });
	keyboard.pushKey(scancode::Return, true);
	CHECK_EQ(heard, std::string{ "hi\n" });

	for (usize i = 0; i < KeyboardDevice::EventBufferCapacity + 10; ++i)
		keyboard.pushText(u32{ 'z' });
	CHECK_EQ(keyboard.availableKeys(), KeyboardDevice::EventBufferCapacity - 1);
	CHECK_EQ(heard.size(), usize{ 3 + KeyboardDevice::EventBufferCapacity + 10 });
}

TEST(device_extras, keystrokes_as_terminal_bytes)
{
	CHECK_EQ(keystrokeToTerminalBytes(u32{ 'x' }), std::string{ "x" });
	CHECK_EQ(keystrokeToTerminalBytes(0xE9u), std::string{ "\xC3\xA9" });
	CHECK_EQ(keystrokeToTerminalBytes(0x20ACu), std::string{ "\xE2\x82\xAC" });
	CHECK_EQ(keystrokeToTerminalBytes(0x1F600u), std::string{ "\xF0\x9F\x98\x80" });
	const auto named = [](u32 code) { return keystrokeToTerminalBytes(KeyboardDevice::KeyNamed | code); };
	CHECK_EQ(named(scancode::Return), std::string{ "\n" });
	CHECK_EQ(named(scancode::Escape), std::string{ "\x1b" });
	CHECK_EQ(named(scancode::Backspace), std::string{ "\b" });
	CHECK_EQ(named(scancode::Up), std::string{ "\x1b[A" });
	CHECK_EQ(named(scancode::Left), std::string{ "\x1b[D" });
	CHECK_EQ(named(scancode::PageDown), std::string{ "\x1b[6~" });
	CHECK_EQ(named(scancode::F1), std::string{});     // no byte form
}

// --- The terminal's raw-keys request -----------------------------------------------------------

TEST(device_extras, a_terminal_with_no_host_behind_it_grants_no_raw_keys)
{
	TerminalDevice terminal{};
	terminal.writeWord(TerminalDevice::ModeRegister, TerminalDevice::ModeRaw);
	CHECK(terminal.rawRequested());
	CHECK_EQ(terminal.readUnsignedWord(TerminalDevice::ModeRegister), 0u);   // asked, and not given
}

TEST(device_extras, the_host_decides_what_a_raw_request_is_granted)
{
	TerminalDevice terminal{};
	u32 asked = 99;
	terminal.setModeHandler([&](u32 requested)
	{
		asked = requested;
		return requested ? (TerminalDevice::ModeRaw | TerminalDevice::ModeKeystrokes) : 0u;
	});

	terminal.writeWord(TerminalDevice::ModeRegister, TerminalDevice::ModeRaw);
	CHECK_EQ(asked, TerminalDevice::ModeRaw);
	CHECK_EQ(terminal.readUnsignedWord(TerminalDevice::ModeRegister), TerminalDevice::ModeRaw | TerminalDevice::ModeKeystrokes);
	CHECK(terminal.rawRequested());

	terminal.writeWord(TerminalDevice::ModeRegister, 0);
	CHECK_EQ(asked, 0u);
	CHECK_EQ(terminal.readUnsignedWord(TerminalDevice::ModeRegister), 0u);
	CHECK(!terminal.rawRequested());
}

TEST(device_extras, only_the_raw_bit_of_a_mode_write_is_a_request)
{
	TerminalDevice terminal{};
	u32 asked = 99;
	terminal.setModeHandler([&](u32 requested) { asked = requested; return requested; });
	terminal.writeWord(TerminalDevice::ModeRegister, 0xFFFFFFFEu);   // the keystrokes bit is the host's to set
	CHECK_EQ(asked, 0u);
}
