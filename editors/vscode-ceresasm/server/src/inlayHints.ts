// The small grey notes an editor draws inside the code: what a name resolves to, where a field
// sits, which port a number is.
//
// Everything here is something the assembler already knows and the source deliberately does not
// say twice - a variable's width, a constant's value, a struct field's offset, the name of a
// macro's parameter, the device behind a port number. None of it is a guess: a hint is only shown
// where the symbol index resolved the name, so a wrong hint means a wrong index rather than a
// plausible-looking invention.

import { InlayHint, InlayHintKind, Range } from 'vscode-languageserver/node';
import { TextDocument } from 'vscode-languageserver-textdocument';
import { describeRegister, IMPLEMENTED_PORTS, isReservedWord, MNEMONICS, PORTS } from './languageData';
import { resolveUserSymbol } from './resolution';
import { getAllTokens, getCleanedLines, qualifierBefore, SymbolIndexer, TokenAtPosition } from './symbolIndex';

export interface InlayHintSettings {
	enabled: boolean;
	variableTypes: boolean;
	constantValues: boolean;
	structOffsets: boolean;
	macroParameterNames: boolean;
	portNames: boolean;
	registerAliases: boolean;
}

export const DEFAULT_INLAY_HINT_SETTINGS: InlayHintSettings = {
	enabled: true,
	variableTypes: true,
	constantValues: true,
	structOffsets: true,
	macroParameterNames: true,
	portNames: true,
	registerAliases: true
};

// The instructions whose operands include a port number, and which operand that is. Everything
// else that takes a small immediate takes it as a value, so annotating those would be wrong.
const PORT_OPERAND: Record<string, number> = {
	in: 1,
	inb: 1,
	inh: 1,
	insb: 1,
	insh: 1,
	inm: 1,
	out: 0,
	outb: 0,
	outh: 0,
	outm: 0
};

interface Argument {
	text: string;
	startCharacter: number;
}

// Splits the operand list of a statement into arguments and remembers where each one starts, so a
// hint can be put in front of it. Bracket-aware, so `[r5 + 4]` stays one argument.
function splitArguments(lineText: string, from: number): Argument[] {
	const args: Argument[] = [];
	let depth = 0;
	let start = from;
	let current = '';

	const push = (end: number) => {
		const trimmed = current.trim();
		if (trimmed.length > 0) {
			args.push({ text: trimmed, startCharacter: start + (current.length - current.trimStart().length) });
		}
		current = '';
		start = end;
	};

	for (let i = from; i < lineText.length; i++) {
		const char = lineText[i];
		if (char === '[') depth++;
		else if (char === ']') depth = Math.max(0, depth - 1);

		if (char === ',' && depth === 0) {
			push(i + 1);
			continue;
		}
		current += char;
	}
	push(lineText.length);

	return args;
}

function parseNumber(text: string): number | null {
	const trimmed = text.trim();
	if (/^0[xX][0-9a-fA-F]+$/.test(trimmed) || /^\d+$/.test(trimmed)) {
		return Number(trimmed);
	}
	return null;
}

// The mnemonic a line starts with, if it starts with one at all. A label declaration, a `let` or a
// section directive does not.
function mnemonicOfLine(tokens: TokenAtPosition[], lineText: string): TokenAtPosition | null {
	const first = tokens[0];
	if (!first) {
		return null;
	}
	// `name:` is a label, not a call.
	const after = lineText.slice(first.endCharacter).trimStart();
	if (after.startsWith(':')) {
		return null;
	}
	return first;
}

export function provideInlayHints(
	document: TextDocument,
	range: Range,
	indexer: SymbolIndexer,
	settings: InlayHintSettings
): InlayHint[] {
	if (!settings.enabled) {
		return [];
	}

	const hints: InlayHint[] = [];
	const lines = getCleanedLines(indexer, document.uri);
	const file = indexer.getFileIndex(document.uri);

	// A declaration site says its own type already; a hint there would be the same word twice.
	const declarationLines = new Set<number>();
	for (const symbol of file.variables.values()) declarationLines.add(symbol.range.start.line);
	for (const symbol of file.consts.values()) declarationLines.add(symbol.range.start.line);
	for (const symbol of file.structs.values()) {
		for (let line = symbol.range.start.line; line <= symbol.bodyEndLine; line++) {
			declarationLines.add(line);
		}
	}

	const lastLine = Math.min(range.end.line, lines.length - 1);
	for (let line = Math.max(0, range.start.line); line <= lastLine; line++) {
		const lineText = lines[line];
		if (lineText.trim().length === 0 || declarationLines.has(line)) {
			continue;
		}

		const tokens = getAllTokens(lineText);
		if (tokens.length === 0) {
			continue;
		}

		const mnemonic = mnemonicOfLine(tokens, lineText);
		const mnemonicText = mnemonic ? mnemonic.text.toLowerCase() : '';

		// --- the device behind a port number ---------------------------------------------------
		if (settings.portNames && mnemonic && mnemonicText in PORT_OPERAND) {
			const args = splitArguments(lineText, mnemonic.endCharacter);
			const argument = args[PORT_OPERAND[mnemonicText]];
			const value = argument ? parseNumber(argument.text) : null;
			const name = value === null ? undefined : PORTS[value];
			if (argument && name) {
				hints.push({
					position: { line, character: argument.startCharacter + argument.text.length },
					label: ` ${name}`,
					kind: InlayHintKind.Type,
					paddingLeft: true,
					tooltip: IMPLEMENTED_PORTS.has(value!)
						? `Port ${name}.`
						: `Port ${name} is reserved in the default map but has no device behind it: reads answer all-ones and writes are swallowed.`
				});
			}
		}

		// --- what a macro calls its arguments ---------------------------------------------------
		if (settings.macroParameterNames && mnemonic && !isReservedWord(mnemonic.text) && !MNEMONICS[mnemonicText]) {
			const { macrosByName, macros } = indexer.collectVisibleSymbols(document.uri);
			const candidates = macrosByName.get(mnemonic.text);
			if (candidates && candidates.length > 0) {
				const args = splitArguments(lineText, mnemonic.endCharacter);
				const macro = macros.get(`${mnemonic.text}/${args.length}`) ?? candidates[0];
				if (macro.params.length === args.length) {
					args.forEach((argument, index) => {
						hints.push({
							position: { line, character: argument.startCharacter },
							label: `${macro.params[index].slice(1)}:`,
							kind: InlayHintKind.Parameter,
							paddingRight: true,
							tooltip: `Parameter \`${macro.params[index]}\` of macro \`${macro.name}\`.`
						});
					});
				}
			}
		}

		// --- what a name resolves to ------------------------------------------------------------
		for (const token of tokens) {
			if (token === mnemonic || token.text.startsWith('$') || token.text.startsWith('%%') || token.text.startsWith('@')) {
				continue;
			}
			if (isReservedWord(token.text)) {
				continue;
			}

			const isField = token.text.startsWith('.') && qualifierBefore(lineText, token) !== null;
			if (token.text.startsWith('.') && !isField) {
				continue; // A local label: its own name is the whole story
			}

			const resolved = resolveUserSymbol(indexer, document.uri, { line, character: token.startCharacter + 1 }, token, lineText);
			if (!resolved) {
				continue;
			}

			if (resolved.kind === 'field' && settings.structOffsets) {
				if (resolved.field.offset !== undefined) {
					hints.push({
						position: { line, character: token.endCharacter },
						label: `+${resolved.field.offset}`,
						kind: InlayHintKind.Type,
						paddingLeft: true,
						tooltip: `\`${resolved.owner.name}.${resolved.field.name}\` is a \`${resolved.field.typeText}\` at offset ${resolved.field.offset}.`
					});
				}
				continue;
			}

			if (resolved.kind === 'variable' && settings.variableTypes) {
				hints.push({
					position: { line, character: token.endCharacter },
					label: `: ${resolved.symbol.typeText}`,
					kind: InlayHintKind.Type,
					paddingLeft: true,
					tooltip: `\`${resolved.symbol.name}\` is declared \`${resolved.symbol.typeText}\`${
						resolved.symbol.section ? ` in \`@${resolved.symbol.section}\`` : ''
					}. A \`ldv\`/\`stv\` picks its width from this.`
				});
				continue;
			}

			if (resolved.kind === 'const') {
				const value = resolved.symbol.valueText.trim();
				// `alias cursor = r5` is indexed as a constant, but reading `cursor` as a register
				// is a different question from reading `MAX` as a number, and someone who names
				// every register will want to silence one without the other.
				const isRegisterAlias = describeRegister(value) !== null;
				if (!(isRegisterAlias ? settings.registerAliases : settings.constantValues)) {
					continue;
				}
				// A long initialiser drawn inline stops being a hint and becomes a second copy of
				// the line, so only what fits is shown.
				if (value.length > 0 && value.length <= 24) {
					hints.push({
						position: { line, character: token.endCharacter },
						label: `= ${value}`,
						kind: InlayHintKind.Type,
						paddingLeft: true,
						tooltip: `\`${resolved.symbol.name}\` is \`${value}\`, substituted where it is written.`
					});
				}
				continue;
			}

			// `Frame.field` puts the offset right after the field, and the struct's own size next
			// to it would be two numbers for one expression - so a qualified use says nothing here.
			const isQualifier = lineText[token.endCharacter] === '.';
			if (resolved.kind === 'struct' && settings.constantValues && !isQualifier && resolved.symbol.size !== undefined) {
				hints.push({
					position: { line, character: token.endCharacter },
					label: `= ${resolved.symbol.size}`,
					kind: InlayHintKind.Type,
					paddingLeft: true,
					tooltip: `The name of a struct stands for its size in bytes.`
				});
			}
		}
	}

	return hints;
}
