#include "expression.h"

#include "debug_session.h"
#include <bit>
#include <cctype>
#include <format>

namespace ceres::debug
{
	namespace
	{
		using Result = std::expected<EvalResult, std::string>;

		EvalResult makeInteger(i64 value, std::string_view type = "i64")
		{
			EvalResult result;
			result.integer = value;
			result.type = type;
			result.text = std::format("{}", value);
			return result;
		}

		EvalResult makeFloat(f32 value)
		{
			EvalResult result;
			result.real = value;
			result.type = "f32";
			result.text = std::format("{}", value);
			return result;
		}

		std::string_view scalarName(ScalarType type) noexcept
		{
			switch (type)
			{
				case ScalarType::U8:  return "u8";
				case ScalarType::U16: return "u16";
				case ScalarType::U32: return "u32";
				case ScalarType::I8:  return "i8";
				case ScalarType::I16: return "i16";
				case ScalarType::I32: return "i32";
				case ScalarType::F32: return "f32";
				default:              return "?";
			}
		}

		u32 scalarSize(ScalarType type) noexcept
		{
			switch (type)
			{
				case ScalarType::U8:
				case ScalarType::I8:  return 1;
				case ScalarType::U16:
				case ScalarType::I16: return 2;
				default:              return 4;
			}
		}

		std::optional<ScalarType> scalarFromName(std::string_view name) noexcept
		{
			if (name == "u8")  return ScalarType::U8;
			if (name == "u16") return ScalarType::U16;
			if (name == "u32") return ScalarType::U32;
			if (name == "i8")  return ScalarType::I8;
			if (name == "i16") return ScalarType::I16;
			if (name == "i32") return ScalarType::I32;
			if (name == "f32") return ScalarType::F32;
			return std::nullopt;
		}

		class Evaluator
		{
		private:
			const DebugSession& _session;
			std::string_view _text;
			usize _offset = 0;
			// The grammar is recursive and the input can come from a watch window, so it needs the
			// same guard the JSON parser has.
			static constexpr u32 MaxDepth = 48;

		public:
			Evaluator(const DebugSession& session, std::string_view text) noexcept :
				_session(session), _text(text) {}

			Result run()
			{
				skipSpace();
				if (atEnd())
					return std::unexpected("Nothing to evaluate");

				auto value = parseOr(0);
				if (!value.has_value())
					return value;

				skipSpace();
				if (!atEnd())
					return std::unexpected(std::format("Unexpected '{}' at offset {}", _text[_offset], _offset));

				return value;
			}

		private:
			bool atEnd() const noexcept { return _offset >= _text.size(); }
			char peek() const noexcept { return _text[_offset]; }
			void skipSpace() noexcept
			{
				while (!atEnd() && std::isspace(static_cast<unsigned char>(peek())))
					++_offset;
			}

			bool accept(std::string_view token) noexcept
			{
				skipSpace();
				if (!_text.substr(_offset).starts_with(token))
					return false;
				_offset += token.size();
				return true;
			}

			// --- Numbers and truth ---

			static i64 toInteger(const EvalResult& value)
			{
				if (value.integer.has_value())
					return value.integer.value();
				if (value.real.has_value())
					return static_cast<i64>(value.real.value());
				return 0;
			}

			static bool eitherIsFloat(const EvalResult& a, const EvalResult& b)
			{
				return a.real.has_value() || b.real.has_value();
			}

			static f32 toFloat(const EvalResult& value)
			{
				if (value.real.has_value())
					return value.real.value();
				return static_cast<f32>(toInteger(value));
			}

			// --- Grammar, loosest binding first ---

			Result parseOr(u32 depth)
			{
				if (depth > MaxDepth)
					return std::unexpected("Expression nested too deeply");

				auto left = parseAnd(depth + 1);
				while (left.has_value() && accept("||"))
				{
					auto right = parseAnd(depth + 1);
					if (!right.has_value())
						return right;
					left = makeInteger(left->truthy() || right->truthy() ? 1 : 0, "bool");
				}
				return left;
			}

			Result parseAnd(u32 depth)
			{
				auto left = parseEquality(depth + 1);
				while (left.has_value() && accept("&&"))
				{
					auto right = parseEquality(depth + 1);
					if (!right.has_value())
						return right;
					left = makeInteger(left->truthy() && right->truthy() ? 1 : 0, "bool");
				}
				return left;
			}

			Result parseEquality(u32 depth)
			{
				auto left = parseComparison(depth + 1);
				while (left.has_value())
				{
					const bool equal = accept("==");
					const bool notEqual = !equal && accept("!=");
					if (!equal && !notEqual)
						break;

					auto right = parseComparison(depth + 1);
					if (!right.has_value())
						return right;

					bool same = false;
					if (eitherIsFloat(left.value(), right.value()))
						same = toFloat(left.value()) == toFloat(right.value());
					else if (left->integer.has_value() && right->integer.has_value())
						same = left->integer.value() == right->integer.value();
					else
						same = left->text == right->text;

					left = makeInteger((equal ? same : !same) ? 1 : 0, "bool");
				}
				return left;
			}

			Result parseComparison(u32 depth)
			{
				auto left = parseBitOr(depth + 1);
				while (left.has_value())
				{
					// Two characters before one, or '<' would swallow the '<' of "<=".
					int which = 0;
					if (accept("<=")) which = 1;
					else if (accept(">=")) which = 2;
					else if (accept("<<")) { _offset -= 2; break; } // A shift, not a comparison.
					else if (accept(">>")) { _offset -= 2; break; }
					else if (accept("<")) which = 3;
					else if (accept(">")) which = 4;
					else break;

					auto right = parseBitOr(depth + 1);
					if (!right.has_value())
						return right;

					bool outcome = false;
					if (eitherIsFloat(left.value(), right.value()))
					{
						const f32 a = toFloat(left.value());
						const f32 b = toFloat(right.value());
						outcome = which == 1 ? a <= b : which == 2 ? a >= b : which == 3 ? a < b : a > b;
					}
					else
					{
						const i64 a = toInteger(left.value());
						const i64 b = toInteger(right.value());
						outcome = which == 1 ? a <= b : which == 2 ? a >= b : which == 3 ? a < b : a > b;
					}

					left = makeInteger(outcome ? 1 : 0, "bool");
				}
				return left;
			}

			Result parseBitOr(u32 depth)
			{
				auto left = parseBitXor(depth + 1);
				while (left.has_value())
				{
					skipSpace();
					// A single '|', not the '||' that means "or".
					if (atEnd() || peek() != '|' || _text.substr(_offset).starts_with("||"))
						break;
					++_offset;

					auto right = parseBitXor(depth + 1);
					if (!right.has_value())
						return right;
					left = makeInteger(toInteger(left.value()) | toInteger(right.value()));
				}
				return left;
			}

			Result parseBitXor(u32 depth)
			{
				auto left = parseBitAnd(depth + 1);
				while (left.has_value() && accept("^"))
				{
					auto right = parseBitAnd(depth + 1);
					if (!right.has_value())
						return right;
					left = makeInteger(toInteger(left.value()) ^ toInteger(right.value()));
				}
				return left;
			}

			Result parseBitAnd(u32 depth)
			{
				auto left = parseShift(depth + 1);
				while (left.has_value())
				{
					skipSpace();
					if (atEnd() || peek() != '&' || _text.substr(_offset).starts_with("&&"))
						break;
					++_offset;

					auto right = parseShift(depth + 1);
					if (!right.has_value())
						return right;
					left = makeInteger(toInteger(left.value()) & toInteger(right.value()));
				}
				return left;
			}

			Result parseShift(u32 depth)
			{
				auto left = parseAdditive(depth + 1);
				while (left.has_value())
				{
					const bool leftShift = accept("<<");
					const bool rightShift = !leftShift && accept(">>");
					if (!leftShift && !rightShift)
						break;

					auto right = parseAdditive(depth + 1);
					if (!right.has_value())
						return right;

					const i64 amount = toInteger(right.value()) & 63;
					left = makeInteger(leftShift
						? toInteger(left.value()) << amount
						: toInteger(left.value()) >> amount);
				}
				return left;
			}

			Result parseAdditive(u32 depth)
			{
				auto left = parseMultiplicative(depth + 1);
				while (left.has_value())
				{
					const bool plus = accept("+");
					const bool minus = !plus && accept("-");
					if (!plus && !minus)
						break;

					auto right = parseMultiplicative(depth + 1);
					if (!right.has_value())
						return right;

					if (eitherIsFloat(left.value(), right.value()))
					{
						const f32 a = toFloat(left.value());
						const f32 b = toFloat(right.value());
						left = makeFloat(plus ? a + b : a - b);
					}
					else
					{
						const i64 a = toInteger(left.value());
						const i64 b = toInteger(right.value());
						left = makeInteger(plus ? a + b : a - b);
					}
				}
				return left;
			}

			Result parseMultiplicative(u32 depth)
			{
				auto left = parseUnary(depth + 1);
				while (left.has_value())
				{
					const bool times = accept("*");
					const bool divide = !times && accept("/");
					const bool modulo = !times && !divide && accept("%");
					if (!times && !divide && !modulo)
						break;

					auto right = parseUnary(depth + 1);
					if (!right.has_value())
						return right;

					if (eitherIsFloat(left.value(), right.value()) && !modulo)
					{
						const f32 a = toFloat(left.value());
						const f32 b = toFloat(right.value());
						if (divide && b == 0.0f)
							return std::unexpected("Division by zero");
						left = makeFloat(times ? a * b : a / b);
					}
					else
					{
						const i64 a = toInteger(left.value());
						const i64 b = toInteger(right.value());
						if (!times && b == 0)
							return std::unexpected("Division by zero");
						left = makeInteger(times ? a * b : divide ? a / b : a % b);
					}
				}
				return left;
			}

			Result parseUnary(u32 depth)
			{
				if (depth > MaxDepth)
					return std::unexpected("Expression nested too deeply");

				if (accept("-"))
				{
					auto value = parseUnary(depth + 1);
					if (!value.has_value())
						return value;
					if (value->real.has_value())
						return makeFloat(-value->real.value());
					return makeInteger(-toInteger(value.value()));
				}

				if (accept("!"))
				{
					auto value = parseUnary(depth + 1);
					if (!value.has_value())
						return value;
					return makeInteger(value->truthy() ? 0 : 1, "bool");
				}

				if (accept("~"))
				{
					auto value = parseUnary(depth + 1);
					if (!value.has_value())
						return value;
					return makeInteger(~toInteger(value.value()));
				}

				return parsePostfix(depth);
			}

			Result parsePostfix(u32 depth)
			{
				auto value = parsePrimary(depth);

				// Indexing, which needs the element type the base carries: `scores[2]` is the
				// third i16 of the array, not the third byte.
				while (value.has_value() && accept("["))
				{
					auto index = parseOr(depth + 1);
					if (!index.has_value())
						return index;
					if (!accept("]"))
						return std::unexpected("Expected ']'");

					if (!value->address.has_value())
						return std::unexpected(std::format("'{}' has no address, so it cannot be indexed", value->text));

					const auto element = scalarFromName(value->type).value_or(ScalarType::U32);
					const u32 size = scalarSize(element);
					const u32 at = value->address.value() + static_cast<u32>(toInteger(index.value())) * size;
					value = loadTyped(element, at);
				}

				return value;
			}

			Result parsePrimary(u32 depth)
			{
				skipSpace();
				if (atEnd())
					return std::unexpected("Expected a value");

				if (accept("("))
				{
					auto value = parseOr(depth + 1);
					if (!value.has_value())
						return value;
					if (!accept(")"))
						return std::unexpected("Expected ')'");
					return value;
				}

				// A bare [addr] is a word, the size the machine loads by default.
				if (peek() == '[')
				{
					++_offset;
					auto address = parseOr(depth + 1);
					if (!address.has_value())
						return address;
					if (!accept("]"))
						return std::unexpected("Expected ']'");
					return loadTyped(ScalarType::U32, static_cast<u32>(toInteger(address.value())));
				}

				if (std::isdigit(static_cast<unsigned char>(peek())))
					return parseNumber();

				if (std::isalpha(static_cast<unsigned char>(peek())) || peek() == '_' || peek() == '.')
					return parseIdentifier(depth);

				return std::unexpected(std::format("Unexpected '{}' at offset {}", peek(), _offset));
			}

			Result parseNumber()
			{
				const usize start = _offset;
				int base = 10;

				if (_text.substr(_offset).starts_with("0x") || _text.substr(_offset).starts_with("0X"))
				{
					base = 16;
					_offset += 2;
				}
				else if (_text.substr(_offset).starts_with("0b") || _text.substr(_offset).starts_with("0B"))
				{
					base = 2;
					_offset += 2;
				}

				const usize digitsStart = _offset;
				bool isFloat = false;
				while (!atEnd())
				{
					const char c = peek();
					if (std::isalnum(static_cast<unsigned char>(c)))
					{
						++_offset;
						continue;
					}
					if (c == '.' && base == 10 && !isFloat)
					{
						isFloat = true;
						++_offset;
						continue;
					}
					break;
				}

				const std::string digits{ _text.substr(digitsStart, _offset - digitsStart) };
				if (digits.empty())
					return std::unexpected(std::format("Malformed number at offset {}", start));

				try
				{
					if (isFloat)
						return makeFloat(std::stof(digits));

					usize consumed = 0;
					const i64 value = static_cast<i64>(std::stoull(digits, &consumed, base));
					if (consumed != digits.size())
						return std::unexpected(std::format("Malformed number '{}'", digits));
					return makeInteger(value);
				}
				catch (const std::exception&)
				{
					return std::unexpected(std::format("Malformed number '{}'", digits));
				}
			}

			Result parseIdentifier(u32 depth)
			{
				const usize start = _offset;
				while (!atEnd() && (std::isalnum(static_cast<unsigned char>(peek())) || peek() == '_' || peek() == '.'))
					++_offset;

				const std::string_view name = _text.substr(start, _offset - start);

				// A type name followed by '[' is a typed load: u8[r2].
				if (const auto type = scalarFromName(name); type.has_value())
				{
					if (accept("["))
					{
						auto address = parseOr(depth + 1);
						if (!address.has_value())
							return address;
						if (!accept("]"))
							return std::unexpected("Expected ']'");
						return loadTyped(type.value(), static_cast<u32>(toInteger(address.value())));
					}
					return std::unexpected(std::format("'{}' is a type; write {}[address] to load one", name, name));
				}

				return resolveName(name);
			}

			// --- Reading the machine ---

			Result loadTyped(ScalarType type, u32 address)
			{
				const u32 size = scalarSize(type);
				const std::vector<u8> bytes = _session.readMemory(address, size);
				if (bytes.size() < size)
					return std::unexpected(std::format("{:#010x} is outside the machine's memory", address));

				u32 raw = 0;
				for (u32 i = 0; i < size; ++i)
					raw |= static_cast<u32>(bytes[i]) << (8 * i);

				EvalResult result;
				result.address = address;
				result.type = scalarName(type);

				switch (type)
				{
					case ScalarType::I8:  result.integer = static_cast<i8>(raw); break;
					case ScalarType::I16: result.integer = static_cast<i16>(raw); break;
					case ScalarType::I32: result.integer = static_cast<i32>(raw); break;
					case ScalarType::F32: result.real = std::bit_cast<f32>(raw); break;
					default:              result.integer = static_cast<i64>(raw); break;
				}

				result.text = result.real.has_value()
					? std::format("{}", result.real.value())
					: std::format("{}", result.integer.value());
				return result;
			}

			Result resolveName(std::string_view name)
			{
				const RegisterView registers = _session.registers();

				if (name == "pc")    return makeInteger(registers.programCounter, "u32");
				if (name == "ticks") return makeInteger(static_cast<i64>(registers.executedInstructions), "u64");
				if (name == "sp")    return makeInteger(registers.general[vm::GeneralPurposeRegisterPool::StackPointerIndex], "u32");
				if (name == "fp")    return makeInteger(registers.general[vm::GeneralPurposeRegisterPool::FramePointerIndex], "u32");
				if (name == "lr")    return makeInteger(registers.general[vm::GeneralPurposeRegisterPool::LinkRegisterIndex], "u32");

				if (name == "zero")      return makeInteger(registers.zero ? 1 : 0, "bool");
				if (name == "sign")      return makeInteger(registers.sign ? 1 : 0, "bool");
				if (name == "carry")     return makeInteger(registers.carry ? 1 : 0, "bool");
				if (name == "overflow")  return makeInteger(registers.overflow ? 1 : 0, "bool");
				if (name == "interrupt") return makeInteger(registers.interruptEnabled ? 1 : 0, "bool");
				if (name == "halting")   return makeInteger(registers.halting ? 1 : 0, "bool");
				if (name == "trap")      return makeInteger(registers.trap ? 1 : 0, "bool");

				if (name.size() >= 2 && (name[0] == 'r' || name[0] == 'f') &&
					std::isdigit(static_cast<unsigned char>(name[1])))
				{
					u32 index = 0;
					bool digitsOnly = true;
					for (usize i = 1; i < name.size(); ++i)
					{
						if (!std::isdigit(static_cast<unsigned char>(name[i])))
						{
							digitsOnly = false;
							break;
						}
						index = index * 10 + static_cast<u32>(name[i] - '0');
					}

					if (digitsOnly)
					{
						if (index >= vm::GeneralPurposeRegisterPool::Count)
							return std::unexpected(std::format("'{}' is out of range; this machine has 16 of each", name));
						if (name[0] == 'r')
							return makeInteger(registers.general[index], "u32");
						return makeFloat(registers.floating[index]);
					}
				}

				const SymbolEntry* symbol = _session.debugInfo().symbolNamed(name);
				if (symbol == nullptr)
					return std::unexpected(std::format("'{}' is not a register, a flag or a known symbol", name));

				const ScalarType type = static_cast<ScalarType>(symbol->scalarType);

				if (symbol->kind == static_cast<u8>(SymbolKind::Constant))
				{
					if ((symbol->flags & SymbolFlag::HasValue) == 0)
						return std::unexpected(std::format("'{}' is a constant whose value was not recorded", name));

					EvalResult result;
					result.type = scalarName(type);
					if (type == ScalarType::F32)
						result.real = std::bit_cast<f32>(symbol->value);
					else if (type == ScalarType::I8)
						result.integer = static_cast<i8>(symbol->value);
					else if (type == ScalarType::I16)
						result.integer = static_cast<i16>(symbol->value);
					else if (type == ScalarType::I32)
						result.integer = static_cast<i32>(symbol->value);
					else
						result.integer = static_cast<i64>(symbol->value);
					result.text = result.real.has_value()
						? std::format("{}", result.real.value())
						: std::format("{}", result.integer.value());
					return result;
				}

				if (symbol->kind == static_cast<u8>(SymbolKind::Label))
					return makeInteger(symbol->address, "address");

				// A variable. A scalar reads as its value; an array keeps its address and element
				// type so indexing works, and renders the way the variables view renders it.
				if (symbol->elementCount <= 1)
					return loadTyped(type, symbol->address);

				EvalResult result;
				result.address = symbol->address;
				result.type = scalarName(type);
				for (const VariableView& variable : _session.globals())
				{
					if (variable.name == name)
					{
						result.text = variable.value;
						result.type = scalarName(type); // Element type, so [i] indexes correctly.
						break;
					}
				}
				if (result.text.empty())
					result.text = std::format("{:#010x}", symbol->address);
				return result;
			}
		};
	}

	std::expected<EvalResult, std::string> evaluate(const DebugSession& session, std::string_view expression)
	{
		return Evaluator{ session, expression }.run();
	}

	std::string interpolate(const DebugSession& session, std::string_view message)
	{
		std::string out;
		usize offset = 0;

		while (offset < message.size())
		{
			const usize open = message.find('{', offset);
			if (open == std::string_view::npos)
			{
				out += message.substr(offset);
				break;
			}

			out += message.substr(offset, open - offset);

			const usize close = message.find('}', open + 1);
			if (close == std::string_view::npos)
			{
				// An unmatched brace is text, not a malformed expression.
				out += message.substr(open);
				break;
			}

			const std::string_view expression = message.substr(open + 1, close - open - 1);
			auto value = evaluate(session, expression);
			// A failing expression is reported in place rather than aborting the message: a
			// logpoint that silently stops logging is worse than one that says what went wrong.
			out += value.has_value() ? value->text : std::format("<{}>", value.error());

			offset = close + 1;
		}

		return out;
	}
}
