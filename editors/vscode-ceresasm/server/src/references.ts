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
	filterQualified?: (line: number, matchText: string) => boolean
): NameReference[] {
	const lines = getCleanedLines(indexer, uri);
	const results: NameReference[] = [];
	for (let line = 0; line < lines.length; line++) {
		for (const candidate of getAllTokens(lines[line])) {
			if (candidate.text !== matchText) {
				continue;
			}
			if (filterQualified && !filterQualified(line, matchText)) {
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

	let results: NameReference[];

	if (resolved.kind === 'variable') {
		const declKey = declarationKey(resolved.symbol.uri, resolved.symbol.range, 0);
		results = scanFileFor(indexer, resolved.symbol.uri, resolved.symbol.name, new Set([declKey]));
	} else if (resolved.kind === 'label') {
		const declKey = declarationKey(resolved.symbol.uri, resolved.symbol.range, resolved.symbol.visibility === 'local' ? 1 : 0);
		const targetQualifiedName = resolved.symbol.qualifiedName;
		const isLocal = resolved.symbol.visibility === 'local';
		results = scanFileFor(
			indexer,
			resolved.symbol.uri,
			resolved.symbol.declaredName,
			new Set([declKey]),
			isLocal
				? (line, name) => {
						const file = indexer.getFileIndex(resolved.symbol.uri);
						const parent = findEnclosingNonLocalLabel(file, line);
						const qualifiedName = parent ? `${parent.declaredName}${name}` : name;
						return qualifiedName === targetQualifiedName;
					}
				: undefined
		);
	} else if (resolved.kind === 'const') {
		const declKey = declarationKey(resolved.symbol.uri, resolved.symbol.range, 0);
		const { filesVisited } = indexer.collectVisibleSymbols(document.uri);
		const searchFiles = new Set(filesVisited);
		searchFiles.add(resolved.symbol.uri);
		results = [...searchFiles].flatMap((uri) => scanFileFor(indexer, uri, resolved.symbol.name, new Set([declKey])));
	} else {
		// macro: every arity sharing this name is treated as one renameable family.
		const declKeys = new Set(resolved.candidates.map((candidate) => declarationKey(candidate.uri, candidate.range, 0)));
		const { filesVisited } = indexer.collectVisibleSymbols(document.uri);
		const searchFiles = new Set(filesVisited);
		for (const candidate of resolved.candidates) {
			searchFiles.add(candidate.uri);
		}
		results = [...searchFiles].flatMap((uri) => scanFileFor(indexer, uri, resolved.symbol.name, declKeys));
	}

	return includeDeclaration ? results : results.filter((r) => !r.isDeclaration);
}
