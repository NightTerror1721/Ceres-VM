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