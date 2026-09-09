#pragma once

#include "literal_scalar.h"
#include "identifier.h"
#include <vector>
#include <optional>
#include <string>

namespace ceres::casm
{
	// A constant expression, kept as a tree rather than folded on the spot.
	//
	// The parser used to fold `2 * 4` into 8 as it read it, which worked only because the operands
	// were literals: a constant is not defined until TranslationUnitBuilder walks the statements, so
	// at parse time there is no value for `BASE` to fold against and `BASE + 4` had to be rejected.
	// Keeping the tree moves evaluation to where the symbol table exists.
	//
	// No cycle detection is needed. A constant is evaluated when its own statement is processed, in
	// source order, so it can only refer to constants already defined - `A = B + 1` with B declared
	// below simply does not resolve, exactly as a forward reference does today.
	class ConstExpr
	{
	public:
		enum class Kind : u8
		{
			Invalid = 0,
			Literal,     // 42, 3.5, 'a', true
			Identifier,  // BASE
			Binary,      // lhs op rhs
			Query,       // sizeof(x), countof(x), dimof(x, n)
		};

		enum class Op : u8
		{
			None = 0,
			Add,
			Subtract,
			Multiply,
			Divide,
			Modulo,
			// Comparisons answer 1 or 0. They exist for `assert`, which needs something to be true
			// about rather than merely non-zero.
			Equal,
			NotEqual,
			Less,
			LessEqual,
			Greater,
			GreaterEqual,
			Negate, // Unary; only the left child is used
		};

		// Questions about a declared symbol that only the symbol table can answer. They exist
		// because a multidimensional array whose sizes were inferred leaves those numbers written
		// nowhere in the source - dimof is the only way to get one back.
		enum class Query : u8
		{
			None = 0,
			SizeOf,   // Bytes the symbol occupies
			CountOf,  // Total number of scalar elements
			DimOf,    // Length of one dimension, counted from the outside in
		};

	private:
		Kind _kind = Kind::Invalid;
		Op _op = Op::None;
		Query _query = Query::None;
		LiteralScalar _literal{};
		NullableIdentifier _name = nullptr;
		u32 _dimensionIndex = 0; // Query::DimOf only
		std::vector<ConstExpr> _children; // Binary: left and right, or just left for Negate

	public:
		ConstExpr() noexcept = default;
		ConstExpr(const ConstExpr&) = default;
		ConstExpr(ConstExpr&&) noexcept = default;
		~ConstExpr() = default;

		ConstExpr& operator=(const ConstExpr&) = default;
		ConstExpr& operator=(ConstExpr&&) noexcept = default;

		bool operator==(const ConstExpr&) const = default;

	public:
		Kind kind() const noexcept { return _kind; }
		Op op() const noexcept { return _op; }
		Query query() const noexcept { return _query; }

		bool isValid() const noexcept { return _kind != Kind::Invalid; }
		bool isLiteral() const noexcept { return _kind == Kind::Literal; }
		bool isIdentifier() const noexcept { return _kind == Kind::Identifier; }
		bool isBinary() const noexcept { return _kind == Kind::Binary; }
		bool isQuery() const noexcept { return _kind == Kind::Query; }

		LiteralScalar literal() const noexcept { return _literal; }
		Identifier name() const { return static_cast<Identifier>(_name); }
		u32 dimensionIndex() const noexcept { return _dimensionIndex; }

		const ConstExpr& left() const noexcept { return _children[0]; }
		const ConstExpr& right() const noexcept { return _children[1]; }

		// True when nothing in the tree needs the symbol table, so it can be folded immediately.
		bool isSelfContained() const noexcept
		{
			switch (_kind)
			{
				case Kind::Literal: return true;
				case Kind::Identifier:
				case Kind::Query: return false;
				case Kind::Binary:
					for (const ConstExpr& child : _children)
					{
						if (!child.isSelfContained())
							return false;
					}
					return true;
				default: return false;
			}
		}

	public:
		static ConstExpr makeLiteral(LiteralScalar value) noexcept
		{
			ConstExpr expr;
			expr._kind = Kind::Literal;
			expr._literal = value;
			return expr;
		}

		static ConstExpr makeIdentifier(Identifier name) noexcept
		{
			ConstExpr expr;
			expr._kind = Kind::Identifier;
			expr._name = name;
			return expr;
		}

		static ConstExpr makeBinary(Op op, ConstExpr&& lhs, ConstExpr&& rhs) noexcept
		{
			ConstExpr expr;
			expr._kind = Kind::Binary;
			expr._op = op;
			expr._children.push_back(std::move(lhs));
			expr._children.push_back(std::move(rhs));
			return expr;
		}

		static ConstExpr makeNegate(ConstExpr&& operand) noexcept
		{
			ConstExpr expr;
			expr._kind = Kind::Binary;
			expr._op = Op::Negate;
			expr._children.push_back(std::move(operand));
			return expr;
		}

		static ConstExpr makeQuery(Query query, Identifier name, u32 dimensionIndex = 0) noexcept
		{
			ConstExpr expr;
			expr._kind = Kind::Query;
			expr._query = query;
			expr._name = name;
			expr._dimensionIndex = dimensionIndex;
			return expr;
		}

		static std::optional<Query> queryFromName(std::string_view name) noexcept
		{
			if (name == "sizeof") return Query::SizeOf;
			if (name == "countof") return Query::CountOf;
			if (name == "dimof") return Query::DimOf;
			return std::nullopt;
		}

		std::string toString() const
		{
			switch (_kind)
			{
				case Kind::Literal: return std::to_string(_literal.asRawValue());
				case Kind::Identifier: return std::string(_name.str());
				case Kind::Query:
				{
					std::string_view name =
						_query == Query::SizeOf ? "sizeof" :
						_query == Query::CountOf ? "countof" : "dimof";
					std::string result = std::string(name) + "(" + std::string(_name.str());
					if (_query == Query::DimOf)
						result += ", " + std::to_string(_dimensionIndex);
					return result + ")";
				}
				case Kind::Binary:
				{
					if (_op == Op::Negate)
						return "-(" + _children[0].toString() + ")";
					std::string_view symbol =
						_op == Op::Add ? " + " :
						_op == Op::Subtract ? " - " :
						_op == Op::Multiply ? " * " : " / ";
					return "(" + _children[0].toString() + std::string(symbol) + _children[1].toString() + ")";
				}
				default: return "<invalid>";
			}
		}
	};
}
