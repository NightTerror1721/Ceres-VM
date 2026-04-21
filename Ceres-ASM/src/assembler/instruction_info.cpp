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
		inst(Opcode::LDR, Mnemonic::LDR, OpcodeParameterType::RD, OpcodeParameterType::RS_IMM16),
		inst(Opcode::LDRB, Mnemonic::LDRB, OpcodeParameterType::RD, OpcodeParameterType::RS_IMM16),
		inst(Opcode::LDRH, Mnemonic::LDRH, OpcodeParameterType::RD, OpcodeParameterType::RS_IMM16),
		inst(Opcode::LDRSB, Mnemonic::LDRSB, OpcodeParameterType::RD, OpcodeParameterType::RS_IMM16),
		inst(Opcode::LDRSH, Mnemonic::LDRSH, OpcodeParameterType::RD, OpcodeParameterType::RS_IMM16),
		inst(Opcode::FLDR, Mnemonic::LDR, OpcodeParameterType::FD, OpcodeParameterType::RS_IMM16),
		inst(sig(Mnemonic::LDV, OperandType::IntegralRegister, OperandType::VariableU8), {
			op(Opcode::LUI, param(OpcodeParameterType::RD, 0), param(OpcodeParameterType::IMM16, 1, 16)),
			op(Opcode::ORI, param(OpcodeParameterType::RD, 0), param(OpcodeParameterType::RS, 0), param(OpcodeParameterType::IMM16, 1)),
			op(Opcode::LDRB, param(OpcodeParameterType::RD, 0), param(OpcodeParameterType::RS, 0), paramUFixed(OpcodeParameterType::IMM16, 0))
		}),
		inst(sig(Mnemonic::LDV, OperandType::IntegralRegister, OperandType::VariableS8), {
			op(Opcode::LUI, param(OpcodeParameterType::RD, 0), param(OpcodeParameterType::IMM16, 1, 16)),
			op(Opcode::ORI, param(OpcodeParameterType::RD, 0), param(OpcodeParameterType::RS, 0), param(OpcodeParameterType::IMM16, 1)),
			op(Opcode::LDRSB, param(OpcodeParameterType::RD, 0), param(OpcodeParameterType::RS, 0), paramUFixed(OpcodeParameterType::IMM16, 0))
		}),
		inst(sig(Mnemonic::LDV, OperandType::IntegralRegister, OperandType::VariableU16), {
			op(Opcode::LUI, param(OpcodeParameterType::RD, 0), param(OpcodeParameterType::IMM16, 1, 16)),
			op(Opcode::ORI, param(OpcodeParameterType::RD, 0), param(OpcodeParameterType::RS, 0), param(OpcodeParameterType::IMM16, 1)),
			op(Opcode::LDRH, param(OpcodeParameterType::RD, 0), param(OpcodeParameterType::RS, 0), paramUFixed(OpcodeParameterType::IMM16, 0))
		}),
		inst(sig(Mnemonic::LDV, OperandType::IntegralRegister, OperandType::VariableS16), {
			op(Opcode::LUI, param(OpcodeParameterType::RD, 0), param(OpcodeParameterType::IMM16, 1, 16)),
			op(Opcode::ORI, param(OpcodeParameterType::RD, 0), param(OpcodeParameterType::RS, 0), param(OpcodeParameterType::IMM16, 1)),
			op(Opcode::LDRSH, param(OpcodeParameterType::RD, 0), param(OpcodeParameterType::RS, 0), paramUFixed(OpcodeParameterType::IMM16, 0))
		}),
		inst(sig(Mnemonic::LDV, OperandType::IntegralRegister, OperandType::VariableU32), {
			op(Opcode::LUI, param(OpcodeParameterType::RD, 0), param(OpcodeParameterType::IMM16, 1, 16)),
			op(Opcode::ORI, param(OpcodeParameterType::RD, 0), param(OpcodeParameterType::RS, 0), param(OpcodeParameterType::IMM16, 1)),
			op(Opcode::LDR, param(OpcodeParameterType::RD, 0), param(OpcodeParameterType::RS, 0), paramUFixed(OpcodeParameterType::IMM16, 0))
		}),
		inst(sig(Mnemonic::LDV, OperandType::IntegralRegister, OperandType::VariableS32), {
			op(Opcode::LUI, param(OpcodeParameterType::RD, 0), param(OpcodeParameterType::IMM16, 1, 16)),
			op(Opcode::ORI, param(OpcodeParameterType::RD, 0), param(OpcodeParameterType::RS, 0), param(OpcodeParameterType::IMM16, 1)),
			op(Opcode::LDR, param(OpcodeParameterType::RD, 0), param(OpcodeParameterType::RS, 0), paramUFixed(OpcodeParameterType::IMM16, 0))
		}),
		inst(sig(Mnemonic::LDV, OperandType::IntegralRegister, OperandType::VariableF32), {
			op(Opcode::LUI, param(OpcodeParameterType::RD, 0), param(OpcodeParameterType::IMM16, 1, 16)),
			op(Opcode::ORI, param(OpcodeParameterType::RD, 0), param(OpcodeParameterType::RS, 0), param(OpcodeParameterType::IMM16, 1)),
			op(Opcode::FLDR, param(OpcodeParameterType::FD, 0), param(OpcodeParameterType::RS, 0), paramUFixed(OpcodeParameterType::IMM16, 0))
		}),
		inst(Opcode::STR, Mnemonic::STR, OpcodeParameterType::RS, OpcodeParameterType::RT_IMM16),
		inst(Opcode::STRB, Mnemonic::STRB, OpcodeParameterType::RS, OpcodeParameterType::RT_IMM16),
		inst(Opcode::STRH, Mnemonic::STRH, OpcodeParameterType::RS, OpcodeParameterType::RT_IMM16),
		inst(Opcode::FSTR, Mnemonic::STR, OpcodeParameterType::FS, OpcodeParameterType::RT_IMM16),
		inst(sig(Mnemonic::STV, OperandType::IntegralRegister, OperandType::VariableU8), {
			op(Opcode::LUI, paramUFixed(OpcodeParameterType::RD, 13), param(OpcodeParameterType::IMM16, 1, 16)),
			op(Opcode::ORI, paramUFixed(OpcodeParameterType::RD, 13), param(OpcodeParameterType::RS, 0), param(OpcodeParameterType::IMM16, 1)),
			op(Opcode::STRB, param(OpcodeParameterType::RS, 0), paramUFixed(OpcodeParameterType::RT, 13), paramUFixed(OpcodeParameterType::IMM16, 0))
		}),
		inst(sig(Mnemonic::STV, OperandType::IntegralRegister, OperandType::VariableS8), {
			op(Opcode::LUI, paramUFixed(OpcodeParameterType::RD, 13), param(OpcodeParameterType::IMM16, 1, 16)),
			op(Opcode::ORI, paramUFixed(OpcodeParameterType::RD, 13), param(OpcodeParameterType::RS, 0), param(OpcodeParameterType::IMM16, 1)),
			op(Opcode::STRB, param(OpcodeParameterType::RS, 0), paramUFixed(OpcodeParameterType::RT, 13), paramUFixed(OpcodeParameterType::IMM16, 0))
		}),
		inst(sig(Mnemonic::STV, OperandType::IntegralRegister, OperandType::VariableU16), {
			op(Opcode::LUI, paramUFixed(OpcodeParameterType::RD, 13), param(OpcodeParameterType::IMM16, 1, 16)),
			op(Opcode::ORI, paramUFixed(OpcodeParameterType::RD, 13), param(OpcodeParameterType::RS, 0), param(OpcodeParameterType::IMM16, 1)),
			op(Opcode::STRH, param(OpcodeParameterType::RS, 0), paramUFixed(OpcodeParameterType::RT, 13), paramUFixed(OpcodeParameterType::IMM16, 0))
		}),
		inst(sig(Mnemonic::STV, OperandType::IntegralRegister, OperandType::VariableS16), {
			op(Opcode::LUI, paramUFixed(OpcodeParameterType::RD, 13), param(OpcodeParameterType::IMM16, 1, 16)),
			op(Opcode::ORI, paramUFixed(OpcodeParameterType::RD, 13), param(OpcodeParameterType::RS, 0), param(OpcodeParameterType::IMM16, 1)),
			op(Opcode::STRH, param(OpcodeParameterType::RS, 0), paramUFixed(OpcodeParameterType::RT, 13), paramUFixed(OpcodeParameterType::IMM16, 0))
		}),
		inst(sig(Mnemonic::STV, OperandType::IntegralRegister, OperandType::VariableU32), {
			op(Opcode::LUI, paramUFixed(OpcodeParameterType::RD, 13), param(OpcodeParameterType::IMM16, 1, 16)),
			op(Opcode::ORI, paramUFixed(OpcodeParameterType::RD, 13), param(OpcodeParameterType::RS, 0), param(OpcodeParameterType::IMM16, 1)),
			op(Opcode::STR, param(OpcodeParameterType::RS, 0), paramUFixed(OpcodeParameterType::RT, 13), paramUFixed(OpcodeParameterType::IMM16, 0))
		}),
		inst(sig(Mnemonic::STV, OperandType::IntegralRegister, OperandType::VariableS32), {
			op(Opcode::LUI, paramUFixed(OpcodeParameterType::RD, 13), param(OpcodeParameterType::IMM16, 1, 16)),
			op(Opcode::ORI, paramUFixed(OpcodeParameterType::RD, 13), param(OpcodeParameterType::RS, 0), param(OpcodeParameterType::IMM16, 1)),
			op(Opcode::STR, param(OpcodeParameterType::RS, 0), paramUFixed(OpcodeParameterType::RT, 13), paramUFixed(OpcodeParameterType::IMM16, 0))
		}),
		inst(sig(Mnemonic::STV, OperandType::IntegralRegister, OperandType::VariableF32), {
			op(Opcode::LUI, paramUFixed(OpcodeParameterType::RD, 13), param(OpcodeParameterType::IMM16, 1, 16)),
			op(Opcode::ORI, paramUFixed(OpcodeParameterType::RD, 13), param(OpcodeParameterType::RS, 0), param(OpcodeParameterType::IMM16, 1)),
			op(Opcode::FSTR, param(OpcodeParameterType::FS, 0), paramUFixed(OpcodeParameterType::RT, 13), paramUFixed(OpcodeParameterType::IMM16, 0))
		}),
		inst(sig(Mnemonic::LA, OperandType::IntegralRegister, OperandType::VariableU8), {
			op(Opcode::LUI, param(OpcodeParameterType::RD, 0), param(OpcodeParameterType::IMM16, 1, 16)),
			op(Opcode::ORI, param(OpcodeParameterType::RD, 0), param(OpcodeParameterType::RS, 0), param(OpcodeParameterType::IMM16, 1))
		}),
		inst(sig(Mnemonic::LA, OperandType::IntegralRegister, OperandType::VariableS8), {
			op(Opcode::LUI, param(OpcodeParameterType::RD, 0), param(OpcodeParameterType::IMM16, 1, 16)),
			op(Opcode::ORI, param(OpcodeParameterType::RD, 0), param(OpcodeParameterType::RS, 0), param(OpcodeParameterType::IMM16, 1))
		}),
		inst(sig(Mnemonic::LA, OperandType::IntegralRegister, OperandType::VariableU16), {
			op(Opcode::LUI, param(OpcodeParameterType::RD, 0), param(OpcodeParameterType::IMM16, 1, 16)),
			op(Opcode::ORI, param(OpcodeParameterType::RD, 0), param(OpcodeParameterType::RS, 0), param(OpcodeParameterType::IMM16, 1))
		}),
		inst(sig(Mnemonic::LA, OperandType::IntegralRegister, OperandType::VariableS16), {
			op(Opcode::LUI, param(OpcodeParameterType::RD, 0), param(OpcodeParameterType::IMM16, 1, 16)),
			op(Opcode::ORI, param(OpcodeParameterType::RD, 0), param(OpcodeParameterType::RS, 0), param(OpcodeParameterType::IMM16, 1))
		}),
		inst(sig(Mnemonic::LA, OperandType::IntegralRegister, OperandType::VariableU32), {
			op(Opcode::LUI, param(OpcodeParameterType::RD, 0), param(OpcodeParameterType::IMM16, 1, 16)),
			op(Opcode::ORI, param(OpcodeParameterType::RD, 0), param(OpcodeParameterType::RS, 0), param(OpcodeParameterType::IMM16, 1))
		}),
		inst(sig(Mnemonic::LA, OperandType::IntegralRegister, OperandType::VariableS32), {
			op(Opcode::LUI, param(OpcodeParameterType::RD, 0), param(OpcodeParameterType::IMM16, 1, 16)),
			op(Opcode::ORI, param(OpcodeParameterType::RD, 0), param(OpcodeParameterType::RS, 0), param(OpcodeParameterType::IMM16, 1))
		}),
		inst(sig(Mnemonic::LA, OperandType::IntegralRegister, OperandType::VariableF32), {
			op(Opcode::LUI, param(OpcodeParameterType::RD, 0), param(OpcodeParameterType::IMM16, 1, 16)),
			op(Opcode::ORI, param(OpcodeParameterType::RD, 0), param(OpcodeParameterType::RS, 0), param(OpcodeParameterType::IMM16, 1))
		}),
		inst(Opcode::LEA, Mnemonic::LEA, OpcodeParameterType::RD, OpcodeParameterType::RT_IMM16),

		inst(Opcode::JP, Mnemonic::JP, OpcodeParameterType::SIMM24),
		inst(Opcode::JPR, Mnemonic::JP, OpcodeParameterType::RS),
		inst(Opcode::CMP, Mnemonic::CMP, OpcodeParameterType::RS, OpcodeParameterType::RT),
		inst(Opcode::CMPI, Mnemonic::CMP, OpcodeParameterType::RS, OpcodeParameterType::SIMM16),
		inst(Opcode::FCMP, Mnemonic::CMP, OpcodeParameterType::FS, OpcodeParameterType::FT),
		inst(Opcode::JZ, Mnemonic::JZ, OpcodeParameterType::SIMM24),
		inst(Opcode::JZR, Mnemonic::JZ, OpcodeParameterType::RS),
		inst(Opcode::JNZ, Mnemonic::JNZ, OpcodeParameterType::SIMM24),
		inst(Opcode::JNZR, Mnemonic::JNZ, OpcodeParameterType::RS),
		inst(Opcode::JC, Mnemonic::JC, OpcodeParameterType::SIMM24),
		inst(Opcode::JCR, Mnemonic::JC, OpcodeParameterType::RS),
		inst(Opcode::JNC, Mnemonic::JNC, OpcodeParameterType::SIMM24),
		inst(Opcode::JNCR, Mnemonic::JNC, OpcodeParameterType::RS),
		inst(Opcode::JS, Mnemonic::JS, OpcodeParameterType::SIMM24),
		inst(Opcode::JSR, Mnemonic::JS, OpcodeParameterType::RS),
		inst(Opcode::JNS, Mnemonic::JNS, OpcodeParameterType::SIMM24),
		inst(Opcode::JNSR, Mnemonic::JNS, OpcodeParameterType::RS),
		inst(Opcode::CALL, Mnemonic::CALL, OpcodeParameterType::SIMM24),
		inst(Opcode::CALLR, Mnemonic::CALL, OpcodeParameterType::RS),
		inst(Opcode::RET, Mnemonic::RET),

		inst(Opcode::PUSH, Mnemonic::PUSH, OpcodeParameterType::RS),
		inst(Opcode::POP, Mnemonic::POP, OpcodeParameterType::RD),
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

		inst(Opcode::IN, Mnemonic::IN, OpcodeParameterType::RD, OpcodeParameterType::IMM8),
		inst(Opcode::INR, Mnemonic::IN, OpcodeParameterType::RD, OpcodeParameterType::RS),
		inst(Opcode::OUT, Mnemonic::OUT, OpcodeParameterType::IMM8, OpcodeParameterType::RS),
		inst(Opcode::OUTR, Mnemonic::OUT, OpcodeParameterType::RS, OpcodeParameterType::RS)
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
