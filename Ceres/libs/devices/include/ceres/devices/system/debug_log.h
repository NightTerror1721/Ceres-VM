#pragma once

// The debug log (plan/v2 SPEC 5.7): a program's messages for whoever runs it, not for its user. They go to the
// host's log - stderr, or the file of `--log` - as `[ceres:<level>] <line>`, never to the program's terminal.

#include <ceres/vm/mmio_bus.h>
#include <functional>
#include <string>
#include <string_view>

namespace ceres::devices
{
	using namespace vm;

	class DebugLogDevice : public IODevice
	{
	public:
		// A finished line, without its newline, and the level it was written at (LevelError ... LevelDebug).
		using LineSink = std::function<void(u32 level, std::string_view line)>;
		// The program asked to stop here (BreakRegister); only a debugger installs one.
		using BreakHandler = std::function<void()>;

		// Write: the low byte is one character of the line; '\n' ends it and sends it to the host.
		static inline constexpr Address OutputRegister = Address(0x00);
		// Read/write: the level the next line is written at, 0 to 3; a larger value is taken as 3.
		static inline constexpr Address LevelRegister = Address(0x04);
		// Write: sends the line written so far, if there is one, as if it had ended.
		static inline constexpr Address FlushRegister = Address(0x08);
		// Write: under `ceres debug`, stops the program as a breakpoint would; anywhere else, nothing.
		static inline constexpr Address BreakRegister = Address(0x0C);
		// Read: 1 when the host collects the log, 0 when a line would go nowhere.
		static inline constexpr Address EnabledRegister = Address(0x10);

		static inline constexpr u32 LevelError = 0;
		static inline constexpr u32 LevelWarning = 1;
		static inline constexpr u32 LevelInfo = 2;
		static inline constexpr u32 LevelDebug = 3;

		// A line longer than this is sent in pieces of this size, so a program that never writes a newline
		// cannot make the host hold on to an unbounded one.
		static inline constexpr usize MaxLine = 4096;

	private:
		std::string _line;
		u32 _level = LevelInfo;
		LineSink _sink;
		BreakHandler _break;

	public:
		DebugLogDevice() = default;
		DebugLogDevice(const DebugLogDevice&) = delete;
		DebugLogDevice(DebugLogDevice&&) = delete;
		~DebugLogDevice() override = default;

		DebugLogDevice& operator=(const DebugLogDevice&) = delete;
		DebugLogDevice& operator=(DebugLogDevice&&) = delete;

	public:
		void attachTo(MmioBus& bus) { bus.attach(default_mmio::DebugLog, *this); }
		void detachFrom(MmioBus& bus) { bus.detach(default_mmio::DebugLog); }

		void setSink(LineSink sink) { _sink = std::move(sink); }
		void setBreakHandler(BreakHandler handler) { _break = std::move(handler); }

		// The name a level goes by in the host's log: "error", "warn", "info" or "debug".
		static std::string_view levelName(u32 level) noexcept;

		// A reset drops the unfinished line and goes back to the info level.
		void reset() override;

	public:
		u32 read(Address offset) override;
		void write(Address offset, u32 value) override;
		const RegisterMap& registers() const override;

	private:
		void emit();
	};
}
