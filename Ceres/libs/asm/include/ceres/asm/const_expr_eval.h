#pragma once

#include "const_expr.h"
#include "literal_scalar.h"
#include <expected>
#include <functional>
#include <string>
#include <string_view>

namespace ceres::casm
{
	class Symbol;

	// How the evaluator finds a name. Both call sites already have somewhere to look - the
	// translation unit's resolution chain while a declaration is being built, and the symbol table
	// while an operand is being resolved - so the lookup is passed in rather than assumed.
	using ConstExprSymbolLookup = std::function<const Symbol*(std::string_view)>;

	// Folds an expression to a single scalar. Integer arithmetic is done signed, which is what the
	// parser used to do when it folded literals on the spot.
	//
	// `wide` is a 64-bit context - an initializer of a u64, i64 or f64 (plan/v2 SPEC 6): integers are worked in 64 bits
	// and floats in double precision. Elsewhere they are worked in 32 bits and single precision, as they always were,
	// unless an operand is itself 64 bits wide (a literal past 32 bits, a typed 64-bit constant); a float literal is
	// read to double precision and narrowed where it is used.
	//
	// No cycle detection: a constant is evaluated when its own statement is processed, in source
	// order, so it can only ever refer to constants already defined.
	std::expected<LiteralScalar, std::string> evaluateConstExpr(const ConstExpr& expression, const ConstExprSymbolLookup& lookup, bool wide = false);
}
