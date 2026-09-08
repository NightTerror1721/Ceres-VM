#pragma once

#include "translation_unit.h"
#include "assembly_state.h"
#include <optional>
#include <format>

namespace ceres::casm
{
	class Linker
	{
	private:
		struct MemoryOffsets
		{
			Address textOffset;
			Address rodataOffset;
			Address dataOffset;
			Address bssOffset;
		};

	private:
		Ref<AssemblyState> _state;
		bool _linked = false;
		// Refreshed while iterating a unit's statements or symbols in link(), so error()/
		// reportError() below attribute to the right file without threading it through every call.
		std::string_view _currentFile;

	public:
		Linker() = delete;
		Linker(const Linker&) noexcept = delete;
		Linker(Linker&&) noexcept = default;
		~Linker() noexcept = default;

		Linker& operator=(const Linker&) noexcept = delete;
		Linker& operator=(Linker&&) noexcept = default;

	public:
		explicit Linker(AssemblyState& state) noexcept : _state(state) {}

		bool link();

	public:
		bool isLinked() const noexcept { return _linked; }

	private:
		// Sections carry whole instructions and 32-bit variables, so each one starts on a 4-byte
		// boundary. Aligning variables inside a section is not enough if the section itself is
		// misaligned.
		static inline constexpr u32 SectionAlignment = 4;

		static constexpr u32 alignUp(u32 value) noexcept
		{
			const u32 remainder = value % SectionAlignment;
			return remainder == 0 ? value : value + (SectionAlignment - remainder);
		}

		void calculateMemoryMap() const;
		void defineLinkerSymbols();
		MemoryOffsets calculateMemoryOffsets() const;

	private:
		[[noreturn]] void error(u32 line, std::string_view message) const
		{
			throw AssemblerError(_currentFile, line, 1, message);
		}

		template <typename... Args>
		[[noreturn]] void error(u32 line, std::string_view formatStr, Args&&... args) const
		{
			std::string message = std::vformat(formatStr, std::make_format_args(args...));
			throw AssemblerError(_currentFile, line, 1, message);
		}

		void reportError(u32 line, std::string_view message) noexcept
		{
			_state.get().errorHandler().reportError(_currentFile, line, 1, message);
		}

		template <typename... Args>
		void reportError(u32 line, std::string_view formatStr, Args&&... args) noexcept
		{
			std::string message = std::vformat(formatStr, std::make_format_args(args...));
			_state.get().errorHandler().reportError(_currentFile, line, 1, message);
		}
	};
}
