// The instruction word: its fields come from one table (fields.h), and it is stored low byte first on any host
// (endian.h) - plan/v2 SPEC 6.6.
#include "framework.h"
#include <ceres/core/isa/instructions.h>
#include <ceres/core/base/endian.h>
#include <array>
#include <bit>
#include <vector>

using namespace ceres;
using namespace ceres::isa;

// Where each field sits, checked by the compiler.
static_assert(fields::Opcode::Mask == 0xFF000000u && fields::Opcode::Shift == 24);
static_assert(fields::Rd::Mask == 0x00F00000u && fields::Rd::Shift == 20);
static_assert(fields::Rs::Mask == 0x000F0000u && fields::Rs::Shift == 16);
static_assert(fields::Rt::Mask == 0x0000F000u && fields::Rt::Shift == 12);
static_assert(fields::Imm8::Mask == 0x000000FFu);
static_assert(fields::Imm16::Mask == 0x0000FFFFu);
static_assert(fields::Imm20::Mask == 0x000FFFFFu);
static_assert(fields::Imm24::Mask == 0x00FFFFFFu);

// A field's set touches that field alone, and masks what does not fit.
static_assert(fields::Rd::set(0xFFFFFFFFu, 0) == 0xFF0FFFFFu);
static_assert(fields::Rd::set(0, 0x1F) == 0x00F00000u);
static_assert(fields::Imm16::get(0x1234ABCDu) == 0xABCDu);

// The builders and the accessors agree, at compile time.
static_assert(Instruction::ADD(1, 2, 3).rd() == 1 && Instruction::ADD(1, 2, 3).rs() == 2 && Instruction::ADD(1, 2, 3).rt() == 3);
static_assert(Instruction::ADD(1, 2, 3).opcode() == Opcode::ADD);
static_assert(Instruction::ADDI(4, 5, 0xBEEF).imm16() == 0xBEEF);
static_assert(Instruction::BL(7, -4).simm20() == -4 && Instruction::BL(7, -4).rd() == 7);
static_assert(Instruction::JP(i24(-8)).simm24().signedValue() == -8);

// Little-endian storage, checked by the compiler too.
static_assert(Instruction(0x11223344u).bytes() == std::array<u8, 4>{ 0x44, 0x33, 0x22, 0x11 });
static_assert([] {
	constexpr std::array<u8, 4> stored{ 0x44, 0x33, 0x22, 0x11 };
	return Instruction::fromBytes(stored.data()).raw() == 0x11223344u;
}());

namespace
{
	// A spread of real encodings: every field width in use, with values that fill them.
	std::vector<Instruction> samples()
	{
		return {
			Instruction::NOP(), Instruction::HALT(), Instruction::ADD(15, 14, 13), Instruction::ADDI(1, 2, 0xFFFF),
			Instruction::LI(3, 0x8000), Instruction::JP(i24(-4)), Instruction::JP(i24(0x7FFFFC)), Instruction::BL(12, -0x80000),
			Instruction::BL(0, 0x7FFFF), Instruction::INT(0xFF), Instruction::MCPY(1, 2, 3), Instruction::FADD(15, 0, 7),
			Instruction::STR(13, 1, static_cast<u16>(-8)), Instruction::LDRX(4, 5, 6),
		};
	}
}

TEST(instruction_fields, every_field_reads_back_what_was_set)
{
	Instruction instruction{};
	instruction.setOpcode(Opcode::ADD);
	instruction.setRd(9);
	instruction.setRs(10);
	instruction.setRt(11);
	instruction.setImm8(0xA5);
	CHECK(instruction.opcode() == Opcode::ADD);
	CHECK_EQ(instruction.rd(), u8{ 9 });
	CHECK_EQ(instruction.rs(), u8{ 10 });
	CHECK_EQ(instruction.rt(), u8{ 11 });
	CHECK_EQ(instruction.imm8(), u8{ 0xA5 });

	instruction.setImm16(0x1234);                      // overlaps rt and imm8, and replaces them
	CHECK_EQ(instruction.imm16(), u16{ 0x1234 });
	CHECK_EQ(instruction.rd(), u8{ 9 });                  // rd and rs sit above it

	instruction.setSImm20(-1);
	CHECK_EQ(instruction.simm20(), -1);
	CHECK_EQ(instruction.rd(), u8{ 9 });                  // 23:20 is left alone
}

TEST(instruction_fields, an_instruction_survives_its_bytes)
{
	for (const Instruction instruction : samples())
	{
		const auto bytes = instruction.bytes();
		CHECK_EQ(bytes[0], static_cast<u8>(instruction.raw()));               // low byte first
		CHECK_EQ(bytes[3], static_cast<u8>(instruction.raw() >> 24));
		CHECK(Instruction::fromBytes(bytes.data()) == instruction);
	}
}

TEST(instruction_fields, a_run_of_instructions_is_encoded_one_after_the_other)
{
	const std::vector<Instruction> program = samples();
	const std::vector<u8> text = Instruction::encode(program);
	CHECK_EQ(text.size(), program.size() * Instruction::Size);
	for (usize i = 0; i < program.size(); ++i)
		CHECK(Instruction::fromBytes(text.data() + i * Instruction::Size) == program[i]);
}

TEST(instruction_fields, the_stored_bytes_do_not_depend_on_the_hosts_byte_order)
{
	// A big-endian host keeps the value 0x11223344 as 11 22 33 44. The same image must decode the same there,
	// so the order in the image is fixed: what a big-endian host would get by storing the value natively is the
	// byte-swapped word, and decoding that as the image would give a different instruction.
	for (const Instruction instruction : samples())
	{
		std::array<u8, 4> bigEndianNative{};
		storeLittleEndian32(bigEndianNative.data(), std::byteswap(instruction.raw()));
		const Instruction misread = Instruction::fromBytes(bigEndianNative.data());
		CHECK_EQ(misread.raw(), std::byteswap(instruction.raw()));
		CHECK(instruction.raw() == std::byteswap(std::byteswap(instruction.raw())));
		// And on any host, the image of the instruction is its value low byte first.
		const auto image = instruction.bytes();
		CHECK_EQ(loadLittleEndian32(image.data()), instruction.raw());
		CHECK_EQ(nativeToLittleEndian(nativeToLittleEndian(instruction.raw())), instruction.raw());
	}
}
