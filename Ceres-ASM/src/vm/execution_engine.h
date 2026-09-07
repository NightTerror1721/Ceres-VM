#pragma once

#include "address.h"
#include "fregisters.h"
#include "memory.h"
#include "instructions.h"
#include "interrupts.h"
#include "io_ports.h"
#include "interrupt_controller.h"
#include <limits>
#include <cmath>
#include <optional>

namespace ceres::vm
{
	class ExecutionEngine
	{
	private:
		GeneralPurposeRegisterPool _registers;
		FloatingPointRegisterPool _fregisters;
		Address _pc; // Program Counter (PC)
		FlagRegister _flags; // Flags register
		Memory& _memory;
		IOPorts& _ioPorts;
		InterruptController& _interrupts;

		// Instructions retired since the last reset. The timer already counts in executed
		// instructions rather than wall clock, so this is the machine's own notion of time and
		// the only clock a debugger can step against deterministically.
		u64 _executedInstructions = 0;

	public:
		explicit ExecutionEngine(Memory& memory, IOPorts& ioPorts, InterruptController& interrupts) :
			_memory(memory), _ioPorts(ioPorts), _interrupts(interrupts)
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

		void setFlags(FlagRegister flags) noexcept { _flags = flags; }
		void setProgramCounter(Address address) noexcept { _pc = address; }

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
		forceinline Instruction fetch() const noexcept { return _memory.readInstruction(_pc); }
		forceinline void advancePC() noexcept { _pc += Instruction::SizeInBytes; }

		forceinline u32 getReg(u8 index) const noexcept { return _registers.getValue(index); }
		forceinline void setReg(u8 index, u32 value) noexcept { _registers.setValue(index, value); }

		forceinline f32 getFloatReg(u8 index) const noexcept { return _fregisters.getValue(index); }
		forceinline void setFloatReg(u8 index, f32 value) noexcept { _fregisters.setValue(index, value); }

		forceinline u32 sp() const noexcept { return _registers.getValue<GeneralPurposeRegisterPool::StackPointerIndex>(); }
		forceinline void sp(u32 value) noexcept { _registers.setValue<GeneralPurposeRegisterPool::StackPointerIndex>(value); }

		forceinline u32 fp() const noexcept { return _registers.getValue<GeneralPurposeRegisterPool::FramePointerIndex>(); }
		forceinline void fp(u32 value) noexcept { _registers.setValue<GeneralPurposeRegisterPool::FramePointerIndex>(value); }

		forceinline u32 lr() const noexcept { return _registers.getValue<GeneralPurposeRegisterPool::LinkRegisterIndex>(); }
		forceinline void lr(u32 value) noexcept { _registers.setValue<GeneralPurposeRegisterPool::LinkRegisterIndex>(value); }

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
		forceinline T read(Address address) const noexcept
		{
			if constexpr (FloatingPoint<T>)
				return _memory.readFloat(address);
			else
				return _memory.read<T>(address);
		}

		template <typename T> requires (Integral<T> || FloatingPoint<T>) && (sizeof(T) <= sizeof(u32))
		forceinline void write(Address address, T value) noexcept
		{
			if constexpr (FloatingPoint<T>)
				_memory.writeFloat(address, value);
			else
				_memory.write(address, value);
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
			return sp() >= Memory::UnrestrictedSegmentStartValue + bytes;
		}

		forceinline bool hasStackData(u32 bytes) const noexcept
		{
			return static_cast<u64>(sp()) + bytes <= static_cast<u64>(_memory.size());
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

			sp(sp() - sizeof(T));
			if constexpr (FloatingPoint<T>)
				write<u32>(Address(sp()), std::bit_cast<u32>(value));
			else
				write(Address(sp()), value);
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
				sp(sp() + sizeof(T));
				return value;
			}
			else
			{
				const T value = read<T>(Address(sp()));
				sp(sp() + sizeof(T));
				return value;
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
		forceinline void FNEG(const Instruction inst) noexcept { executeFloatNeg(inst.fd(), getFloatReg(inst.fs())); }

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
		forceinline void LDR(const Instruction inst) noexcept
		{
			const Address address = getReg(inst.rs()) + inst.imm16();
			if (!checkAlignment<u32>(address))
				return;
			setReg(inst.rd(), read<u32>(address));
			advancePC();
		}
		forceinline void LDRB(const Instruction inst) noexcept { setReg(inst.rd(), read<u8>(getReg(inst.rs()) + inst.imm16())); advancePC(); }
		forceinline void LDRH(const Instruction inst) noexcept
		{
			const Address address = getReg(inst.rs()) + inst.imm16();
			if (!checkAlignment<u16>(address))
				return;
			setReg(inst.rd(), read<u16>(address));
			advancePC();
		}
		forceinline void LDRSB(const Instruction inst) noexcept { setReg(inst.rd(), static_cast<u32>(read<i8>(getReg(inst.rs()) + inst.imm16()))); advancePC(); }
		forceinline void LDRSH(const Instruction inst) noexcept
		{
			const Address address = getReg(inst.rs()) + inst.imm16();
			if (!checkAlignment<i16>(address))
				return;
			setReg(inst.rd(), static_cast<u32>(read<i16>(address)));
			advancePC();
		}
		forceinline void FLDR(const Instruction inst) noexcept
		{
			const Address address = getReg(inst.rs()) + inst.imm16();
			if (!checkAlignment<f32>(address))
				return;
			setFloatReg(inst.fd(), read<f32>(address));
			advancePC();
		}
		forceinline void STR(const Instruction inst) noexcept
		{
			const Address address = getReg(inst.rd()) + inst.imm16();
			if (!checkAlignment<u32>(address))
				return;
			write<u32>(address, getReg(inst.rs()));
			advancePC();
		}
		forceinline void STRB(const Instruction inst) noexcept { write<u8>(getReg(inst.rd()) + inst.imm16(), static_cast<u8>(getReg(inst.rs()))); advancePC(); }
		forceinline void STRH(const Instruction inst) noexcept
		{
			const Address address = getReg(inst.rd()) + inst.imm16();
			if (!checkAlignment<u16>(address))
				return;
			write<u16>(address, static_cast<u16>(getReg(inst.rs())));
			advancePC();
		}
		forceinline void FSTR(const Instruction inst) noexcept
		{
			const Address address = getReg(inst.rd()) + inst.imm16();
			if (!checkAlignment<f32>(address))
				return;
			write<f32>(address, getFloatReg(inst.fs()));
			advancePC();
		}
		forceinline void LEA(const Instruction inst) noexcept { setReg(inst.rd(), getReg(inst.rs()) + inst.imm16()); advancePC(); }

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
		forceinline void RET(const Instruction inst) noexcept
		{
			if (const auto target = pop<u32>())
				_pc = Address(*target);
		}

		forceinline void PUSH(const Instruction inst) noexcept { if (push<u32>(getReg(inst.rs()))) advancePC(); }
		forceinline void POP(const Instruction inst) noexcept { if (const auto v = pop<u32>()) { setReg(inst.rd(), *v); advancePC(); } }
		forceinline void PUSHF(const Instruction inst) noexcept { if (push<u32>(_flags.value())) advancePC(); }
		forceinline void POPF(const Instruction inst) noexcept { if (const auto v = pop<u32>()) { _flags = *v; advancePC(); } }
		forceinline void FPUSH(const Instruction inst) noexcept { if (push<f32>(getFloatReg(inst.fs()))) advancePC(); }
		forceinline void FPOP(const Instruction inst) noexcept { if (const auto v = pop<f32>()) { setFloatReg(inst.fd(), *v); advancePC(); } }

		forceinline void ITOF(const Instruction inst) noexcept { setFloatReg(inst.fd(), static_cast<f32>(getReg(inst.rs()))); advancePC(); }
		forceinline void IITOF(const Instruction inst) noexcept { setFloatReg(inst.fd(), static_cast<f32>(static_cast<i32>(getReg(inst.rs())))); advancePC(); }
		forceinline void FTOI(const Instruction inst) noexcept { setReg(inst.rd(), static_cast<u32>(getFloatReg(inst.fs()))); advancePC(); }
		forceinline void FTOII(const Instruction inst) noexcept { setReg(inst.rd(), static_cast<u32>(static_cast<i32>(getFloatReg(inst.fs())))); advancePC(); }
		forceinline void MTF(const Instruction inst) noexcept { _fregisters[inst.fd()].setBitsFromRegister(_registers[inst.rs()]); advancePC(); }
		forceinline void MFF(const Instruction inst) noexcept { _registers.set(inst.rd(), _fregisters[inst.fs()].bitsAsRegister()); advancePC(); }

		forceinline void IN(const Instruction inst) noexcept
		{
			setReg(inst.rd(), _ioPorts.read<u32>(inst.imm8()));
			advancePC();
		}
		forceinline void INB(const Instruction inst) noexcept
		{
			setReg(inst.rd(), _ioPorts.read<u8>(inst.imm8()));
			advancePC();
		}
		forceinline void INH(const Instruction inst) noexcept
		{
			setReg(inst.rd(), _ioPorts.read<u16>(inst.imm8()));
			advancePC();
		}
		forceinline void INSB(const Instruction inst) noexcept
		{
			setReg(inst.rd(), _ioPorts.read<i8>(inst.imm8()));
			advancePC();
		}
		forceinline void INSH(const Instruction inst) noexcept
		{
			setReg(inst.rd(), _ioPorts.read<i16>(inst.imm8()));
			advancePC();
		}
		forceinline void INM(const Instruction inst) noexcept
		{
			_ioPorts.readBytes(inst.imm8(), getReg(inst.rd()), getReg(inst.rs()));
			advancePC();
		}
		forceinline void INR(const Instruction inst) noexcept
		{
			const u8 port = static_cast<u8>(getReg(inst.rs()));
			setReg(inst.rd(), _ioPorts.read<u32>(port));
			advancePC();
		}
		forceinline void INRB(const Instruction inst) noexcept
		{
			const u8 port = static_cast<u8>(getReg(inst.rs()));
			setReg(inst.rd(), _ioPorts.read<u8>(port));
			advancePC();
		}
		forceinline void INRH(const Instruction inst) noexcept
		{
			const u8 port = static_cast<u8>(getReg(inst.rs()));
			setReg(inst.rd(), _ioPorts.read<u16>(port));
			advancePC();
		}
		forceinline void INRSB(const Instruction inst) noexcept
		{
			const u8 port = static_cast<u8>(getReg(inst.rs()));
			setReg(inst.rd(), static_cast<i8>(_ioPorts.read<i8>(port)));
			advancePC();
		}
		forceinline void INRSH(const Instruction inst) noexcept
		{
			const u8 port = static_cast<u8>(getReg(inst.rs()));
			setReg(inst.rd(), static_cast<i16>(_ioPorts.read<i16>(port)));
			advancePC();
		}
		forceinline void INRM(const Instruction inst) noexcept
		{
			const u8 port = static_cast<u8>(getReg(inst.rs()));
			_ioPorts.readBytes(port, getReg(inst.rd()), getReg(inst.rt()));
			advancePC();
		}
		forceinline void OUT(const Instruction inst) noexcept
		{
			const u32 value = getReg(inst.rs());
			_ioPorts.write<u32>(inst.imm8(), value);
			advancePC();
		}
		forceinline void OUTB(const Instruction inst) noexcept
		{
			const u8 value = static_cast<u8>(getReg(inst.rs()));
			_ioPorts.write<u8>(inst.imm8(), value);
			advancePC();
		}
		forceinline void OUTH(const Instruction inst) noexcept
		{
			const u16 value = static_cast<u16>(getReg(inst.rs()));
			_ioPorts.write<u16>(inst.imm8(), value);
			advancePC();
		}
		forceinline void OUTM(const Instruction inst) noexcept
		{
			const Address address = getReg(inst.rs());
			const u32 size = getReg(inst.rt());
			_ioPorts.writeBytes(inst.imm8(), address, size);
			advancePC();
		}
		forceinline void OUTR(const Instruction inst) noexcept
		{
			const u8 port = static_cast<u8>(getReg(inst.rt()));
			const u32 value = getReg(inst.rs());
			_ioPorts.write<u32>(port, value);
			advancePC();
		}
		forceinline void OUTRB(const Instruction inst) noexcept
		{
			const u8 port = static_cast<u8>(getReg(inst.rt()));
			const u8 value = static_cast<u8>(getReg(inst.rs()));
			_ioPorts.write<u8>(port, value);
			advancePC();
		}
		forceinline void OUTRH(const Instruction inst) noexcept
		{
			const u8 port = static_cast<u8>(getReg(inst.rt()));
			const u16 value = static_cast<u16>(getReg(inst.rs()));
			_ioPorts.write<u16>(port, value);
			advancePC();
		}
		forceinline void OUTRM(const Instruction inst) noexcept
		{
			const u8 port = static_cast<u8>(getReg(inst.rt()));
			const Address address = getReg(inst.rs());
			const u32 size = getReg(inst.rd());
			_ioPorts.writeBytes(port, address, size);
			advancePC();
		}

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
				handlers[static_cast<u8>(Opcode::RET)] = &ExecutionEngine::RET;
				handlers[static_cast<u8>(Opcode::JO)] = &ExecutionEngine::JO;
				handlers[static_cast<u8>(Opcode::JOR)] = &ExecutionEngine::JOR;
				handlers[static_cast<u8>(Opcode::JNO)] = &ExecutionEngine::JNO;
				handlers[static_cast<u8>(Opcode::JNOR)] = &ExecutionEngine::JNOR;

				// Stack
				handlers[static_cast<u8>(Opcode::PUSH)] = &ExecutionEngine::PUSH;
				handlers[static_cast<u8>(Opcode::POP)] = &ExecutionEngine::POP;
				handlers[static_cast<u8>(Opcode::PUSHF)] = &ExecutionEngine::PUSHF;
				handlers[static_cast<u8>(Opcode::POPF)] = &ExecutionEngine::POPF;
				handlers[static_cast<u8>(Opcode::FPUSH)] = &ExecutionEngine::FPUSH;
				handlers[static_cast<u8>(Opcode::FPOP)] = &ExecutionEngine::FPOP;

				// Conversions
				handlers[static_cast<u8>(Opcode::ITOF)] = &ExecutionEngine::ITOF;
				handlers[static_cast<u8>(Opcode::IITOF)] = &ExecutionEngine::IITOF;
				handlers[static_cast<u8>(Opcode::FTOI)] = &ExecutionEngine::FTOI;
				handlers[static_cast<u8>(Opcode::FTOII)] = &ExecutionEngine::FTOII;
				handlers[static_cast<u8>(Opcode::MTF)] = &ExecutionEngine::MTF;
				handlers[static_cast<u8>(Opcode::MFF)] = &ExecutionEngine::MFF;

				// I/O
				handlers[static_cast<u8>(Opcode::IN)] = &ExecutionEngine::IN;
				handlers[static_cast<u8>(Opcode::INB)] = &ExecutionEngine::INB;
				handlers[static_cast<u8>(Opcode::INH)] = &ExecutionEngine::INH;
				handlers[static_cast<u8>(Opcode::INSB)] = &ExecutionEngine::INSB;
				handlers[static_cast<u8>(Opcode::INSH)] = &ExecutionEngine::INSH;
				handlers[static_cast<u8>(Opcode::INM)] = &ExecutionEngine::INM;
				handlers[static_cast<u8>(Opcode::INR)] = &ExecutionEngine::INR;
				handlers[static_cast<u8>(Opcode::INRB)] = &ExecutionEngine::INRB;
				handlers[static_cast<u8>(Opcode::INRH)] = &ExecutionEngine::INRH;
				handlers[static_cast<u8>(Opcode::INRSB)] = &ExecutionEngine::INRSB;
				handlers[static_cast<u8>(Opcode::INRSH)] = &ExecutionEngine::INRSH;
				handlers[static_cast<u8>(Opcode::INRM)] = &ExecutionEngine::INRM;
				handlers[static_cast<u8>(Opcode::OUT)] = &ExecutionEngine::OUT;
				handlers[static_cast<u8>(Opcode::OUTB)] = &ExecutionEngine::OUTB;
				handlers[static_cast<u8>(Opcode::OUTH)] = &ExecutionEngine::OUTH;
				handlers[static_cast<u8>(Opcode::OUTM)] = &ExecutionEngine::OUTM;
				handlers[static_cast<u8>(Opcode::OUTR)] = &ExecutionEngine::OUTR;
				handlers[static_cast<u8>(Opcode::OUTRB)] = &ExecutionEngine::OUTRB;
				handlers[static_cast<u8>(Opcode::OUTRH)] = &ExecutionEngine::OUTRH;
				handlers[static_cast<u8>(Opcode::OUTRM)] = &ExecutionEngine::OUTRM;

				return handlers;
		}();
	};
}
