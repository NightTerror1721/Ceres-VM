#pragma once

#include <ceres/core/base/types.h>

namespace ceres::vm
{
	// Why the last memory fault happened, beyond which interrupt it raised: what SystemControl's FaultReason
	// register reports (plan/v2 SPEC 5.4). The machine raises the reasons it has a cause for; the others are
	// reserved for the phases that bring theirs (the memory map in F4, the 64-bit instructions in F3).
	enum class FaultReason : u32
	{
		None = 0,
		Alignment = 1,      // a misaligned access to RAM
		OutOfRam = 2,
		OutOfVram = 3,
		Unmapped = 4,
		MmioWidth = 5,      // a device register reached by an access that is not an aligned 32-bit one
		MmioBlock = 6,      // a block instruction (mcpy, mset, mcmp, mscan) that touches a device
		MmioUndeclared = 7,
		RegisterPair = 8,
		UnknownOpcode = 9,
		BadSubfield = 10,
	};
}
