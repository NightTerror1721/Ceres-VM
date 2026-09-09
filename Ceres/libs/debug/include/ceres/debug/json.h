#pragma once

// Just enough JSON for the debug protocol to be spoken in both directions.
//
// The project has no external dependencies and this is not the place to acquire one: the shapes
// that cross the wire are known in advance and small. Writing JSON was already being done by hand
// in two places (diagnostics in main.cpp, the debug tables in debug_info.cpp); this replaces the
// second of those and adds the reading half, which is what a request-and-response protocol needs.
//
// Deliberately not a general parser: no streaming, no big-number handling, no duplicate-key
// policy beyond last-wins. It rejects what it does not understand rather than guessing.

#include <ceres/core/base/types.h>
#include <expected>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

namespace ceres::debug::json
{
	class Value;

	using Array = std::vector<Value>;
	// Ordered, so a serialised object is byte-identical between runs; a hash map's order is not.
	using Object = std::map<std::string, Value, std::less<>>;

	class Value
	{
	private:
		// Array and Object hold Values, so they are held indirectly to break the recursion.
		using Storage = std::variant<
			std::monostate,
			bool,
			double,
			std::string,
			std::shared_ptr<Array>,
			std::shared_ptr<Object>>;

		Storage _value;

	public:
		Value() noexcept = default;
		Value(const Value&) = default;
		Value(Value&&) noexcept = default;
		~Value() = default;

		Value& operator=(const Value&) = default;
		Value& operator=(Value&&) noexcept = default;

		Value(bool value) noexcept : _value(value) {}
		Value(double value) noexcept : _value(value) {}
		Value(i32 value) noexcept : _value(static_cast<double>(value)) {}
		Value(u32 value) noexcept : _value(static_cast<double>(value)) {}
		// usize is u64 on every platform this builds for, so one overload covers both.
		Value(u64 value) noexcept : _value(static_cast<double>(value)) {}
		Value(const char* value) : _value(std::string(value)) {}
		Value(std::string value) : _value(std::move(value)) {}
		Value(std::string_view value) : _value(std::string(value)) {}
		Value(Array value) : _value(std::make_shared<Array>(std::move(value))) {}
		Value(Object value) : _value(std::make_shared<Object>(std::move(value))) {}

	public:
		bool isNull() const noexcept { return std::holds_alternative<std::monostate>(_value); }
		bool isBool() const noexcept { return std::holds_alternative<bool>(_value); }
		bool isNumber() const noexcept { return std::holds_alternative<double>(_value); }
		bool isString() const noexcept { return std::holds_alternative<std::string>(_value); }
		bool isArray() const noexcept { return std::holds_alternative<std::shared_ptr<Array>>(_value); }
		bool isObject() const noexcept { return std::holds_alternative<std::shared_ptr<Object>>(_value); }

		// Every accessor takes a fallback rather than throwing: this parses input from another
		// process, so a missing or mistyped field is a thing that happens, not an exception.
		bool asBool(bool fallback = false) const noexcept;
		double asNumber(double fallback = 0.0) const noexcept;
		u32 asU32(u32 fallback = 0) const noexcept;
		u64 asU64(u64 fallback = 0) const noexcept;
		std::string_view asString(std::string_view fallback = {}) const noexcept;

		const Array& asArray() const noexcept;
		const Object& asObject() const noexcept;

		// Null when the key is absent or this is not an object, so lookups can be chained.
		const Value& operator[](std::string_view key) const noexcept;
		bool has(std::string_view key) const noexcept;

	public:
		std::string serialize() const;
		static std::expected<Value, std::string> parse(std::string_view text);
	};

	// Escapes a string into JSON's own form. Exposed because a couple of places build JSON by hand
	// where a whole Value would be more machinery than the job needs.
	std::string escape(std::string_view text);
}
