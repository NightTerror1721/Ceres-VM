import { Position, Range, TextEdit, WorkspaceEdit } from 'vscode-languageserver/node';
import { TextDocument } from 'vscode-languageserver-textdocument';
import { isReservedWord } from './languageData';
import { findReferences } from './references';
import { getCleanedLines, getTokenAtCharacter, SymbolIndexer } from './symbolIndex';

const VALID_NAME_RE = /^[A-Za-z_][A-Za-z0-9_]*$/;

export interface PrepareRenameResult {
	range: Range;
	placeholder: string;
}

// The returned range excludes any sigil ('.', '$', '%%') so the rename input box only ever shows
// the editable bare name - typing over it can't accidentally delete the sigil.
export function providePrepareRename(document: TextDocument, position: Position, indexer: SymbolIndexer): PrepareRenameResult | null {
	const lineText = getCleanedLines(indexer, document.uri)[position.line] ?? '';
	const token = getTokenAtCharacter(lineText, position.character);
	if (!token || isReservedWord(token.text) || token.text.startsWith('@')) {
		return null;
	}
	const offset = token.text.startsWith('%%') ? 2 : token.text.startsWith('.') || token.text.startsWith('$') ? 1 : 0;
	return {
		range: Range.create(position.line, token.startCharacter + offset, position.line, token.endCharacter),
		placeholder: token.text.slice(offset)
	};
}

export function provideRenameEdits(
	document: TextDocument,
	position: Position,
	newName: string,
	indexer: SymbolIndexer
): WorkspaceEdit | null {
	if (!VALID_NAME_RE.test(newName)) {
		return null;
	}
	if (!providePrepareRename(document, position, indexer)) {
		return null;
	}

	const references = findReferences(document, position, indexer, true);
	if (references.length === 0) {
		return null;
	}

	const changes: Record<string, TextEdit[]> = {};
	for (const reference of references) {
		(changes[reference.uri] ??= []).push(TextEdit.replace(reference.range, newName));
	}

	return { changes };
}
