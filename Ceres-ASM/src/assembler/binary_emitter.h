#pragma once

#include "linker.h"
#include "debug/debug_info.h"
#include "vm/program.h"
#include "vm/instructions.h"

namespace ceres::casm
{
	class BinaryEmitter
	{
	private:
		Ref<AssemblyState> _state;
		std::vector<u8> _textBuffer;
		std::vector<u8> _rodataBuffer;
		std::vector<u8> _dataBuffer;
		// Refreshed at the top of each statement processed in emit()'s main loop, so error()/
		// reportError() below attribute to the right file without threading it through every call.
		std::string_view _currentFile;

		// Off by default: every instruction emitted adds a line entry, and nothing but a debugger
		// or an annotated listing has any use for them.
		bool _emitDebugInfo = false;
		// Cleared when the caller only wants the file checked, not a runnable program built. See
		// AssemblerOptions::requireEntryPoint.
		bool _requireEntryPoint = true;
		debug::DebugInfoBuilder _debugBuilder;
		debug::DebugInfo _debugInfo; // Released from the builder at the end of emit()

	public:
		BinaryEmitter() = delete;
		BinaryEmitter(const BinaryEmitter&) noexcept = delete;
		BinaryEmitter(BinaryEmitter&&) noexcept = default;
		~BinaryEmitter() noexcept = default;

		BinaryEmitter& operator=(const BinaryEmitter&) noexcept = delete;
		BinaryEmitter& operator=(BinaryEmitter&&) noexcept = default;

	public:
		explicit BinaryEmitter(AssemblyState& state, bool emitDebugInfo = false, bool requireEntryPoint = true) noexcept :
			_state(state),
			_emitDebugInfo(emitDebugInfo),
			_requireEntryPoint(requireEntryPoint)
		{}

		inline const std::span<const u8> textBuffer() const noexcept { return _textBuffer; }
		inline const std::span<const u8> rodataBuffer() const noexcept { return _rodataBuffer; }
		inline const std::span<const u8> dataBuffer() const noexcept { return _dataBuffer; }
		inline AssemblerErrorHandler& errorHandler() noexcept { return _state.get().errorHandler(); }
		inline const AssemblerErrorHandler& errorHandler() const noexcept { return _state.get().errorHandler(); }

	public:
		std::optional<vm::Program> emit();

		// Only meaningful after emit() and only when the emitter was asked for it; empty otherwise.
		// Moved out rather than copied: the tables are the largest thing the emitter builds.
		debug::DebugInfo takeDebugInfo() noexcept { return std::move(_debugInfo); }

	private:
		void emitData(const RelocatableStatement& statement, bool isRodata);
		void recordDebugSymbols();
		static inline constexpr usize SectionAlignment = 4;

		void padToAlignment(std::vector<u8>& buffer, u32 alignment);
		void padSectionToAlignment(std::vector<u8>& buffer, usize unitStart);
		void emitInstruction(const RelocatableStatement& statement);
		Address lastSectionAddress(SectionType sectionType);

		// Called once per machine word actually written to .text, so a pseudo-instruction that
		// expands to three words contributes three entries and the padding NOPs are marked as
		// such. `flags` carries everything except FirstOfLine, which only the builder can know.
		void recordDebugLine(const RelocatableStatement& statement, Address address, u16 flags);

	private:
		template <typename T>
		static void writeToBuffer(std::vector<u8>& buffer, const T& value)
		{
			if constexpr (SameAs<T, vm::Instruction>)
			{
				auto raw = value.asBytes();
				buffer.insert(buffer.end(), raw.begin(), raw.end());
			}
			else if constexpr (SameAs<T, std::string>)
			{
				buffer.insert(buffer.end(), value.begin(), value.end());
				buffer.push_back('\0'); // Null-terminate the string)
			}
			else if constexpr (Arithmetic<T> && sizeof(T) <= 4)
			{
				// For arithmetic types (integers and floats) of size <= 4 bytes, we can directly copy the bytes
				const u8* bytes = reinterpret_cast<const u8*>(&value);
				buffer.insert(buffer.end(), bytes, bytes + sizeof(T));
			}
			else
			{
				static_assert(false, "Unsupported type for writeToBuffer");
			}
		}

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
