#include "instruction_info.h"
#include <unordered_map>

namespace ceres::casm
{
	using vm::Opcode;

	static constexpr std::pair<const InstructionSignature, InstructionInfo> inst(
		Opcode opcode,
		Mnemonic mnemonic,
		OpcodeParameterType op1 = OpcodeParameterType::Invalid,
		OpcodeParameterType op2 = OpcodeParameterType::Invalid,
		OpcodeParameterType op3 = OpcodeParameterType::Invalid,
		OpcodeParameterType op4 = OpcodeParameterType::Invalid) noexcept
	{
		InstructionSignature signature = InstructionSignature::make(mnemonic, op1, op2, op3, op4);
		InstructionInfo info = InstructionInfo::make(signature, opcode, op1, op2, op3, op4);
		return { signature, std::move(info) };
	}

	static constexpr std::pair<const InstructionSignature, InstructionInfo> inst(InstructionSignature signature, std::initializer_list<OpcodeInfo> opcodes) noexcept
	{
		return { signature, InstructionInfo::make(signature, opcodes) };
	}
	static constexpr std::pair<const InstructionSignature, InstructionInfo> inst(InstructionSignature signature, OpcodeInfo opcode) noexcept
	{
		return { signature, InstructionInfo::make(signature, { opcode }) };
	}

	static constexpr InstructionSignature sig(
		Mnemonic mnemonic,
		OperandType op1 = OperandType::Invalid,
		OperandType op2 = OperandType::Invalid,
		OperandType op3 = OperandType::Invalid,
		OperandType op4 = OperandType::Invalid) noexcept
	{
		return InstructionSignature::make(mnemonic, op1, op2, op3, op4);
	}

	static constexpr OpcodeInfo op(
		Opcode opcode,
		OpcodeParameter param1 = OpcodeParameter::Invalid,
		OpcodeParameter param2 = OpcodeParameter::Invalid,
		OpcodeParameter param3 = OpcodeParameter::Invalid,
		OpcodeParameter param4 = OpcodeParameter::Invalid) noexcept
	{
		return OpcodeInfo::make(opcode, param1, param2, param3, param4);
	}

	static constexpr OpcodeParameter param(OpcodeParameterType type, u8 operandIndex, u8 valueShift = 0) noexcept
	{
		return OpcodeParameter::make(type, operandIndex, valueShift);
	}

	static constexpr OpcodeParameter paramUFixed(OpcodeParameterType type, u32 fixedValue, u8 valueShift = 0) noexcept
	{
		return OpcodeParameter::makeUFixed(type, fixedValue, valueShift);
	}

	// Register the assembler is allowed to clobber while materialising a 32-bit address. Only STV
	// and the float form of LDV need one: every other expansion builds the address in the operand
	// register it was handed. This used to be R12, on the grounds that R13 was the Link Register
	// and an STV would destroy its own return address - which describes a machine this one never
	// was, because CALL pushes the return address on the stack and RET pops it. R13 is the one
	// register with a name of its own to spend on this, so it carries the hazard and the name that
	// warns about it (`at`, the assembler temporary), and R0-R12 are contiguous and all usable.
	static inline constexpr u32 ScratchRegister = 13;

	// ENTER/LEAVE are the only things in the project that name these two by role.
	static inline constexpr u32 FramePointerRegister = 14;
	static inline constexpr u32 StackPointerRegister = 15;

	static constexpr OpcodeParameter paramSFixed(OpcodeParameterType type, i32 fixedValue, u8 valueShift = 0) noexcept
	{
		return OpcodeParameter::makeSFixed(type, fixedValue, valueShift);
	}

	static std::unordered_map<InstructionSignature, InstructionInfo> __instructionsInfoMap
	{
		inst(Opcode::NOP, Mnemonic::NOP),
		inst(Opcode::HALT, Mnemonic::HALT),
		inst(Opcode::TRAP, Mnemonic::TRAP),
		inst(Opcode::RESET, Mnemonic::RESET),
		inst(Opcode::INT, Mnemonic::INT, OpcodeParameterType::IMM8),
		inst(Opcode::IRET, Mnemonic::IRET),
		inst(Opcode::CLI, Mnemonic::CLI),
		inst(Opcode::STI, Mnemonic::STI),

		inst(Opcode::ADD, Mnemonic::ADD, OpcodeParameterType::RD, OpcodeParameterType::RS, OpcodeParameterType::RT),
		inst(Opcode::ADDI, Mnemonic::ADD, OpcodeParameterType::RD, OpcodeParameterType::RS, OpcodeParameterType::IMM16),
		inst(Opcode::ADDC, Mnemonic::ADC, OpcodeParameterType::RD, OpcodeParameterType::RS, OpcodeParameterType::RT),
		inst(Opcode::ADDCI, Mnemonic::ADC, OpcodeParameterType::RD, OpcodeParameterType::RS, OpcodeParameterType::IMM16),
		inst(Opcode::FADD, Mnemonic::ADD, OpcodeParameterType::FD, OpcodeParameterType::FS, OpcodeParameterType::FT),
		inst(Opcode::SUB, Mnemonic::SUB, OpcodeParameterType::RD, OpcodeParameterType::RS, OpcodeParameterType::RT),
		inst(Opcode::SUBI, Mnemonic::SUB, OpcodeParameterType::RD, OpcodeParameterType::RS, OpcodeParameterType::IMM16),
		inst(Opcode::SUBC, Mnemonic::SBC, OpcodeParameterType::RD, OpcodeParameterType::RS, OpcodeParameterType::RT),
		inst(Opcode::SUBCI, Mnemonic::SBC, OpcodeParameterType::RD, OpcodeParameterType::RS, OpcodeParameterType::IMM16),
		inst(Opcode::FSUB, Mnemonic::SUB, OpcodeParameterType::FD, OpcodeParameterType::FS, OpcodeParameterType::FT),
		inst(Opcode::MUL, Mnemonic::MUL, OpcodeParameterType::RD, OpcodeParameterType::RS, OpcodeParameterType::RT),
		inst(Opcode::MULI, Mnemonic::MUL, OpcodeParameterType::RD, OpcodeParameterType::RS, OpcodeParameterType::IMM16),
		inst(Opcode::IMUL, Mnemonic::IMUL, OpcodeParameterType::RD, OpcodeParameterType::RS, OpcodeParameterType::RT),
		inst(Opcode::IMULI, Mnemonic::IMUL, OpcodeParameterType::RD, OpcodeParameterType::RS, OpcodeParameterType::SIMM16),
		inst(Opcode::FMUL, Mnemonic::MUL, OpcodeParameterType::FD, OpcodeParameterType::FS, OpcodeParameterType::FT),
		inst(Opcode::DIV, Mnemonic::DIV, OpcodeParameterType::RD, OpcodeParameterType::RS, OpcodeParameterType::RT),
		inst(Opcode::DIVI, Mnemonic::DIV, OpcodeParameterType::RD, OpcodeParameterType::RS, OpcodeParameterType::IMM16),
		inst(Opcode::IDIV, Mnemonic::IDIV, OpcodeParameterType::RD, OpcodeParameterType::RS, OpcodeParameterType::RT),
		inst(Opcode::IDIVI, Mnemonic::IDIV, OpcodeParameterType::RD, OpcodeParameterType::RS, OpcodeParameterType::SIMM16),
		inst(Opcode::FDIV, Mnemonic::DIV, OpcodeParameterType::FD, OpcodeParameterType::FS, OpcodeParameterType::FT),
		inst(Opcode::MOD, Mnemonic::MOD, OpcodeParameterType::RD, OpcodeParameterType::RS, OpcodeParameterType::RT),
		inst(Opcode::MODI, Mnemonic::MOD, OpcodeParameterType::RD, OpcodeParameterType::RS, OpcodeParameterType::IMM16),
		inst(Opcode::IMOD, Mnemonic::IMOD, OpcodeParameterType::RD, OpcodeParameterType::RS, OpcodeParameterType::RT),
		inst(Opcode::IMODI, Mnemonic::IMOD, OpcodeParameterType::RD, OpcodeParameterType::RS, OpcodeParameterType::SIMM16),
		inst(Opcode::FNEG, Mnemonic::NEG, OpcodeParameterType::FD, OpcodeParameterType::FS),
		inst(sig(Mnemonic::NEG, OperandType::IntegralRegister, OperandType::IntegralRegister), {
			op(Opcode::IMUL, param(OpcodeParameterType::RD, 0), paramSFixed(OpcodeParameterType::RS, -1))
		}),

		inst(Opcode::AND, Mnemonic::AND, OpcodeParameterType::RD, OpcodeParameterType::RS, OpcodeParameterType::RT),
		inst(Opcode::ANDI, Mnemonic::AND, OpcodeParameterType::RD, OpcodeParameterType::RS, OpcodeParameterType::IMM16),
		inst(Opcode::OR, Mnemonic::OR, OpcodeParameterType::RD, OpcodeParameterType::RS, OpcodeParameterType::RT),
		inst(Opcode::ORI, Mnemonic::OR, OpcodeParameterType::RD, OpcodeParameterType::RS, OpcodeParameterType::IMM16),
		inst(Opcode::XOR, Mnemonic::XOR, OpcodeParameterType::RD, OpcodeParameterType::RS, OpcodeParameterType::RT),
		inst(Opcode::XORI, Mnemonic::XOR, OpcodeParameterType::RD, OpcodeParameterType::RS, OpcodeParameterType::IMM16),
		inst(Opcode::NOT, Mnemonic::NOT, OpcodeParameterType::RD, OpcodeParameterType::RS),
		inst(Opcode::SHL, Mnemonic::SHL, OpcodeParameterType::RD, OpcodeParameterType::RS, OpcodeParameterType::RT),
		inst(Opcode::SHLI, Mnemonic::SHL, OpcodeParameterType::RD, OpcodeParameterType::RS, OpcodeParameterType::IMM16),
		inst(Opcode::SHR, Mnemonic::SHR, OpcodeParameterType::RD, OpcodeParameterType::RS, OpcodeParameterType::RT),
		inst(Opcode::SHRI, Mnemonic::SHR, OpcodeParameterType::RD, OpcodeParameterType::RS, OpcodeParameterType::IMM16),
		inst(Opcode::SAR, Mnemonic::SAR, OpcodeParameterType::RD, OpcodeParameterType::RS, OpcodeParameterType::RT),
		inst(Opcode::SARI, Mnemonic::SAR, OpcodeParameterType::RD, OpcodeParameterType::RS, OpcodeParameterType::IMM16),

		inst(Opcode::MOV, Mnemonic::MOV, OpcodeParameterType::RD, OpcodeParameterType::RS),
		inst(Opcode::FMOV, Mnemonic::MOV, OpcodeParameterType::FD, OpcodeParameterType::FS),
		inst(Opcode::LI, Mnemonic::LI, OpcodeParameterType::RD, OpcodeParameterType::IMM16),
		inst(Opcode::LUI, Mnemonic::LUI, OpcodeParameterType::RD, OpcodeParameterType::IMM16),
		inst(Opcode::LDR, Mnemonic::LDR, OpcodeParameterType::RD, OpcodeParameterType::RS_SIMM16),
		inst(Opcode::LDRB, Mnemonic::LDRB, OpcodeParameterType::RD, OpcodeParameterType::RS_SIMM16),
		inst(Opcode::LDRH, Mnemonic::LDRH, OpcodeParameterType::RD, OpcodeParameterType::RS_SIMM16),
		inst(Opcode::LDRSB, Mnemonic::LDRSB, OpcodeParameterType::RD, OpcodeParameterType::RS_SIMM16),
		inst(Opcode::LDRSH, Mnemonic::LDRSH, OpcodeParameterType::RD, OpcodeParameterType::RS_SIMM16),
		inst(Opcode::FLDR, Mnemonic::LDR, OpcodeParameterType::FD, OpcodeParameterType::RS_SIMM16),
		inst(sig(Mnemonic::LDV, OperandType::IntegralRegister, OperandType::VariableU8), {
			op(Opcode::LUI, param(OpcodeParameterType::RD, 0), param(OpcodeParameterType::IMM16, 1, 16)),
			op(Opcode::ORI, param(OpcodeParameterType::RD, 0), param(OpcodeParameterType::RS, 0), param(OpcodeParameterType::IMM16_LOW, 1)),
			op(Opcode::LDRB, param(OpcodeParameterType::RD, 0), param(OpcodeParameterType::RS, 0), paramUFixed(OpcodeParameterType::IMM16, 0))
		}),
		inst(sig(Mnemonic::LDV, OperandType::IntegralRegister, OperandType::VariableS8), {
			op(Opcode::LUI, param(OpcodeParameterType::RD, 0), param(OpcodeParameterType::IMM16, 1, 16)),
			op(Opcode::ORI, param(OpcodeParameterType::RD, 0), param(OpcodeParameterType::RS, 0), param(OpcodeParameterType::IMM16_LOW, 1)),
			op(Opcode::LDRSB, param(OpcodeParameterType::RD, 0), param(OpcodeParameterType::RS, 0), paramUFixed(OpcodeParameterType::IMM16, 0))
		}),
		inst(sig(Mnemonic::LDV, OperandType::IntegralRegister, OperandType::VariableU16), {
			op(Opcode::LUI, param(OpcodeParameterType::RD, 0), param(OpcodeParameterType::IMM16, 1, 16)),
			op(Opcode::ORI, param(OpcodeParameterType::RD, 0), param(OpcodeParameterType::RS, 0), param(OpcodeParameterType::IMM16_LOW, 1)),
			op(Opcode::LDRH, param(OpcodeParameterType::RD, 0), param(OpcodeParameterType::RS, 0), paramUFixed(OpcodeParameterType::IMM16, 0))
		}),
		inst(sig(Mnemonic::LDV, OperandType::IntegralRegister, OperandType::VariableS16), {
			op(Opcode::LUI, param(OpcodeParameterType::RD, 0), param(OpcodeParameterType::IMM16, 1, 16)),
			op(Opcode::ORI, param(OpcodeParameterType::RD, 0), param(OpcodeParameterType::RS, 0), param(OpcodeParameterType::IMM16_LOW, 1)),
			op(Opcode::LDRSH, param(OpcodeParameterType::RD, 0), param(OpcodeParameterType::RS, 0), paramUFixed(OpcodeParameterType::IMM16, 0))
		}),
		inst(sig(Mnemonic::LDV, OperandType::IntegralRegister, OperandType::VariableU32), {
			op(Opcode::LUI, param(OpcodeParameterType::RD, 0), param(OpcodeParameterType::IMM16, 1, 16)),
			op(Opcode::ORI, param(OpcodeParameterType::RD, 0), param(OpcodeParameterType::RS, 0), param(OpcodeParameterType::IMM16_LOW, 1)),
			op(Opcode::LDR, param(OpcodeParameterType::RD, 0), param(OpcodeParameterType::RS, 0), paramUFixed(OpcodeParameterType::IMM16, 0))
		}),
		inst(sig(Mnemonic::LDV, OperandType::IntegralRegister, OperandType::VariableS32), {
			op(Opcode::LUI, param(OpcodeParameterType::RD, 0), param(OpcodeParameterType::IMM16, 1, 16)),
			op(Opcode::ORI, param(OpcodeParameterType::RD, 0), param(OpcodeParameterType::RS, 0), param(OpcodeParameterType::IMM16_LOW, 1)),
			op(Opcode::LDR, param(OpcodeParameterType::RD, 0), param(OpcodeParameterType::RS, 0), paramUFixed(OpcodeParameterType::IMM16, 0))
		}),
		inst(sig(Mnemonic::LDV, OperandType::FloatingPointRegister, OperandType::VariableF32), {
			op(Opcode::LUI, paramUFixed(OpcodeParameterType::RD, ScratchRegister), param(OpcodeParameterType::IMM16, 1, 16)),
			op(Opcode::ORI, paramUFixed(OpcodeParameterType::RD, ScratchRegister), paramUFixed(OpcodeParameterType::RS, ScratchRegister), param(OpcodeParameterType::IMM16_LOW, 1)),
			op(Opcode::FLDR, param(OpcodeParameterType::FD, 0), paramUFixed(OpcodeParameterType::RS, ScratchRegister), paramUFixed(OpcodeParameterType::IMM16, 0))
		}),
		inst(Opcode::STR, Mnemonic::STR, OpcodeParameterType::RS, OpcodeParameterType::RD_SIMM16),
		inst(Opcode::STRB, Mnemonic::STRB, OpcodeParameterType::RS, OpcodeParameterType::RD_SIMM16),
		inst(Opcode::STRH, Mnemonic::STRH, OpcodeParameterType::RS, OpcodeParameterType::RD_SIMM16),
		inst(Opcode::FSTR, Mnemonic::STR, OpcodeParameterType::FS, OpcodeParameterType::RD_SIMM16),
		inst(sig(Mnemonic::STV, OperandType::IntegralRegister, OperandType::VariableU8), {
			op(Opcode::LUI, paramUFixed(OpcodeParameterType::RD, ScratchRegister), param(OpcodeParameterType::IMM16, 1, 16)),
			op(Opcode::ORI, paramUFixed(OpcodeParameterType::RD, ScratchRegister), paramUFixed(OpcodeParameterType::RS, ScratchRegister), param(OpcodeParameterType::IMM16_LOW, 1)),
			op(Opcode::STRB, paramUFixed(OpcodeParameterType::RD, ScratchRegister), param(OpcodeParameterType::RS, 0), paramUFixed(OpcodeParameterType::IMM16, 0))
		}),
		inst(sig(Mnemonic::STV, OperandType::IntegralRegister, OperandType::VariableS8), {
			op(Opcode::LUI, paramUFixed(OpcodeParameterType::RD, ScratchRegister), param(OpcodeParameterType::IMM16, 1, 16)),
			op(Opcode::ORI, paramUFixed(OpcodeParameterType::RD, ScratchRegister), paramUFixed(OpcodeParameterType::RS, ScratchRegister), param(OpcodeParameterType::IMM16_LOW, 1)),
			op(Opcode::STRB, paramUFixed(OpcodeParameterType::RD, ScratchRegister), param(OpcodeParameterType::RS, 0), paramUFixed(OpcodeParameterType::IMM16, 0))
		}),
		inst(sig(Mnemonic::STV, OperandType::IntegralRegister, OperandType::VariableU16), {
			op(Opcode::LUI, paramUFixed(OpcodeParameterType::RD, ScratchRegister), param(OpcodeParameterType::IMM16, 1, 16)),
			op(Opcode::ORI, paramUFixed(OpcodeParameterType::RD, ScratchRegister), paramUFixed(OpcodeParameterType::RS, ScratchRegister), param(OpcodeParameterType::IMM16_LOW, 1)),
			op(Opcode::STRH, paramUFixed(OpcodeParameterType::RD, ScratchRegister), param(OpcodeParameterType::RS, 0), paramUFixed(OpcodeParameterType::IMM16, 0))
		}),
		inst(sig(Mnemonic::STV, OperandType::IntegralRegister, OperandType::VariableS16), {
			op(Opcode::LUI, paramUFixed(OpcodeParameterType::RD, ScratchRegister), param(OpcodeParameterType::IMM16, 1, 16)),
			op(Opcode::ORI, paramUFixed(OpcodeParameterType::RD, ScratchRegister), paramUFixed(OpcodeParameterType::RS, ScratchRegister), param(OpcodeParameterType::IMM16_LOW, 1)),
			op(Opcode::STRH, paramUFixed(OpcodeParameterType::RD, ScratchRegister), param(OpcodeParameterType::RS, 0), paramUFixed(OpcodeParameterType::IMM16, 0))
		}),
		inst(sig(Mnemonic::STV, OperandType::IntegralRegister, OperandType::VariableU32), {
			op(Opcode::LUI, paramUFixed(OpcodeParameterType::RD, ScratchRegister), param(OpcodeParameterType::IMM16, 1, 16)),
			op(Opcode::ORI, paramUFixed(OpcodeParameterType::RD, ScratchRegister), paramUFixed(OpcodeParameterType::RS, ScratchRegister), param(OpcodeParameterType::IMM16_LOW, 1)),
			op(Opcode::STR, paramUFixed(OpcodeParameterType::RD, ScratchRegister), param(OpcodeParameterType::RS, 0), paramUFixed(OpcodeParameterType::IMM16, 0))
		}),
		inst(sig(Mnemonic::STV, OperandType::IntegralRegister, OperandType::VariableS32), {
			op(Opcode::LUI, paramUFixed(OpcodeParameterType::RD, ScratchRegister), param(OpcodeParameterType::IMM16, 1, 16)),
			op(Opcode::ORI, paramUFixed(OpcodeParameterType::RD, ScratchRegister), paramUFixed(OpcodeParameterType::RS, ScratchRegister), param(OpcodeParameterType::IMM16_LOW, 1)),
			op(Opcode::STR, paramUFixed(OpcodeParameterType::RD, ScratchRegister), param(OpcodeParameterType::RS, 0), paramUFixed(OpcodeParameterType::IMM16, 0))
		}),
		inst(sig(Mnemonic::STV, OperandType::FloatingPointRegister, OperandType::VariableF32), {
			op(Opcode::LUI, paramUFixed(OpcodeParameterType::RD, ScratchRegister), param(OpcodeParameterType::IMM16, 1, 16)),
			op(Opcode::ORI, paramUFixed(OpcodeParameterType::RD, ScratchRegister), paramUFixed(OpcodeParameterType::RS, ScratchRegister), param(OpcodeParameterType::IMM16_LOW, 1)),
			op(Opcode::FSTR, paramUFixed(OpcodeParameterType::RD, ScratchRegister), param(OpcodeParameterType::FS, 0), paramUFixed(OpcodeParameterType::IMM16, 0))
		}),
		inst(sig(Mnemonic::LA, OperandType::IntegralRegister, OperandType::VariableU8), {
			op(Opcode::LUI, param(OpcodeParameterType::RD, 0), param(OpcodeParameterType::IMM16, 1, 16)),
			op(Opcode::ORI, param(OpcodeParameterType::RD, 0), param(OpcodeParameterType::RS, 0), param(OpcodeParameterType::IMM16_LOW, 1))
		}),
		inst(sig(Mnemonic::LA, OperandType::IntegralRegister, OperandType::VariableS8), {
			op(Opcode::LUI, param(OpcodeParameterType::RD, 0), param(OpcodeParameterType::IMM16, 1, 16)),
			op(Opcode::ORI, param(OpcodeParameterType::RD, 0), param(OpcodeParameterType::RS, 0), param(OpcodeParameterType::IMM16_LOW, 1))
		}),
		inst(sig(Mnemonic::LA, OperandType::IntegralRegister, OperandType::VariableU16), {
			op(Opcode::LUI, param(OpcodeParameterType::RD, 0), param(OpcodeParameterType::IMM16, 1, 16)),
			op(Opcode::ORI, param(OpcodeParameterType::RD, 0), param(OpcodeParameterType::RS, 0), param(OpcodeParameterType::IMM16_LOW, 1))
		}),
		inst(sig(Mnemonic::LA, OperandType::IntegralRegister, OperandType::VariableS16), {
			op(Opcode::LUI, param(OpcodeParameterType::RD, 0), param(OpcodeParameterType::IMM16, 1, 16)),
			op(Opcode::ORI, param(OpcodeParameterType::RD, 0), param(OpcodeParameterType::RS, 0), param(OpcodeParameterType::IMM16_LOW, 1))
		}),
		inst(sig(Mnemonic::LA, OperandType::IntegralRegister, OperandType::VariableU32), {
			op(Opcode::LUI, param(OpcodeParameterType::RD, 0), param(OpcodeParameterType::IMM16, 1, 16)),
			op(Opcode::ORI, param(OpcodeParameterType::RD, 0), param(OpcodeParameterType::RS, 0), param(OpcodeParameterType::IMM16_LOW, 1))
		}),
		inst(sig(Mnemonic::LA, OperandType::IntegralRegister, OperandType::VariableS32), {
			op(Opcode::LUI, param(OpcodeParameterType::RD, 0), param(OpcodeParameterType::IMM16, 1, 16)),
			op(Opcode::ORI, param(OpcodeParameterType::RD, 0), param(OpcodeParameterType::RS, 0), param(OpcodeParameterType::IMM16_LOW, 1))
		}),
		inst(sig(Mnemonic::LA, OperandType::IntegralRegister, OperandType::VariableF32), {
			op(Opcode::LUI, param(OpcodeParameterType::RD, 0), param(OpcodeParameterType::IMM16, 1, 16)),
			op(Opcode::ORI, param(OpcodeParameterType::RD, 0), param(OpcodeParameterType::RS, 0), param(OpcodeParameterType::IMM16_LOW, 1))
		}),
		inst(sig(Mnemonic::LA, OperandType::IntegralRegister, OperandType::Label), {
			op(Opcode::LUI, param(OpcodeParameterType::RD, 0), param(OpcodeParameterType::IMM16, 1, 16)),
			op(Opcode::ORI, param(OpcodeParameterType::RD, 0), param(OpcodeParameterType::RS, 0), param(OpcodeParameterType::IMM16_LOW, 1))
		}),
		// `[rs + rt]` is an index rather than a displacement, so the same mnemonic picks a
		// different opcode - exactly as `add` does between a register and an immediate.
		inst(Opcode::LDRX, Mnemonic::LDR, OpcodeParameterType::RD, OpcodeParameterType::RS_RT),
		inst(Opcode::LDRBX, Mnemonic::LDRB, OpcodeParameterType::RD, OpcodeParameterType::RS_RT),
		inst(Opcode::LDRHX, Mnemonic::LDRH, OpcodeParameterType::RD, OpcodeParameterType::RS_RT),
		inst(Opcode::LDRSBX, Mnemonic::LDRSB, OpcodeParameterType::RD, OpcodeParameterType::RS_RT),
		inst(Opcode::LDRSHX, Mnemonic::LDRSH, OpcodeParameterType::RD, OpcodeParameterType::RS_RT),
		inst(Opcode::FLDRX, Mnemonic::LDR, OpcodeParameterType::FD, OpcodeParameterType::RS_RT),
		inst(Opcode::STRX, Mnemonic::STR, OpcodeParameterType::RS, OpcodeParameterType::RD_RT),
		inst(Opcode::STRBX, Mnemonic::STRB, OpcodeParameterType::RS, OpcodeParameterType::RD_RT),
		inst(Opcode::STRHX, Mnemonic::STRH, OpcodeParameterType::RS, OpcodeParameterType::RD_RT),
		inst(Opcode::FSTRX, Mnemonic::STR, OpcodeParameterType::FS, OpcodeParameterType::RD_RT),
		// The near forms of LDV/STV: one word, no address to materialise, and so no scratch
		// register borrowed to hold one. The displacement is resolved against the instruction
		// itself at link time, and a variable further than +/- 32 KiB away is an error.
		inst(sig(Mnemonic::LDVP, OperandType::IntegralRegister, OperandType::VariableU8),
			op(Opcode::LDRBP, param(OpcodeParameterType::RD, 0), param(OpcodeParameterType::REL_SIMM16, 1))),
		inst(sig(Mnemonic::LDVP, OperandType::IntegralRegister, OperandType::VariableS8),
			op(Opcode::LDRSBP, param(OpcodeParameterType::RD, 0), param(OpcodeParameterType::REL_SIMM16, 1))),
		inst(sig(Mnemonic::LDVP, OperandType::IntegralRegister, OperandType::VariableU16),
			op(Opcode::LDRHP, param(OpcodeParameterType::RD, 0), param(OpcodeParameterType::REL_SIMM16, 1))),
		inst(sig(Mnemonic::LDVP, OperandType::IntegralRegister, OperandType::VariableS16),
			op(Opcode::LDRSHP, param(OpcodeParameterType::RD, 0), param(OpcodeParameterType::REL_SIMM16, 1))),
		inst(sig(Mnemonic::LDVP, OperandType::IntegralRegister, OperandType::VariableU32),
			op(Opcode::LDRP, param(OpcodeParameterType::RD, 0), param(OpcodeParameterType::REL_SIMM16, 1))),
		inst(sig(Mnemonic::LDVP, OperandType::IntegralRegister, OperandType::VariableS32),
			op(Opcode::LDRP, param(OpcodeParameterType::RD, 0), param(OpcodeParameterType::REL_SIMM16, 1))),
		inst(sig(Mnemonic::LDVP, OperandType::FloatingPointRegister, OperandType::VariableF32),
			op(Opcode::FLDRP, param(OpcodeParameterType::FD, 0), param(OpcodeParameterType::REL_SIMM16, 1))),
		inst(sig(Mnemonic::STVP, OperandType::IntegralRegister, OperandType::VariableU8),
			op(Opcode::STRBP, param(OpcodeParameterType::RS, 0), param(OpcodeParameterType::REL_SIMM16, 1))),
		inst(sig(Mnemonic::STVP, OperandType::IntegralRegister, OperandType::VariableS8),
			op(Opcode::STRBP, param(OpcodeParameterType::RS, 0), param(OpcodeParameterType::REL_SIMM16, 1))),
		inst(sig(Mnemonic::STVP, OperandType::IntegralRegister, OperandType::VariableU16),
			op(Opcode::STRHP, param(OpcodeParameterType::RS, 0), param(OpcodeParameterType::REL_SIMM16, 1))),
		inst(sig(Mnemonic::STVP, OperandType::IntegralRegister, OperandType::VariableS16),
			op(Opcode::STRHP, param(OpcodeParameterType::RS, 0), param(OpcodeParameterType::REL_SIMM16, 1))),
		inst(sig(Mnemonic::STVP, OperandType::IntegralRegister, OperandType::VariableU32),
			op(Opcode::STRP, param(OpcodeParameterType::RS, 0), param(OpcodeParameterType::REL_SIMM16, 1))),
		inst(sig(Mnemonic::STVP, OperandType::IntegralRegister, OperandType::VariableS32),
			op(Opcode::STRP, param(OpcodeParameterType::RS, 0), param(OpcodeParameterType::REL_SIMM16, 1))),
		inst(sig(Mnemonic::STVP, OperandType::FloatingPointRegister, OperandType::VariableF32),
			op(Opcode::FSTRP, param(OpcodeParameterType::FS, 0), param(OpcodeParameterType::REL_SIMM16, 1))),
		inst(Opcode::LEA, Mnemonic::LEA, OpcodeParameterType::RD, OpcodeParameterType::RS_SIMM16),

		inst(Opcode::JP, Mnemonic::JP, OpcodeParameterType::REL_ADDR),
		inst(Opcode::JP, Mnemonic::JP, OpcodeParameterType::SIMM24),
		inst(Opcode::JPR, Mnemonic::JP, OpcodeParameterType::RS),
		inst(Opcode::CMP, Mnemonic::CMP, OpcodeParameterType::RS, OpcodeParameterType::RT),
		inst(Opcode::CMPI, Mnemonic::CMP, OpcodeParameterType::RS, OpcodeParameterType::SIMM16),
		inst(Opcode::FCMP, Mnemonic::CMP, OpcodeParameterType::FS, OpcodeParameterType::FT),
		inst(Opcode::JZ, Mnemonic::JZ, OpcodeParameterType::REL_ADDR),
		inst(Opcode::JZ, Mnemonic::JZ, OpcodeParameterType::SIMM24),
		inst(Opcode::JZR, Mnemonic::JZ, OpcodeParameterType::RS),
		inst(Opcode::JNZ, Mnemonic::JNZ, OpcodeParameterType::REL_ADDR),
		inst(Opcode::JNZ, Mnemonic::JNZ, OpcodeParameterType::SIMM24),
		inst(Opcode::JNZR, Mnemonic::JNZ, OpcodeParameterType::RS),
		inst(Opcode::JC, Mnemonic::JC, OpcodeParameterType::REL_ADDR),
		inst(Opcode::JC, Mnemonic::JC, OpcodeParameterType::SIMM24),
		inst(Opcode::JCR, Mnemonic::JC, OpcodeParameterType::RS),
		inst(Opcode::JNC, Mnemonic::JNC, OpcodeParameterType::REL_ADDR),
		inst(Opcode::JNC, Mnemonic::JNC, OpcodeParameterType::SIMM24),
		inst(Opcode::JNCR, Mnemonic::JNC, OpcodeParameterType::RS),
		inst(Opcode::JS, Mnemonic::JS, OpcodeParameterType::REL_ADDR),
		inst(Opcode::JS, Mnemonic::JS, OpcodeParameterType::SIMM24),
		inst(Opcode::JSR, Mnemonic::JS, OpcodeParameterType::RS),
		inst(Opcode::JNS, Mnemonic::JNS, OpcodeParameterType::REL_ADDR),
		inst(Opcode::JNS, Mnemonic::JNS, OpcodeParameterType::SIMM24),
		inst(Opcode::JNSR, Mnemonic::JNS, OpcodeParameterType::RS),
		inst(Opcode::CALL, Mnemonic::CALL, OpcodeParameterType::REL_ADDR),
		inst(Opcode::CALL, Mnemonic::CALL, OpcodeParameterType::SIMM24),
		inst(Opcode::CALLR, Mnemonic::CALL, OpcodeParameterType::RS),
		inst(Opcode::RET, Mnemonic::RET),

		inst(Opcode::JP, Mnemonic::JMP, OpcodeParameterType::REL_ADDR),
		inst(Opcode::JP, Mnemonic::JMP, OpcodeParameterType::SIMM24),
		inst(Opcode::JPR, Mnemonic::JMP, OpcodeParameterType::RS),
		inst(Opcode::JZ, Mnemonic::JEQ, OpcodeParameterType::REL_ADDR),
		inst(Opcode::JZ, Mnemonic::JEQ, OpcodeParameterType::SIMM24),
		inst(Opcode::JZR, Mnemonic::JEQ, OpcodeParameterType::RS),
		inst(Opcode::JNZ, Mnemonic::JNE, OpcodeParameterType::REL_ADDR),
		inst(Opcode::JNZ, Mnemonic::JNE, OpcodeParameterType::SIMM24),
		inst(Opcode::JNZR, Mnemonic::JNE, OpcodeParameterType::RS),
		inst(Opcode::JGR, Mnemonic::JGR, OpcodeParameterType::REL_ADDR),
		inst(Opcode::JGR, Mnemonic::JGR, OpcodeParameterType::SIMM24),
		inst(Opcode::JGRR, Mnemonic::JGR, OpcodeParameterType::RS),
		inst(Opcode::JGE, Mnemonic::JGE, OpcodeParameterType::REL_ADDR),
		inst(Opcode::JGE, Mnemonic::JGE, OpcodeParameterType::SIMM24),
		inst(Opcode::JGER, Mnemonic::JGE, OpcodeParameterType::RS),
		inst(Opcode::JLS, Mnemonic::JLS, OpcodeParameterType::REL_ADDR),
		inst(Opcode::JLS, Mnemonic::JLS, OpcodeParameterType::SIMM24),
		inst(Opcode::JLSR, Mnemonic::JLS, OpcodeParameterType::RS),
		inst(Opcode::JLE, Mnemonic::JLE, OpcodeParameterType::REL_ADDR),
		inst(Opcode::JLE, Mnemonic::JLE, OpcodeParameterType::SIMM24),
		inst(Opcode::JLER, Mnemonic::JLE, OpcodeParameterType::RS),
		inst(Opcode::JAB, Mnemonic::JAB, OpcodeParameterType::REL_ADDR),
		inst(Opcode::JAB, Mnemonic::JAB, OpcodeParameterType::SIMM24),
		inst(Opcode::JABR, Mnemonic::JAB, OpcodeParameterType::RS),
		inst(Opcode::JAE, Mnemonic::JAE, OpcodeParameterType::REL_ADDR),
		inst(Opcode::JAE, Mnemonic::JAE, OpcodeParameterType::SIMM24),
		inst(Opcode::JAER, Mnemonic::JAE, OpcodeParameterType::RS),
		inst(Opcode::JBL, Mnemonic::JBL, OpcodeParameterType::REL_ADDR),
		inst(Opcode::JBL, Mnemonic::JBL, OpcodeParameterType::SIMM24),
		inst(Opcode::JBLR, Mnemonic::JBL, OpcodeParameterType::RS),
		inst(Opcode::JBE, Mnemonic::JBE, OpcodeParameterType::REL_ADDR),
		inst(Opcode::JBE, Mnemonic::JBE, OpcodeParameterType::SIMM24),
		inst(Opcode::JBER, Mnemonic::JBE, OpcodeParameterType::RS),
		inst(sig(Mnemonic::IFEQ, OperandType::IntegralRegister, OperandType::IntegralRegister, OperandType::Label), {
			op(Opcode::CMP, param(OpcodeParameterType::RS, 0), param(OpcodeParameterType::RT, 1)),
			op(Opcode::JZ, param(OpcodeParameterType::REL_ADDR, 2))
		}),
		inst(sig(Mnemonic::IFEQ, OperandType::IntegralRegister, OperandType::Immediate, OperandType::Label), {
			op(Opcode::CMPI, param(OpcodeParameterType::RS, 0), param(OpcodeParameterType::SIMM16, 1)),
			op(Opcode::JZ, param(OpcodeParameterType::REL_ADDR, 2))
		}),
		inst(sig(Mnemonic::IFEQ, OperandType::FloatingPointRegister, OperandType::FloatingPointRegister, OperandType::Label), {
			op(Opcode::FCMP, param(OpcodeParameterType::FS, 0), param(OpcodeParameterType::FT, 1)),
			op(Opcode::JZ, param(OpcodeParameterType::REL_ADDR, 2))
		}),
		inst(sig(Mnemonic::IFNE, OperandType::IntegralRegister, OperandType::IntegralRegister, OperandType::Label), {
			op(Opcode::CMP, param(OpcodeParameterType::RS, 0), param(OpcodeParameterType::RT, 1)),
			op(Opcode::JNZ, param(OpcodeParameterType::REL_ADDR, 2))
		}),
		inst(sig(Mnemonic::IFNE, OperandType::IntegralRegister, OperandType::Immediate, OperandType::Label), {
			op(Opcode::CMPI, param(OpcodeParameterType::RS, 0), param(OpcodeParameterType::SIMM16, 1)),
			op(Opcode::JNZ, param(OpcodeParameterType::REL_ADDR, 2))
		}),
		inst(sig(Mnemonic::IFNE, OperandType::FloatingPointRegister, OperandType::FloatingPointRegister, OperandType::Label), {
			op(Opcode::FCMP, param(OpcodeParameterType::FS, 0), param(OpcodeParameterType::FT, 1)),
			op(Opcode::JNZ, param(OpcodeParameterType::REL_ADDR, 2))
		}),
		inst(sig(Mnemonic::IFGR, OperandType::IntegralRegister, OperandType::IntegralRegister, OperandType::Label), {
			op(Opcode::CMP, param(OpcodeParameterType::RS, 0), param(OpcodeParameterType::RT, 1)),
			op(Opcode::JGR, param(OpcodeParameterType::REL_ADDR, 2))
		}),
		inst(sig(Mnemonic::IFGR, OperandType::IntegralRegister, OperandType::Immediate, OperandType::Label), {
			op(Opcode::CMPI, param(OpcodeParameterType::RS, 0), param(OpcodeParameterType::SIMM16, 1)),
			op(Opcode::JGR, param(OpcodeParameterType::REL_ADDR, 2))
		}),
		inst(sig(Mnemonic::IFGR, OperandType::FloatingPointRegister, OperandType::FloatingPointRegister, OperandType::Label), {
			op(Opcode::FCMP, param(OpcodeParameterType::FS, 0), param(OpcodeParameterType::FT, 1)),
			op(Opcode::JGR, param(OpcodeParameterType::REL_ADDR, 2))
		}),
		inst(sig(Mnemonic::IFGE, OperandType::IntegralRegister, OperandType::IntegralRegister, OperandType::Label), {
			op(Opcode::CMP, param(OpcodeParameterType::RS, 0), param(OpcodeParameterType::RT, 1)),
			op(Opcode::JGE, param(OpcodeParameterType::REL_ADDR, 2))
		}),
		inst(sig(Mnemonic::IFGE, OperandType::IntegralRegister, OperandType::Immediate, OperandType::Label), {
			op(Opcode::CMPI, param(OpcodeParameterType::RS, 0), param(OpcodeParameterType::SIMM16, 1)),
			op(Opcode::JGE, param(OpcodeParameterType::REL_ADDR, 2))
		}),
		inst(sig(Mnemonic::IFGE, OperandType::FloatingPointRegister, OperandType::FloatingPointRegister, OperandType::Label), {
			op(Opcode::FCMP, param(OpcodeParameterType::FS, 0), param(OpcodeParameterType::FT, 1)),
			op(Opcode::JGE, param(OpcodeParameterType::REL_ADDR, 2))
		}),
		inst(sig(Mnemonic::IFLS, OperandType::IntegralRegister, OperandType::IntegralRegister, OperandType::Label), {
			op(Opcode::CMP, param(OpcodeParameterType::RS, 0), param(OpcodeParameterType::RT, 1)),
			op(Opcode::JLS, param(OpcodeParameterType::REL_ADDR, 2))
		}),
		inst(sig(Mnemonic::IFLS, OperandType::IntegralRegister, OperandType::Immediate, OperandType::Label), {
			op(Opcode::CMPI, param(OpcodeParameterType::RS, 0), param(OpcodeParameterType::SIMM16, 1)),
			op(Opcode::JLS, param(OpcodeParameterType::REL_ADDR, 2))
		}),
		inst(sig(Mnemonic::IFLS, OperandType::FloatingPointRegister, OperandType::FloatingPointRegister, OperandType::Label), {
			op(Opcode::FCMP, param(OpcodeParameterType::FS, 0), param(OpcodeParameterType::FT, 1)),
			op(Opcode::JLS, param(OpcodeParameterType::REL_ADDR, 2))
		}),
		inst(sig(Mnemonic::IFLE, OperandType::IntegralRegister, OperandType::IntegralRegister, OperandType::Label), {
			op(Opcode::CMP, param(OpcodeParameterType::RS, 0), param(OpcodeParameterType::RT, 1)),
			op(Opcode::JLE, param(OpcodeParameterType::REL_ADDR, 2))
		}),
		inst(sig(Mnemonic::IFLE, OperandType::IntegralRegister, OperandType::Immediate, OperandType::Label), {
			op(Opcode::CMPI, param(OpcodeParameterType::RS, 0), param(OpcodeParameterType::SIMM16, 1)),
			op(Opcode::JLE, param(OpcodeParameterType::REL_ADDR, 2))
		}),
		inst(sig(Mnemonic::IFLE, OperandType::FloatingPointRegister, OperandType::FloatingPointRegister, OperandType::Label), {
			op(Opcode::FCMP, param(OpcodeParameterType::FS, 0), param(OpcodeParameterType::FT, 1)),
			op(Opcode::JLE, param(OpcodeParameterType::REL_ADDR, 2))
		}),
		inst(sig(Mnemonic::IFAB, OperandType::IntegralRegister, OperandType::IntegralRegister, OperandType::Label), {
			op(Opcode::CMP, param(OpcodeParameterType::RS, 0), param(OpcodeParameterType::RT, 1)),
			op(Opcode::JAB, param(OpcodeParameterType::REL_ADDR, 2))
		}),
		inst(sig(Mnemonic::IFAB, OperandType::IntegralRegister, OperandType::Immediate, OperandType::Label), {
			op(Opcode::CMPI, param(OpcodeParameterType::RS, 0), param(OpcodeParameterType::SIMM16, 1)),
			op(Opcode::JAB, param(OpcodeParameterType::REL_ADDR, 2))
		}),
		inst(sig(Mnemonic::IFAB, OperandType::FloatingPointRegister, OperandType::FloatingPointRegister, OperandType::Label), {
			op(Opcode::FCMP, param(OpcodeParameterType::FS, 0), param(OpcodeParameterType::FT, 1)),
			op(Opcode::JAB, param(OpcodeParameterType::REL_ADDR, 2))
		}),
		inst(sig(Mnemonic::IFAE, OperandType::IntegralRegister, OperandType::IntegralRegister, OperandType::Label), {
			op(Opcode::CMP, param(OpcodeParameterType::RS, 0), param(OpcodeParameterType::RT, 1)),
			op(Opcode::JAE, param(OpcodeParameterType::REL_ADDR, 2))
		}),
		inst(sig(Mnemonic::IFAE, OperandType::IntegralRegister, OperandType::Immediate, OperandType::Label), {
			op(Opcode::CMPI, param(OpcodeParameterType::RS, 0), param(OpcodeParameterType::SIMM16, 1)),
			op(Opcode::JAE, param(OpcodeParameterType::REL_ADDR, 2))
		}),
		inst(sig(Mnemonic::IFAE, OperandType::FloatingPointRegister, OperandType::FloatingPointRegister, OperandType::Label), {
			op(Opcode::FCMP, param(OpcodeParameterType::FS, 0), param(OpcodeParameterType::FT, 1)),
			op(Opcode::JAE, param(OpcodeParameterType::REL_ADDR, 2))
		}),
		inst(sig(Mnemonic::IFBL, OperandType::IntegralRegister, OperandType::IntegralRegister, OperandType::Label), {
			op(Opcode::CMP, param(OpcodeParameterType::RS, 0), param(OpcodeParameterType::RT, 1)),
			op(Opcode::JBL, param(OpcodeParameterType::REL_ADDR, 2))
		}),
		inst(sig(Mnemonic::IFBL, OperandType::IntegralRegister, OperandType::Immediate, OperandType::Label), {
			op(Opcode::CMPI, param(OpcodeParameterType::RS, 0), param(OpcodeParameterType::SIMM16, 1)),
			op(Opcode::JBL, param(OpcodeParameterType::REL_ADDR, 2))
		}),
		inst(sig(Mnemonic::IFBL, OperandType::FloatingPointRegister, OperandType::FloatingPointRegister, OperandType::Label), {
			op(Opcode::FCMP, param(OpcodeParameterType::FS, 0), param(OpcodeParameterType::FT, 1)),
			op(Opcode::JBL, param(OpcodeParameterType::REL_ADDR, 2))
		}),
		inst(sig(Mnemonic::IFBE, OperandType::IntegralRegister, OperandType::IntegralRegister, OperandType::Label), {
			op(Opcode::CMP, param(OpcodeParameterType::RS, 0), param(OpcodeParameterType::RT, 1)),
			op(Opcode::JBE, param(OpcodeParameterType::REL_ADDR, 2))
		}),
		inst(sig(Mnemonic::IFBE, OperandType::IntegralRegister, OperandType::Immediate, OperandType::Label), {
			op(Opcode::CMPI, param(OpcodeParameterType::RS, 0), param(OpcodeParameterType::SIMM16, 1)),
			op(Opcode::JBE, param(OpcodeParameterType::REL_ADDR, 2))
		}),
		inst(sig(Mnemonic::IFBE, OperandType::FloatingPointRegister, OperandType::FloatingPointRegister, OperandType::Label), {
			op(Opcode::FCMP, param(OpcodeParameterType::FS, 0), param(OpcodeParameterType::FT, 1)),
			op(Opcode::JBE, param(OpcodeParameterType::REL_ADDR, 2))
		}),
		// A full 32-bit constant, the immediate counterpart of LA. LI only reaches 16 bits.
		inst(sig(Mnemonic::LC, OperandType::IntegralRegister, OperandType::Immediate), {
			op(Opcode::LUI, param(OpcodeParameterType::RD, 0), param(OpcodeParameterType::IMM16, 1, 16)),
			op(Opcode::ORI, param(OpcodeParameterType::RD, 0), param(OpcodeParameterType::RS, 0), param(OpcodeParameterType::IMM16_LOW, 1))
		}),
		inst(sig(Mnemonic::INC, OperandType::IntegralRegister),
			op(Opcode::ADDI, param(OpcodeParameterType::RD, 0), param(OpcodeParameterType::RS, 0), paramUFixed(OpcodeParameterType::IMM16, 1))
		),
		inst(sig(Mnemonic::DEC, OperandType::IntegralRegister),
			op(Opcode::SUBI, param(OpcodeParameterType::RD, 0), param(OpcodeParameterType::RS, 0), paramUFixed(OpcodeParameterType::IMM16, 1))
		),
		inst(sig(Mnemonic::CLR, OperandType::IntegralRegister),
			op(Opcode::LI, param(OpcodeParameterType::RD, 0), paramUFixed(OpcodeParameterType::IMM16, 0))
		),
		inst(sig(Mnemonic::TST, OperandType::IntegralRegister),
			op(Opcode::CMPI, param(OpcodeParameterType::RS, 0), paramSFixed(OpcodeParameterType::SIMM16, 0))
		),
		// Exchange without a temporary, so it costs no scratch register.
		inst(sig(Mnemonic::SWAP, OperandType::IntegralRegister, OperandType::IntegralRegister), {
			op(Opcode::XOR, param(OpcodeParameterType::RD, 0), param(OpcodeParameterType::RS, 0), param(OpcodeParameterType::RT, 1)),
			op(Opcode::XOR, param(OpcodeParameterType::RD, 1), param(OpcodeParameterType::RS, 0), param(OpcodeParameterType::RT, 1)),
			op(Opcode::XOR, param(OpcodeParameterType::RD, 0), param(OpcodeParameterType::RS, 0), param(OpcodeParameterType::RT, 1))
		}),
		// The frame pointer had no instruction that touched it; these are it.
		inst(sig(Mnemonic::ENTER), {
			op(Opcode::PUSH, paramUFixed(OpcodeParameterType::RS, FramePointerRegister)),
			op(Opcode::MOV, paramUFixed(OpcodeParameterType::RD, FramePointerRegister), paramUFixed(OpcodeParameterType::RS, StackPointerRegister))
		}),
		inst(sig(Mnemonic::LEAVE), {
			op(Opcode::MOV, paramUFixed(OpcodeParameterType::RD, StackPointerRegister), paramUFixed(OpcodeParameterType::RS, FramePointerRegister)),
			op(Opcode::POP, paramUFixed(OpcodeParameterType::RD, FramePointerRegister))
		}),
		inst(Opcode::JO, Mnemonic::JO, OpcodeParameterType::REL_ADDR),
		inst(Opcode::JO, Mnemonic::JO, OpcodeParameterType::SIMM24),
		inst(Opcode::JOR, Mnemonic::JO, OpcodeParameterType::RS),
		inst(Opcode::JNO, Mnemonic::JNO, OpcodeParameterType::REL_ADDR),
		inst(Opcode::JNO, Mnemonic::JNO, OpcodeParameterType::SIMM24),
		inst(Opcode::JNOR, Mnemonic::JNO, OpcodeParameterType::RS),

		inst(Opcode::PUSH, Mnemonic::PUSH, OpcodeParameterType::RS),
		inst(Opcode::POP, Mnemonic::POP, OpcodeParameterType::RD),
		inst(Opcode::PUSHM, Mnemonic::PUSHM, OpcodeParameterType::IMM16),
		inst(Opcode::POPM, Mnemonic::POPM, OpcodeParameterType::IMM16),
		inst(Opcode::PUSHF, Mnemonic::PUSHF),
		inst(Opcode::POPF, Mnemonic::POPF),
		inst(Opcode::FPUSH, Mnemonic::PUSH, OpcodeParameterType::FS),
		inst(Opcode::FPOP, Mnemonic::POP, OpcodeParameterType::FD),

		inst(Opcode::ITOF, Mnemonic::ITOF, OpcodeParameterType::FD, OpcodeParameterType::RS),
		inst(Opcode::IITOF, Mnemonic::IITOF, OpcodeParameterType::FD, OpcodeParameterType::RS),
		inst(Opcode::FTOI, Mnemonic::FTOI, OpcodeParameterType::RD, OpcodeParameterType::FS),
		inst(Opcode::FTOII, Mnemonic::FTOII, OpcodeParameterType::RD, OpcodeParameterType::FS),
		inst(Opcode::MTF, Mnemonic::MTF, OpcodeParameterType::FD, OpcodeParameterType::RS),
		inst(Opcode::MFF, Mnemonic::MFF, OpcodeParameterType::RD, OpcodeParameterType::FS),

		inst(sig(Mnemonic::IN, OperandType::Immediate, OperandType::IntegralRegister),
			op(Opcode::IN, param(OpcodeParameterType::RD, 1), param(OpcodeParameterType::IMM8, 0))
		),
		inst(sig(Mnemonic::INB, OperandType::Immediate, OperandType::IntegralRegister),
			op(Opcode::INB, param(OpcodeParameterType::RD, 1), param(OpcodeParameterType::IMM8, 0))
		),
		inst(sig(Mnemonic::INH, OperandType::Immediate, OperandType::IntegralRegister),
			op(Opcode::INH, param(OpcodeParameterType::RD, 1), param(OpcodeParameterType::IMM8, 0))
		),
		inst(sig(Mnemonic::INSB, OperandType::Immediate, OperandType::IntegralRegister),
			op(Opcode::INSB, param(OpcodeParameterType::RD, 1), param(OpcodeParameterType::IMM8, 0))
		),
		inst(sig(Mnemonic::INSH, OperandType::Immediate, OperandType::IntegralRegister),
			op(Opcode::INSH, param(OpcodeParameterType::RD, 1), param(OpcodeParameterType::IMM8, 0))
		),
		inst(sig(Mnemonic::INM, OperandType::Immediate, OperandType::IntegralRegister, OperandType::IntegralRegister),
			op(Opcode::INM, param(OpcodeParameterType::RD, 1), param(OpcodeParameterType::RS, 2), param(OpcodeParameterType::IMM8, 0))
		),

		inst(sig(Mnemonic::IN, OperandType::IntegralRegister, OperandType::IntegralRegister),
			op(Opcode::INR, param(OpcodeParameterType::RD, 1), param(OpcodeParameterType::RS, 0))
		),
		inst(sig(Mnemonic::INB, OperandType::IntegralRegister, OperandType::IntegralRegister),
			op(Opcode::INRB, param(OpcodeParameterType::RD, 1), param(OpcodeParameterType::RS, 0))
		),
		inst(sig(Mnemonic::INH, OperandType::IntegralRegister, OperandType::IntegralRegister),
			op(Opcode::INRH, param(OpcodeParameterType::RD, 1), param(OpcodeParameterType::RS, 0))
		),
		inst(sig(Mnemonic::INSB, OperandType::IntegralRegister, OperandType::IntegralRegister),
			op(Opcode::INRSB, param(OpcodeParameterType::RD, 1), param(OpcodeParameterType::RS, 0))
		),
		inst(sig(Mnemonic::INSH, OperandType::IntegralRegister, OperandType::IntegralRegister),
			op(Opcode::INRSH, param(OpcodeParameterType::RD, 1), param(OpcodeParameterType::RS, 0))
		),
		inst(sig(Mnemonic::INM, OperandType::IntegralRegister, OperandType::IntegralRegister, OperandType::IntegralRegister),
			op(Opcode::INRM, param(OpcodeParameterType::RD, 1), param(OpcodeParameterType::RS, 0), param(OpcodeParameterType::RT, 2))
		),

		inst(sig(Mnemonic::OUT, OperandType::Immediate, OperandType::IntegralRegister),
			op(Opcode::OUT, param(OpcodeParameterType::RS, 1), param(OpcodeParameterType::IMM8, 0))
		),
		inst(sig(Mnemonic::OUTB, OperandType::Immediate, OperandType::IntegralRegister),
			op(Opcode::OUTB, param(OpcodeParameterType::RS, 1), param(OpcodeParameterType::IMM8, 0))
		),
		inst(sig(Mnemonic::OUTH, OperandType::Immediate, OperandType::IntegralRegister),
			op(Opcode::OUTH, param(OpcodeParameterType::RS, 1), param(OpcodeParameterType::IMM8, 0))
		),
		inst(sig(Mnemonic::OUTM, OperandType::Immediate, OperandType::IntegralRegister, OperandType::IntegralRegister),
			op(Opcode::OUTM, param(OpcodeParameterType::RS, 1), param(OpcodeParameterType::RT, 2), param(OpcodeParameterType::IMM8, 0))
		),

		inst(sig(Mnemonic::OUT, OperandType::IntegralRegister, OperandType::IntegralRegister),
			op(Opcode::OUTR, param(OpcodeParameterType::RS, 1), param(OpcodeParameterType::RT, 0))
		),
		inst(sig(Mnemonic::OUTB, OperandType::IntegralRegister, OperandType::IntegralRegister),
			op(Opcode::OUTRB, param(OpcodeParameterType::RS, 1), param(OpcodeParameterType::RT, 0))
		),
		inst(sig(Mnemonic::OUTH, OperandType::IntegralRegister, OperandType::IntegralRegister),
			op(Opcode::OUTRH, param(OpcodeParameterType::RS, 1), param(OpcodeParameterType::RT, 0))
		),
		inst(sig(Mnemonic::OUTM, OperandType::IntegralRegister, OperandType::IntegralRegister, OperandType::IntegralRegister),
			op(Opcode::OUTRM, param(OpcodeParameterType::RD, 2), param(OpcodeParameterType::RS, 1), param(OpcodeParameterType::RT, 0))
		)
	};

	std::optional<InstructionInfo> InstructionInfo::find(InstructionSignature signature) noexcept
	{
		if (auto it = __instructionsInfoMap.find(signature); it != __instructionsInfoMap.end())
			return it->second;
		return std::nullopt;
	}

	static std::unordered_map<Mnemonic, u32> calculateInstructionMaxSizesInBytesMap() noexcept
	{
		std::unordered_map<Mnemonic, u32> sizesMap;
		for (const auto& [signature, info] : __instructionsInfoMap)
		{
			if (auto it = sizesMap.find(signature.mnemonic); it != sizesMap.end())
			{
				u32 newSize = info.sizeInBytes();
				if (newSize > it->second)
					it->second = newSize;
			}
			else
				sizesMap[signature.mnemonic] = info.sizeInBytes();
		}
		return sizesMap;
	}

	static std::unordered_map<Mnemonic, u32> __instructionMaxSizesInBytesMap = calculateInstructionMaxSizesInBytesMap();

	std::optional<u32> InstructionInfo::findMaxSizeInBytes(Mnemonic mnemonic) noexcept
	{
		if (auto it = __instructionMaxSizesInBytesMap.find(mnemonic); it != __instructionMaxSizesInBytesMap.end())
			return it->second;
		return std::nullopt;
	}
}
