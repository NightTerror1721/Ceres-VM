#pragma once

#include "linker.h"
#include "vm/program.h"
#include "vm/instructions.h"

namespace ceres::casm
{
	class BinaryEmitter
	{
	private:
		LinkedExecutable _linkedExecutable;
		std::vector<u8> _textBuffer;
		std::vector<u8> _rodataBuffer;
		std::vector<u8> _dataBuffer;
		AssemblerErrorHandler& _errorHandler;

	public:
		BinaryEmitter() = delete;
		BinaryEmitter(const BinaryEmitter&) noexcept = delete;
		BinaryEmitter(BinaryEmitter&&) noexcept = default;
		~BinaryEmitter() noexcept = default;

		BinaryEmitter& operator=(const BinaryEmitter&) noexcept = delete;
		BinaryEmitter& operator=(BinaryEmitter&&) noexcept = default;

	public:
		explicit BinaryEmitter(LinkedExecutable&& linkedExecutable, AssemblerErrorHandler& errorHandler) noexcept :
			_linkedExecutable(std::move(linkedExecutable)), _errorHandler(errorHandler)
		{}

		inline const LinkedExecutable& linkedExecutable() const noexcept { return _linkedExecutable; }
		inline const std::span<const u8> textBuffer() const noexcept { return _textBuffer; }
		inline const std::span<const u8> rodataBuffer() const noexcept { return _rodataBuffer; }
		inline const std::span<const u8> dataBuffer() const noexcept { return _dataBuffer; }
		inline AssemblerErrorHandler& errorHandler() noexcept { return _errorHandler; }
		inline const AssemblerErrorHandler& errorHandler() const noexcept { return _errorHandler; }

	public:
		std::optional<vm::Program> emit();

	private:
		void emitData(const RelocatableStatement& statement, bool isRodata);
		void emitInstruction(const RelocatableStatement& statement);

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
			_errorHandler.reportError(line, 1, message);
		}

		template <typename... Args>
		void reportError(u32 line, std::string_view formatStr, Args&&... args) noexcept
		{
			std::string message = std::vformat(formatStr, std::make_format_args(args...));
			_errorHandler.reportError(line, 1, message);
		}
	};
}
