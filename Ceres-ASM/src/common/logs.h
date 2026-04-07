#pragma once

#include "config.h"
#include <string_view>

#if CERES_ENABLE_LOGGING
#include <chrono>
#include <format>
#endif

namespace ceres
{
	enum class LogLevel
	{
		Debug,
		Info,
		Warning,
		Error,
		Critical
	};

	forceinline constexpr const char* to_string(LogLevel level) noexcept
	{
		switch (level)
		{
			case LogLevel::Debug: return "DEBUG";
			case LogLevel::Info: return "INFO";
			case LogLevel::Warning: return "WARNING";
			case LogLevel::Error: return "ERROR";
			case LogLevel::Critical: return "CRITICAL";
			default: return "UNKNOWN";
		}
	}

	namespace detail
	{
		forceinline void log_time()
		{
#if CERES_ENABLE_LOGGING
			using namespace std::chrono;
			auto now = std::chrono::system_clock::now();
			std::time_t t = std::chrono::system_clock::to_time_t(now);
			std::tm tm{};
#if defined(_MSC_VER) || defined(__MINGW32__)
			localtime_s(&tm, &t);
#else
			localtime_r(&t, &tm);
#endif
			std::fprintf(stderr, "[%02d:%02d:%02d]",
				static_cast<int>(tm.tm_hour),
				static_cast<int>(tm.tm_min),
				static_cast<int>(tm.tm_sec)
			);
#else
			(void)0;
#endif
		}

#if CERES_ENABLE_LOGGING
		template <typename... Args>
		inline void log_impl(LogLevel level, std::string_view fmt, Args&&... args)
		{
			detail::log_time();
			std::fprintf(stderr, "[%s] ", to_string(level));
			try
			{
				auto msg = std::vformat(fmt, std::make_format_args(std::forward<Args>(args)...));
				std::fputs(msg.c_str(), stderr);
			}
			catch (const std::format_error&)
			{
				std::fputs("[LOG FORMAT ERROR]", stderr);
			}
			std::fflush(stderr);
		}
#else
		template <typename... Args>
		forceinline void log_impl(LogLevel, std::string_view, Args&&...)
		{
			(void)0;
		}
#endif
	}
}

#if CERES_ENABLE_LOGGING
#	define ceres_debug(fmt, ...)	ceres::detail::log_impl(ceres::LogLevel::Debug, (fmt), ##__VA_ARGS__)
#	define ceres_info(fmt, ...)		ceres::detail::log_impl(ceres::LogLevel::Info, (fmt), ##__VA_ARGS__)
#	define ceres_warning(fmt, ...)	ceres::detail::log_impl(ceres::LogLevel::Warning, (fmt), ##__VA_ARGS__)
#	define ceres_error(fmt, ...)	ceres::detail::log_impl(ceres::LogLevel::Error, (fmt), ##__VA_ARGS__)
#	define ceres_critical(fmt, ...) ceres::detail::log_impl(ceres::LogLevel::Critical, (fmt), ##__VA_ARGS__)
#else
#	define ceres_debug(fmt, ...)	do { (void)sizeof(fmt); } while(0)
#	define ceres_info(fmt, ...)		do { (void)sizeof(fmt); } while(0)
#	define ceres_warning(fmt, ...)	do { (void)sizeof(fmt); } while(0)
#	define ceres_error(fmt, ...)	do { (void)sizeof(fmt); } while(0)
#	define ceres_critical(fmt, ...)	do { (void)sizeof(fmt); } while(0)
#endif
