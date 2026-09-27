#pragma once

// What the driver's tests share to see a run's output: the program never writes to the host's streams (plan/v2
// SPEC 1.4), so a run is given a --transcript in a temporary file and, when there is input for it, a --type, and
// the transcript is read back.

#include <ceres/driver/driver.h>

#include <atomic>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <string_view>

namespace ceres::testing
{
	struct CapturedRun
	{
		int status = 0;
		std::string output;        // the transcript: what the program wrote to its terminal
		std::string hostOutput;    // what the run wrote to the host's stdout (the listing, if asked for; never the program)
		std::string diagnostics;   // the host's stderr
	};

	inline std::filesystem::path uniqueTempPath(std::string_view stem)
	{
		static std::atomic<unsigned> counter{ 0 };
		return std::filesystem::temp_directory_path() / (std::string(stem) + "_" + std::to_string(++counter));
	}

	inline std::string readWhole(const std::filesystem::path& path)
	{
		std::ifstream file(path, std::ios::binary);
		return std::string(std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>());
	}

	// Runs `command` with a transcript, `typed` typed on its terminal, through `window` when given.
	inline CapturedRun captureRun(driver::RunCommand command, std::string_view typed = {}, driver::WindowHostFactory window = {})
	{
		const auto transcript = uniqueTempPath("ceres_transcript");
		command.transcript = transcript;
		std::filesystem::path typeFile;
		if (!typed.empty())
		{
			typeFile = uniqueTempPath("ceres_typed");
			std::ofstream file(typeFile, std::ios::binary | std::ios::trunc);
			file << typed;
			command.typeFile = typeFile;
		}
		std::istringstream input;
		std::ostringstream output;
		std::ostringstream diagnostics;
		CapturedRun run;
		run.status = driver::execute(command, { &input, &output, &diagnostics }, std::move(window));
		run.output = readWhole(transcript);
		run.hostOutput = output.str();
		run.diagnostics = diagnostics.str();
		std::filesystem::remove(transcript);
		if (!typeFile.empty())
			std::filesystem::remove(typeFile);
		return run;
	}
}
