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
		//
		// 4 KiB: a fault report that names functions and walks the program's frames (the STDLIB's
		// ceres/backtrace.h) needed more than the 1 KiB this used to be when built at -O0, where every
		// temporary has a slot of its own - and a handler that runs out of it has nowhere to report that.
		inline constexpr usize SystemStackSize = 4096;

		inline constexpr usize NullPageSegmentSize = 0x100; // 256 bytes, used for null page (address 0x00000000 - 0x000000FF)
		inline constexpr usize BiosSegmentSize = 0x300; // 768 bytes, used for BIOS (address 0x00000100 - 0x000003FF)
		// The rest of the memory (address 0x00000400 - 0xFFFFFFFF) is available for unrestricted use by programs.
		inline constexpr usize UnrestrictedSegmentStartValue = NullPageSegmentSize + BiosSegmentSize;

		// The physical map (plan/v2 SPEC 2): the RAM from 0 up to 2 GiB, the VRAM from 0xA0000000 up to 1 GiB, the
		// devices' 16 MiB at the top, and nothing in between.
		inline constexpr u32 RamLimitValue = 0x80000000;      // the RAM ends here at the most
		inline constexpr u32 VramStartValue = 0xA0000000;
		inline constexpr u32 VramLimitValue = 0xE0000000;     // the VRAM ends here at the most
		inline constexpr u32 MmioStartValue = 0xFF000000;

		inline constexpr Address NullPageSegmentStart = 0_addr;
		inline constexpr Address BiosSegmentStart = NullPageSegmentStart + Address(NullPageSegmentSize);
		inline constexpr Address UnrestrictedSegmentStart = NullPageSegmentStart + Address(UnrestrictedSegmentStartValue);
	}
}
