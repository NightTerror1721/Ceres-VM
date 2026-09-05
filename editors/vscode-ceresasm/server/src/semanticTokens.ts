import { SemanticTokenModifiers, SemanticTokens, SemanticTokensBuilder, SemanticTokenTypes } from 'vscode-languageserver/node';
import { TextDocument } from 'vscode-languageserver-textdocument';
import { isReservedWord } from './languageData';
import {
	ConstSymbol,
	FileIndex,
	getAllTokens,
	getCleanedLines,
	MacroSymbol,
	sigilLength,
	SymbolIndexer
} from './symbolIndex';

// Deliberately narrow: registers, mnemonics, CASM keywords, types and section directives already
// have correct, unambiguous colouring from the TextMate grammar (syntaxes/casm.tmLanguage.json).
// Semantic tokens take priority over it wherever both apply, so re-classifying reserved words
// here would silently override (and likely flatten) distinctions the grammar already gets right -
// that's exactly the bug this file used to have for registers and section names. This provider
// only covers what a lexical grammar genuinely cannot: telling apart plain identifiers that are
// consts, variables, non-local labels or macro calls, marking their declaration site, and
// classifying a `.local` label *reference* (the grammar only catches its declaration).
export const TOKEN_TYPES = [SemanticTokenTypes.parameter, SemanticTokenTypes.variable, SemanticTokenTypes.function, SemanticTokenTypes.macro] as const;

export const TOKEN_MODIFIERS = [SemanticTokenModifiers.declaration, SemanticTokenModifiers.readonly] as const;

const TYPE_INDEX = new Map<string, number>(TOKEN_TYPES.map((type, index) => [type, index]));
const MODIFIER_BIT = new Map<string, number>(TOKEN_MODIFIERS.map((modifier, index) => [modifier, 1 << index]));

function typeIndex(type: SemanticTokenTypes): number {
	return TYPE_INDEX.get(type) ?? 0;
}
function modifierBit(modifier: SemanticTokenModifiers): number {
	return MODIFIER_BIT.get(modifier) ?? 0;
}

interface Classification {
	type: number;
	modifiers: number;
}

function classify(
	text: string,
	file: FileIndex,
	consts: Map<string, ConstSymbol>,
	macrosByName: Map<string, MacroSymbol[]>
): Classification | null {
	if (isReservedWord(text)) {
		return null;
	}
	if (text.startsWith('$')) {
		return { type: typeIndex(SemanticTokenTypes.parameter), modifiers: 0 };
	}
	if (text.startsWith('.')) {
		// Shape alone is enough to know this is a local label; no need to resolve the target.
		return { type: typeIndex(SemanticTokenTypes.function), modifiers: 0 };
	}
	if (consts.has(text)) {
		return { type: typeIndex(SemanticTokenTypes.variable), modifiers: modifierBit(SemanticTokenModifiers.readonly) };
	}
	if (file.variables.has(text)) {
		return { type: typeIndex(SemanticTokenTypes.variable), modifiers: 0 };
	}
	const label = file.labels.get(text);
	if (label && label.visibility !== 'local') {
		return { type: typeIndex(SemanticTokenTypes.function), modifiers: 0 };
	}
	if (macrosByName.has(text)) {
		return { type: typeIndex(SemanticTokenTypes.macro), modifiers: 0 };
	}
	return null;
}

export function provideSemanticTokens(document: TextDocument, indexer: SymbolIndexer): SemanticTokens {
	const file = indexer.getFileIndex(document.uri);
	const { consts, macrosByName } = indexer.collectVisibleSymbols(document.uri);
	const lines = getCleanedLines(indexer, document.uri);

	// This file's own declaration sites, keyed by (line, bare-name start char, length), so the
	// 'declaration' modifier lands only on the defining occurrence, not every use.
	const declarationKeys = new Set<string>();
	const addDeclaration = (line: number, startCharacter: number, nameLength: number) => {
		declarationKeys.add(`${line}:${startCharacter}:${nameLength}`);
	};
	for (const symbol of file.consts.values()) addDeclaration(symbol.range.start.line, symbol.range.start.character, symbol.name.length);
	for (const symbol of file.variables.values()) addDeclaration(symbol.range.start.line, symbol.range.start.character, symbol.name.length);
	for (const symbol of file.macros.values()) addDeclaration(symbol.range.start.line, symbol.range.start.character, symbol.name.length);
	for (const symbol of file.labels.values()) {
		const offset = sigilLength(symbol.declaredName);
		const bareName = symbol.declaredName.slice(offset);
		addDeclaration(symbol.range.start.line, symbol.range.start.character + offset, bareName.length);
	}

	const builder = new SemanticTokensBuilder();

	for (let line = 0; line < lines.length; line++) {
		for (const token of getAllTokens(lines[line])) {
			const classification = classify(token.text, file, consts, macrosByName);
			if (!classification) {
				continue;
			}
			const offset = sigilLength(token.text);
			const start = token.startCharacter + offset;
			const length = token.endCharacter - start;
			if (length <= 0) {
				continue;
			}
			const modifiers = declarationKeys.has(`${line}:${start}:${length}`)
				? classification.modifiers | modifierBit(SemanticTokenModifiers.declaration)
				: classification.modifiers;
			builder.push(line, start, length, classification.type, modifiers);
		}
	}

	return builder.build();
}
