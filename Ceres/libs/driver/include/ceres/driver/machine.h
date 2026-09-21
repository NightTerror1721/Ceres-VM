#pragma once

#include <ceres/core/base/types.h>
#include <ceres/core/format/program.h>
#include <ceres/vm/memory.h>

#include <expected>
#include <filesystem>
#include <functional>
#include <memory>
#include <span>
#include <string>
#include <string_view>

namespace ceres::driver
{
	// Host callbacks make the machine presentation-neutral. A terminal host may write these to
	// streams; a GUI may enqueue them for its event loop or upload them to an OpenGL surface.
	struct MachineHost
	{
		std::function<void(std::span<const u8>)> terminalOutput;
		std::function<void(std::string_view)> framePresented;
	};

	struct MachineConfig
	{
		usize memorySize = vm::Memory::DefaultSize;
		std::filesystem::path diskImage;
	};

	class Machine
	{
	public:
		explicit Machine(MachineConfig config, MachineHost host = {});
		Machine(const Machine&) = delete;
		Machine(Machine&&) = delete;
		~Machine();

		Machine& operator=(const Machine&) = delete;
		Machine& operator=(Machine&&) = delete;

		std::expected<void, std::string> load(const fmt::Program& program);
		std::expected<void, std::string> run();
		void pushInput(std::span<const u8> bytes);
		void pushInput(std::string_view text);
		// The host has nothing more to send on the terminal: a program reading it sees end of input
		// once it has taken what was buffered.
		void closeInput();
		// The status the program shut the machine down with (0 unless it asked for another).
		int exitCode() const noexcept;
		void pushKey(u32 code, bool pressed = true);
		void pushText(std::string_view utf8);
		void pushMouse(i32 dx, i32 dy, u8 buttons = 0, i8 wheel = 0);
		u64 droppedInputBytes() const noexcept;

	private:
		class Impl;
		std::unique_ptr<Impl> _impl;
	};
}
