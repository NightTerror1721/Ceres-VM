#include "const_expr_eval.h"
#include "symbol_table.h"
#include <format>

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

	std::expected<LiteralScalar, std::string> evaluateConstExpr(const ConstExpr& expression, const ConstExprSymbolLookup& lookup)
	{
		switch (expression.kind())
		{
			case ConstExpr::Kind::Literal:
				return expression.literal();

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
				auto lhs = evaluateConstExpr(expression.left(), lookup);
				if (!lhs.has_value())
					return lhs;

				if (expression.op() == ConstExpr::Op::Negate)
				{
					if (lhs->isFloat())
						return LiteralScalar::makeF32(-lhs->value().f32Value);
					return LiteralScalar::makeI32(-static_cast<i32>(lhs->asRawValue()));
				}

				auto rhs = evaluateConstExpr(expression.right(), lookup);
				if (!rhs.has_value())
					return rhs;

				// A float on either side makes the whole operation floating point, so `PI * 2`
				// stays a float instead of being truncated on the way through.
				if (lhs->isFloat() || rhs->isFloat())
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
					default: return std::unexpected("Unknown operator in constant expression");
				}
			}

			default:
				return std::unexpected("Invalid constant expression");
		}
	}
}
