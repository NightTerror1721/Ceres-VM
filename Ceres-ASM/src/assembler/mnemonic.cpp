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
		{ "CLI", Mnemonic::CLI },
		{ "STI", Mnemonic::STI },

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
		{ "LDVP", Mnemonic::LDVP },
		{ "STVP", Mnemonic::STVP },

		{ "JP", Mnemonic::JP },
		{ "CMP", Mnemonic::CMP },
		{ "JZ", Mnemonic::JZ },
		{ "JNZ", Mnemonic::JNZ },
		{ "JC", Mnemonic::JC },
		{ "JNC", Mnemonic::JNC },
		{ "JS", Mnemonic::JS },
		{ "JNS", Mnemonic::JNS },
		{ "JO", Mnemonic::JO },
		{ "JNO", Mnemonic::JNO },
		{ "CALL", Mnemonic::CALL },
		{ "RET", Mnemonic::RET },
		{ "JMP", Mnemonic::JMP },
		{ "JEQ", Mnemonic::JEQ },
		{ "JNE", Mnemonic::JNE },
		{ "JGR", Mnemonic::JGR },
		{ "JGE", Mnemonic::JGE },
		{ "JLS", Mnemonic::JLS },
		{ "JLE", Mnemonic::JLE },
		{ "JAB", Mnemonic::JAB },
		{ "JAE", Mnemonic::JAE },
		{ "JBL", Mnemonic::JBL },
		{ "JBE", Mnemonic::JBE },
		{ "IFEQ", Mnemonic::IFEQ },
		{ "IFNE", Mnemonic::IFNE },
		{ "IFGR", Mnemonic::IFGR },
		{ "IFGE", Mnemonic::IFGE },
		{ "IFLS", Mnemonic::IFLS },
		{ "IFLE", Mnemonic::IFLE },
		{ "IFAB", Mnemonic::IFAB },
		{ "IFAE", Mnemonic::IFAE },
		{ "IFBL", Mnemonic::IFBL },
		{ "IFBE", Mnemonic::IFBE },
		{ "INC", Mnemonic::INC },
		{ "DEC", Mnemonic::DEC },
		{ "LC", Mnemonic::LC },
		{ "CLR", Mnemonic::CLR },
		{ "SWAP", Mnemonic::SWAP },
		{ "ENTER", Mnemonic::ENTER },
		{ "LEAVE", Mnemonic::LEAVE },
		{ "TST", Mnemonic::TST },

		{ "PUSH", Mnemonic::PUSH },
		{ "POP", Mnemonic::POP },
		{ "PUSHM", Mnemonic::PUSHM },
		{ "POPM", Mnemonic::POPM },
		{ "PUSHF", Mnemonic::PUSHF },
		{ "POPF", Mnemonic::POPF },

		{ "ITOF", Mnemonic::ITOF },
		{ "IITOF", Mnemonic::IITOF },
		{ "FTOI", Mnemonic::FTOI },
		{ "FTOII", Mnemonic::FTOII },
		{ "MTF", Mnemonic::MTF },
		{ "MFF", Mnemonic::MFF },

		{ "IN", Mnemonic::IN },
		{ "INB", Mnemonic::INB },
		{ "INH", Mnemonic::INH },
		{ "INSB", Mnemonic::INSB },
		{ "INSH", Mnemonic::INSH },
		{ "INM", Mnemonic::INM },
		{ "OUT", Mnemonic::OUT },
		{ "OUTB", Mnemonic::OUTB },
		{ "OUTH", Mnemonic::OUTH },
		{ "OUTM", Mnemonic::OUTM }
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
		{ Mnemonic::CLI, "CLI" },
		{ Mnemonic::STI, "STI" },

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
		{ Mnemonic::LDVP, "LDVP" },
		{ Mnemonic::STVP, "STVP" },

		{ Mnemonic::JP, "JP" },
		{ Mnemonic::CMP, "CMP" },
		{ Mnemonic::JZ, "JZ" },
		{ Mnemonic::JNZ, "JNZ" },
		{ Mnemonic::JC, "JC" },
		{ Mnemonic::JNC, "JNC" },
		{ Mnemonic::JS, "JS" },
		{ Mnemonic::JNS, "JNS" },
		{ Mnemonic::JO, "JO" },
		{ Mnemonic::JNO, "JNO" },
		{ Mnemonic::CALL, "CALL" },
		{ Mnemonic::RET, "RET" },
		{ Mnemonic::JMP, "JMP" },
		{ Mnemonic::JEQ, "JEQ" },
		{ Mnemonic::JNE, "JNE" },
		{ Mnemonic::JGR, "JGR" },
		{ Mnemonic::JGE, "JGE" },
		{ Mnemonic::JLS, "JLS" },
		{ Mnemonic::JLE, "JLE" },
		{ Mnemonic::JAB, "JAB" },
		{ Mnemonic::JAE, "JAE" },
		{ Mnemonic::JBL, "JBL" },
		{ Mnemonic::JBE, "JBE" },
		{ Mnemonic::IFEQ, "IFEQ" },
		{ Mnemonic::IFNE, "IFNE" },
		{ Mnemonic::IFGR, "IFGR" },
		{ Mnemonic::IFGE, "IFGE" },
		{ Mnemonic::IFLS, "IFLS" },
		{ Mnemonic::IFLE, "IFLE" },
		{ Mnemonic::IFAB, "IFAB" },
		{ Mnemonic::IFAE, "IFAE" },
		{ Mnemonic::IFBL, "IFBL" },
		{ Mnemonic::IFBE, "IFBE" },
		{ Mnemonic::INC, "INC" },
		{ Mnemonic::DEC, "DEC" },
		{ Mnemonic::LC, "LC" },
		{ Mnemonic::CLR, "CLR" },
		{ Mnemonic::SWAP, "SWAP" },
		{ Mnemonic::ENTER, "ENTER" },
		{ Mnemonic::LEAVE, "LEAVE" },
		{ Mnemonic::TST, "TST" },

		{ Mnemonic::PUSH, "PUSH" },
		{ Mnemonic::POP, "POP" },
		{ Mnemonic::PUSHM, "PUSHM" },
		{ Mnemonic::POPM, "POPM" },
		{ Mnemonic::PUSHF, "PUSHF" },
		{ Mnemonic::POPF, "POPF" },

		{ Mnemonic::ITOF, "ITOF" },
		{ Mnemonic::IITOF, "IITOF" },
		{ Mnemonic::FTOI, "FTOI" },
		{ Mnemonic::FTOII, "FTOII" },
		{ Mnemonic::MTF, "MTF" },
		{ Mnemonic::MFF, "MFF" },

		{ Mnemonic::IN, "IN" },
		{ Mnemonic::INB, "INB" },
		{ Mnemonic::INH, "INH" },
		{ Mnemonic::INSB, "INSB" },
		{ Mnemonic::INSH, "INSH" },
		{ Mnemonic::INM, "INM" },
		{ Mnemonic::OUT, "OUT" },
		{ Mnemonic::OUTB, "OUTB" },
		{ Mnemonic::OUTH, "OUTH" },
		{ Mnemonic::OUTM, "OUTM" }
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