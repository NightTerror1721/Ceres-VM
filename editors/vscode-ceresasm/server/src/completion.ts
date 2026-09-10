import { CompletionItem, CompletionItemKind, Position } from 'vscode-languageserver/node';
import { TextDocument } from 'vscode-languageserver-textdocument';
import { INTERRUPT_NUMBERS, KEYWORDS, LINKER_SYMBOLS, MNEMONICS, SECTIONS, TYPES } from './languageData';
import {
	findEnclosingMacro,
	findEnclosingNonLocalLabel,
	getAllTokens,
	getCleanedLines,
	resolveImportPath,
	SymbolIndexer
} from './symbolIndex';
import { URI } from 'vscode-uri';

export function provideCompletion(document: TextDocument, position: Position, indexer: SymbolIndexer): CompletionItem[] {
	const items: CompletionItem[] = [];

	// After `Frame.` there is exactly one right answer set - that struct's fields, or that
	// module's exports - and offering the whole language alongside them would bury it.
	const member = memberCompletions(document, position, indexer);
	if (member) {
		return member;
	}

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

	for (const [name, doc] of Object.entries(LINKER_SYMBOLS)) {
		items.push({ label: name, kind: CompletionItemKind.Constant, detail: 'defined by the linker', documentation: doc });
	}

	for (const [name, doc] of Object.entries(INTERRUPT_NUMBERS)) {
		items.push({ label: name, kind: CompletionItemKind.Constant, detail: 'interrupt number', documentation: doc });
	}

	for (let i = 0; i <= 15; i++) {
		items.push({ label: `r${i}`, kind: CompletionItemKind.Variable, detail: 'integer register' });
		items.push({ label: `f${i}`, kind: CompletionItemKind.Variable, detail: 'float register' });
	}

	// The three registers that answer to a role as well as to a number.
	for (const [name, detail] of [
		['sp', 'stack pointer (r15)'],
		['fp', 'frame pointer (r14)'],
		['at', 'assembler temporary (r13)']
	] as const) {
		items.push({ label: name, kind: CompletionItemKind.Variable, detail });
	}

	const { file, consts, variables, labels, macrosByName, structs } = indexer.collectVisibleSymbols(document.uri);

	for (const symbol of consts.values()) {
		items.push({ label: symbol.name, kind: CompletionItemKind.Constant, detail: `const = ${symbol.valueText}` });
	}
	for (const symbol of structs.values()) {
		items.push({
			label: symbol.name,
			kind: CompletionItemKind.Struct,
			detail: symbol.size === undefined ? 'struct' : `struct, ${symbol.size} bytes`,
			documentation: symbol.fields.map((field) => `${field.name}: ${field.typeText}`).join('\n')
		});
	}
	// The visible set rather than this file's: what an import brings in can be written here, and
	// leaving it out of the list is what makes a symbol feel like it does not exist.
	for (const symbol of new Map([...variables, ...file.variables]).values()) {
		items.push({ label: symbol.name, kind: CompletionItemKind.Variable, detail: symbol.typeText });
	}

	// A `.name` label only resolves within its own enclosing non-local label (see
	// findEnclosingNonLocalLabel / resolveUserSymbol) - offering one from a different subroutine
	// would suggest something that, if picked, silently refers to a *different* label than the
	// one shown here (or to nothing at all), so those are left out entirely.
	const enclosingNonLocalLabel = findEnclosingNonLocalLabel(file, position.line);
	for (const symbol of new Map([...labels, ...file.labels]).values()) {
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

// `Frame.` and `math.` - the two things a dot after a name can mean here.
function memberCompletions(document: TextDocument, position: Position, indexer: SymbolIndexer): CompletionItem[] | null {
	const lineText = getCleanedLines(indexer, document.uri)[position.line] ?? '';
	const before = lineText.slice(0, position.character);
	const match = /([A-Za-z_][A-Za-z0-9_]*)\.([A-Za-z0-9_]*)$/.exec(before);
	if (!match) {
		return null;
	}

	const qualifier = match[1];
	const { structs } = indexer.collectVisibleSymbols(document.uri);

	const owner = structs.get(qualifier);
	if (owner) {
		return owner.fields.map((field) => ({
			label: field.name,
			kind: CompletionItemKind.Field,
			detail: field.offset === undefined ? field.typeText : `+${field.offset}  ${field.typeText}`,
			documentation: `Byte offset of \`${owner.name}.${field.name}\` within \`${owner.name}\`.`
		}));
	}

	const file = indexer.getFileIndex(document.uri);
	const importSpec = file.imports.find((spec) => spec.alias === qualifier);
	if (!importSpec) {
		return null;
	}

	const moduleUri = URI.file(resolveImportPath(document.uri, importSpec.importPath)).toString();
	const module = indexer.getFileIndex(moduleUri);
	const items: CompletionItem[] = [];

	for (const symbol of module.consts.values()) {
		if (symbol.isGlobal) {
			items.push({ label: symbol.name, kind: CompletionItemKind.Constant, detail: `const = ${symbol.valueText}` });
		}
	}
	for (const symbol of module.variables.values()) {
		if (symbol.isGlobal) {
			items.push({ label: symbol.name, kind: CompletionItemKind.Variable, detail: symbol.typeText });
		}
	}
	for (const symbol of module.structs.values()) {
		if (symbol.isGlobal) {
			items.push({ label: symbol.name, kind: CompletionItemKind.Struct, detail: 'struct' });
		}
	}
	for (const symbol of module.labels.values()) {
		if (symbol.visibility === 'global') {
			items.push({ label: symbol.declaredName, kind: CompletionItemKind.Function, detail: 'global label' });
		}
	}

	return items;
}
