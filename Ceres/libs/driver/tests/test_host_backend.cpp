#include "framework.h"
#include <ceres/driver/driver.h>
#include <ceres/driver/host_backend.h>

#include <filesystem>
#include <fstream>
#include <memory>
#include <sstream>

using namespace ceres;
using namespace ceres::driver;
using namespace ceres::testing;

namespace
{
	// A backend that records how often the loop calls it, without any window. It is what proves the
	// cooperative loop (pump -> slice -> present) runs, without needing SDL in the test.
	class RecordingBackend final : public HostBackend
	{
	public:
		int pumps = 0;
		int presents = 0;

		bool pump(devices::KeyboardDevice&, devices::MouseDevice&, devices::GamepadDevice&) override
		{
			++pumps;
			return true;
		}
		void present(const devices::DisplayDevice&) override { ++presents; }
	};
}

TEST(driver_command, run_accepts_the_window_flag)
{
	char program[] = "ceres";
	char run[] = "run";
	char input[] = "demo.casm";
	char window[] = "--window";
	char* argv[] = { program, run, input, window };
	auto parsed = parseCommandLine(4, argv);
	CHECK(parsed.has_value());
	const auto* command = std::get_if<RunCommand>(&*parsed);
	CHECK(command != nullptr);
	CHECK(command->window);
}

TEST(driver_window, window_without_a_backend_is_a_clean_error)
{
	std::istringstream input;
	std::ostringstream output;
	std::ostringstream diagnostics;
	const int result = execute(RunCommand{.input = "no-such.casm", .window = true},
		{&input, &output, &diagnostics}, {});

	CHECK_EQ(result, 1);
	CHECK(diagnostics.str().find("windowed host") != std::string::npos);
}

TEST(driver_window, a_windowed_run_drives_the_cooperative_loop)
{
	const auto source = std::filesystem::temp_directory_path() / "ceres_driver_window_test.casm";
	{
		std::ofstream file{source};
		file << "@text\n"
			"global main:\n"
			"    la r13, 0xFF000004\n"
			"    li r0, 65\n"
			"    strb [r13 + 0], r0\n"
			"    la r13, 0xFFFF0000\n"
			"    li r0, 1\n"
			"    strb [r13 + 0], r0\n";
	}

	RecordingBackend* captured = nullptr;
	const HostBackendFactory factory = [&captured]() -> std::unique_ptr<HostBackend>
	{
		auto backend = std::make_unique<RecordingBackend>();
		captured = backend.get();
		return backend;
	};

	std::istringstream input;
	std::ostringstream output;
	std::ostringstream diagnostics;
	const int result = execute(RunCommand{.input = source, .window = true},
		{&input, &output, &diagnostics}, factory);
	std::filesystem::remove(source);

	CHECK_EQ(result, 0);
	CHECK(captured != nullptr);
	if (!captured) return;

	// The program printed its byte and shut down; the loop ran at least one pump/present cycle.
	CHECK_EQ(output.str(), std::string{ "A" });
	CHECK(captured->pumps >= 1);
	CHECK(captured->presents >= 1);
}
