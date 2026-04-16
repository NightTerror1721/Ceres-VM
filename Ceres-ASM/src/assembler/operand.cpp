#include "operand.h"
#include "vm/registers.h"
#include <charconv>

namespace ceres::casm
{
	std::expected<Operand, std::string_view> Operand::makeFromLiteralValue(const LiteralValue& value) noexcept
	{
		switch (value.type())
		{
			case LiteralValueType::Integer:
				return Operand::makeImmediate(value.integerValue());

			case LiteralValueType::Float:
				return std::unexpected("Floating-point literals are not directly supported as operands");

			case LiteralValueType::Char:
				return Operand::makeImmediate(static_cast<u32>(value.charValue()));

			case LiteralValueType::Bool:
				return Operand::makeImmediate(static_cast<u32>(value.boolValue() ? 1 : 0));

			case LiteralValueType::String:
				return std::unexpected("String literals are not directly supported as operands");

			case LiteralValueType::Array:
				return std::unexpected("Array literals are not directly supported as operands");

			case LiteralValueType::Identifier:
				return std::unexpected("Identifiers are not directly supported as operands");

			default:
				return std::unexpected("Unsupported literal value type");
		}
	}

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