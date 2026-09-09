#pragma once

#include <ceres/asm/statement.h>
#include <ceres/asm/macro_table.h>

namespace ceres::casm
{
	enum class ScopeType
	{
		Default = 0, // Default scope (no special handling)
		Macro		 // Macro scope (used for macro expansion)
	};

	struct MacroScopeInfo
	{
		ConstRef<Macro> macro; // Reference to the macro being expanded
	};

	class Scope
	{
	public:
		using InfoVariant = std::variant<std::monostate, MacroScopeInfo>;

	private:
		ScopeType _type;
		u32 _line; // Line number in the source code where the scope was created
		InfoVariant _info;

	public:
		Scope() = delete;
		Scope(const Scope&) noexcept = default;
		Scope(Scope&&) noexcept = default;
		~Scope() noexcept = default;

		Scope& operator=(const Scope&) noexcept = default;
		Scope& operator=(Scope&&) noexcept = default;

	private:
		explicit Scope(ScopeType type, u32 line, InfoVariant&& info) noexcept :
			_type(type), _line(line), _info(std::move(info))
		{}

	public:
		inline ScopeType type() const noexcept { return _type; }
		inline u32 line() const noexcept { return _line; }

		inline bool isDefaultScope() const noexcept { return _type == ScopeType::Default; }
		inline bool isMacroScope() const noexcept { return _type == ScopeType::Macro; }

		inline const MacroScopeInfo& macroInfo() const { return std::get<MacroScopeInfo>(_info); }

	public:
		static Scope makeDefault(u32 line) noexcept
		{
			return Scope(ScopeType::Default, line, std::monostate{});
		}

		static Scope makeMacro(u32 line, const Macro& macro) noexcept
		{
			return Scope(ScopeType::Macro, line, MacroScopeInfo{ macro });
		}
	};
}
