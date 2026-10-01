#include <ceres/driver/command.h>

#include <charconv>
#include <chrono>
#include <string_view>

namespace ceres::driver
{
	namespace
	{
		constexpr std::string_view Usage =
			"Ceres - assembler and virtual machine\n"
			"\n"
			"  ceres asm <source.casm> [<source2.casm> ...] [-o <output.cres>] [--listing] [--json]\n"
			"                          [--debug] [--emit-debug-json] [-c] [-I <dir>] [--stdlib]\n"
			"  ceres link <file.cobj|file.car> [...] -o <output.cres> [--debug] [--symtab] [--gc-sections] [--stdlib]\n"
			"  ceres ar <output.car> <file.cobj> [...]\n"
			"  ceres run [<file.casm|file.cres>] [<machine>] [--shell] [--disk <image>]\n"
			"                                  [--window | --headless] [--strict-mmio]\n"
			"                                  [--fullscreen] [--exit-on-halt] [--frames <dir>] [--refresh 50|60]\n"
			"                                  [--transcript <file>] [--screen-log <file>] [--type <file>] [--keys <file>]\n"
			"                                  [--gpu auto|software|hardware]\n"
			"                                  [--rtc <YYYY-MM-DDThh:mm:ss>] [--speed realtime|max|<f>x]\n"
			"                                  [--record <file> | --replay <file>] [--log <file>]\n"
			"                                  [--port <n>=<image>]... [--cart <n>=<file>]...\n"
			"                                  [--env <name>=<value>]... [--host-dir <dir>] [-- <argument>...]\n"
			"  ceres profile <file.casm|file.cres> [<machine>]\n"
			"  ceres disasm <file.casm|file.cres> [--debug]\n"
			"  ceres debug <file.casm|file.cres> [<source2.casm> ...] [<machine>]\n"
			"                                    [--no-stop-on-entry] [--server] [--no-history]\n"
			"\n"
			"  <machine>: [--profile micro|pocket|retro|arcade|polygon|standard|workstation|custom]\n"
			"             [--cpu-clock <hz>] [--gpu-clock <hz>] [--ram <bytes>] [--vram <bytes>]\n"
			"             [--max-video V0-V6] [--max-audio A0-A4] [--max-resolution <w>x<h>]\n"
			"\n"
			"A bare path is shorthand for 'run'.\n"
			"\n"
			"With a window (an SDL build), 'run' shows the machine's screen in it from the start, one frame a vertical\n"
			"blank (--refresh: 50 or 60 a second, 60 by default), and keeps it open with the last frame when the program\n"
			"ends, until a key is pressed or it is closed; --exit-on-halt closes it at once. --fullscreen (or F11) fills\n"
			"the screen. --window makes a window that cannot be opened an error; --headless (or CERES_HEADLESS in the\n"
			"environment) opens none. --frames writes a PNG of the screen into <dir> for every Present. --gpu picks the\n"
			"GPU's executor; there is only the software one so far, and a run without a window always uses it.\n"
			"\n"
			"The program's input and output are its terminal's, in the window: nothing it writes reaches this terminal,\n"
			"and nothing typed here reaches it. --transcript writes everything it wrote to its terminal to a file (the\n"
			"error stream between ESC[E and ESC[e); --screen-log the screen as text at every Present and at the end.\n"
			"--type types a file's text as the machine starts; --keys presses keys at instants of the machine's time\n"
			"('<ms> down|up|press <key>' or '<ms> text <text>' a line). Without a window, the input ends after them.\n"
			"\n"
			"Everything after -- goes to the program: main(argc, argv) gets the input's path as argv[0], then those.\n"
			"--env gives it an environment variable (getenv); nothing of the host's environment is passed on.\n"
			"--host-dir lets it open, write and list the host's files under <dir>, and nowhere else.\n"
			"Without a program, 'run' starts the Ceres shell: shell/shell.cres in the directory CERES_PATH names (where\n"
			"Ceres is installed), or else in the one this ceres is in - or shell/shell-small.cres, the same shell made\n"
			"small, on a machine the first does not fit (micro); --shell goes back to it whenever the program ends.\n"
			"With the shell, the host directory is the current one unless --host-dir names another.\n"
			"--rtc starts the machine's real-time clock at that moment (UTC) instead of the host's.\n"
			"--speed paces the machine's time against the host's: realtime (the default while a window is open), max\n"
			"(the default without one: no waiting at all) or a factor such as 0.5x or 2x.\n"
			"--profile picks the machine: its clocks, its RAM and VRAM and the most video and audio it has ('standard'\n"
			"when none is given: 50 MHz, 64 MiB of RAM). Each of the other machine options starts from that profile and\n"
			"makes it 'custom', which alone reaches 1920x1080: clocks in Hz with an optional k, M or G (the CPU up to 400M,\n"
			"the GPU up to 1G), sizes in bytes with an optional K, M or G, multiples of 4K (RAM up to 2G, VRAM up to 1G).\n"
			"--record writes everything the host gives the machine (input, keys, the mouse, files dropped), each with the\n"
			"cycle it went in at; --replay feeds such a recording back at the same cycles and reads no input of the host's.\n"
			"--log sends the host's log - the program's debug log and what the host has to say about the run, such as an\n"
			"exception nobody handled - to a file instead of stderr, one '[ceres:<level>] <line>' each.\n"
			"\n"
			"--stdlib compiles (and, where a program is produced, links) against the standard library installed with\n"
			"Ceres: its stdlib/lib joins the import search, so a program writes `import \"libceres.decls.casm\"` for the\n"
			"library's names, and libceres.car is linked. Where Ceres is installed is CERES_PATH, or the directory\n"
			"ceres itself is in. -I <dir> adds an import search directory of your own; a relative import is looked for\n"
			"beside the importing file first, then in each search directory.\n";

		struct RawOptions
		{
			std::filesystem::path output;
			std::vector<std::filesystem::path> positional;
			bool compileOnly = false;
			bool usedOutput = false;
			bool listing = false;
			bool usedListing = false;
			bool json = false;
			bool usedJson = false;
			bool debugInfo = false;
			bool usedDebugInfo = false;
			bool debugJson = false;
			bool usedDebugJson = false;
			bool symbolTable = false;
			bool usedSymbolTable = false;
			bool gcSections = false;
			bool usedGcSections = false;
			bool stdlib = false;                              // --stdlib: the library installed with Ceres
			bool usedStdlib = false;
			std::vector<std::filesystem::path> importDirectories;   // -I, in command-line order
			bool usedImport = false;
			bool stopOnEntry = true;
			bool usedStopOnEntry = false;
			bool server = false;
			bool usedServer = false;
			bool recordHistory = true;
			bool usedHistory = false;
			// The window's and the files': --fullscreen, --exit-on-halt, --frames, --refresh, --transcript, --screen-log, --type, --keys (run only).
			bool fullscreen = false;
			bool exitOnHalt = false;
			std::filesystem::path framesDir;
			std::filesystem::path transcript;
			std::filesystem::path screenLog;
			std::filesystem::path typeFile;
			std::filesystem::path keysFile;
			GpuExecutor gpu = GpuExecutor::Auto;
			u32 refresh = 60;
			bool usedScreen = false;
			// The machine: a profile, and the options that change it into `custom`.
			std::optional<ProfileId> profile;
			std::optional<u64> cpuClockHz;
			std::optional<u64> gpuClockHz;
			std::optional<usize> ramBytes;
			std::optional<usize> vramBytes;
			std::optional<u32> maxVideo;
			std::optional<u32> maxAudio;
			std::optional<std::pair<u32, u32>> maxResolution;
			std::filesystem::path disk;
			bool usedDisk = false;            // also set by --port and --cart: none of them belongs to any command but run
			std::vector<PortAttachment> ports;
			std::vector<std::string> arguments;
			std::vector<std::string> environment;
			std::filesystem::path hostDirectory;
			bool usedDashDash = false;        // --, --env and --host-dir belong to run alone
			bool usedEnv = false;
			bool usedHostDir = false;
			bool strictMmio = false;
			std::optional<i64> rtc;
			std::optional<Speed> speed;
			std::filesystem::path record;
			std::filesystem::path replay;
			std::filesystem::path log;
			bool window = false;
			bool usedWindow = false;
			bool headless = false;
			bool usedHeadless = false;
			bool shell = false;               // --shell belongs to run alone
			bool usedShell = false;
		};

		// YYYY-MM-DDThh:mm:ss, UTC, from 1970 on: the machine's real-time clock counts seconds since then.
		std::expected<i64, ParseError> parseRtc(std::string_view text)
		{
			const auto bad = [&] { return std::unexpected(ParseError{ "'--rtc' takes YYYY-MM-DDThh:mm:ss, for example 2026-09-26T12:00:00, not '" + std::string(text) + "'" }); };
			if (text.size() != 19 || text[4] != '-' || text[7] != '-' || text[10] != 'T' || text[13] != ':' || text[16] != ':')
				return bad();
			const auto number = [&](usize at, usize length, int& out)
			{
				const auto [end, error] = std::from_chars(text.data() + at, text.data() + at + length, out);
				return error == std::errc{} && end == text.data() + at + length;
			};
			int year = 0, month = 0, day = 0, hour = 0, minute = 0, second = 0;
			if (!number(0, 4, year) || !number(5, 2, month) || !number(8, 2, day) || !number(11, 2, hour) ||
				!number(14, 2, minute) || !number(17, 2, second))
				return bad();
			const std::chrono::year_month_day date{ std::chrono::year{ year }, std::chrono::month{ static_cast<unsigned>(month) },
				std::chrono::day{ static_cast<unsigned>(day) } };
			if (!date.ok() || year < 1970 || hour < 0 || hour > 23 || minute < 0 || minute > 59 || second < 0 || second > 59)
				return bad();
			const auto days = std::chrono::sys_days{ date }.time_since_epoch();
			return std::chrono::duration_cast<std::chrono::seconds>(days).count() + hour * 3600 + minute * 60 + second;
		}

		// A number of cycles per second, with an optional k, M or G and an optional Hz: 50000000, 50M, 50MHz. Up to
		// `limit` (plan/v2 SPEC 4: 400 MHz for the CPU, 1 GHz for the GPU).
		std::expected<u64, ParseError> parseClock(std::string_view option, std::string_view text, u64 limit)
		{
			const auto bad = [&] { return std::unexpected(ParseError{ "'" + std::string(option) + "' takes cycles per second, for example 50M or 4000000, not '" + std::string(text) + "'" }); };
			std::string_view digits = text;
			if (digits.size() > 2 && (digits.ends_with("Hz") || digits.ends_with("hz")))
				digits.remove_suffix(2);
			u64 scale = 1;
			if (!digits.empty())
			{
				const char unit = digits.back();
				if (unit == 'k' || unit == 'K') scale = 1'000;
				else if (unit == 'M' || unit == 'm') scale = 1'000'000;
				else if (unit == 'G' || unit == 'g') scale = 1'000'000'000;
				if (scale != 1)
					digits.remove_suffix(1);
			}
			u64 value = 0;
			const auto [end, error] = std::from_chars(digits.data(), digits.data() + digits.size(), value);
			if (digits.empty() || error != std::errc{} || end != digits.data() + digits.size())
				return bad();
			if (value == 0 || value > limit / scale)
				return std::unexpected(ParseError{ "'" + std::string(option) + "' must be between 1 Hz and " + std::to_string(limit) + " Hz" });
			return value * scale;
		}

		// A size in bytes, with an optional K, M or G (of 1024): 65536, 64K, 64M. Whether the machine can have it is
		// checked with the rest of the profile (checkCustomProfile).
		std::expected<usize, ParseError> parseBytes(std::string_view option, std::string_view text)
		{
			const auto bad = [&] { return std::unexpected(ParseError{ "'" + std::string(option) + "' takes a number of bytes with an optional K, M or G, for example 64M, not '" + std::string(text) + "'" }); };
			std::string_view digits = text;
			u64 scale = 1;
			if (!digits.empty())
			{
				const char unit = digits.back();
				if (unit == 'k' || unit == 'K') scale = 1024;
				else if (unit == 'm' || unit == 'M') scale = 1024 * 1024;
				else if (unit == 'g' || unit == 'G') scale = 1024 * 1024 * 1024;
				if (scale != 1)
					digits.remove_suffix(1);
			}
			u64 value = 0;
			const auto [end, error] = std::from_chars(digits.data(), digits.data() + digits.size(), value);
			if (digits.empty() || error != std::errc{} || end != digits.data() + digits.size() || value == 0 || value > (u64{ 1 } << 32) / scale)
				return bad();
			return static_cast<usize>(value * scale);
		}

		// A level of `--max-video` (V0-V6) or `--max-audio` (A0-A4): the letter is optional.
		std::expected<u32, ParseError> parseLevel(std::string_view option, std::string_view text, char letter, u32 highest)
		{
			std::string_view digits = text;
			if (!digits.empty() && (digits.front() == letter || digits.front() == letter + ('a' - 'A')))
				digits.remove_prefix(1);
			u32 value = 0;
			const auto [end, error] = std::from_chars(digits.data(), digits.data() + digits.size(), value);
			if (digits.empty() || error != std::errc{} || end != digits.data() + digits.size() || value > highest)
				return std::unexpected(ParseError{ "'" + std::string(option) + "' takes " + std::string(1, letter) + "0 to " +
					std::string(1, letter) + std::to_string(highest) + ", not '" + std::string(text) + "'" });
			return value;
		}

		// <width>x<height>: 1280x720.
		std::expected<std::pair<u32, u32>, ParseError> parseResolution(std::string_view text)
		{
			const usize cross = text.find_first_of("xX");
			u32 width = 0, height = 0;
			if (cross != std::string_view::npos)
			{
				const auto [endW, errorW] = std::from_chars(text.data(), text.data() + cross, width);
				const auto [endH, errorH] = std::from_chars(text.data() + cross + 1, text.data() + text.size(), height);
				if (errorW == std::errc{} && endW == text.data() + cross && errorH == std::errc{} && endH == text.data() + text.size() && cross > 0)
					return std::pair{ width, height };
			}
			return std::unexpected(ParseError{ "'--max-resolution' takes <width>x<height>, for example 1280x720, not '" + std::string(text) + "'" });
		}

		// The machine the options ask for (plan/v2 SPEC 4): the profile, `standard` by default, and any other machine
		// option on top of it, which makes it `custom` and has to stay inside what `custom` allows.
		std::expected<MachineProfile, ParseError> resolveMachine(const RawOptions& raw)
		{
			MachineProfile machine = machineProfile(raw.profile.value_or(ProfileId::Standard));
			const bool loose = raw.cpuClockHz || raw.gpuClockHz || raw.ramBytes || raw.vramBytes || raw.maxVideo || raw.maxAudio || raw.maxResolution;
			if (raw.cpuClockHz) machine.cpuClockHz = *raw.cpuClockHz;
			if (raw.gpuClockHz) machine.gpuClockHz = *raw.gpuClockHz;
			if (raw.ramBytes) machine.ramBytes = *raw.ramBytes;
			if (raw.vramBytes) machine.vramBytes = *raw.vramBytes;
			if (raw.maxVideo) machine.maxVideo = *raw.maxVideo;
			if (raw.maxAudio) machine.maxAudio = *raw.maxAudio;
			if (raw.maxResolution) { machine.maxWidth = raw.maxResolution->first; machine.maxHeight = raw.maxResolution->second; }
			if (loose)
				machine.id = ProfileId::Custom;
			if (machine.id == ProfileId::Custom)
			{
				if (auto fits = checkCustomProfile(machine); !fits)
					return std::unexpected(ParseError{ "No such machine: " + fits.error() });
			}
			return machine;
		}

		bool usesMachine(const RawOptions& raw)
		{
			return raw.profile || raw.cpuClockHz || raw.gpuClockHz || raw.ramBytes || raw.vramBytes || raw.maxVideo || raw.maxAudio || raw.maxResolution;
		}

		bool isCommand(std::string_view text)
		{
			return text == "asm" || text == "run" || text == "disasm" || text == "debug" ||
				text == "profile" || text == "link" || text == "ar";
		}

		ParseError invalidOption(std::string_view option, std::string_view command)
		{
			return ParseError{ "Option '" + std::string(option) + "' is not valid for '" +
				std::string(command) + "'" };
		}
	}

	std::string_view usageText() noexcept { return Usage; }

	std::expected<Command, ParseError> parseCommandLine(int argc, char* const argv[])
	{
		RawOptions raw;
		for (int i = 1; i < argc; ++i)
		{
			const std::string_view argument = argv[i];
			auto nextValue = [&](std::string_view option) -> std::expected<std::string_view, ParseError>
			{
				if (++i >= argc)
					return std::unexpected(ParseError{ "Missing value after " + std::string(option) });
				return argv[i];
			};

			if (argument == "--")
			{
				// The rest is the program's, options or not.
				for (++i; i < argc; ++i)
					raw.arguments.emplace_back(argv[i]);
				raw.usedDashDash = true;
				break;
			}
			if (argument == "--env")
			{
				auto value = nextValue(argument);
				if (!value) return std::unexpected(value.error());
				const std::string_view text = *value;
				if (text.find('=') == std::string_view::npos || text.front() == '=')
					return std::unexpected(ParseError{ "'--env' takes <name>=<value>, for example --env HOME=/save" });
				raw.environment.emplace_back(text);
				raw.usedEnv = true;
			}
			else if (argument == "--host-dir")
			{
				auto value = nextValue(argument);
				if (!value) return std::unexpected(value.error());
				raw.hostDirectory = *value;
				raw.usedHostDir = true;
			}
			else if (argument == "--record" || argument == "--replay")
			{
				auto value = nextValue(argument);
				if (!value) return std::unexpected(value.error());
				(argument == "--record" ? raw.record : raw.replay) = *value;
			}
			else if (argument == "--log")
			{
				auto value = nextValue(argument);
				if (!value) return std::unexpected(value.error());
				raw.log = *value;
			}
			else if (argument == "-o" || argument == "--output")
			{
				auto value = nextValue(argument);
				if (!value) return std::unexpected(value.error());
				raw.output = *value; raw.usedOutput = true;
			}
			else if (argument == "-c") raw.compileOnly = true;
			else if (argument == "--listing") { raw.listing = true; raw.usedListing = true; }
			else if (argument == "--json") { raw.json = true; raw.usedJson = true; }
			else if (argument == "--debug") { raw.debugInfo = true; raw.usedDebugInfo = true; }
			else if (argument == "--emit-debug-json") { raw.debugJson = true; raw.debugInfo = true; raw.usedDebugJson = true; }
			else if (argument == "--symtab") { raw.symbolTable = true; raw.usedSymbolTable = true; }
			else if (argument == "--gc-sections") { raw.gcSections = true; raw.usedGcSections = true; }
			else if (argument == "--stdlib") { raw.stdlib = true; raw.usedStdlib = true; }
			else if (argument == "-I")
			{
				auto value = nextValue(argument);
				if (!value) return std::unexpected(value.error());
				raw.importDirectories.emplace_back(*value);
				raw.usedImport = true;
			}
			else if (argument == "--no-stop-on-entry") { raw.stopOnEntry = false; raw.usedStopOnEntry = true; }
			else if (argument == "--server") { raw.server = true; raw.usedServer = true; }
			else if (argument == "--no-history") { raw.recordHistory = false; raw.usedHistory = true; }
			else if (argument == "--disk")
			{
				auto value = nextValue(argument);
				if (!value) return std::unexpected(value.error());
				raw.disk = *value; raw.usedDisk = true;
			}
			else if (argument == "--port" || argument == "--cart")
			{
				auto value = nextValue(argument);
				if (!value) return std::unexpected(value.error());
				// <port>=<file>: the port is a number, and the file is everything after the first '='
				const std::string_view text = *value;
				const usize equals = text.find('=');
				unsigned port = 0;
				bool numeric = equals != std::string_view::npos && equals > 0 && equals <= 3;
				for (usize i = 0; numeric && i < equals; ++i)
				{
					numeric = text[i] >= '0' && text[i] <= '9';
					port = port * 10 + static_cast<unsigned>(text[i] - '0');
				}
				if (!numeric || equals + 1 >= text.size())
					return std::unexpected(ParseError{ "'" + std::string(argument) + "' takes <port>=<file>, for example " + std::string(argument) + " 0=stick.img" });
				raw.ports.push_back(PortAttachment{ port, std::string(text.substr(equals + 1)), argument == "--cart" });
				raw.usedDisk = true;
			}
			else if (argument == "--shell") { raw.shell = true; raw.usedShell = true; }
			else if (argument == "--window") { raw.window = true; raw.usedWindow = true; }
			else if (argument == "--headless") { raw.headless = true; raw.usedHeadless = true; }
			else if (argument == "--transcript" || argument == "--screen-log" || argument == "--type" || argument == "--keys")
			{
				auto value = nextValue(argument);
				if (!value) return std::unexpected(value.error());
				(argument == "--transcript" ? raw.transcript : argument == "--screen-log" ? raw.screenLog :
					argument == "--type" ? raw.typeFile : raw.keysFile) = *value;
				raw.usedScreen = true;
			}
			else if (argument == "--strict-mmio") raw.strictMmio = true;
			else if (argument == "--fullscreen") { raw.fullscreen = true; raw.usedScreen = true; }
			else if (argument == "--exit-on-halt") { raw.exitOnHalt = true; raw.usedScreen = true; }
			else if (argument == "--frames")
			{
				auto value = nextValue(argument);
				if (!value) return std::unexpected(value.error());
				raw.framesDir = *value;
				raw.usedScreen = true;
			}
			else if (argument == "--gpu")
			{
				auto value = nextValue(argument);
				if (!value) return std::unexpected(value.error());
				if (*value == "auto") raw.gpu = GpuExecutor::Auto;
				else if (*value == "software") raw.gpu = GpuExecutor::Software;
				else if (*value == "hardware") raw.gpu = GpuExecutor::Hardware;
				else return std::unexpected(ParseError{ "'--gpu' takes auto, software or hardware, not '" + std::string(*value) + "'" });
				raw.usedScreen = true;
			}
			else if (argument == "--refresh")
			{
				auto value = nextValue(argument);
				if (!value) return std::unexpected(value.error());
				if (*value != "50" && *value != "60")
					return std::unexpected(ParseError{ "'--refresh' takes 50 or 60, not '" + std::string(*value) + "'" });
				raw.refresh = *value == "50" ? 50u : 60u;
				raw.usedScreen = true;
			}
			else if (argument == "--speed")
			{
				auto value = nextValue(argument);
				if (!value) return std::unexpected(value.error());
				raw.speed = Speed::parse(*value);
				if (!raw.speed)
					return std::unexpected(ParseError{ "'--speed' takes realtime, max or a factor such as 2x, not '" + std::string(*value) + "'" });
			}
			else if (argument == "--profile")
			{
				auto value = nextValue(argument);
				if (!value) return std::unexpected(value.error());
				raw.profile = profileNamed(*value);
				if (!raw.profile)
					return std::unexpected(ParseError{ "'--profile' takes micro, pocket, retro, arcade, polygon, standard, workstation or custom, not '" + std::string(*value) + "'" });
			}
			else if (argument == "--cpu-clock" || argument == "--gpu-clock")
			{
				auto value = nextValue(argument);
				if (!value) return std::unexpected(value.error());
				const bool cpu = argument == "--cpu-clock";
				auto clock = parseClock(argument, *value, cpu ? ProfileLimits::MaxCpuClockHz : ProfileLimits::MaxGpuClockHz);
				if (!clock) return std::unexpected(clock.error());
				(cpu ? raw.cpuClockHz : raw.gpuClockHz) = *clock;
			}
			else if (argument == "--ram" || argument == "--vram")
			{
				auto value = nextValue(argument);
				if (!value) return std::unexpected(value.error());
				auto bytes = parseBytes(argument, *value);
				if (!bytes) return std::unexpected(bytes.error());
				(argument == "--ram" ? raw.ramBytes : raw.vramBytes) = *bytes;
			}
			else if (argument == "--max-video" || argument == "--max-audio")
			{
				auto value = nextValue(argument);
				if (!value) return std::unexpected(value.error());
				const bool video = argument == "--max-video";
				auto level = parseLevel(argument, *value, video ? 'V' : 'A', video ? ProfileLimits::MaxVideo : ProfileLimits::MaxAudio);
				if (!level) return std::unexpected(level.error());
				(video ? raw.maxVideo : raw.maxAudio) = *level;
			}
			else if (argument == "--max-resolution")
			{
				auto value = nextValue(argument);
				if (!value) return std::unexpected(value.error());
				auto resolution = parseResolution(*value);
				if (!resolution) return std::unexpected(resolution.error());
				raw.maxResolution = *resolution;
			}
			else if (argument == "--rtc")
			{
				auto value = nextValue(argument);
				if (!value) return std::unexpected(value.error());
				auto rtc = parseRtc(*value);
				if (!rtc) return std::unexpected(rtc.error());
				raw.rtc = *rtc;
			}
			else if (argument == "-h" || argument == "--help")
				return std::unexpected(ParseError{});
			else if (argument.starts_with('-'))
				return std::unexpected(ParseError{ "Unknown option '" + std::string(argument) + "'" });
			else
				raw.positional.emplace_back(argument);
		}

		if (raw.positional.empty())
			return std::unexpected(ParseError{});

		std::string command = "run";
		usize firstInput = 0;
		if (isCommand(raw.positional.front().string()))
		{
			command = raw.positional.front().string();
			firstInput = 1;
		}
		// `ceres run` alone starts the shell; every other command needs its input.
		if (raw.positional.size() <= firstInput && !(command == "run" && firstInput == 1))
			return std::unexpected(ParseError{ "Missing input file for '" + std::string(command) + "'" });

		std::vector<std::filesystem::path> inputs(raw.positional.begin() + static_cast<std::ptrdiff_t>(firstInput), raw.positional.end());
		if (raw.usedSymbolTable && command != "link")
			return std::unexpected(invalidOption("--symtab", command));
		if (raw.usedGcSections && command != "link")
			return std::unexpected(invalidOption("--gc-sections", command));
		if (raw.usedStdlib && command != "asm" && command != "link")
			return std::unexpected(invalidOption("--stdlib", command));
		if (raw.usedImport && command != "asm")
			return std::unexpected(invalidOption("-I", command));
		if (raw.usedShell && command != "run")
			return std::unexpected(invalidOption("--shell", command));
		if ((raw.usedDashDash || raw.usedEnv || raw.usedHostDir || raw.strictMmio || raw.rtc || raw.speed ||
			!raw.record.empty() || !raw.replay.empty() || !raw.log.empty() || raw.usedScreen) && command != "run")
			return std::unexpected(invalidOption(raw.usedScreen ? "a window or output option" : raw.usedEnv ? "--env" : raw.usedHostDir ? "--host-dir" : raw.strictMmio ? "--strict-mmio" :
				raw.rtc ? "--rtc" : raw.speed ? "--speed" : !raw.record.empty() ? "--record" :
				!raw.replay.empty() ? "--replay" : !raw.log.empty() ? "--log" : "--", command));
		// The machine's options belong to the commands that run one.
		if (usesMachine(raw) && command != "run" && command != "profile" && command != "debug")
			return std::unexpected(invalidOption(raw.profile ? "--profile" : "a machine option", command));
		const auto machine = resolveMachine(raw);
		if (!machine)
			return std::unexpected(machine.error());
		if (command == "asm")
		{
			if (raw.usedDisk || raw.usedWindow || raw.usedHeadless || raw.usedStopOnEntry || raw.usedServer || raw.usedHistory)
				return std::unexpected(invalidOption("a supplied option", command));
			return AssembleCommand{ std::move(inputs), std::move(raw.output), raw.compileOnly, raw.listing,
				raw.json, raw.debugInfo, raw.debugJson, raw.stdlib, std::move(raw.importDirectories) };
		}
		if (command == "link")
		{
			if (raw.compileOnly || raw.usedListing || raw.usedJson || raw.usedDisk || raw.usedWindow || raw.usedHeadless || raw.usedStopOnEntry || raw.usedServer || raw.usedHistory)
				return std::unexpected(invalidOption("a supplied option", command));
			if (raw.output.empty()) return std::unexpected(ParseError{ "'ceres link' needs -o <output.cres>" });
			return LinkCommand{ std::move(inputs), std::move(raw.output), raw.debugInfo, raw.debugJson, raw.symbolTable, raw.gcSections, raw.stdlib };
		}
		if (command == "ar")
		{
			if (raw.compileOnly || raw.usedListing || raw.usedJson || raw.usedDebugInfo || raw.usedDebugJson || raw.usedDisk ||
				raw.usedWindow || raw.usedHeadless || raw.usedStopOnEntry || raw.usedServer || raw.usedHistory || raw.usedOutput)
				return std::unexpected(invalidOption("a supplied option", command));
			if (inputs.size() < 2) return std::unexpected(ParseError{ "'ceres ar' needs an output and at least one object" });
			ArchiveCommand archive{ std::move(inputs.front()), {} };
			archive.inputs.assign(std::make_move_iterator(inputs.begin() + 1), std::make_move_iterator(inputs.end()));
			return archive;
		}
		if (inputs.size() != 1 && command != "debug" && !(command == "run" && inputs.empty()))
			return std::unexpected(ParseError{ "'" + std::string(command) + "' takes a single input file" });
		if (command == "run")
		{
			if (raw.compileOnly || raw.usedOutput || raw.usedJson || raw.usedDebugJson || raw.usedStopOnEntry || raw.usedServer || raw.usedHistory)
				return std::unexpected(invalidOption("a supplied option", command));
			if (raw.window && raw.headless)
				return std::unexpected(ParseError{ "'--window' and '--headless' are opposites: pick one" });
			if (!raw.record.empty() && !raw.replay.empty())
				return std::unexpected(ParseError{ "'--record' and '--replay' are opposites: pick one" });
			if (raw.shell && inputs.empty())
				return std::unexpected(ParseError{ "'--shell' goes back to the shell after a program: name one, or leave both out for the shell alone" });
			return RunCommand{ inputs.empty() ? std::filesystem::path{} : std::move(inputs.front()), *machine, std::move(raw.disk), raw.listing, raw.debugInfo, raw.window, raw.headless,
				std::move(raw.ports), std::move(raw.arguments), std::move(raw.environment), std::move(raw.hostDirectory), raw.strictMmio,
				raw.rtc, raw.speed, std::move(raw.record), std::move(raw.replay), std::move(raw.log), raw.refresh, raw.fullscreen,
				raw.exitOnHalt, std::move(raw.framesDir), std::move(raw.transcript), std::move(raw.screenLog), std::move(raw.typeFile),
				std::move(raw.keysFile), raw.gpu, raw.shell };
		}
		if (command == "profile")
		{
			if (raw.compileOnly || raw.usedOutput || raw.usedJson || raw.usedDebugInfo || raw.usedDebugJson || raw.usedDisk || raw.usedWindow || raw.usedHeadless || raw.usedStopOnEntry || raw.usedServer || raw.usedHistory)
				return std::unexpected(invalidOption("a supplied option", command));
			return ProfileCommand{ std::move(inputs.front()), *machine, raw.listing };
		}
		if (command == "disasm")
		{
			if (raw.compileOnly || raw.usedOutput || raw.usedListing || raw.usedJson || raw.usedDisk || raw.usedWindow || raw.usedHeadless || raw.usedStopOnEntry || raw.usedServer || raw.usedHistory)
				return std::unexpected(invalidOption("a supplied option", command));
			return DisassembleCommand{ std::move(inputs.front()), raw.debugInfo, raw.debugJson };
		}
		if (raw.compileOnly || raw.usedOutput || raw.usedListing || raw.usedJson || raw.usedDebugInfo || raw.usedDebugJson || raw.usedDisk || raw.usedWindow || raw.usedHeadless)
			return std::unexpected(invalidOption("a supplied option", command));
		return DebugCommand{ std::move(inputs), *machine, raw.stopOnEntry, raw.server, raw.recordHistory };
	}
}
