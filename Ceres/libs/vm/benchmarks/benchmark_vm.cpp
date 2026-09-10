#include <ceres/core/format/program.h>
#include <ceres/vm/ceresvm.h>
#include <algorithm>
#include <array>
#include <chrono>
#include <cstdio>
#include <vector>

using namespace ceres;
using namespace ceres::fmt;
using namespace ceres::vm;

namespace
{
	using Clock = std::chrono::steady_clock;

	struct Benchmark
	{
		const char* name;
		std::vector<Instruction> instructions;
		u32 iterations;
		u64 steps;
		u32 runsPerSample;
	};

	Program makeProgram(const Benchmark& benchmark)
	{
		const ProgramHeader header{
			.magic = ProgramHeader::MagicNumber,
			.version = ProgramHeader::CurrentVersion,
			.entryPoint = Memory::UnrestrictedSegmentStart.value(),
			.textSize = static_cast<u32>(benchmark.instructions.size() * Instruction::Size),
		};
		return Program::make(header, Instruction::asBytes(benchmark.instructions), {}, {});
	}

	double run(const Benchmark& benchmark)
	{
		CeresVM vm;
		const Program program = makeProgram(benchmark);
		if (!vm.loadProgram(program).has_value())
			return 0.0;

		const auto start = Clock::now();
		for (u32 run = 0; run != benchmark.runsPerSample; ++run)
		{
			vm.engine().reset();
			for (u64 step = 0; step != benchmark.steps; ++step)
				vm.engine().step();
		}
		return std::chrono::duration<double>(Clock::now() - start).count();
	}

	bool verify(const Benchmark& benchmark)
	{
		CeresVM vm;
		const Program program = makeProgram(benchmark);
		if (!vm.loadProgram(program).has_value())
			return false;
		for (u64 step = 0; step != benchmark.steps; ++step)
			vm.engine().step();
		return vm.engine().isHalted() && vm.engine().executedInstructions() == benchmark.steps;
	}
}

int main()
{
	constexpr u32 iterations = 50'000;
	const std::array benchmarks{
		Benchmark{
			"alu-branch",
			{
				Instruction::LI(1, iterations), Instruction::LI(2, 1), Instruction::LI(3, 3),
				Instruction::ADD(4, 2, 3), Instruction::XOR(2, 2, 4), Instruction::ROL(3, 3, 2),
				Instruction::SUBI(1, 1, 1), Instruction::CMPI(1, 0), Instruction::JNZ(i24(-20)), Instruction::HALT(),
			},
			iterations,
			3u + 6u * iterations + 1u,
			100,
		},
		Benchmark{
			"indexed-memory",
			{
				Instruction::LI(1, iterations), Instruction::LUI(2, 1), Instruction::LI(3, 0),
				Instruction::LI(4, 1), Instruction::LI(5, 1020),
				Instruction::STRX(2, 4, 3), Instruction::LDRX(6, 2, 3), Instruction::ADDI(4, 4, 3),
				Instruction::ADDI(3, 3, 4), Instruction::AND(3, 3, 5), Instruction::SUBI(1, 1, 1),
				Instruction::CMPI(1, 0), Instruction::JNZ(i24(-28)), Instruction::HALT(),
			},
			iterations,
			5u + 8u * iterations + 1u,
			100,
		},
	};

	for (const Benchmark& benchmark : benchmarks)
	{
		if (!verify(benchmark))
		{
			std::fprintf(stderr, "%s: verification failed\n", benchmark.name);
			return 1;
		}

		std::array<double, 7> samples;
		for (double& sample : samples)
			sample = run(benchmark);
		std::sort(samples.begin(), samples.end());
		const double seconds = samples[samples.size() / 2];
		const double mips = static_cast<double>(benchmark.steps) * benchmark.runsPerSample / seconds / 1e6;
		std::printf("%-16s %8.2f MIPS  (%llu instructions x %u, median of %zu samples)\n", benchmark.name, mips,
			static_cast<unsigned long long>(benchmark.steps), benchmark.runsPerSample, samples.size());
	}
}
