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
#include <vector>

namespace ceres::driver
{
	// Host callbacks make the machine presentation-neutral. A terminal host may write these to
	// streams; a GUI may enqueue them for its event loop or upload them to an OpenGL surface.
	struct MachineHost
	{
		std::function<void(std::span<const u8>)> terminalOutput;
		// The terminal's error stream (TerminalDevice::ErrorOutputRegister); empty sends it to the host's stderr.
		std::function<void(std::span<const u8>)> terminalError;
		std::function<void(std::string_view)> framePresented;
	};

	struct MachineConfig
	{
		usize memorySize = vm::Memory::DefaultSize;
		std::filesystem::path diskImage;
		// Media already plugged into the peripheral ports when the machine starts: {port, file, cartridge}.
		struct Port
		{
			unsigned port = 0;
			std::filesystem::path path;
			bool cartridge = false;
		};
		std::vector<Port> ports;
		// What main(argc, argv) and getenv see (vm::ProgramArguments).
		std::vector<std::string> arguments;
		std::vector<std::string> environment;
		// The host directory the program's host files are under (HostFsDevice); empty for none.
		std::filesystem::path hostDirectory;
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

		// Plugs a host file into a peripheral port while the machine runs, or pulls it out again; the program is told
		// by an event and an interrupt. A cartridge is read only and has to exist. False, with `error` saying why,
		// when the port does not exist, is taken, or the file will not open.
		bool attachPeripheral(unsigned port, const std::filesystem::path& path, bool cartridge, std::string* error = nullptr);
		bool detachPeripheral(unsigned port);
		std::string describePeripheral(unsigned port) const;

	private:
		class Impl;
		std::unique_ptr<Impl> _impl;
	};
}
