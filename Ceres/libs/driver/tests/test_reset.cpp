// The system control device's reset command (2) from a running program down to `ceres run`: the machine
// starts the program again from its entry point, with its image as it was loaded, instead of stopping.

#include "framework.h"
#include <ceres/driver/command.h>
#include <ceres/driver/driver.h>
#include <ceres/driver/host_backend.h>
#include <ceres/devices/devices.h>
#include <ceres/vm/ceresvm.h>

#include <filesystem>
#include <fstream>
#include <memory>
#include <sstream>
#include <string>

using namespace ceres;
using namespace ceres::driver;
using namespace ceres::testing;

namespace
{
	// What a program printed under `ceres run`, or "exit N: <diagnostics>" when it did not end with 0.
	std::string run(const char* name, const std::string& source)
	{
		const auto path = std::filesystem::temp_directory_path() / name;
		{
			std::ofstream file(path, std::ios::binary | std::ios::trunc);
			file << source;
		}
		std::istringstream input;
		std::ostringstream output;
		std::ostringstream diagnostics;
		RunCommand command{ .input = path };
		const int result = execute(command, { &input, &output, &diagnostics });
		std::filesystem::remove(path);
		return result == 0 ? output.str() : "exit " + std::to_string(result) + ": " + diagnostics.str();
	}

	// A host with a window that nothing happens in: what the windowed loop does between frames.
	class QuietWindow final : public HostBackend
	{
	public:
		bool pump(devices::KeyboardDevice&, devices::MouseDevice&, devices::GamepadDevice&) override { return true; }
		void present(const devices::DisplayDevice&) override {}
	};

	// The first run marks the word just above the image (which a reset does not reload), prints '1' and
	// the digit in `counter`, changes `counter`, and resets. The second run finds the mark, prints '2' and
	// `counter` again - reloaded, so 7 once more - and shuts down. An `X` would mean the instruction after
	// the reset ran.
	const char* ResetOnce =
		"@data\n"
		"    let counter: u32 = 7\n"
		"@text\n"
		"global main:\n"
		"    la   r1, __heap_start\n"
		"    ldr  r2, [r1 + 0]\n"
		"    la   r3, 0x52534554\n"
		"    la   r4, 0xFF000004\n"
		"    la   r6, counter\n"
		"    ldr  r7, [r6 + 0]\n"
		"    add  r7, r7, 48\n"
		"    ifeq r2, r3, .second\n"
		"    str  [r1 + 0], r3\n"
		"    li   r5, 49\n"
		"    str  [r4 + 0], r5\n"
		"    str  [r4 + 0], r7\n"
		"    li   r7, 9\n"
		"    str  [r6 + 0], r7\n"
		"    li   r0, 2\n"
		"    la   r13, 0xFFFF0000\n"
		"    str  [r13 + 0], r0\n"
		"    li   r5, 88\n"
		"    str  [r4 + 0], r5\n"
		"    halt\n"
		".second:\n"
		"    li   r5, 50\n"
		"    str  [r4 + 0], r5\n"
		"    str  [r4 + 0], r7\n"
		"    li   r2, 0\n"
		"    str  [r1 + 0], r2\n"
		"    la   r0, 0x0301\n"
		"    la   r13, 0xFFFF0000\n"
		"    str  [r13 + 0], r0\n"
		"    halt\n";

	// The same program ending with status 0, so `run` returns its output.
	std::string withStatusZero()
	{
		std::string program = ResetOnce;
		const auto at = program.find("0x0301");
		if (at != std::string::npos)
			program.replace(at, 6, "0x0001");
		return program;
	}
}

TEST(driver_reset, a_reset_starts_the_program_again_with_its_image_reloaded)
{
	// Status 3 from the second run: `run` reports it as "exit 3", with the output in front of it gone,
	// so the output is checked through a status-0 variant as well.
	const std::string shown = run("ceres_reset_a.casm", ResetOnce);
	CHECK(shown.starts_with("exit 3"));

	CHECK_EQ(run("ceres_reset_b.casm", withStatusZero()), std::string{ "1727" });
}

TEST(driver_reset, a_windowed_host_restarts_the_program_between_frames_too)
{
	const auto path = std::filesystem::temp_directory_path() / "ceres_reset_window.casm";
	{
		std::ofstream file(path, std::ios::binary | std::ios::trunc);
		file << withStatusZero();
	}
	const HostBackendFactory factory = []() -> std::unique_ptr<HostBackend> { return std::make_unique<QuietWindow>(); };
	std::istringstream input;
	std::ostringstream output;
	std::ostringstream diagnostics;
	const int result = execute(RunCommand{ .input = path, .window = true }, { &input, &output, &diagnostics }, factory);
	std::filesystem::remove(path);
	CHECK_EQ(result, 0);
	CHECK_EQ(output.str(), std::string{ "1727" });
}

TEST(driver_reset, a_reset_disarms_the_timer_and_drops_a_transfer_in_flight)
{
	devices::TimerDevice timer;
	timer.arm(1000, true);
	CHECK(timer.isArmed());
	timer.reset();
	CHECK(!timer.isArmed());
	CHECK_EQ(timer.cycles(), u64{ 0 });

	devices::SystemControlDevice control;
	u32 told = 0xFFFFFFFFu;
	control.setFeaturesCallback([&](u32 features) { told = features; });
	control.write(devices::SystemControlDevice::FeaturesRegister, devices::SystemControlDevice::FeatureDivisionFault);
	CHECK_EQ(control.features(), devices::SystemControlDevice::FeatureDivisionFault);
	control.reset();
	CHECK_EQ(control.features(), 0u);
	CHECK_EQ(told, 0u);

	devices::DmaController dma;
	dma.write(vm::Address(0x08), 16);               // a length ...
	dma.write(vm::Address(0x0C), 1);                // ... armed: it lands on its event
	CHECK(dma.isPending());
	dma.reset();
	CHECK(!dma.isPending());
	CHECK_EQ(dma.read(vm::Address(0x10)), 0u);   // neither busy nor done
}

TEST(driver_reset, a_machine_without_a_reset_request_does_not_restart)
{
	vm::CeresVM vm;
	CHECK(!vm.isResetRequested());
	CHECK(!vm.restartIfRequested());
	vm.requestReset();
	CHECK(vm.isResetRequested());
	CHECK(!vm.isPoweredOn());
	CHECK(!vm.restartIfRequested());          // nothing loaded: nothing to start again
	CHECK(!vm.isResetRequested());
}
