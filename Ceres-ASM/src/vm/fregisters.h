#pragma once

#include "registers.h"
#include "address.h"

namespace ceres::vm
{
	class FloatingPointRegister
	{
	public:
		using ValueType = f32;

		static inline constexpr ValueType MaxValue = std::numeric_limits<ValueType>::max();
		static inline constexpr ValueType MinValue = std::numeric_limits<ValueType>::lowest();
		static inline constexpr ValueType NaNValue = std::numeric_limits<ValueType>::quiet_NaN();
		static inline constexpr ValueType InfinityValue = std::numeric_limits<ValueType>::infinity();
		static inline constexpr ValueType NegativeInfinityValue = -std::numeric_limits<ValueType>::infinity();
		static inline constexpr usize Size = sizeof(ValueType);

	private:
		ValueType _value = 0.0f;

	public:
		constexpr FloatingPointRegister() noexcept = default;
		constexpr FloatingPointRegister(const FloatingPointRegister&) noexcept = default;
		constexpr FloatingPointRegister(FloatingPointRegister&&) noexcept = default;
		constexpr ~FloatingPointRegister() noexcept = default;

		constexpr FloatingPointRegister& operator=(const FloatingPointRegister&) noexcept = default;
		constexpr FloatingPointRegister& operator=(FloatingPointRegister&&) noexcept = default;

		constexpr bool operator==(const FloatingPointRegister&) const noexcept = default;
		constexpr auto operator<=>(const FloatingPointRegister&) const noexcept = default;

	public:
		forceinline constexpr FloatingPointRegister(ValueType value) noexcept : _value(value) {}

		template <Integral T> requires (sizeof(T) <= Register::Size)
		forceinline constexpr FloatingPointRegister(T value) noexcept : _value(static_cast<ValueType>(value)) {}

		forceinline constexpr FloatingPointRegister(u24 value) noexcept : _value(static_cast<ValueType>(value.unsignedValue())) {}
		forceinline constexpr FloatingPointRegister(i24 value) noexcept : _value(static_cast<ValueType>(value.signedValue())) {}
		forceinline constexpr FloatingPointRegister(Register value) noexcept : _value(static_cast<ValueType>(value.value())) {}
		forceinline constexpr FloatingPointRegister(Address address) noexcept : _value(static_cast<ValueType>(address.value())) {}

		forceinline constexpr ValueType value() const noexcept { return _value; }

		forceinline constexpr Register bitsAsRegister() const noexcept { return Register(std::bit_cast<Register::ValueType>(_value)); }
		forceinline constexpr Register bitsAsSignedRegister() const noexcept { return Register(std::bit_cast<Register::SignedValueType>(_value)); }

		forceinline constexpr void set(ValueType value) noexcept { _value = value; }

		template <Integral T> requires (sizeof(T) <= Register::Size)
		forceinline constexpr void set(T value) noexcept { _value = static_cast<ValueType>(value); }

		forceinline constexpr void set(u24 value) noexcept { _value = static_cast<ValueType>(value.unsignedValue()); }
		forceinline constexpr void set(i24 value) noexcept { _value = static_cast<ValueType>(value.signedValue()); }
		forceinline constexpr void set(Register value) noexcept { _value = static_cast<ValueType>(value.value()); }
		forceinline constexpr void set(Address address) noexcept { _value = static_cast<ValueType>(address.value()); }

		forceinline constexpr void setBitsFromRegister(Register reg) noexcept { _value = std::bit_cast<ValueType>(reg.value()); }
		forceinline constexpr void setBitsFromSignedRegister(Register reg) noexcept { _value = std::bit_cast<ValueType>(reg.value<Register::SignedValueType>()); }

	public:
		constexpr explicit operator bool() const noexcept { return _value != 0.0f; }
		constexpr bool operator!() const noexcept { return _value == 0.0f; }

		constexpr operator ValueType() const noexcept { return _value; }

		template <Integral T> requires (sizeof(T) <= Register::Size)
		constexpr explicit operator T() const noexcept { return static_cast<T>(_value); }

		constexpr explicit operator u24() const noexcept { return u24(static_cast<u24::UnsignedValueType>(_value)); }
		constexpr explicit operator i24() const noexcept { return i24(static_cast<i24::SignedValueType>(_value)); }
		constexpr explicit operator Register() const noexcept { return Register(static_cast<Register::ValueType>(_value)); }
		constexpr explicit operator Address() const noexcept { return Address(static_cast<Address::ValueType>(_value)); }

		constexpr FloatingPointRegister operator+(FloatingPointRegister other) const noexcept { return FloatingPointRegister(_value + other._value); }
		constexpr FloatingPointRegister operator-(FloatingPointRegister other) const noexcept { return FloatingPointRegister(_value - other._value); }
		constexpr FloatingPointRegister operator*(FloatingPointRegister other) const noexcept { return FloatingPointRegister(_value * other._value); }
		constexpr FloatingPointRegister operator/(FloatingPointRegister other) const noexcept { return FloatingPointRegister(_value / other._value); }

		constexpr FloatingPointRegister& operator+=(FloatingPointRegister other) noexcept { _value += other._value; return *this; }
		constexpr FloatingPointRegister& operator-=(FloatingPointRegister other) noexcept { _value -= other._value; return *this; }
		constexpr FloatingPointRegister& operator*=(FloatingPointRegister other) noexcept { _value *= other._value; return *this; }
		constexpr FloatingPointRegister& operator/=(FloatingPointRegister other) noexcept { _value /= other._value; return *this; }

		constexpr FloatingPointRegister operator+() const noexcept { return *this; }
		constexpr FloatingPointRegister operator-() const noexcept { return FloatingPointRegister(-_value); }

		constexpr FloatingPointRegister operator++() noexcept { _value += 1.0f; return *this; }
		constexpr FloatingPointRegister operator++(int) noexcept { FloatingPointRegister temp(*this); _value += 1.0f; return temp; }

		constexpr FloatingPointRegister operator--() noexcept { _value -= 1.0f; return *this; }
		constexpr FloatingPointRegister operator--(int) noexcept { FloatingPointRegister temp(*this); _value -= 1.0f; return temp; }
	};

	enum class FIndex : u8
	{
		F0 = 0,
		F1 = 1,
		F2 = 2,
		F3 = 3,
		F4 = 4,
		F5 = 5,
		F6 = 6,
		F7 = 7,
		F8 = 8,
		F9 = 9,
		F10 = 10,
		F11 = 11,
		F12 = 12,
		F13 = 13,
		F14 = 14,
		F15 = 15
	};
	forceinline constexpr u8 toRawIndex(FIndex index) noexcept { return static_cast<u8>(index); }
	forceinline constexpr FIndex toFIndex(u8 regIndex) noexcept { return static_cast<FIndex>(regIndex); }

	class FloatingPointRegisterPool
	{
		static inline constexpr usize Count = GeneralPurposeRegisterPool::Count;
		static inline constexpr usize Size = Count * FloatingPointRegister::Size;

	private:
		std::array<FloatingPointRegister, Count> _registers{};

	public:
		constexpr FloatingPointRegisterPool() noexcept = default;
		constexpr FloatingPointRegisterPool(const FloatingPointRegisterPool&) noexcept = default;
		constexpr FloatingPointRegisterPool(FloatingPointRegisterPool&&) noexcept = default;
		constexpr ~FloatingPointRegisterPool() noexcept = default;

		constexpr FloatingPointRegisterPool& operator=(const FloatingPointRegisterPool&) noexcept = default;
		constexpr FloatingPointRegisterPool& operator=(FloatingPointRegisterPool&&) noexcept = default;

	public:
		forceinline constexpr FloatingPointRegister get(usize index) const noexcept { return _registers[index]; }

		template <usize Index> requires (Index < Count)
		forceinline constexpr FloatingPointRegister get() const noexcept { return _registers[Index]; }

		template <FIndex Index> requires (static_cast<usize>(Index) < Count)
		forceinline constexpr FloatingPointRegister get() const noexcept { return _registers[static_cast<usize>(Index)]; }

		forceinline constexpr void set(usize index, FloatingPointRegister value) noexcept { _registers[index] = value; }

		template <usize Index> requires (Index < Count)
		forceinline constexpr void set(FloatingPointRegister value) noexcept { _registers[Index] = value; }

		template <FIndex Index> requires (static_cast<usize>(Index) < Count)
		forceinline constexpr void set(FloatingPointRegister value) noexcept { _registers[static_cast<usize>(Index)] = value; }

		forceinline constexpr FloatingPointRegister::ValueType getValue(usize index) const noexcept { return _registers[index].value(); }

		template <usize Index> requires (Index < Count)
		forceinline constexpr FloatingPointRegister::ValueType getValue() const noexcept { return _registers[Index].value(); }

		template <FIndex Index> requires (static_cast<usize>(Index) < Count)
		forceinline constexpr FloatingPointRegister::ValueType getValue() const noexcept { return _registers[static_cast<usize>(Index)].value(); }

		forceinline constexpr void setValue(usize index, FloatingPointRegister::ValueType value) noexcept { _registers[index].set(value); }

		template <usize Index> requires (Index < Count)
		forceinline constexpr void setValue(FloatingPointRegister::ValueType value) noexcept { _registers[Index].set(value); }

		template <FIndex Index> requires (static_cast<usize>(Index) < Count)
		forceinline constexpr void setValue(FloatingPointRegister::ValueType value) noexcept { _registers[static_cast<usize>(Index)].set(value); }

		forceinline constexpr FloatingPointRegister& f0() noexcept { return _registers[0]; }
		forceinline constexpr FloatingPointRegister& f1() noexcept { return _registers[1]; }
		forceinline constexpr FloatingPointRegister& f2() noexcept { return _registers[2]; }
		forceinline constexpr FloatingPointRegister& f3() noexcept { return _registers[3]; }
		forceinline constexpr FloatingPointRegister& f4() noexcept { return _registers[4]; }
		forceinline constexpr FloatingPointRegister& f5() noexcept { return _registers[5]; }
		forceinline constexpr FloatingPointRegister& f6() noexcept { return _registers[6]; }
		forceinline constexpr FloatingPointRegister& f7() noexcept { return _registers[7]; }
		forceinline constexpr FloatingPointRegister& f8() noexcept { return _registers[8]; }
		forceinline constexpr FloatingPointRegister& f9() noexcept { return _registers[9]; }
		forceinline constexpr FloatingPointRegister& f10() noexcept { return _registers[10]; }
		forceinline constexpr FloatingPointRegister& f11() noexcept { return _registers[11]; }
		forceinline constexpr FloatingPointRegister& f12() noexcept { return _registers[12]; }
		forceinline constexpr FloatingPointRegister& f13() noexcept { return _registers[13]; }
		forceinline constexpr FloatingPointRegister& f14() noexcept { return _registers[14]; }
		forceinline constexpr FloatingPointRegister& f15() noexcept { return _registers[15]; }

		forceinline constexpr FloatingPointRegister f0() const noexcept { return _registers[0]; }
		forceinline constexpr FloatingPointRegister f1() const noexcept { return _registers[1]; }
		forceinline constexpr FloatingPointRegister f2() const noexcept { return _registers[2]; }
		forceinline constexpr FloatingPointRegister f3() const noexcept { return _registers[3]; }
		forceinline constexpr FloatingPointRegister f4() const noexcept { return _registers[4]; }
		forceinline constexpr FloatingPointRegister f5() const noexcept { return _registers[5]; }
		forceinline constexpr FloatingPointRegister f6() const noexcept { return _registers[6]; }
		forceinline constexpr FloatingPointRegister f7() const noexcept { return _registers[7]; }
		forceinline constexpr FloatingPointRegister f8() const noexcept { return _registers[8]; }
		forceinline constexpr FloatingPointRegister f9() const noexcept { return _registers[9]; }
		forceinline constexpr FloatingPointRegister f10() const noexcept { return _registers[10]; }
		forceinline constexpr FloatingPointRegister f11() const noexcept { return _registers[11]; }
		forceinline constexpr FloatingPointRegister f12() const noexcept { return _registers[12]; }
		forceinline constexpr FloatingPointRegister f13() const noexcept { return _registers[13]; }
		forceinline constexpr FloatingPointRegister f14() const noexcept { return _registers[14]; }
		forceinline constexpr FloatingPointRegister f15() const noexcept { return _registers[15]; }

	public:
		forceinline constexpr FloatingPointRegister& operator[](usize index) noexcept { return _registers[index]; }
		forceinline constexpr FloatingPointRegister operator[](usize index) const noexcept { return _registers[index]; }

		forceinline constexpr FloatingPointRegister& operator[](FIndex index) noexcept { return _registers[static_cast<usize>(index)]; }
		forceinline constexpr FloatingPointRegister operator[](FIndex index) const noexcept { return _registers[static_cast<usize>(index)]; }
	};

	inline consteval FloatingPointRegister operator""_freg(long double value) noexcept
	{
		return FloatingPointRegister(static_cast<FloatingPointRegister::ValueType>(value));
	}
}