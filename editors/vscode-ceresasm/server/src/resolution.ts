import { Position } from 'vscode-languageserver/node';
import {
	ConstSymbol,
	findEnclosingNonLocalLabel,
	LabelSymbol,
	MacroSymbol,
	splitTopLevelArguments,
	SymbolIndexer,
	TokenAtPosition,
	VariableSymbol
} from './symbolIndex';

export type ResolvedSymbol =
	| { kind: 'const'; symbol: ConstSymbol }
	| { kind: 'variable'; symbol: VariableSymbol }
	| { kind: 'label'; symbol: LabelSymbol }
	| { kind: 'macro'; symbol: MacroSymbol; candidates: MacroSymbol[] };

// Shared by hover and go-to-definition: given the identifier token under the cursor, resolves it
// against the visible symbol set (this file's own labels/variables, plus consts/macros pulled in
// transitively through `import`). Callers are expected to have already ruled out reserved words
// (mnemonics, registers, keywords, types) before calling this.
export function resolveUserSymbol(
	indexer: SymbolIndexer,
	uri: string,
	position: Position,
	token: TokenAtPosition,
	lineText: string
): ResolvedSymbol | null {
	const { file, consts, macros, macrosByName } = indexer.collectVisibleSymbols(uri);

	if (token.text.startsWith('.')) {
		const parent = findEnclosingNonLocalLabel(file, position.line);
		const qualifiedName = parent ? `${parent.declaredName}${token.text}` : token.text;
		const label = file.labels.get(qualifiedName);
		return label ? { kind: 'label', symbol: label } : null;
	}

	const constSymbol = consts.get(token.text);
	if (constSymbol) {
		return { kind: 'const', symbol: constSymbol };
	}

	const variableSymbol = file.variables.get(token.text);
	if (variableSymbol) {
		return { kind: 'variable', symbol: variableSymbol };
	}

	const labelSymbol = file.labels.get(token.text);
	if (labelSymbol && labelSymbol.visibility !== 'local') {
		return { kind: 'label', symbol: labelSymbol };
	}

	const macroCandidates = macrosByName.get(token.text);
	if (macroCandidates && macroCandidates.length > 0) {
		const argsText = lineText.slice(token.endCharacter).replace(/^\s*,?\s*/, '');
		const argCount = argsText.trim().length === 0 ? 0 : splitTopLevelArguments(argsText).length;
		const exact = macros.get(`${token.text}/${argCount}`);
		return { kind: 'macro', symbol: exact ?? macroCandidates[0], candidates: macroCandidates };
	}

	return null;
}
