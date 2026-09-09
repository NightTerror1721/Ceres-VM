#include <ceres/asm/instruction_info.h>
#include <unordered_map>

namespace ceres::casm
{
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

		// The high half of a multiply, the orderings, the bit counts and the two widenings. `abs`
		// picks FABS for a pair of float registers, the way `neg` picks FNEG.
		inst(Opcode::MULH, Mnemonic::MULH, OpcodeParameterType::RD, OpcodeParameterType::RS, OpcodeParameterType::RT),
		inst(Opcode::IMULH, Mnemonic::IMULH, OpcodeParameterType::RD, OpcodeParameterType::RS, OpcodeParameterType::RT),
		inst(Opcode::MIN, Mnemonic::MIN, OpcodeParameterType::RD, OpcodeParameterType::RS, OpcodeParameterType::RT),
		inst(Opcode::IMIN, Mnemonic::IMIN, OpcodeParameterType::RD, OpcodeParameterType::RS, OpcodeParameterType::RT),
		inst(Opcode::MAX, Mnemonic::MAX, OpcodeParameterType::RD, OpcodeParameterType::RS, OpcodeParameterType::RT),
		inst(Opcode::IMAX, Mnemonic::IMAX, OpcodeParameterType::RD, OpcodeParameterType::RS, OpcodeParameterType::RT),
		inst(Opcode::ROL, Mnemonic::ROL, OpcodeParameterType::RD, OpcodeParameterType::RS, OpcodeParameterType::RT),
		inst(Opcode::ROR, Mnemonic::ROR, OpcodeParameterType::RD, OpcodeParameterType::RS, OpcodeParameterType::RT),
		inst(Opcode::MINI, Mnemonic::MIN, OpcodeParameterType::RD, OpcodeParameterType::RS, OpcodeParameterType::IMM16),
		inst(Opcode::IMINI, Mnemonic::IMIN, OpcodeParameterType::RD, OpcodeParameterType::RS, OpcodeParameterType::SIMM16),
		inst(Opcode::MAXI, Mnemonic::MAX, OpcodeParameterType::RD, OpcodeParameterType::RS, OpcodeParameterType::IMM16),
		inst(Opcode::IMAXI, Mnemonic::IMAX, OpcodeParameterType::RD, OpcodeParameterType::RS, OpcodeParameterType::SIMM16),
		inst(Opcode::ROLI, Mnemonic::ROL, OpcodeParameterType::RD, OpcodeParameterType::RS, OpcodeParameterType::IMM16),
		inst(Opcode::RORI, Mnemonic::ROR, OpcodeParameterType::RD, OpcodeParameterType::RS, OpcodeParameterType::IMM16),
		inst(Opcode::ABS, Mnemonic::ABS, OpcodeParameterType::RD, OpcodeParameterType::RS),
		inst(Opcode::CLZ, Mnemonic::CLZ, OpcodeParameterType::RD, OpcodeParameterType::RS),
		inst(Opcode::POPCNT, Mnemonic::POPCNT, OpcodeParameterType::RD, OpcodeParameterType::RS),
		inst(Opcode::BSWAP, Mnemonic::BSWAP, OpcodeParameterType::RD, OpcodeParameterType::RS),
		inst(Opcode::SXTB, Mnemonic::SXTB, OpcodeParameterType::RD, OpcodeParameterType::RS),
		inst(Opcode::SXTH, Mnemonic::SXTH, OpcodeParameterType::RD, OpcodeParameterType::RS),
		inst(Opcode::FSQRT, Mnemonic::SQRT, OpcodeParameterType::FD, OpcodeParameterType::FS),
		inst(Opcode::FABS, Mnemonic::ABS, OpcodeParameterType::FD, OpcodeParameterType::FS),

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
		inst(Opcode::STR, Mnemonic::STR, OpcodeParameterType::RD_SIMM16, OpcodeParameterType::RS),
		inst(Opcode::STRB, Mnemonic::STRB, OpcodeParameterType::RD_SIMM16, OpcodeParameterType::RS),
		inst(Opcode::STRH, Mnemonic::STRH, OpcodeParameterType::RD_SIMM16, OpcodeParameterType::RS),
		inst(Opcode::FSTR, Mnemonic::STR, OpcodeParameterType::RD_SIMM16, OpcodeParameterType::FS),
		inst(sig(Mnemonic::STV, OperandType::VariableU8, OperandType::IntegralRegister), {
			op(Opcode::LUI, paramUFixed(OpcodeParameterType::RD, ScratchRegister), param(OpcodeParameterType::IMM16, 0, 16)),
			op(Opcode::ORI, paramUFixed(OpcodeParameterType::RD, ScratchRegister), paramUFixed(OpcodeParameterType::RS, ScratchRegister), param(OpcodeParameterType::IMM16_LOW, 0)),
			op(Opcode::STRB, paramUFixed(OpcodeParameterType::RD, ScratchRegister), param(OpcodeParameterType::RS, 1), paramUFixed(OpcodeParameterType::IMM16, 0))
		}),
		inst(sig(Mnemonic::STV, OperandType::VariableS8, OperandType::IntegralRegister), {
			op(Opcode::LUI, paramUFixed(OpcodeParameterType::RD, ScratchRegister), param(OpcodeParameterType::IMM16, 0, 16)),
			op(Opcode::ORI, paramUFixed(OpcodeParameterType::RD, ScratchRegister), paramUFixed(OpcodeParameterType::RS, ScratchRegister), param(OpcodeParameterType::IMM16_LOW, 0)),
			op(Opcode::STRB, paramUFixed(OpcodeParameterType::RD, ScratchRegister), param(OpcodeParameterType::RS, 1), paramUFixed(OpcodeParameterType::IMM16, 0))
		}),
		inst(sig(Mnemonic::STV, OperandType::VariableU16, OperandType::IntegralRegister), {
			op(Opcode::LUI, paramUFixed(OpcodeParameterType::RD, ScratchRegister), param(OpcodeParameterType::IMM16, 0, 16)),
			op(Opcode::ORI, paramUFixed(OpcodeParameterType::RD, ScratchRegister), paramUFixed(OpcodeParameterType::RS, ScratchRegister), param(OpcodeParameterType::IMM16_LOW, 0)),
			op(Opcode::STRH, paramUFixed(OpcodeParameterType::RD, ScratchRegister), param(OpcodeParameterType::RS, 1), paramUFixed(OpcodeParameterType::IMM16, 0))
		}),
		inst(sig(Mnemonic::STV, OperandType::VariableS16, OperandType::IntegralRegister), {
			op(Opcode::LUI, paramUFixed(OpcodeParameterType::RD, ScratchRegister), param(OpcodeParameterType::IMM16, 0, 16)),
			op(Opcode::ORI, paramUFixed(OpcodeParameterType::RD, ScratchRegister), paramUFixed(OpcodeParameterType::RS, ScratchRegister), param(OpcodeParameterType::IMM16_LOW, 0)),
			op(Opcode::STRH, paramUFixed(OpcodeParameterType::RD, ScratchRegister), param(OpcodeParameterType::RS, 1), paramUFixed(OpcodeParameterType::IMM16, 0))
		}),
		inst(sig(Mnemonic::STV, OperandType::VariableU32, OperandType::IntegralRegister), {
			op(Opcode::LUI, paramUFixed(OpcodeParameterType::RD, ScratchRegister), param(OpcodeParameterType::IMM16, 0, 16)),
			op(Opcode::ORI, paramUFixed(OpcodeParameterType::RD, ScratchRegister), paramUFixed(OpcodeParameterType::RS, ScratchRegister), param(OpcodeParameterType::IMM16_LOW, 0)),
			op(Opcode::STR, paramUFixed(OpcodeParameterType::RD, ScratchRegister), param(OpcodeParameterType::RS, 1), paramUFixed(OpcodeParameterType::IMM16, 0))
		}),
		inst(sig(Mnemonic::STV, OperandType::VariableS32, OperandType::IntegralRegister), {
			op(Opcode::LUI, paramUFixed(OpcodeParameterType::RD, ScratchRegister), param(OpcodeParameterType::IMM16, 0, 16)),
			op(Opcode::ORI, paramUFixed(OpcodeParameterType::RD, ScratchRegister), paramUFixed(OpcodeParameterType::RS, ScratchRegister), param(OpcodeParameterType::IMM16_LOW, 0)),
			op(Opcode::STR, paramUFixed(OpcodeParameterType::RD, ScratchRegister), param(OpcodeParameterType::RS, 1), paramUFixed(OpcodeParameterType::IMM16, 0))
		}),
		inst(sig(Mnemonic::STV, OperandType::VariableF32, OperandType::FloatingPointRegister), {
			op(Opcode::LUI, paramUFixed(OpcodeParameterType::RD, ScratchRegister), param(OpcodeParameterType::IMM16, 0, 16)),
			op(Opcode::ORI, paramUFixed(OpcodeParameterType::RD, ScratchRegister), paramUFixed(OpcodeParameterType::RS, ScratchRegister), param(OpcodeParameterType::IMM16_LOW, 0)),
			op(Opcode::FSTR, paramUFixed(OpcodeParameterType::RD, ScratchRegister), param(OpcodeParameterType::FS, 1), paramUFixed(OpcodeParameterType::IMM16, 0))
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
		// The access width written on the access instead of in the mnemonic. One `ldr` and one
		// `str` cover what the whole ldrb/ldrh/ldrsb/ldrsh family covers, because the operand
		// type now says what a bare `[rs + imm]` could not - exactly as a variable's declared
		// type already does for LDV and STV. The width-suffixed mnemonics still work.
		inst(sig(Mnemonic::LDR, OperandType::IntegralRegister, OperandType::MemoryU8),
			op(Opcode::LDRB, param(OpcodeParameterType::RD, 0), param(OpcodeParameterType::RS_SIMM16, 1))),
		inst(sig(Mnemonic::LDR, OperandType::IntegralRegister, OperandType::IndexedU8),
			op(Opcode::LDRBX, param(OpcodeParameterType::RD, 0), param(OpcodeParameterType::RS_RT, 1))),
		inst(sig(Mnemonic::LDR, OperandType::IntegralRegister, OperandType::MemoryS8),
			op(Opcode::LDRSB, param(OpcodeParameterType::RD, 0), param(OpcodeParameterType::RS_SIMM16, 1))),
		inst(sig(Mnemonic::LDR, OperandType::IntegralRegister, OperandType::IndexedS8),
			op(Opcode::LDRSBX, param(OpcodeParameterType::RD, 0), param(OpcodeParameterType::RS_RT, 1))),
		inst(sig(Mnemonic::LDR, OperandType::IntegralRegister, OperandType::MemoryU16),
			op(Opcode::LDRH, param(OpcodeParameterType::RD, 0), param(OpcodeParameterType::RS_SIMM16, 1))),
		inst(sig(Mnemonic::LDR, OperandType::IntegralRegister, OperandType::IndexedU16),
			op(Opcode::LDRHX, param(OpcodeParameterType::RD, 0), param(OpcodeParameterType::RS_RT, 1))),
		inst(sig(Mnemonic::LDR, OperandType::IntegralRegister, OperandType::MemoryS16),
			op(Opcode::LDRSH, param(OpcodeParameterType::RD, 0), param(OpcodeParameterType::RS_SIMM16, 1))),
		inst(sig(Mnemonic::LDR, OperandType::IntegralRegister, OperandType::IndexedS16),
			op(Opcode::LDRSHX, param(OpcodeParameterType::RD, 0), param(OpcodeParameterType::RS_RT, 1))),
		inst(sig(Mnemonic::LDR, OperandType::IntegralRegister, OperandType::MemoryU32),
			op(Opcode::LDR, param(OpcodeParameterType::RD, 0), param(OpcodeParameterType::RS_SIMM16, 1))),
		inst(sig(Mnemonic::LDR, OperandType::IntegralRegister, OperandType::IndexedU32),
			op(Opcode::LDRX, param(OpcodeParameterType::RD, 0), param(OpcodeParameterType::RS_RT, 1))),
		inst(sig(Mnemonic::LDR, OperandType::IntegralRegister, OperandType::MemoryS32),
			op(Opcode::LDR, param(OpcodeParameterType::RD, 0), param(OpcodeParameterType::RS_SIMM16, 1))),
		inst(sig(Mnemonic::LDR, OperandType::IntegralRegister, OperandType::IndexedS32),
			op(Opcode::LDRX, param(OpcodeParameterType::RD, 0), param(OpcodeParameterType::RS_RT, 1))),
		inst(sig(Mnemonic::LDR, OperandType::FloatingPointRegister, OperandType::MemoryF32),
			op(Opcode::FLDR, param(OpcodeParameterType::FD, 0), param(OpcodeParameterType::RS_SIMM16, 1))),
		inst(sig(Mnemonic::LDR, OperandType::FloatingPointRegister, OperandType::IndexedF32),
			op(Opcode::FLDRX, param(OpcodeParameterType::FD, 0), param(OpcodeParameterType::RS_RT, 1))),
		inst(sig(Mnemonic::STR, OperandType::MemoryU8, OperandType::IntegralRegister),
			op(Opcode::STRB, param(OpcodeParameterType::RS, 1), param(OpcodeParameterType::RD_SIMM16, 0))),
		inst(sig(Mnemonic::STR, OperandType::IndexedU8, OperandType::IntegralRegister),
			op(Opcode::STRBX, param(OpcodeParameterType::RS, 1), param(OpcodeParameterType::RD_RT, 0))),
		inst(sig(Mnemonic::STR, OperandType::MemoryS8, OperandType::IntegralRegister),
			op(Opcode::STRB, param(OpcodeParameterType::RS, 1), param(OpcodeParameterType::RD_SIMM16, 0))),
		inst(sig(Mnemonic::STR, OperandType::IndexedS8, OperandType::IntegralRegister),
			op(Opcode::STRBX, param(OpcodeParameterType::RS, 1), param(OpcodeParameterType::RD_RT, 0))),
		inst(sig(Mnemonic::STR, OperandType::MemoryU16, OperandType::IntegralRegister),
			op(Opcode::STRH, param(OpcodeParameterType::RS, 1), param(OpcodeParameterType::RD_SIMM16, 0))),
		inst(sig(Mnemonic::STR, OperandType::IndexedU16, OperandType::IntegralRegister),
			op(Opcode::STRHX, param(OpcodeParameterType::RS, 1), param(OpcodeParameterType::RD_RT, 0))),
		inst(sig(Mnemonic::STR, OperandType::MemoryS16, OperandType::IntegralRegister),
			op(Opcode::STRH, param(OpcodeParameterType::RS, 1), param(OpcodeParameterType::RD_SIMM16, 0))),
		inst(sig(Mnemonic::STR, OperandType::IndexedS16, OperandType::IntegralRegister),
			op(Opcode::STRHX, param(OpcodeParameterType::RS, 1), param(OpcodeParameterType::RD_RT, 0))),
		inst(sig(Mnemonic::STR, OperandType::MemoryU32, OperandType::IntegralRegister),
			op(Opcode::STR, param(OpcodeParameterType::RS, 1), param(OpcodeParameterType::RD_SIMM16, 0))),
		inst(sig(Mnemonic::STR, OperandType::IndexedU32, OperandType::IntegralRegister),
			op(Opcode::STRX, param(OpcodeParameterType::RS, 1), param(OpcodeParameterType::RD_RT, 0))),
		inst(sig(Mnemonic::STR, OperandType::MemoryS32, OperandType::IntegralRegister),
			op(Opcode::STR, param(OpcodeParameterType::RS, 1), param(OpcodeParameterType::RD_SIMM16, 0))),
		inst(sig(Mnemonic::STR, OperandType::IndexedS32, OperandType::IntegralRegister),
			op(Opcode::STRX, param(OpcodeParameterType::RS, 1), param(OpcodeParameterType::RD_RT, 0))),
		inst(sig(Mnemonic::STR, OperandType::MemoryF32, OperandType::FloatingPointRegister),
			op(Opcode::FSTR, param(OpcodeParameterType::FS, 1), param(OpcodeParameterType::RD_SIMM16, 0))),
		inst(sig(Mnemonic::STR, OperandType::IndexedF32, OperandType::FloatingPointRegister),
			op(Opcode::FSTRX, param(OpcodeParameterType::FS, 1), param(OpcodeParameterType::RD_RT, 0))),
		inst(Opcode::LDRX, Mnemonic::LDR, OpcodeParameterType::RD, OpcodeParameterType::RS_RT),
		inst(Opcode::LDRBX, Mnemonic::LDRB, OpcodeParameterType::RD, OpcodeParameterType::RS_RT),
		inst(Opcode::LDRHX, Mnemonic::LDRH, OpcodeParameterType::RD, OpcodeParameterType::RS_RT),
		inst(Opcode::LDRSBX, Mnemonic::LDRSB, OpcodeParameterType::RD, OpcodeParameterType::RS_RT),
		inst(Opcode::LDRSHX, Mnemonic::LDRSH, OpcodeParameterType::RD, OpcodeParameterType::RS_RT),
		inst(Opcode::FLDRX, Mnemonic::LDR, OpcodeParameterType::FD, OpcodeParameterType::RS_RT),
		inst(Opcode::STRX, Mnemonic::STR, OpcodeParameterType::RD_RT, OpcodeParameterType::RS),
		inst(Opcode::STRBX, Mnemonic::STRB, OpcodeParameterType::RD_RT, OpcodeParameterType::RS),
		inst(Opcode::STRHX, Mnemonic::STRH, OpcodeParameterType::RD_RT, OpcodeParameterType::RS),
		inst(Opcode::FSTRX, Mnemonic::STR, OpcodeParameterType::RD_RT, OpcodeParameterType::FS),
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
		inst(sig(Mnemonic::STVP, OperandType::VariableU8, OperandType::IntegralRegister),
			op(Opcode::STRBP, param(OpcodeParameterType::RS, 1), param(OpcodeParameterType::REL_SIMM16, 0))),
		inst(sig(Mnemonic::STVP, OperandType::VariableS8, OperandType::IntegralRegister),
			op(Opcode::STRBP, param(OpcodeParameterType::RS, 1), param(OpcodeParameterType::REL_SIMM16, 0))),
		inst(sig(Mnemonic::STVP, OperandType::VariableU16, OperandType::IntegralRegister),
			op(Opcode::STRHP, param(OpcodeParameterType::RS, 1), param(OpcodeParameterType::REL_SIMM16, 0))),
		inst(sig(Mnemonic::STVP, OperandType::VariableS16, OperandType::IntegralRegister),
			op(Opcode::STRHP, param(OpcodeParameterType::RS, 1), param(OpcodeParameterType::REL_SIMM16, 0))),
		inst(sig(Mnemonic::STVP, OperandType::VariableU32, OperandType::IntegralRegister),
			op(Opcode::STRP, param(OpcodeParameterType::RS, 1), param(OpcodeParameterType::REL_SIMM16, 0))),
		inst(sig(Mnemonic::STVP, OperandType::VariableS32, OperandType::IntegralRegister),
			op(Opcode::STRP, param(OpcodeParameterType::RS, 1), param(OpcodeParameterType::REL_SIMM16, 0))),
		inst(sig(Mnemonic::STVP, OperandType::VariableF32, OperandType::FloatingPointRegister),
			op(Opcode::FSTRP, param(OpcodeParameterType::FS, 1), param(OpcodeParameterType::REL_SIMM16, 0))),
		// One word where the other two overloads of LA are two, which is why the size a statement
		// reserves has to come from its own signature rather than from the mnemonic.
		inst(Opcode::LEA, Mnemonic::LA, OpcodeParameterType::RD, OpcodeParameterType::RS_SIMM16),

		// `mov` is the one instruction that moves anything anywhere. Every form below already
		// exists under its own mnemonic; what makes them one instruction is that no two share a
		// signature - and that the destination comes first, which is what tells a load from a
		// store when both are a register and a memory operand.
		inst(Opcode::LI, Mnemonic::MOV, OpcodeParameterType::RD, OpcodeParameterType::IMM16),
		inst(sig(Mnemonic::MOV, OperandType::IntegralRegister, OperandType::VariableU8), {
			op(Opcode::LUI, param(OpcodeParameterType::RD, 0), param(OpcodeParameterType::IMM16, 1, 16)),
			op(Opcode::ORI, param(OpcodeParameterType::RD, 0), param(OpcodeParameterType::RS, 0), param(OpcodeParameterType::IMM16_LOW, 1))
		}),
		inst(sig(Mnemonic::MOV, OperandType::IntegralRegister, OperandType::VariableS8), {
			op(Opcode::LUI, param(OpcodeParameterType::RD, 0), param(OpcodeParameterType::IMM16, 1, 16)),
			op(Opcode::ORI, param(OpcodeParameterType::RD, 0), param(OpcodeParameterType::RS, 0), param(OpcodeParameterType::IMM16_LOW, 1))
		}),
		inst(sig(Mnemonic::MOV, OperandType::IntegralRegister, OperandType::VariableU16), {
			op(Opcode::LUI, param(OpcodeParameterType::RD, 0), param(OpcodeParameterType::IMM16, 1, 16)),
			op(Opcode::ORI, param(OpcodeParameterType::RD, 0), param(OpcodeParameterType::RS, 0), param(OpcodeParameterType::IMM16_LOW, 1))
		}),
		inst(sig(Mnemonic::MOV, OperandType::IntegralRegister, OperandType::VariableS16), {
			op(Opcode::LUI, param(OpcodeParameterType::RD, 0), param(OpcodeParameterType::IMM16, 1, 16)),
			op(Opcode::ORI, param(OpcodeParameterType::RD, 0), param(OpcodeParameterType::RS, 0), param(OpcodeParameterType::IMM16_LOW, 1))
		}),
		inst(sig(Mnemonic::MOV, OperandType::IntegralRegister, OperandType::VariableU32), {
			op(Opcode::LUI, param(OpcodeParameterType::RD, 0), param(OpcodeParameterType::IMM16, 1, 16)),
			op(Opcode::ORI, param(OpcodeParameterType::RD, 0), param(OpcodeParameterType::RS, 0), param(OpcodeParameterType::IMM16_LOW, 1))
		}),
		inst(sig(Mnemonic::MOV, OperandType::IntegralRegister, OperandType::VariableS32), {
			op(Opcode::LUI, param(OpcodeParameterType::RD, 0), param(OpcodeParameterType::IMM16, 1, 16)),
			op(Opcode::ORI, param(OpcodeParameterType::RD, 0), param(OpcodeParameterType::RS, 0), param(OpcodeParameterType::IMM16_LOW, 1))
		}),
		inst(sig(Mnemonic::MOV, OperandType::IntegralRegister, OperandType::VariableF32), {
			op(Opcode::LUI, param(OpcodeParameterType::RD, 0), param(OpcodeParameterType::IMM16, 1, 16)),
			op(Opcode::ORI, param(OpcodeParameterType::RD, 0), param(OpcodeParameterType::RS, 0), param(OpcodeParameterType::IMM16_LOW, 1))
		}),
		inst(sig(Mnemonic::MOV, OperandType::IntegralRegister, OperandType::Label), {
			op(Opcode::LUI, param(OpcodeParameterType::RD, 0), param(OpcodeParameterType::IMM16, 1, 16)),
			op(Opcode::ORI, param(OpcodeParameterType::RD, 0), param(OpcodeParameterType::RS, 0), param(OpcodeParameterType::IMM16_LOW, 1))
		}),
		inst(Opcode::LDR, Mnemonic::MOV, OpcodeParameterType::RD, OpcodeParameterType::RS_SIMM16),
		inst(Opcode::LDRX, Mnemonic::MOV, OpcodeParameterType::RD, OpcodeParameterType::RS_RT),
		inst(Opcode::FLDR, Mnemonic::MOV, OpcodeParameterType::FD, OpcodeParameterType::RS_SIMM16),
		inst(Opcode::FLDRX, Mnemonic::MOV, OpcodeParameterType::FD, OpcodeParameterType::RS_RT),
		inst(Opcode::STR, Mnemonic::MOV, OpcodeParameterType::RD_SIMM16, OpcodeParameterType::RS),
		inst(Opcode::STRX, Mnemonic::MOV, OpcodeParameterType::RD_RT, OpcodeParameterType::RS),
		inst(Opcode::FSTR, Mnemonic::MOV, OpcodeParameterType::RD_SIMM16, OpcodeParameterType::FS),
		inst(Opcode::FSTRX, Mnemonic::MOV, OpcodeParameterType::RD_RT, OpcodeParameterType::FS),
		inst(sig(Mnemonic::MOV, OperandType::IntegralRegister, OperandType::MemoryU8),
			op(Opcode::LDRB, param(OpcodeParameterType::RD, 0), param(OpcodeParameterType::RS_SIMM16, 1))),
		inst(sig(Mnemonic::MOV, OperandType::IntegralRegister, OperandType::IndexedU8),
			op(Opcode::LDRBX, param(OpcodeParameterType::RD, 0), param(OpcodeParameterType::RS_RT, 1))),
		inst(sig(Mnemonic::MOV, OperandType::IntegralRegister, OperandType::MemoryS8),
			op(Opcode::LDRSB, param(OpcodeParameterType::RD, 0), param(OpcodeParameterType::RS_SIMM16, 1))),
		inst(sig(Mnemonic::MOV, OperandType::IntegralRegister, OperandType::IndexedS8),
			op(Opcode::LDRSBX, param(OpcodeParameterType::RD, 0), param(OpcodeParameterType::RS_RT, 1))),
		inst(sig(Mnemonic::MOV, OperandType::IntegralRegister, OperandType::MemoryU16),
			op(Opcode::LDRH, param(OpcodeParameterType::RD, 0), param(OpcodeParameterType::RS_SIMM16, 1))),
		inst(sig(Mnemonic::MOV, OperandType::IntegralRegister, OperandType::IndexedU16),
			op(Opcode::LDRHX, param(OpcodeParameterType::RD, 0), param(OpcodeParameterType::RS_RT, 1))),
		inst(sig(Mnemonic::MOV, OperandType::IntegralRegister, OperandType::MemoryS16),
			op(Opcode::LDRSH, param(OpcodeParameterType::RD, 0), param(OpcodeParameterType::RS_SIMM16, 1))),
		inst(sig(Mnemonic::MOV, OperandType::IntegralRegister, OperandType::IndexedS16),
			op(Opcode::LDRSHX, param(OpcodeParameterType::RD, 0), param(OpcodeParameterType::RS_RT, 1))),
		inst(sig(Mnemonic::MOV, OperandType::IntegralRegister, OperandType::MemoryU32),
			op(Opcode::LDR, param(OpcodeParameterType::RD, 0), param(OpcodeParameterType::RS_SIMM16, 1))),
		inst(sig(Mnemonic::MOV, OperandType::IntegralRegister, OperandType::IndexedU32),
			op(Opcode::LDRX, param(OpcodeParameterType::RD, 0), param(OpcodeParameterType::RS_RT, 1))),
		inst(sig(Mnemonic::MOV, OperandType::IntegralRegister, OperandType::MemoryS32),
			op(Opcode::LDR, param(OpcodeParameterType::RD, 0), param(OpcodeParameterType::RS_SIMM16, 1))),
		inst(sig(Mnemonic::MOV, OperandType::IntegralRegister, OperandType::IndexedS32),
			op(Opcode::LDRX, param(OpcodeParameterType::RD, 0), param(OpcodeParameterType::RS_RT, 1))),
		inst(sig(Mnemonic::MOV, OperandType::FloatingPointRegister, OperandType::MemoryF32),
			op(Opcode::FLDR, param(OpcodeParameterType::FD, 0), param(OpcodeParameterType::RS_SIMM16, 1))),
		inst(sig(Mnemonic::MOV, OperandType::FloatingPointRegister, OperandType::IndexedF32),
			op(Opcode::FLDRX, param(OpcodeParameterType::FD, 0), param(OpcodeParameterType::RS_RT, 1))),
		inst(sig(Mnemonic::MOV, OperandType::MemoryU8, OperandType::IntegralRegister),
			op(Opcode::STRB, param(OpcodeParameterType::RS, 1), param(OpcodeParameterType::RD_SIMM16, 0))),
		inst(sig(Mnemonic::MOV, OperandType::IndexedU8, OperandType::IntegralRegister),
			op(Opcode::STRBX, param(OpcodeParameterType::RS, 1), param(OpcodeParameterType::RD_RT, 0))),
		inst(sig(Mnemonic::MOV, OperandType::MemoryS8, OperandType::IntegralRegister),
			op(Opcode::STRB, param(OpcodeParameterType::RS, 1), param(OpcodeParameterType::RD_SIMM16, 0))),
		inst(sig(Mnemonic::MOV, OperandType::IndexedS8, OperandType::IntegralRegister),
			op(Opcode::STRBX, param(OpcodeParameterType::RS, 1), param(OpcodeParameterType::RD_RT, 0))),
		inst(sig(Mnemonic::MOV, OperandType::MemoryU16, OperandType::IntegralRegister),
			op(Opcode::STRH, param(OpcodeParameterType::RS, 1), param(OpcodeParameterType::RD_SIMM16, 0))),
		inst(sig(Mnemonic::MOV, OperandType::IndexedU16, OperandType::IntegralRegister),
			op(Opcode::STRHX, param(OpcodeParameterType::RS, 1), param(OpcodeParameterType::RD_RT, 0))),
		inst(sig(Mnemonic::MOV, OperandType::MemoryS16, OperandType::IntegralRegister),
			op(Opcode::STRH, param(OpcodeParameterType::RS, 1), param(OpcodeParameterType::RD_SIMM16, 0))),
		inst(sig(Mnemonic::MOV, OperandType::IndexedS16, OperandType::IntegralRegister),
			op(Opcode::STRHX, param(OpcodeParameterType::RS, 1), param(OpcodeParameterType::RD_RT, 0))),
		inst(sig(Mnemonic::MOV, OperandType::MemoryU32, OperandType::IntegralRegister),
			op(Opcode::STR, param(OpcodeParameterType::RS, 1), param(OpcodeParameterType::RD_SIMM16, 0))),
		inst(sig(Mnemonic::MOV, OperandType::IndexedU32, OperandType::IntegralRegister),
			op(Opcode::STRX, param(OpcodeParameterType::RS, 1), param(OpcodeParameterType::RD_RT, 0))),
		inst(sig(Mnemonic::MOV, OperandType::MemoryS32, OperandType::IntegralRegister),
			op(Opcode::STR, param(OpcodeParameterType::RS, 1), param(OpcodeParameterType::RD_SIMM16, 0))),
		inst(sig(Mnemonic::MOV, OperandType::IndexedS32, OperandType::IntegralRegister),
			op(Opcode::STRX, param(OpcodeParameterType::RS, 1), param(OpcodeParameterType::RD_RT, 0))),
		inst(sig(Mnemonic::MOV, OperandType::MemoryF32, OperandType::FloatingPointRegister),
			op(Opcode::FSTR, param(OpcodeParameterType::FS, 1), param(OpcodeParameterType::RD_SIMM16, 0))),
		inst(sig(Mnemonic::MOV, OperandType::IndexedF32, OperandType::FloatingPointRegister),
			op(Opcode::FSTRX, param(OpcodeParameterType::FS, 1), param(OpcodeParameterType::RD_RT, 0))),

		// The short forms of the same, which is what relaxation rewrites a reachable one into.
		inst(sig(Mnemonic::LDVP, OperandType::IntegralRegister, OperandType::AtVariableU8),
			op(Opcode::LDRBP, param(OpcodeParameterType::RD, 0), param(OpcodeParameterType::REL_SIMM16, 1))),
		inst(sig(Mnemonic::LDVP, OperandType::IntegralRegister, OperandType::AtVariableS8),
			op(Opcode::LDRSBP, param(OpcodeParameterType::RD, 0), param(OpcodeParameterType::REL_SIMM16, 1))),
		inst(sig(Mnemonic::LDVP, OperandType::IntegralRegister, OperandType::AtVariableU16),
			op(Opcode::LDRHP, param(OpcodeParameterType::RD, 0), param(OpcodeParameterType::REL_SIMM16, 1))),
		inst(sig(Mnemonic::LDVP, OperandType::IntegralRegister, OperandType::AtVariableS16),
			op(Opcode::LDRSHP, param(OpcodeParameterType::RD, 0), param(OpcodeParameterType::REL_SIMM16, 1))),
		inst(sig(Mnemonic::LDVP, OperandType::IntegralRegister, OperandType::AtVariableU32),
			op(Opcode::LDRP, param(OpcodeParameterType::RD, 0), param(OpcodeParameterType::REL_SIMM16, 1))),
		inst(sig(Mnemonic::LDVP, OperandType::IntegralRegister, OperandType::AtVariableS32),
			op(Opcode::LDRP, param(OpcodeParameterType::RD, 0), param(OpcodeParameterType::REL_SIMM16, 1))),
		inst(sig(Mnemonic::LDVP, OperandType::FloatingPointRegister, OperandType::AtVariableF32),
			op(Opcode::FLDRP, param(OpcodeParameterType::FD, 0), param(OpcodeParameterType::REL_SIMM16, 1))),
		inst(sig(Mnemonic::STVP, OperandType::AtVariableU8, OperandType::IntegralRegister),
			op(Opcode::STRBP, param(OpcodeParameterType::RS, 1), param(OpcodeParameterType::REL_SIMM16, 0))),
		inst(sig(Mnemonic::STVP, OperandType::AtVariableS8, OperandType::IntegralRegister),
			op(Opcode::STRBP, param(OpcodeParameterType::RS, 1), param(OpcodeParameterType::REL_SIMM16, 0))),
		inst(sig(Mnemonic::STVP, OperandType::AtVariableU16, OperandType::IntegralRegister),
			op(Opcode::STRHP, param(OpcodeParameterType::RS, 1), param(OpcodeParameterType::REL_SIMM16, 0))),
		inst(sig(Mnemonic::STVP, OperandType::AtVariableS16, OperandType::IntegralRegister),
			op(Opcode::STRHP, param(OpcodeParameterType::RS, 1), param(OpcodeParameterType::REL_SIMM16, 0))),
		inst(sig(Mnemonic::STVP, OperandType::AtVariableU32, OperandType::IntegralRegister),
			op(Opcode::STRP, param(OpcodeParameterType::RS, 1), param(OpcodeParameterType::REL_SIMM16, 0))),
		inst(sig(Mnemonic::STVP, OperandType::AtVariableS32, OperandType::IntegralRegister),
			op(Opcode::STRP, param(OpcodeParameterType::RS, 1), param(OpcodeParameterType::REL_SIMM16, 0))),
		inst(sig(Mnemonic::STVP, OperandType::AtVariableF32, OperandType::FloatingPointRegister),
			op(Opcode::FSTRP, param(OpcodeParameterType::FS, 1), param(OpcodeParameterType::REL_SIMM16, 0))),

		// `[counter]` is the contents of a variable - the same expansion LDV and STV produce,
		// reached through the bracket that means exactly that everywhere else in the language.
		// `mov` takes both directions, which is what makes it the one instruction that moves
		// anything anywhere: (register, memory) is a load and (memory, register) a store.
		inst(sig(Mnemonic::LDR, OperandType::IntegralRegister, OperandType::AtVariableU8), {
			op(Opcode::LUI, param(OpcodeParameterType::RD, 0), param(OpcodeParameterType::IMM16, 1, 16)),
			op(Opcode::ORI, param(OpcodeParameterType::RD, 0), param(OpcodeParameterType::RS, 0), param(OpcodeParameterType::IMM16_LOW, 1)),
			op(Opcode::LDRB, param(OpcodeParameterType::RD, 0), param(OpcodeParameterType::RS, 0), paramUFixed(OpcodeParameterType::IMM16, 0))
		}),
		inst(sig(Mnemonic::MOV, OperandType::IntegralRegister, OperandType::AtVariableU8), {
			op(Opcode::LUI, param(OpcodeParameterType::RD, 0), param(OpcodeParameterType::IMM16, 1, 16)),
			op(Opcode::ORI, param(OpcodeParameterType::RD, 0), param(OpcodeParameterType::RS, 0), param(OpcodeParameterType::IMM16_LOW, 1)),
			op(Opcode::LDRB, param(OpcodeParameterType::RD, 0), param(OpcodeParameterType::RS, 0), paramUFixed(OpcodeParameterType::IMM16, 0))
		}),
		inst(sig(Mnemonic::LDR, OperandType::IntegralRegister, OperandType::AtVariableS8), {
			op(Opcode::LUI, param(OpcodeParameterType::RD, 0), param(OpcodeParameterType::IMM16, 1, 16)),
			op(Opcode::ORI, param(OpcodeParameterType::RD, 0), param(OpcodeParameterType::RS, 0), param(OpcodeParameterType::IMM16_LOW, 1)),
			op(Opcode::LDRSB, param(OpcodeParameterType::RD, 0), param(OpcodeParameterType::RS, 0), paramUFixed(OpcodeParameterType::IMM16, 0))
		}),
		inst(sig(Mnemonic::MOV, OperandType::IntegralRegister, OperandType::AtVariableS8), {
			op(Opcode::LUI, param(OpcodeParameterType::RD, 0), param(OpcodeParameterType::IMM16, 1, 16)),
			op(Opcode::ORI, param(OpcodeParameterType::RD, 0), param(OpcodeParameterType::RS, 0), param(OpcodeParameterType::IMM16_LOW, 1)),
			op(Opcode::LDRSB, param(OpcodeParameterType::RD, 0), param(OpcodeParameterType::RS, 0), paramUFixed(OpcodeParameterType::IMM16, 0))
		}),
		inst(sig(Mnemonic::LDR, OperandType::IntegralRegister, OperandType::AtVariableU16), {
			op(Opcode::LUI, param(OpcodeParameterType::RD, 0), param(OpcodeParameterType::IMM16, 1, 16)),
			op(Opcode::ORI, param(OpcodeParameterType::RD, 0), param(OpcodeParameterType::RS, 0), param(OpcodeParameterType::IMM16_LOW, 1)),
			op(Opcode::LDRH, param(OpcodeParameterType::RD, 0), param(OpcodeParameterType::RS, 0), paramUFixed(OpcodeParameterType::IMM16, 0))
		}),
		inst(sig(Mnemonic::MOV, OperandType::IntegralRegister, OperandType::AtVariableU16), {
			op(Opcode::LUI, param(OpcodeParameterType::RD, 0), param(OpcodeParameterType::IMM16, 1, 16)),
			op(Opcode::ORI, param(OpcodeParameterType::RD, 0), param(OpcodeParameterType::RS, 0), param(OpcodeParameterType::IMM16_LOW, 1)),
			op(Opcode::LDRH, param(OpcodeParameterType::RD, 0), param(OpcodeParameterType::RS, 0), paramUFixed(OpcodeParameterType::IMM16, 0))
		}),
		inst(sig(Mnemonic::LDR, OperandType::IntegralRegister, OperandType::AtVariableS16), {
			op(Opcode::LUI, param(OpcodeParameterType::RD, 0), param(OpcodeParameterType::IMM16, 1, 16)),
			op(Opcode::ORI, param(OpcodeParameterType::RD, 0), param(OpcodeParameterType::RS, 0), param(OpcodeParameterType::IMM16_LOW, 1)),
			op(Opcode::LDRSH, param(OpcodeParameterType::RD, 0), param(OpcodeParameterType::RS, 0), paramUFixed(OpcodeParameterType::IMM16, 0))
		}),
		inst(sig(Mnemonic::MOV, OperandType::IntegralRegister, OperandType::AtVariableS16), {
			op(Opcode::LUI, param(OpcodeParameterType::RD, 0), param(OpcodeParameterType::IMM16, 1, 16)),
			op(Opcode::ORI, param(OpcodeParameterType::RD, 0), param(OpcodeParameterType::RS, 0), param(OpcodeParameterType::IMM16_LOW, 1)),
			op(Opcode::LDRSH, param(OpcodeParameterType::RD, 0), param(OpcodeParameterType::RS, 0), paramUFixed(OpcodeParameterType::IMM16, 0))
		}),
		inst(sig(Mnemonic::LDR, OperandType::IntegralRegister, OperandType::AtVariableU32), {
			op(Opcode::LUI, param(OpcodeParameterType::RD, 0), param(OpcodeParameterType::IMM16, 1, 16)),
			op(Opcode::ORI, param(OpcodeParameterType::RD, 0), param(OpcodeParameterType::RS, 0), param(OpcodeParameterType::IMM16_LOW, 1)),
			op(Opcode::LDR, param(OpcodeParameterType::RD, 0), param(OpcodeParameterType::RS, 0), paramUFixed(OpcodeParameterType::IMM16, 0))
		}),
		inst(sig(Mnemonic::MOV, OperandType::IntegralRegister, OperandType::AtVariableU32), {
			op(Opcode::LUI, param(OpcodeParameterType::RD, 0), param(OpcodeParameterType::IMM16, 1, 16)),
			op(Opcode::ORI, param(OpcodeParameterType::RD, 0), param(OpcodeParameterType::RS, 0), param(OpcodeParameterType::IMM16_LOW, 1)),
			op(Opcode::LDR, param(OpcodeParameterType::RD, 0), param(OpcodeParameterType::RS, 0), paramUFixed(OpcodeParameterType::IMM16, 0))
		}),
		inst(sig(Mnemonic::LDR, OperandType::IntegralRegister, OperandType::AtVariableS32), {
			op(Opcode::LUI, param(OpcodeParameterType::RD, 0), param(OpcodeParameterType::IMM16, 1, 16)),
			op(Opcode::ORI, param(OpcodeParameterType::RD, 0), param(OpcodeParameterType::RS, 0), param(OpcodeParameterType::IMM16_LOW, 1)),
			op(Opcode::LDR, param(OpcodeParameterType::RD, 0), param(OpcodeParameterType::RS, 0), paramUFixed(OpcodeParameterType::IMM16, 0))
		}),
		inst(sig(Mnemonic::MOV, OperandType::IntegralRegister, OperandType::AtVariableS32), {
			op(Opcode::LUI, param(OpcodeParameterType::RD, 0), param(OpcodeParameterType::IMM16, 1, 16)),
			op(Opcode::ORI, param(OpcodeParameterType::RD, 0), param(OpcodeParameterType::RS, 0), param(OpcodeParameterType::IMM16_LOW, 1)),
			op(Opcode::LDR, param(OpcodeParameterType::RD, 0), param(OpcodeParameterType::RS, 0), paramUFixed(OpcodeParameterType::IMM16, 0))
		}),
		inst(sig(Mnemonic::LDR, OperandType::FloatingPointRegister, OperandType::AtVariableF32), {
			op(Opcode::LUI, paramUFixed(OpcodeParameterType::RD, ScratchRegister), param(OpcodeParameterType::IMM16, 1, 16)),
			op(Opcode::ORI, paramUFixed(OpcodeParameterType::RD, ScratchRegister), paramUFixed(OpcodeParameterType::RS, ScratchRegister), param(OpcodeParameterType::IMM16_LOW, 1)),
			op(Opcode::FLDR, param(OpcodeParameterType::FD, 0), paramUFixed(OpcodeParameterType::RS, ScratchRegister), paramUFixed(OpcodeParameterType::IMM16, 0))
		}),
		inst(sig(Mnemonic::MOV, OperandType::FloatingPointRegister, OperandType::AtVariableF32), {
			op(Opcode::LUI, paramUFixed(OpcodeParameterType::RD, ScratchRegister), param(OpcodeParameterType::IMM16, 1, 16)),
			op(Opcode::ORI, paramUFixed(OpcodeParameterType::RD, ScratchRegister), paramUFixed(OpcodeParameterType::RS, ScratchRegister), param(OpcodeParameterType::IMM16_LOW, 1)),
			op(Opcode::FLDR, param(OpcodeParameterType::FD, 0), paramUFixed(OpcodeParameterType::RS, ScratchRegister), paramUFixed(OpcodeParameterType::IMM16, 0))
		}),
		inst(sig(Mnemonic::STR, OperandType::AtVariableU8, OperandType::IntegralRegister), {
			op(Opcode::LUI, paramUFixed(OpcodeParameterType::RD, ScratchRegister), param(OpcodeParameterType::IMM16, 0, 16)),
			op(Opcode::ORI, paramUFixed(OpcodeParameterType::RD, ScratchRegister), paramUFixed(OpcodeParameterType::RS, ScratchRegister), param(OpcodeParameterType::IMM16_LOW, 0)),
			op(Opcode::STRB, paramUFixed(OpcodeParameterType::RD, ScratchRegister), param(OpcodeParameterType::RS, 1), paramUFixed(OpcodeParameterType::IMM16, 0))
		}),
		inst(sig(Mnemonic::MOV, OperandType::AtVariableU8, OperandType::IntegralRegister), {
			op(Opcode::LUI, paramUFixed(OpcodeParameterType::RD, ScratchRegister), param(OpcodeParameterType::IMM16, 0, 16)),
			op(Opcode::ORI, paramUFixed(OpcodeParameterType::RD, ScratchRegister), paramUFixed(OpcodeParameterType::RS, ScratchRegister), param(OpcodeParameterType::IMM16_LOW, 0)),
			op(Opcode::STRB, paramUFixed(OpcodeParameterType::RD, ScratchRegister), param(OpcodeParameterType::RS, 1), paramUFixed(OpcodeParameterType::IMM16, 0))
		}),
		inst(sig(Mnemonic::STR, OperandType::AtVariableS8, OperandType::IntegralRegister), {
			op(Opcode::LUI, paramUFixed(OpcodeParameterType::RD, ScratchRegister), param(OpcodeParameterType::IMM16, 0, 16)),
			op(Opcode::ORI, paramUFixed(OpcodeParameterType::RD, ScratchRegister), paramUFixed(OpcodeParameterType::RS, ScratchRegister), param(OpcodeParameterType::IMM16_LOW, 0)),
			op(Opcode::STRB, paramUFixed(OpcodeParameterType::RD, ScratchRegister), param(OpcodeParameterType::RS, 1), paramUFixed(OpcodeParameterType::IMM16, 0))
		}),
		inst(sig(Mnemonic::MOV, OperandType::AtVariableS8, OperandType::IntegralRegister), {
			op(Opcode::LUI, paramUFixed(OpcodeParameterType::RD, ScratchRegister), param(OpcodeParameterType::IMM16, 0, 16)),
			op(Opcode::ORI, paramUFixed(OpcodeParameterType::RD, ScratchRegister), paramUFixed(OpcodeParameterType::RS, ScratchRegister), param(OpcodeParameterType::IMM16_LOW, 0)),
			op(Opcode::STRB, paramUFixed(OpcodeParameterType::RD, ScratchRegister), param(OpcodeParameterType::RS, 1), paramUFixed(OpcodeParameterType::IMM16, 0))
		}),
		inst(sig(Mnemonic::STR, OperandType::AtVariableU16, OperandType::IntegralRegister), {
			op(Opcode::LUI, paramUFixed(OpcodeParameterType::RD, ScratchRegister), param(OpcodeParameterType::IMM16, 0, 16)),
			op(Opcode::ORI, paramUFixed(OpcodeParameterType::RD, ScratchRegister), paramUFixed(OpcodeParameterType::RS, ScratchRegister), param(OpcodeParameterType::IMM16_LOW, 0)),
			op(Opcode::STRH, paramUFixed(OpcodeParameterType::RD, ScratchRegister), param(OpcodeParameterType::RS, 1), paramUFixed(OpcodeParameterType::IMM16, 0))
		}),
		inst(sig(Mnemonic::MOV, OperandType::AtVariableU16, OperandType::IntegralRegister), {
			op(Opcode::LUI, paramUFixed(OpcodeParameterType::RD, ScratchRegister), param(OpcodeParameterType::IMM16, 0, 16)),
			op(Opcode::ORI, paramUFixed(OpcodeParameterType::RD, ScratchRegister), paramUFixed(OpcodeParameterType::RS, ScratchRegister), param(OpcodeParameterType::IMM16_LOW, 0)),
			op(Opcode::STRH, paramUFixed(OpcodeParameterType::RD, ScratchRegister), param(OpcodeParameterType::RS, 1), paramUFixed(OpcodeParameterType::IMM16, 0))
		}),
		inst(sig(Mnemonic::STR, OperandType::AtVariableS16, OperandType::IntegralRegister), {
			op(Opcode::LUI, paramUFixed(OpcodeParameterType::RD, ScratchRegister), param(OpcodeParameterType::IMM16, 0, 16)),
			op(Opcode::ORI, paramUFixed(OpcodeParameterType::RD, ScratchRegister), paramUFixed(OpcodeParameterType::RS, ScratchRegister), param(OpcodeParameterType::IMM16_LOW, 0)),
			op(Opcode::STRH, paramUFixed(OpcodeParameterType::RD, ScratchRegister), param(OpcodeParameterType::RS, 1), paramUFixed(OpcodeParameterType::IMM16, 0))
		}),
		inst(sig(Mnemonic::MOV, OperandType::AtVariableS16, OperandType::IntegralRegister), {
			op(Opcode::LUI, paramUFixed(OpcodeParameterType::RD, ScratchRegister), param(OpcodeParameterType::IMM16, 0, 16)),
			op(Opcode::ORI, paramUFixed(OpcodeParameterType::RD, ScratchRegister), paramUFixed(OpcodeParameterType::RS, ScratchRegister), param(OpcodeParameterType::IMM16_LOW, 0)),
			op(Opcode::STRH, paramUFixed(OpcodeParameterType::RD, ScratchRegister), param(OpcodeParameterType::RS, 1), paramUFixed(OpcodeParameterType::IMM16, 0))
		}),
		inst(sig(Mnemonic::STR, OperandType::AtVariableU32, OperandType::IntegralRegister), {
			op(Opcode::LUI, paramUFixed(OpcodeParameterType::RD, ScratchRegister), param(OpcodeParameterType::IMM16, 0, 16)),
			op(Opcode::ORI, paramUFixed(OpcodeParameterType::RD, ScratchRegister), paramUFixed(OpcodeParameterType::RS, ScratchRegister), param(OpcodeParameterType::IMM16_LOW, 0)),
			op(Opcode::STR, paramUFixed(OpcodeParameterType::RD, ScratchRegister), param(OpcodeParameterType::RS, 1), paramUFixed(OpcodeParameterType::IMM16, 0))
		}),
		inst(sig(Mnemonic::MOV, OperandType::AtVariableU32, OperandType::IntegralRegister), {
			op(Opcode::LUI, paramUFixed(OpcodeParameterType::RD, ScratchRegister), param(OpcodeParameterType::IMM16, 0, 16)),
			op(Opcode::ORI, paramUFixed(OpcodeParameterType::RD, ScratchRegister), paramUFixed(OpcodeParameterType::RS, ScratchRegister), param(OpcodeParameterType::IMM16_LOW, 0)),
			op(Opcode::STR, paramUFixed(OpcodeParameterType::RD, ScratchRegister), param(OpcodeParameterType::RS, 1), paramUFixed(OpcodeParameterType::IMM16, 0))
		}),
		inst(sig(Mnemonic::STR, OperandType::AtVariableS32, OperandType::IntegralRegister), {
			op(Opcode::LUI, paramUFixed(OpcodeParameterType::RD, ScratchRegister), param(OpcodeParameterType::IMM16, 0, 16)),
			op(Opcode::ORI, paramUFixed(OpcodeParameterType::RD, ScratchRegister), paramUFixed(OpcodeParameterType::RS, ScratchRegister), param(OpcodeParameterType::IMM16_LOW, 0)),
			op(Opcode::STR, paramUFixed(OpcodeParameterType::RD, ScratchRegister), param(OpcodeParameterType::RS, 1), paramUFixed(OpcodeParameterType::IMM16, 0))
		}),
		inst(sig(Mnemonic::MOV, OperandType::AtVariableS32, OperandType::IntegralRegister), {
			op(Opcode::LUI, paramUFixed(OpcodeParameterType::RD, ScratchRegister), param(OpcodeParameterType::IMM16, 0, 16)),
			op(Opcode::ORI, paramUFixed(OpcodeParameterType::RD, ScratchRegister), paramUFixed(OpcodeParameterType::RS, ScratchRegister), param(OpcodeParameterType::IMM16_LOW, 0)),
			op(Opcode::STR, paramUFixed(OpcodeParameterType::RD, ScratchRegister), param(OpcodeParameterType::RS, 1), paramUFixed(OpcodeParameterType::IMM16, 0))
		}),
		inst(sig(Mnemonic::STR, OperandType::AtVariableF32, OperandType::FloatingPointRegister), {
			op(Opcode::LUI, paramUFixed(OpcodeParameterType::RD, ScratchRegister), param(OpcodeParameterType::IMM16, 0, 16)),
			op(Opcode::ORI, paramUFixed(OpcodeParameterType::RD, ScratchRegister), paramUFixed(OpcodeParameterType::RS, ScratchRegister), param(OpcodeParameterType::IMM16_LOW, 0)),
			op(Opcode::FSTR, paramUFixed(OpcodeParameterType::RD, ScratchRegister), param(OpcodeParameterType::FS, 1), paramUFixed(OpcodeParameterType::IMM16, 0))
		}),
		inst(sig(Mnemonic::MOV, OperandType::AtVariableF32, OperandType::FloatingPointRegister), {
			op(Opcode::LUI, paramUFixed(OpcodeParameterType::RD, ScratchRegister), param(OpcodeParameterType::IMM16, 0, 16)),
			op(Opcode::ORI, paramUFixed(OpcodeParameterType::RD, ScratchRegister), paramUFixed(OpcodeParameterType::RS, ScratchRegister), param(OpcodeParameterType::IMM16_LOW, 0)),
			op(Opcode::FSTR, paramUFixed(OpcodeParameterType::RD, ScratchRegister), param(OpcodeParameterType::FS, 1), paramUFixed(OpcodeParameterType::IMM16, 0))
		}),

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
		// The link register is an operand, so `bl` competes with nothing: pick whichever register
		// the function can spare, and return with the `jp` that already exists.
		inst(Opcode::BL, Mnemonic::BL, OpcodeParameterType::RD, OpcodeParameterType::REL_ADDR20),
		inst(Opcode::BLR, Mnemonic::BL, OpcodeParameterType::RD, OpcodeParameterType::RS),

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
		inst(sig(Mnemonic::LA, OperandType::IntegralRegister, OperandType::Immediate), {
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
		// The frame pointer had no instruction that touched it; these are it. `enter` on its own
		// opens a frame of nothing, for a function whose locals all live in registers.
		inst(sig(Mnemonic::ENTER), op(Opcode::ENTER, paramUFixed(OpcodeParameterType::IMM16, 0))),
		inst(Opcode::ENTER, Mnemonic::ENTER, OpcodeParameterType::IMM16),
		inst(Opcode::LEAVE, Mnemonic::LEAVE),
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
		// No operands, which no other overload of PUSH/POP has, so the signature tells them apart.
		inst(Opcode::PUSHF, Mnemonic::PUSH),
		inst(Opcode::POPF, Mnemonic::POP),
		inst(Opcode::FPUSH, Mnemonic::PUSH, OpcodeParameterType::FS),
		inst(Opcode::FPOP, Mnemonic::POP, OpcodeParameterType::FD),

		inst(Opcode::ITOF, Mnemonic::ITOF, OpcodeParameterType::FD, OpcodeParameterType::RS),
		inst(Opcode::IITOF, Mnemonic::IITOF, OpcodeParameterType::FD, OpcodeParameterType::RS),
		inst(Opcode::FTOI, Mnemonic::FTOI, OpcodeParameterType::RD, OpcodeParameterType::FS),
		inst(Opcode::FTOII, Mnemonic::FTOII, OpcodeParameterType::RD, OpcodeParameterType::FS),
		inst(Opcode::MTF, Mnemonic::MTF, OpcodeParameterType::FD, OpcodeParameterType::RS),
		inst(Opcode::MFF, Mnemonic::MFF, OpcodeParameterType::RD, OpcodeParameterType::FS),

		inst(sig(Mnemonic::IN, OperandType::IntegralRegister, OperandType::Immediate),
			op(Opcode::IN, param(OpcodeParameterType::RD, 0), param(OpcodeParameterType::IMM8, 1))
		),
		inst(sig(Mnemonic::INB, OperandType::IntegralRegister, OperandType::Immediate),
			op(Opcode::INB, param(OpcodeParameterType::RD, 0), param(OpcodeParameterType::IMM8, 1))
		),
		inst(sig(Mnemonic::INH, OperandType::IntegralRegister, OperandType::Immediate),
			op(Opcode::INH, param(OpcodeParameterType::RD, 0), param(OpcodeParameterType::IMM8, 1))
		),
		inst(sig(Mnemonic::INSB, OperandType::IntegralRegister, OperandType::Immediate),
			op(Opcode::INSB, param(OpcodeParameterType::RD, 0), param(OpcodeParameterType::IMM8, 1))
		),
		inst(sig(Mnemonic::INSH, OperandType::IntegralRegister, OperandType::Immediate),
			op(Opcode::INSH, param(OpcodeParameterType::RD, 0), param(OpcodeParameterType::IMM8, 1))
		),
		inst(sig(Mnemonic::INM, OperandType::IntegralRegister, OperandType::Immediate, OperandType::IntegralRegister),
			op(Opcode::INM, param(OpcodeParameterType::RD, 0), param(OpcodeParameterType::RS, 2), param(OpcodeParameterType::IMM8, 1))
		),

		inst(sig(Mnemonic::IN, OperandType::IntegralRegister, OperandType::IntegralRegister),
			op(Opcode::INR, param(OpcodeParameterType::RD, 0), param(OpcodeParameterType::RS, 1))
		),
		inst(sig(Mnemonic::INB, OperandType::IntegralRegister, OperandType::IntegralRegister),
			op(Opcode::INRB, param(OpcodeParameterType::RD, 0), param(OpcodeParameterType::RS, 1))
		),
		inst(sig(Mnemonic::INH, OperandType::IntegralRegister, OperandType::IntegralRegister),
			op(Opcode::INRH, param(OpcodeParameterType::RD, 0), param(OpcodeParameterType::RS, 1))
		),
		inst(sig(Mnemonic::INSB, OperandType::IntegralRegister, OperandType::IntegralRegister),
			op(Opcode::INRSB, param(OpcodeParameterType::RD, 0), param(OpcodeParameterType::RS, 1))
		),
		inst(sig(Mnemonic::INSH, OperandType::IntegralRegister, OperandType::IntegralRegister),
			op(Opcode::INRSH, param(OpcodeParameterType::RD, 0), param(OpcodeParameterType::RS, 1))
		),
		inst(sig(Mnemonic::INM, OperandType::IntegralRegister, OperandType::IntegralRegister, OperandType::IntegralRegister),
			op(Opcode::INRM, param(OpcodeParameterType::RD, 0), param(OpcodeParameterType::RS, 1), param(OpcodeParameterType::RT, 2))
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

	std::optional<u32> InstructionInfo::reservedSizeOf(const InstructionSignature& signature) noexcept
	{
		if (const auto info = find(signature); info.has_value())
			return info.value().sizeInBytes();

		return findMaxSizeInBytes(signature.mnemonic);
	}

	std::optional<u32> InstructionInfo::findMaxSizeInBytes(Mnemonic mnemonic) noexcept
	{
		if (auto it = __instructionMaxSizesInBytesMap.find(mnemonic); it != __instructionMaxSizesInBytesMap.end())
			return it->second;
		return std::nullopt;
	}
}
