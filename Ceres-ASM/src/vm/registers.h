#pragma once

#include "common/types.h"
#include "common/int24.h"
#include <compare>
#include <array>

namespace ceres::vm
{
	class Address;

	class Register
	{
	public:
		using ValueType = u32;
		using SignedValueType = i32;

		static inline constexpr ValueType MaxValue = std::numeric_limits<ValueType>::max();
		static inline constexpr ValueType MinValue = std::numeric_limits<ValueType>::min();
		static inline constexpr usize Size = sizeof(ValueType);

	protected:
		ValueType _value = 0;

	public:
		constexpr Register() noexcept = default;
		constexpr Register(const Register&) noexcept = default;
		constexpr Register(Register&&) noexcept = default;
		constexpr ~Register() noexcept = default;

		constexpr Register& operator=(const Register&) noexcept = default;
		constexpr Register& operator=(Register&&) noexcept = default;

		constexpr bool operator==(const Register&) const noexcept = default;
		constexpr auto operator<=>(const Register&) const noexcept = default;

	public:
		forceinline constexpr Register(ValueType value) noexcept : _value(value) {}

		template <Integral T> requires (sizeof(T) <= Size)
		explicit (SignedIntegral<T> && sizeof(T) == Size) 
		forceinline constexpr Register(T value) noexcept : _value(static_cast<ValueType>(value)) {}

		forceinline constexpr Register(u24 value) noexcept : _value(static_cast<ValueType>(value)) {}
		forceinline constexpr Register(i24 value) noexcept : _value(static_cast<ValueType>(value)) {}

		forceinline constexpr ValueType value() const noexcept { return _value; }

		template <Integral T> requires (sizeof(T) <= Size)
		forceinline constexpr T value() const noexcept { return static_cast<T>(_value); }

		forceinline constexpr void set(ValueType value) noexcept { _value = value; }

		template <Integral T> requires (sizeof(T) <= Size)
		forceinline constexpr void set(T value) noexcept { _value = static_cast<ValueType>(value); }

		forceinline constexpr void set(u24 value) noexcept { _value = static_cast<ValueType>(value); }
		forceinline constexpr void set(i24 value) noexcept { _value = static_cast<ValueType>(value); }

	public:
		constexpr explicit operator bool() const noexcept { return _value != 0; }
		constexpr bool operator!() const noexcept { return _value == 0; }

		constexpr operator ValueType() const noexcept { return _value; }

		template <Integral T> requires (sizeof(T) <= Size)
		explicit (SignedIntegral<T> && sizeof(T) == Size)
		constexpr operator T() const noexcept { return static_cast<T>(_value); }

		constexpr Register operator+(Register other) const noexcept { return Register(_value + other._value); }
		constexpr Register operator-(Register other) const noexcept { return Register(_value - other._value); }
		constexpr Register operator*(Register other) const noexcept { return Register(_value * other._value); }
		constexpr Register operator/(Register other) const noexcept { return Register(_value / other._value); }
		constexpr Register operator%(Register other) const noexcept { return Register(_value % other._value); }

		constexpr Register operator&(Register other) const noexcept { return Register(_value & other._value); }
		constexpr Register operator|(Register other) const noexcept { return Register(_value | other._value); }
		constexpr Register operator^(Register other) const noexcept { return Register(_value ^ other._value); }
		constexpr Register operator~() const noexcept { return Register(~_value); }
		constexpr Register operator<<(Register other) const noexcept { return Register(_value << other._value); }
		constexpr Register operator>>(Register other) const noexcept { return Register(_value >> other._value); }

		constexpr Register& operator+=(Register other) noexcept { _value += other._value; return *this; }
		constexpr Register& operator-=(Register other) noexcept { _value -= other._value; return *this; }
		constexpr Register& operator*=(Register other) noexcept { _value *= other._value; return *this; }
		constexpr Register& operator/=(Register other) noexcept { _value /= other._value; return *this; }
		constexpr Register& operator%=(Register other) noexcept { _value %= other._value; return *this; }

		constexpr Register& operator&=(Register other) noexcept { _value &= other._value; return *this; }
		constexpr Register& operator|=(Register other) noexcept { _value |= other._value; return *this; }
		constexpr Register& operator^=(Register other) noexcept { _value ^= other._value; return *this; }
		constexpr Register& operator<<=(Register other) noexcept { _value <<= other._value; return *this; }
		constexpr Register& operator>>=(Register other) noexcept { _value >>= other._value; return *this; }

		constexpr Register operator+() const noexcept { return *this; }
        constexpr Register operator-() const noexcept { return Register(static_cast<ValueType>(~_value + 1)); }

		constexpr Register operator++() noexcept { ++_value; return *this; }
		constexpr Register operator++(int) noexcept { Register temp(*this); ++_value; return temp; }

		constexpr Register operator--() noexcept { --_value; return *this; }
		constexpr Register operator--(int) noexcept { Register temp(*this); --_value; return temp; }

	public:
		constexpr explicit Register(Address address) noexcept;

		constexpr Address toAddress() const noexcept;
	};

	enum class RIndex : u8
	{
		R0 = 0,
		R1 = 1,
		R2 = 2,
		R3 = 3,
		R4 = 4,
		R5 = 5,
		R6 = 6,
		R7 = 7,
		R8 = 8,
		R9 = 9,
		R10 = 10,
		R11 = 11,
		R12 = 12,
		R13 = 13, // Link Register
		R14 = 14, // Frame Pointer
		R15 = 15, // Stack Pointer

		LR = R13, // Link Register
		FP = R14, // Frame Pointer
		SP = R15  // Stack Pointer
	};
	forceinline constexpr u8 toRawIndex(RIndex index) noexcept { return static_cast<u8>(index); }
	forceinline constexpr RIndex toRIndex(u8 regIndex) noexcept { return static_cast<RIndex>(regIndex); }

	class GeneralPurposeRegisterPool
	{
	public:
		static inline constexpr usize Count = 16;
		static inline constexpr usize Size = Count * Register::Size;

		static inline constexpr usize StackPointerIndex = Count - 1; // R15 is the stack pointer (SP)
		static inline constexpr usize FramePointerIndex = Count - 2; // R14 is the frame pointer (FP)
		static inline constexpr usize LinkRegisterIndex = Count - 3; // R13 is the link register (LR)
		static inline constexpr usize GeneralPurposeStartIndex = 0; // R0-R12 are general-purpose registers
		static inline constexpr usize GeneralPurposeEndIndex = Count - 4; // R0-R12 are general-purpose registers

	private:
		std::array<Register, Count> _registers{};

	public:
		constexpr GeneralPurposeRegisterPool() noexcept = default;
		constexpr GeneralPurposeRegisterPool(const GeneralPurposeRegisterPool&) noexcept = default;
		constexpr GeneralPurposeRegisterPool(GeneralPurposeRegisterPool&&) noexcept = default;
		constexpr ~GeneralPurposeRegisterPool() noexcept = default;

		constexpr GeneralPurposeRegisterPool& operator=(const GeneralPurposeRegisterPool&) noexcept = default;
		constexpr GeneralPurposeRegisterPool& operator=(GeneralPurposeRegisterPool&&) noexcept = default;

	public:
		forceinline constexpr Register get(usize index) const noexcept { return _registers[index]; }

		template <usize Index> requires (Index < Count)
		forceinline constexpr Register get() const noexcept { return _registers[Index]; }

		template <RIndex Index> requires (static_cast<usize>(Index) < Count)
		forceinline constexpr Register get() const noexcept { return _registers[static_cast<usize>(Index)]; }

		forceinline constexpr void set(usize index, Register value) noexcept { _registers[index] = value; }

		template <usize Index> requires (Index < Count)
		forceinline constexpr void set(Register value) noexcept { _registers[Index] = value; }

		template <RIndex Index> requires (static_cast<usize>(Index) < Count)
		forceinline constexpr void set(Register value) noexcept { _registers[static_cast<usize>(Index)] = value; }

		forceinline constexpr Register::ValueType getValue(usize index) const noexcept { return _registers[index].value(); }

		template <usize Index> requires (Index < Count)
		forceinline constexpr Register::ValueType getValue() const noexcept { return _registers[Index].value(); }

		template <RIndex Index> requires (static_cast<usize>(Index) < Count)
		forceinline constexpr Register::ValueType getValue() const noexcept { return _registers[static_cast<usize>(Index)].value(); }

		template <Integral T> requires (sizeof(T) <= sizeof(Register::ValueType))
		forceinline constexpr T getValue(usize index) const noexcept { return _registers[index].template value<T>(); }

		template <Integral T, usize Index> requires (Index < Count && sizeof(T) <= sizeof(Register::ValueType))
		forceinline constexpr T getValue() const noexcept { return _registers[Index].template value<T>(); }

		template <Integral T, RIndex Index> requires (static_cast<usize>(Index) < Count && sizeof(T) <= sizeof(Register::ValueType))
		forceinline constexpr T getValue() const noexcept { return _registers[static_cast<usize>(Index)].template value<T>(); }

		forceinline constexpr void setValue(usize index, Register::ValueType value) noexcept { _registers[index].set(value); }

		template <usize Index> requires (Index < Count)
		forceinline constexpr void setValue(Register::ValueType value) noexcept { _registers[Index].set(value); }

		template <RIndex Index> requires (static_cast<usize>(Index) < Count)
		forceinline constexpr void setValue(Register::ValueType value) noexcept { _registers[static_cast<usize>(Index)].set(value); }

		template <Integral T> requires (sizeof(T) <= sizeof(Register::ValueType))
		forceinline constexpr void setValue(usize index, T value) noexcept { _registers[index].template set<T>(value); }

		template <Integral T, usize Index> requires (Index < Count && sizeof(T) <= sizeof(Register::ValueType))
		forceinline constexpr void setValue(T value) noexcept { _registers[Index].template set<T>(value); }

		template <Integral T, RIndex Index> requires (static_cast<usize>(Index) < Count && sizeof(T) <= sizeof(Register::ValueType))
		forceinline constexpr void setValue(T value) noexcept { _registers[static_cast<usize>(Index)].template set<T>(value); }

		forceinline constexpr Register& r0() noexcept { return _registers[0]; }
		forceinline constexpr Register& r1() noexcept { return _registers[1]; }
		forceinline constexpr Register& r2() noexcept { return _registers[2]; }
		forceinline constexpr Register& r3() noexcept { return _registers[3]; }
		forceinline constexpr Register& r4() noexcept { return _registers[4]; }
		forceinline constexpr Register& r5() noexcept { return _registers[5]; }
		forceinline constexpr Register& r6() noexcept { return _registers[6]; }
		forceinline constexpr Register& r7() noexcept { return _registers[7]; }
		forceinline constexpr Register& r8() noexcept { return _registers[8]; }
		forceinline constexpr Register& r9() noexcept { return _registers[9]; }
		forceinline constexpr Register& r10() noexcept { return _registers[10]; }
		forceinline constexpr Register& r11() noexcept { return _registers[11]; }
		forceinline constexpr Register& r12() noexcept { return _registers[12]; }
		forceinline constexpr Register& r13() noexcept { return _registers[13]; } // Link Register (LR)
		forceinline constexpr Register& r14() noexcept { return _registers[14]; } // Frame Pointer (FP)
		forceinline constexpr Register& r15() noexcept { return _registers[15]; } // Stack Pointer (SP)

		forceinline constexpr Register& sp() noexcept { return _registers[StackPointerIndex]; }
		forceinline constexpr Register& fp() noexcept { return _registers[FramePointerIndex]; }
		forceinline constexpr Register& lr() noexcept { return _registers[LinkRegisterIndex]; }

		forceinline constexpr Register r0() const noexcept { return _registers[0]; }
		forceinline constexpr Register r1() const noexcept { return _registers[1]; }
		forceinline constexpr Register r2() const noexcept { return _registers[2]; }
		forceinline constexpr Register r3() const noexcept { return _registers[3]; }
		forceinline constexpr Register r4() const noexcept { return _registers[4]; }
		forceinline constexpr Register r5() const noexcept { return _registers[5]; }
		forceinline constexpr Register r6() const noexcept { return _registers[6]; }
		forceinline constexpr Register r7() const noexcept { return _registers[7]; }
		forceinline constexpr Register r8() const noexcept { return _registers[8]; }
		forceinline constexpr Register r9() const noexcept { return _registers[9]; }
		forceinline constexpr Register r10() const noexcept { return _registers[10]; }
		forceinline constexpr Register r11() const noexcept { return _registers[11]; }
		forceinline constexpr Register r12() const noexcept { return _registers[12]; }
		forceinline constexpr Register r13() const noexcept { return _registers[13]; } // Link Register (LR)
		forceinline constexpr Register r14() const noexcept { return _registers[14]; } // Frame Pointer (FP)
		forceinline constexpr Register r15() const noexcept { return _registers[15]; } // Stack Pointer (SP)

		forceinline constexpr Register sp() const noexcept { return _registers[StackPointerIndex]; }
		forceinline constexpr Register fp() const noexcept { return _registers[FramePointerIndex]; }
		forceinline constexpr Register lr() const noexcept { return _registers[LinkRegisterIndex]; }

	public:
		forceinline constexpr Register& operator[](usize index) noexcept { return _registers[index]; }
		forceinline constexpr Register operator[](usize index) const noexcept { return _registers[index]; }

		forceinline constexpr Register& operator[](RIndex index) noexcept { return _registers[static_cast<usize>(index)]; }
		forceinline constexpr Register operator[](RIndex index) const noexcept { return _registers[static_cast<usize>(index)]; }
	};

	inline consteval Register operator""_reg(unsigned long long value) noexcept
	{
		return Register(static_cast<Register::ValueType>(value));
	}

	enum class ExecutionFlag : u8
	{
		None		= 0,
		Zero		= 1 << 0, // Zero Flag (ZF)
		Sign		= 1 << 1, // Sign Flag (SF)
		Carry		= 1 << 2, // Carry Flag (CF)
		Overflow	= 1 << 3, // Overflow Flag (OF)
		Interrupt	= 1 << 4, // Interrupt Flag (IF)
		Halting		= 1 << 5, // Halting Flag (HF)
		Trap		= 1 << 6, // Trap Flag (TF)
	};

	constexpr ExecutionFlag operator|(ExecutionFlag lhs, ExecutionFlag rhs) noexcept
	{
		return static_cast<ExecutionFlag>(static_cast<u8>(lhs) | static_cast<u8>(rhs));
	}
	constexpr ExecutionFlag operator&(ExecutionFlag lhs, ExecutionFlag rhs) noexcept
	{
		return static_cast<ExecutionFlag>(static_cast<u8>(lhs) & static_cast<u8>(rhs));
	}
	constexpr ExecutionFlag operator^(ExecutionFlag lhs, ExecutionFlag rhs) noexcept
	{
		return static_cast<ExecutionFlag>(static_cast<u8>(lhs) ^ static_cast<u8>(rhs));
	}
	constexpr ExecutionFlag operator~(ExecutionFlag flag) noexcept
	{
		return static_cast<ExecutionFlag>(~static_cast<u8>(flag));
	}

	class FlagRegister : public Register
	{
	public:
		using Base = Register;
		using ValueType = Register::ValueType;

	public:
		constexpr FlagRegister() noexcept = default;
		constexpr FlagRegister(const FlagRegister&) noexcept = default;
		constexpr FlagRegister(FlagRegister&&) noexcept = default;
		constexpr ~FlagRegister() noexcept = default;

		constexpr FlagRegister& operator=(const FlagRegister&) noexcept = default;
		constexpr FlagRegister& operator=(FlagRegister&&) noexcept = default;

	public:
		forceinline constexpr FlagRegister(ValueType flags) noexcept : Base(flags) {}

		forceinline constexpr void reset() noexcept { _value = 0; }

		forceinline constexpr bool get(ExecutionFlag flag) const noexcept { return (_value & static_cast<ValueType>(flag)) != 0; }
		forceinline constexpr void set(ExecutionFlag flag) noexcept { _value |= static_cast<ValueType>(flag); }
		forceinline constexpr void clear(ExecutionFlag flag) noexcept { _value &= ~static_cast<ValueType>(flag); }
		forceinline constexpr void toggle(ExecutionFlag flag) noexcept { _value ^= static_cast<ValueType>(flag); }

		forceinline constexpr void setFlag(ExecutionFlag flag, bool value) noexcept
		{
			if (value)
				set(flag);
			else
				clear(flag);
		}

		template <ExecutionFlag Flag> forceinline constexpr bool get() const noexcept { return (_value & static_cast<ValueType>(Flag)) != 0; }
		template <ExecutionFlag Flag> forceinline constexpr void set() noexcept { _value |= static_cast<ValueType>(Flag); }
		template <ExecutionFlag Flag> forceinline constexpr void clear() noexcept { _value &= ~static_cast<ValueType>(Flag); }
		template <ExecutionFlag Flag> forceinline constexpr void toggle() noexcept { _value ^= static_cast<ValueType>(Flag); }
		template <ExecutionFlag Flag> forceinline constexpr void setFlag(bool value) noexcept
		{
			if (value)
				set<Flag>();
			else
				clear<Flag>();
		}

		forceinline constexpr bool zero() const noexcept { return get<ExecutionFlag::Zero>(); }
		forceinline constexpr bool sign() const noexcept { return get<ExecutionFlag::Sign>(); }
		forceinline constexpr bool carry() const noexcept { return get<ExecutionFlag::Carry>(); }
		forceinline constexpr bool overflow() const noexcept { return get<ExecutionFlag::Overflow>(); }
		forceinline constexpr bool interrupt() const noexcept { return get<ExecutionFlag::Interrupt>(); }
		forceinline constexpr bool halting() const noexcept { return get<ExecutionFlag::Halting>(); }
		forceinline constexpr bool trap() const noexcept { return get<ExecutionFlag::Trap>(); }
	};
}
