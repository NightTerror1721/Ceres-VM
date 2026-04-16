#pragma once

#include "common_defs.h"
#include <variant>
#include <vector>
#include <span>
#include <memory>

namespace ceres::casm
{
	enum class LiteralValueType
	{
		Integer,
		Float,
		Char,
		Bool,
		String,
		Array,
		Identifier
	};

	class LiteralValue
	{
	public:
		using LiteralArrayTypePtr = std::unique_ptr<std::vector<LiteralValue>>;
		using LiteralStringTypePtr = std::unique_ptr<LiteralStringType>;

	private:
		LiteralValueType _type;
		std::variant<
			LiteralIntegerType,
			LiteralFloatType,
			LiteralCharType,
			LiteralBoolType,
			LiteralStringTypePtr,
			LiteralArrayTypePtr,
			Identifier
		> _value;

	public:
		LiteralValue() noexcept : _type(LiteralValueType::Integer), _value(LiteralIntegerType{ 0 }) {}
		LiteralValue(const LiteralValue& other) noexcept : _type(other._type), _value() { _copyFrom<true>(other); }
		LiteralValue(LiteralValue&&) noexcept = default;
		~LiteralValue() noexcept = default;

		LiteralValue& operator=(const LiteralValue& other) noexcept { _copyFrom<false>(other); return *this; }
		LiteralValue& operator=(LiteralValue&&) noexcept = default;

		bool operator==(const LiteralValue& other) const noexcept
		{
			return _type == other._type && _value == other._value;
		}

	public:
		explicit LiteralValue(LiteralIntegerType value) noexcept : _type(LiteralValueType::Integer), _value(value) {}
		explicit LiteralValue(LiteralFloatType value) noexcept : _type(LiteralValueType::Float), _value(value) {}
		explicit LiteralValue(LiteralCharType value) noexcept : _type(LiteralValueType::Char), _value(value) {}
		explicit LiteralValue(LiteralBoolType value) noexcept : _type(LiteralValueType::Bool), _value(value) {}
		explicit LiteralValue(std::string_view value) noexcept : _type(LiteralValueType::String), _value(std::make_unique<LiteralStringType>(value)) {}
		explicit LiteralValue(const LiteralStringType& value) noexcept : _type(LiteralValueType::String), _value(std::make_unique<LiteralStringType>(value)) {}
		explicit LiteralValue(LiteralStringType&& value) noexcept : _type(LiteralValueType::String), _value(std::make_unique<LiteralStringType>(std::move(value))) {}
		explicit LiteralValue(std::vector<LiteralValue>&& value) noexcept :
			_type(LiteralValueType::Array), _value(std::make_unique<std::vector<LiteralValue>>(std::move(value)))
		{}
		explicit LiteralValue(const Identifier& value) noexcept : _type(LiteralValueType::Identifier), _value(value) {}
		explicit LiteralValue(Identifier&& value) noexcept : _type(LiteralValueType::Identifier), _value(std::move(value)) {}

		constexpr LiteralValueType type() const noexcept { return _type; }
		constexpr bool isInteger() const noexcept { return _type == LiteralValueType::Integer; }
		constexpr bool isFloat() const noexcept { return _type == LiteralValueType::Float; }
		constexpr bool isChar() const noexcept { return _type == LiteralValueType::Char; }
		constexpr bool isBool() const noexcept { return _type == LiteralValueType::Bool; }
		constexpr bool isString() const noexcept { return _type == LiteralValueType::String; }
		constexpr bool isArray() const noexcept { return _type == LiteralValueType::Array; }
		constexpr bool isIdentifier() const noexcept { return _type == LiteralValueType::Identifier; }

		constexpr const auto& value() const noexcept { return _value; }
		constexpr LiteralIntegerType integerValue() const noexcept { return std::get<LiteralIntegerType>(_value); }
		constexpr LiteralFloatType floatValue() const noexcept { return std::get<LiteralFloatType>(_value); }
		constexpr LiteralCharType charValue() const noexcept { return std::get<LiteralCharType>(_value); }
		constexpr LiteralBoolType boolValue() const noexcept { return std::get<LiteralBoolType>(_value); }
		constexpr const LiteralStringType& stringValue() const noexcept { return *std::get<LiteralStringTypePtr>(_value); }
		constexpr std::span<const LiteralValue> arrayValue() const noexcept
		{
			const auto& arrayPtr = std::get<LiteralArrayTypePtr>(_value);
			return std::span<const LiteralValue>(arrayPtr->begin(), arrayPtr->end());
		}
		constexpr const Identifier& identifierValue() const noexcept { return std::get<Identifier>(_value); }

		constexpr std::vector<LiteralValue>& arrayValueMutable() noexcept { return *std::get<LiteralArrayTypePtr>(_value); }

		constexpr void setValue(const LiteralValue& value) noexcept { *this = value; }

		constexpr bool isValid() const noexcept
		{
			switch (_type)
			{
				case LiteralValueType::Integer:
				case LiteralValueType::Float:
				case LiteralValueType::Char:
				case LiteralValueType::Bool:
					return true;
				case LiteralValueType::String:
					return std::get<LiteralStringTypePtr>(_value) != nullptr;
				case LiteralValueType::Array:
					return std::get<LiteralArrayTypePtr>(_value) != nullptr;
				case LiteralValueType::Identifier:
					return std::get<Identifier>(_value).isValid();
				default:
					return false;
			}
		}

		constexpr bool isValidInteger(DataType expectedType) const noexcept
		{
			if (!isInteger())
				return false;

			switch (expectedType)
			{
				case DataType::U8:
					return integerValue() <= std::numeric_limits<u8>::max();
				case DataType::U16:
					return integerValue() <= std::numeric_limits<u16>::max();
				case DataType::U32:
					return true; // All u32 values are valid for U32
				case DataType::I8:
					return integerValue() <= static_cast<LiteralIntegerType>(std::numeric_limits<i8>::max()) && integerValue() >= static_cast<LiteralIntegerType>(std::numeric_limits<i8>::min());
				case DataType::I16:
					return integerValue() <= static_cast<LiteralIntegerType>(std::numeric_limits<i16>::max()) && integerValue() >= static_cast<LiteralIntegerType>(std::numeric_limits<i16>::min());
				case DataType::I32:
					return integerValue() <= static_cast<LiteralIntegerType>(std::numeric_limits<i32>::max()) && integerValue() >= static_cast<LiteralIntegerType>(std::numeric_limits<i32>::min());
				case DataType::Char:
					return static_cast<u8>(charValue()) <= std::numeric_limits<u8>::max(); // Char is an alias for u8
				case DataType::Bool:
					return static_cast<u8>(boolValue()) <= 1; // Bool is an alias for u8, where 0 represents false and 1 represents true
				default:
					return false; // Not a valid integer data type
			}
		}

		constexpr bool checkArrayElementTypes(DataType dataType, bool permitIdentifiers, std::optional<u32> expectedSize = std::nullopt) const noexcept
		{
			if (!isArray())
				return false;

			const auto& arrayElements = arrayValue();
			if (expectedSize.has_value() && arrayElements.size() != expectedSize.value())
				return false; // Array size does not match the expected size

			for (const auto& element : arrayElements)
			{
				if (element.isIdentifier())
				{
					if (permitIdentifiers)
						continue; // Identifiers are assumed to be valid for now, actual validity will be checked during semantic analysis
					else
						return false; // Identifiers are not permitted
				}

				switch (dataType)
				{
					case DataType::U8:
					case DataType::U16:
					case DataType::U32:
					case DataType::I8:
					case DataType::I16:
					case DataType::I32:
						if (!element.isInteger() || !element.isValidInteger(dataType))
							return false;
						break;

					case DataType::F32:
						if (!element.isFloat())
							return false;
						break;

					case DataType::Char:
						if (!element.isChar())
							return false;
						break;

					case DataType::Bool:
						if (!element.isBool())
							return false;
						break;

					default:
						return false; // Unsupported data type for array elements
				}
			}
			return true;
		}

	private:
		template <bool IsConstructor>
		void _copyFrom(const LiteralValue& other) noexcept
		{
			if constexpr (IsConstructor)
			{
				_type = other._type;
				switch (_type)
				{
					case LiteralValueType::Integer:
						_value = other.integerValue();
						break;

					case LiteralValueType::Float:
						_value = other.floatValue();
						break;

					case LiteralValueType::Char:
						_value = other.charValue();
						break;

					case LiteralValueType::Bool:
						_value = other.boolValue();
						break;

					case LiteralValueType::String:
						_value = std::make_unique<LiteralStringType>(other.stringValue());
						break;

					case LiteralValueType::Array:
						_value = std::make_unique<std::vector<LiteralValue>>(other.arrayValue().begin(), other.arrayValue().end());
						break;

					case LiteralValueType::Identifier:
						_value = other.identifierValue();
						break;
				}
			}
			else
			{
				if (this != &other)
				{
					this->~LiteralValue(); // Destroy current value
					new (this) LiteralValue(other); // Placement new to copy construct
				}
			}
		}

	public:
		static LiteralValue makeInteger(LiteralIntegerType value) noexcept { return LiteralValue(value); }
		static LiteralValue makeFloat(LiteralFloatType value) noexcept { return LiteralValue(value); }
		static LiteralValue makeChar(LiteralCharType value) noexcept { return LiteralValue(value); }
		static LiteralValue makeBool(LiteralBoolType value) noexcept { return LiteralValue(value); }
		static LiteralValue makeString(std::string_view value) noexcept { return LiteralValue(value); }
		static LiteralValue makeString(const LiteralStringType& value) noexcept { return LiteralValue(value); }
		static LiteralValue makeString(LiteralStringType&& value) noexcept { return LiteralValue(std::move(value)); }
		static LiteralValue makeArray(std::vector<LiteralValue>&& value) noexcept { return LiteralValue(std::move(value)); }
		static LiteralValue makeIdentifier(const Identifier& value) noexcept { return LiteralValue(value); }
		static LiteralValue makeIdentifier(Identifier&& value) noexcept { return LiteralValue(std::move(value)); }
	};
}
