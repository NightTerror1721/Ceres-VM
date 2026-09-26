#pragma once

#ifdef CERES_DEBUG
#	ifndef CERES_ENABLE_LOGGING
#		define CERES_ENABLE_LOGGING 1
#	else
#		define CERES_ENABLE_LOGGING 0
#	endif
#	ifndef CERES_ENABLE_ASSERTS
#		define CERES_ENABLE_ASSERTS 1
#	else
#		define CERES_ENABLE_ASSERTS 0
#	endif
#endif

#ifndef forceinline
#	ifdef _MSC_VER
#		define forceinline __forceinline
#	elif defined(__GNUC__) || defined(__clang__)
#		define forceinline __attribute__((always_inline)) inline
#	else
#		define forceinline inline
#	endif
#endif

// The opposite, for a cold path that must stay out of the hot function it is called from.
#ifndef neverinline
#	ifdef _MSC_VER
#		define neverinline __declspec(noinline)
#	elif defined(__GNUC__) || defined(__clang__)
#		define neverinline __attribute__((noinline))
#	else
#		define neverinline
#	endif
#endif