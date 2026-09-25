#include <ceres/core/format/program.h>
#include <ceres/vm/ceresvm.h>
#include <algorithm>
#include <array>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <span>
#include <vector>

using namespace ceres;
using namespace ceres::fmt;
using namespace ceres::vm;

// One loop per instruction class, so the interpreter's cost can be told apart by class (plan/v2 F0.1), plus a
// mixed loop shaped like compiled code. Every loop is
//
//     prologue; loop: body; SUBI r1, r1, 1; CMPI r1, 0; JNZ loop; HALT; subroutine
//
// and r1 holds the iteration count. The report gives the loop's MIPS and, net of the three-instruction loop
// tail (measured by the "loop" benchmark, whose body is empty), the nanoseconds each instruction of the class
// costs. Usage: ceres_vm_benchmarks [name...] runs only the named benchmarks.
namespace
{
	using Clock = std::chrono::steady_clock;

	// A device that answers every word, as cheap as a device can be: what an MMIO access costs is then the bus.
	class RegisterFile final : public IODevice
	{
		u32 _word = 0;

		u32 read(Address) override { return _word; }
		const RegisterMap& registers() const override
		{
			static constexpr RegisterInfo table[] = {
				{ 0x00, "Word0", RegisterAccess::ReadWrite, 0, false, "" },
				{ 0x04, "Word1", RegisterAccess::ReadWrite, 0, false, "" },
			};
			static constexpr RegisterMap map{ "register-file", table };
			return map;
		}

		void write(Address, u32 value) override { _word = value; }
	};

	struct Benchmark
	{
		const char* name;
		std::vector<Instruction> prologue;
		std::vector<Instruction> body;
		// Instructions the subroutine runs per call, and the body indices of the CALLs that go to it.
		std::vector<Instruction> subroutine;
		std::vector<usize> calls;
		// Instructions of the measured class among those one iteration executes.
		u32 classPerIteration;
		u32 runsPerSample;
		bool needsDevice = false;

		u64 stepsPerIteration() const noexcept
		{
			return body.size() + calls.size() * subroutine.size() + 3;
		}
	};

	constexpr u32 Iterations = 50'000;
	constexpr u32 LoopRegister = 1;

	std::vector<Instruction> assemble(const Benchmark& benchmark)
	{
		std::vector<Instruction> code{ Instruction::LI(LoopRegister, Iterations) };
		code.insert(code.end(), benchmark.prologue.begin(), benchmark.prologue.end());
		const usize loop = code.size();
		code.insert(code.end(), benchmark.body.begin(), benchmark.body.end());
		code.push_back(Instruction::SUBI(LoopRegister, LoopRegister, 1));
		code.push_back(Instruction::CMPI(LoopRegister, 0));
		code.push_back(Instruction::JNZ(i24((static_cast<i32>(loop) - static_cast<i32>(code.size())) * 4)));
		code.push_back(Instruction::HALT());
		const usize subroutine = code.size();
		code.insert(code.end(), benchmark.subroutine.begin(), benchmark.subroutine.end());
		for (const usize call : benchmark.calls)
			code[loop + call] = Instruction::CALL(i24((static_cast<i32>(subroutine) - static_cast<i32>(loop + call)) * 4));
		return code;
	}

	u64 totalSteps(const Benchmark& benchmark)
	{
		return 1u + benchmark.prologue.size() + benchmark.stepsPerIteration() * Iterations + 1u;
	}

	Program makeProgram(const Benchmark& benchmark)
	{
		const std::vector<Instruction> code = assemble(benchmark);
		ProgramHeader header{};
		header.magic = ProgramHeader::MagicNumber;
		header.version = ProgramHeader::CurrentVersion;
		header.entryPoint = Memory::UnrestrictedSegmentStart.value();
		header.textSize = static_cast<u32>(code.size() * Instruction::Size);
		const std::vector<u8> text = Instruction::encode(code);
		return Program::make(header, text, {}, {});
	}

	// The device window the MMIO benchmark reaches: slot 1, 0xFF010000.
	constexpr u16 DeviceSlot = 1;
	// The LUI immediate that reaches it, taken from the bus so that the program and attach() cannot drift apart.
	constexpr u16 DeviceBaseHigh = static_cast<u16>(MmioBus::slot(DeviceSlot).value() >> 16);

	double run(const Benchmark& benchmark)
	{
		RegisterFile device; // outlives the machine it is attached to
		CeresVM vm;
		if (benchmark.needsDevice)
			vm.io().attach(MmioBus::slot(DeviceSlot), device);
		const Program program = makeProgram(benchmark);
		if (!vm.loadProgram(program).has_value())
			return 0.0;

		const u64 steps = totalSteps(benchmark);
		const auto start = Clock::now();
		for (u32 run = 0; run != benchmark.runsPerSample; ++run)
		{
			vm.engine().reset();
			for (u64 step = 0; step != steps; ++step)
				vm.engine().step();
		}
		return std::chrono::duration<double>(Clock::now() - start).count();
	}

	bool verify(const Benchmark& benchmark)
	{
		RegisterFile device; // outlives the machine it is attached to
		CeresVM vm;
		if (benchmark.needsDevice)
			vm.io().attach(MmioBus::slot(DeviceSlot), device);
		const Program program = makeProgram(benchmark);
		if (!vm.loadProgram(program).has_value())
			return false;
		const u64 steps = totalSteps(benchmark);
		for (u64 step = 0; step != steps; ++step)
			vm.engine().step();
		return vm.engine().isHalted() && vm.engine().executedInstructions() == steps;
	}

	template <usize N>
	std::vector<Instruction> repeat(const std::array<Instruction, N>& group, usize times)
	{
		std::vector<Instruction> body;
		for (usize i = 0; i != times; ++i)
			body.insert(body.end(), group.begin(), group.end());
		return body;
	}

	std::vector<Benchmark> benchmarks()
	{
		// Registers: r1 the loop, r2 a RAM base (0x10000), r3 an index, r4 = 3, r5 = 7 (r5 = 1020 in the
		// indexed loops), r6-r9 results, r10/r11 block pointers or the device base. f1 = 3.0, f2 = 7.0.
		const std::vector<Instruction> constants{
			Instruction::LUI(2, 1), Instruction::LI(3, 0), Instruction::LI(4, 3), Instruction::LI(5, 7),
			Instruction::ITOF(1, 4), Instruction::ITOF(2, 5),
		};
		auto with = [&](std::initializer_list<Instruction> more) {
			std::vector<Instruction> prologue = constants;
			prologue.insert(prologue.end(), more);
			return prologue;
		};

		std::vector<Benchmark> list;

		list.push_back({ "loop", constants, {}, {}, {}, 0, 200 });

		list.push_back({ "alu", constants,
			repeat(std::array{
				Instruction::ADD(6, 4, 5), Instruction::XOR(7, 6, 4), Instruction::ROL(8, 7, 4), Instruction::SHLI(9, 8, 3),
				Instruction::AND(6, 9, 5), Instruction::OR(7, 6, 4), Instruction::SUB(8, 7, 5), Instruction::MOV(9, 8),
			}, 2),
			{}, {}, 16, 100 });

		list.push_back({ "ram", constants,
			repeat(std::array{
				Instruction::STR(2, 4, 0), Instruction::LDR(6, 2, 0), Instruction::STR(2, 5, 4), Instruction::LDR(7, 2, 4),
				Instruction::STRB(2, 4, 9), Instruction::LDRB(8, 2, 9), Instruction::STRH(2, 5, 10), Instruction::LDRH(9, 2, 10),
			}, 2),
			{}, {}, 16, 100 });

		list.push_back({ "mmio", with({ Instruction::LUI(10, DeviceBaseHigh) }),
			repeat(std::array{ Instruction::STR(10, 4, 0), Instruction::LDR(6, 10, 0), Instruction::STR(10, 5, 4), Instruction::LDR(7, 10, 4) }, 4),
			{}, {}, 16, 100, true });

		// Every jump lands on the next instruction; with r4 < r5, JZ falls through and the rest are taken.
		list.push_back({ "branch", constants,
			[] {
				std::vector<Instruction> body{ Instruction::CMP(4, 5) };
				const auto rest = repeat(std::array{ Instruction::JP(i24(4)), Instruction::JZ(i24(4)), Instruction::JNZ(i24(4)),
					Instruction::JLS(i24(4)), Instruction::JLE(i24(4)) }, 3);
				body.insert(body.end(), rest.begin(), rest.end());
				return body;
			}(),
			{}, {}, 15, 100 });

		list.push_back({ "call-ret", constants, repeat(std::array{ Instruction::NOP() }, 8), { Instruction::RET() },
			{ 0, 1, 2, 3, 4, 5, 6, 7 }, 16, 100 });

		list.push_back({ "push-pop", constants,
			repeat(std::array{ Instruction::PUSH(4), Instruction::PUSH(5), Instruction::POP(6), Instruction::POP(7) }, 4),
			{}, {}, 16, 100 });

		// Four registers a mask: r4-r7.
		list.push_back({ "pushm-popm", constants, repeat(std::array{ Instruction::PUSHM(0x00F0), Instruction::POPM(0x00F0) }, 4),
			{}, {}, 8, 100 });

		list.push_back({ "mul", constants,
			repeat(std::array{ Instruction::MUL(6, 4, 5), Instruction::IMUL(7, 5, 4), Instruction::MULH(8, 4, 5), Instruction::IMULH(9, 5, 4) }, 4),
			{}, {}, 16, 100 });

		list.push_back({ "div", constants,
			repeat(std::array{ Instruction::DIV(6, 5, 4), Instruction::IDIV(7, 5, 4), Instruction::MOD(8, 5, 4), Instruction::IMOD(9, 5, 4) }, 4),
			{}, {}, 16, 100 });

		list.push_back({ "float", constants,
			repeat(std::array{ Instruction::FADD(3, 1, 2), Instruction::FMUL(4, 1, 2), Instruction::FSUB(5, 2, 1), Instruction::FMIN(6, 1, 2),
				Instruction::FABS(7, 5), Instruction::FNEG(8, 1), Instruction::FCMP(1, 2), Instruction::ITOF(9, 4) }, 2),
			{}, {}, 16, 100 });

		list.push_back({ "fdiv", constants,
			repeat(std::array{ Instruction::FDIV(3, 2, 1), Instruction::FSQRT(4, 2), Instruction::FDIV(5, 1, 2), Instruction::FSQRT(6, 1) }, 4),
			{}, {}, 16, 100 });

		list.push_back({ "fma", constants, repeat(std::array{ Instruction::FMA(3, 1, 2) }, 16), {}, {}, 16, 100 });

		// 256 bytes a copy or a fill: RAM from 0x20000 to 0x20100 and 0x21000. Three setup instructions (the
		// block instructions consume their registers) count against the class, so this reads high.
		list.push_back({ "block-256", with({ Instruction::LUI(10, 2), Instruction::LUI(11, 2), Instruction::ORI(11, 11, 0x1000) }),
			repeat(std::array{
				Instruction::LI(9, 256), Instruction::MOV(7, 10), Instruction::MOV(8, 11), Instruction::MCPY(7, 8, 9),
				Instruction::LI(9, 256), Instruction::MOV(7, 11), Instruction::LI(8, 0x55), Instruction::MSET(7, 8, 9),
			}, 2),
			{}, {}, 4, 100 });

		// Shaped like a compiled loop over an int array that calls a small function each time: loads and
		// stores, ALU, a multiply, a taken branch, a call with a frame. 14 instructions in the loop, 6 in the
		// function; r5 is the index mask.
		list.push_back({ "mixed", with({ Instruction::LI(5, 1020), Instruction::LI(9, 3) }),
			{
				Instruction::LDRX(6, 2, 3), Instruction::ADD(7, 7, 6), Instruction::MUL(8, 6, 9), Instruction::STRX(2, 8, 3),
				Instruction::ADDI(3, 3, 4), Instruction::AND(3, 3, 5), Instruction::CMP(4, 5), Instruction::JLS(i24(4)),
				Instruction::NOP(), Instruction::ADDI(7, 7, 1), Instruction::SHRI(8, 7, 2), Instruction::XOR(6, 6, 8),
				Instruction::LDR(10, 2, 4), Instruction::ADD(7, 7, 10),
			},
			{ Instruction::PUSH(4), Instruction::ADD(4, 6, 7), Instruction::STR(2, 4, 4), Instruction::SUBI(4, 4, 1),
				Instruction::POP(4), Instruction::RET() },
			{ 8 }, 0, 100 });

		return list;
	}
}

int main(int argc, char** argv)
{
	const std::span<char*> only(argv + (argc > 0 ? 1 : 0), argc > 1 ? static_cast<usize>(argc - 1) : 0);
	double loopNsPerIteration = 0.0;

	std::printf("%-12s %9s %10s %12s\n", "benchmark", "MIPS", "ns/instr", "ns/class op");
	for (const Benchmark& benchmark : benchmarks())
	{
		const bool isLoop = std::strcmp(benchmark.name, "loop") == 0;
		if (!only.empty() && !isLoop &&
			std::none_of(only.begin(), only.end(), [&](const char* name) { return std::strcmp(name, benchmark.name) == 0; }))
			continue;

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
		const double runs = benchmark.runsPerSample;
		const double steps = static_cast<double>(totalSteps(benchmark));
		const double nsPerIteration = seconds * 1e9 / runs / Iterations;
		if (isLoop)
			loopNsPerIteration = nsPerIteration;

		std::printf("%-12s %9.1f %10.2f", benchmark.name, steps * runs / seconds / 1e6, seconds * 1e9 / runs / steps);
		if (benchmark.classPerIteration != 0)
			std::printf(" %12.2f", (nsPerIteration - loopNsPerIteration) / benchmark.classPerIteration);
		std::printf("\n");
	}
}
