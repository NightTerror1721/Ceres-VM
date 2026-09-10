#include "framework.h"
#include <ceres/driver/command.h>
#include <ceres/driver/driver.h>

#include <filesystem>
#include <fstream>
#include <sstream>

using namespace ceres::driver;
using namespace ceres::testing;

TEST(driver_command, bare_path_is_run)
{
	char program[] = "ceres";
	char input[] = "demo.casm";
	char* argv[] = { program, input };
	auto parsed = parseCommandLine(2, argv);
	CHECK(parsed.has_value());
	CHECK(std::holds_alternative<RunCommand>(*parsed));
}

TEST(driver_command, invalid_memory_is_a_usage_error)
{
	char program[] = "ceres";
	char run[] = "run";
	char input[] = "demo.casm";
	char memory[] = "--memory";
	char value[] = "nope";
	char* argv[] = { program, run, input, memory, value };
	auto parsed = parseCommandLine(5, argv);
	CHECK(!parsed.has_value());
}

TEST(driver_command, archive_has_an_output_not_a_fake_input)
{
	char program[] = "ceres";
	char ar[] = "ar";
	char output[] = "library.car";
	char object[] = "member.cobj";
	char* argv[] = { program, ar, output, object };
	auto parsed = parseCommandLine(4, argv);
	CHECK(parsed.has_value());
	const auto* archive = std::get_if<ArchiveCommand>(&*parsed);
	CHECK(archive != nullptr);
	CHECK_EQ(archive->output.string(), std::string("library.car"));
	CHECK_EQ(archive->inputs.size(), std::size_t{1});
}

TEST(driver_command, options_from_another_command_are_rejected)
{
	char program[] = "ceres";
	char run[] = "run";
	char input[] = "demo.casm";
	char json[] = "--json";
	char* argv[] = { program, run, input, json };
	auto parsed = parseCommandLine(4, argv);
	CHECK(!parsed.has_value());
}

TEST(driver_command, json_diagnostics_stay_on_the_output_stream)
{
	const auto source = std::filesystem::temp_directory_path() / "ceres_driver_invalid_test.casm";
	{
		std::ofstream file{source};
		file << "this is not Ceres assembly\n";
	}

	std::istringstream input;
	std::ostringstream output;
	std::ostringstream diagnostics;
	const int result = execute(AssembleCommand{.inputs = {source}, .jsonDiagnostics = true},
		{&input, &output, &diagnostics});
	std::filesystem::remove(source);

	CHECK_EQ(result, 1);
	CHECK(output.str().starts_with("["));
	CHECK(diagnostics.str().empty());
}

TEST(driver_command, missing_input_is_an_operational_error)
{
	std::istringstream input;
	std::ostringstream output;
	std::ostringstream diagnostics;
	const int result = execute(RunCommand{.input = "definitely-missing.casm"},
		{&input, &output, &diagnostics});

	CHECK_EQ(result, 1);
	CHECK(diagnostics.str().starts_with("No such file:"));
}
