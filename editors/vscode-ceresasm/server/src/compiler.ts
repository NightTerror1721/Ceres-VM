import { execFile } from 'child_process';
import { readdir, stat } from 'fs/promises';
import * as path from 'path';

export interface CompilerDiagnostic {
	// Absolute path of the file this diagnostic belongs to; empty for one that isn't tied to a
	// specific file (e.g. "linking failed").
	file: string;
	line: number;
	column: number;
	// The assembler emits warnings too now - an unused private declaration is dead with certainty,
	// but it does not stop a build.
	severity: 'error' | 'warning';
	message: string;
}

export class CompilerNotFoundError extends Error {
	constructor(compilerPath: string) {
		super(`ceres executable not found: ${compilerPath}`);
		this.name = 'CompilerNotFoundError';
	}
}

// This has to keep agreeing with client/src/compilerPath.ts about what "the compiler" means:
// the two resolvers run in different processes and only one of them has the VSCode API, but a
// diagnostic from one compiler and a debug session from another would be worse than either.
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

// Ancestor directories of a document, up to MAX_ANCESTOR_SEARCH_DEPTH levels - lets the compiler
// be found next to the file being checked even with no workspace folder open at all (e.g. a
// `.casm` file opened directly via "Open File...").
function ancestorsOf(documentPath: string): string[] {
	const ancestors: string[] = [];
	let dir = path.dirname(documentPath);
	for (let i = 0; i < MAX_ANCESTOR_SEARCH_DEPTH; i++) {
		ancestors.push(dir);
		const parent = path.dirname(dir);
		if (parent === dir) {
			break;
		}
		dir = parent;
	}
	return ancestors;
}

export async function resolveCompilerPath(configuredPath: string, workspaceRoots: string[], documentPath?: string): Promise<string> {
	const trimmed = configuredPath.trim();
	if (trimmed.length > 0) {
		return trimmed;
	}

	const searchRoots = documentPath ? [...workspaceRoots, ...ancestorsOf(documentPath)] : workspaceRoots;

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

	// Nothing found; let execFile try PATH and report ENOENT if it's not there.
	return process.platform === 'win32' ? 'ceres.exe' : 'ceres';
}

export function checkFile(compilerPath: string, filePath: string): Promise<CompilerDiagnostic[]> {
	return new Promise((resolve, reject) => {
		execFile(
			compilerPath,
			['asm', filePath, '--json'],
			{ timeout: 10_000 },
			(error, stdout, stderr) => {
				if (error && (error as NodeJS.ErrnoException).code === 'ENOENT') {
					reject(new CompilerNotFoundError(compilerPath));
					return;
				}

				// A non-zero exit means assembly failed, which is expected and carried by stdout,
				// not a reason to reject on its own.
				const trimmed = stdout.trim();
				if (trimmed.length === 0) {
					reject(new Error(stderr.trim() || (error ? error.message : 'ceres produced no output')));
					return;
				}

				try {
					resolve(JSON.parse(trimmed) as CompilerDiagnostic[]);
				} catch (parseError) {
					reject(new Error(`Could not parse ceres --json output: ${(parseError as Error).message}`));
				}
			}
		);
	});
}
