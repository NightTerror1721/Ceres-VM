#pragma once

// What an object file has that a whole-program build never needed: a note saying "this field holds
// an address, and here is which one", left where the address itself would have gone.
//
// The assembler resolves a symbol to a number and the emitter writes that number into a field. When
// every unit is assembled together that number is final, so nothing has to be remembered. Assembled
// on its own, a unit knows the shape of every access and none of the addresses: its own sections
// have not been placed yet, and a symbol from another unit has not even been seen. A relocation is
// the difference - the field, how the value goes into it, and what the value is *of*.

#include "common_defs.h"
#include <string>

namespace ceres::casm
{
	// The field a relocation writes into, named after the encoding it belongs to rather than after
	// what it is used for: the linker has to reproduce exactly what the emitter would have written,
	// range check included, and the field is what decides that.
	enum class RelocationField : u8
	{
		Imm8,
		Imm16,
		// The low half of a two-instruction address, and the one field that is deliberately not
		// range-checked: the upper half went into the paired LUI.
		Imm16Low,
		SImm16,
		SImm20,
		Imm24,
		SImm24,
	};

	struct Relocation
	{
		// Where the word to patch sits in the object's own .text, in bytes.
		u32 offset = 0;

		RelocationField field = RelocationField::Imm16;
		// Applied to the resolved address before it goes into the field: 16 for the LUI half of an
		// address built in two instructions, 0 everywhere else.
		u8 shift = 0;
		// The field holds the distance from the patched word to the target, not the target.
		bool pcRelative = false;

		// Which section the target lives in. Meaningless for an external, whose section is
		// whatever the object that defines it says.
		SectionType section = SectionType::Text;
		// Defined in another unit, so the link has to look the name up. A local target needs no
		// lookup - `section` and `addend` already say where it is - but keeps its name anyway,
		// because an error about a distance is nearly useless without one.
		bool external = false;
		std::string symbol;
		// The target's offset within its section, for a local target; an extra displacement added
		// to the symbol's address, for an external one.
		i32 addend = 0;

		bool isExternal() const noexcept { return external; }
	};
}
