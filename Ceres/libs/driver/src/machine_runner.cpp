#include "machine_runner.h"
#include "console_input.h"

#include <ceres/driver/driver.h>
#include <ceres/driver/pacer.h>
#include <ceres/devices/devices.h>
#include <ceres/devices/storage/disk.h>
#include <ceres/devices/video/text_framebuffer.h>
#include <ceres/devices/input/gamepad.h>
#include <ceres/devices/input/keyboard.h>
#include <ceres/devices/input/mouse.h>
#include <ceres/devices/video/display.h>
#include <ceres/devices/audio/audio.h>
#include <ceres/devices/storage/peripherals.h>
#include <ceres/devices/storage/host_fs.h>
#include <ceres/devices/video/blitter.h>
#include <ceres/vm/ceresvm.h>

#include <atomic>
#include <chrono>
#include <format>
#include <iostream>
#include <map>
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
		FramebufferDevice framebuffer;
		KeyboardDevice keyboard;
		MouseDevice mouse;
		DisplayDevice display;
		GamepadDevice gamepad;
		AudioDevice audio;
		PeripheralDevice peripherals;
		HostFsDevice hostFs;
		BlitterDevice blitter;
		std::string startupError;

		Impl(const MachineConfig& config, const MachineHost& host) :
			vm(config.memorySize),
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
			vm.setProgramArguments(vm::ProgramArguments{ config.arguments, config.environment });
			control.attachTo(vm.io());
			terminal.attachTo(vm.io());
			terminal.setModeHandler([](u32 requested)
			{
				return (requested & TerminalDevice::ModeRaw) ? (TerminalDevice::ModeRaw | TerminalDevice::ModeKeystrokes) : 0u;
			});
			timer.attachTo(vm.io());
			dma.attachTo(vm.io());
			disk.attachTo(vm.io());
			framebuffer.attachTo(vm.io());
			keyboard.attachTo(vm.io());
			mouse.attachTo(vm.io());
			display.attachTo(vm.io());
			gamepad.attachTo(vm.io());
			audio.attachTo(vm.io());
			peripherals.attachTo(vm.io());
			hostFs.attachTo(vm.io());
			blitter.attachTo(vm.io());
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
			if (host.framePresented)
				framebuffer.setPresentSink(std::move(host.framePresented));
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
			framebuffer.detachFrom(vm.io());
			disk.detachFrom(vm.io());
			timer.detachFrom(vm.io());
			dma.detachFrom(vm.io());
			keyboard.detachFrom(vm.io());
			mouse.detachFrom(vm.io());
			display.detachFrom(vm.io());
			gamepad.detachFrom(vm.io());
			audio.detachFrom(vm.io());
			peripherals.detachFrom(vm.io());
			hostFs.detachFrom(vm.io());
			blitter.detachFrom(vm.io());
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
		void printProfile(CeresVM& vm, const DebugInfo& info, std::ostream& err)
		{
			struct HotLine { u32 fileId; u32 line; u64 count; };
			std::map<std::pair<u32, u32>, u64> lines;
			u64 total = 0;
			const auto counts = vm.engine().executionCounts();
			for (const auto& entry : info.lines())
			{
				if (entry.address < vm.engine().textStart())
					continue;
				const usize index = (entry.address - vm.engine().textStart()) / Instruction::Size;
				if (index < counts.size())
				{
					lines[{entry.expansionFileId, entry.expansionLine}] += counts[index];
					total += counts[index];
				}
			}

			std::vector<HotLine> hot;
			for (const auto& [location, count] : lines)
				if (count != 0)
					hot.push_back({location.first, location.second, count});
			std::ranges::sort(hot, {}, &HotLine::count);
			std::ranges::reverse(hot);

			err << std::format("\n{} instructions executed\n\n     count      share  line\n", total);
			for (const auto& row : hot)
				err << std::format("{:>10}  {:>8.2f}%  {}:{}\n", row.count,
				total ? 100.0 * row.count / total : 0.0, info.fileName(row.fileId), row.line);
		}
	}

	int runMachine(const Program& program, usize memorySize, const DebugInfo* profileInfo,
		const std::filesystem::path& diskImage, const std::vector<PortAttachment>& ports, vm::ProgramArguments arguments,
		const std::filesystem::path& hostDirectory, HostServices services, HostBackend* backend, const MachineOptions& options)
	{
		CeresVM vm{memorySize};
		vm.engine().setStrictMmio(options.strictMmio);
		if (options.cpuClockHz)
			vm.io().scheduler().setClockHz(*options.cpuClockHz);
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
		auto terminal = std::make_shared<TerminalDevice>();
		TimerDevice timer;
		DmaController dma;
		DiskDevice disk;
		FramebufferDevice framebuffer;
		// Shared, like the terminal: the console's reader thread may outlive this function and must find the
		// device there, detached, rather than gone.
		auto keyboard = std::make_shared<KeyboardDevice>();
		MouseDevice mouse;
		DisplayDevice display;
		GamepadDevice gamepad;
		AudioDevice audio;
		PeripheralDevice peripherals;
		HostFsDevice hostFs;
		BlitterDevice blitter;
		control.attachTo(vm.io());
		terminal->attachTo(vm.io());
		framebuffer.setWindowHost(backend != nullptr && backend->showsText());
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
		display.attachTo(vm.io());
		gamepad.attachTo(vm.io());
		audio.attachTo(vm.io());
		terminal->setOutputSink([out = services.output](u8 byte) { out->put(static_cast<char>(byte)); out->flush(); });
		terminal->setErrorSink([err = services.diagnostics](u8 byte) { err->put(static_cast<char>(byte)); err->flush(); });
		framebuffer.setPresentSink([out = services.output](std::string_view frame) { *out << frame; out->flush(); });
		if (!diskImage.empty() && !disk.open(diskImage))
		{
			*services.diagnostics << "Failed to open disk image: " << diskImage.string() << '\n';
			return 1;
		}
		disk.attachTo(vm.io());
		framebuffer.attachTo(vm.io());
		peripherals.attachTo(vm.io());
		if (!hostDirectory.empty() && !hostFs.setRoot(hostDirectory))
		{
			*services.diagnostics << "--host-dir: not a directory: " << hostDirectory.string() << '\n';
			return 1;
		}
		hostFs.attachTo(vm.io());
		blitter.attachTo(vm.io());
		for (const PortAttachment& port : ports)
		{
			std::string error;
			if (!peripherals.attachFile(port.port, port.path, port.cartridge ? PeripheralDevice::Kind::Cartridge : PeripheralDevice::Kind::Storage, &error))
			{
				*services.diagnostics << "Failed to plug in " << port.path.string() << ": " << error << '\n';
				return 1;
			}
		}

		// The reader of an input stream other than std::cin, joined before this returns: the stream belongs to
		// the caller and may be gone once it does. Declared ahead of doneOnExit so that it is destroyed after it
		// and the reader has already been told the machine is done.
		struct JoinOnExit
		{
			std::thread thread;
			~JoinOnExit() { if (thread.joinable()) thread.join(); }
		} streamReader;

		// Raised when the machine is done, so a reader parked on a full ring stops waiting for a
		// program that will never read it.
		const auto machineDone = std::make_shared<std::atomic<bool>>(false);
		struct DoneOnExit
		{
			std::shared_ptr<std::atomic<bool>> flag;
			~DoneOnExit() { flag->store(true, std::memory_order_release); }
		} doneOnExit{machineDone};

		// The host's speakers, if it has any, play what the audio device is asked for. Taken off
		// again before the device goes away, since the sound is made on another thread.
		struct AudioHost
		{
			HostBackend* backend;
			~AudioHost() { if (backend) backend->detachAudio(); }
		} audioHost{backend};
		if (backend)
			backend->attachAudio(audio);

		// A file dropped on the window is plugged into the first free port: a cartridge when it is called *.cart,
		// a storage stick otherwise. Removed again before the device goes away.
		struct DropHandler
		{
			HostBackend* backend;
			~DropHandler() { if (backend) backend->setFileDropHandler({}); }
		} dropHandler{backend};
		if (backend)
			backend->setFileDropHandler([&peripherals, diagnostics = services.diagnostics](const std::filesystem::path& path)
			{
				const bool cartridge = path.extension() == ".cart";
				std::string error;
				const int port = peripherals.attachToFreePort(path, cartridge ? PeripheralDevice::Kind::Cartridge : PeripheralDevice::Kind::Storage, &error);
				if (port < 0)
					*diagnostics << "Could not plug in " << path.string() << ": " << error << '\n';
			});

		// When standard input is a console, it can give the program more than lines. The program asks through
		// the terminal's ModeRegister for keys as they are pressed; a window already has them.
		std::shared_ptr<ConsoleInput> console;
		if (services.input == &std::cin)
			console = ConsoleInput::open();
		struct RestoreConsole
		{
			std::shared_ptr<ConsoleInput> console;
			~RestoreConsole() { if (console) console->restore(); }
		} restoreConsole{console};
		terminal->setModeHandler([console, windowed = backend != nullptr](u32 requested) -> u32
		{
			const bool raw = (requested & TerminalDevice::ModeRaw) != 0;
			const u32 granted = raw ? (TerminalDevice::ModeRaw | TerminalDevice::ModeKeystrokes) : 0u;
			if (windowed)
				return granted;   // the window's keyboard is already the source; the console stays as it is
			if (!console)
				return 0;         // input from a pipe or a file: nothing to switch
			console->setRaw(raw);
			return granted;
		});
		// A window's keystrokes also go to the terminal as bytes - unless the program asked for raw keys, in
		// which case it reads them from the keyboard and the bytes would only pile up unread.
		if (backend)
			keyboard->setKeystrokeSink([terminal](u32 keystroke)
			{
				if (!terminal->rawRequested())
					terminal->pushInput(keystrokeToTerminalBytes(keystroke));
			});

		if (console)
		{
			// The ring holds 64 bytes, so the cooked path is flow-controlled exactly as the stream one below.
			std::thread([console, terminal, keyboard, machineDone]
			{
				ConsoleInput::Sink sink;
				sink.bytes = [&](std::span<const u8> bytes)
				{
					for (const u8 byte : bytes)
					{
						while (terminal->availableBytes() >= TerminalDevice::InputBufferCapacity - 1)
						{
							if (machineDone->load(std::memory_order_acquire))
								return;
							std::this_thread::sleep_for(std::chrono::microseconds(200));
						}
						terminal->pushInput(static_cast<char>(byte));
					}
				};
				sink.key = [&](u32 code, bool pressed) { keyboard->pushKey(code, pressed); };
				sink.text = [&](u32 codePoint) { keyboard->pushText(codePoint); };
				sink.endOfInput = [&] { terminal->closeInput(); };
				sink.stopped = [&] { return machineDone->load(std::memory_order_acquire); };
				console->run(sink);
			}).detach();
		}
		else if (services.input != nullptr)
		{
			// A blocked read of std::cin cannot be cancelled portably, so that reader is left behind
			// when the machine is done; std::cin outlives it. Any other stream is the caller's and ends
			// (a string, a file), so its reader is joined before returning (streamReader). Shared
			// ownership prevents a stale reader from touching a destroyed device; detachFrom clears
			// its VM connection on return.
			//
			// The ring holds 64 bytes and drops what does not fit, which is right for a keystroke
			// source but wrong for a pipe: a program that is busy for a moment would lose the tail of
			// a piped file. So this reader is the flow control - it holds the byte back until the
			// program has taken enough. It is the ring's only producer, so room seen here cannot be
			// taken by anyone else before the push.
			auto reader = [input = services.input, terminal, machineDone]
			{
				char c;
				while (input->get(c))
				{
					while (terminal->availableBytes() >= TerminalDevice::InputBufferCapacity - 1)
					{
						if (machineDone->load(std::memory_order_acquire))
							return;
						std::this_thread::sleep_for(std::chrono::microseconds(200));
					}
					terminal->pushInput(c);
				}
				// The stream ended (a pipe ran dry, or the user closed stdin): say so, so a program
				// waiting for more can stop waiting.
				terminal->closeInput();
			};
			if (services.input == &std::cin)
				std::thread(std::move(reader)).detach();
			else
				streamReader.thread = std::thread(std::move(reader));
		}

		if (auto loaded = vm.loadProgram(program); !loaded)
		{
			terminal->detachFrom(vm.io());
			*services.diagnostics << "Failed to load program: " << loaded.error() << '\n';
			return 1;
		}
		if (profileInfo)
			vm.engine().enableProfiling();

		// A windowed host pumps its events and presents its frames between slices of the machine's time, and a
		// machine that keeps pace with the host waits between them: neither can simply run to completion.
		if (backend || (options.speed && !options.speed->max))
		{
			if (auto powered = vm.powerOn(); !powered)
			{
				terminal->detachFrom(vm.io());
				*services.diagnostics << "Failed to power on: " << powered.error() << '\n';
				return 1;
			}

			using HostClock = std::chrono::steady_clock;
			constexpr auto PresentEvery = std::chrono::milliseconds(16);   // the window's own redraw, about 60 a second
			Pacer pacer{ options.speed.value_or(Speed::unlimited()), vm.io().scheduler().clockHz() };
			const u64 sliceCycles = std::max<u64>(1, vm.io().scheduler().clockHz() / 1000);   // a millisecond of machine time
			HostClock::time_point lastPresent{};
			u64 displayPresents = display.presentCount();

			while ((vm.isPoweredOn() || vm.restartIfRequested()) && (!backend || backend->pump(*keyboard, mouse, gamepad)))
			{
				// Without --speed a machine keeps real time while its window is open, and runs flat out without one.
				if (!options.speed)
					pacer.setSpeed(backend && backend->windowOpen() ? Speed::realtime() : Speed::unlimited());

				// Ahead of the host: wait a little and pump again, rather than run on. Otherwise a slice, up to the next
				// millisecond of machine time. A halted machine with nothing scheduled waits for the host inside its
				// step, and the host's keys only reach it through pump(), so a halt ends the slice; so does a frame of
				// text presented for the window, so each one is shown.
				if (!pacer.pace(vm.engine().cycles()))
				{
					const u64 end = vm.engine().cycles() + sliceCycles;
					while (vm.isPoweredOn())
					{
						vm.engine().step();
						if (vm.engine().cycles() >= end || vm.engine().isHalted() || framebuffer.hasWindowFrame())
							break;
					}
				}

				if (!backend)
					continue;
				const HostClock::time_point now = HostClock::now();
				if (display.presentCount() != displayPresents || now - lastPresent >= PresentEvery)
				{
					displayPresents = display.presentCount();
					lastPresent = now;
					backend->present(display);
					backend->reportSpeed(pacer.effectiveSpeed());
				}

				// A frame of the text framebuffer that the program presented for the window. Taken here, between
				// slices, rather than drawn from inside the instruction that presented it.
				FramebufferDevice::Frame frame;
				if (framebuffer.takeWindowFrame(frame) && !backend->presentText(frame))
					framebuffer.fallBackToTerminal();
			}
		}
		else if (auto result = vm.run(); !result)
		{
			terminal->detachFrom(vm.io());
			*services.diagnostics << "Failed to run program: " << result.error() << '\n';
			return 1;
		}

		if (profileInfo)
			printProfile(vm, *profileInfo, *services.diagnostics);
		terminal->detachFrom(vm.io());
		return control.exitCode();
	}
}
