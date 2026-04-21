#pragma once

#include "mnemonic.h"
#include "vm/opcodes.h"
#include "vm/instructions.h"
#include <string>
#include <array>
#include <vector>
#include <optional>
#include <compare>

namespace ceres::casm
{
	enum class OperandType : u8
	{
		Invalid = 0,				// Invalid operand type
		IntegralRegister,			// Integral register (e.g., r0, r1, ..., r15)
		FloatingPointRegister,		// Floating-point register (e.g., f0, f1, ..., f15)
		Immediate,					// Immediate value (e.g., 42, 0xFF, etc.)
		RegisterPlusAddress,		// Register plus address (e.g., [r0 + 0x10], [r1 + label], etc.)
		VariableU8,					// 8-bit variable (for pseudo-instructions)
		VariableS8,					// 8-bit signed variable (for pseudo-instructions)
		VariableU16,				// 16-bit variable (for pseudo-instructions)
		VariableS16,				// 16-bit signed variable (for pseudo-instructions)
		VariableU32,				// 32-bit variable (for pseudo-instructions)
		VariableS32,				// 32-bit signed variable (for pseudo-instructions)
		VariableF32,				// Floating-point variable (for pseudo-instructions)
	};

	enum class OpcodeParameterType : u8
	{
		Invalid = 0,	// Invalid parameter type
		RD,				// Destination integral register
		RS,				// Source integral register
		RT,				// Target integral register
		FD,				// Destination floating-point register
		FS,				// Source floating-point register
		FT,				// Target floating-point register
		IMM8,			// 8-bit immediate value
		IMM16,			// 16-bit immediate value
		SIMM16,			// 16-bit signed immediate value
		IMM24,			// 24-bit immediate value
		SIMM24,			// 24-bit signed immediate value
		RD_IMM16,		// Destination register with 16-bit immediate value
		RS_IMM16,		// Source register with 16-bit immediate value
		RT_IMM16,		// Target register with 16-bit immediate value
	};


	class OpcodeParameter
	{
	public:
		static const OpcodeParameter Invalid;

	private:
		OpcodeParameterType _type = OpcodeParameterType::Invalid;
		bool _fixed = false; // Whether the parameter is a fixed value or an operand index
		u8 _valueShift = 0; // Shift amount for immediate or fixed values (e.g., for IMM8, IMM16, IMM24)
		u32 _fixedValueOrOperandIndex = 0; // If fixed, the fixed value or operand index

	public:
		constexpr OpcodeParameter() noexcept = default;
		constexpr OpcodeParameter(const OpcodeParameter&) noexcept = default;
		constexpr OpcodeParameter(OpcodeParameter&&) noexcept = default;
		constexpr ~OpcodeParameter() noexcept = default;

		constexpr OpcodeParameter& operator=(const OpcodeParameter&) noexcept = default;
		constexpr OpcodeParameter& operator=(OpcodeParameter&&) noexcept = default;

	private:
		constexpr OpcodeParameter(OpcodeParameterType type, bool fixed, u8 valueShift, u32 fixedValueOrOperandIndex) noexcept :
			_type(type), _fixed(fixed), _valueShift(valueShift), _fixedValueOrOperandIndex(fixedValueOrOperandIndex)
		{}

	public:
		constexpr OpcodeParameterType type() const noexcept { return _type; }
		constexpr bool isFixed() const noexcept { return _fixed; }
		constexpr u8 fixedValueShift() const noexcept { return _valueShift; }
		constexpr u8 operandIndex() const noexcept { return static_cast<u8>(_fixedValueOrOperandIndex); }

		constexpr u8 fixedValueU8() const noexcept
		{
			if (!_fixed)
				return 0;
			return static_cast<u8>(_fixedValueOrOperandIndex >> _valueShift);
		}
		constexpr u16 fixedValueU16() const noexcept
		{
			if (!_fixed)
				return 0;
			return static_cast<u16>(_fixedValueOrOperandIndex >> _valueShift);
		}
		constexpr u24 fixedValueU24() const noexcept
		{
			if (!_fixed)
				return u24(0);
			return static_cast<u24>(_fixedValueOrOperandIndex >> _valueShift);
		}

		constexpr i8 fixedValueS8() const noexcept
		{
			if (!_fixed)
				return 0;
			return static_cast<i8>(static_cast<i32>(_fixedValueOrOperandIndex) >> static_cast<i32>(_valueShift));
		}
		constexpr i16 fixedValueS16() const noexcept
		{
			if (!_fixed)
				return 0;
			return static_cast<i16>(static_cast<i32>(_fixedValueOrOperandIndex) >> static_cast<i32>(_valueShift));
		}
		constexpr i24 fixedValueS24() const noexcept
		{
			if (!_fixed)
				return i24(0);
			return static_cast<i24>(static_cast<i32>(_fixedValueOrOperandIndex) >> static_cast<i32>(_valueShift));
		}

		constexpr bool isValid() const noexcept { return _type != OpcodeParameterType::Invalid; }
		constexpr bool isInvalid() const noexcept { return _type == OpcodeParameterType::Invalid; }

		constexpr OperandType operandType() const noexcept { return toOperandType(_type); }

	public:
		static constexpr OpcodeParameter make(OpcodeParameterType type, u8 operandIndex, u8 valueShift = 0) noexcept
		{
			return OpcodeParameter{ type, false, valueShift, operandIndex };
		}

		static constexpr OpcodeParameter makeUFixed(OpcodeParameterType type, u32 fixedValue, u8 valueShift = 0) noexcept
		{
			return OpcodeParameter{ type, true, valueShift, fixedValue };
		}

		static constexpr OpcodeParameter makeSFixed(OpcodeParameterType type, i32 fixedValue, u8 valueShift = 0) noexcept
		{
			return OpcodeParameter{ type, true, valueShift, static_cast<u32>(fixedValue) };
		}

		static constexpr OpcodeParameter makeInvalid() noexcept
		{
			return OpcodeParameter{};
		}

		static constexpr OperandType toOperandType(OpcodeParameterType info) noexcept
		{
			switch (info)
			{
				case OpcodeParameterType::RD:
				case OpcodeParameterType::RS:
				case OpcodeParameterType::RT:
					return OperandType::IntegralRegister;

				case OpcodeParameterType::FD:
				case OpcodeParameterType::FS:
				case OpcodeParameterType::FT:
					return OperandType::FloatingPointRegister;

				case OpcodeParameterType::IMM8:
				case OpcodeParameterType::IMM16:
				case OpcodeParameterType::IMM24:
				case OpcodeParameterType::SIMM16:
				case OpcodeParameterType::SIMM24:
					return OperandType::Immediate;

				case OpcodeParameterType::RD_IMM16:
				case OpcodeParameterType::RS_IMM16:
				case OpcodeParameterType::RT_IMM16:
					return OperandType::RegisterPlusAddress;

				default:
					return OperandType::Invalid;
			}
		}

		static constexpr std::string_view operandTypeToString(OperandType type) noexcept
		{
			switch (type)
			{
				case OperandType::IntegralRegister: return "Reg";
				case OperandType::FloatingPointRegister: return "FReg";
				case OperandType::Immediate: return "Imm";
				case OperandType::RegisterPlusAddress: return "Reg+Addr";
				case OperandType::VariableU8: return "VarU8";
				case OperandType::VariableS8: return "VarS8";
				case OperandType::VariableU16: return "VarU16";
				case OperandType::VariableS16: return "VarS16";
				case OperandType::VariableU32: return "VarU32";
				case OperandType::VariableS32: return "VarS32";
				case OperandType::VariableF32: return "VarF32";
				default: return "Unknown";
			}
		}
	};

	inline constexpr OpcodeParameter OpcodeParameter::Invalid = OpcodeParameter::makeInvalid();

	class OpcodeInfo
	{
	public:
		static inline constexpr usize MaxParametersPerOpcode = 4; // Maximum number of parameters per opcode
		using ParameterArray = std::array<OpcodeParameter, MaxParametersPerOpcode>;

	private:
		vm::Opcode _opcode = vm::Opcode::NOP;
		ParameterArray _parameters{};

	public:
		constexpr OpcodeInfo() noexcept = default;
		constexpr OpcodeInfo(const OpcodeInfo&) noexcept = default;
		constexpr OpcodeInfo(OpcodeInfo&&) noexcept = default;
		constexpr ~OpcodeInfo() noexcept = default;

		constexpr OpcodeInfo& operator=(const OpcodeInfo&) noexcept = default;
		constexpr OpcodeInfo& operator=(OpcodeInfo&&) noexcept = default;

	private:
		constexpr OpcodeInfo(vm::Opcode opcode, ParameterArray&& parameters) noexcept :
			_opcode(opcode), _parameters(std::move(parameters))
		{}

	public:
		constexpr vm::Opcode opcode() const noexcept { return _opcode; }
		constexpr std::span<const OpcodeParameter> parameters() const noexcept { return _parameters; }

		constexpr usize parameterCount() const noexcept
		{
			usize count = 0;
			for (int i = 0; i < MaxParametersPerOpcode; ++i)
			{
				if (_parameters[i].isValid())
					++count;
			}
			return count;
		}
		constexpr const OpcodeParameter& parameterAt(usize index) const noexcept
		{
			if (index >= _parameters.size())
				return OpcodeParameter::Invalid; // Return the invalid parameter as a fallback
			return _parameters[index];
		}

	public:
		static constexpr OpcodeInfo make(vm::Opcode opcode, ParameterArray&& parameters) noexcept
		{
			return OpcodeInfo{ opcode, std::move(parameters) };
		}

		static constexpr OpcodeInfo make(vm::Opcode opcode) noexcept
		{
			return OpcodeInfo{ opcode, {} };
		}

		static constexpr OpcodeInfo make(
			vm::Opcode opcode,
			OpcodeParameter param1,
			OpcodeParameter param2 = OpcodeParameter::Invalid,
			OpcodeParameter param3 = OpcodeParameter::Invalid,
			OpcodeParameter param4 = OpcodeParameter::Invalid) noexcept
		{
			return OpcodeInfo{ opcode, { param1, param2, param3, param4 } };
		}
	};

	struct InstructionSignature
	{
		static inline constexpr usize MaxOperandsPerInstruction = 4; // Maximum number of operands per instruction
		using OperandArray = std::array<OperandType, MaxOperandsPerInstruction>;

		Mnemonic mnemonic;
		OperandArray operands;

		constexpr OperandType operandTypeAt(usize index) const noexcept
		{
			if (index >= operands.size())
				return OperandType::Invalid;
			return operands[index];
		}

		constexpr usize operandCount() const noexcept
		{
			usize count = 0;
			for (int i = 0; i < MaxOperandsPerInstruction; ++i)
			{
				if (operands[i] != OperandType::Invalid)
					++count;
			}
			return count;
		}

		constexpr bool operator==(const InstructionSignature&) const noexcept = default;

		static constexpr InstructionSignature make(Mnemonic mnemonic, OperandArray&& operands) noexcept
		{
			return InstructionSignature{ mnemonic, std::move(operands) };
		}

		static constexpr InstructionSignature make(Mnemonic mnemonic, const std::span<const OpcodeParameter, MaxOperandsPerInstruction>& parameters) noexcept
		{
			return InstructionSignature{ mnemonic, {
				parameters[0].operandType(),
				parameters[1].operandType(),
				parameters[2].operandType(),
				parameters[3].operandType()
			} };
		}

		static constexpr InstructionSignature make(Mnemonic mnemonic) noexcept
		{
			return InstructionSignature{ mnemonic, {} };
		}

		static constexpr InstructionSignature make(
			Mnemonic mnemonic,
			OperandType op1,
			OperandType op2 = OperandType::Invalid,
			OperandType op3 = OperandType::Invalid,
			OperandType op4 = OperandType::Invalid) noexcept
		{
			return InstructionSignature{ mnemonic, { op1, op2, op3, op4 } };
		}

		static constexpr InstructionSignature make(
			Mnemonic mnemonic,
			OpcodeParameterType op1,
			OpcodeParameterType op2 = OpcodeParameterType::Invalid,
			OpcodeParameterType op3 = OpcodeParameterType::Invalid,
			OpcodeParameterType op4 = OpcodeParameterType::Invalid) noexcept
		{
			return make(
				mnemonic,
				OpcodeParameter::toOperandType(op1),
				OpcodeParameter::toOperandType(op2),
				OpcodeParameter::toOperandType(op3),
				OpcodeParameter::toOperandType(op4)
			);
		}


		std::string toString() const
		{
			std::string result;
			result.reserve(20); // Reserve some space to avoid multiple allocations

			result += std::string(mnemonicToString(mnemonic));

			for (int i = 0; i < MaxOperandsPerInstruction; ++i)
			{
				if (operands[i] != OperandType::Invalid)
				{
					result += " ";
					result += std::string(OpcodeParameter::operandTypeToString(operands[i]));
				}
			}

			return result;
		}
	};

	class InstructionInfo
	{
	public:
		using Signature = InstructionSignature;
		using OpcodesVector = std::vector<OpcodeInfo>;

	private:
		Signature _signature{};
		OpcodesVector _opcodes{};

	public:
		constexpr InstructionInfo() noexcept = default;
		constexpr InstructionInfo(const InstructionInfo&) noexcept = default;
		constexpr InstructionInfo(InstructionInfo&&) noexcept = default;
		constexpr ~InstructionInfo() noexcept = default;

		constexpr InstructionInfo& operator=(const InstructionInfo&) noexcept = default;
		constexpr InstructionInfo& operator=(InstructionInfo&&) noexcept = default;

	private:
		constexpr explicit InstructionInfo(Signature signature, OpcodesVector&& opcodes) noexcept :
			_signature(std::move(signature)), _opcodes(std::move(opcodes))
		{}

	public:
		constexpr InstructionSignature signature() const noexcept { return _signature; }
		constexpr std::span<const OpcodeInfo> opcodes() const noexcept { return _opcodes; }
		constexpr usize opcodeCount() const noexcept { return _opcodes.size(); }
		constexpr u32 sizeInBytes() const noexcept { return static_cast<u32>(vm::Instruction::Size * _opcodes.size()); }
		
		inline usize maxOpcodeCountAtAllOverloads() const noexcept
		{
			auto result = findMaxSizeInBytes(_signature.mnemonic);
			if (result.has_value() && *result >= vm::Instruction::Size)
				return *result / vm::Instruction::Size;
			return opcodeCount();
		}

	public:
		static constexpr InstructionInfo make(Signature signature, OpcodesVector&& opcodes) noexcept
		{
			return InstructionInfo{ std::move(signature), std::move(opcodes) };
		}

		static constexpr InstructionInfo make(Signature signature, vm::Opcode opcode) noexcept
		{
			return InstructionInfo{ std::move(signature), { OpcodeInfo::make(opcode) } };
		}

		static constexpr InstructionInfo make(
			Signature signature,
			vm::Opcode opcode,
			OpcodeParameter param1,
			OpcodeParameter param2 = OpcodeParameter::Invalid,
			OpcodeParameter param3 = OpcodeParameter::Invalid,
			OpcodeParameter param4 = OpcodeParameter::Invalid) noexcept
		{
			return InstructionInfo{ std::move(signature), { OpcodeInfo::make(opcode, param1, param2, param3, param4) } };
		}

		static constexpr InstructionInfo make(
			Signature signature,
			vm::Opcode opcode,
			OpcodeParameterType param1,
			OpcodeParameterType param2 = OpcodeParameterType::Invalid,
			OpcodeParameterType param3 = OpcodeParameterType::Invalid,
			OpcodeParameterType param4 = OpcodeParameterType::Invalid) noexcept
		{
			return make(
				std::move(signature),
				opcode,
				OpcodeParameter::make(param1, 0),
				OpcodeParameter::make(param2, 1),
				OpcodeParameter::make(param3, 2),
				OpcodeParameter::make(param4, 3)
			);
		}

	public:
		static std::optional<InstructionInfo> find(InstructionSignature signature) noexcept;
		static std::optional<u32> findMaxSizeInBytes(Mnemonic mnemonic) noexcept;
	};
}

template <>
struct std::hash<ceres::casm::InstructionSignature>
{
	static inline std::size_t operator()(const ceres::casm::InstructionSignature& signature) noexcept
	{
		std::hash<std::uint8_t> h;
		std::size_t result = h(static_cast<std::uint8_t>(signature.mnemonic));
		const std::size_t prime = 31;
		for (int i = 0; i < ceres::casm::InstructionSignature::MaxOperandsPerInstruction; ++i)
			result = result * prime ^ h(static_cast<std::uint8_t>(signature.operands[i]));

		return result;
	}
};
