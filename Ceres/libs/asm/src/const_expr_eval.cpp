#include <ceres/asm/const_expr_eval.h>
#include <ceres/asm/symbol_table.h>
#include <format>
#include <limits>

namespace ceres::casm
{
	namespace
	{
		std::expected<LiteralScalar, std::string> evaluateQuery(const ConstExpr& expression, const ConstExprSymbolLookup& lookup)
		{
			const std::string name{ expression.name().view() };
			const Symbol* symbol = lookup ? lookup(name) : nullptr;
			if (symbol == nullptr)
				return std::unexpected(std::format("'{}' is not declared", name));

			if (!symbol->hasDataType())
				return std::unexpected(std::format("'{}' has no data type to ask about", name));

			const DataType dataType = symbol->dataType();

			switch (expression.query())
			{
				case ConstExpr::Query::SizeOf:
				{
					const auto size = dataType.sizeInBytes();
					if (!size.has_value())
						return std::unexpected(std::format("the size of '{}' is not known", name));
					return LiteralScalar::makeU32(size.value());
				}

				case ConstExpr::Query::CountOf:
					return LiteralScalar::makeU32(dataType.numElements());

				case ConstExpr::Query::DimOf:
				{
					const u32 index = expression.dimensionIndex();
					// A scalar has no dimensions; a one-dimensional array has exactly one, whether or
					// not it was declared with a rank.
					const u8 rank = dataType.rank();
					if (index >= rank)
						return std::unexpected(std::format(
							"'{}' is {}, which has no dimension {}", name, dataType.toString(), index));
					return LiteralScalar::makeU32(dataType.dimension(static_cast<u8>(index)));
				}

				default:
					return std::unexpected("Unknown query");
			}
		}
	}

	namespace
	{
		// An integer operand as a 64-bit signed number: a signed one keeps its sign, an unsigned one does not grow one.
		i64 wideInteger(const LiteralScalar& value) noexcept { return static_cast<i64>(value.rawBits64()); }

		// An operand as a double: a float as it is, an integer as the number it is.
		f64 wideFloat(const LiteralScalar& value) noexcept
		{
			if (value.isFloat())
				return value.asDouble();
			return value.isWide() || value.isSigned() ? static_cast<f64>(wideInteger(value)) : static_cast<f64>(value.rawBits64());
		}

		std::expected<LiteralScalar, std::string> wideFloatBinary(ConstExpr::Op op, f64 a, f64 b)
		{
			switch (op)
			{
				case ConstExpr::Op::Add: return LiteralScalar::makeF64(a + b);
				case ConstExpr::Op::Subtract: return LiteralScalar::makeF64(a - b);
				case ConstExpr::Op::Multiply: return LiteralScalar::makeF64(a * b);
				case ConstExpr::Op::Divide:
					if (b == 0.0)
						return std::unexpected("Division by zero in constant expression");
					return LiteralScalar::makeF64(a / b);
				case ConstExpr::Op::Equal: return LiteralScalar::makeI32(a == b ? 1 : 0);
				case ConstExpr::Op::NotEqual: return LiteralScalar::makeI32(a != b ? 1 : 0);
				case ConstExpr::Op::Less: return LiteralScalar::makeI32(a < b ? 1 : 0);
				case ConstExpr::Op::LessEqual: return LiteralScalar::makeI32(a <= b ? 1 : 0);
				case ConstExpr::Op::Greater: return LiteralScalar::makeI32(a > b ? 1 : 0);
				case ConstExpr::Op::GreaterEqual: return LiteralScalar::makeI32(a >= b ? 1 : 0);
				case ConstExpr::Op::Modulo:
					return std::unexpected("'%' has no meaning for floating-point values");
				default: return std::unexpected("Unknown operator in constant expression");
			}
		}

		// 64-bit integer arithmetic, wrapping as the machine's does rather than as signed overflow in C++ would.
		std::expected<LiteralScalar, std::string> wideIntegerBinary(ConstExpr::Op op, i64 a, i64 b)
		{
			const u64 ua = static_cast<u64>(a);
			const u64 ub = static_cast<u64>(b);
			constexpr i64 Min = std::numeric_limits<i64>::min();
			switch (op)
			{
				case ConstExpr::Op::Add: return LiteralScalar::makeI64(static_cast<i64>(ua + ub));
				case ConstExpr::Op::Subtract: return LiteralScalar::makeI64(static_cast<i64>(ua - ub));
				case ConstExpr::Op::Multiply: return LiteralScalar::makeI64(static_cast<i64>(ua * ub));
				case ConstExpr::Op::Divide:
					if (b == 0)
						return std::unexpected("Division by zero in constant expression");
					return LiteralScalar::makeI64(a == Min && b == -1 ? Min : a / b);
				case ConstExpr::Op::Modulo:
					if (b == 0)
						return std::unexpected("Remainder by zero in constant expression");
					return LiteralScalar::makeI64(a == Min && b == -1 ? 0 : a % b);
				case ConstExpr::Op::Equal: return LiteralScalar::makeI32(a == b ? 1 : 0);
				case ConstExpr::Op::NotEqual: return LiteralScalar::makeI32(a != b ? 1 : 0);
				case ConstExpr::Op::Less: return LiteralScalar::makeI32(a < b ? 1 : 0);
				case ConstExpr::Op::LessEqual: return LiteralScalar::makeI32(a <= b ? 1 : 0);
				case ConstExpr::Op::Greater: return LiteralScalar::makeI32(a > b ? 1 : 0);
				case ConstExpr::Op::GreaterEqual: return LiteralScalar::makeI32(a >= b ? 1 : 0);
				default: return std::unexpected("Unknown operator in constant expression");
			}
		}
	}

	std::expected<LiteralScalar, std::string> evaluateConstExpr(const ConstExpr& expression, const ConstExprSymbolLookup& lookup, bool wide)
	{
		switch (expression.kind())
		{
			case ConstExpr::Kind::Literal:
			{
				// A float literal is read to double precision; outside a 64-bit context it is the float it always was.
				const LiteralScalar literal = expression.literal();
				if (literal.isF64() && !wide)
					return LiteralScalar::makeF32(static_cast<f32>(literal.asDouble()));
				return literal;
			}

			case ConstExpr::Kind::Identifier:
			{
				const std::string name{ expression.name().view() };
				const Symbol* symbol = lookup ? lookup(name) : nullptr;
				if (symbol == nullptr)
					return std::unexpected(std::format("'{}' is not declared", name));
				if (!symbol->isConstant())
					return std::unexpected(std::format("'{}' is not a constant, so it cannot appear in a constant expression", name));
				if (!symbol->hasValue() || !symbol->value().isScalar())
					return std::unexpected(std::format("'{}' is not a scalar constant", name));
				return symbol->value().first();
			}

			case ConstExpr::Kind::Query:
				return evaluateQuery(expression, lookup);

			case ConstExpr::Kind::Binary:
			{
				auto lhs = evaluateConstExpr(expression.left(), lookup, wide);
				if (!lhs.has_value())
					return lhs;

				if (expression.op() == ConstExpr::Op::Negate)
				{
					if (lhs->isF64())
						return LiteralScalar::makeF64(-lhs->asDouble());
					if (lhs->isFloat())
						return LiteralScalar::makeF32(-lhs->value().f32Value);
					if (wide || lhs->isWide())
						return LiteralScalar::makeI64(static_cast<i64>(u64{ 0 } - lhs->rawBits64()));
					return LiteralScalar::makeI32(-static_cast<i32>(lhs->asRawValue()));
				}

				auto rhs = evaluateConstExpr(expression.right(), lookup, wide);
				if (!rhs.has_value())
					return rhs;

				const bool floating = lhs->isFloat() || rhs->isFloat();
				// A 64-bit context, or a 64-bit operand, makes the operation a 64-bit one.
				if (floating && (wide || lhs->isWide() || rhs->isWide()))
					return wideFloatBinary(expression.op(), wideFloat(*lhs), wideFloat(*rhs));
				if (!floating && (wide || lhs->isWide() || rhs->isWide()))
					return wideIntegerBinary(expression.op(), wideInteger(*lhs), wideInteger(*rhs));

				// A float on either side makes the whole operation floating point, so `PI * 2`
				// stays a float instead of being truncated on the way through.
				if (floating)
				{
					const f32 a = lhs->isFloat() ? lhs->value().f32Value : static_cast<f32>(static_cast<i32>(lhs->asRawValue()));
					const f32 b = rhs->isFloat() ? rhs->value().f32Value : static_cast<f32>(static_cast<i32>(rhs->asRawValue()));

					switch (expression.op())
					{
						case ConstExpr::Op::Add: return LiteralScalar::makeF32(a + b);
						case ConstExpr::Op::Subtract: return LiteralScalar::makeF32(a - b);
						case ConstExpr::Op::Multiply: return LiteralScalar::makeF32(a * b);
						case ConstExpr::Op::Divide:
							if (b == 0.0f)
								return std::unexpected("Division by zero in constant expression");
							return LiteralScalar::makeF32(a / b);
						// A comparison is a whole number whichever kind of thing it compared.
						case ConstExpr::Op::Equal: return LiteralScalar::makeI32(a == b ? 1 : 0);
						case ConstExpr::Op::NotEqual: return LiteralScalar::makeI32(a != b ? 1 : 0);
						case ConstExpr::Op::Less: return LiteralScalar::makeI32(a < b ? 1 : 0);
						case ConstExpr::Op::LessEqual: return LiteralScalar::makeI32(a <= b ? 1 : 0);
						case ConstExpr::Op::Greater: return LiteralScalar::makeI32(a > b ? 1 : 0);
						case ConstExpr::Op::GreaterEqual: return LiteralScalar::makeI32(a >= b ? 1 : 0);
						case ConstExpr::Op::Modulo:
							return std::unexpected("'%' has no meaning for floating-point values");
						default: return std::unexpected("Unknown operator in constant expression");
					}
				}

				const i32 a = static_cast<i32>(lhs->asRawValue());
				const i32 b = static_cast<i32>(rhs->asRawValue());

				switch (expression.op())
				{
					case ConstExpr::Op::Add: return LiteralScalar::makeI32(a + b);
					case ConstExpr::Op::Subtract: return LiteralScalar::makeI32(a - b);
					case ConstExpr::Op::Multiply: return LiteralScalar::makeI32(a * b);
					case ConstExpr::Op::Divide:
						if (b == 0)
							return std::unexpected("Division by zero in constant expression");
						return LiteralScalar::makeI32(a / b);
					case ConstExpr::Op::Modulo:
						if (b == 0)
							return std::unexpected("Remainder by zero in constant expression");
						return LiteralScalar::makeI32(a % b);
					case ConstExpr::Op::Equal: return LiteralScalar::makeI32(a == b ? 1 : 0);
					case ConstExpr::Op::NotEqual: return LiteralScalar::makeI32(a != b ? 1 : 0);
					case ConstExpr::Op::Less: return LiteralScalar::makeI32(a < b ? 1 : 0);
					case ConstExpr::Op::LessEqual: return LiteralScalar::makeI32(a <= b ? 1 : 0);
					case ConstExpr::Op::Greater: return LiteralScalar::makeI32(a > b ? 1 : 0);
					case ConstExpr::Op::GreaterEqual: return LiteralScalar::makeI32(a >= b ? 1 : 0);
				default: return std::unexpected("Unknown operator in constant expression");
				}
			}

			default:
				return std::unexpected("Invalid constant expression");
		}
	}
}
