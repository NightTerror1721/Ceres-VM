import { Hover, MarkupKind, Position, Range } from 'vscode-languageserver/node';
import { TextDocument } from 'vscode-languageserver-textdocument';
import { describeRegister, KEYWORDS, LINKER_SYMBOLS, MNEMONICS, SECTIONS, TYPES } from './languageData';
import { resolveUserSymbol } from './resolution';
import { findEnclosingMacro, getCleanedLines, getTokenAtCharacter, LabelSymbol, MacroSymbol, StructSymbol, SymbolIndexer } from './symbolIndex';

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

// The `///` doc comment above a declaration, if any, followed by whatever technical detail this
// hover already computes for it (a struct's field table, a field's offset...). The doc comment
// leads because it says what the symbol is *for*; the rest says how it is laid out.
function withDoc(doc: string | undefined, ...rest: string[]): string {
	return [doc, ...rest].filter((part) => part && part.length > 0).join('\n\n');
}

// `global ` in front when the declaration itself carried it - shown in the fenced code block, so
// this has to read as real CASM to the grammar; `#types` in the TextMate grammar is what actually
// colors `reg`/`imm`/`freg`/`mem`/`label`/`any`/`void` as a type instead of a plain identifier.
function globalPrefix(isGlobal: boolean): string {
	return isGlobal ? 'global ' : '';
}

// `clamp $value, $lo, $hi` untagged, or `clamp(reg value, imm lo, imm hi) -> reg` once its doc
// comment tags every parameter it has - never a mix of the two, so a partially-tagged macro
// doesn't look more precise than it actually is.
function macroSignature(macro: MacroSymbol): string {
	const prefix = globalPrefix(macro.isGlobal);
	const plain = `${prefix}${macro.name} ${macro.params.join(', ')}`.trimEnd();
	if (macro.docParams.length !== macro.params.length) {
		return plain;
	}

	const byName = new Map(macro.docParams.map((tag) => [tag.name, tag] as const));
	const typedParams: string[] = [];
	for (const paramName of macro.params) {
		const tag = byName.get(paramName);
		if (!tag) {
			return plain; // A tag names a parameter that doesn't match this macro's own header.
		}
		typedParams.push(`${tag.kind} ${paramName.replace(/^\$/, '')}`);
	}

	const returns = macro.docReturn ? ` -> ${macro.docReturn.kind}` : '';
	return `${prefix}${macro.name}(${typedParams.join(', ')})${returns}`;
}

// The `@param`/`@return` tags as a table plus a return line, in the same style `describeStruct`
// already uses for fields - empty when the doc comment carries no tags at all. The leading `#`
// column is the parameter's position, left-to-right, exactly as it has to be written at a call
// site - not repeated from anywhere else already visible in the hover.
function describeMacroTags(macro: MacroSymbol): string {
	if (macro.docParams.length === 0 && !macro.docReturn) {
		return '';
	}

	const byName = new Map(macro.docParams.map((tag) => [tag.name, tag] as const));
	const rows = macro.params.map((paramName, index) => {
		const tag = byName.get(paramName);
		const kind = tag ? `\`${tag.kind}\`` : '?';
		const description = tag ? tag.description : '';
		return `| ${index + 1} | \`${paramName}\` | ${kind} | ${description} |`;
	});
	const table = rows.length > 0 ? ['| # | Param | Kind | |', '| --- | --- | --- | --- |', ...rows].join('\n') : '';

	const returnLine = macro.docReturn
		? `**Returns:** \`${macro.docReturn.kind}\`${macro.docReturn.description ? ` — ${macro.docReturn.description}` : ''}`
		: '';

	return [table, returnLine].filter((part) => part.length > 0).join('\n\n');
}

// `factorial:` untagged, or `factorial(reg r0) -> reg` once its doc comment tags it - the same
// pseudo-signature notation `macroSignature` uses, adapted to a label: there is no header to
// reconcile against, so whatever the doc comment tags is shown exactly as written.
function labelSignature(label: LabelSymbol): string {
	const prefix = globalPrefix(label.visibility === 'global');
	const plain = `${prefix}${label.declaredName}:`;
	if (label.docParams.length === 0 && !label.docReturn) {
		return plain;
	}

	const params = label.docParams.map((tag) => `${tag.kind} ${tag.name}`);
	const returns = label.docReturn ? ` -> ${label.docReturn.kind}` : '';
	return `${prefix}${label.declaredName}(${params.join(', ')})${returns}`;
}

// The same `@param`/`@return` tags as `describeMacroTags`, but for a label used as a subroutine
// (see docs/24-Calling-Convention.md) - there is no header to reconcile against here, since
// nothing in the language declares what a label takes, so every tag the doc comment carries is
// shown exactly as written.
function describeLabelTags(label: LabelSymbol): string {
	if (label.docParams.length === 0 && !label.docReturn) {
		return '';
	}

	const rows = label.docParams.map(
		(tag, index) => `| ${index + 1} | \`${tag.name}\` | \`${tag.kind}\` | ${tag.description} |`
	);
	const table = rows.length > 0 ? ['| # | Param | Kind | |', '| --- | --- | --- | --- |', ...rows].join('\n') : '';

	const returnLine = label.docReturn
		? `**Returns:** \`${label.docReturn.kind}\`${label.docReturn.description ? ` — ${label.docReturn.description}` : ''}`
		: '';

	return [table, returnLine].filter((part) => part.length > 0).join('\n\n');
}

// A struct as the assembler lays it out: every field with the offset its name stands for, which
// is the number that actually gets written into a displacement.
function describeStruct(symbol: StructSymbol): string {
	if (symbol.fields.length === 0) {
		return '';
	}

	const rows = symbol.fields.map((field) => {
		const offset = field.offset === undefined ? '?' : `+${field.offset}`;
		return `| \`${offset}\` | \`${symbol.name}.${field.name}\` | \`${field.typeText}\` |`;
	});

	return ['| Offset | Field | Type |', '| --- | --- | --- |', ...rows].join('\n');
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

	if (token.text in LINKER_SYMBOLS) {
		return hover(token.text, 'defined by the linker', LINKER_SYMBOLS[token.text], range);
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
		case 'struct': {
			const size = resolved.symbol.size === undefined ? 'size unknown here' : `${resolved.symbol.size} bytes`;
			// The name is a constant holding the total size, which is what makes `u8[Frame]` and
			// `enter Frame` work - so the hover leads with the number, then the layout.
			return hover(
				`struct ${resolved.symbol.name}   // ${size}`,
				`struct with ${resolved.symbol.fields.length} field(s)`,
				withDoc(resolved.symbol.doc, describeStruct(resolved.symbol)),
				range
			);
		}
		case 'field': {
			const offset = resolved.field.offset === undefined
				? 'Its offset could not be worked out here.'
				: `Stands for the constant \`${resolved.field.offset}\` - the byte offset of the field, not its contents.`;
			return hover(
				`${resolved.owner.name}.${resolved.field.name}: ${resolved.field.typeText}`,
				'struct field',
				withDoc(resolved.field.doc, offset),
				range
			);
		}
		case 'const':
			return hover(`const ${resolved.symbol.name} = ${resolved.symbol.valueText}`, 'const', withDoc(resolved.symbol.doc), range);
		case 'variable': {
			const description = resolved.symbol.section ? `Declared in \`@${resolved.symbol.section}\`.` : '';
			return hover(`let ${resolved.symbol.name}: ${resolved.symbol.typeText}`, 'variable', withDoc(resolved.symbol.doc, description), range);
		}
		case 'label': {
			const code = labelSignature(resolved.symbol);
			return hover(code, `${resolved.symbol.visibility} label`, withDoc(resolved.symbol.doc, describeLabelTags(resolved.symbol)), range);
		}
		case 'macro': {
			const code = resolved.candidates.map((candidate) => macroSignature(candidate)).join('\n');
			const kindLabel = resolved.candidates.length > 1 ? `macro (${resolved.candidates.length} overloads)` : 'macro';
			return hover(code, kindLabel, withDoc(resolved.symbol.doc, describeMacroTags(resolved.symbol)), range);
		}
	}
}
