#pragma once

#include "linker.h"
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

	public:
		BinaryEmitter() = delete;
		BinaryEmitter(const BinaryEmitter&) noexcept = delete;
		BinaryEmitter(BinaryEmitter&&) noexcept = default;
		~BinaryEmitter() noexcept = default;

		BinaryEmitter& operator=(const BinaryEmitter&) noexcept = delete;
		BinaryEmitter& operator=(BinaryEmitter&&) noexcept = default;

	public:
		explicit BinaryEmitter(AssemblyState& state) noexcept :
			_state(state)
		{}

		inline const std::span<const u8> textBuffer() const noexcept { return _textBuffer; }
		inline const std::span<const u8> rodataBuffer() const noexcept { return _rodataBuffer; }
		inline const std::span<const u8> dataBuffer() const noexcept { return _dataBuffer; }
		inline AssemblerErrorHandler& errorHandler() noexcept { return _state.get().errorHandler(); }
		inline const AssemblerErrorHandler& errorHandler() const noexcept { return _state.get().errorHandler(); }

	public:
		std::optional<vm::Program> emit();

	private:
		void emitData(const RelocatableStatement& statement, bool isRodata);
		void emitInstruction(const RelocatableStatement& statement);
		Address lastSectionAddress(SectionType sectionType);

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
