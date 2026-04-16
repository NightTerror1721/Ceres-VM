#pragma once

#include "common/types.h"
#include <limits>
#include <compare>

namespace ceres::casm
{
	class Size
	{
	public:
		using ValueType = u32;

		static inline constexpr ValueType InvalidValue = static_cast<ValueType>(-1);
		static inline constexpr ValueType MaxValue = std::numeric_limits<ValueType>::max() - 1;
		static inline constexpr ValueType MinValue = 0;
		static inline constexpr usize SizeInBytes = sizeof(ValueType);

	private:
		ValueType _value = MinValue;

	public:
		constexpr Size() noexcept = default;
		constexpr Size(const Size&) noexcept = default;
		constexpr Size(Size&&) noexcept = default;
		constexpr ~Size() noexcept = default;

		constexpr Size& operator=(const Size&) noexcept = default;
		constexpr Size& operator=(Size&&) noexcept = default;

		constexpr bool operator==(const Size&) const noexcept = default;
		constexpr auto operator<=>(const Size&) const noexcept = default;

	public:
		constexpr explicit Size(ValueType value) noexcept : _value(value) {}

		template <Integral T> requires (sizeof(T) <= SizeInBytes)
		constexpr explicit Size(T value) noexcept : _value(static_cast<ValueType>(value)) {}

		constexpr ValueType value() const noexcept { return _value; }

		constexpr bool isValid() const noexcept { return _value != InvalidValue; }
		constexpr bool isInvalid() const noexcept { return _value == InvalidValue; }

	public:
		constexpr explicit operator bool() const noexcept { return _value > MinValue && _value != InvalidValue; }
		constexpr bool operator!() const noexcept { return _value <= MinValue || _value == InvalidValue; }

		constexpr explicit operator ValueType() const noexcept { return _value; }

		template <Integral T> requires (sizeof(T) <= SizeInBytes)
		constexpr explicit operator T() const noexcept { return static_cast<T>(_value); }

		constexpr Size operator+(const Size& other) const noexcept
		{
			if (isInvalid() || other.isInvalid())
				return Size(InvalidValue);
			if (_value > MaxValue - other._value)
				return Size(InvalidValue); // Overflow
			return Size(_value + other._value);
		}
		constexpr Size operator-(const Size& other) const noexcept
		{
			if (isInvalid() || other.isInvalid())
				return Size(InvalidValue);
			if (_value < other._value)
				return Size(InvalidValue); // Underflow
			return Size(_value - other._value);
		}
		constexpr Size operator*(const Size& other) const noexcept
		{
			if (isInvalid() || other.isInvalid())
				return Size(InvalidValue);
			if (other._value == 0)
				return Size(MinValue); // Multiplication by zero
			if (_value > MaxValue / other._value)
				return Size(InvalidValue); // Overflow
			return Size(_value * other._value);
		}
		constexpr Size operator/(const Size& other) const noexcept
		{
			if (isInvalid() || other.isInvalid() || other._value == 0)
				return Size(InvalidValue);
			return Size(_value / other._value);
		}
		constexpr Size operator%(const Size& other) const noexcept
		{
			if (isInvalid() || other.isInvalid() || other._value == 0)
				return Size(InvalidValue);
			return Size(_value % other._value);
		}

		constexpr Size& operator+=(const Size& other) noexcept
		{
			*this = *this + other;
			return *this;
		}
		constexpr Size& operator-=(const Size& other) noexcept
		{
			*this = *this - other;
			return *this;
		}
		constexpr Size& operator*=(const Size& other) noexcept
		{
			*this = *this * other;
			return *this;
		}
		constexpr Size& operator/=(const Size& other) noexcept
		{
			*this = *this / other;
			return *this;
		}
		constexpr Size& operator%=(const Size& other) noexcept
		{
			*this = *this % other;
			return *this;
		}

	public:
		static const Size Invalid;
		static const Size Max;
		static const Size Min;
		static const Size Zero;
	};

	inline constexpr const Size Size::Invalid{ InvalidValue };
	inline constexpr const Size Size::Max{ MaxValue };
	inline constexpr const Size Size::Min{ MinValue };
	inline constexpr const Size Size::Zero{ MinValue };

	inline consteval Size operator"" _sz(unsigned long long value) noexcept
	{
		if (value > Size::MaxValue)
			return Size::Invalid;
		return Size(static_cast<Size::ValueType>(value));
	}
}
