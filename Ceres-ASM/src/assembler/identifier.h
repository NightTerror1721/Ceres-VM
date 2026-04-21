#pragma once

#include "common/types.h"
#include <string>
#include <compare>

namespace ceres::casm
{
	class Identifier
	{
	private:
		std::string_view _name{};

	public:
		constexpr Identifier() noexcept = default;
		constexpr Identifier(const Identifier&) noexcept = default;
		constexpr Identifier(Identifier&&) noexcept = default;
		constexpr ~Identifier() noexcept = default;

		constexpr Identifier& operator=(const Identifier&) noexcept = default;
		constexpr Identifier& operator=(Identifier&&) noexcept = default;

		constexpr bool operator==(const Identifier&) const noexcept = default;
		constexpr auto operator<=>(const Identifier&) const noexcept = default;

	public:
		constexpr explicit Identifier(std::string_view name) noexcept : _name(name) {}

		constexpr std::string_view name() const noexcept { return _name; }

		constexpr bool empty() const noexcept { return _name.empty(); }
		constexpr usize size() const noexcept { return _name.size(); }

		inline usize hash() const noexcept { return std::hash<std::string_view>::_Do_hash(_name); }

		forceinline constexpr bool isValid() const noexcept { return isValidIdentifierName(_name); }

	public:
		static Identifier makeInvalid() noexcept { return Identifier{}; }
		static Identifier make(std::string_view name) noexcept { return Identifier(name); }

		static forceinline constexpr bool isAsciiAlpha(char ch) noexcept { return (ch >= 'A' && ch <= 'Z') || (ch >= 'a' && ch <= 'z'); }
		static forceinline constexpr bool isAsciiDigit(char ch) noexcept { return ch >= '0' && ch <= '9'; }
		static forceinline constexpr bool isAsciiAlnum(char ch) noexcept { return isAsciiAlpha(ch) || isAsciiDigit(ch); }

		static constexpr bool isValidIdentifierName(std::string_view name) noexcept
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
	};
}

template <>
struct std::hash<ceres::casm::Identifier>
{
	static std::size_t operator()(const ceres::casm::Identifier& identifier) noexcept
	{
		return identifier.hash();
	}
};
