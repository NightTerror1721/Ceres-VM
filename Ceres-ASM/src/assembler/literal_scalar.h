#pragma once

#include "common_defs.h"
#include "data_type.h"
#include <variant>
#include <bit>
#include <optional>

namespace ceres::casm
{
	class LiteralScalar
	{
	public:
		union ValueType
		{
			u8 u8Value;
			u16 u16Value;
			u32 u32Value;
			i8 i8Value;
			i16 i16Value;
			i32 i32Value;
			f32 f32Value;

			u32 __raw; // For raw access to the underlying value (useful for comparisons and hashing)

			constexpr ValueType() noexcept : __raw(0) {}
			constexpr ValueType(u8 value) noexcept : u8Value(value) {}
			constexpr ValueType(u16 value) noexcept : u16Value(value) {}
			constexpr ValueType(u32 value) noexcept : u32Value(value) {}
			constexpr ValueType(i8 value) noexcept : i8Value(value) {}
			constexpr ValueType(i16 value) noexcept : i16Value(value) {}
			constexpr ValueType(i32 value) noexcept : i32Value(value) {}
			constexpr ValueType(f32 value) noexcept : f32Value(value) {}
			constexpr ValueType(const ValueType&) noexcept = default;
			constexpr ValueType(ValueType&&) noexcept = default;
			constexpr ~ValueType() noexcept = default;

			constexpr ValueType& operator=(const ValueType&) noexcept = default;
			constexpr ValueType& operator=(ValueType&&) noexcept = default;

			constexpr bool operator==(const ValueType& other) const noexcept { return __raw == other.__raw; }
		};

	private:
		DataTypeScalarCode _scalarCode = DataTypeScalarCode::U8;
		ValueType _value = {};

	public:
		constexpr LiteralScalar() noexcept = default;
		constexpr LiteralScalar(const LiteralScalar&) noexcept = default;
		constexpr LiteralScalar(LiteralScalar&&) noexcept = default;
		constexpr ~LiteralScalar() noexcept = default;

		constexpr LiteralScalar& operator=(const LiteralScalar&) noexcept = default;
		constexpr LiteralScalar& operator=(LiteralScalar&&) noexcept = default;

	private:
		constexpr explicit LiteralScalar(DataTypeScalarCode scalarCode, ValueType value) noexcept :
			_scalarCode(scalarCode), _value(value)
		{}

	public:
		constexpr DataTypeScalarCode scalarCode() const noexcept { return _scalarCode; }
		constexpr ValueType value() const noexcept { return _value; }

		constexpr DataType dataType() const noexcept { return DataType::makeScalar(_scalarCode); }

		constexpr bool isU8() const noexcept { return _scalarCode == DataTypeScalarCode::U8; }
		constexpr bool isU16() const noexcept { return _scalarCode == DataTypeScalarCode::U16; }
		constexpr bool isU32() const noexcept { return _scalarCode == DataTypeScalarCode::U32; }
		constexpr bool isI8() const noexcept { return _scalarCode == DataTypeScalarCode::I8; }
		constexpr bool isI16() const noexcept { return _scalarCode == DataTypeScalarCode::I16; }
		constexpr bool isI32() const noexcept { return _scalarCode == DataTypeScalarCode::I32; }
		constexpr bool isF32() const noexcept { return _scalarCode == DataTypeScalarCode::F32; }

		constexpr bool isInteger() const noexcept
		{
			return _scalarCode == DataTypeScalarCode::U8 ||
				_scalarCode == DataTypeScalarCode::U16 ||
				_scalarCode == DataTypeScalarCode::U32 ||
				_scalarCode == DataTypeScalarCode::I8 ||
				_scalarCode == DataTypeScalarCode::I16 ||
				_scalarCode == DataTypeScalarCode::I32;
		}

		constexpr bool isFloat() const noexcept { return _scalarCode == DataTypeScalarCode::F32; }

		// Raw 32-bit pattern of the value, read through the *active* union member. Reading __raw
		// directly is unreliable: the union constructors only initialise the member they name, so
		// for anything narrower than 32 bits the remaining bytes are indeterminate.
		constexpr u32 rawBits() const noexcept
		{
			switch (_scalarCode)
			{
				case DataTypeScalarCode::U8:  return static_cast<u32>(_value.u8Value);
				case DataTypeScalarCode::U16: return static_cast<u32>(_value.u16Value);
				case DataTypeScalarCode::U32: return _value.u32Value;
				case DataTypeScalarCode::I8:  return static_cast<u32>(static_cast<i32>(_value.i8Value));
				case DataTypeScalarCode::I16: return static_cast<u32>(static_cast<i32>(_value.i16Value));
				case DataTypeScalarCode::I32: return static_cast<u32>(_value.i32Value);
				case DataTypeScalarCode::F32: return std::bit_cast<u32>(_value.f32Value);
				default: return 0;
			}
		}

		constexpr u32 asRawValue() const noexcept { return rawBits(); }

		// Width in bits of a scalar code, or 0 if it has none.
		static constexpr u32 bitWidthOf(DataTypeScalarCode scalarCode) noexcept
		{
			switch (scalarCode)
			{
				case DataTypeScalarCode::U8:
				case DataTypeScalarCode::I8:  return 8;
				case DataTypeScalarCode::U16:
				case DataTypeScalarCode::I16: return 16;
				case DataTypeScalarCode::U32:
				case DataTypeScalarCode::I32:
				case DataTypeScalarCode::F32: return 32;
				default: return 0;
			}
		}

		// True when truncating `raw` to `bits` loses no information under either a signed or an
		// unsigned reading. This is what lets `-10` be written where an i16 is expected, and what
		// rejects `70000` where a u16 is expected.
		static constexpr bool fitsInBits(u32 raw, u32 bits) noexcept
		{
			if (bits == 0 || bits >= 32)
				return bits >= 32;

			const u32 mask = (1u << bits) - 1u;
			const u32 truncated = raw & mask;
			const u32 signExtended = (truncated & (1u << (bits - 1))) != 0 ? (truncated | ~mask) : truncated;

			return raw == truncated || raw == signExtended;
		}

		// Re-tag this scalar as `target`. Integer literals are untyped until context gives them a
		// type, so any integer converts to any integer as long as no significant bit is lost.
		// Returns nullopt when the value does not fit, or when the conversion is not integer-to-integer.
		constexpr std::optional<LiteralScalar> coerceTo(DataTypeScalarCode target) const noexcept
		{
			if (_scalarCode == target)
				return *this;

			if (!isInteger() || !DataType::isIntegerScalarCode(target))
				return std::nullopt; // No implicit conversion to or from f32.

			const u32 raw = rawBits();
			const u32 width = bitWidthOf(target);
			if (!fitsInBits(raw, width))
				return std::nullopt;

			switch (target)
			{
				case DataTypeScalarCode::U8:  return makeU8(static_cast<u8>(raw));
				case DataTypeScalarCode::U16: return makeU16(static_cast<u16>(raw));
				case DataTypeScalarCode::U32: return makeU32(raw);
				case DataTypeScalarCode::I8:  return makeI8(static_cast<i8>(raw));
				case DataTypeScalarCode::I16: return makeI16(static_cast<i16>(raw));
				case DataTypeScalarCode::I32: return makeI32(static_cast<i32>(raw));
				default: return std::nullopt;
			}
		}

	public:
		static constexpr LiteralScalar makeU8(u8 value) noexcept { return LiteralScalar{ DataTypeScalarCode::U8, ValueType{value} }; }
		static constexpr LiteralScalar makeU16(u16 value) noexcept { return LiteralScalar{ DataTypeScalarCode::U16, ValueType{value} }; }
		static constexpr LiteralScalar makeU32(u32 value) noexcept { return LiteralScalar{ DataTypeScalarCode::U32, ValueType{value} }; }
		static constexpr LiteralScalar makeI8(i8 value) noexcept { return LiteralScalar{ DataTypeScalarCode::I8, ValueType{value} }; }
		static constexpr LiteralScalar makeI16(i16 value) noexcept { return LiteralScalar{ DataTypeScalarCode::I16, ValueType{value} }; }
		static constexpr LiteralScalar makeI32(i32 value) noexcept { return LiteralScalar{ DataTypeScalarCode::I32, ValueType{value} }; }
		static constexpr LiteralScalar makeF32(f32 value) noexcept { return LiteralScalar{ DataTypeScalarCode::F32, ValueType{value} }; }

		static constexpr LiteralScalar makeFromChar(char value) noexcept { return makeU8(static_cast<u8>(value)); }
		static constexpr LiteralScalar makeFromBool(bool value) noexcept { return makeU8(static_cast<u8>(value ? 1 : 0)); }

		template <typename T> requires (SameAs<T, u8> || SameAs<T, u16> || SameAs<T, u32> || SameAs<T, i8> || SameAs<T, i16> || SameAs<T, i32> || SameAs<T, f32> || SameAs<T, char> || SameAs<T, bool>)
		static constexpr LiteralScalar make(T value) noexcept
		{
			if constexpr (SameAs<T, u8>)
				return makeU8(value);
			else if constexpr (SameAs<T, u16>)
				return makeU16(value);
			else if constexpr (SameAs<T, u32>)
				return makeU32(value);
			else if constexpr (SameAs<T, i8>)
				return makeI8(value);
			else if constexpr (SameAs<T, i16>)
				return makeI16(value);
			else if constexpr (SameAs<T, i32>)
				return makeI32(value);
			else if constexpr (SameAs<T, f32>)
				return makeF32(value);
			else if constexpr (SameAs<T, char>)
				return makeFromChar(value);
			else if constexpr (SameAs<T, bool>)
				return makeFromBool(value);
			else
				static_assert(false, "Unsupported type for LiteralScalar");
		}
	};
}
