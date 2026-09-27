#pragma once

#include <ceres/core/base/types.h>

#include <string_view>

namespace ceres::vm
{
	// Why the last memory fault happened, beyond which interrupt it raised: what SystemControl's FaultReason
	// register reports (plan/v2 SPEC 5.4), for a memory fault and for an illegal instruction. OutOfRam, OutOfVram and
	// Unmapped are the physical map's (SPEC 2): an access, a fetch or a block past the RAM, past the VRAM, or into an
	// empty region.
	enum class FaultReason : u32
	{
		None = 0,
		Alignment = 1,      // a misaligned access to RAM or VRAM
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

	// The reason's name as SPEC 5.4 spells it, for a diagnostic; empty for a value that names none.
	constexpr std::string_view faultReasonName(FaultReason reason) noexcept
	{
		switch (reason)
		{
			case FaultReason::None:           return "None";
			case FaultReason::Alignment:      return "Alignment";
			case FaultReason::OutOfRam:       return "OutOfRam";
			case FaultReason::OutOfVram:      return "OutOfVram";
			case FaultReason::Unmapped:       return "Unmapped";
			case FaultReason::MmioWidth:      return "MmioWidth";
			case FaultReason::MmioBlock:      return "MmioBlock";
			case FaultReason::MmioUndeclared: return "MmioUndeclared";
			case FaultReason::RegisterPair:   return "RegisterPair";
			case FaultReason::UnknownOpcode:  return "UnknownOpcode";
			case FaultReason::BadSubfield:    return "BadSubfield";
		}
		return {};
	}
}
