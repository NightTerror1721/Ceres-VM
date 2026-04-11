#pragma once

#include "common/types.h"
#include <compare>

namespace ceres
{
	class SignedInt24;
	class UnsignedInt24;

	class SignedInt24
	{
	public:
		using SignedValueType = i32;
		using UnsignedValueType = u32;
		using BitsType = u32;

		static inline constexpr SignedValueType MinValue = -0x00800000; // Minimum value for signed 24-bit integer (-8,388,608)
		static inline constexpr SignedValueType MaxValue = 0x007FFFFF;  // Maximum value for signed 24-bit integer (8,388,607)
        // Use an unsigned mask for bit operations to avoid signed shift/and issues
		static inline constexpr BitsType BitMask = 0x00FFFFFFu; // Mask for 24 bits (16,777,215)

	private:
		BitsType _value = 0;

	public:
		constexpr SignedInt24() noexcept = default;
		constexpr SignedInt24(const SignedInt24&) noexcept = default;
		constexpr SignedInt24(SignedInt24&&) noexcept = default;
		constexpr ~SignedInt24() noexcept = default;

		constexpr SignedInt24& operator=(const SignedInt24&) noexcept = default;
		constexpr SignedInt24& operator=(SignedInt24&&) noexcept = default;

		constexpr bool operator==(const SignedInt24&) const noexcept = default;
		constexpr auto operator<=>(const SignedInt24&) const noexcept = default;

 public:
		// Construct from a signed 32-bit value: store the low 24-bit pattern.
		forceinline constexpr explicit SignedInt24(SignedValueType value) noexcept
			: _value(static_cast<BitsType>(static_cast<u32>(value) & BitMask)) {}

		// Construct from an unsigned 32-bit value: store the low 24-bit pattern.
		forceinline constexpr explicit SignedInt24(UnsignedValueType value) noexcept
			: _value(static_cast<BitsType>(value & BitMask)) {}

		// Return the numeric signed value by interpreting the stored 24-bit pattern
		// as two's-complement and sign-extending to 32 bits.
		forceinline constexpr SignedValueType signedValue() const noexcept
		{
			const BitsType bits = _value & BitMask;
			constexpr BitsType SignBit = 1u << 23;
			if (bits & SignBit)
				return static_cast<SignedValueType>(static_cast<u32>(bits | ~BitMask));
			else
				return static_cast<SignedValueType>(bits);
		}

		// Return the numeric unsigned value by zero-extending the stored 24-bit pattern.
		forceinline constexpr UnsignedValueType unsignedValue() const noexcept { return static_cast<UnsignedValueType>(_value & BitMask); }

		// Return the raw 24-bit pattern.
		forceinline constexpr BitsType bits() const noexcept { return _value & BitMask; }

	public:
       constexpr explicit SignedInt24(UnsignedInt24 unsignedValue) noexcept;

		// Backwards-compatible explicit cast to unsigned 32-bit: zero-extend stored bits.
		forceinline constexpr explicit operator UnsignedValueType() const noexcept { return unsignedValue(); }

		// Backwards-compatible explicit cast to signed 32-bit: interpret stored bits as signed.
		forceinline constexpr explicit operator SignedValueType() const noexcept { return signedValue(); }
	};

	class UnsignedInt24
	{
	public:
		using SignedValueType = i32;
		using UnsignedValueType = u32;
		using BitsType = u32;

		static inline constexpr UnsignedValueType MinValue = 0; // Minimum value for unsigned 24-bit integer
		static inline constexpr UnsignedValueType MaxValue = 0x00FFFFFF; // Maximum value for unsigned 24-bit integer (16,777,215)
        static inline constexpr BitsType BitMask = 0x00FFFFFFu; // Mask for 24 bits (16,777,215)

	private:
		BitsType _value = 0;

	public:
		constexpr UnsignedInt24() noexcept = default;
		constexpr UnsignedInt24(const UnsignedInt24&) noexcept = default;
		constexpr UnsignedInt24(UnsignedInt24&&) noexcept = default;
		constexpr ~UnsignedInt24() noexcept = default;

		constexpr UnsignedInt24& operator=(const UnsignedInt24&) noexcept = default;
		constexpr UnsignedInt24& operator=(UnsignedInt24&&) noexcept = default;

		constexpr bool operator==(const UnsignedInt24&) const noexcept = default;
		constexpr auto operator<=>(const UnsignedInt24&) const noexcept = default;

	public:
        forceinline constexpr explicit UnsignedInt24(UnsignedValueType value) noexcept : _value(value & BitMask)
		{
			// Masking already ensures the value fits in 24 bits; no further clamping needed.
		}

		forceinline constexpr explicit UnsignedInt24(SignedValueType signedValue) noexcept
		{
			// Preserve the underlying 24-bit two's-complement bit pattern for symmetry
			// with SignedInt24 <-> UnsignedInt24 conversions. Use SignedInt24::bits() to
			// explicitly obtain the 24-bit raw pattern.
			u32 bits = static_cast<u32>(signedValue) & BitMask;
			_value = bits; // Store as unsigned, preserving the bit pattern
		}

		forceinline constexpr UnsignedValueType unsignedValue() const noexcept { return static_cast<UnsignedValueType>(_value & BitMask); }

		// Return the numeric value converted to signed 32-bit without interpreting
		// the 24-bit pattern as two's complement. This preserves the numeric value
		// as a non-negative integer in the signed domain (0..16,777,215).
		forceinline constexpr SignedValueType signedValue() const noexcept { return static_cast<SignedValueType>(_value); }

		// Return the raw 24-bit pattern (same as value()).
		forceinline constexpr BitsType bits() const noexcept { return _value & BitMask; }

	public:
		constexpr explicit UnsignedInt24(SignedInt24 signedValue) noexcept;

		// Backwards-compatible explicit cast to unsigned 32-bit: zero-extend stored bits.
		forceinline constexpr explicit operator UnsignedValueType() const noexcept { return unsignedValue(); }

		// Backwards-compatible explicit cast to signed 32-bit: interpret stored bits as signed.
		forceinline constexpr explicit operator SignedValueType() const noexcept { return signedValue(); }
	};

	forceinline constexpr SignedInt24::SignedInt24(UnsignedInt24 unsignedValue) noexcept
	{
		// Store the raw 24-bit pattern coming from UnsignedInt24.
		_value = unsignedValue.bits() & BitMask;
	}

	forceinline constexpr UnsignedInt24::UnsignedInt24(SignedInt24 signedValue) noexcept
	{
		// Store the raw 24-bit pattern coming from SignedInt24.
		_value = signedValue.bits() & BitMask;
	}

	using i24 = SignedInt24;
	using u24 = UnsignedInt24;
}
