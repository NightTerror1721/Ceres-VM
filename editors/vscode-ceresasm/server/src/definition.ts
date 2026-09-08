import { Location, Position, Range } from 'vscode-languageserver/node';
import { TextDocument } from 'vscode-languageserver-textdocument';
import { isReservedWord } from './languageData';
import { definitionLocationOf, resolveUserSymbol } from './resolution';
import { getCleanedLines, getTokenAtCharacter, SymbolIndexer } from './symbolIndex';

export function provideDefinition(document: TextDocument, position: Position, indexer: SymbolIndexer): Location | null {
	const lineText = getCleanedLines(indexer, document.uri)[position.line] ?? '';

	const token = getTokenAtCharacter(lineText, position.character);
	if (!token) {
		return null;
	}

	// Reserved words, and sigil-only forms with no symbol-index entry, have no definition to jump to.
	if (isReservedWord(token.text) || token.text.startsWith('$') || token.text.startsWith('%%') || token.text.startsWith('@')) {
		return null;
	}

	const resolved = resolveUserSymbol(indexer, document.uri, position, token, lineText);
	if (!resolved) {
		return null;
	}

	const target = definitionLocationOf(resolved);
	return Location.create(target.uri, target.range);
}
