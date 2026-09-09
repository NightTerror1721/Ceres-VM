#pragma once

#include "linker.h"
#include "relocation.h"
#include <ceres/core/format/debug_info.h>
#include <ceres/core/format/program.h>
#include <ceres/core/isa/instructions.h>

namespace ceres::casm
{
	using namespace fmt;
	using namespace isa;

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
		// Set to the file being written out when the output is an object rather than a program.
		// Two things follow: every field that would have held a final address becomes a
		// relocation and is left at zero, because neither this object's sections nor the symbols
		// it takes from elsewhere have an address yet; and only this one unit is emitted, so what
		// it imported stays a declaration instead of becoming a second copy of that file's code.
		std::string_view _objectRootFile;
		std::vector<Relocation> _relocations;
		DebugInfoBuilder _debugBuilder;
		DebugInfo _debugInfo; // Released from the builder at the end of emit()

	public:
		BinaryEmitter() = delete;
		BinaryEmitter(const BinaryEmitter&) noexcept = delete;
		BinaryEmitter(BinaryEmitter&&) noexcept = default;
		~BinaryEmitter() noexcept = default;

		BinaryEmitter& operator=(const BinaryEmitter&) noexcept = delete;
		BinaryEmitter& operator=(BinaryEmitter&&) noexcept = default;

	public:
		explicit BinaryEmitter(AssemblyState& state, bool emitDebugInfo = false, bool requireEntryPoint = true,
			std::string_view objectRootFile = {}) noexcept :
			_state(state),
			_emitDebugInfo(emitDebugInfo),
			_requireEntryPoint(requireEntryPoint),
			_objectRootFile(objectRootFile)
		{}

		inline const std::span<const u8> textBuffer() const noexcept { return _textBuffer; }
		inline const std::span<const u8> rodataBuffer() const noexcept { return _rodataBuffer; }
		inline const std::span<const u8> dataBuffer() const noexcept { return _dataBuffer; }
		inline AssemblerErrorHandler& errorHandler() noexcept { return _state.get().errorHandler(); }
		inline const AssemblerErrorHandler& errorHandler() const noexcept { return _state.get().errorHandler(); }

	public:
		std::optional<Program> emit();

		// Only meaningful after emit() and only when the emitter was asked for it; empty otherwise.
		// Moved out rather than copied: the tables are the largest thing the emitter builds.
		DebugInfo takeDebugInfo() noexcept { return std::move(_debugInfo); }

		// Empty unless the emitter was put in object mode: a program has its addresses written
		// into it and nothing left to relocate.
		std::vector<Relocation> takeRelocations() noexcept { return std::move(_relocations); }

	private:
		void emitData(const RelocatableStatement& statement, bool isRodata);
		void recordDebugSymbols();
		static inline constexpr usize SectionAlignment = 4;

		void padToAlignment(std::vector<u8>& buffer, u32 alignment);
		void padSectionToAlignment(std::vector<u8>& buffer, usize unitStart);
		void emitInstruction(const RelocatableStatement& statement);

		// Turns the address a field was about to receive into a note for the linker. Returns
		// true when it did, and then the field is left at zero: the linker computes the whole
		// value, because a half-built address cannot be finished by adding a base to it - the
		// high half of a two-instruction address depends on a carry out of the low one.
		bool recordRelocation(const Operand& operand, RelocationField field, u8 shift, bool pcRelative);

		// A memory displacement is a signed 16-bit field. It used to be written with a plain
		// truncation, so `[r1 + 70000]` quietly became `[r1 + 4464]` and `[r1 - 8]` became
		// `[r1 + 65528]` - the second of which is why the field is signed now.
		bool checkDisplacement(const RelocatableStatement& statement, u32 value);
		Address lastSectionAddress(SectionType sectionType);

		// Called once per machine word actually written to .text, so a pseudo-instruction that
		// expands to three words contributes three entries and the padding NOPs are marked as
		// such. `flags` carries everything except FirstOfLine, which only the builder can know.
		void recordFrames();
		void recordDebugLine(const RelocatableStatement& statement, Address address, u16 flags);

	private:
		template <typename T>
		static void writeToBuffer(std::vector<u8>& buffer, const T& value)
		{
			if constexpr (SameAs<T, Instruction>)
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
