// The keyboard (KeyboardDevice): the device on its own, and a program reaching it through its registers.
#include "device_test_machine.h"

// --- The keyboard ------------------------------------------------------------------------------

TEST(keyboard, the_keyboard_reports_press_and_release_events)
{
	KeyboardDevice keyboard{};

	CHECK_EQ(keyboard.read(KeyboardDevice::StatusRegister), u32{ 0 });

	keyboard.pushKey('A', true);
	keyboard.pushKey('A', false);

	// Bits 30:0 are the code, bit 31 is the pressed flag.
	CHECK_EQ(keyboard.read(KeyboardDevice::EventRegister), u32{ 'A' | KeyboardDevice::EventPressed });
	CHECK_EQ(keyboard.read(KeyboardDevice::EventRegister), u32{ 'A' });
	CHECK_EQ(keyboard.read(KeyboardDevice::EventRegister), u32{ 0 }); // Empty again.
}

TEST(keyboard, the_keyboard_reports_how_many_events_are_available)
{
	KeyboardDevice keyboard{};

	CHECK_EQ(keyboard.availableEvents(), usize{ 0 });
	keyboard.pushKey('a');
	keyboard.pushKey('b');

	CHECK_EQ(keyboard.availableEvents(), usize{ 2 });
	CHECK_EQ(keyboard.read(KeyboardDevice::StatusRegister) & KeyboardDevice::StatusDataReady, KeyboardDevice::StatusDataReady);

	keyboard.read(KeyboardDevice::EventRegister);
	CHECK_EQ(keyboard.availableEvents(), usize{ 1 });
}

TEST(keyboard, a_keyboard_block_read_drains_events_and_counts_them)
{
	CeresVM vm{};
	KeyboardDevice keyboard{};
	keyboard.attachTo(vm.io());

	keyboard.pushKey('a');
	keyboard.pushKey('b');
	keyboard.pushKey('c');

	keyboard.write(KeyboardDevice::BlockAddressRegister, 0x3000);
	keyboard.write(KeyboardDevice::BlockLengthRegister, 8); // room for two 4-byte events
	keyboard.write(KeyboardDevice::BlockCommandRegister, KeyboardDevice::BlockCommandRead);

	CHECK_EQ(keyboard.read(KeyboardDevice::BlockReadCountRegister), u32{ 2 });
	// One 32-bit event per four bytes, little-endian: 'a' pressed is 0x80000061.
	CHECK_EQ(vm.memory().readUnchecked<u8>(Address(0x3000)), u8{ 0x61 });
	CHECK_EQ(vm.memory().readUnchecked<u8>(Address(0x3001)), u8{ 0x00 });
	CHECK_EQ(vm.memory().readUnchecked<u8>(Address(0x3002)), u8{ 0x00 });
	CHECK_EQ(vm.memory().readUnchecked<u8>(Address(0x3003)), u8{ 0x80 });
	CHECK_EQ(vm.memory().readUnchecked<u8>(Address(0x3004)), u8{ 0x62 });
	CHECK_EQ(keyboard.availableEvents(), usize{ 1 }); // 'c' remains.
}

TEST(keyboard, a_keyboard_event_keeps_a_code_wider_than_eight_bits)
{
	KeyboardDevice keyboard{};

	// SDL scancodes go past 255; the code must survive round-trip in the low 31 bits.
	keyboard.pushKey(0x12345678u, true);
	CHECK_EQ(keyboard.read(KeyboardDevice::EventRegister), u32{ 0x12345678u | KeyboardDevice::EventPressed });

	keyboard.pushKey(0x12345678u, false);
	CHECK_EQ(keyboard.read(KeyboardDevice::EventRegister), u32{ 0x12345678u });
}

TEST(keyboard, pushing_a_key_raises_the_keyboards_interrupt)
{
	Machine m{ Instruction::STI(), Instruction::NOP(), Instruction::NOP() };

	KeyboardDevice keyboard{};
	keyboard.attachTo(m.vm().io());

	m.installHandler(KeyboardDevice::Interrupt, Address(0x800), {
		Instruction::LI(9, 0x61),
		Instruction::IRET(),
	});

	keyboard.pushKey('X');
	m.step(3);

	CHECK_EQ(m.reg(9), 0x61u);
}

// --- Typed characters --------------------------------------------------------------------------

TEST(keyboard, typed_text_arrives_as_code_points_apart_from_the_key_events)
{
	KeyboardDevice keyboard{};
	keyboard.pushKey(4, true); // the physical A key
	keyboard.pushText(std::string_view{ "A" });

	const u32 status = keyboard.read(KeyboardDevice::StatusRegister);
	CHECK_EQ(status & KeyboardDevice::StatusDataReady, KeyboardDevice::StatusDataReady);
	CHECK_EQ(status & KeyboardDevice::StatusTextReady, KeyboardDevice::StatusTextReady);

	CHECK_EQ(keyboard.read(KeyboardDevice::TextRegister), u32{ 'A' });
	CHECK_EQ(keyboard.read(KeyboardDevice::StatusRegister) & KeyboardDevice::StatusTextReady, 0u);
	CHECK_EQ(keyboard.read(KeyboardDevice::TextRegister), 0u); // Empty reads 0

	// The event queue was not touched by the text queue.
	CHECK_EQ(keyboard.read(KeyboardDevice::EventRegister), (4u | KeyboardDevice::EventPressed));
}

TEST(keyboard, a_utf8_string_is_decoded_into_code_points)
{
	KeyboardDevice keyboard{};
	keyboard.pushText(std::string_view{ "a\xC3\xA9\xE2\x82\xAC\xF0\x9F\x98\x80" }); // a é € 😀

	CHECK_EQ(keyboard.read(KeyboardDevice::TextRegister), 0x61u);
	CHECK_EQ(keyboard.read(KeyboardDevice::TextRegister), 0xE9u);
	CHECK_EQ(keyboard.read(KeyboardDevice::TextRegister), 0x20ACu);
	CHECK_EQ(keyboard.read(KeyboardDevice::TextRegister), 0x1F600u);
	CHECK_EQ(keyboard.availableText(), usize{ 0 });
}

TEST(keyboard, a_malformed_utf8_sequence_is_skipped_not_guessed_at)
{
	KeyboardDevice keyboard{};
	// A stray continuation byte, a lead byte cut short by a plain one, then a good character.
	keyboard.pushText(std::string_view{ "\x80" "\xC3" "x" "y" });

	CHECK_EQ(keyboard.read(KeyboardDevice::TextRegister), u32{ 'x' });
	CHECK_EQ(keyboard.read(KeyboardDevice::TextRegister), u32{ 'y' });
	CHECK_EQ(keyboard.availableText(), usize{ 0 });
}

TEST(keyboard, a_lead_byte_no_utf8_has_is_skipped_too)
{
	// 0xF8 starts no UTF-8 sequence. Read as a three-byte lead it would swallow the two continuation bytes
	// after it and queue a character nobody typed.
	KeyboardDevice keyboard{};
	keyboard.pushText(std::string_view{ "\xF8" "\x80\x80" "z" });

	CHECK_EQ(keyboard.read(KeyboardDevice::TextRegister), u32{ 'z' });
	CHECK_EQ(keyboard.availableText(), usize{ 0 });
}

TEST(keyboard, typed_text_raises_the_keyboards_interrupt_and_a_full_queue_drops_it)
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

// --- Keystrokes, in the order they were typed --------------------------------------------------

TEST(keyboard, keystrokes_keep_the_order_the_text_and_the_named_keys_were_typed_in)
{
	KeyboardDevice keyboard{};
	keyboard.pushText(u32{ 'a' });
	keyboard.pushKey(scancode::Return, true);
	keyboard.pushText(u32{ 'b' });
	keyboard.pushKey(scancode::Up, true);

	CHECK_EQ(keyboard.read(KeyboardDevice::StatusRegister) & KeyboardDevice::StatusKeyReady, KeyboardDevice::StatusKeyReady);
	CHECK_EQ(keyboard.read(KeyboardDevice::KeyRegister), u32{ 'a' });
	CHECK_EQ(keyboard.read(KeyboardDevice::KeyRegister), KeyboardDevice::KeyNamed | scancode::Return);
	CHECK_EQ(keyboard.read(KeyboardDevice::KeyRegister), u32{ 'b' });
	CHECK_EQ(keyboard.read(KeyboardDevice::KeyRegister), KeyboardDevice::KeyNamed | scancode::Up);
	CHECK_EQ(keyboard.read(KeyboardDevice::StatusRegister) & KeyboardDevice::StatusKeyReady, 0u);
	CHECK_EQ(keyboard.read(KeyboardDevice::KeyRegister), 0u);   // empty reads 0
}

TEST(keyboard, only_a_press_of_a_key_with_no_character_is_a_keystroke)
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
	CHECK_EQ(keyboard.read(KeyboardDevice::KeyRegister), KeyboardDevice::KeyNamed | scancode::Escape);
	CHECK_EQ(keyboard.read(KeyboardDevice::KeyRegister), KeyboardDevice::KeyNamed | scancode::F1);
	CHECK_EQ(keyboard.read(KeyboardDevice::KeyRegister), KeyboardDevice::KeyNamed | scancode::F12);
	CHECK_EQ(keyboard.read(KeyboardDevice::KeyRegister), KeyboardDevice::KeyNamed | scancode::Delete);
}

TEST(keyboard, a_control_code_typed_as_text_is_not_a_keystroke)
{
	KeyboardDevice keyboard{};
	keyboard.pushText(u32{ 7 });
	keyboard.pushText(u32{ 127 });
	CHECK_EQ(keyboard.availableKeys(), usize{ 0 });
	keyboard.pushText(u32{ 0x20AC });
	CHECK_EQ(keyboard.read(KeyboardDevice::KeyRegister), 0x20ACu);
}

TEST(keyboard, the_keystroke_sink_hears_each_keystroke_and_a_full_queue_drops_but_still_reports)
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

TEST(keyboard, keystrokes_as_terminal_bytes)
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
