import { execFile } from 'child_process';
import { access, constants } from 'fs/promises';
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

// Checked in order under each candidate root before falling back to PATH. Three shapes because a
// "root" might be the repository root, the Ceres-ASM folder itself, or its src/ folder, depending
// on whether it came from an open workspace folder or from walking up from the document being
// checked (see resolveCompilerPath).
const CANDIDATE_RELATIVE_PATHS = [
	path.join('Ceres-ASM', 'src', 'ceres.exe'),
	path.join('Ceres-ASM', 'src', 'ceres'),
	path.join('src', 'ceres.exe'),
	path.join('src', 'ceres'),
	'ceres.exe',
	'ceres'
];

const MAX_ANCESTOR_SEARCH_DEPTH = 8;

async function fileExists(candidate: string): Promise<boolean> {
	try {
		await access(candidate, constants.F_OK);
		return true;
	} catch {
		return false;
	}
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

	for (const root of searchRoots) {
		for (const relative of CANDIDATE_RELATIVE_PATHS) {
			const candidate = path.join(root, relative);
			if (await fileExists(candidate)) {
				return candidate;
			}
		}
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
