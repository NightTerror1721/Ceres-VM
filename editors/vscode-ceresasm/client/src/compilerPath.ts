// Finding the `ceres` executable from the extension host.
//
// The language server has its own resolver (server/src/compiler.ts) because it runs in a separate
// process with no VSCode API at all and has to walk ancestor directories by hand. This one has the
// API, so it can ask for the workspace folders directly - but both read the same
// `ceresAsm.compilerPath` setting and look under the same relative paths, and they must keep
// agreeing about what "the compiler" means.

import { stat } from 'fs/promises';
import * as path from 'path';
import * as vscode from 'vscode';

const CANDIDATE_RELATIVE_PATHS = [
	path.join('Ceres-ASM', 'src', 'ceres.exe'),
	path.join('Ceres-ASM', 'src', 'ceres'),
	path.join('src', 'ceres.exe'),
	path.join('src', 'ceres'),
	'ceres.exe',
	'ceres'
];

const MAX_ANCESTOR_SEARCH_DEPTH = 8;

// Every candidate that exists, newest first. Taking the first one that exists instead used to
// mean that a stale build left in Ceres-ASM/src won a workspace-root one that had just been
// rebuilt - and a compiler older than the language rejects source that is perfectly valid, with
// a message about the source rather than about itself. The newest build is the one that knows
// the most mnemonics, so it is the one to run.
async function newestExisting(candidates: string[]): Promise<string | null> {
	const found: { path: string; modifiedAt: number }[] = [];
	for (const candidate of candidates) {
		try {
			const info = await stat(candidate);
			if (info.isFile()) {
				found.push({ path: candidate, modifiedAt: info.mtimeMs });
			}
		} catch {
			// Not there, or not readable: the next candidate gets its turn.
		}
	}

	found.sort((a, b) => b.modifiedAt - a.modifiedAt);
	return found.length > 0 ? found[0].path : null;
}

function ancestorsOf(from: string): string[] {
	const ancestors: string[] = [];
	let directory = path.dirname(from);
	for (let i = 0; i < MAX_ANCESTOR_SEARCH_DEPTH; i++) {
		ancestors.push(directory);
		const parent = path.dirname(directory);
		if (parent === directory) {
			break;
		}
		directory = parent;
	}
	return ancestors;
}

export async function resolveCeresExecutable(programPath?: string): Promise<string> {
	const configured = vscode.workspace.getConfiguration('ceresAsm').get<string>('compilerPath', '').trim();
	if (configured.length > 0) {
		return configured;
	}

	const roots = (vscode.workspace.workspaceFolders ?? []).map((folder) => folder.uri.fsPath);
	// The program being debugged is a better starting point than the workspace when a single
	// `.casm` file has been opened without a folder at all.
	const searchRoots = programPath ? [...roots, ...ancestorsOf(programPath)] : roots;

	const candidates: string[] = [];
	for (const root of searchRoots) {
		for (const relative of CANDIDATE_RELATIVE_PATHS) {
			candidates.push(path.join(root, relative));
		}
	}

	const newest = await newestExisting(candidates);
	if (newest) {
		return newest;
	}

	// Nothing found; let the spawn try PATH and report ENOENT with a message that says what to do.
	return process.platform === 'win32' ? 'ceres.exe' : 'ceres';
}
