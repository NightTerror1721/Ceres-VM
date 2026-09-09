#pragma once

#include "config.h"
#include <cstdio>
#include <cstdlib>
#include <format>

namespace ceres::detail
{
	[[noreturn]] inline void assert_failed(const char* expression, const char* file, int line, const char* function, const char* message) noexcept
	{
		if (message)
		{
			std::fprintf(stderr, "[CERES ASSERTION FAILED][%s:%d][%s] %s (expr: %s)\n",
				file, line, function, message, expression);

		}
		else
		{
			std::fprintf(stderr, "[CERES ASSERTION FAILED][%s:%d][%s] (expr: %s)\n",
				file, line, function, expression);
		}

		std::fflush(stderr);
		std::abort();
	}

	[[noreturn]] inline void assert_failed(const char* expression, const char* file, int line, const char* function) noexcept
	{
		assert_failed(expression, file, line, function, nullptr);
	}

	template <typename... Args>
	[[noreturn]] inline void assert_failed(const char* expression, const char* file, int line, const char* function, const char* format, Args&&... args) noexcept
	{
		try
		{
            std::string message = std::vformat(format, std::make_format_args(args...));
			assert_failed(expression, file, line, function, message.c_str());
		}
		catch (const std::format_error&)
		{
			std::fputs("[ASSERT FORMAT ERROR]", stderr);
			assert_failed(expression, file, line, function, nullptr);
		}
	}
}

#if CERES_ENABLE_ASSERTS
	#define ceres_assert(expr, ...) do { \
		if (!(expr)) { \
			ceres::detail::assert_failed(#expr, __FILE__, __LINE__, __func__, ##__VA_ARGS__); \
		} \
	} while (0)
#else
#	define ceres_assert(expr, ...) ((void)0)
#endif
