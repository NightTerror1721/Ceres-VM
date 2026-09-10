#pragma once

#include "memory.h"
#include "mmio_bus.h"
#include "interrupt_controller.h"
#include "mmu.h"
#include <ceres/core/isa/address.h>
#include <ceres/core/isa/fregisters.h>
#include <ceres/core/isa/instructions.h>
#include <ceres/core/isa/interrupts.h>
#include <algorithm>
#include <bit>
#include <span>
#include <vector>
#include <limits>
#include <cmath>
#include <functional>
#include <optional>

namespace ceres::vm
{
	// Which way a load or a store went, for a watchpoint that wants to know.
	enum class AccessKind : u8 { Read, Write };

	class ExecutionEngine
	{
	private:
		GeneralPurposeRegisterPool _registers;
		FloatingPointRegisterPool _fregisters;
		Address _pc; // Program Counter (PC)
		FlagRegister _flags; // Flags register
		Memory& _memory;
		MmioBus& _mmioBus;
		InterruptController& _interrupts;
		Mmu _mmu;

		// Set the instant a translation or an alignment check redirects the PC into a fault handler,
		// and checked by advancePC() so the handler that was mid-instruction cannot then walk the PC
		// past that redirect. Reset once per step(), before fetch(). Existing faults (alignment, the
		// text-segment write guard) already returned early before their own advancePC() and never
		// needed this; it is what makes every *other* load/store handler - the ones that call
		// read<T>()/write<T>() unconditionally, like LDRB - safe now that either can fault too.
		bool _faulted = false;

		// Instructions retired since the last reset. The timer already counts in executed
		// instructions rather than wall clock, so this is the machine's own notion of time and
		// the only clock a debugger can step against deterministically.
		u64 _executedInstructions = 0;

		// The lowest address the stack may grow down to. Until a program is loaded this is all the
		// machine can defend - the vector table and the BIOS - which is what it was defending
		// before: a runaway stack ate the program's own text for as many megabytes as it took to
		// reach 0x400, and only then said so. loadProgram lowers it to the end of the loaded image.
		u32 _stackLimit = static_cast<u32>(Memory::UnrestrictedSegmentStartValue);

		// How many interrupts are being serviced, and what the program's own stack pointer was when
		// the first one arrived. Nested interrupts stay on the system stack; only the outermost one
		// switches, and only it switches back.
		u32 _interruptDepth = 0;
		u32 _savedStackPointer = 0;

		// The loaded program's own text, which nothing a correct program does ever writes to. An
		// empty range means no program is loaded and there is nothing to protect. A store into it
		// used to simply take effect, so a lost pointer rewrote an instruction that had not run
		// yet and the machine went wrong somewhere else entirely.
		u32 _textStart = 0;
		u32 _textEnd = 0;

		// One counter per instruction word of .text, and null unless a profile was asked for.
		// The machine already counts in executed instructions rather than wall clock, so this is a
		// profile that comes out the same on every run - which a real machine cannot offer.
		std::vector<u64> _executionCounts;
		u64* _executionCountsData = nullptr;

		// Empty unless a debugger is attached. A watchpoint that compares snapshots between
		// instructions cannot see a read at all, nor a write that puts back the value that was
		// already there; this sees the access itself. The cost when nobody is watching is one
		// predictable branch per load and store.
		std::function<void(AccessKind, u32, u32)> _accessObserver;

		// Empty unless a debugger is attached; see setInterruptObserver.
		std::function<void(InterruptNumber, Address, bool)> _interruptObserver;


	public:
		explicit ExecutionEngine(Memory& memory, MmioBus& mmioBus, InterruptController& interrupts) :
			_memory(memory), _mmioBus(mmioBus), _interrupts(interrupts)
		{}
		ExecutionEngine(const ExecutionEngine&) = delete;
		ExecutionEngine(ExecutionEngine&&) = delete;
		~ExecutionEngine() = default;

		ExecutionEngine& operator=(const ExecutionEngine&) = delete;
		ExecutionEngine& operator=(ExecutionEngine&&) = delete;

	public:
		void reset() noexcept;
		void step() noexcept;

	public:
		// Read-only observability. Tests (and any future debugger) need to inspect the machine
		// state between steps; nothing here can mutate it.
		constexpr const GeneralPurposeRegisterPool& registers() const noexcept { return _registers; }
		constexpr const FloatingPointRegisterPool& fregisters() const noexcept { return _fregisters; }
		constexpr const FlagRegister& flags() const noexcept { return _flags; }
		constexpr Address programCounter() const noexcept { return _pc; }
		constexpr bool isHalted() const noexcept { return _flags.halting(); }
		constexpr u64 executedInstructions() const noexcept { return _executedInstructions; }
		constexpr u32 stackLimit() const noexcept { return _stackLimit; }
		constexpr u32 interruptDepth() const noexcept { return _interruptDepth; }
		// Where the program's own stack ends and the system stack begins.
		u32 systemStackFloor() const noexcept { return static_cast<u32>(_memory.size() - Memory::SystemStackSize); }
		constexpr u32 textStart() const noexcept { return _textStart; }
		constexpr u32 textEnd() const noexcept { return _textEnd; }
		std::span<const u64> executionCounts() const noexcept { return _executionCounts; }

		// A program builds its own page tables in plain memory before PGON, and a test or debugger
		// wants to build them from outside too - see docs/27-Virtual-Memory-and-Paging.md.
		Mmu& mmu() noexcept { return _mmu; }
		const Mmu& mmu() const noexcept { return _mmu; }

	public:
		// Write access, for a debugger: setting a register from the editor's variables view,
		// jumping to the cursor, restoring a snapshot. Kept apart from the read-only block above
		// and out of the private helpers the instruction handlers use, so a call from inside the
		// machine itself reads as the mistake it would be. Each one rejects an out-of-range index
		// rather than trusting a caller that is, by definition, outside the machine.
		bool setRegister(u8 index, u32 value) noexcept
		{
			if (index >= GeneralPurposeRegisterPool::Count)
				return false;
			_registers.setValue(index, value);
			return true;
		}

		bool setFloatRegister(u8 index, f32 value) noexcept
		{
			if (index >= FloatingPointRegisterPool::Count)
				return false;
			_fregisters.setValue(index, value);
			return true;
		}

		// Set by the loader once it knows where the image ends. A reset deliberately leaves it
		// alone: the same program is still in memory, so the same ground is still worth guarding.
		// Told about every load and store the program performs, with the address and the width.
		// Instruction fetch is deliberately not reported: it is not an access the program made.
		void setAccessObserver(std::function<void(AccessKind, u32, u32)> observer) noexcept
		{
			_accessObserver = std::move(observer);
		}

		void setStackLimit(u32 lowestAddress) noexcept { _stackLimit = lowestAddress; }

		// Also the loader's to set, and also kept across a reset. Pass an empty range to lift the
		// protection, which is what a machine with no program loaded has.
		void setTextRange(u32 start, u32 end) noexcept { _textStart = start; _textEnd = end; }

		// Starts counting how often each instruction word runs. Call after the text range is set;
		// the counters are indexed off it.
		void enableProfiling()
		{
			_executionCounts.assign(_textEnd > _textStart ? (_textEnd - _textStart) / Instruction::Size : 0, 0);
			_executionCountsData = _executionCounts.empty() ? nullptr : _executionCounts.data();
		}

		void setFlags(FlagRegister flags) noexcept { _flags = flags; }
		void setProgramCounter(Address address) noexcept { _pc = address; }
		// Only for restoring a snapshot: the machine's clock has to go back with the rest of it,
		// or a restored timer would fire against a count that never rewound.
		void setExecutedInstructions(u64 count) noexcept { _executedInstructions = count; }

	public:
		// Told about every interrupt the machine takes, with the program counter as it stood when
		// the interrupt fired — which for a fault is the instruction that caused it, and is
		// otherwise unrecoverable once the dispatch has redirected the PC into the handler.
		// `entered` distinguishes a handler actually being run from a request that was masked,
		// had no handler, or could not be delivered for want of stack.
		//
		// Must not throw: it is called from triggerInterrupt, which is noexcept.
		using InterruptObserver = std::function<void(InterruptNumber number, Address atPc, bool entered)>;
		void setInterruptObserver(InterruptObserver observer) noexcept { _interruptObserver = std::move(observer); }
		void clearInterruptObserver() noexcept { _interruptObserver = nullptr; }

	private:
		inline void handleReset() noexcept { reset(); }
		void handleHalt() noexcept;
		void handleTrap() noexcept;

		void triggerInterrupt(InterruptNumber interruptNumber) noexcept;

		inline void execute(const Instruction instruction) noexcept
		{
			InstructionHandler handler = InstructionHandlers[static_cast<u8>(instruction.opcode())];
			(this->*handler)(instruction);
		}

	private:
		// The one MMU chokepoint every read<T>/write<T> call and fetch() share. Paging off is the
		// fast, common path: the address comes back unchanged and nothing about Memory has to know
		// paging exists at all. Paging on and a translation failure both go through triggerInterrupt
		// here, exactly like the alignment and text-segment checks already do, so PageFault behaves
		// like every other fault this engine raises rather than like a new kind of failure.
		forceinline std::optional<Address> translate(Address address, MmuAccess access) noexcept
		{
			if (!_flags.get<ExecutionFlag::Paging>())
				return address;

			// The null page and the BIOS (below 0x400) and the system stack (the top SystemStackSize
			// bytes) are VM-owned memory that no program's page table describes - they stay physical
			// whether or not paging is on, the same way they are already reached through the unchecked
			// path rather than the checked one. This is not just convenience: without it, a page fault
			// taken while the system stack itself happened to be unmapped would recurse into dispatching
			// the very fault it is trying to save a frame for, and a program that forgot to map its own
			// fault vectors could never even reach the BIOS's default handler to fail safely.
			const u32 raw = address.value();
			if (raw < Memory::UnrestrictedSegmentStartValue || raw >= systemStackFloor())
				return address;

			if (const auto physical = _mmu.translate(_memory, address, access))
				return physical;

			triggerInterrupt(InterruptNumber::PageFault);
			return std::nullopt;
		}

		forceinline Instruction fetch() noexcept
		{
			const auto physical = translate(_pc, MmuAccess::Execute);
			if (!physical.has_value())
				return Instruction(0); // Never executed: step() checks _faulted and skips execute().
			return _memory.readInstruction(*physical);
		}
		// A no-op once this instruction has already faulted: triggerInterrupt has redirected the PC
		// to the handler, and a handler still mid-execution after that must not then walk it forward
		// again. Every handler that does not check for a fault explicitly - most of them - relies on
		// this to stay correct now that read<T>/write<T> can fault too.
		forceinline void advancePC() noexcept { if (!_faulted) _pc += Instruction::SizeInBytes; }

		forceinline u32 getReg(u8 index) const noexcept { return _registers.getValue(index); }
		forceinline void setReg(u8 index, u32 value) noexcept { _registers.setValue(index, value); }

		forceinline f32 getFloatReg(u8 index) const noexcept { return _fregisters.getValue(index); }
		forceinline void setFloatReg(u8 index, f32 value) noexcept { _fregisters.setValue(index, value); }

		forceinline u32 sp() const noexcept { return _registers.getValue<GeneralPurposeRegisterPool::StackPointerIndex>(); }
		forceinline void sp(u32 value) noexcept { _registers.setValue<GeneralPurposeRegisterPool::StackPointerIndex>(value); }

		forceinline u32 fp() const noexcept { return _registers.getValue<GeneralPurposeRegisterPool::FramePointerIndex>(); }
		forceinline void fp(u32 value) noexcept { _registers.setValue<GeneralPurposeRegisterPool::FramePointerIndex>(value); }

		forceinline u32 at() const noexcept { return _registers.getValue<GeneralPurposeRegisterPool::AssemblerTempIndex>(); }
		forceinline void at(u32 value) noexcept { _registers.setValue<GeneralPurposeRegisterPool::AssemblerTempIndex>(value); }

		// A halfword or word access has to sit on a boundary of its own size. Byte accesses never
		// fault. Returns false when the access is misaligned, having already raised the fault.
		template <typename T>
		forceinline bool checkAlignment(Address address) noexcept
		{
			if constexpr (sizeof(T) <= 1)
			{
				return true;
			}
			else
			{
				if ((address.value() % sizeof(T)) == 0)
					return true;

				triggerInterrupt(InterruptNumber::AlignmentFault);
				return false;
			}
		}

		template <typename T> requires (Integral<T> || FloatingPoint<T>) && (sizeof(T) <= sizeof(u32))
		forceinline T read(Address address) noexcept
		{
			if (_accessObserver)
				_accessObserver(AccessKind::Read, address.value(), static_cast<u32>(sizeof(T)));

			const auto physical = translate(address, MmuAccess::Read);
			if (!physical.has_value())
				return T{};

			// The top 16 MiB of physical address space is never backed by Memory - see MmioBus. A
			// physical address there routes here instead, whether it arrived as-is (paging off) or
			// as the frame a page table happened to map to (paging on): mapping a device's window
			// into a program's own virtual space is then just an ordinary page table entry.
			if (MmioBus::contains(*physical))
			{
				if constexpr (FloatingPoint<T>)
					return std::bit_cast<T>(_mmioBus.read<u32>(*physical));
				else
					return _mmioBus.read<T>(*physical);
			}

			if constexpr (FloatingPoint<T>)
				return _memory.readFloat(*physical);
			else
				return _memory.read<T>(*physical);
		}

		// Returns false when the write would land in the program's own text, having already raised
		// MemoryFault. The caller must abort the instruction, exactly as for a misaligned access.
		forceinline bool checkWritable(Address address, u32 size) noexcept
		{
			const u64 base = address.value();
			if (_textEnd > _textStart && base < _textEnd && base + size > _textStart)
			{
				triggerInterrupt(InterruptNumber::MemoryFault);
				return false;
			}
			return true;
		}

		template <typename T> requires (Integral<T> || FloatingPoint<T>) && (sizeof(T) <= sizeof(u32))
		forceinline void write(Address address, T value) noexcept
		{
			if (_accessObserver)
				_accessObserver(AccessKind::Write, address.value(), static_cast<u32>(sizeof(T)));

			const auto physical = translate(address, MmuAccess::Write);
			if (!physical.has_value())
				return;

			if (MmioBus::contains(*physical))
			{
				if constexpr (FloatingPoint<T>)
					_mmioBus.write<u32>(*physical, std::bit_cast<u32>(value));
				else
					_mmioBus.write<std::make_unsigned_t<T>>(*physical, static_cast<std::make_unsigned_t<T>>(value));
				return;
			}

			if constexpr (FloatingPoint<T>)
				_memory.writeFloat(*physical, value);
			else
				_memory.write(*physical, value);
		}

		template <ExecutionFlag Flag>
		forceinline void flag(bool value) noexcept { _flags.setFlag<Flag>(value); }
		forceinline void carry(bool value) noexcept { _flags.setFlag<ExecutionFlag::Carry>(value); }
		forceinline void zero(bool value) noexcept { _flags.setFlag<ExecutionFlag::Zero>(value); }
		forceinline void sign(bool value) noexcept { _flags.setFlag<ExecutionFlag::Sign>(value); }
		forceinline void overflow(bool value) noexcept { _flags.setFlag<ExecutionFlag::Overflow>(value); }
		forceinline void interrupt(bool value) noexcept { _flags.setFlag<ExecutionFlag::Interrupt>(value); }
		forceinline void halting(bool value) noexcept { _flags.setFlag<ExecutionFlag::Halting>(value); }
		forceinline void trap(bool value) noexcept { _flags.setFlag<ExecutionFlag::Trap>(value); }

		template <ExecutionFlag Flag>
		forceinline bool flag() noexcept { return _flags.get<Flag>(); }
		forceinline bool carry() noexcept { return _flags.carry(); }
		forceinline bool zero() noexcept { return _flags.zero(); }
		forceinline bool sign() noexcept { return _flags.sign(); }
		forceinline bool overflow() noexcept { return _flags.overflow(); }
		forceinline bool interrupt() noexcept { return _flags.interrupt(); }
		forceinline bool halting() noexcept { return _flags.halting(); }
		forceinline bool trap() noexcept { return _flags.trap(); }

		// The stack grows down from the end of memory. Below the unrestricted segment it would run
		// into the BIOS and the interrupt vectors; the checked write already refuses those addresses,
		// so without this the overflow was silent and execution carried on with garbage.
		forceinline bool hasStackRoom(u32 bytes) const noexcept
		{
			// A handler runs on the system stack, whose floor is the top of the program's own.
			const u32 floor = _interruptDepth > 0 ? systemStackFloor() : _stackLimit;
			return sp() >= floor + bytes;
		}

		forceinline bool hasStackData(u32 bytes) const noexcept
		{
			// The ceiling is where the stack started: the top of memory for a handler, and the
			// floor of the system stack for the program, which is where its own stack begins.
			const u64 ceiling = _interruptDepth > 0
				? static_cast<u64>(_memory.size())
				: static_cast<u64>(systemStackFloor());
			return static_cast<u64>(sp()) + bytes <= ceiling;
		}

		// Returns false when the push faulted. The caller must abort the instruction: the fault
		// handler has already redirected the PC, and finishing the instruction would overwrite it.
		template <typename T> requires (Integral<T> || FloatingPoint<T>) && (sizeof(T) <= sizeof(u32))
		forceinline bool push(T value) noexcept
		{
			if (!hasStackRoom(sizeof(T)))
			{
				triggerInterrupt(InterruptNumber::StackOverflow);
				return false;
			}

			// sp moves before the write lands, so a write that page-faults has to put it back: the
			// fault handler's IRET re-runs this same PUSH from the top, and a second decrement of an
			// sp that never actually got written would leave a live word of stack skipped forever.
			const u32 savedSp = sp();
			sp(savedSp - sizeof(T));
			if constexpr (FloatingPoint<T>)
				write<u32>(Address(sp()), std::bit_cast<u32>(value));
			else
				write(Address(sp()), value);

			if (_faulted)
			{
				sp(savedSp);
				return false;
			}
			return true;
		}

		template <typename T> requires (Integral<T> || FloatingPoint<T>) && (sizeof(T) <= sizeof(u32))
		// Empty when the pop faulted; see push() for why the caller has to bail out.
		forceinline std::optional<T> pop() noexcept
		{
			// Popping past the top of memory means the stack is unbalanced: a RET without its CALL,
			// or one POP too many. Same fault class as an overflow.
			if (!hasStackData(sizeof(T)))
			{
				triggerInterrupt(InterruptNumber::StackOverflow);
				return std::nullopt;
			}

			if constexpr (FloatingPoint<T>)
			{
				const T value = std::bit_cast<T>(read<u32>(Address(sp())));
				if (_faulted)
					return std::nullopt; // sp is untouched: the fault handler's IRET retries this pop from scratch.
				sp(sp() + sizeof(T));
				return value;
			}
			else
			{
				const T value = read<T>(Address(sp()));
				if (_faulted)
					return std::nullopt;
				sp(sp() + sizeof(T));
				return value;
			}
		}

		// The same flag rule the bitwise operations use: Zero and Sign from the result, Carry and
		// Overflow cleared, because none of these can carry or overflow. ABS is the exception and
		// sets its own Overflow.
		forceinline void executeResult(const u8 regDest, const u32 result) noexcept
		{
			zero(result == 0);
			sign((result & 0x80000000) != 0);
			carry(false);
			overflow(false);

			setReg(regDest, result);
			advancePC();
		}

		forceinline void executeAbs(const u8 regDest, const u32 value) noexcept
		{
			const i32 signedValue = static_cast<i32>(value);
			// |INT_MIN| is not representable: the result is INT_MIN again, and Overflow says so
			// rather than the machine pretending it answered the question.
			const bool overflowed = value == 0x80000000u;
			const u32 result = overflowed ? value : static_cast<u32>(signedValue < 0 ? -signedValue : signedValue);

			zero(result == 0);
			sign((result & 0x80000000) != 0);
			carry(false);
			overflow(overflowed);

			setReg(regDest, result);
			advancePC();
		}

		static forceinline constexpr u32 rotateLeft(u32 value, u32 amount) noexcept
		{
			amount &= 31u;
			return amount == 0 ? value : ((value << amount) | (value >> (32u - amount)));
		}

		// One bit per category, RISC-V fclass-style but collapsed to a single NaN bit rather than
		// separating quiet from signaling - the standard library does not portably distinguish them.
		// Bit 0: -Infinity  Bit 1: -Normal  Bit 2: -Subnormal  Bit 3: -Zero
		// Bit 4: +Zero      Bit 5: +Subnormal  Bit 6: +Normal  Bit 7: +Infinity  Bit 8: NaN
		static forceinline u32 classifyFloat(f32 value) noexcept
		{
			const bool negative = std::signbit(value);
			switch (std::fpclassify(value))
			{
				case FP_NAN:       return 1u << 8;
				case FP_INFINITE:  return negative ? (1u << 0) : (1u << 7);
				case FP_ZERO:      return negative ? (1u << 3) : (1u << 4);
				case FP_SUBNORMAL: return negative ? (1u << 2) : (1u << 5);
				default:           return negative ? (1u << 1) : (1u << 6); // FP_NORMAL
			}
		}

		forceinline void executeAdd(const u8 regDest, const u32 a, const u32 b) noexcept
		{
			const u64 result = static_cast<u64>(a) + static_cast<u64>(b);

			zero((result & 0xFFFFFFFF) == 0);
			sign((result & 0x80000000) != 0);
			carry(result > 0xFFFFFFFF);
			overflow((~(a ^ b) & (a ^ static_cast<u32>(result))) & 0x80000000);

			setReg(regDest, static_cast<u32>(result));
			advancePC();
		}

		forceinline void executeFloatAdd(const u8 regDest, const f32 a, const f32 b) noexcept
		{
			const f32 result = a + b;
			const auto resultClass = std::fpclassify(result);

			zero(resultClass == FP_ZERO);
			sign(std::signbit(result));
			carry(false);
			overflow(resultClass == FP_INFINITE && std::fpclassify(a) != FP_INFINITE && std::fpclassify(b) != FP_INFINITE);

			setFloatReg(regDest, result);
			advancePC();
		}

		forceinline void executeSub(const u8 regDest, const u32 a, const u32 b) noexcept
		{
			const u64 result = static_cast<u64>(a) - static_cast<u64>(b);

			zero((result & 0xFFFFFFFF) == 0);
			sign((result & 0x80000000) != 0);
			carry(a < b);
			overflow(((a ^ b) & (a ^ static_cast<u32>(result))) & 0x80000000);

			setReg(regDest, static_cast<u32>(result));
			advancePC();
		}

		forceinline void executeFloatSub(const u8 regDest, const f32 a, const f32 b) noexcept
		{
			const f32 result = a - b;
			const auto resultClass = std::fpclassify(result);

			zero(resultClass == FP_ZERO);
			sign(std::signbit(result));
			carry(false);
			overflow(resultClass == FP_INFINITE && std::fpclassify(a) != FP_INFINITE && std::fpclassify(b) != FP_INFINITE);

			setFloatReg(regDest, result);
			advancePC();
		}

        forceinline void executeSignedMul(const u8 regDest, const i32 a, const i32 b) noexcept
		{
			constexpr i64 max32 = static_cast<i64>(std::numeric_limits<i32>::max());
			constexpr i64 min32 = static_cast<i64>(std::numeric_limits<i32>::min());

			const i64 result = static_cast<i64>(a) * static_cast<i64>(b);
			const i32 result32 = static_cast<i32>(result);
			const bool didOverflow = (result > max32 || result < min32);

			zero(result32 == 0);
			sign(result32 < 0);
			carry(didOverflow);
			overflow(didOverflow);

			setReg(regDest, static_cast<u32>(result32));
			advancePC();
		}

		forceinline void executeUnsignedMul(const u8 regDest, const u32 a, const u32 b) noexcept
		{
            const u64 result = static_cast<u64>(a) * static_cast<u64>(b);
			const u32 result32 = static_cast<u32>(result);
			const bool didCarry = result > 0xFFFFFFFF;

			zero(result32 == 0);
			sign((result32 & 0x80000000) != 0);
			carry(didCarry);
			overflow(didCarry);

			setReg(regDest, result32);
			advancePC();
		}

		forceinline void executeFloatMul(const u8 regDest, const f32 a, const f32 b) noexcept
		{
			const f32 result = a * b;
			const auto resultClass = std::fpclassify(result);

			zero(resultClass == FP_ZERO);
			sign(std::signbit(result));
			carry(false);
			overflow(resultClass == FP_INFINITE && std::fpclassify(a) != FP_INFINITE && std::fpclassify(b) != FP_INFINITE);

			setFloatReg(regDest, result);
			advancePC();
		}

		forceinline void executeSignedDiv(const u8 regDest, const i32 a, const i32 b) noexcept
		{
			if (b == 0)
			{
				trap(true);
				advancePC();
				return;
			}

			const i64 result = static_cast<i64>(a) / static_cast<i64>(b);
			const i32 result32 = static_cast<i32>(result);

			zero(result32 == 0);
			sign(result32 < 0);
			carry(false);

			const bool didOverflow = (b == -1 && a == std::numeric_limits<i32>::min());
			overflow(didOverflow);

			setReg(regDest, static_cast<u32>(result32));
			advancePC();
		}

		forceinline void executeUnsignedDiv(const u8 regDest, const u32 a, const u32 b) noexcept
		{
			if (b == 0)
			{
				trap(true);
				advancePC();
				return;
			}

			const u32 result = a / b;

			zero(result == 0);
			sign((result & 0x80000000) != 0);
			carry(false);
			overflow(false);

			setReg(regDest, result);
			advancePC();
		}

		forceinline void executeFloatDiv(const u8 regDest, const f32 a, const f32 b) noexcept
		{
			if (b == 0.0f)
			{
				trap(true);
				advancePC();
				return;
			}

			const f32 result = a / b;
			const auto resultClass = std::fpclassify(result);

			zero(resultClass == FP_ZERO);
			sign(std::signbit(result));
			carry(false);
			overflow(resultClass == FP_INFINITE && std::fpclassify(a) != FP_INFINITE && std::fpclassify(b) != FP_INFINITE);

			setFloatReg(regDest, result);
			advancePC();
		}

		// The float sibling of executeUnsignedMod/executeSignedMod: same trap-on-zero convention as
		// every other division-shaped instruction (see executeFloatDiv), even though IEEE fmod(x, 0)
		// is well-defined as NaN - consistency with the rest of the divide family wins here.
		forceinline void executeFloatMod(const u8 regDest, const f32 a, const f32 b) noexcept
		{
			if (b == 0.0f)
			{
				trap(true);
				advancePC();
				return;
			}

			const f32 result = std::fmod(a, b);
			const auto resultClass = std::fpclassify(result);

			zero(resultClass == FP_ZERO);
			sign(std::signbit(result));
			carry(false);
			overflow(false);

			setFloatReg(regDest, result);
			advancePC();
		}

		forceinline void executeSignedMod(const u8 regDest, const i32 a, const i32 b) noexcept
		{
			if (b == 0)
			{
				trap(true);
				advancePC();
				return;
			}

			// remainder computed in wider type to avoid UB
			const i64 result = static_cast<i64>(a) % static_cast<i64>(b);
			const i32 result32 = static_cast<i32>(result);

			zero(result32 == 0);
			sign(result32 < 0);
			carry(false);
			overflow(false);

			setReg(regDest, static_cast<u32>(result32));
			advancePC();
		}

		forceinline void executeUnsignedMod(const u8 regDest, const u32 a, const u32 b) noexcept
		{
			if (b == 0)
			{
				trap(true);
				advancePC();
				return;
			}

			const u32 result = a % b;

			zero(result == 0);
			sign((result & 0x80000000) != 0);
			carry(false);
			overflow(false);

			setReg(regDest, result);
			advancePC();
		}

		forceinline void executeFloatNeg(const u8 regDest, const f32 a) noexcept
		{
			const f32 result = -a;
			const auto resultClass = std::fpclassify(result);

			zero(resultClass == FP_ZERO);
			sign(std::signbit(result));
			carry(false);
			overflow(resultClass == FP_INFINITE && std::fpclassify(a) != FP_INFINITE);

			setFloatReg(regDest, result);
			advancePC();
		}

		forceinline void executeAnd(const u8 regDest, const u32 a, const u32 b) noexcept
		{
			const u32 result = a & b;

			zero(result == 0);
			sign((result & 0x80000000) != 0);
			carry(false);
			overflow(false);

			setReg(regDest, result);
			advancePC();
		}

		forceinline void executeOr(const u8 regDest, const u32 a, const u32 b) noexcept
		{
			const u32 result = a | b;

			zero(result == 0);
			sign((result & 0x80000000) != 0);
			carry(false);
			overflow(false);

			setReg(regDest, result);
			advancePC();
		}

		forceinline void executeXor(const u8 regDest, const u32 a, const u32 b) noexcept
		{
			const u32 result = a ^ b;

			zero(result == 0);
			sign((result & 0x80000000) != 0);
			carry(false);
			overflow(false);

			setReg(regDest, result);
			advancePC();
		}

		forceinline void executeNot(const u8 regDest, const u32 a) noexcept
		{
			const u32 result = ~a;

			zero(result == 0);
			sign((result & 0x80000000) != 0);
			carry(false);
			overflow(false);

			setReg(regDest, result);
			advancePC();
		}

		forceinline void executeShl(const u8 regDest, const u32 a, const u32 b) noexcept
		{
			if (b == 0)
			{
				// No shift, so no flags are affected
				setReg(regDest, a);
				advancePC();
				return;
			}

			const u32 shiftAmount = b & 0x1F; // Only consider the lower 5 bits for shift amount
			const u32 result = a << shiftAmount;

			zero(result == 0);
			sign((result & 0x80000000) != 0);
			carry((a << (shiftAmount - 1)) & 0x80000000);
			overflow(false);

			setReg(regDest, result);
			advancePC();
		}

		forceinline void executeShr(const u8 regDest, const u32 a, const u32 b) noexcept
		{
			if (b == 0)
			{
				// No shift, so no flags are affected
				setReg(regDest, a);
				advancePC();
				return;
			}

			const u32 shiftAmount = b & 0x1F; // Only consider the lower 5 bits for shift amount
			const u32 result = a >> shiftAmount;

			zero(result == 0);
			sign((result & 0x80000000) != 0);
			carry((a >> (shiftAmount - 1)) & 0x1);
			overflow(false);

			setReg(regDest, result);
			advancePC();
		}

		forceinline void executeSar(const u8 regDest, const u32 a, const u32 b) noexcept
		{
			if (b == 0)
			{
				// No shift, so no flags are affected
				setReg(regDest, a);
				advancePC();
				return;
			}

			const u32 shiftAmount = b & 0x1F; // Only consider the lower 5 bits for shift amount
			const i32 signedA = static_cast<i32>(a);
			const i32 result = signedA >> shiftAmount;

			zero(result == 0);
			sign(result < 0);
			carry((signedA >> (shiftAmount - 1)) & 0x1);
			overflow(false);

			setReg(regDest, static_cast<u32>(result));
			advancePC();
		}

		template <ExecutionFlag Flag>
		forceinline void executeJumpIfFlag(const Instruction inst) noexcept
		{
			if (flag<Flag>())
				_pc += inst.simm24().signedValue();
			else
				advancePC();
		}

		template <ExecutionFlag Flag>
		forceinline void executeJumpRegIfFlag(const Instruction inst) noexcept
		{
			if (flag<Flag>())
				_pc = Address(getReg(inst.rs()));
			else
				advancePC();
		}

		// The comparison jumps read two flags, so they cannot go through the templates above.
		// CMP leaves: Zero = (a == b), Sign = sign bit of (a - b), Carry = (a < b) unsigned,
		// Overflow = signed overflow of (a - b). Signed ordering is Sign against Overflow;
		// unsigned ordering is Carry, with Zero separating < from <=.
		forceinline void executeJumpIf(const Instruction inst, bool condition) noexcept
		{
			if (condition)
				_pc += inst.simm24().signedValue();
			else
				advancePC();
		}

		forceinline void executeJumpRegIf(const Instruction inst, bool condition) noexcept
		{
			if (condition)
				_pc = Address(getReg(inst.rs()));
			else
				advancePC();
		}

		forceinline bool signedLess() noexcept { return flag<ExecutionFlag::Sign>() != flag<ExecutionFlag::Overflow>(); }
		forceinline bool isEqual() noexcept { return flag<ExecutionFlag::Zero>(); }
		forceinline bool unsignedBelow() noexcept { return flag<ExecutionFlag::Carry>(); }

		template <ExecutionFlag Flag>
		forceinline void executeJumpIfNotFlag(const Instruction inst) noexcept
		{
			if (!flag<Flag>())
				_pc += inst.simm24().signedValue();
			else
				advancePC();
		}

		template <ExecutionFlag Flag>
		forceinline void executeJumpRegIfNotFlag(const Instruction inst) noexcept
		{
			if (!flag<Flag>())
				_pc = Address(getReg(inst.rs()));
			else
				advancePC();
		}

	private:
		forceinline void NOP(const Instruction inst) noexcept { advancePC(); }
		forceinline void HALT(const Instruction inst) noexcept { handleHalt(); }
		// Both advance first: the return address has to be the instruction after them, or IRET
		// would land back on the trap and loop forever.
		forceinline void TRAP(const Instruction inst) noexcept { advancePC(); triggerInterrupt(InterruptNumber::Trap); }
		forceinline void RESET(const Instruction inst) noexcept { triggerInterrupt(InterruptNumber::Reset); }
		forceinline void INT(const Instruction inst) noexcept { advancePC(); triggerInterrupt(static_cast<InterruptNumber>(inst.imm8())); }
		// Without these the interrupt flag could never be set, so every user interrupt was
		// unreachable: triggerInterrupt drops numbers >= 16 while the flag is clear.
		forceinline void CLI(const Instruction inst) noexcept { _flags.clear<ExecutionFlag::Interrupt>(); advancePC(); }
		forceinline void STI(const Instruction inst) noexcept { _flags.set<ExecutionFlag::Interrupt>(); advancePC(); }

		// Memory Management Unit. See docs/27-Virtual-Memory-and-Paging.md.
		forceinline void MTP(const Instruction inst) noexcept { _mmu.setPtbr(getReg(inst.rs())); advancePC(); }
		forceinline void MFP(const Instruction inst) noexcept { setReg(inst.rd(), _mmu.ptbr()); advancePC(); }
		forceinline void PGON(const Instruction inst) noexcept { _flags.set<ExecutionFlag::Paging>(); advancePC(); }
		forceinline void PGOFF(const Instruction inst) noexcept { _flags.clear<ExecutionFlag::Paging>(); advancePC(); }
		forceinline void INVLPG(const Instruction inst) noexcept { _mmu.invalidate(Address(getReg(inst.rs()))); advancePC(); }
		forceinline void FLPG(const Instruction inst) noexcept { _mmu.invalidateAll(); advancePC(); }
		forceinline void MFPF(const Instruction inst) noexcept { setReg(inst.rd(), _mmu.faultAddress().value()); advancePC(); }

		// Puts the program's own stack pointer back once the outermost handler is done with it.
		forceinline void leaveInterrupt() noexcept
		{
			if (_interruptDepth == 0)
				return;

			--_interruptDepth;
			if (_interruptDepth == 0)
				sp(_savedStackPointer);
		}

		forceinline void IRET(const Instruction inst) noexcept
		{
			// triggerInterrupt pushes flags and then the PC, so the PC is on top and has to come off
			// first. Popping them the other way round restored the flags word as the program counter:
			// an interrupt taken while halted resumed at address 0x30, the flag bits themselves.
			const auto newPC = pop<u32>();
			if (!newPC.has_value())
				return;
			const auto newFlags = pop<u32>();
			if (!newFlags.has_value())
				return;

			// triggerInterrupt pushes the PC of the instruction that had not run yet, so returning
			// means restoring it exactly. Advancing here skipped that instruction, which is invisible
			// for a software interrupt (INT advances before trapping) and wrong for everything else.
			_pc = Address(*newPC);
			_flags = FlagRegister(*newFlags);

			// After the pops, so the handler's own two words come off the system stack first. A
			// handler that pushed more than it popped is forgiven by this rather than corrupting
			// the stack of the program it interrupted.
			leaveInterrupt();
		}

		forceinline void ADD(const Instruction inst) noexcept { executeAdd(inst.rd(), getReg(inst.rs()), getReg(inst.rt())); }
		forceinline void ADDI(const Instruction inst) noexcept { executeAdd(inst.rd(), getReg(inst.rs()), inst.imm16()); }
		forceinline void ADDC(const Instruction inst) noexcept { executeAdd(inst.rd(), getReg(inst.rs()), getReg(inst.rt()) + (carry() ? 1 : 0)); }
		forceinline void ADDCI(const Instruction inst) noexcept { executeAdd(inst.rd(), getReg(inst.rs()), inst.imm16() + (carry() ? 1 : 0)); }
		forceinline void FADD(const Instruction inst) noexcept { executeFloatAdd(inst.fd(), getFloatReg(inst.fs()), getFloatReg(inst.ft())); }
		forceinline void SUB(const Instruction inst) noexcept { executeSub(inst.rd(), getReg(inst.rs()), getReg(inst.rt())); }
		forceinline void SUBI(const Instruction inst) noexcept { executeSub(inst.rd(), getReg(inst.rs()), inst.imm16()); }
		forceinline void SUBC(const Instruction inst) noexcept { executeSub(inst.rd(), getReg(inst.rs()), getReg(inst.rt()) + (carry() ? 1 : 0)); }
		forceinline void SUBCI(const Instruction inst) noexcept { executeSub(inst.rd(), getReg(inst.rs()), inst.imm16() + (carry() ? 1 : 0)); }
		forceinline void FSUB(const Instruction inst) noexcept { executeFloatSub(inst.fd(), getFloatReg(inst.fs()), getFloatReg(inst.ft())); }
		forceinline void MUL(const Instruction inst) noexcept { executeUnsignedMul(inst.rd(), getReg(inst.rs()), getReg(inst.rt())); }
		forceinline void MULI(const Instruction inst) noexcept { executeUnsignedMul(inst.rd(), getReg(inst.rs()), inst.imm16()); }
		forceinline void IMUL(const Instruction inst) noexcept { executeSignedMul(inst.rd(), getReg(inst.rs()), getReg(inst.rt())); }
		forceinline void IMULI(const Instruction inst) noexcept { executeSignedMul(inst.rd(), getReg(inst.rs()), inst.simm16()); }
		forceinline void FMUL(const Instruction inst) noexcept { executeFloatMul(inst.fd(), getFloatReg(inst.fs()), getFloatReg(inst.ft())); }
		forceinline void DIV(const Instruction inst) noexcept { executeUnsignedDiv(inst.rd(), getReg(inst.rs()), getReg(inst.rt())); }
		forceinline void DIVI(const Instruction inst) noexcept { executeUnsignedDiv(inst.rd(), getReg(inst.rs()), inst.imm16()); }
		forceinline void IDIV(const Instruction inst) noexcept { executeSignedDiv(inst.rd(), getReg(inst.rs()), getReg(inst.rt())); }
		forceinline void IDIVI(const Instruction inst) noexcept { executeSignedDiv(inst.rd(), getReg(inst.rs()), inst.simm16()); }
		forceinline void FDIV(const Instruction inst) noexcept { executeFloatDiv(inst.fd(), getFloatReg(inst.fs()), getFloatReg(inst.ft())); }
		forceinline void MOD(const Instruction inst) noexcept { executeUnsignedMod(inst.rd(), getReg(inst.rs()), getReg(inst.rt())); }
		forceinline void MODI(const Instruction inst) noexcept { executeUnsignedMod(inst.rd(), getReg(inst.rs()), inst.imm16()); }
		forceinline void IMOD(const Instruction inst) noexcept { executeSignedMod(inst.rd(), getReg(inst.rs()), getReg(inst.rt())); }
		forceinline void IMODI(const Instruction inst) noexcept { executeSignedMod(inst.rd(), getReg(inst.rs()), inst.simm16()); }
		forceinline void FMOD(const Instruction inst) noexcept { executeFloatMod(inst.fd(), getFloatReg(inst.fs()), getFloatReg(inst.ft())); }
		forceinline void FNEG(const Instruction inst) noexcept { executeFloatNeg(inst.fd(), getFloatReg(inst.fs())); }

		forceinline void MULH(const Instruction inst) noexcept
		{
			const u64 product = static_cast<u64>(getReg(inst.rs())) * static_cast<u64>(getReg(inst.rt()));
			executeResult(inst.rd(), static_cast<u32>(product >> 32));
		}
		forceinline void IMULH(const Instruction inst) noexcept
		{
			const i64 product = static_cast<i64>(static_cast<i32>(getReg(inst.rs()))) *
				static_cast<i64>(static_cast<i32>(getReg(inst.rt())));
			executeResult(inst.rd(), static_cast<u32>(static_cast<u64>(product) >> 32));
		}
		forceinline void ABS(const Instruction inst) noexcept { executeAbs(inst.rd(), getReg(inst.rs())); }
		forceinline void MIN(const Instruction inst) noexcept { executeResult(inst.rd(), std::min(getReg(inst.rs()), getReg(inst.rt()))); }
		forceinline void MINI(const Instruction inst) noexcept { executeResult(inst.rd(), std::min(getReg(inst.rs()), static_cast<u32>(inst.imm16()))); }
		forceinline void IMIN(const Instruction inst) noexcept { executeResult(inst.rd(), static_cast<u32>(std::min(static_cast<i32>(getReg(inst.rs())), static_cast<i32>(getReg(inst.rt()))))); }
		forceinline void IMINI(const Instruction inst) noexcept { executeResult(inst.rd(), static_cast<u32>(std::min(static_cast<i32>(getReg(inst.rs())), static_cast<i32>(inst.simm16())))); }
		forceinline void MAX(const Instruction inst) noexcept { executeResult(inst.rd(), std::max(getReg(inst.rs()), getReg(inst.rt()))); }
		forceinline void MAXI(const Instruction inst) noexcept { executeResult(inst.rd(), std::max(getReg(inst.rs()), static_cast<u32>(inst.imm16()))); }
		forceinline void IMAX(const Instruction inst) noexcept { executeResult(inst.rd(), static_cast<u32>(std::max(static_cast<i32>(getReg(inst.rs())), static_cast<i32>(getReg(inst.rt()))))); }
		forceinline void IMAXI(const Instruction inst) noexcept { executeResult(inst.rd(), static_cast<u32>(std::max(static_cast<i32>(getReg(inst.rs())), static_cast<i32>(inst.simm16())))); }
		// Light-touch flags, the way AND/OR/XOR set Zero/Sign and always clear Carry/Overflow: there
		// is no carry or overflow to report from picking one of two values that already existed.
		forceinline void executeFloatMinMax(const u8 regDest, const f32 result) noexcept
		{
			zero(std::fpclassify(result) == FP_ZERO);
			sign(std::signbit(result));
			carry(false);
			overflow(false);
			setFloatReg(regDest, result);
			advancePC();
		}
		forceinline void FMIN(const Instruction inst) noexcept { executeFloatMinMax(inst.fd(), std::fmin(getFloatReg(inst.fs()), getFloatReg(inst.ft()))); }
		forceinline void FMAX(const Instruction inst) noexcept { executeFloatMinMax(inst.fd(), std::fmax(getFloatReg(inst.fs()), getFloatReg(inst.ft()))); }
		forceinline void CLZ(const Instruction inst) noexcept { executeResult(inst.rd(), static_cast<u32>(std::countl_zero(getReg(inst.rs())))); }
		forceinline void CTZ(const Instruction inst) noexcept { executeResult(inst.rd(), static_cast<u32>(std::countr_zero(getReg(inst.rs())))); }
		forceinline void POPCNT(const Instruction inst) noexcept { executeResult(inst.rd(), static_cast<u32>(std::popcount(getReg(inst.rs())))); }
		forceinline void BSWAP(const Instruction inst) noexcept { executeResult(inst.rd(), std::byteswap(getReg(inst.rs()))); }
		forceinline void ROL(const Instruction inst) noexcept { executeResult(inst.rd(), rotateLeft(getReg(inst.rs()), getReg(inst.rt()))); }
		forceinline void ROLI(const Instruction inst) noexcept { executeResult(inst.rd(), rotateLeft(getReg(inst.rs()), inst.imm16())); }
		forceinline void ROR(const Instruction inst) noexcept { executeResult(inst.rd(), rotateLeft(getReg(inst.rs()), 32u - (getReg(inst.rt()) & 31u))); }
		forceinline void RORI(const Instruction inst) noexcept { executeResult(inst.rd(), rotateLeft(getReg(inst.rs()), 32u - (static_cast<u32>(inst.imm16()) & 31u))); }
		forceinline void SXTB(const Instruction inst) noexcept { executeResult(inst.rd(), static_cast<u32>(static_cast<i32>(static_cast<i8>(getReg(inst.rs()) & 0xFFu)))); }
		forceinline void SXTH(const Instruction inst) noexcept { executeResult(inst.rd(), static_cast<u32>(static_cast<i32>(static_cast<i16>(getReg(inst.rs()) & 0xFFFFu)))); }
		forceinline void FSQRT(const Instruction inst) noexcept { setFloatReg(inst.fd(), std::sqrt(getFloatReg(inst.fs()))); advancePC(); }
		forceinline void FABS(const Instruction inst) noexcept { setFloatReg(inst.fd(), std::fabs(getFloatReg(inst.fs()))); advancePC(); }
		// Rounding, sign injection and the multiply-accumulate a software math library needs for
		// polynomial evaluation. None of these touch the flags register, the same as FSQRT/FABS
		// above: they hand back an unambiguous float and there is nothing a flag would add.
		forceinline void FROUND(const Instruction inst) noexcept { setFloatReg(inst.fd(), std::nearbyint(getFloatReg(inst.fs()))); advancePC(); }
		forceinline void FFLOOR(const Instruction inst) noexcept { setFloatReg(inst.fd(), std::floor(getFloatReg(inst.fs()))); advancePC(); }
		forceinline void FCEIL(const Instruction inst) noexcept { setFloatReg(inst.fd(), std::ceil(getFloatReg(inst.fs()))); advancePC(); }
		forceinline void FTRUNC(const Instruction inst) noexcept { setFloatReg(inst.fd(), std::trunc(getFloatReg(inst.fs()))); advancePC(); }
		forceinline void FCOPYSIGN(const Instruction inst) noexcept { setFloatReg(inst.fd(), std::copysign(getFloatReg(inst.fs()), getFloatReg(inst.ft()))); advancePC(); }
		// fd is read as the accumulator as well as written, so this is exactly FADD's flag/rounding
		// behaviour applied to (fd, fs * ft) - reusing executeFloatAdd instead of duplicating it.
		forceinline void FMA(const Instruction inst) noexcept { executeFloatAdd(inst.fd(), getFloatReg(inst.fd()), getFloatReg(inst.fs()) * getFloatReg(inst.ft())); }
		forceinline void FCLASS(const Instruction inst) noexcept { setReg(inst.rd(), classifyFloat(getFloatReg(inst.fs()))); advancePC(); }
		// Estimates in name only: a software-interpreted VM has no cycle cost to save by answering
		// approximately, so these give an exact reciprocal rather than faking the low precision a
		// real FPU's lookup-table hardware would produce. They keep the trap-on-zero convention the
		// rest of the divide family uses, since 1/0 is exactly the case that family already guards.
		forceinline void FRECIPE(const Instruction inst) noexcept
		{
			const f32 value = getFloatReg(inst.fs());
			if (value == 0.0f)
			{
				trap(true);
				advancePC();
				return;
			}
			setFloatReg(inst.fd(), 1.0f / value);
			advancePC();
		}
		forceinline void FRSQRTE(const Instruction inst) noexcept
		{
			const f32 value = getFloatReg(inst.fs());
			if (value == 0.0f)
			{
				trap(true);
				advancePC();
				return;
			}
			setFloatReg(inst.fd(), 1.0f / std::sqrt(value));
			advancePC();
		}

		forceinline void AND(const Instruction inst) noexcept { executeAnd(inst.rd(), getReg(inst.rs()), getReg(inst.rt())); }
		forceinline void ANDI(const Instruction inst) noexcept { executeAnd(inst.rd(), getReg(inst.rs()), inst.imm16()); }
		forceinline void OR(const Instruction inst) noexcept { executeOr(inst.rd(), getReg(inst.rs()), getReg(inst.rt())); }
		forceinline void ORI(const Instruction inst) noexcept { executeOr(inst.rd(), getReg(inst.rs()), inst.imm16()); }
		forceinline void XOR(const Instruction inst) noexcept { executeXor(inst.rd(), getReg(inst.rs()), getReg(inst.rt())); }
		forceinline void XORI(const Instruction inst) noexcept { executeXor(inst.rd(), getReg(inst.rs()), inst.imm16()); }
		forceinline void NOT(const Instruction inst) noexcept { executeNot(inst.rd(), getReg(inst.rs())); }
		forceinline void SHL(const Instruction inst) noexcept { executeShl(inst.rd(), getReg(inst.rs()), getReg(inst.rt())); }
		forceinline void SHLI(const Instruction inst) noexcept { executeShl(inst.rd(), getReg(inst.rs()), inst.imm16()); }
		forceinline void SHR(const Instruction inst) noexcept { executeShr(inst.rd(), getReg(inst.rs()), getReg(inst.rt())); }
		forceinline void SHRI(const Instruction inst) noexcept { executeShr(inst.rd(), getReg(inst.rs()), inst.imm16()); }
		forceinline void SAR(const Instruction inst) noexcept { executeSar(inst.rd(), getReg(inst.rs()), getReg(inst.rt())); }
		forceinline void SARI(const Instruction inst) noexcept { executeSar(inst.rd(), getReg(inst.rs()), inst.imm16()); }

		forceinline void MOV(const Instruction inst) noexcept { setReg(inst.rd(), getReg(inst.rs())); advancePC(); }
		forceinline void FMOV(const Instruction inst) noexcept { setFloatReg(inst.fd(), getFloatReg(inst.fs())); advancePC(); }
		forceinline void LI(const Instruction inst) noexcept { setReg(inst.rd(), inst.imm16()); advancePC(); }
		forceinline void LUI(const Instruction inst) noexcept { setReg(inst.rd(), static_cast<u32>(inst.imm16()) << 16u); advancePC(); }
		// A memory displacement is signed: `[fp - 8]` has to reach eight bytes below the base, not
		// 65528 above it. Widened through i32 and folded back to u32 so the addition wraps the way
		// a 32-bit address space does.
		static forceinline constexpr u32 displacement(const Instruction inst) noexcept
		{
			return static_cast<u32>(static_cast<i32>(inst.simm16()));
		}

		forceinline void LDR(const Instruction inst) noexcept
		{
			const Address address = getReg(inst.rs()) + displacement(inst);
			if (!checkAlignment<u32>(address))
				return;
			setReg(inst.rd(), read<u32>(address));
			advancePC();
		}
		forceinline void LDRB(const Instruction inst) noexcept { setReg(inst.rd(), read<u8>(getReg(inst.rs()) + displacement(inst))); advancePC(); }
		forceinline void LDRH(const Instruction inst) noexcept
		{
			const Address address = getReg(inst.rs()) + displacement(inst);
			if (!checkAlignment<u16>(address))
				return;
			setReg(inst.rd(), read<u16>(address));
			advancePC();
		}
		forceinline void LDRSB(const Instruction inst) noexcept { setReg(inst.rd(), static_cast<u32>(read<i8>(getReg(inst.rs()) + displacement(inst)))); advancePC(); }
		forceinline void LDRSH(const Instruction inst) noexcept
		{
			const Address address = getReg(inst.rs()) + displacement(inst);
			if (!checkAlignment<i16>(address))
				return;
			setReg(inst.rd(), static_cast<u32>(read<i16>(address)));
			advancePC();
		}
		forceinline void FLDR(const Instruction inst) noexcept
		{
			const Address address = getReg(inst.rs()) + displacement(inst);
			if (!checkAlignment<f32>(address))
				return;
			setFloatReg(inst.fd(), read<f32>(address));
			advancePC();
		}
		forceinline void STR(const Instruction inst) noexcept
		{
			const Address address = getReg(inst.rd()) + displacement(inst);
			if (!checkAlignment<u32>(address) || !checkWritable(address, sizeof(u32)))
				return;
			write<u32>(address, getReg(inst.rs()));
			advancePC();
		}
		forceinline void STRB(const Instruction inst) noexcept
		{
			const Address address = getReg(inst.rd()) + displacement(inst);
			if (!checkWritable(address, sizeof(u8)))
				return;
			write<u8>(address, static_cast<u8>(getReg(inst.rs())));
			advancePC();
		}
		forceinline void STRH(const Instruction inst) noexcept
		{
			const Address address = getReg(inst.rd()) + displacement(inst);
			if (!checkAlignment<u16>(address) || !checkWritable(address, sizeof(u16)))
				return;
			write<u16>(address, static_cast<u16>(getReg(inst.rs())));
			advancePC();
		}
		forceinline void FSTR(const Instruction inst) noexcept
		{
			const Address address = getReg(inst.rd()) + displacement(inst);
			if (!checkAlignment<f32>(address) || !checkWritable(address, sizeof(f32)))
				return;
			write<f32>(address, getFloatReg(inst.fs()));
			advancePC();
		}
		// Indexed forms. The address is two registers added at run time, so there is no
		// displacement to range-check and no ADD to write before every access.
		forceinline Address indexed(const Instruction inst) const noexcept { return Address(getReg(inst.rs()) + getReg(inst.rt())); }
		forceinline Address indexedStore(const Instruction inst) const noexcept { return Address(getReg(inst.rd()) + getReg(inst.rt())); }

		forceinline void LDRX(const Instruction inst) noexcept
		{
			const Address address = indexed(inst);
			if (!checkAlignment<u32>(address))
				return;
			setReg(inst.rd(), read<u32>(address));
			advancePC();
		}
		forceinline void LDRBX(const Instruction inst) noexcept { setReg(inst.rd(), read<u8>(indexed(inst))); advancePC(); }
		forceinline void LDRHX(const Instruction inst) noexcept
		{
			const Address address = indexed(inst);
			if (!checkAlignment<u16>(address))
				return;
			setReg(inst.rd(), read<u16>(address));
			advancePC();
		}
		forceinline void LDRSBX(const Instruction inst) noexcept { setReg(inst.rd(), static_cast<u32>(read<i8>(indexed(inst)))); advancePC(); }
		forceinline void LDRSHX(const Instruction inst) noexcept
		{
			const Address address = indexed(inst);
			if (!checkAlignment<i16>(address))
				return;
			setReg(inst.rd(), static_cast<u32>(read<i16>(address)));
			advancePC();
		}
		forceinline void FLDRX(const Instruction inst) noexcept
		{
			const Address address = indexed(inst);
			if (!checkAlignment<f32>(address))
				return;
			setFloatReg(inst.fd(), read<f32>(address));
			advancePC();
		}
		forceinline void STRX(const Instruction inst) noexcept
		{
			const Address address = indexedStore(inst);
			if (!checkAlignment<u32>(address) || !checkWritable(address, sizeof(u32)))
				return;
			write<u32>(address, getReg(inst.rs()));
			advancePC();
		}
		forceinline void STRBX(const Instruction inst) noexcept
		{
			const Address address = indexedStore(inst);
			if (!checkWritable(address, sizeof(u8)))
				return;
			write<u8>(address, static_cast<u8>(getReg(inst.rs())));
			advancePC();
		}
		forceinline void STRHX(const Instruction inst) noexcept
		{
			const Address address = indexedStore(inst);
			if (!checkAlignment<u16>(address) || !checkWritable(address, sizeof(u16)))
				return;
			write<u16>(address, static_cast<u16>(getReg(inst.rs())));
			advancePC();
		}
		forceinline void FSTRX(const Instruction inst) noexcept
		{
			const Address address = indexedStore(inst);
			if (!checkAlignment<f32>(address) || !checkWritable(address, sizeof(f32)))
				return;
			write<f32>(address, getFloatReg(inst.fs()));
			advancePC();
		}
		// The displacement is measured from the instruction itself, like a branch's, so the address
		// is formed without materialising anything and without borrowing a register to hold it.
		forceinline Address pcRelative(const Instruction inst) const noexcept
		{
			return Address(static_cast<u32>(static_cast<i32>(_pc.value()) + inst.simm16()));
		}

		forceinline void LDRP(const Instruction inst) noexcept
		{
			const Address address = pcRelative(inst);
			if (!checkAlignment<u32>(address))
				return;
			setReg(inst.rd(), read<u32>(address));
			advancePC();
		}
		forceinline void LDRBP(const Instruction inst) noexcept { setReg(inst.rd(), read<u8>(pcRelative(inst))); advancePC(); }
		forceinline void LDRHP(const Instruction inst) noexcept
		{
			const Address address = pcRelative(inst);
			if (!checkAlignment<u16>(address))
				return;
			setReg(inst.rd(), read<u16>(address));
			advancePC();
		}
		forceinline void LDRSBP(const Instruction inst) noexcept { setReg(inst.rd(), static_cast<u32>(read<i8>(pcRelative(inst)))); advancePC(); }
		forceinline void LDRSHP(const Instruction inst) noexcept
		{
			const Address address = pcRelative(inst);
			if (!checkAlignment<i16>(address))
				return;
			setReg(inst.rd(), static_cast<u32>(read<i16>(address)));
			advancePC();
		}
		forceinline void FLDRP(const Instruction inst) noexcept
		{
			const Address address = pcRelative(inst);
			if (!checkAlignment<f32>(address))
				return;
			setFloatReg(inst.fd(), read<f32>(address));
			advancePC();
		}
		forceinline void STRP(const Instruction inst) noexcept
		{
			const Address address = pcRelative(inst);
			if (!checkAlignment<u32>(address) || !checkWritable(address, sizeof(u32)))
				return;
			write<u32>(address, getReg(inst.rs()));
			advancePC();
		}
		forceinline void STRBP(const Instruction inst) noexcept
		{
			const Address address = pcRelative(inst);
			if (!checkWritable(address, sizeof(u8)))
				return;
			write<u8>(address, static_cast<u8>(getReg(inst.rs())));
			advancePC();
		}
		forceinline void STRHP(const Instruction inst) noexcept
		{
			const Address address = pcRelative(inst);
			if (!checkAlignment<u16>(address) || !checkWritable(address, sizeof(u16)))
				return;
			write<u16>(address, static_cast<u16>(getReg(inst.rs())));
			advancePC();
		}
		forceinline void FSTRP(const Instruction inst) noexcept
		{
			const Address address = pcRelative(inst);
			if (!checkAlignment<f32>(address) || !checkWritable(address, sizeof(f32)))
				return;
			write<f32>(address, getFloatReg(inst.fs()));
			advancePC();
		}
		forceinline void LEA(const Instruction inst) noexcept { setReg(inst.rd(), getReg(inst.rs()) + displacement(inst)); advancePC(); }

		forceinline void JP(const Instruction inst) noexcept { _pc += inst.simm24().signedValue(); }
		forceinline void JPR(const Instruction inst) noexcept { _pc = Address(getReg(inst.rs())); }
		forceinline void CMP(const Instruction inst) noexcept
		{
			const u32 a = getReg(inst.rs());
			const u32 b = getReg(inst.rt());

			zero(a == b);
			sign((static_cast<i32>(a) - static_cast<i32>(b)) < 0);
			carry(a < b);
			overflow(((a ^ b) & (a ^ (a - b))) & 0x80000000);

			advancePC();
		}
		forceinline void CMPI(const Instruction inst) noexcept
		{
			const u32 a = getReg(inst.rs());
			const u32 b = static_cast<u32>(inst.simm16());

			zero(a == b);
			sign((static_cast<i32>(a) - static_cast<i32>(b)) < 0);
			carry(a < b);
			overflow(((a ^ b) & (a ^ (a - b))) & 0x80000000);

			advancePC();
		}
		forceinline void FCMP(const Instruction inst) noexcept
		{
			const f32 a = getFloatReg(inst.fs());
			const f32 b = getFloatReg(inst.ft());

			zero(a == b);
			sign(std::signbit(a - b));
			carry(a < b);
			overflow(false); // Overflow doesn't really make sense for floating-point comparisons

			advancePC();
		}
		forceinline void JZ(const Instruction inst) noexcept { executeJumpIfFlag<ExecutionFlag::Zero>(inst); }
		forceinline void JZR(const Instruction inst) noexcept { executeJumpRegIfFlag<ExecutionFlag::Zero>(inst); }
		forceinline void JNZ(const Instruction inst) noexcept { executeJumpIfNotFlag<ExecutionFlag::Zero>(inst); }
		forceinline void JNZR(const Instruction inst) noexcept { executeJumpRegIfNotFlag<ExecutionFlag::Zero>(inst); }
		forceinline void JC(const Instruction inst) noexcept { executeJumpIfFlag<ExecutionFlag::Carry>(inst); }
		forceinline void JCR(const Instruction inst) noexcept { executeJumpRegIfFlag<ExecutionFlag::Carry>(inst); }
		forceinline void JNC(const Instruction inst) noexcept { executeJumpIfNotFlag<ExecutionFlag::Carry>(inst); }
		forceinline void JNCR(const Instruction inst) noexcept { executeJumpRegIfNotFlag<ExecutionFlag::Carry>(inst); }
		forceinline void JS(const Instruction inst) noexcept { executeJumpIfFlag<ExecutionFlag::Sign>(inst); }
		forceinline void JSR(const Instruction inst) noexcept { executeJumpRegIfFlag<ExecutionFlag::Sign>(inst); }
		forceinline void JNS(const Instruction inst) noexcept { executeJumpIfNotFlag<ExecutionFlag::Sign>(inst); }
		forceinline void JNSR(const Instruction inst) noexcept { executeJumpRegIfNotFlag<ExecutionFlag::Sign>(inst); }
		forceinline void JO(const Instruction inst) noexcept { executeJumpIfFlag<ExecutionFlag::Overflow>(inst); }
		forceinline void JOR(const Instruction inst) noexcept { executeJumpRegIfFlag<ExecutionFlag::Overflow>(inst); }
		forceinline void JNO(const Instruction inst) noexcept { executeJumpIfNotFlag<ExecutionFlag::Overflow>(inst); }
		forceinline void JNOR(const Instruction inst) noexcept { executeJumpRegIfNotFlag<ExecutionFlag::Overflow>(inst); }

		forceinline void JGR(const Instruction inst) noexcept { executeJumpIf(inst, !isEqual() && !signedLess()); }
		forceinline void JGRR(const Instruction inst) noexcept { executeJumpRegIf(inst, !isEqual() && !signedLess()); }
		forceinline void JGE(const Instruction inst) noexcept { executeJumpIf(inst, !signedLess()); }
		forceinline void JGER(const Instruction inst) noexcept { executeJumpRegIf(inst, !signedLess()); }
		forceinline void JLS(const Instruction inst) noexcept { executeJumpIf(inst, signedLess()); }
		forceinline void JLSR(const Instruction inst) noexcept { executeJumpRegIf(inst, signedLess()); }
		forceinline void JLE(const Instruction inst) noexcept { executeJumpIf(inst, isEqual() || signedLess()); }
		forceinline void JLER(const Instruction inst) noexcept { executeJumpRegIf(inst, isEqual() || signedLess()); }

		forceinline void JAB(const Instruction inst) noexcept { executeJumpIf(inst, !unsignedBelow() && !isEqual()); }
		forceinline void JABR(const Instruction inst) noexcept { executeJumpRegIf(inst, !unsignedBelow() && !isEqual()); }
		forceinline void JAE(const Instruction inst) noexcept { executeJumpIf(inst, !unsignedBelow()); }
		forceinline void JAER(const Instruction inst) noexcept { executeJumpRegIf(inst, !unsignedBelow()); }
		forceinline void JBL(const Instruction inst) noexcept { executeJumpIf(inst, unsignedBelow()); }
		forceinline void JBLR(const Instruction inst) noexcept { executeJumpRegIf(inst, unsignedBelow()); }
		forceinline void JBE(const Instruction inst) noexcept { executeJumpIf(inst, unsignedBelow() || isEqual()); }
		forceinline void JBER(const Instruction inst) noexcept { executeJumpRegIf(inst, unsignedBelow() || isEqual()); }
		forceinline void CALL(const Instruction inst) noexcept
		{
			if (!push<u32>((_pc + Instruction::SizeInBytes).value())) // Push return address onto the stack
				return;
			// signedValue() sign-extends; Address(i24) would zero-extend and send a backward call
			// roughly 16 MiB forward.
			_pc += inst.simm24().signedValue(); // Jump to target address
		}
		forceinline void CALLR(const Instruction inst) noexcept
		{
			if (!push<u32>((_pc + Instruction::SizeInBytes).value())) // Push return address onto the stack
				return;
			_pc = Address(getReg(inst.rs())); // Jump to target address
		}
		// The return address is written before the jump, so `bl r1, self` is a call to itself that
		// still comes back. Nothing is pushed, so nothing can overflow.
		forceinline void BL(const Instruction inst) noexcept
		{
			const u32 returnAddress = (_pc + Instruction::SizeInBytes).value();
			const i32 displacement = inst.simm20();
			setReg(inst.rd(), returnAddress);
			_pc += displacement;
		}
		forceinline void BLR(const Instruction inst) noexcept
		{
			const u32 returnAddress = (_pc + Instruction::SizeInBytes).value();
			const u32 target = getReg(inst.rs()); // read first: rd and rs may be the same register
			setReg(inst.rd(), returnAddress);
			_pc = Address(target);
		}
		forceinline void RET(const Instruction inst) noexcept
		{
			if (const auto target = pop<u32>())
				_pc = Address(*target);
		}

		forceinline void PUSH(const Instruction inst) noexcept { if (push<u32>(getReg(inst.rs()))) advancePC(); }
		forceinline void POP(const Instruction inst) noexcept { if (const auto v = pop<u32>()) { setReg(inst.rd(), *v); advancePC(); } }
		forceinline void PUSHF(const Instruction inst) noexcept { if (push<u32>(_flags.value())) advancePC(); }
		forceinline void POPF(const Instruction inst) noexcept { if (const auto v = pop<u32>()) { _flags = *v; advancePC(); } }
		// All or nothing. A half-saved frame is worse than one that never started: the epilogue's
		// POPM would restore the wrong registers out of the wrong slots and carry on as if it had
		// worked, so the room for the whole mask is checked before the first word goes down.
		forceinline void PUSHM(const Instruction inst) noexcept
		{
			const u16 mask = inst.imm16();
			const u32 words = static_cast<u32>(std::popcount(mask));
			if (!hasStackRoom(words * static_cast<u32>(sizeof(u32))))
			{
				triggerInterrupt(InterruptNumber::StackOverflow);
				return;
			}

			for (u32 i = GeneralPurposeRegisterPool::Count; i-- > 0;)
			{
				if (mask & (1u << i))
				{
					// The whole-mask room check above only rules out StackOverflow; a page fault is a
					// second, independent way a push here can fail, and this loop has to stop the
					// moment one does; a push already made this discipline to itself (see push<T>), but
					// nothing stopped this loop from calling it again for the next register in the
					// mask, onto memory that instruction was never actually supposed to touch.
					if (!push<u32>(getReg(static_cast<u8>(i))))
						break;
				}
			}
			advancePC();
		}
		forceinline void POPM(const Instruction inst) noexcept
		{
			const u16 mask = inst.imm16();
			const u32 words = static_cast<u32>(std::popcount(mask));
			if (!hasStackData(words * static_cast<u32>(sizeof(u32))))
			{
				triggerInterrupt(InterruptNumber::StackOverflow);
				return;
			}

			for (u32 i = 0; i < GeneralPurposeRegisterPool::Count; ++i)
			{
				if (mask & (1u << i))
				{
					const auto value = pop<u32>();
					if (!value.has_value())
						break; // Same reasoning as PUSHM's loop: stop at the first fault, don't paper over it.
					setReg(static_cast<u8>(i), *value);
				}
			}
			advancePC();
		}
		// The saved fp and the frame are one indivisible step: a prologue that pushed fp and then
		// found no room for the frame would leave a function running on a stack it does not own.
		forceinline void ENTER(const Instruction inst) noexcept
		{
			const u32 frameSize = inst.imm16();
			if (!hasStackRoom(static_cast<u32>(sizeof(u32)) + frameSize))
			{
				triggerInterrupt(InterruptNumber::StackOverflow);
				return;
			}

			(void)push<u32>(fp());
			fp(sp());
			sp(sp() - frameSize);
			advancePC();
		}
		forceinline void LEAVE(const Instruction inst) noexcept
		{
			sp(fp());
			if (const auto saved = pop<u32>())
			{
				fp(*saved);
				advancePC();
			}
		}
		forceinline void FPUSH(const Instruction inst) noexcept { if (push<f32>(getFloatReg(inst.fs()))) advancePC(); }
		forceinline void FPOP(const Instruction inst) noexcept { if (const auto v = pop<f32>()) { setFloatReg(inst.fd(), *v); advancePC(); } }

		forceinline void ITOF(const Instruction inst) noexcept { setFloatReg(inst.fd(), static_cast<f32>(getReg(inst.rs()))); advancePC(); }
		forceinline void IITOF(const Instruction inst) noexcept { setFloatReg(inst.fd(), static_cast<f32>(static_cast<i32>(getReg(inst.rs())))); advancePC(); }
		forceinline void FTOI(const Instruction inst) noexcept { setReg(inst.rd(), static_cast<u32>(getFloatReg(inst.fs()))); advancePC(); }
		forceinline void FTOII(const Instruction inst) noexcept { setReg(inst.rd(), static_cast<u32>(static_cast<i32>(getFloatReg(inst.fs())))); advancePC(); }
		forceinline void MTF(const Instruction inst) noexcept { _fregisters[inst.fd()].setBitsFromRegister(_registers[inst.rs()]); advancePC(); }
		forceinline void MFF(const Instruction inst) noexcept { _registers.set(inst.rd(), _fregisters[inst.fs()].bitsAsRegister()); advancePC(); }

		forceinline void INVALID(const Instruction inst) noexcept { triggerInterrupt(InterruptNumber::IllegalInstruction); }

	private:
		using InstructionHandler = void (ExecutionEngine::*)(const Instruction) noexcept;
		static inline constexpr std::array<InstructionHandler, 256> InstructionHandlers = []() consteval noexcept -> std::array<InstructionHandler, 256>
		{
				// Aggregate initialisation with a single element only assigns index 0; every other
				// slot would stay null and calling one is a crash, not an illegal-instruction trap.
				std::array<InstructionHandler, 256> handlers{};
				handlers.fill(&ExecutionEngine::INVALID);

				// Control
				handlers[static_cast<u8>(Opcode::NOP)] = &ExecutionEngine::NOP;
				handlers[static_cast<u8>(Opcode::HALT)] = &ExecutionEngine::HALT;
				handlers[static_cast<u8>(Opcode::TRAP)] = &ExecutionEngine::TRAP;
				handlers[static_cast<u8>(Opcode::RESET)] = &ExecutionEngine::RESET;
				handlers[static_cast<u8>(Opcode::INT)] = &ExecutionEngine::INT;
				handlers[static_cast<u8>(Opcode::IRET)] = &ExecutionEngine::IRET;
				handlers[static_cast<u8>(Opcode::CLI)] = &ExecutionEngine::CLI;
				handlers[static_cast<u8>(Opcode::STI)] = &ExecutionEngine::STI;

				// MMU
				handlers[static_cast<u8>(Opcode::MTP)] = &ExecutionEngine::MTP;
				handlers[static_cast<u8>(Opcode::MFP)] = &ExecutionEngine::MFP;
				handlers[static_cast<u8>(Opcode::PGON)] = &ExecutionEngine::PGON;
				handlers[static_cast<u8>(Opcode::PGOFF)] = &ExecutionEngine::PGOFF;
				handlers[static_cast<u8>(Opcode::INVLPG)] = &ExecutionEngine::INVLPG;
				handlers[static_cast<u8>(Opcode::FLPG)] = &ExecutionEngine::FLPG;
				handlers[static_cast<u8>(Opcode::MFPF)] = &ExecutionEngine::MFPF;

				// Arithmetic
				handlers[static_cast<u8>(Opcode::ADD)] = &ExecutionEngine::ADD;
				handlers[static_cast<u8>(Opcode::ADDI)] = &ExecutionEngine::ADDI;
				handlers[static_cast<u8>(Opcode::ADDC)] = &ExecutionEngine::ADDC;
				handlers[static_cast<u8>(Opcode::ADDCI)] = &ExecutionEngine::ADDCI;
				handlers[static_cast<u8>(Opcode::FADD)] = &ExecutionEngine::FADD;
				handlers[static_cast<u8>(Opcode::SUB)] = &ExecutionEngine::SUB;
				handlers[static_cast<u8>(Opcode::SUBI)] = &ExecutionEngine::SUBI;
				handlers[static_cast<u8>(Opcode::SUBC)] = &ExecutionEngine::SUBC;
				handlers[static_cast<u8>(Opcode::SUBCI)] = &ExecutionEngine::SUBCI;
				handlers[static_cast<u8>(Opcode::FSUB)] = &ExecutionEngine::FSUB;
				handlers[static_cast<u8>(Opcode::MUL)] = &ExecutionEngine::MUL;
				handlers[static_cast<u8>(Opcode::MULI)] = &ExecutionEngine::MULI;
				handlers[static_cast<u8>(Opcode::IMUL)] = &ExecutionEngine::IMUL;
				handlers[static_cast<u8>(Opcode::IMULI)] = &ExecutionEngine::IMULI;
				handlers[static_cast<u8>(Opcode::FMUL)] = &ExecutionEngine::FMUL;
				handlers[static_cast<u8>(Opcode::DIV)] = &ExecutionEngine::DIV;
				handlers[static_cast<u8>(Opcode::DIVI)] = &ExecutionEngine::DIVI;
				handlers[static_cast<u8>(Opcode::IDIV)] = &ExecutionEngine::IDIV;
				handlers[static_cast<u8>(Opcode::IDIVI)] = &ExecutionEngine::IDIVI;
				handlers[static_cast<u8>(Opcode::FDIV)] = &ExecutionEngine::FDIV;
				handlers[static_cast<u8>(Opcode::MOD)] = &ExecutionEngine::MOD;
				handlers[static_cast<u8>(Opcode::MODI)] = &ExecutionEngine::MODI;
				handlers[static_cast<u8>(Opcode::IMOD)] = &ExecutionEngine::IMOD;
				handlers[static_cast<u8>(Opcode::IMODI)] = &ExecutionEngine::IMODI;
				handlers[static_cast<u8>(Opcode::FMOD)] = &ExecutionEngine::FMOD;
				handlers[static_cast<u8>(Opcode::FNEG)] = &ExecutionEngine::FNEG;

				// Logical
				handlers[static_cast<u8>(Opcode::AND)] = &ExecutionEngine::AND;
				handlers[static_cast<u8>(Opcode::ANDI)] = &ExecutionEngine::ANDI;
				handlers[static_cast<u8>(Opcode::OR)] = &ExecutionEngine::OR;
				handlers[static_cast<u8>(Opcode::ORI)] = &ExecutionEngine::ORI;
				handlers[static_cast<u8>(Opcode::XOR)] = &ExecutionEngine::XOR;
				handlers[static_cast<u8>(Opcode::XORI)] = &ExecutionEngine::XORI;
				handlers[static_cast<u8>(Opcode::NOT)] = &ExecutionEngine::NOT;

				// Shifts
				handlers[static_cast<u8>(Opcode::SHL)] = &ExecutionEngine::SHL;
				handlers[static_cast<u8>(Opcode::SHLI)] = &ExecutionEngine::SHLI;
				handlers[static_cast<u8>(Opcode::SHR)] = &ExecutionEngine::SHR;
				handlers[static_cast<u8>(Opcode::SHRI)] = &ExecutionEngine::SHRI;
				handlers[static_cast<u8>(Opcode::SAR)] = &ExecutionEngine::SAR;
				handlers[static_cast<u8>(Opcode::SARI)] = &ExecutionEngine::SARI;

				// Moves / Loads / Stores
				handlers[static_cast<u8>(Opcode::MOV)] = &ExecutionEngine::MOV;
				handlers[static_cast<u8>(Opcode::FMOV)] = &ExecutionEngine::FMOV;
				handlers[static_cast<u8>(Opcode::LI)] = &ExecutionEngine::LI;
				handlers[static_cast<u8>(Opcode::LUI)] = &ExecutionEngine::LUI;
				handlers[static_cast<u8>(Opcode::LDR)] = &ExecutionEngine::LDR;
				handlers[static_cast<u8>(Opcode::LDRB)] = &ExecutionEngine::LDRB;
				handlers[static_cast<u8>(Opcode::LDRH)] = &ExecutionEngine::LDRH;
				handlers[static_cast<u8>(Opcode::LDRSB)] = &ExecutionEngine::LDRSB;
				handlers[static_cast<u8>(Opcode::LDRSH)] = &ExecutionEngine::LDRSH;
				handlers[static_cast<u8>(Opcode::FLDR)] = &ExecutionEngine::FLDR;
				handlers[static_cast<u8>(Opcode::STR)] = &ExecutionEngine::STR;
				handlers[static_cast<u8>(Opcode::STRB)] = &ExecutionEngine::STRB;
				handlers[static_cast<u8>(Opcode::STRH)] = &ExecutionEngine::STRH;
				handlers[static_cast<u8>(Opcode::FSTR)] = &ExecutionEngine::FSTR;
				handlers[static_cast<u8>(Opcode::LEA)] = &ExecutionEngine::LEA;

				// Jumps / Branches / Calls
				handlers[static_cast<u8>(Opcode::JP)] = &ExecutionEngine::JP;
				handlers[static_cast<u8>(Opcode::JPR)] = &ExecutionEngine::JPR;
				handlers[static_cast<u8>(Opcode::CMP)] = &ExecutionEngine::CMP;
				handlers[static_cast<u8>(Opcode::CMPI)] = &ExecutionEngine::CMPI;
				handlers[static_cast<u8>(Opcode::FCMP)] = &ExecutionEngine::FCMP;
				handlers[static_cast<u8>(Opcode::JZ)] = &ExecutionEngine::JZ;
				handlers[static_cast<u8>(Opcode::JZR)] = &ExecutionEngine::JZR;
				handlers[static_cast<u8>(Opcode::JNZ)] = &ExecutionEngine::JNZ;
				handlers[static_cast<u8>(Opcode::JNZR)] = &ExecutionEngine::JNZR;
				handlers[static_cast<u8>(Opcode::JC)] = &ExecutionEngine::JC;
				handlers[static_cast<u8>(Opcode::JCR)] = &ExecutionEngine::JCR;
				handlers[static_cast<u8>(Opcode::JNC)] = &ExecutionEngine::JNC;
				handlers[static_cast<u8>(Opcode::JNCR)] = &ExecutionEngine::JNCR;
				handlers[static_cast<u8>(Opcode::JS)] = &ExecutionEngine::JS;
				handlers[static_cast<u8>(Opcode::JSR)] = &ExecutionEngine::JSR;
				handlers[static_cast<u8>(Opcode::JNS)] = &ExecutionEngine::JNS;
				handlers[static_cast<u8>(Opcode::JNSR)] = &ExecutionEngine::JNSR;
				handlers[static_cast<u8>(Opcode::CALL)] = &ExecutionEngine::CALL;
				handlers[static_cast<u8>(Opcode::CALLR)] = &ExecutionEngine::CALLR;
				handlers[static_cast<u8>(Opcode::BL)] = &ExecutionEngine::BL;
				handlers[static_cast<u8>(Opcode::BLR)] = &ExecutionEngine::BLR;
				handlers[static_cast<u8>(Opcode::RET)] = &ExecutionEngine::RET;
				handlers[static_cast<u8>(Opcode::JO)] = &ExecutionEngine::JO;
				handlers[static_cast<u8>(Opcode::JOR)] = &ExecutionEngine::JOR;
				handlers[static_cast<u8>(Opcode::JNO)] = &ExecutionEngine::JNO;
				handlers[static_cast<u8>(Opcode::JNOR)] = &ExecutionEngine::JNOR;
				handlers[static_cast<u8>(Opcode::JGR)] = &ExecutionEngine::JGR;
				handlers[static_cast<u8>(Opcode::JGRR)] = &ExecutionEngine::JGRR;
				handlers[static_cast<u8>(Opcode::JGE)] = &ExecutionEngine::JGE;
				handlers[static_cast<u8>(Opcode::JGER)] = &ExecutionEngine::JGER;
				handlers[static_cast<u8>(Opcode::JLS)] = &ExecutionEngine::JLS;
				handlers[static_cast<u8>(Opcode::JLSR)] = &ExecutionEngine::JLSR;
				handlers[static_cast<u8>(Opcode::JLE)] = &ExecutionEngine::JLE;
				handlers[static_cast<u8>(Opcode::JLER)] = &ExecutionEngine::JLER;
				handlers[static_cast<u8>(Opcode::JAB)] = &ExecutionEngine::JAB;
				handlers[static_cast<u8>(Opcode::JABR)] = &ExecutionEngine::JABR;
				handlers[static_cast<u8>(Opcode::JAE)] = &ExecutionEngine::JAE;
				handlers[static_cast<u8>(Opcode::JAER)] = &ExecutionEngine::JAER;
				handlers[static_cast<u8>(Opcode::JBL)] = &ExecutionEngine::JBL;
				handlers[static_cast<u8>(Opcode::JBLR)] = &ExecutionEngine::JBLR;
				handlers[static_cast<u8>(Opcode::JBE)] = &ExecutionEngine::JBE;
				handlers[static_cast<u8>(Opcode::JBER)] = &ExecutionEngine::JBER;

				// Stack
				handlers[static_cast<u8>(Opcode::PUSH)] = &ExecutionEngine::PUSH;
				handlers[static_cast<u8>(Opcode::POP)] = &ExecutionEngine::POP;
				handlers[static_cast<u8>(Opcode::PUSHF)] = &ExecutionEngine::PUSHF;
				handlers[static_cast<u8>(Opcode::POPF)] = &ExecutionEngine::POPF;
				handlers[static_cast<u8>(Opcode::LDRX)] = &ExecutionEngine::LDRX;
				handlers[static_cast<u8>(Opcode::LDRBX)] = &ExecutionEngine::LDRBX;
				handlers[static_cast<u8>(Opcode::LDRHX)] = &ExecutionEngine::LDRHX;
				handlers[static_cast<u8>(Opcode::LDRSBX)] = &ExecutionEngine::LDRSBX;
				handlers[static_cast<u8>(Opcode::LDRSHX)] = &ExecutionEngine::LDRSHX;
				handlers[static_cast<u8>(Opcode::FLDRX)] = &ExecutionEngine::FLDRX;
				handlers[static_cast<u8>(Opcode::STRX)] = &ExecutionEngine::STRX;
				handlers[static_cast<u8>(Opcode::STRBX)] = &ExecutionEngine::STRBX;
				handlers[static_cast<u8>(Opcode::STRHX)] = &ExecutionEngine::STRHX;
				handlers[static_cast<u8>(Opcode::FSTRX)] = &ExecutionEngine::FSTRX;
				handlers[static_cast<u8>(Opcode::LDRP)] = &ExecutionEngine::LDRP;
				handlers[static_cast<u8>(Opcode::LDRBP)] = &ExecutionEngine::LDRBP;
				handlers[static_cast<u8>(Opcode::LDRHP)] = &ExecutionEngine::LDRHP;
				handlers[static_cast<u8>(Opcode::LDRSBP)] = &ExecutionEngine::LDRSBP;
				handlers[static_cast<u8>(Opcode::LDRSHP)] = &ExecutionEngine::LDRSHP;
				handlers[static_cast<u8>(Opcode::FLDRP)] = &ExecutionEngine::FLDRP;
				handlers[static_cast<u8>(Opcode::STRP)] = &ExecutionEngine::STRP;
				handlers[static_cast<u8>(Opcode::STRBP)] = &ExecutionEngine::STRBP;
				handlers[static_cast<u8>(Opcode::STRHP)] = &ExecutionEngine::STRHP;
				handlers[static_cast<u8>(Opcode::FSTRP)] = &ExecutionEngine::FSTRP;
				handlers[static_cast<u8>(Opcode::MULH)] = &ExecutionEngine::MULH;
				handlers[static_cast<u8>(Opcode::IMULH)] = &ExecutionEngine::IMULH;
				handlers[static_cast<u8>(Opcode::ABS)] = &ExecutionEngine::ABS;
				handlers[static_cast<u8>(Opcode::MIN)] = &ExecutionEngine::MIN;
				handlers[static_cast<u8>(Opcode::MINI)] = &ExecutionEngine::MINI;
				handlers[static_cast<u8>(Opcode::IMIN)] = &ExecutionEngine::IMIN;
				handlers[static_cast<u8>(Opcode::IMINI)] = &ExecutionEngine::IMINI;
				handlers[static_cast<u8>(Opcode::MAX)] = &ExecutionEngine::MAX;
				handlers[static_cast<u8>(Opcode::MAXI)] = &ExecutionEngine::MAXI;
				handlers[static_cast<u8>(Opcode::IMAX)] = &ExecutionEngine::IMAX;
				handlers[static_cast<u8>(Opcode::IMAXI)] = &ExecutionEngine::IMAXI;
				handlers[static_cast<u8>(Opcode::FMIN)] = &ExecutionEngine::FMIN;
				handlers[static_cast<u8>(Opcode::FMAX)] = &ExecutionEngine::FMAX;
				handlers[static_cast<u8>(Opcode::CLZ)] = &ExecutionEngine::CLZ;
				handlers[static_cast<u8>(Opcode::CTZ)] = &ExecutionEngine::CTZ;
				handlers[static_cast<u8>(Opcode::POPCNT)] = &ExecutionEngine::POPCNT;
				handlers[static_cast<u8>(Opcode::BSWAP)] = &ExecutionEngine::BSWAP;
				handlers[static_cast<u8>(Opcode::ROL)] = &ExecutionEngine::ROL;
				handlers[static_cast<u8>(Opcode::ROLI)] = &ExecutionEngine::ROLI;
				handlers[static_cast<u8>(Opcode::ROR)] = &ExecutionEngine::ROR;
				handlers[static_cast<u8>(Opcode::RORI)] = &ExecutionEngine::RORI;
				handlers[static_cast<u8>(Opcode::SXTB)] = &ExecutionEngine::SXTB;
				handlers[static_cast<u8>(Opcode::SXTH)] = &ExecutionEngine::SXTH;
				handlers[static_cast<u8>(Opcode::FSQRT)] = &ExecutionEngine::FSQRT;
				handlers[static_cast<u8>(Opcode::FABS)] = &ExecutionEngine::FABS;
				handlers[static_cast<u8>(Opcode::FROUND)] = &ExecutionEngine::FROUND;
				handlers[static_cast<u8>(Opcode::FFLOOR)] = &ExecutionEngine::FFLOOR;
				handlers[static_cast<u8>(Opcode::FCEIL)] = &ExecutionEngine::FCEIL;
				handlers[static_cast<u8>(Opcode::FTRUNC)] = &ExecutionEngine::FTRUNC;
				handlers[static_cast<u8>(Opcode::FCOPYSIGN)] = &ExecutionEngine::FCOPYSIGN;
				handlers[static_cast<u8>(Opcode::FMA)] = &ExecutionEngine::FMA;
				handlers[static_cast<u8>(Opcode::FCLASS)] = &ExecutionEngine::FCLASS;
				handlers[static_cast<u8>(Opcode::FRECIPE)] = &ExecutionEngine::FRECIPE;
				handlers[static_cast<u8>(Opcode::FRSQRTE)] = &ExecutionEngine::FRSQRTE;
				handlers[static_cast<u8>(Opcode::ENTER)] = &ExecutionEngine::ENTER;
				handlers[static_cast<u8>(Opcode::LEAVE)] = &ExecutionEngine::LEAVE;
				handlers[static_cast<u8>(Opcode::PUSHM)] = &ExecutionEngine::PUSHM;
				handlers[static_cast<u8>(Opcode::POPM)] = &ExecutionEngine::POPM;
				handlers[static_cast<u8>(Opcode::FPUSH)] = &ExecutionEngine::FPUSH;
				handlers[static_cast<u8>(Opcode::FPOP)] = &ExecutionEngine::FPOP;

				// Conversions
				handlers[static_cast<u8>(Opcode::ITOF)] = &ExecutionEngine::ITOF;
				handlers[static_cast<u8>(Opcode::IITOF)] = &ExecutionEngine::IITOF;
				handlers[static_cast<u8>(Opcode::FTOI)] = &ExecutionEngine::FTOI;
				handlers[static_cast<u8>(Opcode::FTOII)] = &ExecutionEngine::FTOII;
				handlers[static_cast<u8>(Opcode::MTF)] = &ExecutionEngine::MTF;
				handlers[static_cast<u8>(Opcode::MFF)] = &ExecutionEngine::MFF;

				return handlers;
		}();
	};
}
