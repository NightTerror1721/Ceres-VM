// Writing a program to disk and reading it back. Until Phase 5 the assembler could only hand a
// Program to a VM in the same process, because Program could be loaded five ways and written
// none.

#include "framework.h"
#include "assemble_helper.h"
#include <ceres/core/format/program.h>
#include <filesystem>
#include <sstream>

using namespace ceres;
using namespace ceres::fmt;
using namespace ceres::testing;

namespace
{
	constexpr std::string_view SampleSource =
		"const OUT = 0x01\r\n"
		"@rodata\r\n"
		"    let msg: u8[4] = \"abc\"\r\n"
		"@data\r\n"
		"    let counter: u32 = 7\r\n"
		"@text\r\n"
		"global main:\r\n"
		"    la r1, msg\r\n"
		"    ldv r2, counter\r\n"
		"    outb OUT, r2\r\n"
		"    ret\r\n";
}

TEST(programfile, a_program_survives_a_round_trip_through_a_stream)
{
	AssembleResult assembled = assembleSource(SampleSource, "roundtrip");

	CHECK(assembled.ok());
	if (!assembled.ok()) { Registry::instance().recordFailure(assembled.joinedErrors()); return; }

	std::stringstream stream(std::ios::in | std::ios::out | std::ios::binary);

	const auto written = assembled.program->writeToStream(stream);
	CHECK(written.has_value());
	if (!written.has_value()) { Registry::instance().recordFailure(written.error()); return; }

	auto reloaded = Program::loadFromStream(stream);
	CHECK(reloaded.has_value());
	if (!reloaded.has_value()) { Registry::instance().recordFailure(reloaded.error()); return; }

	const ProgramHeader& before = assembled.program->header();
	const ProgramHeader& after = reloaded->header();

	CHECK_EQ(after.magic, before.magic);
	CHECK_EQ(after.version, before.version);
	CHECK_EQ(after.entryPoint, before.entryPoint);
	CHECK_EQ(after.textSize, before.textSize);
	CHECK_EQ(after.rodataSize, before.rodataSize);
	CHECK_EQ(after.dataSize, before.dataSize);

	CHECK(std::equal(before.magic == after.magic ? assembled.program->text().begin() : assembled.program->text().begin(),
		assembled.program->text().end(), reloaded->text().begin()));
	CHECK(std::equal(assembled.program->rodata().begin(), assembled.program->rodata().end(), reloaded->rodata().begin()));
	CHECK(std::equal(assembled.program->data().begin(), assembled.program->data().end(), reloaded->data().begin()));
}

TEST(programfile, a_program_survives_a_round_trip_through_a_file)
{
	AssembleResult assembled = assembleSource(SampleSource, "filetrip");

	CHECK(assembled.ok());
	if (!assembled.ok()) { Registry::instance().recordFailure(assembled.joinedErrors()); return; }

	const std::filesystem::path path = std::filesystem::temp_directory_path() / "ceres_test_roundtrip.cres";

	const auto saved = assembled.program->saveToFile(path);
	CHECK(saved.has_value());
	if (!saved.has_value()) { Registry::instance().recordFailure(saved.error()); return; }

	auto reloaded = Program::loadFromFile(path);
	CHECK(reloaded.has_value());
	if (!reloaded.has_value())
	{
		Registry::instance().recordFailure(reloaded.error());
		std::error_code ignored;
		std::filesystem::remove(path, ignored);
		return;
	}

	CHECK_EQ(reloaded->text().size(), assembled.program->text().size());
	CHECK(std::equal(assembled.program->text().begin(), assembled.program->text().end(), reloaded->text().begin()));

	std::error_code ignored;
	std::filesystem::remove(path, ignored);
}

TEST(programfile, the_written_file_starts_with_the_magic_number)
{
	AssembleResult assembled = assembleSource(SampleSource, "magic");

	CHECK(assembled.ok());
	if (!assembled.ok()) { Registry::instance().recordFailure(assembled.joinedErrors()); return; }

	std::stringstream stream(std::ios::in | std::ios::out | std::ios::binary);
	(void)assembled.program->writeToStream(stream);

	const std::string bytes = stream.str();
	CHECK(bytes.size() >= sizeof(ProgramHeader));
	if (bytes.size() < sizeof(ProgramHeader)) return;

	// 'CRES' little-endian.
	CHECK_EQ(static_cast<u8>(bytes[0]), u8{ 'S' });
	CHECK_EQ(static_cast<u8>(bytes[1]), u8{ 'E' });
	CHECK_EQ(static_cast<u8>(bytes[2]), u8{ 'R' });
	CHECK_EQ(static_cast<u8>(bytes[3]), u8{ 'C' });
}

TEST(programfile, the_file_is_exactly_the_header_plus_the_three_sections)
{
	AssembleResult assembled = assembleSource(SampleSource, "size");

	CHECK(assembled.ok());
	if (!assembled.ok()) { Registry::instance().recordFailure(assembled.joinedErrors()); return; }

	std::stringstream stream(std::ios::in | std::ios::out | std::ios::binary);
	(void)assembled.program->writeToStream(stream);

	const ProgramHeader& header = assembled.program->header();
	const usize expected = sizeof(ProgramHeader) + header.textSize + header.rodataSize + header.dataSize;

	// bss occupies no space in the file: it is zeroed at load time.
	CHECK_EQ(stream.str().size(), expected);
}

TEST(programfile, a_file_that_is_not_a_program_is_rejected)
{
	std::stringstream stream(std::ios::in | std::ios::out | std::ios::binary);
	stream << "this is not a Ceres program, not even close";

	auto loaded = Program::loadFromStream(stream);
	CHECK(!loaded.has_value());
}

// Adding the comparison jumps moved every opcode above the control-flow block, so a version 1 file
// is not a version 2 file with unfamiliar instructions in it - it is one where 0x70 used to mean
// PUSH and now means JAB. The check that only rejected versions *newer* than the current one would
// have loaded it and run it as something else entirely, silently.
TEST(programfile, a_file_from_before_the_opcode_renumbering_is_rejected)
{
	AssembleResult assembled = assembleSource(SampleSource, "oldversion");

	CHECK(assembled.ok());
	if (!assembled.ok()) { Registry::instance().recordFailure(assembled.joinedErrors()); return; }

	std::stringstream stream(std::ios::in | std::ios::out | std::ios::binary);
	const auto written = assembled.program->writeToStream(stream);
	CHECK(written.has_value());
	if (!written.has_value()) { Registry::instance().recordFailure(written.error()); return; }

	// Rewind the version field in place, leaving a well-formed file that claims to be version 1.
	std::string bytes = stream.str();
	const usize versionOffset = offsetof(ProgramHeader, version);
	bytes[versionOffset] = 1;
	bytes[versionOffset + 1] = 0;

	std::stringstream older(bytes, std::ios::in | std::ios::out | std::ios::binary);
	auto loaded = Program::loadFromStream(older);

	CHECK(!loaded.has_value());
}

TEST(programfile, the_current_version_is_still_accepted)
{
	AssembleResult assembled = assembleSource(SampleSource, "currentversion");

	CHECK(assembled.ok());
	if (!assembled.ok()) { Registry::instance().recordFailure(assembled.joinedErrors()); return; }
	CHECK_EQ(assembled.program->header().version, ProgramHeader::CurrentVersion);

	std::stringstream stream(std::ios::in | std::ios::out | std::ios::binary);
	const auto written = assembled.program->writeToStream(stream);
	CHECK(written.has_value());
	if (!written.has_value()) return;

	auto reloaded = Program::loadFromStream(stream);
	CHECK(reloaded.has_value());
	if (!reloaded.has_value()) Registry::instance().recordFailure(reloaded.error());
}
