#pragma once

#include <ceres/core/base/types.h>
#include <string>
#include <string_view>
#include <unordered_set>
#include <compare>
#include <stdexcept>
#include <format>

namespace ceres::casm
{
	class Identifier;
	class LiteralString;

	class StringPool
	{
	public:
		using ValueType = std::string;
		using ViewType = std::string_view;

	private:
		struct Hasher
		{
			using is_transparent = void;
			static inline std::size_t operator()(const ValueType& str) noexcept { return std::hash<ValueType>{}(str); }
			static inline std::size_t operator()(ViewType str) noexcept { return std::hash<ViewType>{}(str); }
		};
		struct Equal
		{
			using is_transparent = void;
			static constexpr bool operator()(const ValueType& lhs, const ValueType& rhs) noexcept { return lhs == rhs; }
			static constexpr bool operator()(const ValueType& lhs, ViewType rhs) noexcept { return lhs == rhs; }
			static constexpr bool operator()(ViewType lhs, const ValueType& rhs) noexcept { return lhs == rhs; }
			static constexpr bool operator()(ViewType lhs, ViewType rhs) noexcept { return lhs == rhs; }
		};

	private:
		std::unordered_set<ValueType, Hasher, Equal> _strings;

	public:
		StringPool() = default;
		StringPool(const StringPool&) = delete;
		StringPool(StringPool&&) = default;
		~StringPool() = default;

		StringPool& operator=(const StringPool&) = delete;
		StringPool& operator=(StringPool&&) = default;

	public:
		inline const ValueType& intern(ViewType str)
		{
			if (const auto it = _strings.find(str); it != _strings.end())
				return *it;

			auto [it, _] = _strings.emplace(str);
			return *it;
		}
		inline const ValueType& intern(const ValueType& str)
		{
			if (const auto it = _strings.find(str); it != _strings.end())
				return *it;

			auto [it, _] = _strings.emplace(str);
			return *it;
		}
		inline const ValueType& intern(ValueType&& str)
		{
			if (const auto it = _strings.find(str); it != _strings.end())
				return *it;

			auto [it, _] = _strings.emplace(std::move(str));
			return *it;
		}

		inline std::pair<const ValueType&, bool> tryIntern(ViewType str)
		{
			if (const auto it = _strings.find(str); it != _strings.end())
				return { *it, false };

			auto [it, inserted] = _strings.emplace(str);
			return { *it, inserted };
		}
		inline std::pair<const ValueType&, bool> tryIntern(const ValueType& str)
		{
			if (const auto it = _strings.find(str); it != _strings.end())
				return { *it, false };

			auto [it, inserted] = _strings.emplace(str);
			return { *it, inserted };
		}
		inline std::pair<const ValueType&, bool> tryIntern(ValueType&& str)
		{
			if (const auto it = _strings.find(str); it != _strings.end())
				return { *it, false };

			auto [it, inserted] = _strings.emplace(std::move(str));
			return { *it, inserted };
		}

	public:
		Identifier makeIdentifier(ViewType name) noexcept;
		Identifier makeIdentifier(const ValueType& name) noexcept;
		Identifier makeIdentifier(ValueType&& name) noexcept;

		LiteralString makeLiteralString(ViewType str) noexcept;
		LiteralString makeLiteralString(const ValueType& str) noexcept;
		LiteralString makeLiteralString(ValueType&& str) noexcept;
	};

	class PooledString
	{
	public:
		using PoolType = StringPool;
		using ValueType = PoolType::ValueType;
		using ViewType = PoolType::ViewType;
		using PointerType = const ValueType*;
		using ReferenceType = const ValueType&;
		using DataType = const ValueType::value_type*;
		using CharType = ValueType::value_type;
		using SizeType = ValueType::size_type;
		using iterator = ValueType::const_iterator;
		using const_iterator = ValueType::const_iterator;

	protected:
		PointerType _value;

	public:
		constexpr PooledString(const PooledString&) noexcept = default;
		constexpr PooledString(PooledString&&) noexcept = default;
		constexpr ~PooledString() noexcept = default;

		constexpr PooledString& operator=(const PooledString&) noexcept = default;
		constexpr PooledString& operator=(PooledString&&) noexcept = default;

		constexpr bool operator==(const PooledString& other) const noexcept { return _value == other._value; }
		constexpr auto operator<=>(const PooledString& other) const noexcept { return *_value <=> *other._value; }

	protected:
		constexpr explicit PooledString() noexcept : _value(nullptr) {}
		constexpr explicit PooledString(ReferenceType value) noexcept : _value(&value) {}

	public:
		[[nodiscard]] constexpr bool isEmpty() const noexcept { return _value == nullptr; }
		[[nodiscard]] constexpr SizeType size() const noexcept { return _value->size(); }

		[[nodiscard]] constexpr ViewType view() const noexcept { return *_value; }
		[[nodiscard]] constexpr ReferenceType str() const noexcept { return *_value; }
		[[nodiscard]] constexpr DataType data() const noexcept { return _value->data(); }

		[[nodiscard]] constexpr CharType at(SizeType pos) const { return _value->at(pos); }
		
	public:
		[[nodiscard]] constexpr operator ViewType() const noexcept { return *_value; }

		[[nodiscard]] constexpr CharType operator[](SizeType pos) const noexcept { return (*_value)[pos]; }

	public:
		const_iterator begin() const noexcept { return _value->begin(); }
		const_iterator cbegin() const noexcept { return _value->cbegin(); }

		const_iterator end() const noexcept { return _value->end(); }
		const_iterator cend() const noexcept { return _value->cend(); }
	};

	class NullablePooledString : public PooledString
	{
	public:
		constexpr NullablePooledString(const NullablePooledString&) noexcept = default;
		constexpr NullablePooledString(NullablePooledString&&) noexcept = default;
		constexpr ~NullablePooledString() noexcept = default;

		constexpr NullablePooledString& operator=(const NullablePooledString&) noexcept = default;
		constexpr NullablePooledString& operator=(NullablePooledString&&) noexcept = default;

	protected:
		constexpr NullablePooledString() noexcept : PooledString() {}
		constexpr explicit NullablePooledString(ReferenceType value) noexcept : PooledString(value) {}

	public:
		constexpr NullablePooledString(const PooledString& pooledString) noexcept : PooledString(pooledString) {}
		constexpr NullablePooledString(PooledString&& pooledString) noexcept : PooledString(std::move(pooledString)) {}

		constexpr NullablePooledString& operator=(const PooledString& pooledString) noexcept
		{
			PooledString::operator=(pooledString);
			return *this;
		}
		constexpr NullablePooledString& operator=(PooledString&& pooledString) noexcept
		{
			PooledString::operator=(std::move(pooledString));
			return *this;
		}

	public:
		constexpr bool isNull() const noexcept { return _value == nullptr; }

		constexpr explicit operator bool() const noexcept { return !isNull(); }
		constexpr bool operator!() const noexcept { return isNull(); }

		constexpr bool operator==(const NullablePooledString& other) const noexcept
		{
			if (isNull())
				return other.isNull();
			if (other.isNull())
				return false;
			return PooledString::operator==(other);
		}

		constexpr auto operator<=>(const NullablePooledString& other) const noexcept
		{
			if (isNull())
				return other.isNull() ? std::strong_ordering::equal : std::strong_ordering::less;
			if (other.isNull())
				return std::strong_ordering::greater;
			return PooledString::operator<=>(other);
		}
	};

	class Identifier;
	class NullableIdentifier;

	class Identifier : public PooledString
	{
		friend class StringPool;
		friend class NullableIdentifier;

	public:
		constexpr Identifier() noexcept = delete;
		constexpr Identifier(const Identifier&) noexcept = default;
		constexpr Identifier(Identifier&&) noexcept = default;
		constexpr ~Identifier() noexcept = default;

		constexpr Identifier& operator=(const Identifier&) noexcept = default;
		constexpr Identifier& operator=(Identifier&&) noexcept = default;

	private:
		constexpr explicit Identifier(ReferenceType value) noexcept : PooledString(value) {}

	public:
		static inline Identifier makeFromStringPool(StringPool& pool, ViewType name) { return Identifier(pool.intern(name)); }
		static inline Identifier makeFromStringPool(StringPool& pool, const ValueType& name) { return Identifier(pool.intern(name)); }
		static inline Identifier makeFromStringPool(StringPool& pool, ValueType&& name) { return Identifier(pool.intern(std::move(name))); }

	public:
		static inline constexpr bool isAsciiAlpha(char ch) noexcept { return (ch >= 'A' && ch <= 'Z') || (ch >= 'a' && ch <= 'z'); }
		static inline constexpr bool isAsciiDigit(char ch) noexcept { return ch >= '0' && ch <= '9'; }
		static inline constexpr bool isAsciiAlnum(char ch) noexcept { return isAsciiAlpha(ch) || isAsciiDigit(ch); }

		static inline constexpr bool isValidIdentifierName(std::string_view name) noexcept
		{
			if (name.empty())
				return false;

			if (!isAsciiAlpha(name.front()) && name.front() != '_')
				return false;

			for (char c : name)
			{
				if (!isAsciiAlnum(c) && c != '_')
					return false;
			}

			return true;
		}
	};

	class NullableIdentifier : public NullablePooledString
	{
		friend class StringPool;

	public:
		constexpr NullableIdentifier() noexcept : NullablePooledString() {}
		constexpr NullableIdentifier(const NullableIdentifier&) noexcept = default;
		constexpr NullableIdentifier(NullableIdentifier&&) noexcept = default;
		constexpr ~NullableIdentifier() noexcept = default;

		constexpr NullableIdentifier& operator=(const NullableIdentifier&) noexcept = default;
		constexpr NullableIdentifier& operator=(NullableIdentifier&&) noexcept = default;

	private:
		constexpr explicit NullableIdentifier(ReferenceType value) noexcept : NullablePooledString(value) {}

	public:
		constexpr NullableIdentifier(decltype(nullptr)) noexcept : NullablePooledString() {}
		constexpr NullableIdentifier(const Identifier& identifier) noexcept : NullablePooledString(identifier) {}
		constexpr NullableIdentifier(Identifier&& identifier) noexcept : NullablePooledString(std::move(identifier)) {}

		constexpr NullableIdentifier& operator=(const Identifier& identifier) noexcept
		{
			NullablePooledString::operator=(identifier);
			return *this;
		}
		constexpr NullableIdentifier& operator=(Identifier&& identifier) noexcept
		{
			NullablePooledString::operator=(std::move(identifier));
			return *this;
		}

		inline explicit operator Identifier() const
		{
			if (isNull())
				throw std::runtime_error("Cannot convert null NullableIdentifier to Identifier.");
			return Identifier(*_value);
		}

	public:
		static inline NullableIdentifier makeFromStringPool(StringPool& pool, ViewType name) { return NullableIdentifier(pool.intern(name)); }
		static inline NullableIdentifier makeFromStringPool(StringPool& pool, const ValueType& name) { return NullableIdentifier(pool.intern(name)); }
		static inline NullableIdentifier makeFromStringPool(StringPool& pool, ValueType&& name) { return NullableIdentifier(pool.intern(std::move(name))); }
	};

	class LiteralString : public PooledString
	{
		friend class StringPool;

	public:
		constexpr LiteralString() noexcept = delete;
		constexpr LiteralString(const LiteralString&) noexcept = default;
		constexpr LiteralString(LiteralString&&) noexcept = default;
		constexpr ~LiteralString() noexcept = default;

		constexpr LiteralString& operator=(const LiteralString&) noexcept = default;
		constexpr LiteralString& operator=(LiteralString&&) noexcept = default;

	private:
		constexpr explicit LiteralString(ReferenceType value) noexcept : PooledString(value) {}

	public:
		static inline LiteralString makeFromStringPool(StringPool& pool, ViewType str) { return LiteralString(pool.intern(str)); }
		static inline LiteralString makeFromStringPool(StringPool& pool, const ValueType& str) { return LiteralString(pool.intern(str)); }
		static inline LiteralString makeFromStringPool(StringPool& pool, ValueType&& str) { return LiteralString(pool.intern(std::move(str))); }
	};

	inline Identifier StringPool::makeIdentifier(ViewType name) noexcept { return Identifier::makeFromStringPool(*this, name); }
	inline Identifier StringPool::makeIdentifier(const ValueType& name) noexcept { return Identifier::makeFromStringPool(*this, name); }
	inline Identifier StringPool::makeIdentifier(ValueType&& name) noexcept { return Identifier::makeFromStringPool(*this, std::move(name)); }

	inline LiteralString StringPool::makeLiteralString(ViewType str) noexcept { return LiteralString::makeFromStringPool(*this, str); }
	inline LiteralString StringPool::makeLiteralString(const ValueType& str) noexcept { return LiteralString::makeFromStringPool(*this, str); }
	inline LiteralString StringPool::makeLiteralString(ValueType&& str) noexcept { return LiteralString::makeFromStringPool(*this, std::move(str)); }
}

// Without these, std::format picks the range formatter (PooledString exposes begin/end) and an
// identifier prints as ['m', 'a', 'i', 'n'] instead of main.
template <>
struct std::formatter<ceres::casm::Identifier> : std::formatter<std::string_view>
{
	auto format(const ceres::casm::Identifier& value, std::format_context& ctx) const
	{
		return std::formatter<std::string_view>::format(value.view(), ctx);
	}
};

template <>
struct std::formatter<ceres::casm::LiteralString> : std::formatter<std::string_view>
{
	auto format(const ceres::casm::LiteralString& value, std::format_context& ctx) const
	{
		return std::formatter<std::string_view>::format(value.view(), ctx);
	}
};

template <>
struct std::formatter<ceres::casm::NullableIdentifier> : std::formatter<std::string_view>
{
	auto format(const ceres::casm::NullableIdentifier& value, std::format_context& ctx) const
	{
		return std::formatter<std::string_view>::format(value.isNull() ? std::string_view{ "<null>" } : value.view(), ctx);
	}
};
