import { CompletionItem, CompletionItemKind, Position } from 'vscode-languageserver/node';
import { TextDocument } from 'vscode-languageserver-textdocument';
import { KEYWORDS, MNEMONICS, SECTIONS, TYPES } from './languageData';
import { findEnclosingMacro, findEnclosingNonLocalLabel, getAllTokens, getCleanedLines, SymbolIndexer } from './symbolIndex';

export function provideCompletion(document: TextDocument, position: Position, indexer: SymbolIndexer): CompletionItem[] {
	const items: CompletionItem[] = [];

	for (const [name, doc] of Object.entries(MNEMONICS)) {
		items.push({
			label: name,
			kind: CompletionItemKind.Keyword,
			detail: doc.operands ? `${name} ${doc.operands}` : name,
			documentation: doc.summary
		});
	}

	for (const [name, doc] of Object.entries(KEYWORDS)) {
		items.push({ label: name, kind: CompletionItemKind.Keyword, documentation: doc });
	}

	for (const [name, doc] of Object.entries(TYPES)) {
		items.push({ label: name, kind: CompletionItemKind.TypeParameter, documentation: doc });
	}

	for (const [name, doc] of Object.entries(SECTIONS)) {
		items.push({ label: `@${name}`, kind: CompletionItemKind.Module, documentation: doc });
	}

	for (let i = 0; i <= 15; i++) {
		items.push({ label: `r${i}`, kind: CompletionItemKind.Variable, detail: 'integer register' });
		items.push({ label: `f${i}`, kind: CompletionItemKind.Variable, detail: 'float register' });
	}

	const { file, consts, macrosByName } = indexer.collectVisibleSymbols(document.uri);

	for (const symbol of consts.values()) {
		items.push({ label: symbol.name, kind: CompletionItemKind.Constant, detail: `const = ${symbol.valueText}` });
	}
	for (const symbol of file.variables.values()) {
		items.push({ label: symbol.name, kind: CompletionItemKind.Variable, detail: symbol.typeText });
	}

	// A `.name` label only resolves within its own enclosing non-local label (see
	// findEnclosingNonLocalLabel / resolveUserSymbol) - offering one from a different subroutine
	// would suggest something that, if picked, silently refers to a *different* label than the
	// one shown here (or to nothing at all), so those are left out entirely.
	const enclosingNonLocalLabel = findEnclosingNonLocalLabel(file, position.line);
	for (const symbol of file.labels.values()) {
		if (symbol.visibility === 'local') {
			const owner = enclosingNonLocalLabel ? `${enclosingNonLocalLabel.declaredName}${symbol.declaredName}` : symbol.declaredName;
			if (owner !== symbol.qualifiedName) {
				continue;
			}
		}
		items.push({ label: symbol.declaredName, kind: CompletionItemKind.Function, detail: `${symbol.visibility} label` });
	}

	for (const [name, candidates] of macrosByName) {
		const arities = candidates.map((candidate) => `(${candidate.params.join(', ') || 'no arguments'})`).join(' | ');
		items.push({ label: name, kind: CompletionItemKind.Snippet, detail: `macro ${arities}` });
	}

	// $params and %%labels only mean anything inside the macro body that declares them.
	const enclosingMacro = findEnclosingMacro(file, position.line);
	if (enclosingMacro) {
		for (const param of enclosingMacro.params) {
			items.push({ label: param, kind: CompletionItemKind.Variable, detail: `parameter of macro ${enclosingMacro.name}` });
		}

		const hygienicLabels = new Set<string>();
		const lines = getCleanedLines(indexer, document.uri);
		for (let line = enclosingMacro.range.start.line; line <= enclosingMacro.bodyEndLine && line < lines.length; line++) {
			for (const token of getAllTokens(lines[line])) {
				if (token.text.startsWith('%%')) {
					hygienicLabels.add(token.text);
				}
			}
		}
		for (const label of hygienicLabels) {
			items.push({ label, kind: CompletionItemKind.Function, detail: 'hygienic macro label' });
		}
	}

	return items;
}
