#pragma once

#include "common_defs.h"
#include "statement.h"
#include "errors.h"
#include <string>
#include <unordered_map>
#include <vector>
#include <compare>
#include <span>
#include <format>

namespace ceres::casm
{
	class TranslationUnit;

	struct MacroSignature
	{
		std::string name; // Name of the macro
		u32 parameterCount; // Number of parameters the macro takes

		constexpr explicit MacroSignature(std::string&& name, u32 parameterCount) noexcept :
			name(std::move(name)), parameterCount(parameterCount)
		{}
		constexpr explicit MacroSignature(std::string_view name, u32 parameterCount) noexcept :
			name(name), parameterCount(parameterCount)
		{}
		constexpr MacroSignature(const MacroSignature&) noexcept = default;
		constexpr MacroSignature(MacroSignature&&) noexcept = default;
		constexpr ~MacroSignature() noexcept = default;

		constexpr MacroSignature& operator=(const MacroSignature&) noexcept = default;
		constexpr MacroSignature& operator=(MacroSignature&&) noexcept = default;

		constexpr bool operator==(const MacroSignature&) const noexcept = default;
		constexpr auto operator<=>(const MacroSignature&) const noexcept = default;

		static constexpr MacroSignature make(std::string&& name, u32 parameterCount) noexcept
		{
			return MacroSignature(std::move(name), parameterCount);
		}
		static constexpr MacroSignature make(std::string_view name, u32 parameterCount) noexcept
		{
			return MacroSignature(name, parameterCount);
		}
	};
}

template <>
struct std::hash<ceres::casm::MacroSignature>
{
	static inline std::size_t operator()(const ceres::casm::MacroSignature& signature) noexcept
	{
		std::hash<std::string> h;
		std::hash<ceres::u32> h2;
		return h(signature.name) ^ (h2(signature.parameterCount) << 1);
	}
};

namespace ceres::casm
{
	class Macro
	{
	public:
		using Signature = MacroSignature;

	private:
		Signature _signature;
		std::unordered_map<std::string, u32> _parameterIndices; // Map from parameter name to its index
		std::vector<Statement> _body;

	public:
		Macro() = delete;
		Macro(const Macro&) = default;
		Macro(Macro&&) = default;
		~Macro() = default;

		Macro& operator=(const Macro&) = default;
		Macro& operator=(Macro&&) = default;

	private:
		inline explicit Macro(Signature&& signature, std::vector<std::string>&& parameters, std::vector<Statement>&& body) noexcept :
			_signature(std::move(signature)), _body(std::move(body))
		{
			_parameterIndices.reserve(parameters.size());
			for (u32 i = 0; i < parameters.size(); ++i)
				_parameterIndices.emplace(std::move(parameters[i]), i);
		}

	public:
		inline const Signature& signature() const noexcept { return _signature; }
		inline std::string_view name() const noexcept { return _signature.name; }
		inline u32 parameterCount() const noexcept { return _signature.parameterCount; }
		inline std::span<const Statement> body() const noexcept { return _body; }

		inline std::optional<u32> parameterIndex(const std::string& name) const noexcept
		{
			if (const auto it = _parameterIndices.find(name); it != _parameterIndices.end())
				return it->second;
			return std::nullopt;
		}

	public:
		static Macro make(Signature&& signature, std::vector<std::string>&& parameters, std::vector<Statement>&& body) noexcept
		{
			return Macro(std::move(signature), std::move(parameters), std::move(body));
		}

		static Macro make(std::string&& name, std::vector<std::string>&& parameters, std::vector<Statement>&& body) noexcept
		{
			return Macro(MacroSignature::make(std::move(name), static_cast<u32>(parameters.size())), std::move(parameters), std::move(body));
		}
	};

	class MacroTable
	{
	private:
		std::unordered_map<MacroSignature, Macro> _macros;

	public:
		MacroTable() = default;
		MacroTable(const MacroTable&) = delete;
		MacroTable(MacroTable&&) = default;
		~MacroTable() = default;

		MacroTable& operator=(const MacroTable&) = delete;
		MacroTable& operator=(MacroTable&&) = default;

	public:
		void defineMacro(MacroSignature&& signature, std::vector<std::string>&& parameters, std::vector<Statement>&& body);

		OptionalConstRef<Macro> getMacro(const MacroSignature& signature) const noexcept;

		void importMacros(const TranslationUnit& translationUnit);

	public:
		void defineMacro(std::string&& name, std::vector<std::string>&& parameters, std::vector<Statement>&& body)
		{
			defineMacro(MacroSignature::make(std::move(name), static_cast<u32>(parameters.size())), std::move(parameters), std::move(body));
		}
		void defineMacro(std::string_view name, std::vector<std::string>&& parameters, std::vector<Statement>&& body)
		{
			defineMacro(MacroSignature::make(name, static_cast<u32>(parameters.size())), std::move(parameters), std::move(body));
		}

		// Overload taking the interned form used by the AST. Parameter names are copied out of
		// the pool because Macro indexes them in a std::unordered_map<std::string, u32>.
		void defineMacro(Identifier name, std::vector<Identifier>&& parameters, std::vector<Statement>&& body)
		{
			std::vector<std::string> parameterNames;
			parameterNames.reserve(parameters.size());
			for (Identifier parameter : parameters)
				parameterNames.emplace_back(parameter.view());

			defineMacro(MacroSignature::make(name.view(), static_cast<u32>(parameterNames.size())), std::move(parameterNames), std::move(body));
		}

	private:
		void checkRedefinition(u32 line, const MacroSignature& signature) const;

	private:
		[[noreturn]] void error(u32 line, std::string_view message) const
		{
			throw AssemblerError(line, 1, message);
		}

		template <typename... Args>
		[[noreturn]] void error(u32 line, std::string_view formatStr, Args&&... args) const
		{
			std::string message = std::vformat(formatStr, std::make_format_args(args...));
			throw AssemblerError(line, 1, message);
		}
	};
}
