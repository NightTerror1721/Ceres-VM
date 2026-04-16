#pragma once

#include "common/types.h"
#include <stdexcept>
#include <vector>

namespace ceres::casm
{
	class AssemblerErrorHandler;

	class AssemblerError : public std::runtime_error
	{
	protected:
		u32 _line;
		u32 _column;

	public:
		AssemblerError(u32 line, u32 column, std::string_view message) noexcept :
			std::runtime_error(std::string(message)), _line(line), _column(column)
		{}

		constexpr u32 line() const noexcept { return _line; }
		constexpr u32 column() const noexcept { return _column; }

		friend class AssemblerErrorHandler;
	};

	struct AssemblerErrorEntry
	{
		u32 line;
		u32 column;
		std::string message;
	};

	class AssemblerErrorHandler
	{
	public:
		using iterator = std::vector<AssemblerErrorEntry>::const_iterator;

	private:
		std::vector<AssemblerErrorEntry> _errors;

	public:
		AssemblerErrorHandler() noexcept = default;
		AssemblerErrorHandler(const AssemblerErrorHandler&) noexcept = delete;
		AssemblerErrorHandler(AssemblerErrorHandler&&) noexcept = default;
		~AssemblerErrorHandler() noexcept = default;

		AssemblerErrorHandler& operator=(const AssemblerErrorHandler&) noexcept = delete;
		AssemblerErrorHandler& operator=(AssemblerErrorHandler&&) noexcept = default;

	public:
		constexpr bool hasErrors() const noexcept { return !_errors.empty(); }

		void reportError(u32 line, u32 column, std::string_view message) noexcept
		{
			_errors.push_back({ line, column, std::string(message) });
		}

		void reportError(const AssemblerError& error) noexcept
		{
			_errors.push_back(AssemblerErrorEntry{ error._line, error._column, error.what() });
		}

	public:
		iterator begin() const noexcept { return _errors.begin(); }
		iterator end() const noexcept { return _errors.end(); }
	};
}
