import { MarkupKind, ParameterInformation, Position, SignatureHelp, SignatureInformation } from 'vscode-languageserver/node';
import { TextDocument } from 'vscode-languageserver-textdocument';
import { getCleanedLines, MacroSymbol, SymbolIndexer } from './symbolIndex';

// A macro call looks exactly like an instruction - an identifier, then comma-separated operands -
// so the leading name is found the same way everywhere else in this codebase finds it.
const STATEMENT_HEAD_RE = /^(\s*)([A-Za-z_][A-Za-z0-9_]*)\b/;

function countTopLevelCommas(text: string): number {
	let depth = 0;
	let count = 0;
	for (const char of text) {
		if (char === '[') {
			depth++;
		} else if (char === ']') {
			depth = Math.max(0, depth - 1);
		} else if (char === ',' && depth === 0) {
			count++;
		}
	}
	return count;
}

// `clamp(reg value, imm lo, imm hi) -> reg` once every parameter is tagged, `clamp $value, $lo,
// $hi` otherwise - the same all-or-nothing rule `macroSignature` in hover.ts uses, so the hover
// popup and the signature help never disagree about whether a macro "looks tagged".
function signatureFor(macro: MacroSymbol): SignatureInformation {
	const byName = new Map(macro.docParams.map((tag) => [tag.name, tag] as const));
	const tagged = macro.docParams.length === macro.params.length && macro.params.every((name) => byName.has(name));

	const parameters: ParameterInformation[] = macro.params.map((paramName) => {
		const tag = byName.get(paramName);
		const label = tagged && tag ? `${tag.kind} ${paramName.replace(/^\$/, '')}` : paramName;
		return tag?.description ? { label, documentation: { kind: MarkupKind.Markdown, value: tag.description } } : { label };
	});

	const returns = tagged && macro.docReturn ? ` -> ${macro.docReturn.kind}` : '';
	const label = `${macro.name}(${parameters.map((parameter) => parameter.label).join(', ')})${returns}`;

	return {
		label,
		documentation: macro.doc ? { kind: MarkupKind.Markdown, value: macro.doc } : undefined,
		parameters
	};
}

export function provideSignatureHelp(document: TextDocument, position: Position, indexer: SymbolIndexer): SignatureHelp | null {
	const lineText = getCleanedLines(indexer, document.uri)[position.line] ?? '';
	const headMatch = STATEMENT_HEAD_RE.exec(lineText);
	if (!headMatch) {
		return null;
	}

	const name = headMatch[2];
	const nameEnd = headMatch[0].length;

	const { macrosByName } = indexer.collectVisibleSymbols(document.uri);
	const candidates = macrosByName.get(name);
	if (!candidates || candidates.length === 0) {
		return null;
	}

	const argsTextBeforeCursor = position.character > nameEnd ? lineText.slice(nameEnd, position.character) : '';
	const activeParameter = countTopLevelCommas(argsTextBeforeCursor);

	// The smallest overload that still has a slot for the parameter being typed - and the largest
	// one available once more arguments have been typed than any overload declares.
	const sorted = [...candidates].sort((a, b) => a.arity - b.arity);
	let activeSignature = sorted.findIndex((candidate) => candidate.arity > activeParameter);
	if (activeSignature === -1) {
		activeSignature = sorted.length - 1;
	}

	return {
		signatures: sorted.map(signatureFor),
		activeSignature,
		activeParameter: Math.min(activeParameter, Math.max(0, sorted[activeSignature].params.length - 1))
	};
}
