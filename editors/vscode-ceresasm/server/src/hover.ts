import { Hover, MarkupKind, Position, Range } from 'vscode-languageserver/node';
import { TextDocument } from 'vscode-languageserver-textdocument';
import { describeRegister, KEYWORDS, MNEMONICS, SECTIONS, TYPES } from './languageData';
import { resolveUserSymbol } from './resolution';
import { findEnclosingMacro, getCleanedLines, getTokenAtCharacter, SymbolIndexer } from './symbolIndex';

// A fenced ```casm block gets the same TextMate-grammar syntax highlighting in the hover popup
// as the editor itself; a single-backtick inline span never does; a plain paragraph doesn't
// support a language tag at all.
function hover(code: string, kindLabel: string, description: string, range: Range): Hover {
	const parts = [`\`\`\`casm\n${code}\n\`\`\``];
	if (kindLabel) {
		parts.push(`*${kindLabel}*`);
	}
	if (description) {
		parts.push(description);
	}
	return { contents: { kind: MarkupKind.Markdown, value: parts.join('\n\n') }, range };
}

export function provideHover(document: TextDocument, position: Position, indexer: SymbolIndexer): Hover | null {
	const lineText = getCleanedLines(indexer, document.uri)[position.line] ?? '';
	const token = getTokenAtCharacter(lineText, position.character);
	if (!token) {
		return null;
	}

	const range = Range.create(position.line, token.startCharacter, position.line, token.endCharacter);

	if (token.text.startsWith('@')) {
		const doc = SECTIONS[token.text.slice(1)];
		if (doc) {
			return hover(token.text, 'section', doc, range);
		}
	}

	const registerDoc = describeRegister(token.text);
	if (registerDoc) {
		return hover(token.text, 'register', registerDoc, range);
	}

	const mnemonic = MNEMONICS[token.text.toLowerCase()];
	if (mnemonic) {
		const code = mnemonic.operands ? `${token.text.toLowerCase()} ${mnemonic.operands}` : token.text.toLowerCase();
		return hover(code, mnemonic.pseudo ? 'pseudo-instruction' : 'instruction', mnemonic.summary, range);
	}

	if (token.text in KEYWORDS) {
		return hover(token.text, 'keyword', KEYWORDS[token.text], range);
	}

	if (token.text in TYPES) {
		return hover(token.text, 'type', TYPES[token.text], range);
	}

	if (token.text.startsWith('$')) {
		const file = indexer.getFileIndex(document.uri);
		const macro = findEnclosingMacro(file, position.line);
		if (macro && macro.params.includes(token.text)) {
			return hover(token.text, 'macro parameter', `Parameter of macro \`${macro.name}\` (${macro.params.join(', ')}).`, range);
		}
		return hover(token.text, 'macro parameter', '', range);
	}

	if (token.text.startsWith('%%')) {
		return hover(token.text, 'hygienic macro label', 'Unique to each expansion of the enclosing macro.', range);
	}

	const resolved = resolveUserSymbol(indexer, document.uri, position, token, lineText);
	if (!resolved) {
		return null;
	}

	switch (resolved.kind) {
		case 'const':
			return hover(`const ${resolved.symbol.name} = ${resolved.symbol.valueText}`, 'const', '', range);
		case 'variable': {
			const description = resolved.symbol.section ? `Declared in \`@${resolved.symbol.section}\`.` : '';
			return hover(`let ${resolved.symbol.name}: ${resolved.symbol.typeText}`, 'variable', description, range);
		}
		case 'label': {
			const code = resolved.symbol.visibility === 'global' ? `global ${resolved.symbol.declaredName}:` : `${resolved.symbol.declaredName}:`;
			return hover(code, `${resolved.symbol.visibility} label`, '', range);
		}
		case 'macro': {
			const code = resolved.candidates.map((candidate) => `${resolved.symbol.name} ${candidate.params.join(', ')}`.trimEnd()).join('\n');
			const kindLabel = resolved.candidates.length > 1 ? `macro (${resolved.candidates.length} overloads)` : 'macro';
			return hover(code, kindLabel, '', range);
		}
	}
}
