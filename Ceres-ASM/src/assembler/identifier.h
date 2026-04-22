#pragma once

#include "common/types.h"
#include "common/string_utils.h"
#include <string>
#include <compare>

namespace ceres::casm
{
	inline constexpr bool isAsciiAlpha(char ch) noexcept { return (ch >= 'A' && ch <= 'Z') || (ch >= 'a' && ch <= 'z'); }
	inline constexpr bool isAsciiDigit(char ch) noexcept { return ch >= '0' && ch <= '9'; }
	inline constexpr bool isAsciiAlnum(char ch) noexcept { return isAsciiAlpha(ch) || isAsciiDigit(ch); }

	inline constexpr bool isValidIdentifierName(std::string_view name) noexcept
	{
		if (name.empty())
			return false;

		if (!isAsciiAlpha(name.front()) && name.front() != '_')
			return false;

		for (char c : name)
		{
			if (!isAsciiAlnum(c) && c != '_')
				return false;
		}

		return true;
	}
}
