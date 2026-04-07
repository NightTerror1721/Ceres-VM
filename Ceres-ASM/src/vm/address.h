#pragma once

#include "common/types.h"
#include "registers.h"
#include <compare>

namespace ceres::vm
{
	class Address
	{
	public:
		using ValueType = u32;

		static inline constexpr ValueType MaxValue = 0xFFFFFFFF;
		static inline constexpr ValueType NullValue = 0x00000000;
		static inline constexpr usize Size = sizeof(ValueType);

	private:
		ValueType _value = NullValue;

	public:
		constexpr Address() noexcept = default;
		constexpr Address(const Address&) noexcept = default;
		constexpr Address(Address&&) noexcept = default;
		constexpr ~Address() noexcept = default;

		constexpr Address& operator=(const Address&) noexcept = default;
		constexpr Address& operator=(Address&&) noexcept = default;

		constexpr bool operator==(const Address&) const noexcept = default;
		constexpr auto operator<=>(const Address&) const noexcept = default;

	public:
		forceinline constexpr Address(ValueType value) noexcept : _value(value) {}

		template <Integral T> requires (sizeof(T) <= Size)
		explicit (SignedIntegral<T>) forceinline constexpr Address(T value) noexcept : _value(static_cast<ValueType>(value)) {}

		forceinline explicit constexpr Address(Register reg) noexcept : _value(reg.value()) {}

		forceinline constexpr ValueType value() const noexcept { return _value; }

		forceinline constexpr Register toRegister() const noexcept { return Register(_value); }

		forceinline constexpr bool isNull() const noexcept { return _value == NullValue; }
		forceinline constexpr bool isValid() const noexcept { return _value != NullValue; }

		forceinline constexpr bool isInRange(Address start, Address end) const noexcept { return _value >= start._value && _value < end._value; }
		forceinline constexpr bool isInRange(ValueType start, ValueType end) const noexcept { return _value >= start && _value < end; }

		forceinline constexpr bool isInRangeInclusive(Address start, Address end) const noexcept { return _value >= start._value && _value <= end._value; }
		forceinline constexpr bool isInRangeInclusive(ValueType start, ValueType end) const noexcept { return _value >= start && _value <= end; }

	public:
		constexpr explicit operator bool() const noexcept { return _value != NullValue; }
		constexpr bool operator!() const noexcept { return _value == NullValue; }

		forceinline constexpr Address operator+(Address other) const noexcept { return Address(_value + other._value); }
		forceinline constexpr Address operator-(Address other) const noexcept { return Address(_value - other._value); }
		forceinline constexpr Address operator*(Address other) const noexcept { return Address(_value * other._value); }
		forceinline constexpr Address operator/(Address other) const noexcept { return Address(_value / other._value); }
		forceinline constexpr Address operator%(Address other) const noexcept { return Address(_value % other._value); }

		forceinline constexpr Address& operator+=(Address other) noexcept { _value += other._value; return *this; }
		forceinline constexpr Address& operator-=(Address other) noexcept { _value -= other._value; return *this; }
		forceinline constexpr Address& operator*=(Address other) noexcept { _value *= other._value; return *this; }
		forceinline constexpr Address& operator/=(Address other) noexcept { _value /= other._value; return *this; }
		forceinline constexpr Address& operator%=(Address other) noexcept { _value %= other._value; return *this; }

		forceinline constexpr Address operator++() noexcept { ++_value; return *this; }
		forceinline constexpr Address operator++(int) noexcept { Address temp = *this; ++_value; return temp; }

		forceinline constexpr Address operator--() noexcept { --_value; return *this; }
		forceinline constexpr Address operator--(int) noexcept { Address temp = *this; --_value; return temp; }

	public:
		static const Address Null;
		static const Address Max;
	};

	inline constexpr Address Address::Null{ Address::NullValue };
	inline constexpr Address Address::Max{ Address::MaxValue };

	inline consteval Address operator""_addr(unsigned long long value) noexcept
	{
		return Address(static_cast<Address::ValueType>(value));
	}


	forceinline constexpr Register::Register(Address address) noexcept : _value(address.value()) {}

	forceinline constexpr Address Register::toAddress() const noexcept { return Address(_value); }
}
