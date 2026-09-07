#pragma once

#include "literal_scalar.h"
#include "const_expr.h"
#include "strings_pool.h"
#include "common/fixed_vector.h"
#include <compare>
#include <span>

namespace ceres::casm
{
	class LiteralValue
	{
	public:
		using iterator = FixedVector<LiteralScalar>::iterator;
		using const_iterator = FixedVector<LiteralScalar>::const_iterator;

	private:
		FixedVector<LiteralScalar> _elements = FixedVector<LiteralScalar>::makeEmpty();

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
		constexpr explicit LiteralValue(FixedVector<LiteralScalar>&& elements) noexcept :
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

			if (expectedType.hasUnknownSize() && hasUnknownSize())
				return false; // Both the expected type and the literal value have unknown sizes, which is not allowed

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

		static constexpr LiteralValue make(FixedVector<LiteralScalar>&& elements) noexcept
		{
			return LiteralValue(std::move(elements));
		}

		static constexpr LiteralValue make(std::vector<LiteralScalar>&& elements) noexcept
		{
			return LiteralValue(FixedVector<LiteralScalar>(std::move(elements)));
		}

		static constexpr LiteralValue make(std::span<const LiteralScalar> elements) noexcept
		{
			return LiteralValue(FixedVector<LiteralScalar>(elements));
		}

		static constexpr LiteralValue make(std::string_view str) noexcept
		{
			std::vector<LiteralScalar> elements;
			elements.reserve(str.size() + 1);
			for (char c : str)
				elements.emplace_back(LiteralScalar::makeFromChar(c));
			elements.emplace_back(LiteralScalar::makeFromChar('\0')); // Null terminator
			return LiteralValue(FixedVector<LiteralScalar>(std::move(elements)));
		}

		static constexpr LiteralValue makeEmpty() noexcept
		{
			return LiteralValue();

		}
	};

	// One element of an unresolved literal. Three shapes, because a literal is not flat any more:
	//
	//   an expression   42, BASE, BLOCK * 2, sizeof(buf)
	//   a group         [1, 2, 3] nested inside another [ ... ]
	//
	// A string literal inside an array is a group of character literals, so [ "ada", "grace" ] is
	// two groups and needs no special case anywhere below.
	class LiteralValueReferenceElement
	{
	public:
		using Group = std::vector<LiteralValueReferenceElement>;

	private:
		std::variant<ConstExpr, Group> _value;

	public:
		LiteralValueReferenceElement() noexcept = default;
		LiteralValueReferenceElement(const LiteralValueReferenceElement&) = default;
		LiteralValueReferenceElement(LiteralValueReferenceElement&&) noexcept = default;
		~LiteralValueReferenceElement() = default;

		LiteralValueReferenceElement& operator=(const LiteralValueReferenceElement&) = default;
		LiteralValueReferenceElement& operator=(LiteralValueReferenceElement&&) noexcept = default;

		bool operator==(const LiteralValueReferenceElement&) const = default;

	public:
		explicit LiteralValueReferenceElement(LiteralScalar scalar) noexcept : _value(ConstExpr::makeLiteral(scalar)) {}
		explicit LiteralValueReferenceElement(Identifier identifier) noexcept : _value(ConstExpr::makeIdentifier(identifier)) {}
		explicit LiteralValueReferenceElement(ConstExpr&& expression) noexcept : _value(std::move(expression)) {}
		explicit LiteralValueReferenceElement(Group&& group) noexcept : _value(std::move(group)) {}

		bool isExpression() const noexcept { return std::holds_alternative<ConstExpr>(_value); }
		bool isGroup() const noexcept { return std::holds_alternative<Group>(_value); }

		const ConstExpr& expression() const noexcept { return std::get<ConstExpr>(_value); }
		const Group& group() const noexcept { return std::get<Group>(_value); }

		// A plain literal or a plain identifier, which is all the type checks below can reason
		// about before resolution. Anything else is deferred to TranslationUnitBuilder.
		bool isScalar() const noexcept { return isExpression() && expression().isLiteral(); }
		bool isIdentifier() const noexcept { return isExpression() && expression().isIdentifier(); }

		LiteralScalar scalarValue() const noexcept { return expression().literal(); }
		Identifier identifierValue() const { return expression().name(); }

		// True when this element, or anything under it, cannot be inspected until the symbol table
		// exists - a group, an identifier, an arithmetic expression or a query.
		bool needsResolution() const noexcept
		{
			if (isGroup())
				return true;
			return !expression().isLiteral();
		}

	public:
		static LiteralValueReferenceElement makeGroup(Group&& group) noexcept
		{
			return LiteralValueReferenceElement(std::move(group));
		}

		// A string spans a whole trailing dimension, so it is a group of characters rather than one
		// element. The terminating zero is part of it, as it is for a top-level string.
		static LiteralValueReferenceElement makeString(LiteralString str) noexcept
		{
			Group characters;
			characters.reserve(str.size() + 1);
			for (usize i = 0; i < str.size(); ++i)
				characters.emplace_back(LiteralScalar::makeFromChar(str[i]));
			characters.emplace_back(LiteralScalar::makeFromChar('\0'));
			return LiteralValueReferenceElement(std::move(characters));
		}
	};

	class LiteralValueReference
	{
	public:
		using ElementType = LiteralValueReferenceElement;
		using iterator = std::vector<ElementType>::iterator;
		using const_iterator = std::vector<ElementType>::const_iterator;

	private:
		std::vector<ElementType> _elements;

	public:
		LiteralValueReference() noexcept = default;
		LiteralValueReference(const LiteralValueReference&) = default;
		LiteralValueReference(LiteralValueReference&&) noexcept = default;
		~LiteralValueReference() = default;

		LiteralValueReference& operator=(const LiteralValueReference&) = default;
		LiteralValueReference& operator=(LiteralValueReference&&) noexcept = default;

		bool operator==(const LiteralValueReference&) const = default;

	private:
		explicit LiteralValueReference(LiteralScalar value) noexcept :
			_elements{ LiteralValueReferenceElement{ value } }
		{}
		explicit LiteralValueReference(Identifier value) noexcept :
			_elements{ LiteralValueReferenceElement{ value } }
		{}
		explicit LiteralValueReference(std::vector<ElementType>&& elements) noexcept :
			_elements(std::move(elements))
		{}

	public:
		LiteralValueReference(const LiteralValue& literalValue) noexcept
		{
			_elements.reserve(literalValue.elements().size());
			for (LiteralScalar scalar : literalValue.elements())
				_elements.emplace_back(scalar);
		}

		// True when any element is a group or needs the symbol table. The checks below can only
		// reason about plain literals, so where this is true they stand down and let
		// TranslationUnitBuilder do the real work once the shape and the constants are known.
		bool needsResolution() const noexcept
		{
			for (const auto& element : _elements)
			{
				if (element.needsResolution())
					return true;
			}
			return false;
		}

		bool hasGroups() const noexcept
		{
			for (const auto& element : _elements)
			{
				if (element.isGroup())
					return true;
			}
			return false;
		}

		ElementType first() const noexcept
		{
			if (_elements.empty())
				return ElementType(); // Return a default ElementType if the array is empty
			return _elements.front(); // Return the first element
		}
		std::span<const ElementType> elements() const noexcept { return _elements; }

		bool areAllElementsSameType() const noexcept
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

		// An unresolved literal has not been given a type yet: integer literals are untyped until
		// the declaration provides one, so any integer element is compatible with any integer type.
		// The width check happens when the value is resolved (TranslationUnitBuilder).
		bool allElementsMatchDataTypeScalarCode(DataTypeScalarCode expectedType) const noexcept
		{
			const bool expectsInteger = DataType::isIntegerScalarCode(expectedType);

			for (const auto& element : _elements)
			{
				if (!element.isScalar())
					continue; // Identifier elements are checked once resolved.

				const DataTypeScalarCode elementCode = element.scalarValue().scalarCode();
				if (elementCode == expectedType)
					continue;

				if (expectsInteger && DataType::isIntegerScalarCode(elementCode))
					continue; // Untyped integer literal; width is validated at resolution time.

				return false; // Found an element with an incompatible type
			}
			return true; // All elements are compatible with the expected type
		}

		DataTypeScalarCode scalarCode() const noexcept
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

		DataType dataType() const noexcept
		{
			if (_elements.empty() || !areAllElementsSameType())
				return DataType::Invalid; // Invalid data type if the array is empty or elements have different types

			u32 arraySize = size();
			if (arraySize == 1)
				return DataType::makeScalar(scalarCode()); // Return the scalar type if there's only one element
			return DataType::makeSizedArray(scalarCode(), arraySize); // Create a DataType for the array with the scalar type and size
		}

		bool hasUnknownSize() const noexcept { return _elements.empty(); }
		bool empty() const noexcept { return _elements.empty(); }
		u32 size() const noexcept { return static_cast<u32>(_elements.size()); }

		bool matchDataType(DataType expectedType) const noexcept
		{
			if (needsResolution())
				return true; // Judged once the shape and the constants are known.

			if (!allElementsMatchDataTypeScalarCode(expectedType.scalarCode()))
				return false; // Element types do not match the expected scalar type

			if (expectedType.hasUnknownSize() && hasUnknownSize())
				return false; // Both the expected type and the array have unknown sizes, which is not allowed

			if (!expectedType.hasUnknownSize() && size() != expectedType.numElements())
				return false; // Expected a sized array, but the sizes do not match

			return true; // The array matches the expected data type
		}

	public:
		iterator begin() noexcept { return _elements.begin(); }
		const_iterator begin() const noexcept { return _elements.begin(); }
		const_iterator cbegin() const noexcept { return _elements.cbegin(); }

		iterator end() noexcept { return _elements.end(); }
		const_iterator end() const noexcept { return _elements.end(); }
		const_iterator cend() const noexcept { return _elements.cend(); }

	public:
		static LiteralValueReference makeIdentifier(Identifier identifier) noexcept { return LiteralValueReference(identifier); }
		static LiteralValueReference makeU8(u8 value) noexcept { return LiteralValueReference(LiteralScalar::makeU8(value)); }
		static LiteralValueReference makeU16(u16 value) noexcept { return LiteralValueReference(LiteralScalar::makeU16(value)); }
		static LiteralValueReference makeU32(u32 value) noexcept { return LiteralValueReference(LiteralScalar::makeU32(value)); }
		static LiteralValueReference makeI8(i8 value) noexcept { return LiteralValueReference(LiteralScalar::makeI8(value)); }
		static LiteralValueReference makeI16(i16 value) noexcept { return LiteralValueReference(LiteralScalar::makeI16(value)); }
		static LiteralValueReference makeI32(i32 value) noexcept { return LiteralValueReference(LiteralScalar::makeI32(value)); }
		static LiteralValueReference makeF32(f32 value) noexcept { return LiteralValueReference(LiteralScalar::makeF32(value)); }
		static LiteralValueReference makeChar(char value) noexcept { return LiteralValueReference(LiteralScalar::makeFromChar(value)); }
		static LiteralValueReference makeBool(bool value) noexcept { return LiteralValueReference(LiteralScalar::makeFromBool(value)); }

		static LiteralValueReference make(std::vector<ElementType>&& elements) noexcept
		{
			return LiteralValueReference(std::move(elements));
		}

		static LiteralValueReference makeExpression(ConstExpr&& expression) noexcept
		{
			std::vector<ElementType> elements;
			elements.emplace_back(std::move(expression));
			return LiteralValueReference(std::move(elements));
		}

		// A top-level string is a flat run of characters, so `let s: u8[4] = "abc"` keeps working
		// exactly as before. Inside an array a string is a single element instead - see
		// LiteralValueReferenceElement::makeString.
		static LiteralValueReference makeString(LiteralString str) noexcept
		{
			std::vector<ElementType> elements;
			elements.reserve(str.size() + 1);
			for (usize i = 0; i < str.size(); ++i)
				elements.emplace_back(LiteralScalar::makeFromChar(str[i]));
			elements.emplace_back(LiteralScalar::makeFromChar('\0'));
			return LiteralValueReference(std::move(elements));
		}

		static LiteralValueReference makeEmpty() noexcept
		{
			return LiteralValueReference();
		}
	};
}
