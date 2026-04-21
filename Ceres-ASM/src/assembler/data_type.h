#pragma once

#include "common_defs.h"
#include <compare>
#include <string>
#include <string_view>
#include <expected>

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

	class DataType
	{
	private:
		DataTypeScalarCode _scalarCode = DataTypeScalarCode::Invalid;
		u32 _numElements = 1; // For arrays, this represents the number of elements. For scalars, this is 1. 0 indicates an unsized array (e.g., string or unsized array).

	public:
		constexpr DataType() noexcept = default;
		constexpr DataType(const DataType&) noexcept = default;
		constexpr DataType(DataType&&) noexcept = default;
		constexpr ~DataType() noexcept = default;

		constexpr DataType& operator=(const DataType&) noexcept = default;
		constexpr DataType& operator=(DataType&&) noexcept = default;

		constexpr bool operator==(const DataType&) const noexcept = default;

	private:
		constexpr DataType(DataTypeScalarCode scalarCode, u32 numElements) noexcept :
			_scalarCode(scalarCode), _numElements(numElements)
		{}

	public:
		constexpr DataTypeScalarCode scalarCode() const noexcept { return _scalarCode; }
		constexpr u32 numElements() const noexcept { return _numElements; }

		constexpr bool isValid() const noexcept { return _scalarCode != DataTypeScalarCode::Invalid; }
		constexpr bool isScalar() const noexcept { return isValid() && _numElements == 1; }
		constexpr bool isUnsizedArray() const noexcept { return isValid() && _numElements == 0; }
		constexpr bool isSizedArray() const noexcept { return isValid() && _numElements > 1; }
		constexpr bool isArray() const noexcept { return isValid() && _numElements != 1; }
		constexpr bool hasUnknownSize() const noexcept { return !isValid() || _numElements == 0; } // Unsized array (e.g., string or unsized array)

		constexpr DataType withNumElements(u32 numElements) const noexcept { return DataType{ _scalarCode, numElements }; }
		constexpr DataType withScalarCode(DataTypeScalarCode scalarCode) const noexcept { return DataType{ scalarCode, _numElements }; }

		constexpr DataType asScalar() const noexcept { return DataType{ _scalarCode, 1 }; }
		constexpr DataType asUnsizedArray() const noexcept { return DataType{ _scalarCode, 0 }; }

		constexpr std::optional<u32> sizeInBytes() const noexcept
		{
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
			std::string result{ scalarCodeToString(_scalarCode) };

			if (isSizedArray())
				result += "[" + std::to_string(_numElements) + "]";
			else if (isUnsizedArray())
				result += "[]";

			return result;
		}

	public:
		static constexpr DataType makeScalar(DataTypeScalarCode scalarCode) noexcept { return DataType{scalarCode, 1}; }
		static constexpr DataType makeUnsizedArray(DataTypeScalarCode scalarCode) noexcept { return DataType{ scalarCode, 0 }; }
		static constexpr DataType makeSizedArray(DataTypeScalarCode scalarCode, u32 numElements) noexcept { return DataType{ scalarCode, numElements }; }

		static constexpr DataType makeChar() noexcept { return DataType{ DataTypeScalarCode::U8, 1 }; }
		static constexpr DataType makeBool() noexcept { return DataType{ DataTypeScalarCode::U8, 1 }; }
		static constexpr DataType makeString() noexcept { return DataType{ DataTypeScalarCode::U8, 0 }; } // Unsized array of u8 (null-terminated string)

		static constexpr std::expected<DataType, std::string_view> fromString(std::string_view str) noexcept
		{
			if (str == "u8") return makeScalar(DataTypeScalarCode::U8);
			if (str == "u16") return makeScalar(DataTypeScalarCode::U16);
			if (str == "u32") return makeScalar(DataTypeScalarCode::U32);
			if (str == "i8") return makeScalar(DataTypeScalarCode::I8);
			if (str == "i16") return makeScalar(DataTypeScalarCode::I16);
			if (str == "i32") return makeScalar(DataTypeScalarCode::I32);
			if (str == "f32") return makeScalar(DataTypeScalarCode::F32);
			if (str == "char") return makeChar();
			if (str == "bool") return makeBool();
			if (str == "string") return makeString();
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

	class DataTypeReference
	{
	private:
		DataTypeScalarCode _scalarCode = DataTypeScalarCode::Invalid;
		u32 _numElements = 1; // 0 indicates an unsized array (e.g., string or unsized array)
		Identifier _numElementsIdentifier{}; // Only used if _dataType is an unsized array and the size is specified by an identifier

	public:
		constexpr DataTypeReference() noexcept = default;
		constexpr DataTypeReference(const DataTypeReference&) noexcept = default;
		constexpr DataTypeReference(DataTypeReference&&) noexcept = default;
		constexpr ~DataTypeReference() noexcept = default;

		constexpr DataTypeReference& operator=(const DataTypeReference&) noexcept = default;
		constexpr DataTypeReference& operator=(DataTypeReference&&) noexcept = default;

		constexpr bool operator==(const DataTypeReference&) const noexcept = default;

	private:
		constexpr DataTypeReference(DataTypeScalarCode scalarCode, u32 numElements) noexcept :
			_scalarCode(scalarCode), _numElements(numElements)
		{}
		constexpr DataTypeReference(DataTypeScalarCode scalarCode, Identifier numElementsIdentifier) noexcept :
			_scalarCode(scalarCode), _numElements(0), _numElementsIdentifier(numElementsIdentifier)
		{}

	public:
		constexpr DataTypeReference(DataType dataType) noexcept :
			_scalarCode(dataType.scalarCode()), _numElements(dataType.numElements())
		{}

		constexpr DataTypeScalarCode scalarCode() const noexcept { return _scalarCode; }

		constexpr bool hasNumElementsIdentifier() const noexcept { return !_numElementsIdentifier.empty(); }
		constexpr Identifier numElementsIdentifier() const noexcept { return _numElementsIdentifier; }
		constexpr u32 numElementsIntegerValue() const noexcept { return !hasNumElementsIdentifier() ? _numElements : 0; }

		constexpr bool isValid() const noexcept { return _scalarCode != DataTypeScalarCode::Invalid; }
		constexpr bool isScalar() const noexcept { return isValid() && _numElements == 1 && !hasNumElementsIdentifier(); }
		constexpr bool isUnsizedArray() const noexcept { return isValid() && _numElements == 0 && !hasNumElementsIdentifier(); }
		constexpr bool isSizedArray() const noexcept { return isValid() && ((_numElements > 1 && !hasNumElementsIdentifier()) || (_numElements == 0 && hasNumElementsIdentifier())); }
		constexpr bool isArray() const noexcept { return isUnsizedArray() || isSizedArray(); }
		constexpr bool hasUnknownSize() const noexcept { return !isValid() || (_numElements == 0 && !hasNumElementsIdentifier()); }

		inline std::string toString() const noexcept
		{
			std::string result{ DataType::scalarCodeToString(_scalarCode) };

			if (isSizedArray())
				result += "[" + (hasNumElementsIdentifier() ? std::string(_numElementsIdentifier.name()) : std::to_string(_numElements)) + "]";
			else if (isUnsizedArray())
				result += "[]";

			return result;
		}

	public:
		static constexpr DataTypeReference make(DataType dataType) noexcept { return DataTypeReference{ dataType }; }
		static constexpr DataTypeReference make(DataTypeScalarCode scalarCode, u32 numElements = 1) noexcept
		{
			return DataTypeReference{ scalarCode, numElements };
		}
		static constexpr DataTypeReference make(DataTypeScalarCode scalarCode, const Identifier& numElementsIdentifier) noexcept
		{
			return DataTypeReference{ scalarCode, numElementsIdentifier };
		}
	};

	constexpr bool operator!(DataTypeScalarCode code) noexcept
	{
		return code == DataTypeScalarCode::Invalid;
	}
}
