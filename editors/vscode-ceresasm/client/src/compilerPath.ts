// Finding the `ceres` executable from the extension host.
//
// The language server has its own resolver (server/src/compiler.ts) because it runs in a separate
// process with no VSCode API at all and has to walk ancestor directories by hand. This one has the
// API, so it can ask for the workspace folders directly - but both read the same
// `ceresAsm.compilerPath` setting and look under the same relative paths, and they must keep
// agreeing about what "the compiler" means.

import { readdir, stat } from 'fs/promises';
import * as path from 'path';
import * as vscode from 'vscode';

const EXECUTABLE_NAMES = ['ceres.exe', 'ceres'];

// A loose `ceres` sitting at a candidate root, which is what someone gets after copying the
// binary somewhere convenient. Everything built by CMake is found by scanning instead - see
// cmakeBuildCandidates - because its path carries two segments nobody can list in advance.
const CANDIDATE_RELATIVE_PATHS = EXECUTABLE_NAMES;

const MAX_ANCESTOR_SEARCH_DEPTH = 8;

// CMake puts the binary at <root>/[Ceres/]build/<preset>/bin/<config>/ceres[.exe]. Both the
// preset ("msvc", "gcc", "ninja", or whatever else lives in CMakeUserPresets.json) and the
// configuration ("Debug", "Release", ...) are chosen by whoever built it, so the only honest way
// to find it is to read the two directories rather than guess their contents. A single-config
// generator drops the executable straight into bin/, so that shape is tried too.
async function cmakeBuildCandidates(root: string): Promise<string[]> {
	const candidates: string[] = [];

	for (const buildRoot of [path.join(root, 'Ceres', 'build'), path.join(root, 'build')]) {
		let presets: string[];
		try {
			presets = await readdir(buildRoot);
		} catch {
			continue; // No build tree here; the next root gets its turn.
		}

		for (const preset of presets) {
			const binDirectory = path.join(buildRoot, preset, 'bin');
			let configurations: string[];
			try {
				configurations = await readdir(binDirectory);
			} catch {
				continue;
			}

			for (const name of EXECUTABLE_NAMES) {
				candidates.push(path.join(binDirectory, name));
			}
			for (const configuration of configurations) {
				for (const name of EXECUTABLE_NAMES) {
					candidates.push(path.join(binDirectory, configuration, name));
				}
			}
		}
	}

	return candidates;
}

// Every candidate that exists, newest first. Taking the first one that exists instead used to
// mean that a stale build won one that had just been rebuilt - and a compiler older than the
// language rejects source that is perfectly valid, with a message about the source rather than
// about itself. The newest build is the one that knows the most mnemonics, so it is the one to
// run. With several presets and configurations side by side this matters more, not less: the
// one you built last is the one you meant.
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
		candidates.push(...await cmakeBuildCandidates(root));
	}

	const newest = await newestExisting(candidates);
	if (newest) {
		return newest;
	}

	// Nothing found; let the spawn try PATH and report ENOENT with a message that says what to do.
	return process.platform === 'win32' ? 'ceres.exe' : 'ceres';
}
