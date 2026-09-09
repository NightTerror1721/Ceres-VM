#pragma once

#include <ceres/core/isa/address.h>

namespace ceres::fmt
{
	namespace MemoryMap
	{
		using namespace isa;

		// The top of memory belongs to interrupt handlers, not to the program. A handler used to
		// run on whatever stack it interrupted, which meant a program that had nearly exhausted
		// its own stack could not take an interrupt at all: the push of the saved PC was the thing
		// that overflowed, and the overflow was itself an interrupt.
		inline constexpr usize SystemStackSize = 1024;

		inline constexpr usize NullPageSegmentSize = 0x100; // 256 bytes, used for null page (address 0x00000000 - 0x000000FF)
		inline constexpr usize BiosSegmentSize = 0x300; // 768 bytes, used for BIOS (address 0x00000100 - 0x000003FF)
		// The rest of the memory (address 0x00000400 - 0xFFFFFFFF) is available for unrestricted use by programs.
		inline constexpr usize UnrestrictedSegmentStartValue = NullPageSegmentSize + BiosSegmentSize;

		inline constexpr Address NullPageSegmentStart = 0_addr;
		inline constexpr Address BiosSegmentStart = NullPageSegmentStart + Address(NullPageSegmentSize);
		inline constexpr Address UnrestrictedSegmentStart = NullPageSegmentStart + Address(UnrestrictedSegmentStartValue);
	}
}
