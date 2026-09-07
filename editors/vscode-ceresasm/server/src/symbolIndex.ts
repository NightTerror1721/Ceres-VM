// A lightweight, line-based scanner that indexes CASM source for editor features (hover,
// completion, go-to-definition). This is deliberately not a re-implementation of the real
// parser: it only extracts the shape of declarations (names, kinds, locations), never their
// semantics (constant-folding, type checking, encoding...) - that stays the real compiler's job,
// reached through `compiler.ts` for diagnostics.
import { readFileSync, statSync } from 'fs';
import * as path from 'path';
import { Range } from 'vscode-languageserver/node';
import { TextDocument } from 'vscode-languageserver-textdocument';
import { TextDocuments } from 'vscode-languageserver/node';
import { URI } from 'vscode-uri';

export interface ConstSymbol {
	kind: 'const';
	isGlobal: boolean;
	name: string;
	uri: string;
	range: Range;
	valueText: string;
}

export interface VariableSymbol {
	kind: 'variable';
	isGlobal: boolean;
	name: string;
	uri: string;
	range: Range;
	typeText: string;
	section: string | undefined;
}

export type LabelVisibility = 'global' | 'file' | 'local';

export interface LabelSymbol {
	kind: 'label';
	declaredName: string;
	qualifiedName: string;
	visibility: LabelVisibility;
	uri: string;
	range: Range;
}

export interface MacroSymbol {
	kind: 'macro';
	isGlobal: boolean;
	name: string;
	arity: number;
	params: string[];
	uri: string;
	range: Range;
	bodyStartLine: number;
	bodyEndLine: number;
}

export interface ImportSpec {
	importPath: string;
	range: Range;
}

export interface FileIndex {
	uri: string;
	consts: Map<string, ConstSymbol>;
	variables: Map<string, VariableSymbol>;
	labels: Map<string, LabelSymbol>; // keyed by qualifiedName
	macros: Map<string, MacroSymbol>; // keyed by `${name}/${arity}`
	imports: ImportSpec[];
	nonLocalLabelOrder: LabelSymbol[]; // ascending by line, for local-label scope resolution
}

function emptyFileIndex(uri: string): FileIndex {
	return {
		uri,
		consts: new Map(),
		variables: new Map(),
		labels: new Map(),
		macros: new Map(),
		imports: [],
		nonLocalLabelOrder: []
	};
}

function lineRange(line: number, startCharacter: number, endCharacter: number): Range {
	return Range.create(line, startCharacter, line, endCharacter);
}

// Comments are blanked out (replaced with spaces) rather than removed, so line/column offsets
// used everywhere else stay valid.
export function stripComments(text: string): string {
	let out = '';
	let inBlockComment = false;
	for (let i = 0; i < text.length; i++) {
		if (inBlockComment) {
			if (text[i] === '*' && text[i + 1] === '/') {
				out += '  ';
				i++;
				inBlockComment = false;
			} else if (text[i] === '\n') {
				out += '\n';
			} else {
				out += ' ';
			}
			continue;
		}
		if (text[i] === '/' && text[i + 1] === '*') {
			out += '  ';
			i++;
			inBlockComment = true;
			continue;
		}
		if (text[i] === '/' && text[i + 1] === '/') {
			// Blank the rest of the line.
			while (i < text.length && text[i] !== '\n') {
				out += ' ';
				i++;
			}
			i--;
			continue;
		}
		out += text[i];
	}
	return out;
}

// Blanks string literal *contents* (quotes stay, so structure and offsets are unaffected). Used
// only by the generic per-line token scanners (references, rename, semantic tokens) - never by
// buildFileIndex, which needs the real text of `const`/`let` initialisers for hover display.
export function stripStringLiterals(text: string): string {
	let out = '';
	let inString = false;
	for (let i = 0; i < text.length; i++) {
		const char = text[i];
		if (inString) {
			if (char === '\\' && text[i + 1] !== undefined && text[i + 1] !== '\n') {
				out += '  ';
				i++;
				continue;
			}
			if (char === '"' || char === '\n') {
				out += char;
				inString = false;
				continue;
			}
			out += ' ';
			continue;
		}
		if (char === '"') {
			out += char;
			inString = true;
			continue;
		}
		out += char;
	}
	return out;
}

// Comment- and string-content-blanked lines of a file, for scanners that only care about which
// positions look like real code identifiers, not the literal text of a comment or string.
export function getCleanedLines(indexer: SymbolIndexer, uri: string): string[] {
	return stripStringLiterals(stripComments(indexer.getText(uri))).split(/\r\n|\r|\n/);
}

const IMPORT_RE = /^(\s*)import\s+"([^"]*)"/;
const CONST_RE = /^(\s*)(global\s+)?const\s+([A-Za-z_][A-Za-z0-9_]*)\s*=\s*(\S.*?)\s*$/;
const LET_RE = /^(\s*)(global\s+)?let\s+([A-Za-z_][A-Za-z0-9_]*)\s*:\s*([A-Za-z_][A-Za-z0-9_]*(?:\s*\[[^\]]*\])*)/;
const SECTION_RE = /^(\s*)@(text|rodata|data|bss)\b/;
const LABEL_RE = /^(\s*)(global\s+)?(\.?[A-Za-z_][A-Za-z0-9_]*)\s*:/;
const MACRO_RE = /^(\s*)(global\s+)?macro\s+([A-Za-z_][A-Za-z0-9_]*)\b(.*)$/;
const ENDMACRO_RE = /^\s*endmacro\b/;
const MACRO_PARAM_RE = /\$[A-Za-z_][A-Za-z0-9_]*/g;

export function buildFileIndex(uri: string, text: string): FileIndex {
	const index = emptyFileIndex(uri);
	const clean = stripComments(text);
	const lines = clean.split(/\r\n|\r|\n/);

	let currentSection: string | undefined;
	let currentNonLocalLabel: string | undefined;
	let activeMacro: MacroSymbol | undefined;

	for (let lineNumber = 0; lineNumber < lines.length; lineNumber++) {
		const line = lines[lineNumber];

		const endMacroMatch = ENDMACRO_RE.exec(line);
		if (endMacroMatch && activeMacro) {
			activeMacro.bodyEndLine = lineNumber;
			activeMacro = undefined;
			continue;
		}

		const sectionMatch = SECTION_RE.exec(line);
		if (sectionMatch) {
			currentSection = sectionMatch[2];
			continue;
		}

		const importMatch = IMPORT_RE.exec(line);
		if (importMatch) {
			const [, indent, importPath] = importMatch;
			const start = indent.length;
			index.imports.push({
				importPath,
				range: lineRange(lineNumber, start, line.length)
			});
			continue;
		}

		const macroMatch = MACRO_RE.exec(line);
		if (macroMatch) {
			const [, indent, globalPrefix, name, rest] = macroMatch;
			const params = [...rest.matchAll(MACRO_PARAM_RE)].map((m) => m[0]);
			const nameStart = indent.length + (globalPrefix ? globalPrefix.length : 0) + 'macro '.length;
			const symbol: MacroSymbol = {
				kind: 'macro',
				isGlobal: Boolean(globalPrefix),
				name,
				arity: params.length,
				params,
				uri,
				range: lineRange(lineNumber, nameStart, nameStart + name.length),
				bodyStartLine: lineNumber + 1,
				bodyEndLine: lines.length - 1
			};
			index.macros.set(`${name}/${params.length}`, symbol);
			activeMacro = symbol;
			continue;
		}

		const constMatch = CONST_RE.exec(line);
		if (constMatch) {
			const [, indent, globalPrefix, name, valueText] = constMatch;
			const nameStart = indent.length + (globalPrefix ? globalPrefix.length : 0) + 'const '.length;
			index.consts.set(name, {
				kind: 'const',
				isGlobal: Boolean(globalPrefix),
				name,
				uri,
				range: lineRange(lineNumber, nameStart, nameStart + name.length),
				valueText
			});
			continue;
		}

		const letMatch = LET_RE.exec(line);
		if (letMatch) {
			const [, indent, globalPrefix, name, typeText] = letMatch;
			const nameStart = indent.length + (globalPrefix ? globalPrefix.length : 0) + 'let '.length;
			index.variables.set(name, {
				kind: 'variable',
				isGlobal: Boolean(globalPrefix),
				name,
				uri,
				range: lineRange(lineNumber, nameStart, nameStart + name.length),
				typeText: typeText.replace(/\s+/g, ''),
				section: currentSection
			});
			continue;
		}

		const labelMatch = LABEL_RE.exec(line);
		if (labelMatch) {
			const [, indent, globalPrefix, declaredName] = labelMatch;
			const isLocal = declaredName.startsWith('.');
			const nameStart = indent.length + (globalPrefix ? globalPrefix.length : 0);
			const visibility: LabelVisibility = isLocal ? 'local' : globalPrefix ? 'global' : 'file';

			let qualifiedName: string;
			if (isLocal) {
				qualifiedName = currentNonLocalLabel ? `${currentNonLocalLabel}${declaredName}` : declaredName;
			} else {
				qualifiedName = declaredName;
				currentNonLocalLabel = declaredName;
			}

			const symbol: LabelSymbol = {
				kind: 'label',
				declaredName,
				qualifiedName,
				visibility,
				uri,
				range: lineRange(lineNumber, nameStart, nameStart + declaredName.length)
			};
			index.labels.set(qualifiedName, symbol);
			if (!isLocal) {
				index.nonLocalLabelOrder.push(symbol);
			}
			continue;
		}
	}

	return index;
}

export function findEnclosingNonLocalLabel(index: FileIndex, line: number): LabelSymbol | undefined {
	let candidate: LabelSymbol | undefined;
	for (const label of index.nonLocalLabelOrder) {
		if (label.range.start.line <= line) {
			candidate = label;
		} else {
			break;
		}
	}
	return candidate;
}

export function findEnclosingMacro(index: FileIndex, line: number): MacroSymbol | undefined {
	for (const macro of index.macros.values()) {
		if (line >= macro.bodyStartLine && line <= macro.bodyEndLine) {
			return macro;
		}
	}
	return undefined;
}

export function resolveImportPath(fromUri: string, importPath: string): string {
	const fromFsPath = URI.parse(fromUri).fsPath;
	return path.resolve(path.dirname(fromFsPath), importPath);
}

export interface VisibleSymbols {
	file: FileIndex;
	consts: Map<string, ConstSymbol>;
	variables: Map<string, VariableSymbol>;
	labels: Map<string, LabelSymbol>; // non-local, by name
	macros: Map<string, MacroSymbol>; // by `${name}/${arity}`
	macrosByName: Map<string, MacroSymbol[]>;
	filesVisited: string[]; // uri, includes the requested file itself
}

export class SymbolIndexer {
	private readonly fileCache = new Map<string, { mtimeMs: number; index: FileIndex }>();

	constructor(private readonly documents: TextDocuments<TextDocument>) {}

	getFileIndex(uri: string): FileIndex {
		const openDocument = this.documents.get(uri);
		if (openDocument) {
			// Always rebuilt from the live buffer: cheap regex scan, and must reflect unsaved edits.
			return buildFileIndex(uri, openDocument.getText());
		}

		let fsPath: string;
		try {
			fsPath = URI.parse(uri).fsPath;
		} catch {
			return emptyFileIndex(uri);
		}

		try {
			const stat = statSync(fsPath);
			const cached = this.fileCache.get(fsPath);
			if (cached && cached.mtimeMs === stat.mtimeMs) {
				return cached.index;
			}
			const text = readFileSync(fsPath, 'utf8');
			const index = buildFileIndex(uri, text);
			this.fileCache.set(fsPath, { mtimeMs: stat.mtimeMs, index });
			return index;
		} catch {
			return emptyFileIndex(uri);
		}
	}

	// What an `import` makes visible: whatever the imported file declares `global`, transitively and
	// cycle-safe. The requested file itself contributes everything it declares, global or not.
	//
	// Labels come along too. They are not imported the way a constant is - the linker publishes
	// every global label - but without them a `call` into another file resolves to nothing and the
	// grammar's fallback paints it as a variable.
	collectVisibleSymbols(uri: string): VisibleSymbols {
		const consts = new Map<string, ConstSymbol>();
		const variables = new Map<string, VariableSymbol>();
		const labels = new Map<string, LabelSymbol>();
		const macros = new Map<string, MacroSymbol>();
		const visited = new Set<string>();
		const file = this.getFileIndex(uri);

		const visit = (currentUri: string, currentIndex: FileIndex): void => {
			if (visited.has(currentUri)) {
				return;
			}
			visited.add(currentUri);

			const isOwnFile = currentUri === uri;

			for (const [name, symbol] of currentIndex.consts) {
				if ((isOwnFile || symbol.isGlobal) && !consts.has(name)) {
					consts.set(name, symbol);
				}
			}
			for (const [name, symbol] of currentIndex.variables) {
				if ((isOwnFile || symbol.isGlobal) && !variables.has(name)) {
					variables.set(name, symbol);
				}
			}
			for (const symbol of currentIndex.nonLocalLabelOrder) {
				if ((isOwnFile || symbol.visibility === 'global') && !labels.has(symbol.qualifiedName)) {
					labels.set(symbol.qualifiedName, symbol);
				}
			}
			for (const [key, symbol] of currentIndex.macros) {
				if ((isOwnFile || symbol.isGlobal) && !macros.has(key)) {
					macros.set(key, symbol);
				}
			}
			for (const importSpec of currentIndex.imports) {
				const importedFsPath = resolveImportPath(currentUri, importSpec.importPath);
				const importedUri = URI.file(importedFsPath).toString();
				if (visited.has(importedUri)) {
					continue;
				}
				visit(importedUri, this.getFileIndex(importedUri));
			}
		};

		visit(uri, file);

		const macrosByName = new Map<string, MacroSymbol[]>();
		for (const symbol of macros.values()) {
			const list = macrosByName.get(symbol.name) ?? [];
			list.push(symbol);
			macrosByName.set(symbol.name, list);
		}

		return { file, consts, variables, labels, macros, macrosByName, filesVisited: [...visited] };
	}

	// Live buffer text for an open document, otherwise the file's content on disk (empty string
	// if it can't be read). Used by callers that need the raw lines of a file the index already
	// knows about (references, rename), not just its parsed symbols.
	getText(uri: string): string {
		const openDocument = this.documents.get(uri);
		if (openDocument) {
			return openDocument.getText();
		}
		try {
			return readFileSync(URI.parse(uri).fsPath, 'utf8');
		} catch {
			return '';
		}
	}
}

// Length of a token's leading sigil ('.', '$', '%%'), if any - used to compute the "bare name"
// sub-range shared by references, rename and semantic tokens.
export function sigilLength(tokenText: string): number {
	if (tokenText.startsWith('%%')) return 2;
	if (tokenText.startsWith('.') || tokenText.startsWith('$') || tokenText.startsWith('@')) return 1;
	return 0;
}

export interface TokenAtPosition {
	text: string;
	startCharacter: number;
	endCharacter: number;
}

const TOKEN_RE = /\$[A-Za-z_][A-Za-z0-9_]*|%%[A-Za-z_][A-Za-z0-9_]*|@[A-Za-z_][A-Za-z0-9_]*|\.[A-Za-z_][A-Za-z0-9_]*|[A-Za-z_][A-Za-z0-9_]*/g;

export function getTokenAtCharacter(lineText: string, character: number): TokenAtPosition | null {
	TOKEN_RE.lastIndex = 0;
	let match: RegExpExecArray | null;
	while ((match = TOKEN_RE.exec(lineText))) {
		const start = match.index;
		const end = start + match[0].length;
		if (character >= start && character <= end) {
			return { text: match[0], startCharacter: start, endCharacter: end };
		}
		if (start > character) {
			break;
		}
	}
	return null;
}

// Every token on a line, in order - used where a caller needs to scan a whole line (references,
// rename, semantic tokens) rather than resolve one position (hover, definition, completion).
export function getAllTokens(lineText: string): TokenAtPosition[] {
	TOKEN_RE.lastIndex = 0;
	const tokens: TokenAtPosition[] = [];
	let match: RegExpExecArray | null;
	while ((match = TOKEN_RE.exec(lineText))) {
		tokens.push({ text: match[0], startCharacter: match.index, endCharacter: match.index + match[0].length });
	}
	return tokens;
}

// Splits a macro/instruction call's argument text on top-level commas (bracket-aware, so
// `[r5 + 4]` doesn't get split), to guess the argument count for macro-arity resolution.
export function splitTopLevelArguments(text: string): string[] {
	const args: string[] = [];
	let depth = 0;
	let current = '';
	for (const char of text) {
		if (char === '[') {
			depth++;
		} else if (char === ']') {
			depth = Math.max(0, depth - 1);
		}
		if (char === ',' && depth === 0) {
			args.push(current.trim());
			current = '';
			continue;
		}
		current += char;
	}
	if (current.trim().length > 0) {
		args.push(current.trim());
	}
	return args;
}
