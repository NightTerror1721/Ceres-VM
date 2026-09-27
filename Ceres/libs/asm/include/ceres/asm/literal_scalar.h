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
			u64 u64Value;
			i64 i64Value;
			f64 f64Value;

			u64 __raw; // For raw access to the underlying value (useful for comparisons and hashing)

			constexpr ValueType() noexcept : __raw(0) {}
			constexpr ValueType(u8 value) noexcept : u8Value(value) {}
			constexpr ValueType(u16 value) noexcept : u16Value(value) {}
			constexpr ValueType(u32 value) noexcept : u32Value(value) {}
			constexpr ValueType(i8 value) noexcept : i8Value(value) {}
			constexpr ValueType(i16 value) noexcept : i16Value(value) {}
			constexpr ValueType(i32 value) noexcept : i32Value(value) {}
			constexpr ValueType(f32 value) noexcept : f32Value(value) {}
			constexpr ValueType(u64 value) noexcept : u64Value(value) {}
			constexpr ValueType(i64 value) noexcept : i64Value(value) {}
			constexpr ValueType(f64 value) noexcept : f64Value(value) {}
			constexpr ValueType(const ValueType&) noexcept = default;
			constexpr ValueType(ValueType&&) noexcept = default;
			constexpr ~ValueType() noexcept = default;

			constexpr ValueType& operator=(const ValueType&) noexcept = default;
			constexpr ValueType& operator=(ValueType&&) noexcept = default;

			// Deliberately deleted: only LiteralScalar knows which member is active, and __raw holds
			// indeterminate bytes for anything narrower than 64 bits. Use LiteralScalar::operator==.
			constexpr bool operator==(const ValueType& other) const = delete;
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

		// Compares the tag and the value read through the active union member. A defaulted
		// comparison would have gone through the union and read indeterminate bytes.
		constexpr bool operator==(const LiteralScalar& other) const noexcept
		{
			return _scalarCode == other._scalarCode && rawBits64() == other.rawBits64();
		}

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
		constexpr bool isU64() const noexcept { return _scalarCode == DataTypeScalarCode::U64; }
		constexpr bool isI64() const noexcept { return _scalarCode == DataTypeScalarCode::I64; }
		constexpr bool isF64() const noexcept { return _scalarCode == DataTypeScalarCode::F64; }

		constexpr bool isInteger() const noexcept { return DataType::isIntegerScalarCode(_scalarCode); }
		constexpr bool isFloat() const noexcept { return _scalarCode == DataTypeScalarCode::F32 || _scalarCode == DataTypeScalarCode::F64; }
		// One of the 64-bit kinds (plan/v2 SPEC 6): u64, i64, f64.
		constexpr bool isWide() const noexcept { return DataType::isWideScalarCode(_scalarCode); }
		constexpr bool isSigned() const noexcept
		{
			return _scalarCode == DataTypeScalarCode::I8 || _scalarCode == DataTypeScalarCode::I16 ||
				_scalarCode == DataTypeScalarCode::I32 || _scalarCode == DataTypeScalarCode::I64;
		}

		// Raw 32-bit pattern of the value, read through the *active* union member. Reading __raw
		// directly is unreliable: the union constructors only initialise the member they name, so
		// for anything narrower the remaining bytes are indeterminate. A 64-bit value gives its low
		// word, so a caller that wants 32 bits has to have checked they are enough (fitsIn32Bits).
		constexpr u32 rawBits() const noexcept { return static_cast<u32>(rawBits64()); }

		// The value as 64 bits: a signed integer sign-extended, an unsigned one zero-extended, a float its bit pattern.
		constexpr u64 rawBits64() const noexcept
		{
			switch (_scalarCode)
			{
				case DataTypeScalarCode::U8:  return static_cast<u64>(_value.u8Value);
				case DataTypeScalarCode::U16: return static_cast<u64>(_value.u16Value);
				case DataTypeScalarCode::U32: return static_cast<u64>(_value.u32Value);
				case DataTypeScalarCode::I8:  return static_cast<u64>(static_cast<i64>(_value.i8Value));
				case DataTypeScalarCode::I16: return static_cast<u64>(static_cast<i64>(_value.i16Value));
				case DataTypeScalarCode::I32: return static_cast<u64>(static_cast<i64>(_value.i32Value));
				case DataTypeScalarCode::F32: return static_cast<u64>(std::bit_cast<u32>(_value.f32Value));
				case DataTypeScalarCode::U64: return _value.u64Value;
				case DataTypeScalarCode::I64: return static_cast<u64>(_value.i64Value);
				case DataTypeScalarCode::F64: return std::bit_cast<u64>(_value.f64Value);
				default: return 0;
			}
		}

		constexpr u32 asRawValue() const noexcept { return rawBits(); }

		// A float's value in double precision; 0 for anything else.
		constexpr f64 asDouble() const noexcept
		{
			return isF64() ? _value.f64Value : isF32() ? static_cast<f64>(_value.f32Value) : 0.0;
		}

		// Whether an integer is the same number read as 32 bits, signed or unsigned: what every 32-bit use of a value
		// (an immediate, a size, a 32-bit datum) needs of one that came out 64 bits wide.
		constexpr bool fitsIn32Bits() const noexcept { return !isWide() || fitsInBits64(rawBits64(), 32); }

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
				case DataTypeScalarCode::U64:
				case DataTypeScalarCode::I64:
				case DataTypeScalarCode::F64: return 64;
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

		// The same over 64 bits, for a value whose source or target is a 64-bit kind.
		static constexpr bool fitsInBits64(u64 raw, u32 bits) noexcept
		{
			if (bits == 0 || bits >= 64)
				return bits >= 64;

			const u64 mask = (u64{ 1 } << bits) - 1u;
			const u64 truncated = raw & mask;
			const u64 signExtended = (truncated & (u64{ 1 } << (bits - 1))) != 0 ? (truncated | ~mask) : truncated;

			return raw == truncated || raw == signExtended;
		}

		// Re-tag this scalar as `target`. Integer literals are untyped until context gives them a
		// type, so any integer converts to any integer as long as no significant bit is lost; a
		// float converts to the other float width (f64 to f32 rounds to nearest). Returns nullopt when
		// the value does not fit, or when the conversion is between an integer and a float.
		constexpr std::optional<LiteralScalar> coerceTo(DataTypeScalarCode target) const noexcept
		{
			if (_scalarCode == target)
				return *this;

			if (isFloat() && (target == DataTypeScalarCode::F32 || target == DataTypeScalarCode::F64))
				return target == DataTypeScalarCode::F32 ? makeF32(static_cast<f32>(asDouble())) : makeF64(asDouble());

			if (!isInteger() || !DataType::isIntegerScalarCode(target))
				return std::nullopt; // No implicit conversion to or from a float.

			const u32 width = bitWidthOf(target);
			if (!isWide() && width <= 32)
			{
				// Between the 32-bit kinds, as it always was.
				const u32 raw = rawBits();
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

			// A signed source widens with its sign and an unsigned one with zeroes, then the same rule over 64 bits.
			const u64 raw = rawBits64();
			if (!fitsInBits64(raw, width))
				return std::nullopt;

			switch (target)
			{
				case DataTypeScalarCode::U8:  return makeU8(static_cast<u8>(raw));
				case DataTypeScalarCode::U16: return makeU16(static_cast<u16>(raw));
				case DataTypeScalarCode::U32: return makeU32(static_cast<u32>(raw));
				case DataTypeScalarCode::I8:  return makeI8(static_cast<i8>(raw));
				case DataTypeScalarCode::I16: return makeI16(static_cast<i16>(raw));
				case DataTypeScalarCode::I32: return makeI32(static_cast<i32>(raw));
				case DataTypeScalarCode::U64: return makeU64(raw);
				case DataTypeScalarCode::I64: return makeI64(static_cast<i64>(raw));
				default: return std::nullopt;
			}
		}

	public:
		// A typed zero, for the padding that fills a declared dimension the initialiser left short.
		static constexpr LiteralScalar makeZero(DataTypeScalarCode scalarCode) noexcept
		{
			switch (scalarCode)
			{
				case DataTypeScalarCode::U8:  return makeU8(0);
				case DataTypeScalarCode::U16: return makeU16(0);
				case DataTypeScalarCode::U32: return makeU32(0);
				case DataTypeScalarCode::I8:  return makeI8(0);
				case DataTypeScalarCode::I16: return makeI16(0);
				case DataTypeScalarCode::I32: return makeI32(0);
				case DataTypeScalarCode::F32: return makeF32(0.0f);
				case DataTypeScalarCode::U64: return makeU64(0);
				case DataTypeScalarCode::I64: return makeI64(0);
				case DataTypeScalarCode::F64: return makeF64(0.0);
				default: return makeU8(0);
			}
		}

		static constexpr LiteralScalar makeU8(u8 value) noexcept { return LiteralScalar{ DataTypeScalarCode::U8, ValueType{value} }; }
		static constexpr LiteralScalar makeU16(u16 value) noexcept { return LiteralScalar{ DataTypeScalarCode::U16, ValueType{value} }; }
		static constexpr LiteralScalar makeU32(u32 value) noexcept { return LiteralScalar{ DataTypeScalarCode::U32, ValueType{value} }; }
		static constexpr LiteralScalar makeI8(i8 value) noexcept { return LiteralScalar{ DataTypeScalarCode::I8, ValueType{value} }; }
		static constexpr LiteralScalar makeI16(i16 value) noexcept { return LiteralScalar{ DataTypeScalarCode::I16, ValueType{value} }; }
		static constexpr LiteralScalar makeI32(i32 value) noexcept { return LiteralScalar{ DataTypeScalarCode::I32, ValueType{value} }; }
		static constexpr LiteralScalar makeF32(f32 value) noexcept { return LiteralScalar{ DataTypeScalarCode::F32, ValueType{value} }; }
		static constexpr LiteralScalar makeU64(u64 value) noexcept { return LiteralScalar{ DataTypeScalarCode::U64, ValueType{value} }; }
		static constexpr LiteralScalar makeI64(i64 value) noexcept { return LiteralScalar{ DataTypeScalarCode::I64, ValueType{value} }; }
		static constexpr LiteralScalar makeF64(f64 value) noexcept { return LiteralScalar{ DataTypeScalarCode::F64, ValueType{value} }; }

		static constexpr LiteralScalar makeFromChar(char value) noexcept { return makeU8(static_cast<u8>(value)); }
		static constexpr LiteralScalar makeFromBool(bool value) noexcept { return makeU8(static_cast<u8>(value ? 1 : 0)); }

		template <typename T> requires (SameAs<T, u8> || SameAs<T, u16> || SameAs<T, u32> || SameAs<T, i8> || SameAs<T, i16> || SameAs<T, i32> || SameAs<T, f32> ||
			SameAs<T, u64> || SameAs<T, i64> || SameAs<T, f64> || SameAs<T, char> || SameAs<T, bool>)
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
			else if constexpr (SameAs<T, u64>)
				return makeU64(value);
			else if constexpr (SameAs<T, i64>)
				return makeI64(value);
			else if constexpr (SameAs<T, f64>)
				return makeF64(value);
			else if constexpr (SameAs<T, char>)
				return makeFromChar(value);
			else if constexpr (SameAs<T, bool>)
				return makeFromBool(value);
			else
				static_assert(false, "Unsupported type for LiteralScalar");
		}
	};
}
