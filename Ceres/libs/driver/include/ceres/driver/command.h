#pragma once

#include <ceres/vm/memory.h>
#include <expected>
#include <filesystem>
#include <optional>
#include <ceres/driver/pacer.h>
#include <ceres/driver/profiles.h>
#include <string>
#include <variant>
#include <vector>

namespace ceres::driver
{
	struct AssembleCommand
	{
		std::vector<std::filesystem::path> inputs;
		std::filesystem::path output;
		bool compileOnly = false;
		bool listing = false;
		bool jsonDiagnostics = false;
		bool debugInfo = false;
		bool debugJson = false;
	};

	struct LinkCommand
	{
		std::vector<std::filesystem::path> inputs;
		std::filesystem::path output;
		bool debugInfo = false;
		bool debugJson = false;
		bool symbolTable = false;   // --symtab
		bool gcSections = false;    // --gc-sections
	};

	struct ArchiveCommand
	{
		std::filesystem::path output;
		std::vector<std::filesystem::path> inputs;
	};

	// `--port 0=stick.img` and `--cart 1=game.cart`: a medium plugged into one of the peripheral ports before the program starts.
	struct PortAttachment
	{
		unsigned port = 0;
		std::filesystem::path path;
		bool cartridge = false;   // read only, and has to exist
	};

	// --gpu: which executor composes the GPU's frames (plan/v2 SPEC 10). There is only the software one until F12,
	// so the three run the same; without a window it is always the software one.
	enum class GpuExecutor { Auto, Software, Hardware };

	struct RunCommand
	{
		std::filesystem::path input;            // empty: the shell (<ceres path>/shell/shell.cres) is the program
		MachineProfile machine = defaultMachineProfile();   // --profile and the options that make it custom
		std::filesystem::path diskImage;
		bool listing = false;
		bool debugInfo = false;
		bool window = false;     // --window: a window that cannot be opened is an error
		bool headless = false;   // --headless: no window (so does CERES_HEADLESS in the environment)
		std::vector<PortAttachment> ports;
		std::vector<std::string> arguments;     // after --: argv[1] on (argv[0] is the input's path)
		std::vector<std::string> environment;   // --env NAME=value, in order
		std::filesystem::path hostDirectory;    // --host-dir: the host directory the program's host files live in
		bool strictMmio = false;                // --strict-mmio: an undeclared device register faults instead of reading 0
		std::optional<i64> rtc;                 // --rtc: the real-time clock's start, in seconds since 1970 (UTC)
		std::optional<Speed> speed;             // --speed realtime|max|<f>x
		std::filesystem::path record;           // --record: write the host's input, stamped with its cycles, here
		std::filesystem::path replay;           // --replay: feed a recording back instead of the host's input
		std::filesystem::path logFile;          // --log: the host's log (the debug log and the diagnostics) goes here
		u32 refresh = 60;                       // --refresh 50|60: the display's frames a second
		bool fullscreen = false;                // --fullscreen: the window takes the whole screen (F11 toggles it)
		bool exitOnHalt = false;                // --exit-on-halt: the window closes when the program ends
		std::filesystem::path framesDir;        // --frames: a PNG of the screen for every Present, into this directory
		std::filesystem::path transcript;       // --transcript: every byte the program writes to the terminal
		std::filesystem::path screenLog;        // --screen-log: the text plane as text at every Present and at the end
		std::filesystem::path typeFile;         // --type: text typed on the terminal as the machine starts
		std::filesystem::path keysFile;         // --keys: keyboard events at instants of the machine's time
		GpuExecutor gpu = GpuExecutor::Auto;    // --gpu auto|software|hardware
		bool shell = false;                     // --shell: back to the shell whenever the program ends (so without an input)
	};

	struct ProfileCommand
	{
		std::filesystem::path input;
		MachineProfile machine = defaultMachineProfile();
		bool listing = false;
	};

	struct DisassembleCommand
	{
		std::filesystem::path input;
		bool debugInfo = false;
		bool debugJson = false;
	};

	struct DebugCommand
	{
		std::vector<std::filesystem::path> inputs;
		MachineProfile machine = defaultMachineProfile();
		bool stopOnEntry = true;
		bool server = false;
		bool recordHistory = true;
	};

	using Command = std::variant<AssembleCommand, LinkCommand, ArchiveCommand, RunCommand,
		ProfileCommand, DisassembleCommand, DebugCommand>;

	struct ParseError { std::string message; };

	std::expected<Command, ParseError> parseCommandLine(int argc, char* const argv[]);
	std::string_view usageText() noexcept;
}
