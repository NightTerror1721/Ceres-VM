import { Position, Range } from 'vscode-languageserver/node';
import { TextDocument } from 'vscode-languageserver-textdocument';
import { isReservedWord } from './languageData';
import { resolveUserSymbol } from './resolution';
import {
	findEnclosingMacro,
	findEnclosingNonLocalLabel,
	getAllTokens,
	getCleanedLines,
	getTokenAtCharacter,
	qualifierBefore,
	sigilLength,
	SymbolIndexer,
	TokenAtPosition
} from './symbolIndex';

export interface NameReference {
	uri: string;
	// Covers only the bare name - the sigil ('.', '$', '%%'), if any, is excluded, so this range
	// can be replaced directly with a new name during rename without re-adding it.
	range: Range;
	isDeclaration: boolean;
}

function bareRange(line: number, token: TokenAtPosition): Range {
	const offset = sigilLength(token.text);
	return Range.create(line, token.startCharacter + offset, line, token.endCharacter);
}

function scanFileFor(
	indexer: SymbolIndexer,
	uri: string,
	matchText: string,
	declarationKeys: Set<string>,
	accept?: (lineText: string, line: number, token: TokenAtPosition) => boolean
): NameReference[] {
	const lines = getCleanedLines(indexer, uri);
	const results: NameReference[] = [];
	for (let line = 0; line < lines.length; line++) {
		for (const candidate of getAllTokens(lines[line])) {
			if (candidate.text !== matchText) {
				continue;
			}
			if (accept && !accept(lines[line], line, candidate)) {
				continue;
			}
			const range = bareRange(line, candidate);
			const key = `${uri}:${range.start.line}:${range.start.character}`;
			results.push({ uri, range, isDeclaration: declarationKeys.has(key) });
		}
	}
	return results;
}

function declarationKey(uri: string, range: Range, sigilChars: number): string {
	return `${uri}:${range.start.line}:${range.start.character + sigilChars}`;
}

export function findReferences(
	document: TextDocument,
	position: Position,
	indexer: SymbolIndexer,
	includeDeclaration: boolean
): NameReference[] {
	const lineText = getCleanedLines(indexer, document.uri)[position.line] ?? '';
	const token = getTokenAtCharacter(lineText, position.character);
	if (!token || isReservedWord(token.text)) {
		return [];
	}

	// Macro parameters and hygienic labels never leave the enclosing macro body, and never
	// collide with same-named ones in a different macro - resolved without touching other files.
	if (token.text.startsWith('$') || token.text.startsWith('%%')) {
		const file = indexer.getFileIndex(document.uri);
		const macro = findEnclosingMacro(file, position.line);
		if (!macro || (token.text.startsWith('$') && !macro.params.includes(token.text))) {
			return [];
		}
		const lines = getCleanedLines(indexer, document.uri);
		const results: NameReference[] = [];
		const declarationLine = macro.range.start.line;
		for (let line = macro.range.start.line; line <= macro.bodyEndLine && line < lines.length; line++) {
			for (const candidate of getAllTokens(lines[line])) {
				if (candidate.text !== token.text) {
					continue;
				}
				const isDeclaration = token.text.startsWith('$') && line === declarationLine;
				results.push({ uri: document.uri, range: bareRange(line, candidate), isDeclaration });
			}
		}
		return includeDeclaration ? results : results.filter((r) => !r.isDeclaration);
	}

	const resolved = resolveUserSymbol(indexer, document.uri, position, token, lineText);
	if (!resolved) {
		return [];
	}

	// Everything an import can reach, plus wherever the declaration itself lives. A `global`
	// anything is used from other files by definition, so searching only its own file would
	// answer "one use" for a routine the whole program calls.
	const searchFilesFor = (declaringUri: string): string[] => {
		const { filesVisited } = indexer.collectVisibleSymbols(document.uri);
		const files = new Set(filesVisited);
		files.add(declaringUri);
		files.add(document.uri);
		return [...files];
	};

	let results: NameReference[];

	if (resolved.kind === 'variable') {
		const declKey = declarationKey(resolved.symbol.uri, resolved.symbol.range, 0);
		const files = resolved.symbol.isGlobal ? searchFilesFor(resolved.symbol.uri) : [resolved.symbol.uri];
		results = files.flatMap((uri) => scanFileFor(indexer, uri, resolved.symbol.name, new Set([declKey])));
	} else if (resolved.kind === 'label') {
		const declKey = declarationKey(resolved.symbol.uri, resolved.symbol.range, resolved.symbol.visibility === 'local' ? 1 : 0);
		const targetQualifiedName = resolved.symbol.qualifiedName;
		const isLocal = resolved.symbol.visibility === 'local';
		// A local label means a different thing under each parent, so it is matched by the
		// qualified name rather than by the two characters on the line.
		const accept = isLocal
			? (_lineText: string, line: number, candidate: TokenAtPosition): boolean => {
					const file = indexer.getFileIndex(resolved.symbol.uri);
					const parent = findEnclosingNonLocalLabel(file, line);
					const qualifiedName = parent ? `${parent.declaredName}${candidate.text}` : candidate.text;
					return qualifiedName === targetQualifiedName;
				}
			: undefined;
		const files = resolved.symbol.visibility === 'global' ? searchFilesFor(resolved.symbol.uri) : [resolved.symbol.uri];
		results = files.flatMap((uri) =>
			scanFileFor(indexer, uri, resolved.symbol.declaredName, new Set([declKey]), accept)
		);
	} else if (resolved.kind === 'const') {
		const declKey = declarationKey(resolved.symbol.uri, resolved.symbol.range, 0);
		results = searchFilesFor(resolved.symbol.uri).flatMap((uri) =>
			scanFileFor(indexer, uri, resolved.symbol.name, new Set([declKey]))
		);
	} else if (resolved.kind === 'struct') {
		const declKey = declarationKey(resolved.symbol.uri, resolved.symbol.range, 0);
		results = searchFilesFor(resolved.symbol.uri).flatMap((uri) =>
			scanFileFor(indexer, uri, resolved.symbol.name, new Set([declKey]))
		);
	} else if (resolved.kind === 'field') {
		// `.count` on its own is a local label somewhere else in the same file, so a field is only
		// a use of this field where its own struct is written immediately before the dot.
		const owner = resolved.owner.name;
		const declKey = declarationKey(resolved.field.uri, resolved.field.range, 0);
		results = searchFilesFor(resolved.field.uri).flatMap((uri) =>
			scanFileFor(indexer, uri, `.${resolved.field.name}`, new Set([declKey]), (text, _line, candidate) =>
				qualifierBefore(text, candidate) === owner
			)
		);
		// The declaration inside the struct body is written `count: u32`, which is not a `.count`
		// token at all, so it is added by hand.
		if (includeDeclaration) {
			results.unshift({ uri: resolved.field.uri, range: resolved.field.range, isDeclaration: true });
		}
	} else {
		// macro: every arity sharing this name is treated as one renameable family.
		const declKeys = new Set(resolved.candidates.map((candidate) => declarationKey(candidate.uri, candidate.range, 0)));
		const files = new Set(searchFilesFor(resolved.symbol.uri));
		for (const candidate of resolved.candidates) {
			files.add(candidate.uri);
		}
		results = [...files].flatMap((uri) => scanFileFor(indexer, uri, resolved.symbol.name, declKeys));
	}

	return includeDeclaration ? results : results.filter((r) => !r.isDeclaration);
}
