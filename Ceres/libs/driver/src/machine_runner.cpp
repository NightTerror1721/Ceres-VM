#include "machine_runner.h"

#include <ceres/driver/driver.h>
#include <ceres/devices/devices.h>
#include <ceres/devices/storage_devices.h>
#include <ceres/devices/input_devices.h>
#include <ceres/devices/display_device.h>
#include <ceres/devices/audio_device.h>
#include <ceres/vm/ceresvm.h>

#include <atomic>
#include <chrono>
#include <format>
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
		std::string startupError;

		Impl(const MachineConfig& config, const MachineHost& host) :
			vm(config.memorySize),
			control([this] { vm.shutdown(); }, [this] { vm.shutdown(); })
		{
			control.setFeaturesCallback([this](u32 features)
			{
				vm.engine().setDivisionFaults((features & SystemControlDevice::FeatureDivisionFault) != 0);
			});
			control.attachTo(vm.io());
			terminal.attachTo(vm.io());
			timer.attachTo(vm.io());
			dma.attachTo(vm.io());
			disk.attachTo(vm.io());
			framebuffer.attachTo(vm.io());
			keyboard.attachTo(vm.io());
			mouse.attachTo(vm.io());
			display.attachTo(vm.io());
			gamepad.attachTo(vm.io());
			audio.attachTo(vm.io());

			if (host.terminalOutput)
				terminal.setOutputSink([sink = host.terminalOutput](u8 byte)
				{
					sink(std::span<const u8>(&byte, 1));
				});
			if (host.framePresented)
				framebuffer.setPresentSink(std::move(host.framePresented));
			if (!config.diskImage.empty() && !disk.open(config.diskImage))
				startupError = "Failed to open disk image: " + config.diskImage.string();
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
		const std::filesystem::path& diskImage, HostServices services, HostBackend* backend)
	{
		CeresVM vm{memorySize};
		SystemControlDevice control{[&vm] { vm.shutdown(); }, [&vm] { vm.shutdown(); }};
		control.setFeaturesCallback([&vm](u32 features)
		{
			vm.engine().setDivisionFaults((features & SystemControlDevice::FeatureDivisionFault) != 0);
		});
		auto terminal = std::make_shared<TerminalDevice>();
		TimerDevice timer;
		DmaController dma;
		DiskDevice disk;
		FramebufferDevice framebuffer;
		KeyboardDevice keyboard;
		MouseDevice mouse;
		DisplayDevice display;
		GamepadDevice gamepad;
		AudioDevice audio;
		control.attachTo(vm.io());
		terminal->attachTo(vm.io());
		timer.attachTo(vm.io());
		dma.attachTo(vm.io());
		keyboard.attachTo(vm.io());
		mouse.attachTo(vm.io());
		display.attachTo(vm.io());
		gamepad.attachTo(vm.io());
		audio.attachTo(vm.io());
		terminal->setOutputSink([out = services.output](u8 byte) { out->put(static_cast<char>(byte)); out->flush(); });
		framebuffer.setPresentSink([out = services.output](std::string_view frame) { *out << frame; out->flush(); });
		if (!diskImage.empty() && !disk.open(diskImage))
		{
			*services.diagnostics << "Failed to open disk image: " << diskImage.string() << '\n';
			return 1;
		}
		disk.attachTo(vm.io());
		framebuffer.attachTo(vm.io());

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

		if (services.input != nullptr)
		{
			// A blocked console read cannot be cancelled portably. Shared ownership prevents a stale
			// reader from touching a destroyed device; detachFrom clears its VM connection on return.
			//
			// The ring holds 64 bytes and drops what does not fit, which is right for a keystroke
			// source but wrong for a pipe: a program that is busy for a moment would lose the tail of
			// a piped file. So this reader is the flow control - it holds the byte back until the
			// program has taken enough. It is the ring's only producer, so room seen here cannot be
			// taken by anyone else before the push.
			std::thread([input = services.input, terminal, machineDone]
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
			}).detach();
		}

		if (auto loaded = vm.loadProgram(program); !loaded)
		{
			terminal->detachFrom(vm.io());
			*services.diagnostics << "Failed to load program: " << loaded.error() << '\n';
			return 1;
		}
		if (profileInfo)
			vm.engine().enableProfiling();

		if (backend)
		{
			// A windowed host needs to pump its events and present its frame between slices of
			// instructions, so the machine cannot simply run to completion.
			if (auto powered = vm.powerOn(); !powered)
			{
				terminal->detachFrom(vm.io());
				*services.diagnostics << "Failed to power on: " << powered.error() << '\n';
				return 1;
			}

			while (vm.isPoweredOn() && backend->pump(keyboard, mouse, gamepad))
			{
				const u64 slice = backend->instructionsPerFrame();
				for (u64 i = 0; i < slice && vm.isPoweredOn(); ++i)
					vm.engine().step();
				backend->present(display);
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
