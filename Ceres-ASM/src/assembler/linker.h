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
		void calculateMemoryMap() const;
		MemoryOffsets calculateMemoryOffsets() const;

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

		void reportError(u32 line, std::string_view message) noexcept
		{
			_state.get().errorHandler().reportError(line, 1, message);
		}

		template <typename... Args>
		void reportError(u32 line, std::string_view formatStr, Args&&... args) noexcept
		{
			std::string message = std::vformat(formatStr, std::make_format_args(args...));
			_state.get().errorHandler().reportError(line, 1, message);
		}
	};
}
