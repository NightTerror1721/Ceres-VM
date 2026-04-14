#pragma once

#include "address.h"
#include "fregisters.h"
#include "memory.h"
#include "instructions.h"
#include "interrupts.h"
#include "io_ports.h"
#include <limits>
#include <cmath>

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

	public:
		explicit ExecutionEngine(Memory& memory, IOPorts& ioPorts) : _memory(memory), _ioPorts(ioPorts) {}
		ExecutionEngine(const ExecutionEngine&) = delete;
		ExecutionEngine(ExecutionEngine&&) = delete;
		~ExecutionEngine() = default;

		ExecutionEngine& operator=(const ExecutionEngine&) = delete;
		ExecutionEngine& operator=(ExecutionEngine&&) = delete;

	public:
		void reset() noexcept;
		void step() noexcept;

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

		template <typename T> requires (Integral<T> || FloatingPoint<T>) && (sizeof(T) <= sizeof(u32))
		forceinline void push(T value) noexcept
		{
			sp(sp() - sizeof(T));
			if constexpr (FloatingPoint<T>)
				write<u32>(Address(sp()), std::bit_cast<u32>(value));
			else
				write(Address(sp()), value);
		}

		template <typename T> requires (Integral<T> || FloatingPoint<T>) && (sizeof(T) <= sizeof(u32))
		forceinline T pop() noexcept
		{
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

        forceinline void executeSignedMul(const u8 regDest, const u32 a, const u32 b) noexcept
		{
			constexpr i64 max32 = static_cast<i64>(std::numeric_limits<i32>::max());
			constexpr i64 min32 = static_cast<i64>(std::numeric_limits<i32>::min());

			const i64 result = static_cast<i64>(static_cast<i32>(a)) * static_cast<i64>(static_cast<i32>(b));
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

		forceinline void executeSignedDiv(const u8 regDest, const u32 a, const u32 b) noexcept
		{
			if (b == 0)
			{
				trap(true);
				advancePC();
				return;
			}

			const i32 ia = static_cast<i32>(a);
			const i32 ib = static_cast<i32>(b);

			const i64 result = static_cast<i64>(ia) / static_cast<i64>(ib);
			const i32 result32 = static_cast<i32>(result);

			zero(result32 == 0);
			sign(result32 < 0);
			carry(false);

			const bool didOverflow = (ib == -1 && ia == std::numeric_limits<i32>::min());
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

		forceinline void executeSignedMod(const u8 regDest, const u32 a, const u32 b) noexcept
		{
			if (b == 0)
			{
				trap(true);
				advancePC();
				return;
			}

			const i32 ia = static_cast<i32>(a);
			const i32 ib = static_cast<i32>(b);

			// remainder computed in wider type to avoid UB
			const i64 result = static_cast<i64>(ia) % static_cast<i64>(ib);
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
		forceinline void TRAP(const Instruction inst) noexcept { triggerInterrupt(InterruptNumber::Trap); }
		forceinline void RESET(const Instruction inst) noexcept { triggerInterrupt(InterruptNumber::Reset); }
		forceinline void INT(const Instruction inst) noexcept { triggerInterrupt(static_cast<InterruptNumber>(inst.imm8())); }
		forceinline void IRET(const Instruction inst) noexcept
		{
			// Restore PC and flags from the stack
			const u32 newFlags = pop<u32>();
			const u32 newPC = pop<u32>();

			_pc = Address(newPC);
			_flags = FlagRegister(newFlags);

			advancePC();
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
		forceinline void IMULI(const Instruction inst) noexcept { executeSignedMul(inst.rd(), getReg(inst.rs()), inst.imm16()); }
		forceinline void FMUL(const Instruction inst) noexcept { executeFloatMul(inst.fd(), getFloatReg(inst.fs()), getFloatReg(inst.ft())); }
		forceinline void DIV(const Instruction inst) noexcept { executeUnsignedDiv(inst.rd(), getReg(inst.rs()), getReg(inst.rt())); }
		forceinline void DIVI(const Instruction inst) noexcept { executeUnsignedDiv(inst.rd(), getReg(inst.rs()), inst.imm16()); }
		forceinline void IDIV(const Instruction inst) noexcept { executeSignedDiv(inst.rd(), getReg(inst.rs()), getReg(inst.rt())); }
		forceinline void IDIVI(const Instruction inst) noexcept { executeSignedDiv(inst.rd(), getReg(inst.rs()), inst.imm16()); }
		forceinline void FDIV(const Instruction inst) noexcept { executeFloatDiv(inst.fd(), getFloatReg(inst.fs()), getFloatReg(inst.ft())); }
		forceinline void MOD(const Instruction inst) noexcept { executeUnsignedMod(inst.rd(), getReg(inst.rs()), getReg(inst.rt())); }
		forceinline void MODI(const Instruction inst) noexcept { executeUnsignedMod(inst.rd(), getReg(inst.rs()), inst.imm16()); }
		forceinline void IMOD(const Instruction inst) noexcept { executeSignedMod(inst.rd(), getReg(inst.rs()), getReg(inst.rt())); }
		forceinline void IMODI(const Instruction inst) noexcept { executeSignedMod(inst.rd(), getReg(inst.rs()), inst.imm16()); }
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
		forceinline void MOVI(const Instruction inst) noexcept { setReg(inst.rd(), inst.imm16()); advancePC(); }
		forceinline void MOVIH(const Instruction inst) noexcept { setReg(inst.rd(), static_cast<u32>(inst.imm16()) << 16u); advancePC(); }
		forceinline void FMOV(const Instruction inst) noexcept { setFloatReg(inst.fd(), getFloatReg(inst.fs())); advancePC(); }
		forceinline void LDRB(const Instruction inst) noexcept { setReg(inst.rd(), read<u8>(getReg(inst.rs()) + inst.imm16())); advancePC(); }
		forceinline void ILDRB(const Instruction inst) noexcept { setReg(inst.rd(), static_cast<u32>(read<i8>(getReg(inst.rs()) + inst.imm16()))); advancePC(); }
		forceinline void LDRW(const Instruction inst) noexcept { setReg(inst.rd(), read<u16>(getReg(inst.rs()) + inst.imm16())); advancePC(); }
		forceinline void ILDRW(const Instruction inst) noexcept { setReg(inst.rd(), static_cast<u32>(read<i16>(getReg(inst.rs()) + inst.imm16()))); advancePC(); }
		forceinline void LDRD(const Instruction inst) noexcept { setReg(inst.rd(), read<u32>(getReg(inst.rs()) + inst.imm16())); advancePC(); }
		forceinline void FLDR(const Instruction inst) noexcept { setFloatReg(inst.fd(), read<f32>(getReg(inst.rs()) + inst.imm16())); advancePC(); }
		forceinline void LEA(const Instruction inst) noexcept { setReg(inst.rd(), getReg(inst.rs()) + inst.imm16()); advancePC(); }
		forceinline void STRB(const Instruction inst) noexcept { write<u8>(getReg(inst.rs()) + inst.imm16(), static_cast<u8>(getReg(inst.rt()))); advancePC(); }
		forceinline void STRW(const Instruction inst) noexcept { write<u16>(getReg(inst.rs()) + inst.imm16(), static_cast<u16>(getReg(inst.rt()))); advancePC(); }
		forceinline void STRD(const Instruction inst) noexcept { write<u32>(getReg(inst.rs()) + inst.imm16(), getReg(inst.rt())); advancePC(); }
		forceinline void FSTR(const Instruction inst) noexcept { write<f32>(getReg(inst.rs()) + inst.imm16(), getFloatReg(inst.ft())); advancePC(); }

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
			const u32 b = inst.imm16();

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
		forceinline void CALL(const Instruction inst) noexcept
		{
			push<u32>((_pc + Instruction::SizeInBytes).value()); // Push return address onto the stack
			_pc += Address(inst.simm24()); // Jump to target address
		}
		forceinline void CALLR(const Instruction inst) noexcept
		{
			push<u32>((_pc + Instruction::SizeInBytes).value()); // Push return address onto the stack
			_pc = Address(getReg(inst.rs())); // Jump to target address
		}
		forceinline void RET(const Instruction inst) noexcept { _pc = Address(pop<u32>()); }

		forceinline void PUSH(const Instruction inst) noexcept { push<u32>(getReg(inst.rs())); advancePC(); }
		forceinline void POP(const Instruction inst) noexcept { setReg(inst.rs(), pop<u32>()); advancePC(); }
		forceinline void PUSHF(const Instruction inst) noexcept { push<u32>(_flags.value()); advancePC(); }
		forceinline void POPF(const Instruction inst) noexcept { _flags = pop<u32>(); advancePC(); }
		forceinline void FPUSH(const Instruction inst) noexcept { push<f32>(getFloatReg(inst.fs())); advancePC(); }
		forceinline void FPOP(const Instruction inst) noexcept { setFloatReg(inst.fs(), pop<f32>()); advancePC(); }

		forceinline void ITOF(const Instruction inst) noexcept { setFloatReg(inst.fd(), static_cast<f32>(getReg(inst.rs()))); advancePC(); }
		forceinline void IITOF(const Instruction inst) noexcept { setFloatReg(inst.fd(), static_cast<f32>(static_cast<i32>(getReg(inst.rs())))); advancePC(); }
		forceinline void FTOI(const Instruction inst) noexcept { setReg(inst.rd(), static_cast<u32>(getFloatReg(inst.fs()))); advancePC(); }
		forceinline void FTOII(const Instruction inst) noexcept { setReg(inst.rd(), static_cast<u32>(static_cast<i32>(getFloatReg(inst.fs())))); advancePC(); }
		forceinline void MTF(const Instruction inst) noexcept { _fregisters[inst.fd()].setBitsFromRegister(_registers[inst.rs()]); advancePC(); }
		forceinline void MFF(const Instruction inst) noexcept { _registers.set(inst.rd(), _fregisters[inst.fs()].bitsAsRegister()); advancePC(); }

		forceinline void IN(const Instruction inst) noexcept
		{
			setReg(inst.rd(), _ioPorts.read(inst.imm8()));
			advancePC();
		}
		forceinline void INR(const Instruction inst) noexcept
		{
			const u8 port = static_cast<u8>(getReg(inst.rs()));
			setReg(inst.rd(), _ioPorts.read(port));
			advancePC();
		}
		forceinline void OUT(const Instruction inst) noexcept
		{
			const u8 value = static_cast<u8>(getReg(inst.rs()));
			_ioPorts.write(inst.imm8(), value);
			advancePC();
		}
		forceinline void OUTR(const Instruction inst) noexcept
		{
			const u8 port = static_cast<u8>(getReg(inst.rt()));
			const u8 value = static_cast<u8>(getReg(inst.rs()));
			_ioPorts.write(port, value);
			advancePC();
		}

		forceinline void INVALID(const Instruction inst) noexcept { triggerInterrupt(InterruptNumber::IllegalInstruction); }

	private:
		using InstructionHandler = void (ExecutionEngine::*)(const Instruction) noexcept;
		static inline constexpr std::array<InstructionHandler, 256> InstructionHandlers = []() consteval noexcept -> std::array<InstructionHandler, 256>
		{
				std::array<InstructionHandler, 256> handlers{ &ExecutionEngine::INVALID };

				// Control
				handlers[static_cast<u8>(Opcode::NOP)] = &ExecutionEngine::NOP;
				handlers[static_cast<u8>(Opcode::HALT)] = &ExecutionEngine::HALT;
				handlers[static_cast<u8>(Opcode::TRAP)] = &ExecutionEngine::TRAP;
				handlers[static_cast<u8>(Opcode::RESET)] = &ExecutionEngine::RESET;
				handlers[static_cast<u8>(Opcode::INT)] = &ExecutionEngine::INT;
				handlers[static_cast<u8>(Opcode::IRET)] = &ExecutionEngine::IRET;

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
				handlers[static_cast<u8>(Opcode::MOVI)] = &ExecutionEngine::MOVI;
				handlers[static_cast<u8>(Opcode::MOVIH)] = &ExecutionEngine::MOVIH;
				handlers[static_cast<u8>(Opcode::FMOV)] = &ExecutionEngine::FMOV;
				handlers[static_cast<u8>(Opcode::LDRB)] = &ExecutionEngine::LDRB;
				handlers[static_cast<u8>(Opcode::ILDRB)] = &ExecutionEngine::ILDRB;
				handlers[static_cast<u8>(Opcode::LDRW)] = &ExecutionEngine::LDRW;
				handlers[static_cast<u8>(Opcode::ILDRW)] = &ExecutionEngine::ILDRW;
				handlers[static_cast<u8>(Opcode::LDRD)] = &ExecutionEngine::LDRD;
				handlers[static_cast<u8>(Opcode::FLDR)] = &ExecutionEngine::FLDR;
				handlers[static_cast<u8>(Opcode::LEA)] = &ExecutionEngine::LEA;
				handlers[static_cast<u8>(Opcode::STRB)] = &ExecutionEngine::STRB;
				handlers[static_cast<u8>(Opcode::STRW)] = &ExecutionEngine::STRW;
				handlers[static_cast<u8>(Opcode::STRD)] = &ExecutionEngine::STRD;
				handlers[static_cast<u8>(Opcode::FSTR)] = &ExecutionEngine::FSTR;

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
				handlers[static_cast<u8>(Opcode::INR)] = &ExecutionEngine::INR;
				handlers[static_cast<u8>(Opcode::OUT)] = &ExecutionEngine::OUT;
				handlers[static_cast<u8>(Opcode::OUTR)] = &ExecutionEngine::OUTR;

				return handlers;
		}();
	};
}
