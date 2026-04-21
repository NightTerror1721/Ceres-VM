#pragma once

#include "common_defs.h"
#include "data_type.h"
#include <variant>

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

		constexpr u32 asRawValue() const noexcept { return _value.__raw; }

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
