#pragma once

#include "common/types.h"
#include <span>
#include <stdexcept>
#include <vector>

namespace ceres::casm
{
	class AssemblerErrorHandler;

	class AssemblerError : public std::runtime_error
	{
	protected:
		// A view, not a copy: thrown/caught within one assemble() call, so it never outlives the
		// stable, interned path storage it points into (see AssemblyState::internedPath). Empty
		// if this error isn't tied to a specific file.
		std::string_view _file;
		u32 _line;
		u32 _column;

	public:
		AssemblerError(std::string_view file, u32 line, u32 column, std::string_view message) noexcept :
			std::runtime_error(std::string(message)), _file(file), _line(line), _column(column)
		{}

		constexpr std::string_view file() const noexcept { return _file; }
		constexpr u32 line() const noexcept { return _line; }
		constexpr u32 column() const noexcept { return _column; }

		friend class AssemblerErrorHandler;
	};

	// Not everything worth saying stops the build. A private declaration nobody uses is certainly
	// dead - it cannot be reached from outside the unit that declares it - but it is not an error.
	enum class DiagnosticSeverity : u8
	{
		Error = 0,
		Warning,
	};

	struct AssemblerErrorEntry
	{
		std::string file;
		u32 line;
		u32 column;
		std::string message;
		DiagnosticSeverity severity = DiagnosticSeverity::Error;

		bool isWarning() const noexcept { return severity == DiagnosticSeverity::Warning; }
	};

	class AssemblerErrorHandler
	{
	public:
		using iterator = std::vector<AssemblerErrorEntry>::const_iterator;

	private:
		std::vector<AssemblerErrorEntry> _errors;
		usize _errorCount = 0; // Warnings live in the same list; only these fail the build.

	public:
		AssemblerErrorHandler() noexcept = default;
		AssemblerErrorHandler(const AssemblerErrorHandler&) noexcept = delete;
		AssemblerErrorHandler(AssemblerErrorHandler&&) noexcept = default;
		~AssemblerErrorHandler() noexcept = default;

		AssemblerErrorHandler& operator=(const AssemblerErrorHandler&) noexcept = delete;
		AssemblerErrorHandler& operator=(AssemblerErrorHandler&&) noexcept = default;

	public:
		constexpr bool hasErrors() const noexcept { return _errorCount > 0; }
		constexpr bool hasDiagnostics() const noexcept { return !_errors.empty(); }

		constexpr std::span<const AssemblerErrorEntry> errors() const noexcept { return std::span<const AssemblerErrorEntry>(_errors); }

		void reportError(std::string_view file, u32 line, u32 column, std::string_view message) noexcept
		{
			_errors.push_back({ std::string(file), line, column, std::string(message), DiagnosticSeverity::Error });
			++_errorCount;
		}

		void reportWarning(std::string_view file, u32 line, u32 column, std::string_view message) noexcept
		{
			_errors.push_back({ std::string(file), line, column, std::string(message), DiagnosticSeverity::Warning });
		}

		void reportError(const AssemblerError& error) noexcept
		{
			// The one deliberate copy: AssemblerErrorEntry is the external-facing result, meant to
			// be read after assemble() has returned, so unlike everything upstream of it, it owns
			// its strings rather than viewing into state that may by then be gone.
			_errors.push_back(AssemblerErrorEntry{ std::string(error._file), error._line, error._column, error.what(), DiagnosticSeverity::Error });
			++_errorCount;
		}

		void clearErrors() noexcept
		{
			_errors.clear();
			_errorCount = 0;
		}

	public:
		iterator begin() const noexcept { return _errors.begin(); }
		iterator end() const noexcept { return _errors.end(); }
	};
}
