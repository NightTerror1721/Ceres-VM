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
	// No cycle detection: a constant is evaluated when its own statement is processed, in source
	// order, so it can only ever refer to constants already defined.
	std::expected<LiteralScalar, std::string> evaluateConstExpr(const ConstExpr& expression, const ConstExprSymbolLookup& lookup);
}
