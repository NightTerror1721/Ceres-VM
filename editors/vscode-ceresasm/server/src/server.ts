import { unlink, writeFile } from 'fs/promises';
import * as path from 'path';
import {
	CompletionItem,
	CompletionParams,
	createConnection,
	Definition,
	Diagnostic,
	DiagnosticSeverity,
	DidChangeConfigurationNotification,
	FoldingRange,
	FoldingRangeParams,
	Hover,
	HoverParams,
	InitializeParams,
	InlayHint,
	InlayHintParams,
	InitializeResult,
	Location,
	PrepareRenameParams,
	ProposedFeatures,
	Range,
	ReferenceParams,
	RenameParams,
	SemanticTokens,
	SemanticTokensParams,
	TextDocumentPositionParams,
	TextDocumentSyncKind,
	TextDocuments,
	WorkspaceEdit
} from 'vscode-languageserver/node';
import { TextDocument } from 'vscode-languageserver-textdocument';
import { URI } from 'vscode-uri';

import { checkFile, CompilerNotFoundError, resolveCompilerPath } from './compiler';
import { provideCompletion } from './completion';
import { provideDefinition } from './definition';
import { provideFoldingRanges } from './folding';
import { provideHover } from './hover';
import { DEFAULT_INLAY_HINT_SETTINGS, InlayHintSettings, provideInlayHints } from './inlayHints';
import { findReferences } from './references';
import { providePrepareRename, provideRenameEdits } from './rename';
import { TOKEN_MODIFIERS, TOKEN_TYPES, provideSemanticTokens } from './semanticTokens';
import { getCleanedLines, SymbolIndexer } from './symbolIndex';

const connection = createConnection(ProposedFeatures.all);
const documents = new TextDocuments(TextDocument);
const indexer = new SymbolIndexer(documents);

// Last-resort safety net: Node terminates the process on an unhandled rejection by default, which
// would otherwise take the whole language server down over a single failed validation. Every
// known failure path below is already caught; this only catches what isn't.
process.on('unhandledRejection', (reason) => {
	const message = reason instanceof Error ? (reason.stack ?? reason.message) : String(reason);
	connection.console.error(`Unhandled rejection: ${message}`);
});
process.on('uncaughtException', (error) => {
	connection.console.error(`Uncaught exception: ${error.stack ?? error.message}`);
});

let hasConfigurationCapability = false;

// Fetched once and kept until the user changes something, because a hint request arrives for
// every visible range of every scroll and a round trip per request would be felt.
let cachedInlayHintSettings: InlayHintSettings | undefined;
let hasWorkspaceFolderCapability = false;
let workspaceRoots: string[] = [];
let warnedAboutMissingCompiler = false;

const debounceTimers = new Map<string, ReturnType<typeof setTimeout>>();
const VALIDATION_DEBOUNCE_MS = 300;

// Diagnostics for a file reached through `import` are published against *that file's own* URI,
// not the root document's - so this tracks, per root document, which other URIs it last published
// to, and clears any that no longer have anything to report (sendDiagnostics only replaces the
// set for the URI you call it with; it won't clear an unrelated one on its own).
const secondaryDiagnosticUris = new Map<string, Set<string>>();

connection.onInitialize((params: InitializeParams): InitializeResult => {
	const capabilities = params.capabilities;
	hasConfigurationCapability = Boolean(capabilities.workspace?.configuration);
	hasWorkspaceFolderCapability = Boolean(capabilities.workspace?.workspaceFolders);

	if (params.workspaceFolders && params.workspaceFolders.length > 0) {
		workspaceRoots = params.workspaceFolders.map((folder) => URI.parse(folder.uri).fsPath);
	} else if (params.rootUri) {
		workspaceRoots = [URI.parse(params.rootUri).fsPath];
	}

	return {
		capabilities: {
			textDocumentSync: TextDocumentSyncKind.Incremental,
			hoverProvider: true,
			definitionProvider: true,
			referencesProvider: true,
			renameProvider: { prepareProvider: true },
			foldingRangeProvider: true,
			inlayHintProvider: { resolveProvider: false },
			completionProvider: {
				triggerCharacters: ['.', '$', '%', '@']
			},
			semanticTokensProvider: {
				legend: { tokenTypes: [...TOKEN_TYPES], tokenModifiers: [...TOKEN_MODIFIERS] },
				full: true
			}
		}
	};
});

connection.onInitialized(() => {
	if (hasConfigurationCapability) {
		connection.client
			.register(DidChangeConfigurationNotification.type, undefined)
			.catch((error: unknown) => connection.console.error(`Failed to register for configuration changes: ${String(error)}`));
	}
	if (hasWorkspaceFolderCapability) {
		connection.workspace.onDidChangeWorkspaceFolders(() => {
			connection.workspace
				.getWorkspaceFolders()
				.then((folders) => {
					workspaceRoots = (folders ?? []).map((folder) => URI.parse(folder.uri).fsPath);
				})
				.catch((error: unknown) => connection.console.error(`Failed to refresh workspace folders: ${String(error)}`));
		});
	}
});

async function getCompilerPath(documentPath: string): Promise<string> {
	let configuredPath = '';
	if (hasConfigurationCapability) {
		const settings = await connection.workspace.getConfiguration({ section: 'ceresAsm' });
		configuredPath = typeof settings?.compilerPath === 'string' ? settings.compilerPath : '';
	}
	return resolveCompilerPath(configuredPath, workspaceRoots, documentPath);
}

// A missing or malformed setting falls back to the default rather than turning the feature off:
// the answer to "I do not know what you want" is what everyone else gets.
async function getInlayHintSettings(): Promise<InlayHintSettings> {
	if (cachedInlayHintSettings) {
		return cachedInlayHintSettings;
	}
	if (!hasConfigurationCapability) {
		cachedInlayHintSettings = DEFAULT_INLAY_HINT_SETTINGS;
		return cachedInlayHintSettings;
	}

	try {
		const settings = await connection.workspace.getConfiguration({ section: 'ceresAsm.inlayHints' });
		const read = (key: keyof InlayHintSettings): boolean =>
			typeof settings?.[key] === 'boolean' ? (settings[key] as boolean) : DEFAULT_INLAY_HINT_SETTINGS[key];

		cachedInlayHintSettings = {
			enabled: read('enabled'),
			variableTypes: read('variableTypes'),
			constantValues: read('constantValues'),
			structOffsets: read('structOffsets'),
			macroParameterNames: read('macroParameterNames'),
			portNames: read('portNames'),
			registerAliases: read('registerAliases')
		};
	} catch {
		cachedInlayHintSettings = DEFAULT_INLAY_HINT_SETTINGS;
	}

	return cachedInlayHintSettings ?? DEFAULT_INLAY_HINT_SETTINGS;
}

connection.onDidChangeConfiguration(() => {
	cachedInlayHintSettings = undefined;
	// Hints already drawn were drawn under the old settings, so they have to be asked for again -
	// nothing else would make a toggle take effect until the file is scrolled or edited.
	connection.languages.inlayHint.refresh().catch((error: unknown) => {
		connection.console.error(`Failed to refresh inlay hints: ${String(error)}`);
	});

	for (const document of documents.all()) {
		scheduleValidation(document);
	}
});

// The whole statement, not the one character the compiler pointed at. A column is precise and
// nearly invisible: a single-character squiggle under a 40-column line reads as a speck, and the
// column an assembler reports is where it *noticed* the problem rather than where the problem is
// - an operand that does not fit is reported at the operand, and what is wrong is the
// instruction. The indentation and the trailing spaces are left out, so the mark covers the code
// and nothing else.
function diagnosticRange(uri: string, line: number, column: number): Range {
	const lines = getCleanedLines(indexer, uri);
	const lineText = lines[line];

	if (lineText !== undefined) {
		const start = lineText.length - lineText.trimStart().length;
		const end = lineText.trimEnd().length;
		if (end > start) {
			return { start: { line, character: start }, end: { line, character: end } };
		}
	}

	// A line that is blank, or one from a file that could not be read: fall back to the column,
	// which is the only thing left that is true.
	return { start: { line, character: column }, end: { line, character: column + 1 } };
}

function scheduleValidation(document: TextDocument): void {
	const existing = debounceTimers.get(document.uri);
	if (existing) {
		clearTimeout(existing);
	}
	debounceTimers.set(
		document.uri,
		setTimeout(() => {
			debounceTimers.delete(document.uri);
			validateDocument(document).catch((error: unknown) => {
				connection.console.error(
					`Validation of ${document.uri} failed: ${error instanceof Error ? error.message : String(error)}`
				);
			});
		}, VALIDATION_DEBOUNCE_MS)
	);
}

// The compiler is checked against a sibling temp file, not the buffer's on-disk path, so unsaved
// edits are validated too. Writing it next to the original (rather than to the system temp dir)
// keeps relative `import "..."` paths resolving the same way the real assembly would.
async function validateDocument(document: TextDocument): Promise<void> {
	const uri = URI.parse(document.uri);
	if (uri.scheme !== 'file') {
		return;
	}

	const filePath = uri.fsPath;
	const tempPath = path.join(
		path.dirname(filePath),
		`.${path.basename(filePath)}.ceresasm-check-${process.pid}.casm`
	);

	// Diagnostics grouped by target URI: entries for the root file itself (file === tempPath, or
	// empty for one not tied to any file) go to the document being edited; entries for anything
	// else - a file reached through `import` - go to that file's own URI instead.
	const diagnosticsByUri = new Map<string, Diagnostic[]>();
	const getBucket = (targetUri: string): Diagnostic[] => {
		let bucket = diagnosticsByUri.get(targetUri);
		if (!bucket) {
			bucket = [];
			diagnosticsByUri.set(targetUri, bucket);
		}
		return bucket;
	};
	getBucket(document.uri); // Always publish for the document itself, even if it's just to clear it.

	try {
		const compilerPath = await getCompilerPath(filePath);

		await writeFile(tempPath, document.getText(), 'utf8');
		try {
			const results = await checkFile(compilerPath, tempPath);
			for (const entry of results) {
				const line = Math.max(0, entry.line - 1);
				const character = Math.max(0, entry.column - 1);
				const targetUri = !entry.file || entry.file === tempPath ? document.uri : URI.file(entry.file).toString();
				getBucket(targetUri).push({
					severity: entry.severity === 'warning' ? DiagnosticSeverity.Warning : DiagnosticSeverity.Error,
					// Measured against the document being edited when the entry belongs to it, and
					// against the imported file's own text when it does not - the two are different
					// files with different lines.
					range: diagnosticRange(targetUri, line, character),
					message: entry.message,
					source: 'ceresasm'
				});
			}
			warnedAboutMissingCompiler = false;
		} finally {
			await unlink(tempPath).catch(() => undefined);
		}
	} catch (error) {
		// However this fails, it must show up *in the document* (Problems panel + squiggle), not
		// only as a notification toast: a toast is easy to miss or dismiss, and once missed there
		// would otherwise be no other sign that a file has silently stopped being checked at all.
		const notCheckedMessage =
			error instanceof CompilerNotFoundError
				? "CeresASM: couldn't find the 'ceres' compiler, so this file hasn't been checked. Build it " +
					"with `cmake --build --preset msvc-debug` in Ceres/ (see the extension README), or set 'ceresAsm.compilerPath' in your settings."
				: `CeresASM: this file hasn't been checked - ${(error as Error).message}`;
		getBucket(document.uri).push({
			severity: DiagnosticSeverity.Warning,
			range: diagnosticRange(document.uri, 0, 0),
			message: notCheckedMessage,
			source: 'ceresasm'
		});

		if (error instanceof CompilerNotFoundError) {
			if (!warnedAboutMissingCompiler) {
				warnedAboutMissingCompiler = true;
				void connection.window.showWarningMessage(notCheckedMessage);
			}
		} else {
			void connection.window.showErrorMessage(notCheckedMessage);
		}
	}

	// Clear diagnostics on any secondary (imported) file that had some last time but has none now.
	const previousSecondaryUris = secondaryDiagnosticUris.get(document.uri);
	if (previousSecondaryUris) {
		for (const uri of previousSecondaryUris) {
			if (!diagnosticsByUri.has(uri)) {
				diagnosticsByUri.set(uri, []);
			}
		}
	}
	secondaryDiagnosticUris.set(document.uri, new Set([...diagnosticsByUri.keys()].filter((uri) => uri !== document.uri)));

	for (const [targetUri, targetDiagnostics] of diagnosticsByUri) {
		try {
			connection.sendDiagnostics({ uri: targetUri, diagnostics: targetDiagnostics });
		} catch (error) {
			connection.console.error(`Failed to publish diagnostics for ${targetUri}: ${(error as Error).message}`);
		}
	}
}

documents.onDidOpen((event) => scheduleValidation(event.document));
documents.onDidChangeContent((event) => scheduleValidation(event.document));
documents.onDidSave((event) => scheduleValidation(event.document));
documents.onDidClose((event) => {
	const timer = debounceTimers.get(event.document.uri);
	if (timer) {
		clearTimeout(timer);
		debounceTimers.delete(event.document.uri);
	}

	const urisToClear = [event.document.uri, ...(secondaryDiagnosticUris.get(event.document.uri) ?? [])];
	secondaryDiagnosticUris.delete(event.document.uri);

	for (const uri of urisToClear) {
		try {
			connection.sendDiagnostics({ uri, diagnostics: [] });
		} catch (error) {
			connection.console.error(`Failed to clear diagnostics for ${uri}: ${(error as Error).message}`);
		}
	}
});

connection.onHover((params: HoverParams): Hover | null => {
	try {
		const document = documents.get(params.textDocument.uri);
		if (!document) {
			return null;
		}
		return provideHover(document, params.position, indexer);
	} catch (error) {
		connection.console.error(`Hover request failed: ${(error as Error).message}`);
		return null;
	}
});

connection.languages.inlayHint.on(async (params: InlayHintParams): Promise<InlayHint[]> => {
	try {
		const document = documents.get(params.textDocument.uri);
		if (!document) {
			return [];
		}
		return provideInlayHints(document, params.range, indexer, await getInlayHintSettings());
	} catch (error) {
		connection.console.error(`Inlay hint request failed: ${(error as Error).message}`);
		return [];
	}
});

connection.onDefinition((params: TextDocumentPositionParams): Definition | null => {
	try {
		const document = documents.get(params.textDocument.uri);
		if (!document) {
			return null;
		}
		return provideDefinition(document, params.position, indexer);
	} catch (error) {
		connection.console.error(`Definition request failed: ${(error as Error).message}`);
		return null;
	}
});

connection.onCompletion((params: CompletionParams): CompletionItem[] => {
	try {
		const document = documents.get(params.textDocument.uri);
		if (!document) {
			return [];
		}
		return provideCompletion(document, params.position, indexer);
	} catch (error) {
		connection.console.error(`Completion request failed: ${(error as Error).message}`);
		return [];
	}
});

connection.onReferences((params: ReferenceParams): Location[] => {
	try {
		const document = documents.get(params.textDocument.uri);
		if (!document) {
			return [];
		}
		const references = findReferences(document, params.position, indexer, params.context.includeDeclaration);
		return references.map((reference) => Location.create(reference.uri, reference.range));
	} catch (error) {
		connection.console.error(`References request failed: ${(error as Error).message}`);
		return [];
	}
});

connection.onPrepareRename((params: PrepareRenameParams): Range | null => {
	try {
		const document = documents.get(params.textDocument.uri);
		if (!document) {
			return null;
		}
		return providePrepareRename(document, params.position, indexer)?.range ?? null;
	} catch (error) {
		connection.console.error(`Prepare rename request failed: ${(error as Error).message}`);
		return null;
	}
});

connection.onRenameRequest((params: RenameParams): WorkspaceEdit | null => {
	try {
		const document = documents.get(params.textDocument.uri);
		if (!document) {
			return null;
		}
		const edit = provideRenameEdits(document, params.position, params.newName, indexer);
		const fileCount = edit?.changes ? Object.keys(edit.changes).length : 0;
		if (fileCount > 1) {
			void connection.window.showInformationMessage(
				`CeresASM: renamed across ${fileCount} files reachable through import. Files that import the same ` +
					"symbol from elsewhere in the workspace aren't discovered automatically - check them manually."
			);
		}
		return edit;
	} catch (error) {
		connection.console.error(`Rename request failed: ${(error as Error).message}`);
		return null;
	}
});

connection.onFoldingRanges((params: FoldingRangeParams): FoldingRange[] => {
	try {
		const document = documents.get(params.textDocument.uri);
		if (!document) {
			return [];
		}
		return provideFoldingRanges(document);
	} catch (error) {
		connection.console.error(`Folding range request failed: ${(error as Error).message}`);
		return [];
	}
});

connection.languages.semanticTokens.on((params: SemanticTokensParams): SemanticTokens => {
	try {
		const document = documents.get(params.textDocument.uri);
		if (!document) {
			return { data: [] };
		}
		return provideSemanticTokens(document, indexer);
	} catch (error) {
		connection.console.error(`Semantic tokens request failed: ${(error as Error).message}`);
		return { data: [] };
	}
});

documents.listen(connection);
connection.listen();
