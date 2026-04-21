#pragma once

#include "literal_scalar.h"
#include <vector>
#include <compare>
#include <span>

namespace ceres::casm
{
	class LiteralValue
	{
	public:
		using iterator = std::vector<LiteralScalar>::iterator;
		using const_iterator = std::vector<LiteralScalar>::const_iterator;

	private:
		std::vector<LiteralScalar> _elements = {};

	public:
		constexpr LiteralValue() = default;
		constexpr LiteralValue(const LiteralValue&) noexcept = default;
		constexpr LiteralValue(LiteralValue&&) noexcept = default;
		constexpr ~LiteralValue() noexcept = default;

		constexpr LiteralValue& operator=(const LiteralValue&) noexcept = default;
		constexpr LiteralValue& operator=(LiteralValue&&) noexcept = default;

		constexpr bool operator==(const LiteralValue&) const noexcept = default;

	private:
		constexpr explicit LiteralValue(LiteralScalar value) noexcept :
			_elements{ value }
		{}
		constexpr explicit LiteralValue(std::vector<LiteralScalar>&& elements) noexcept :
			_elements(std::move(elements))
		{}

	public:
		constexpr LiteralScalar first() const noexcept
		{
			if (_elements.empty())
				return LiteralScalar(); // Return a default LiteralScalar if the array is empty
			return _elements.front(); // Return the first element
		}
		constexpr std::span<const LiteralScalar> elements() const noexcept { return _elements; }

		constexpr bool areAllElementsSameType() const noexcept
		{
			if (_elements.empty())
				return true; // An empty array is considered to have all elements of the same type

			auto firstType = _elements.front().scalarCode();
			for (const auto& element : _elements)
			{
				if (element.scalarCode() != firstType)
					return false; // Found an element with a different type
			}
			return true; // All elements have the same type
		}

		constexpr bool allElementsMatchDataTypeScalarCode(DataTypeScalarCode expectedType) const noexcept
		{
			for (const auto& element : _elements)
			{
				if (element.scalarCode() != expectedType)
					return false; // Found an element with a different type
			}
			return true; // All elements match the expected type
		}

		constexpr DataTypeScalarCode scalarCode() const noexcept
		{
			if (_elements.empty())
				return DataTypeScalarCode::Invalid; // An empty array has no valid element type
			return _elements.front().scalarCode(); // All elements must have the same scalar type
		}

		constexpr DataType dataType() const noexcept
		{
			if (_elements.empty() || !areAllElementsSameType())
				return DataType::Invalid; // Invalid data type if the array is empty or elements have different types

			u32 arraySize = size();
			if (arraySize == 1)
				return DataType::makeScalar(scalarCode()); // Return the scalar type if there's only one element
			return DataType::makeSizedArray(scalarCode(), arraySize); // Create a DataType for the array with the scalar type and size
		}

		constexpr bool hasUnknownSize() const noexcept { return _elements.empty(); }
		constexpr bool isScalar() const noexcept { return size() == 1; }
		constexpr bool empty() const noexcept { return _elements.empty(); }
		constexpr u32 size() const noexcept { return static_cast<u32>(_elements.size()); }

		constexpr bool matchDataType(DataType expectedType) const noexcept
		{
			if (!allElementsMatchDataTypeScalarCode(expectedType.scalarCode()))
				return false; // Element types do not match the expected scalar type

			if (expectedType.hasUnknownSize() && !hasUnknownSize())
				return false; // Expected an unsized array, but this array has a known size

			if (!expectedType.hasUnknownSize() && size() != expectedType.numElements())
				return false; // Expected a sized array, but the sizes do not match

			return true; // The array matches the expected data type
		}

	public:
		constexpr iterator begin() noexcept { return _elements.begin(); }
		constexpr const_iterator begin() const noexcept { return _elements.begin(); }
		constexpr const_iterator cbegin() const noexcept { return _elements.cbegin(); }

		constexpr iterator end() noexcept { return _elements.end(); }
		constexpr const_iterator end() const noexcept { return _elements.end(); }
		constexpr const_iterator cend() const noexcept { return _elements.cend(); }

	public:
		static constexpr LiteralValue make(u8 value) noexcept { return LiteralValue(LiteralScalar::makeU8(value)); }
		static constexpr LiteralValue make(u16 value) noexcept { return LiteralValue(LiteralScalar::makeU16(value)); }
		static constexpr LiteralValue make(u32 value) noexcept { return LiteralValue(LiteralScalar::makeU32(value)); }
		static constexpr LiteralValue make(i8 value) noexcept { return LiteralValue(LiteralScalar::makeI8(value)); }
		static constexpr LiteralValue make(i16 value) noexcept { return LiteralValue(LiteralScalar::makeI16(value)); }
		static constexpr LiteralValue make(i32 value) noexcept { return LiteralValue(LiteralScalar::makeI32(value)); }
		static constexpr LiteralValue make(f32 value) noexcept { return LiteralValue(LiteralScalar::makeF32(value)); }
		static constexpr LiteralValue make(char value) noexcept { return LiteralValue(LiteralScalar::makeFromChar(value)); }
		static constexpr LiteralValue make(bool value) noexcept { return LiteralValue(LiteralScalar::makeFromBool(value)); }

		static constexpr LiteralValue make(std::vector<LiteralScalar>&& elements) noexcept
		{
			return LiteralValue(std::move(elements));
		}

		static constexpr LiteralValue make(std::span<const LiteralScalar> elements) noexcept
		{
			return LiteralValue(std::vector<LiteralScalar>(elements.begin(), elements.end()));
		}

		static constexpr LiteralValue make(std::string_view str) noexcept
		{
			std::vector<LiteralScalar> elements;
			elements.reserve(str.size() + 1);
			for (char c : str)
				elements.emplace_back(LiteralScalar::makeFromChar(c));
			elements.emplace_back(LiteralScalar::makeFromChar('\0')); // Null terminator
			return LiteralValue(std::move(elements));
		}

		static constexpr LiteralValue makeEmpty() noexcept
		{
			return LiteralValue();

		}
	};

	class LiteralValueReferenceElement
	{
	private:
		std::variant<LiteralScalar, Identifier> _value;

	public:
		constexpr LiteralValueReferenceElement() noexcept = default;
		constexpr LiteralValueReferenceElement(const LiteralValueReferenceElement&) noexcept = default;
		constexpr LiteralValueReferenceElement(LiteralValueReferenceElement&&) noexcept = default;
		constexpr ~LiteralValueReferenceElement() noexcept = default;

		constexpr LiteralValueReferenceElement& operator=(const LiteralValueReferenceElement&) noexcept = default;
		constexpr LiteralValueReferenceElement& operator=(LiteralValueReferenceElement&&) noexcept = default;

		constexpr bool operator==(const LiteralValueReferenceElement&) const noexcept = default;

	public:
		constexpr LiteralValueReferenceElement(LiteralScalar scalar) noexcept : _value(scalar) {}
		constexpr LiteralValueReferenceElement(const Identifier& identifier) noexcept : _value(identifier) {}

		constexpr bool isScalar() const noexcept { return std::holds_alternative<LiteralScalar>(_value); }
		constexpr bool isIdentifier() const noexcept { return std::holds_alternative<Identifier>(_value); }

		constexpr const LiteralScalar& scalarValue() const noexcept { return std::get<LiteralScalar>(_value); }
		constexpr const Identifier& identifierValue() const noexcept { return std::get<Identifier>(_value); }
	};

	class LiteralValueReference
	{
	public:
		using ElementType = LiteralValueReferenceElement;
		using iterator = std::vector<ElementType>::iterator;
		using const_iterator = std::vector<ElementType>::const_iterator;

	private:
		std::vector<ElementType> _elements = {};

	public:
		constexpr LiteralValueReference() noexcept = default;
		constexpr LiteralValueReference(const LiteralValueReference&) noexcept = default;
		constexpr LiteralValueReference(LiteralValueReference&&) noexcept = default;
		constexpr ~LiteralValueReference() noexcept = default;

		constexpr LiteralValueReference& operator=(const LiteralValueReference&) noexcept = default;
		constexpr LiteralValueReference& operator=(LiteralValueReference&&) noexcept = default;

		constexpr bool operator==(const LiteralValueReference&) const noexcept = default;

	private:
		constexpr explicit LiteralValueReference(LiteralScalar value) noexcept :
			_elements{ value }
		{}
		constexpr explicit LiteralValueReference(const Identifier& value) noexcept :
			_elements{ value }
		{}
		constexpr explicit LiteralValueReference(std::vector<ElementType>&& elements) noexcept :
			_elements(std::move(elements))
		{}

	public:
		constexpr LiteralValueReference(const LiteralValue& literalValue) noexcept
		{
			_elements.reserve(literalValue.size());
			for (const auto& scalar : literalValue.elements())
				_elements.emplace_back(scalar);
		}

		constexpr ElementType first() const noexcept
		{
			if (_elements.empty())
				return ElementType(); // Return a default ElementType if the array is empty
			return _elements.front(); // Return the first element
		}
		constexpr std::span<const ElementType> elements() const noexcept { return _elements; }

		constexpr bool areAllElementsSameType() const noexcept
		{
			if (_elements.empty())
				return true; // An empty array is considered to have all elements of the same type

			DataTypeScalarCode firstType = DataTypeScalarCode::Invalid;
			for (const auto& element : _elements)
			{
				if (element.isScalar())
				{
					if (firstType == DataTypeScalarCode::Invalid)
						firstType = element.scalarValue().scalarCode();
					else if (element.scalarValue().scalarCode() != firstType)
						return false; // Found an element with a different type
				}
			}
			return true; // All elements have the same type
		}

		constexpr bool allElementsMatchDataTypeScalarCode(DataTypeScalarCode expectedType) const noexcept
		{
			for (const auto& element : _elements)
			{
				if (element.isScalar() && element.scalarValue().scalarCode() != expectedType)
					return false; // Found an element with a different type
			}
			return true; // All elements match the expected type
		}

		constexpr DataTypeScalarCode scalarCode() const noexcept
		{
			if (_elements.empty())
				return DataTypeScalarCode::Invalid; // An empty array has no valid element type

			for (const auto& element : _elements)
			{
				if (element.isScalar())
					return element.scalarValue().scalarCode(); // Return the scalar type of the first scalar element
			}
			return DataTypeScalarCode::Invalid; // No scalar elements found
		}

		constexpr DataType dataType() const noexcept
		{
			if (_elements.empty() || !areAllElementsSameType())
				return DataType::Invalid; // Invalid data type if the array is empty or elements have different types

			u32 arraySize = size();
			if (arraySize == 1)
				return DataType::makeScalar(scalarCode()); // Return the scalar type if there's only one element
			return DataType::makeSizedArray(scalarCode(), arraySize); // Create a DataType for the array with the scalar type and size
		}

		constexpr bool hasUnknownSize() const noexcept { return _elements.empty(); }
		constexpr bool empty() const noexcept { return _elements.empty(); }
		constexpr u32 size() const noexcept { return static_cast<u32>(_elements.size()); }

		constexpr bool matchDataType(DataType expectedType) const noexcept
		{
			if (!allElementsMatchDataTypeScalarCode(expectedType.scalarCode()))
				return false; // Element types do not match the expected scalar type

			if (expectedType.hasUnknownSize() && !hasUnknownSize())
				return false; // Expected an unsized array, but this array has a known size

			if (!expectedType.hasUnknownSize() && size() != expectedType.numElements())
				return false; // Expected a sized array, but the sizes do not match

			return true; // The array matches the expected data type
		}

		constexpr bool matchDataType(DataTypeReference expectedType) const noexcept
		{
			if (!allElementsMatchDataTypeScalarCode(expectedType.scalarCode()))
				return false; // Element types do not match the expected scalar type

			if (expectedType.hasUnknownSize() && !hasUnknownSize())
				return false; // Expected an unsized array, but this array has a known size

			if (!expectedType.hasUnknownSize() && !expectedType.hasNumElementsIdentifier() && size() != expectedType.numElementsIntegerValue())
				return false; // Expected a sized array, but the sizes do not match

			return true; // The array matches the expected data type
		}

	public:
		constexpr iterator begin() noexcept { return _elements.begin(); }
		constexpr const_iterator begin() const noexcept { return _elements.begin(); }
		constexpr const_iterator cbegin() const noexcept { return _elements.cbegin(); }

		constexpr iterator end() noexcept { return _elements.end(); }
		constexpr const_iterator end() const noexcept { return _elements.end(); }
		constexpr const_iterator cend() const noexcept { return _elements.cend(); }

	public:
		static constexpr LiteralValueReference make(const Identifier& identifier) noexcept { return LiteralValueReference(identifier); }
		static constexpr LiteralValueReference make(u8 value) noexcept { return LiteralValueReference(LiteralScalar::makeU8(value)); }
		static constexpr LiteralValueReference make(u16 value) noexcept { return LiteralValueReference(LiteralScalar::makeU16(value)); }
		static constexpr LiteralValueReference make(u32 value) noexcept { return LiteralValueReference(LiteralScalar::makeU32(value)); }
		static constexpr LiteralValueReference make(i8 value) noexcept { return LiteralValueReference(LiteralScalar::makeI8(value)); }
		static constexpr LiteralValueReference make(i16 value) noexcept { return LiteralValueReference(LiteralScalar::makeI16(value)); }
		static constexpr LiteralValueReference make(i32 value) noexcept { return LiteralValueReference(LiteralScalar::makeI32(value)); }
		static constexpr LiteralValueReference make(f32 value) noexcept { return LiteralValueReference(LiteralScalar::makeF32(value)); }
		static constexpr LiteralValueReference make(char value) noexcept { return LiteralValueReference(LiteralScalar::makeFromChar(value)); }
		static constexpr LiteralValueReference make(bool value) noexcept { return LiteralValueReference(LiteralScalar::makeFromBool(value)); }

		static constexpr LiteralValueReference make(std::vector<ElementType>&& elements) noexcept
		{
			return LiteralValueReference(std::move(elements));
		}

		static constexpr LiteralValueReference make(std::span<const ElementType> elements) noexcept
		{
			return LiteralValueReference(std::vector<ElementType>(elements.begin(), elements.end()));
		}

		static constexpr LiteralValueReference make(std::string_view str) noexcept
		{
			std::vector<ElementType> elements;
			elements.reserve(str.size() + 1);
			for (char c : str)
				elements.emplace_back(LiteralScalar::makeFromChar(c));
			elements.emplace_back(LiteralScalar::makeFromChar('\0')); // Null terminator
			return LiteralValueReference(std::move(elements));
		}

		static constexpr LiteralValueReference makeEmpty() noexcept
		{
			return LiteralValueReference();

		}
	};
}
