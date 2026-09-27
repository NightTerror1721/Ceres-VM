#include "machine_runner.h"
#include "headless_output.h"
#include "key_script.h"
#include "png_writer.h"

#include <ceres/driver/driver.h>
#include <ceres/driver/host_log.h>
#include <ceres/driver/pacer.h>
#include <ceres/driver/input_journal.h>
#include <ceres/devices/devices.h>
#include <ceres/devices/storage/disk.h>
#include <ceres/devices/input/gamepad.h>
#include <ceres/devices/input/keyboard.h>
#include <ceres/devices/input/mouse.h>
#include <ceres/devices/audio/audio.h>
#include <ceres/devices/storage/peripherals.h>
#include <ceres/devices/storage/host_fs.h>
#include <ceres/devices/video/blitter.h>
#include <ceres/devices/video/gpu.h>
#include <ceres/vm/ceresvm.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <format>
#include <functional>
#include <fstream>
#include <iostream>
#include <iterator>
#include <map>
#include <optional>
#include <memory>
#include <thread>

namespace ceres::driver
{
	using namespace devices;
	using namespace fmt;
	using namespace vm;

	class Machine::Impl
	{
	public:
		CeresVM vm;
		SystemControlDevice control;
		TerminalDevice terminal;
		TimerDevice timer;
		DmaController dma;
		DiskDevice disk;
		KeyboardDevice keyboard;
		MouseDevice mouse;
		GamepadDevice gamepad;
		AudioDevice audio;
		PeripheralDevice peripherals;
		HostFsDevice hostFs;
		BlitterDevice blitter;
		DebugLogDevice debugLog;
		std::string startupError;

		Impl(const MachineConfig& config, const MachineHost& host) :
			vm(config.machine.ramBytes, config.machine.vramBytes),
			control([this] { vm.shutdown(); }, [this] { vm.requestReset(); })
		{
			control.setFeaturesCallback([this](u32 features)
			{
				vm.engine().setDivisionFaults((features & SystemControlDevice::FeatureDivisionFault) != 0);
				vm.engine().setIeeeDivide((features & SystemControlDevice::FeatureIeeeDivide) != 0);
			});
			control.setStackLimitHandlers([this] { return vm.engine().stackLimit(); },
				[this](u32 address) { vm.engine().setProgramStackLimit(address); });
			control.setFaultInfoHandlers([this] { return vm.engine().faultAddress(); }, [this] { return vm.engine().faultAccess(); }, [this] { return vm.engine().faultReason(); });
			control.setArgumentHandler([this](u32 which) { return vm.argumentInfo(which); });
			control.setProfileId(static_cast<u32>(config.machine.id));
			vm.io().scheduler().setClockHz(config.machine.cpuClockHz);
			vm.setProgramArguments(vm::ProgramArguments{ config.arguments, config.environment });
			control.attachTo(vm.io());
			terminal.attachTo(vm.io());
			timer.attachTo(vm.io());
			dma.attachTo(vm.io());
			disk.attachTo(vm.io());
			keyboard.attachTo(vm.io());
			mouse.attachTo(vm.io());
			gamepad.attachTo(vm.io());
			audio.attachTo(vm.io());
			peripherals.attachTo(vm.io());
			hostFs.attachTo(vm.io());
			blitter.attachTo(vm.io());
			debugLog.attachTo(vm.io());
			if (host.debugLog)
				debugLog.setSink(host.debugLog);
			if (!config.hostDirectory.empty() && !hostFs.setRoot(config.hostDirectory))
				startupError = "Not a directory: " + config.hostDirectory.string();

			if (host.terminalError)
				terminal.setErrorSink([sink = host.terminalError](u8 byte)
				{
					sink(std::span<const u8>(&byte, 1));
				});
			if (host.terminalOutput)
				terminal.setOutputSink([sink = host.terminalOutput](u8 byte)
				{
					sink(std::span<const u8>(&byte, 1));
				});
			if (!config.diskImage.empty() && !disk.open(config.diskImage))
				startupError = "Failed to open disk image: " + config.diskImage.string();
			for (const MachineConfig::Port& port : config.ports)
			{
				std::string error;
				if (!peripherals.attachFile(port.port, port.path, port.cartridge ? PeripheralDevice::Kind::Cartridge : PeripheralDevice::Kind::Storage, &error))
					startupError = "Failed to plug in " + port.path.string() + ": " + error;
			}
		}

		~Impl()
		{
			terminal.detachFrom(vm.io());
			disk.detachFrom(vm.io());
			timer.detachFrom(vm.io());
			dma.detachFrom(vm.io());
			keyboard.detachFrom(vm.io());
			mouse.detachFrom(vm.io());
			gamepad.detachFrom(vm.io());
			audio.detachFrom(vm.io());
			peripherals.detachFrom(vm.io());
			hostFs.detachFrom(vm.io());
			blitter.detachFrom(vm.io());
			debugLog.detachFrom(vm.io());
			control.detachFrom(vm.io());
		}
	};

	Machine::Machine(MachineConfig config, MachineHost host) : _impl(std::make_unique<Impl>(config, host)) {}
	Machine::~Machine() = default;

	std::expected<void, std::string> Machine::load(const Program& program)
	{
		if (!_impl->startupError.empty())
			return std::unexpected(_impl->startupError);
		if (auto result = _impl->vm.loadProgram(program); !result)
			return std::unexpected(result.error());
		return {};
	}

	std::expected<void, std::string> Machine::run()
	{
		if (!_impl->startupError.empty())
			return std::unexpected(_impl->startupError);
		if (auto result = _impl->vm.run(); !result)
			return std::unexpected(result.error());
		return {};
	}

	void Machine::pushInput(std::span<const u8> bytes) { _impl->terminal.pushInput(bytes); }
	void Machine::pushInput(std::string_view text) { _impl->terminal.pushInput(text); }
	void Machine::closeInput() { _impl->terminal.closeInput(); }
	int Machine::exitCode() const noexcept { return _impl->control.exitCode(); }
	void Machine::pushKey(u32 code, bool pressed) { _impl->keyboard.pushKey(code, pressed); }
	void Machine::pushText(std::string_view utf8) { _impl->keyboard.pushText(utf8); }
	void Machine::pushMouse(i32 dx, i32 dy, u8 buttons, i8 wheel) { _impl->mouse.pushMotion(dx, dy, buttons, wheel); }
	u64 Machine::droppedInputBytes() const noexcept { return _impl->terminal.droppedInputBytes(); }

	bool Machine::attachPeripheral(unsigned port, const std::filesystem::path& path, bool cartridge, std::string* error)
	{
		return _impl->peripherals.attachFile(port, path, cartridge ? PeripheralDevice::Kind::Cartridge : PeripheralDevice::Kind::Storage, error);
	}

	bool Machine::detachPeripheral(unsigned port) { return _impl->peripherals.detach(port); }
	std::string Machine::describePeripheral(unsigned port) const { return _impl->peripherals.describe(port); }

	namespace
	{
		// A whole file, or nothing when it cannot be read.
		std::optional<std::string> readFile(const std::filesystem::path& path)
		{
			std::ifstream file(path, std::ios::binary);
			if (!file)
				return std::nullopt;
			return std::string(std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>());
		}

		std::string interruptName(InterruptNumber number)
		{
			switch (number)
			{
				case InterruptNumber::Reset:              return "Reset";
				case InterruptNumber::Trap:               return "Trap";
				case InterruptNumber::IllegalInstruction: return "IllegalInstruction";
				case InterruptNumber::MemoryFault:        return "MemoryFault";
				case InterruptNumber::DivisionByZero:     return "DivisionByZero";
				case InterruptNumber::StackOverflow:      return "StackOverflow";
				case InterruptNumber::AlignmentFault:     return "AlignmentFault";
				case InterruptNumber::PageFault:          return "PageFault";
				case InterruptNumber::Syscall:            return "Syscall";
				default: break;
			}
			const u32 value = static_cast<u32>(number);
			return value >= ReservedInterruptCount ? std::format("UserInterrupt{}", value - ReservedInterruptCount) : std::format("interrupt {}", value);
		}

		// A machine that stopped in one of the BIOS's default handlers (bios.h) ran into an exception the program
		// did not handle, and the handler shut it down with status 1. What to tell the user about it: which
		// exception, the instruction it came from, and for a memory fault what that instruction was doing and why
		// it faulted (SystemControl's FaultAddress, FaultAccess and FaultReason). Nothing for any other stop.
		std::optional<std::string> describeUnhandledException(const CeresVM& vm)
		{
			const ExecutionEngine& engine = vm.engine();
			const auto number = BIOS::defaultHandlerVector(engine.programCounter());
			if (vm.isPoweredOn() || !number)
				return std::nullopt;

			// The dispatch pushed the flags and then the PC it was at, and the handler left its stack alone: that
			// PC is the faulting instruction for a fault, and the one after it for trap and syscall.
			const u32 sp = engine.registers().getValue(GeneralPurposeRegisterPool::StackPointerIndex);
			const bool framed = sp <= vm.memory().size() - sizeof(u32) && sp % sizeof(u32) == 0;
			const u32 pc = framed ? vm.memory().readUnchecked<u32>(Address(sp)) : engine.programCounter().value();
			std::string message = std::format("Unhandled {} at 0x{:08X}", interruptName(*number), pc);

			const bool memoryFault = *number == InterruptNumber::MemoryFault || *number == InterruptNumber::AlignmentFault || *number == InterruptNumber::PageFault;
			if (memoryFault)
			{
				const u32 access = engine.faultAccess();
				const u32 kind = access & 0xFF;
				const u32 size = access >> 8;
				const std::string_view what = kind == 1 ? "read" : kind == 2 ? "write" : kind == 3 ? "fetch" : "access";
				message += size != 0 ? std::format(": {} of {} bytes at 0x{:08X}", what, size, engine.faultAddress())
					: std::format(": {} at 0x{:08X}", what, engine.faultAddress());
			}
			if (const u32 reason = engine.faultReason(); reason != 0 && (memoryFault || *number == InterruptNumber::IllegalInstruction))
			{
				const std::string_view name = faultReasonName(static_cast<FaultReason>(reason));
				message += name.empty() ? std::format(" (FaultReason {})", reason) : std::format(" (FaultReason {}, {})", reason, name);
			}
			return message;
		}

		// What the run cost, in instructions and in CPU cycles (plan/v2 SPEC 3.2): by function first - a function
		// being the code from one global text label to the next - and then by source line. Shares are of the cycles,
		// which is what the machine's time is made of.
		void printProfile(CeresVM& vm, const DebugInfo& info, std::ostream& err)
		{
			struct Cost { u64 count = 0; u64 cycles = 0; };
			const auto counts = vm.engine().executionCounts();
			const auto cycles = vm.engine().cycleCounts();
			const u32 textStart = vm.engine().textStart();

			// The functions: text labels that are not local (a local one is kept as `function.label`), by address.
			std::vector<std::pair<u32, std::string_view>> functions;
			for (const auto& symbol : info.symbols())
			{
				const std::string_view name = info.symbolName(symbol);
				if (static_cast<SymbolKind>(symbol.kind) == SymbolKind::Label && static_cast<SymbolSection>(symbol.section) == SymbolSection::Text &&
					!name.empty() && name.find('.') == std::string_view::npos && symbol.address >= textStart)
					functions.emplace_back(symbol.address, name);
			}
			std::ranges::sort(functions);

			std::map<std::pair<u32, u32>, Cost> lines;
			std::map<std::string_view, Cost> byFunction;
			Cost total;
			for (usize index = 0; index < counts.size(); ++index)
			{
				if (counts[index] == 0)
					continue;
				const u32 address = textStart + static_cast<u32>(index * Instruction::Size);
				const auto after = std::ranges::upper_bound(functions, address, {}, &std::pair<u32, std::string_view>::first);
				Cost& function = byFunction[after == functions.begin() ? std::string_view("(no function)") : std::prev(after)->second];
				function.count += counts[index];
				function.cycles += cycles[index];
				total.count += counts[index];
				total.cycles += cycles[index];
			}
			for (const auto& entry : info.lines())
			{
				if (entry.address < textStart)
					continue;
				const usize index = (entry.address - textStart) / Instruction::Size;
				if (index < counts.size())
				{
					Cost& line = lines[{entry.expansionFileId, entry.expansionLine}];
					line.count += counts[index];
					line.cycles += cycles[index];
				}
			}

			const auto share = [&](u64 part) { return total.cycles ? 100.0 * static_cast<double>(part) / static_cast<double>(total.cycles) : 0.0; };
			err << std::format("\n{} instructions executed, {} cycles\n", total.count, total.cycles);

			std::vector<std::pair<std::string_view, Cost>> hotFunctions(byFunction.begin(), byFunction.end());
			std::ranges::sort(hotFunctions, std::greater{}, [](const auto& row) { return row.second.cycles; });
			err << "\n      cycles      share  instructions  function\n";
			for (const auto& [name, cost] : hotFunctions)
				err << std::format("{:>12}  {:>8.2f}%  {:>12}  {}\n", cost.cycles, share(cost.cycles), cost.count, name);

			struct HotLine { u32 fileId; u32 line; Cost cost; };
			std::vector<HotLine> hot;
			for (const auto& [location, cost] : lines)
				if (cost.count != 0)
					hot.push_back({ location.first, location.second, cost });
			std::ranges::sort(hot, std::greater{}, [](const HotLine& row) { return row.cost.cycles; });
			err << "\n      cycles      share         count  line\n";
			for (const auto& row : hot)
				err << std::format("{:>12}  {:>8.2f}%  {:>12}  {}:{}\n", row.cost.cycles, share(row.cost.cycles), row.cost.count,
					info.fileName(row.fileId), row.line);
		}
	}

	int runMachine(const Program& program, const MachineProfile& machine, const DebugInfo* profileInfo,
		const std::filesystem::path& diskImage, const std::vector<PortAttachment>& ports, vm::ProgramArguments arguments,
		const std::filesystem::path& hostDirectory, HostServices services, HostIo host, const MachineOptions& options)
	{
		// What the host says about the run, and the program's debug log: to the diagnostics stream, or to --log.
		HostLog log{ *services.diagnostics };
		if (!options.logFile.empty() && !log.openFile(options.logFile))
		{
			*services.diagnostics << "Cannot write the log " << options.logFile.string() << '\n';
			return 1;
		}

		// The host may refuse to reserve a large machine (a 2 GiB RAM and a 1 GiB VRAM are three committed
		// gigabytes): that is the run's error to report, not an exception out of it.
		std::unique_ptr<CeresVM> machineHolder;
		try
		{
			machineHolder = std::make_unique<CeresVM>(machine.ramBytes, machine.vramBytes);
		}
		catch (const std::bad_alloc&)
		{
			log.error(std::format("The host cannot reserve {} bytes of RAM and {} of VRAM for the machine", machine.ramBytes, machine.vramBytes));
			return 1;
		}
		CeresVM& vm = *machineHolder;
		vm.engine().setStrictMmio(options.strictMmio);
		vm.io().scheduler().setClockHz(machine.cpuClockHz);
		vm.setProgramArguments(std::move(arguments));
		// A reset starts the program again from its entry point (CeresVM::restartIfRequested): vm.run()
		// does that on its own, and the windowed loop below between two frames.
		SystemControlDevice control{[&vm] { vm.shutdown(); }, [&vm] { vm.requestReset(); }};
		control.setFeaturesCallback([&vm](u32 features)
		{
			vm.engine().setDivisionFaults((features & SystemControlDevice::FeatureDivisionFault) != 0);
			vm.engine().setIeeeDivide((features & SystemControlDevice::FeatureIeeeDivide) != 0);
		});
		control.setStackLimitHandlers([&vm] { return vm.engine().stackLimit(); },
			[&vm](u32 address) { vm.engine().setProgramStackLimit(address); });
		control.setFaultInfoHandlers([&vm] { return vm.engine().faultAddress(); }, [&vm] { return vm.engine().faultAccess(); }, [&vm] { return vm.engine().faultReason(); });
		control.setArgumentHandler([&vm](u32 which) { return vm.argumentInfo(which); });
		control.setProfileId(static_cast<u32>(machine.id));
		auto terminal = std::make_shared<TerminalDevice>();
		TimerDevice timer;
		DmaController dma;
		DiskDevice disk;
		// Shared, like the terminal: the console's reader thread may outlive this function and must find the
		// device there, detached, rather than gone.
		auto keyboard = std::make_shared<KeyboardDevice>();
		MouseDevice mouse;
		GamepadDevice gamepad;
		AudioDevice audio;
		PeripheralDevice peripherals;
		HostFsDevice hostFs;
		BlitterDevice blitter;
		DebugLogDevice debugLog;
		debugLog.setSink([&log](u32 level, std::string_view line) { log.write(level, line); });
		control.attachTo(vm.io());
		terminal->attachTo(vm.io());
		debugLog.attachTo(vm.io());
		GpuDevice gpu;
		gpu.configure(GpuDevice::Config{ .gpuClockHz = machine.gpuClockHz, .maxLevel = machine.maxVideo,
			.maxWidth = machine.maxWidth, .maxHeight = machine.maxHeight, .refresh = options.refresh });
		gpu.attachTo(vm.io());
		// The terminal draws in the GPU's text plane (plan/v2 SPEC 8). It may outlive this function (a reader thread
		// holds it), so it lets go of the GPU before the GPU goes.
		terminal->setScreen(&gpu);
		struct DetachGpu
		{
			GpuDevice& device;
			TerminalDevice& terminal;
			CeresVM& machine;
			~DetachGpu()
			{
				terminal.setScreen(nullptr);
				device.detachFrom(machine.io());
			}
		} detachGpu{ gpu, *terminal, vm };
		if (options.rtc)
			timer.setRtcStart(*options.rtc);
		timer.attachTo(vm.io());
		dma.attachTo(vm.io());
		keyboard->attachTo(vm.io());
		struct DetachKeyboard
		{
			KeyboardDevice& device;
			CeresVM& machine;
			~DetachKeyboard() { device.detachFrom(machine.io()); }
		} detachKeyboard{*keyboard, vm};
		mouse.attachTo(vm.io());
		gamepad.attachTo(vm.io());
		audio.attachTo(vm.io());
		// The program's output never reaches the host's terminal (plan/v2 SPEC 1.4): a copy goes to --transcript when
		// one is asked for.
		HeadlessOutput headless;
		if (const std::string error = headless.open(options.transcript, options.screenLog); !error.empty())
		{
			log.error(error);
			return 1;
		}
		terminal->setOutputSink([&headless](u8 byte) { headless.transcriptByte(byte, false); });
		terminal->setErrorSink([&headless](u8 byte) { headless.transcriptByte(byte, true); });
		struct ForgetSinks
		{
			TerminalDevice& device;
			~ForgetSinks() { device.clearOutputSink(); device.setErrorSink({}); }
		} forgetSinks{ *terminal };
		if (!diskImage.empty() && !disk.open(diskImage))
		{
			log.error("Failed to open disk image: " + diskImage.string());
			return 1;
		}
		disk.attachTo(vm.io());
		peripherals.attachTo(vm.io());
		if (!hostDirectory.empty() && !hostFs.setRoot(hostDirectory))
		{
			log.error("--host-dir: not a directory: " + hostDirectory.string());
			return 1;
		}
		hostFs.attachTo(vm.io());
		blitter.attachTo(vm.io());
		for (const PortAttachment& port : ports)
		{
			std::string error;
			if (!peripherals.attachFile(port.port, port.path, port.cartridge ? PeripheralDevice::Kind::Cartridge : PeripheralDevice::Kind::Storage, &error))
			{
				log.error("Failed to plug in " + port.path.string() + ": " + error);
				return 1;
			}
		}

		// Every input the host gives the machine waits here and goes in between two slices, stamped with its
		// cycle (input_journal.h). Shared, like the terminal: the stdin reader may outlive this function, and
		// posts into a hub that no longer pokes anything.
		const auto input = std::make_shared<InputHub>(&vm.interrupts());
		struct DisconnectInput
		{
			std::shared_ptr<InputHub> hub;
			~DisconnectInput() { hub->disconnect(); }
		} disconnectInput{input};
		std::ofstream recording;
		if (!options.record.empty())
		{
			recording.open(options.record, std::ios::binary);
			if (!recording)
			{
				log.error("Cannot write the input recording " + options.record.string());
				return 1;
			}
			input->record(recording);
		}
		if (!options.replay.empty())
		{
			std::ifstream file(options.replay, std::ios::binary);
			auto events = file ? readInputRecording(file) : std::unexpected(std::string("it cannot be opened"));
			if (!events)
			{
				log.error("Cannot replay " + options.replay.string() + ": " + events.error());
				return 1;
			}
			input->replay(std::move(*events));
		}

		// The host's speakers, if it has any, play what the audio device is asked for. Taken off
		// again before the device goes away, since the sound is made on another thread.
		struct AudioHost
		{
			AudioOutput* output;
			~AudioHost() { if (output) output->detachAudio(); }
		} audioHost{host.audio};
		if (host.audio)
			host.audio->attachAudio(audio);

		// A file dropped on the window is plugged into the first free port: a cartridge when it is called *.cart,
		// a storage stick otherwise. Removed again before the device goes away.
		struct DropHandler
		{
			HostInput* input;
			~DropHandler() { if (input) input->setFileDropHandler({}); }
		} dropHandler{host.input};
		const auto plugIn = [&peripherals, &log](const std::filesystem::path& path)
		{
			const bool cartridge = path.extension() == ".cart";
			std::string error;
			const int port = peripherals.attachToFreePort(path, cartridge ? PeripheralDevice::Kind::Cartridge : PeripheralDevice::Kind::Storage, &error);
			if (port < 0)
				log.error("Could not plug in " + path.string() + ": " + error);
		};
		if (host.input)
			host.input->setFileDropHandler([input](const std::filesystem::path& path)
			{
				const std::u8string utf8 = path.u8string();
				input->post(InputEvent{ .kind = InputEvent::Kind::FileDrop, .data = std::string(utf8.begin(), utf8.end()) });
			});

		// What is typed - in the window, or by --keys - goes to the terminal as well as the keyboard: through its line
		// discipline, or at once in raw mode (plan/v2 SPEC 8.3). The host's own stdin is never read.
		keyboard->setKeystrokeSink([terminal](u32 keystroke) { terminal->typeKeystroke(keystroke); });

		if (auto loaded = vm.loadProgram(program); !loaded)
		{
			terminal->detachFrom(vm.io());
			log.error("Failed to load program: " + loaded.error());
			return 1;
		}
		if (profileInfo)
			vm.engine().enableProfiling();

		// One loop for every run. Between two slices of the machine's time: the host's input goes in (the injection
		// point), a window pumps its events and presents its frames, and a machine that keeps pace with the host
		// waits for it.
		if (auto powered = vm.powerOn(); !powered)
		{
			terminal->detachFrom(vm.io());
			log.error("Failed to power on: " + powered.error());
			return 1;
		}

		using HostClock = std::chrono::steady_clock;
		Pacer pacer{ options.speed.value_or(Speed::unlimited()), vm.io().scheduler().clockHz() };
		const u64 sliceCycles = std::max<u64>(1, vm.io().scheduler().clockHz() / 1000);   // a millisecond of machine time
		InputTargets targets{ *terminal, *keyboard, mouse, gamepad, plugIn };

		// The screen. A machine with a window shows it from the start, at the GPU's resolution; one that cannot open
		// it runs without, unless the window was asked for by name.
		if (host.video)
		{
			host.video->setFullscreen(options.fullscreen);
			if (!host.video->openWindow(gpu.width(), gpu.height()))
			{
				if (options.requireWindow)
				{
					log.error("Failed to open a window.");
					return 1;
				}
				host.video = nullptr;
				host.input = nullptr;
			}
		}

		// Scripted input (plan/v2 SPEC 10): --type is typed as the machine starts, --keys at the instants it names.
		// Without a window nobody else can type, so the terminal's input ends once they are done - at once when there
		// are none. A replay has its own.
		if (!input->replaying())
		{
			std::vector<InputEvent> script;
			if (!options.keysFile.empty())
			{
				const auto text = readFile(options.keysFile);
				auto events = text ? parseKeyScript(*text, vm.io().scheduler().clockHz()) : std::unexpected(std::string("it cannot be read"));
				if (!events)
				{
					log.error("--keys " + options.keysFile.string() + ": " + events.error());
					return 1;
				}
				script = std::move(*events);
			}
			if (!options.typeFile.empty())
			{
				const auto text = readFile(options.typeFile);
				if (!text)
				{
					log.error("--type: cannot read " + options.typeFile.string());
					return 1;
				}
				if (!text->empty())
					input->post(InputEvent{ .kind = InputEvent::Kind::Typed, .data = *text });
			}
			if (!host.windowed())
			{
				if (script.empty())
					input->post(InputEvent{ .kind = InputEvent::Kind::TerminalClose });
				else
					script.push_back(InputEvent{ .cycle = script.back().cycle + sliceCycles, .kind = InputEvent::Kind::TerminalClose });
			}
			input->script(std::move(script));
		}
		if (!options.framesDir.empty())
		{
			std::error_code error;
			std::filesystem::create_directories(options.framesDir, error);
			if (error)
			{
				log.error("--frames: cannot make the directory " + options.framesDir.string());
				return 1;
			}
		}

		// Every vertical blank (plan/v2 F5.5): the window gets the frame the scanout composes - no more than one every
		// few milliseconds of the host's, so a machine running flat out does not spend its time drawing - and a
		// Present also goes to --frames as a PNG, composed at the blank itself so it is the same on every run.
		const HostStatus runningStatus{ std::string(profileName(machine.id)), 0.0, std::nullopt };
		constexpr auto MinPresentGap = std::chrono::milliseconds(8);
		constexpr auto StatusEvery = std::chrono::milliseconds(250);
		video::VideoFrame frame;
		u64 framesWritten = 0;
		bool framesFailed = false;
		HostClock::time_point lastShown{};
		HostClock::time_point lastStatus{};
		gpu.setVblankObserver([&](bool presented)
		{
			bool composed = false;
			if (presented && headless.hasScreenLog())
				headless.screen(gpu.screenText(), false);
			if (presented && !options.framesDir.empty() && !framesFailed)
			{
				gpu.compose(frame);
				composed = true;
				const auto path = options.framesDir / std::format("frame_{:06}.png", framesWritten++);
				if (!writePng(path, frame))
				{
					framesFailed = true;
					log.error("--frames: cannot write " + path.string());
				}
			}
			if (!host.video)
				return;
			const HostClock::time_point now = HostClock::now();
			if (now - lastShown >= MinPresentGap)
			{
				if (!composed)
					gpu.compose(frame);
				host.video->present(frame);
				lastShown = now;
			}
			if (now - lastStatus >= StatusEvery)
			{
				HostStatus status = runningStatus;
				status.speed = pacer.effectiveSpeed();
				host.video->setStatus(status);
				lastStatus = now;
			}
		});
		struct ForgetObserver
		{
			GpuDevice& device;
			~ForgetObserver() { device.setVblankObserver({}); }
		} forgetObserver{ gpu };
		bool hostQuit = false;

		for (;;)
		{
			if (!vm.isPoweredOn())
			{
				if (!vm.restartIfRequested())
					break;
				input->restarted();
			}
			if (host.input && !host.input->pump(*input))
			{
				input->quit(vm.engine().cycles());
				hostQuit = true;
				break;
			}
			if (!input->inject(vm.engine().cycles(), targets))
				break;   // a replay reached the moment its recording's host quit

			// Without --speed a machine keeps real time while its window is open, and runs flat out without one.
			if (!options.speed)
				pacer.setSpeed(host.video && host.video->windowOpen() ? Speed::realtime() : Speed::unlimited());

			// Ahead of the host: wait a little and look again, rather than run on. Otherwise a slice, up to the next
			// millisecond of machine time. A halted machine with nothing scheduled waits for the host inside its
			// step, and the host's input only goes in between slices, so a halt ends the slice.
			if (!pacer.pace(vm.engine().cycles()))
			{
				const u64 end = vm.engine().cycles() + sliceCycles;
				while (vm.isPoweredOn())
				{
					vm.engine().step();
					if (vm.engine().cycles() >= end || vm.engine().isHalted())
						break;
				}
			}
		}

		// An exception nobody handled: the report goes to the host's log and is painted over the screen (plan/v2 F5.6).
		if (const auto unhandled = describeUnhandledException(vm))
		{
			log.error(*unhandled);
			terminal->showFault(*unhandled);
		}
		if (profileInfo)
			printProfile(vm, *profileInfo, *services.diagnostics);
		headless.screen(gpu.screenText(), true);
		headless.finish();

		// The program is done, but the window stays with its last frame and says how it ended, until it is closed or
		// a key is pressed (plan/v2 D22) - unless --exit-on-halt, or the host is the one that ended the run.
		if (host.video && host.video->windowOpen() && !hostQuit && !options.exitOnHalt)
		{
			gpu.setVblankObserver({});
			gpu.compose(frame);   // the screen as the program left it, with a fault's report if it had one
			host.video->present(frame);
			HostStatus ended = runningStatus;
			ended.exitCode = control.exitCode();
			host.video->setStatus(ended);
			struct AnyKey final : InputSink
			{
				bool pressed = false;
				void key(u32, bool down) override { pressed = pressed || down; }
				void text(std::string_view) override {}
				void mouse(i32, i32, u8, i8) override {}
				void gamepad(u16, i16, i16, i16, i16, u16, u16) override {}
			} anyKey;
			while (host.input == nullptr || host.input->pump(anyKey))
			{
				if (anyKey.pressed || host.input == nullptr)
					break;
				std::this_thread::sleep_for(std::chrono::milliseconds(16));
			}
		}
		terminal->detachFrom(vm.io());
		return control.exitCode();
	}
}
