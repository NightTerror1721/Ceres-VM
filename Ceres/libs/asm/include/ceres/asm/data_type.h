#pragma once

#include "common_defs.h"
#include "strings_pool.h"
#include <ceres/core/base/string_utils.h>
#include <compare>
#include <string>
#include <string_view>
#include <expected>
#include <array>
#include <span>

namespace ceres::casm
{
	enum class DataTypeScalarCode
	{
		Invalid = 0,

		U8,
		U16,
		U32,
		I8,
		I16,
		I32,
		F32,
	};

	// Which spelling a type was written with. An alias resolves to an underlying scalar - `ptr` is a
	// u32 - but the name is kept rather than thrown away at parse time: it is what diagnostics
	// should say, and it is what makes a range check possible for the aliases that have one.
	enum class DataTypeAlias : u8
	{
		None = 0,
		Char,   // u8  - a character
		Bool,   // u8  - 0 or 1, and nothing else
		String, // u8[] - a run of characters with a terminating zero
		Ptr,    // u32 - a memory address
		Port,   // u8  - an I/O port number
		Irq,    // u8  - an interrupt vector number, 0-63
		Byte,   // u8
		Half,   // u16
		Word,   // u32
	};

	class DataType
	{
	public:
		// [][][] is as deep as the syntax goes in practice; a fourth is headroom. Each dimension is
		// a u16, so a single one caps at 65535 elements - the total is still a u32.
		static inline constexpr u8 MaxRank = 4;

	private:
		DataTypeScalarCode _scalarCode = DataTypeScalarCode::Invalid;
		// The *total* number of scalars, which is what every consumer already wanted: sizeInBytes,
		// alignment, the emitter's flat write loop and ldv/stv all work off this and did not have to
		// change when arrays gained dimensions. 1 for a scalar, 0 for an array of unknown length.
		u32 _numElements = 1;
		// The shape that total was built from, kept only so diagnostics and dimof can report it.
		// Invariant: _numElements is the product of the first _rank entries. _rank 0 means a scalar.
		std::array<u16, MaxRank> _dims{};
		u8 _rank = 0;
		// Carried for diagnostics and validation only, which is why it is deliberately left out of
		// operator== below: `char` and `u8` are the same type, they are just not the same spelling.
		DataTypeAlias _alias = DataTypeAlias::None;

	public:
		constexpr DataType() noexcept = default;
		constexpr DataType(const DataType&) noexcept = default;
		constexpr DataType(DataType&&) noexcept = default;
		constexpr ~DataType() noexcept = default;

		constexpr DataType& operator=(const DataType&) noexcept = default;
		constexpr DataType& operator=(DataType&&) noexcept = default;

		// Compares what the type *is*, not how it was spelled.
		constexpr bool operator==(const DataType& other) const noexcept
		{
			return _scalarCode == other._scalarCode && _numElements == other._numElements
				&& _rank == other._rank && _dims == other._dims;
		}

	private:
		constexpr DataType(DataTypeScalarCode scalarCode, u32 numElements) noexcept :
			_scalarCode(scalarCode), _numElements(numElements),
			_dims{ static_cast<u16>(numElements), 0, 0, 0 }, _rank(numElements == 1 ? u8{ 0 } : u8{ 1 })
		{}

		constexpr DataType(DataTypeScalarCode scalarCode, std::span<const u32> dimensions) noexcept :
			_scalarCode(scalarCode)
		{
			_rank = static_cast<u8>(dimensions.size() < MaxRank ? dimensions.size() : MaxRank);
			u32 total = 1;
			for (u8 i = 0; i < _rank; ++i)
			{
				_dims[i] = static_cast<u16>(dimensions[i]);
				total *= dimensions[i];
			}
			_numElements = _rank == 0 ? 1 : total;
		}

	public:
		constexpr DataTypeScalarCode scalarCode() const noexcept { return _scalarCode; }
		constexpr u32 numElements() const noexcept { return _numElements; }

		constexpr u8 rank() const noexcept { return _rank; }
		constexpr bool isMultiDimensional() const noexcept { return _rank > 1; }

		// Dimensions run outermost first, matching the order they are written: i32[2][3] is 2 then 3.
		constexpr u32 dimension(u8 index) const noexcept { return index < _rank ? _dims[index] : 0; }

		constexpr DataTypeAlias alias() const noexcept { return _alias; }
		constexpr DataType withAlias(DataTypeAlias alias) const noexcept
		{
			DataType result = *this;
			result._alias = alias;
			return result;
		}

		constexpr bool isValid() const noexcept { return _scalarCode != DataTypeScalarCode::Invalid; }
		constexpr bool isScalar() const noexcept { return isValid() && _numElements == 1; }
		constexpr bool isUnsizedArray() const noexcept { return isValid() && _numElements == 0; }
		constexpr bool isSizedArray() const noexcept { return isValid() && _numElements > 1; }
		constexpr bool isArray() const noexcept { return isValid() && _numElements != 1; }
		constexpr bool hasUnknownSize() const noexcept { return !isValid() || _numElements == 0; } // Unsized array (e.g., string or unsized array)

		// Collapses to a one-dimensional array of that many elements - what every existing caller
		// means by it (a string filling an unsized u8[], a literal fixing an unsized declaration).
		constexpr DataType withNumElements(u32 numElements) const noexcept { return DataType{ _scalarCode, numElements }; }
		constexpr DataType withScalarCode(DataTypeScalarCode scalarCode) const noexcept { return DataType{ scalarCode, _numElements }; }

		constexpr DataType asScalar() const noexcept { return DataType{ _scalarCode, 1 }; }
		constexpr DataType asUnsizedArray() const noexcept { return DataType{ _scalarCode, 0 }; }

		// Natural alignment of the element type. A u32 read from an odd address works today only
		// because the VM assembles integers byte by byte; aligning keeps that an implementation
		// detail rather than a requirement.
		constexpr u32 alignment() const noexcept
		{
			switch (_scalarCode)
			{
				case DataTypeScalarCode::U8:
				case DataTypeScalarCode::I8:  return 1;
				case DataTypeScalarCode::U16:
				case DataTypeScalarCode::I16: return 2;
				case DataTypeScalarCode::U32:
				case DataTypeScalarCode::I32:
				case DataTypeScalarCode::F32: return 4;
				default: return 1;
			}
		}

		constexpr std::optional<u32> sizeInBytes() const noexcept
		{
			if (_numElements == 0)
				return std::nullopt; // Unsized array (e.g., string or unsized array) has unknown size

			u32 scalarSize = 0;
			switch (_scalarCode)
			{
				case DataTypeScalarCode::U8: scalarSize = 1; break;
				case DataTypeScalarCode::U16: scalarSize = 2; break;
				case DataTypeScalarCode::U32: scalarSize = 4; break;
				case DataTypeScalarCode::I8: scalarSize = 1; break;
				case DataTypeScalarCode::I16: scalarSize = 2; break;
				case DataTypeScalarCode::I32: scalarSize = 4; break;
				case DataTypeScalarCode::F32: scalarSize = 4; break;
				default: scalarSize = 0; break;
			}
			if (scalarSize == 0)
				return std::nullopt;
			return scalarSize * _numElements;
		}

		inline std::string toString() const noexcept
		{
			if (_alias == DataTypeAlias::String)
				return "string";

			std::string result{ _alias == DataTypeAlias::None ? scalarCodeToString(_scalarCode) : aliasToString(_alias) };

			if (_rank > 1)
			{
				for (u8 i = 0; i < _rank; ++i)
					result += "[" + std::to_string(_dims[i]) + "]";
			}
			else if (isSizedArray())
				result += "[" + std::to_string(_numElements) + "]";
			else if (isUnsizedArray())
				result += "[]";

			return result;
		}

	public:
		static constexpr DataType makeScalar(DataTypeScalarCode scalarCode) noexcept { return DataType{scalarCode, 1}; }
		static constexpr DataType makeUnsizedArray(DataTypeScalarCode scalarCode) noexcept { return DataType{ scalarCode, 0 }; }
		static constexpr DataType makeSizedArray(DataTypeScalarCode scalarCode, u32 numElements) noexcept { return DataType{ scalarCode, numElements }; }
		static constexpr DataType makeArray(DataTypeScalarCode scalarCode, std::span<const u32> dimensions) noexcept { return DataType{ scalarCode, dimensions }; }

		static constexpr DataType makeChar() noexcept { return DataType{ DataTypeScalarCode::U8, 1 }.withAlias(DataTypeAlias::Char); }
		static constexpr DataType makeBool() noexcept { return DataType{ DataTypeScalarCode::U8, 1 }.withAlias(DataTypeAlias::Bool); }
		static constexpr DataType makeString() noexcept { return DataType{ DataTypeScalarCode::U8, 0 }.withAlias(DataTypeAlias::String); } // Unsized array of u8 (null-terminated string)
		static constexpr DataType makePtr() noexcept { return DataType{ DataTypeScalarCode::U32, 1 }.withAlias(DataTypeAlias::Ptr); } // A memory address

		static constexpr std::expected<DataType, std::string_view> fromString(std::string_view str) noexcept
		{
			if (str == "u8") return makeScalar(DataTypeScalarCode::U8);
			if (str == "u16") return makeScalar(DataTypeScalarCode::U16);
			if (str == "u32") return makeScalar(DataTypeScalarCode::U32);
			if (str == "i8") return makeScalar(DataTypeScalarCode::I8);
			if (str == "i16") return makeScalar(DataTypeScalarCode::I16);
			if (str == "i32") return makeScalar(DataTypeScalarCode::I32);
			if (str == "f32") return makeScalar(DataTypeScalarCode::F32);
			if (str == "ptr") return makePtr();
			if (str == "char") return makeChar();
			if (str == "bool") return makeBool();
			if (str == "string") return makeString();
			if (str == "port") return makeScalar(DataTypeScalarCode::U8).withAlias(DataTypeAlias::Port);
			if (str == "irq") return makeScalar(DataTypeScalarCode::U8).withAlias(DataTypeAlias::Irq);
			if (str == "byte") return makeScalar(DataTypeScalarCode::U8).withAlias(DataTypeAlias::Byte);
			if (str == "half") return makeScalar(DataTypeScalarCode::U16).withAlias(DataTypeAlias::Half);
			if (str == "word") return makeScalar(DataTypeScalarCode::U32).withAlias(DataTypeAlias::Word);
			return std::unexpected("Invalid data type string");
		}
		static constexpr std::expected<DataType, std::string_view> fromString(std::string_view str, u32 numElements) noexcept
		{
			auto baseTypeResult = fromString(str);
			if (!baseTypeResult.has_value())
				return baseTypeResult;
			return baseTypeResult->withNumElements(numElements);
		}

	public:
		static constexpr bool isIntegerScalarCode(DataTypeScalarCode scalarCode) noexcept
		{
			return scalarCode == DataTypeScalarCode::U8 ||
				scalarCode == DataTypeScalarCode::U16 ||
				scalarCode == DataTypeScalarCode::U32 ||
				scalarCode == DataTypeScalarCode::I8 ||
				scalarCode == DataTypeScalarCode::I16 ||
				scalarCode == DataTypeScalarCode::I32;
		}

		static constexpr std::string_view aliasToString(DataTypeAlias alias) noexcept
		{
			switch (alias)
			{
				case DataTypeAlias::Char:   return "char";
				case DataTypeAlias::Bool:   return "bool";
				case DataTypeAlias::String: return "string";
				case DataTypeAlias::Ptr:    return "ptr";
				case DataTypeAlias::Port:   return "port";
				case DataTypeAlias::Irq:    return "irq";
				case DataTypeAlias::Byte:   return "byte";
				case DataTypeAlias::Half:   return "half";
				case DataTypeAlias::Word:   return "word";
				default: return "";
			}
		}

		// The aliases that promise something the underlying scalar does not. u8 already caps a port
		// at 255, but nothing else caps an irq at 63 or a bool at 1.
		static constexpr std::optional<u32> aliasUpperBound(DataTypeAlias alias) noexcept
		{
			switch (alias)
			{
				case DataTypeAlias::Bool: return 1u;   // false or true
				case DataTypeAlias::Irq:  return 63u;  // the interrupt vector table has 64 entries
				case DataTypeAlias::Port: return 255u;
				default: return std::nullopt;
			}
		}

		static constexpr std::string_view scalarCodeToString(DataTypeScalarCode scalarCode) noexcept
		{
			switch (scalarCode)
			{
				case DataTypeScalarCode::U8: return "u8";
				case DataTypeScalarCode::U16: return "u16";
				case DataTypeScalarCode::U32: return "u32";
				case DataTypeScalarCode::I8: return "i8";
				case DataTypeScalarCode::I16: return "i16";
				case DataTypeScalarCode::I32: return "i32";
				case DataTypeScalarCode::F32: return "f32";
				default: return "invalid";
			}
		}

	public:
		static const DataType Invalid;
		static const DataType U8;
		static const DataType U16;
		static const DataType U32;
		static const DataType I8;
		static const DataType I16;
		static const DataType I32;
		static const DataType F32;
		static const DataType Char;
		static const DataType Bool;
		static const DataType String; // Unsized array of u8 (null-terminated string)
		static const DataType Ptr;
	};

	inline constexpr const DataType DataType::Invalid = DataType{ DataTypeScalarCode::Invalid, 1 };
	inline constexpr const DataType DataType::U8 = DataType::makeScalar(DataTypeScalarCode::U8);
	inline constexpr const DataType DataType::U16 = DataType::makeScalar(DataTypeScalarCode::U16);
	inline constexpr const DataType DataType::U32 = DataType::makeScalar(DataTypeScalarCode::U32);
	inline constexpr const DataType DataType::I8 = DataType::makeScalar(DataTypeScalarCode::I8);
	inline constexpr const DataType DataType::I16 = DataType::makeScalar(DataTypeScalarCode::I16);
	inline constexpr const DataType DataType::I32 = DataType::makeScalar(DataTypeScalarCode::I32);
	inline constexpr const DataType DataType::F32 = DataType::makeScalar(DataTypeScalarCode::F32);
	inline constexpr const DataType DataType::Char = DataType::makeChar();
	inline constexpr const DataType DataType::Bool = DataType::makeBool();
	inline constexpr const DataType DataType::String = DataType::makeString(); // Unsized array of u8 (null-terminated string)
	inline constexpr const DataType DataType::Ptr = DataType::makePtr();

	constexpr bool operator!(DataTypeScalarCode code) noexcept
	{
		return code == DataTypeScalarCode::Invalid;
	}

}
