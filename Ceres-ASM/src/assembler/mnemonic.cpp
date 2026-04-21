#include "mnemonic.h"
#include <unordered_map>
#include <algorithm>

namespace ceres::casm
{
	static inline const std::unordered_map<std::string, Mnemonic> __toLowerMap(const std::unordered_map<std::string_view, Mnemonic>& originalMap)
	{
		std::unordered_map<std::string, Mnemonic> lowerMap;
		lowerMap.reserve(originalMap.size());
		for (const auto& [key, value] : originalMap)
		{
			std::string lowerKey(key);
			std::transform(lowerKey.begin(), lowerKey.end(), lowerKey.begin(), [](unsigned char c) {
				return static_cast<char>(std::tolower(c));
				});
			lowerMap.emplace(std::move(lowerKey), value);
		}
		return lowerMap;
	}

	static const std::unordered_map<std::string_view, Mnemonic> __StringToMnemonic
	{
		{ "NOP", Mnemonic::NOP },
		{ "HALT", Mnemonic::HALT },
		{ "TRAP", Mnemonic::TRAP },
		{ "RESET", Mnemonic::RESET },
		{ "INT", Mnemonic::INT },
		{ "IRET", Mnemonic::IRET },

		{ "ADD", Mnemonic::ADD },
		{ "ADC", Mnemonic::ADC },
		{ "SUB", Mnemonic::SUB },
		{ "SBC", Mnemonic::SBC },
		{ "MUL", Mnemonic::MUL },
		{ "IMUL", Mnemonic::IMUL },
		{ "DIV", Mnemonic::DIV },
		{ "IDIV", Mnemonic::IDIV },
		{ "MOD", Mnemonic::MOD },
		{ "IMOD", Mnemonic::IMOD },
		{ "NEG", Mnemonic::NEG },

		{ "AND", Mnemonic::AND },
		{ "OR", Mnemonic::OR },
		{ "XOR", Mnemonic::XOR },
		{ "NOT", Mnemonic::NOT },
		{ "SHL", Mnemonic::SHL },
		{ "SHR", Mnemonic::SHR },
		{ "SAR", Mnemonic::SAR },

		{ "MOV", Mnemonic::MOV },
		{ "LI", Mnemonic::LI },
		{ "LUI", Mnemonic::LUI },
		{ "LDR", Mnemonic::LDR },
		{ "LDRB", Mnemonic::LDRB },
		{ "LDRH", Mnemonic::LDRH },
		{ "LDRSB", Mnemonic::LDRSB },
		{ "LDRSH", Mnemonic::LDRSH },
		{ "LDV", Mnemonic::LDV },
		{ "STR", Mnemonic::STR },
		{ "STRB", Mnemonic::STRB },
		{ "STRH", Mnemonic::STRH },
		{ "STV", Mnemonic::STV },
		{ "LA", Mnemonic::LA },
		{ "LEA", Mnemonic::LEA },

		{ "JP", Mnemonic::JP },
		{ "CMP", Mnemonic::CMP },
		{ "JZ", Mnemonic::JZ },
		{ "JNZ", Mnemonic::JNZ },
		{ "JC", Mnemonic::JC },
		{ "JNC", Mnemonic::JNC },
		{ "JS", Mnemonic::JS },
		{ "JNS", Mnemonic::JNS },
		{ "CALL", Mnemonic::CALL },
		{ "RET", Mnemonic::RET },

		{ "PUSH", Mnemonic::PUSH },
		{ "POP", Mnemonic::POP },
		{ "PUSHF", Mnemonic::PUSHF },
		{ "POPF", Mnemonic::POPF },

		{ "ITOF", Mnemonic::ITOF },
		{ "IITOF", Mnemonic::IITOF },
		{ "FTOI", Mnemonic::FTOI },
		{ "FTOII", Mnemonic::FTOII },
		{ "MTF", Mnemonic::MTF },
		{ "MFF", Mnemonic::MFF },

		{ "IN", Mnemonic::IN },
		{ "OUT", Mnemonic::OUT }
	};

	static const std::unordered_map<std::string, Mnemonic> __StringLowerToMnemonic = __toLowerMap(__StringToMnemonic);

	static const std::unordered_map<Mnemonic, std::string_view> __MnemonicToString
	{
		{ Mnemonic::NOP, "NOP" },
		{ Mnemonic::HALT, "HALT" },
		{ Mnemonic::TRAP, "TRAP" },
		{ Mnemonic::RESET, "RESET" },
		{ Mnemonic::INT, "INT" },
		{ Mnemonic::IRET, "IRET" },

		{ Mnemonic::ADD, "ADD" },
		{ Mnemonic::ADC, "ADC" },
		{ Mnemonic::SUB, "SUB" },
		{ Mnemonic::SBC, "SBC" },
		{ Mnemonic::MUL, "MUL" },
		{ Mnemonic::IMUL, "IMUL" },
		{ Mnemonic::DIV, "DIV" },
		{ Mnemonic::IDIV, "IDIV" },
		{ Mnemonic::MOD, "MOD" },
		{ Mnemonic::IMOD, "IMOD" },
		{ Mnemonic::NEG, "NEG" },

		{ Mnemonic::AND, "AND" },
		{ Mnemonic::OR, "OR" },
		{ Mnemonic::XOR, "XOR" },
		{ Mnemonic::NOT, "NOT" },
		{ Mnemonic::SHL, "SHL" },
		{ Mnemonic::SHR, "SHR" },
		{ Mnemonic::SAR, "SAR" },

		{ Mnemonic::MOV, "MOV" },
		{ Mnemonic::LI, "LI" },
		{ Mnemonic::LUI, "LUI" },
		{ Mnemonic::LDR, "LDR" },
		{ Mnemonic::LDRB, "LDRB" },
		{ Mnemonic::LDRH, "LDRH" },
		{ Mnemonic::LDRSB, "LDRSB" },
		{ Mnemonic::LDRSH, "LDRSH" },
		{ Mnemonic::LDV, "LDV" },
		{ Mnemonic::STR, "STR" },
		{ Mnemonic::STRB, "STRB" },
		{ Mnemonic::STRH, "STRH" },
		{ Mnemonic::STV, "STV" },
		{ Mnemonic::LA, "LA" },
		{ Mnemonic::LEA, "LEA" },

		{ Mnemonic::JP, "JP" },
		{ Mnemonic::CMP, "CMP" },
		{ Mnemonic::JZ, "JZ" },
		{ Mnemonic::JNZ, "JNZ" },
		{ Mnemonic::JC, "JC" },
		{ Mnemonic::JNC, "JNC" },
		{ Mnemonic::JS, "JS" },
		{ Mnemonic::JNS, "JNS" },
		{ Mnemonic::CALL, "CALL" },
		{ Mnemonic::RET, "RET" },

		{ Mnemonic::PUSH, "PUSH" },
		{ Mnemonic::POP, "POP" },
		{ Mnemonic::PUSHF, "PUSHF" },
		{ Mnemonic::POPF, "POPF" },

		{ Mnemonic::ITOF, "ITOF" },
		{ Mnemonic::IITOF, "IITOF" },
		{ Mnemonic::FTOI, "FTOI" },
		{ Mnemonic::FTOII, "FTOII" },
		{ Mnemonic::MTF, "MTF" },
		{ Mnemonic::MFF, "MFF" },

		{ Mnemonic::IN, "IN" },
		{ Mnemonic::OUT, "OUT" }
	};

	std::string_view mnemonicToString(Mnemonic mnemonic) noexcept
	{
		return __MnemonicToString.at(mnemonic);
	}

	std::optional<Mnemonic> stringToMnemonic(std::string_view str, bool caseSensitive) noexcept
	{
		auto it = __StringToMnemonic.find(str);
		if (it != __StringToMnemonic.end())
			return it->second;

		if (!caseSensitive)
		{
			std::string lowerStr(str);
			std::transform(lowerStr.begin(), lowerStr.end(), lowerStr.begin(), [](unsigned char c) {
				return static_cast<char>(std::tolower(c));
				});

			auto lowerIt = __StringLowerToMnemonic.find(lowerStr);
			if (lowerIt != __StringLowerToMnemonic.end())
				return lowerIt->second;
		}

		return std::nullopt;
	}
}