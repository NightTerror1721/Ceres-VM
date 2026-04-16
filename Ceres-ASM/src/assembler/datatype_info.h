#pragma once

#include "common_defs.h"
#include "literal_value.h"
#include <variant>

namespace ceres::casm
{


	enum class DataTypeCategory : u8
	{
		Scalar, // Scalar data types (u8, u16, u32, i8, i16, i32, f32, char, bool)
		UnsizedArray, // Unsized array data type (e.g., string, which is an alias for u8[] and represents a null-terminated array of bytes, i16[], etc.)
		SizedArray // Sized array data type (e.g., u8[10], char[20], etc.)
	};

	class DataTypeInfo
	{
	private:
		DataType _type;
		DataTypeCategory _category;
		// For unsized arrays, arraySize should be 0 (indicating that the size is not specified). For sized arrays, arraySize should be greater than 0.
		std::variant<std::monostate, LiteralIntegerType, Identifier> _arraySize;

	public:
		constexpr DataTypeInfo() noexcept : _type(static_cast<DataType>(-1)), _category(DataTypeCategory::Scalar), _arraySize(std::monostate{}) {}

		constexpr DataTypeInfo(DataType type, DataTypeCategory category, LiteralIntegerType arraySize) noexcept
			: _type(type), _category(category), _arraySize(arraySize)
		{}

		constexpr DataTypeInfo(DataType type, DataTypeCategory category) noexcept
			: _type(type), _category(category), _arraySize(std::monostate{})
		{}

		constexpr DataTypeInfo(DataType type, DataTypeCategory category, const Identifier& arraySizeIdentifier) noexcept
			: _type(type), _category(category), _arraySize(arraySizeIdentifier)
		{}
		constexpr DataTypeInfo(DataType type, DataTypeCategory category, Identifier&& arraySizeIdentifier) noexcept
			: _type(type), _category(category), _arraySize(std::move(arraySizeIdentifier))
		{}

		constexpr DataTypeInfo(DataType type) noexcept
			: _type(type), _category(DataTypeCategory::Scalar), _arraySize(std::monostate{})
		{}

		constexpr DataType type() const noexcept { return _type; }
		constexpr DataTypeCategory category() const noexcept { return _category; }

		constexpr bool isScalar() const noexcept { return _category == DataTypeCategory::Scalar; }
		constexpr bool isUnsizedArray() const noexcept { return _category == DataTypeCategory::UnsizedArray; }
		constexpr bool isSizedArray() const noexcept { return _category == DataTypeCategory::SizedArray; }
		constexpr bool isString() const noexcept { return _category == DataTypeCategory::UnsizedArray && _type == DataType::String; }

		constexpr bool hasArraySize() const noexcept { return std::holds_alternative<LiteralIntegerType>(_arraySize) || std::holds_alternative<Identifier>(_arraySize); }
		constexpr bool hasIntegerArraySize() const noexcept { return std::holds_alternative<LiteralIntegerType>(_arraySize); }
		constexpr bool hasIdentifierArraySize() const noexcept { return std::holds_alternative<Identifier>(_arraySize); }

		constexpr LiteralIntegerType integerArraySize() const noexcept { return std::holds_alternative<LiteralIntegerType>(_arraySize) ? std::get<LiteralIntegerType>(_arraySize) : 0; }
		constexpr const Identifier& identifierArraySize() const noexcept { return std::get<Identifier>(_arraySize); }

		constexpr void setArraySize(LiteralIntegerType size) noexcept
		{
			if (_category == DataTypeCategory::SizedArray)
				_arraySize = size;
		}

		constexpr bool isValid() const noexcept
		{
			if (_category == DataTypeCategory::Scalar)
				return isScalarDataType(_type);

			if (_category == DataTypeCategory::UnsizedArray)
				return std::holds_alternative<std::monostate>(_arraySize); // Unsized arrays must have an array size of 0 (indicating that the size is not specified)

			if (_category == DataTypeCategory::SizedArray)
			{
				if (!isScalarDataType(_type))
					return false; // Sized arrays must be of scalar types
				if (std::holds_alternative<LiteralIntegerType>(_arraySize) && std::get<LiteralIntegerType>(_arraySize) == 0)
					return false; // Sized arrays cannot have an array size of 0
				// If the array size is specified as an identifier, we cannot determine its validity at compile time, so we assume it's valid.
				// The actual validity of the identifier will need to be checked during semantic analysis.
				return std::holds_alternative<Identifier>(_arraySize) && std::get<Identifier>(_arraySize).isValid();
			}

			return false;
		}

		constexpr bool matchLiteralValue(const LiteralValue& value, bool permitIdentifiers) const noexcept
		{
			if (!isValid())
				return false;

			switch (_category)
			{
				case DataTypeCategory::Scalar:
					switch (_type)
					{
						case DataType::U8:
						case DataType::U16:
						case DataType::U32:
						case DataType::I8:
						case DataType::I16:
						case DataType::I32:
							return value.isInteger() && value.isValidInteger(_type);

						case DataType::F32:
							return value.isFloat();

						case DataType::Char:
							return value.isChar();

						case DataType::Bool:
							return value.isBool();

						default:
							return false;
					}
				case DataTypeCategory::UnsizedArray:
					if (_type == DataType::String)
						return value.isString(); // Unsized array of type string matches string literal

					// Unsized array of other types matches array literal with matching element types
					return value.isArray() && value.checkArrayElementTypes(_type, permitIdentifiers);

				case DataTypeCategory::SizedArray:
					if (std::holds_alternative<LiteralIntegerType>(_arraySize))
					{
						LiteralIntegerType size = std::get<LiteralIntegerType>(_arraySize);
						if (value.isArray())
							return value.checkArrayElementTypes(_type, permitIdentifiers, size); // Sized array matches array literal with matching element types and size
					}
					else if (std::holds_alternative<Identifier>(_arraySize))
					{
						if (!permitIdentifiers)
							return false; // Identifiers are not permitted, so we cannot match a sized array with an identifier size

						// If the array size is specified as an identifier, we cannot determine its validity at compile time,
						// so we assume it's valid. The actual validity of the identifier will need to be checked during semantic analysis.
						// Sized array matches array literal with matching element types, size will be checked during semantic analysis
						if (value.isArray())
							return value.checkArrayElementTypes(_type, permitIdentifiers);
					}
					return false;

				default:
					return false;
			}
		}

		constexpr u32 sizeInBytes() const noexcept
		{
			if (!isValid())
				return 0;

			switch (_category)
			{
			case DataTypeCategory::Scalar:
				return dataTypeSizeInBytes(_type);

			case DataTypeCategory::UnsizedArray:
				return 0; // Unsized arrays have no fixed size

			case DataTypeCategory::SizedArray:
				if (std::holds_alternative<LiteralIntegerType>(_arraySize))
					return dataTypeSizeInBytes(_type) * std::get<LiteralIntegerType>(_arraySize);
				else if (std::holds_alternative<Identifier>(_arraySize))
					return 0; // Size is determined at runtime, so we cannot calculate the size in bytes at compile time
				else
					return 0; // Invalid array size

			default:
				return 0; // Invalid category
			}
		}

		std::string toString() const noexcept
		{
			if (!isValid())
				return "Invalid DataTypeInfo";

			std::string result;
			switch (_category)
			{
				case DataTypeCategory::Scalar:
					result = dataTypeToString(_type);
					break;
				case DataTypeCategory::UnsizedArray:
					result = std::string(dataTypeToString(_type)) + "[]";
					break;
				case DataTypeCategory::SizedArray:
					result = std::string(dataTypeToString(_type)) + "[";
					if (std::holds_alternative<LiteralIntegerType>(_arraySize))
						result += std::to_string(std::get<LiteralIntegerType>(_arraySize));
					else if (std::holds_alternative<Identifier>(_arraySize))
						result += std::get<Identifier>(_arraySize).name();
					result += "]";
					break;
			}
			return result;
		}

	public:
		static constexpr DataTypeInfo makeScalar(DataType type) noexcept
		{
			if (!isScalarDataType(type))
				return {}; // Invalid DataTypeInfo
			return DataTypeInfo(type, DataTypeCategory::Scalar);
		}

		static constexpr DataTypeInfo makeUnsizedArray(DataType elementType) noexcept
		{
			return DataTypeInfo(elementType, DataTypeCategory::UnsizedArray);
		}

		static constexpr DataTypeInfo makeSizedArray(DataType elementType, u32 arraySize) noexcept
		{
			if (!isScalarDataType(elementType) || arraySize == 0)
				return {}; // Invalid DataTypeInfo
			return DataTypeInfo(elementType, DataTypeCategory::SizedArray, arraySize);
		}
		static constexpr DataTypeInfo makeSizedArray(DataType elementType, const Identifier& arraySizeIdentifier) noexcept
		{
			if (!isScalarDataType(elementType) || arraySizeIdentifier.isValid())
				return {}; // Invalid DataTypeInfo
			return DataTypeInfo(elementType, DataTypeCategory::SizedArray, arraySizeIdentifier);
		}
		static constexpr DataTypeInfo makeSizedArray(DataType elementType, Identifier&& arraySizeIdentifier) noexcept
		{
			if (!isScalarDataType(elementType) || arraySizeIdentifier.isValid())
				return {}; // Invalid DataTypeInfo
			return DataTypeInfo(elementType, DataTypeCategory::SizedArray, std::move(arraySizeIdentifier));
		}
	};
}
