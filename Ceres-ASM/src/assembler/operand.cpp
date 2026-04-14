#include "operand.h"
#include "vm/registers.h"
#include <charconv>

namespace ceres::casm
{
	std::optional<RegisterInfo> RegisterInfo::get(std::string_view name) noexcept
	{
		constexpr int maxRegisterIndex = vm::GeneralPurposeRegisterPool::Count - 1; // Maximum register index (15 for 16 registers)

		if (name.size() < 2)
			return std::nullopt;

		bool isFloatingPoint = false;
		if (name[0] == 'R' || name[0] == 'r')
			isFloatingPoint = false;
		else if (name[0] == 'F' || name[0] == 'f')
			isFloatingPoint = true;
		else
			return std::nullopt; // Invalid register prefix

        unsigned index = 0;
		const char* start = name.data() + 1;
		const char* end = name.data() + name.size();
		const auto result = std::from_chars(start, end, index); // Validate the register index
		// Ensure parsing succeeded, consumed the whole string, at least one digit was read,
		// and the index is within range.
		if (result.ec != std::errc() || result.ptr != end || result.ptr == start || index > static_cast<unsigned>(maxRegisterIndex))
			return std::nullopt; // Invalid register index

		return RegisterInfo{ static_cast<u8>(index), isFloatingPoint };
	}
}