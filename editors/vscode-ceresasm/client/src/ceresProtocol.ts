// Talks to `ceres debug --server`: newline-delimited JSON in both directions, one process per
// debug session.
//
// This is not the Debug Adapter Protocol - it is a small vocabulary in the machine's own terms,
// and `debugAdapter.ts` translates. Keeping the two apart means the C++ side stays something a
// script can drive, and that DAP's awkward corners stay on this side of the boundary.

import { ChildProcess, spawn } from 'child_process';

export interface CeresLocation {
	address: number;
	file?: string;
	line?: number;
	sourceFile?: string;
	sourceLine?: number;
	macroDepth?: number;
	isPadding?: boolean;
}

export interface CeresStop extends CeresLocation {
	reason: string;
	description: string;
	breakpointId?: number;
	exception?: string;
	exceptionAddress?: number;
	exceptionLocation?: CeresLocation;
}

export interface CeresFrame extends CeresLocation {
	id: number;
	name: string;
	returnAddress: number;
	stackPointer: number;
	isInterruptHandler: boolean;
	reconstructed: boolean;
}

export interface CeresRegisters {
	general: number[];
	floating: number[];
	pc: number;
	flags: number;
	ticks: number;
	zero: boolean;
	sign: boolean;
	carry: boolean;
	overflow: boolean;
	interrupt: boolean;
	halting: boolean;
	trap: boolean;
}

export interface CeresVariable {
	name: string;
	type: string;
	value: string;
	address: number;
	size: number;
	isConstant: boolean;
}

export interface CeresInstruction extends CeresLocation {
	raw: number;
	text: string;
	symbol: string;
}

export interface CeresCapabilities {
	lineBreakpoints: boolean;
	functionBreakpoints: boolean;
	instructionBreakpoints: boolean;
	disassembly: boolean;
	readMemory: boolean;
	writeMemory: boolean;
	setRegister: boolean;
	restart: boolean;
	goto: boolean;
}

export interface CeresInitialized {
	protocolVersion: number;
	hasDebugInfo: boolean;
	entryPoint: number;
	textSize: number;
	capabilities: CeresCapabilities;
}

type Pending = {
	resolve: (body: Record<string, unknown>) => void;
	reject: (error: Error) => void;
};

export type CeresEventHandler = (event: string, body: Record<string, unknown>) => void;

export class CeresProtocolError extends Error {}

export class CeresProtocolClient {
	private process: ChildProcess | undefined;
	private nextSeq = 1;
	private readonly pending = new Map<number, Pending>();
	private buffer = '';
	private stderr = '';
	private exited = false;

	constructor(private readonly onEvent: CeresEventHandler) {}

	// Resolves once the server has announced itself, so the caller knows what it is talking to
	// before it sends anything.
	start(executable: string, args: string[], cwd: string | undefined): Promise<CeresInitialized> {
		return new Promise<CeresInitialized>((resolve, reject) => {
			let settled = false;

			const child = spawn(executable, args, { cwd, stdio: ['pipe', 'pipe', 'pipe'] });
			this.process = child;

			child.on('error', (error) => {
				// ENOENT here means the executable was not found, which is by far the most common
				// way this goes wrong and deserves to say so plainly.
				const message = (error as NodeJS.ErrnoException).code === 'ENOENT'
					? `Could not run '${executable}'. Build it under Ceres-ASM/src, or set 'ceresAsm.compilerPath'.`
					: error.message;
				if (!settled) {
					settled = true;
					reject(new CeresProtocolError(message));
				}
				this.failAllPending(new CeresProtocolError(message));
			});

			child.stderr?.on('data', (chunk: Buffer) => {
				this.stderr += chunk.toString('utf8');
			});

			child.stdout?.on('data', (chunk: Buffer) => {
				this.buffer += chunk.toString('utf8');
				let newline = this.buffer.indexOf('\n');
				while (newline >= 0) {
					const line = this.buffer.slice(0, newline).trim();
					this.buffer = this.buffer.slice(newline + 1);
					if (line.length > 0) {
						this.handleLine(line, (initialized) => {
							if (!settled) {
								settled = true;
								resolve(initialized);
							}
						});
					}
					newline = this.buffer.indexOf('\n');
				}
			});

			child.on('exit', (code) => {
				this.exited = true;
				// Anything still waiting will never be answered now, and a silent hang is the
				// worst possible failure mode for a debug session.
				const detail = this.stderr.trim();
				this.failAllPending(new CeresProtocolError(
					`The debugger process exited${code === null ? '' : ` with code ${code}`}` +
					(detail ? `: ${detail}` : '')));

				if (!settled) {
					settled = true;
					reject(new CeresProtocolError(detail || 'The debugger process exited before it started up'));
				}
				this.onEvent('processExited', { code: code ?? 0 });
			});
		});
	}

	private handleLine(line: string, onInitialized: (initialized: CeresInitialized) => void): void {
		let message: Record<string, unknown>;
		try {
			message = JSON.parse(line) as Record<string, unknown>;
		} catch {
			this.onEvent('protocolError', { message: `Could not parse: ${line.slice(0, 200)}` });
			return;
		}

		if (message.type === 'event') {
			const event = String(message.event ?? '');
			const body = (message.body ?? {}) as Record<string, unknown>;
			if (event === 'initialized') {
				onInitialized(body as unknown as CeresInitialized);
			}
			this.onEvent(event, body);
			return;
		}

		if (message.type === 'response') {
			const seq = Number(message.request_seq ?? -1);
			const waiting = this.pending.get(seq);
			if (!waiting) {
				return;
			}
			this.pending.delete(seq);

			if (message.success === true) {
				waiting.resolve((message.body ?? {}) as Record<string, unknown>);
			} else {
				waiting.reject(new CeresProtocolError(String(message.message ?? 'The request failed')));
			}
		}
	}

	private failAllPending(error: Error): void {
		for (const waiting of this.pending.values()) {
			waiting.reject(error);
		}
		this.pending.clear();
	}

	send(command: string, args: Record<string, unknown> = {}): Promise<Record<string, unknown>> {
		if (!this.process || this.exited) {
			return Promise.reject(new CeresProtocolError('The debugger is not running'));
		}

		const seq = this.nextSeq++;
		const payload = JSON.stringify({ seq, type: 'request', command, arguments: args });

		return new Promise<Record<string, unknown>>((resolve, reject) => {
			this.pending.set(seq, { resolve, reject });
			this.process?.stdin?.write(payload + '\n', (error) => {
				if (error) {
					this.pending.delete(seq);
					reject(new CeresProtocolError(error.message));
				}
			});
		});
	}

	stop(): void {
		if (!this.process || this.exited) {
			return;
		}
		// Ask first: the machine may be mid-instruction, and the server closes down cleanly when
		// its input ends. The kill is only for a process that ignores that.
		this.send('disconnect').catch(() => undefined);
		this.process.stdin?.end();

		const child = this.process;
		setTimeout(() => {
			if (!this.exited) {
				child.kill();
			}
		}, 1000);
	}
}
