// The Debug Adapter Protocol side of the debugger, translating between what VSCode asks for and
// what `ceres debug --server` understands.
//
// It implements vscode.DebugAdapter directly rather than building on @vscode/debugadapter. Running
// inline means VSCode hands over already-parsed DAP messages and takes them back the same way, so
// the library's whole job - Content-Length framing and a base class - is not needed, and the
// extension keeps its dependency list as short as the rest of the project's.

import * as path from 'path';
import * as vscode from 'vscode';
import {
	CeresFrame,
	CeresInitialized,
	CeresInstruction,
	CeresProtocolClient,
	CeresRegisters,
	CeresStop,
	CeresVariable
} from './ceresProtocol';

// Enough of the DAP shape to be precise about what crosses the boundary; the full protocol types
// would be a dependency for no gain.
interface DapMessage {
	seq: number;
	type: 'request' | 'response' | 'event';
	[key: string]: unknown;
}

// Variable containers. DAP identifies them by number, and zero means "not expandable".
const enum Scope {
	Registers = 1000,
	Flags = 1001,
	Globals = 1002,
	FloatRegisters = 1003
}

const THREAD_ID = 1;

// The machine's system exceptions, spelled the way the session spells them.
const EXCEPTION_FILTERS = [
	'Trap',
	'IllegalInstruction',
	'MemoryFault',
	'DivisionByZero',
	'StackOverflow',
	'AlignmentFault'
];

export interface CeresLaunchArguments {
	program: string;
	sources?: string[];
	stopOnEntry?: boolean;
	memory?: number;
	cwd?: string;
	ceresPath?: string;
	trace?: boolean;
}

export class CeresDebugAdapter implements vscode.DebugAdapter {
	private readonly messageEmitter = new vscode.EventEmitter<vscode.DebugProtocolMessage>();
	readonly onDidSendMessage = this.messageEmitter.event;

	private readonly client: CeresProtocolClient;
	private sequence = 1;
	private initialized: CeresInitialized | undefined;
	private launchArguments: CeresLaunchArguments | undefined;
	private terminated = false;

	// The program writes bytes, not characters: an accented letter reaches the terminal device as
	// two separate port writes, so the decoder has to be told the stream continues.
	private readonly decoder = new TextDecoder('utf-8');

	// Filled from the last stop so `variables` and `evaluate` can answer without another round
	// trip for state that has not moved.
	private registers: CeresRegisters | undefined;
	private globals: CeresVariable[] = [];
	// Kept so `exceptionInfo` can answer without another round trip: DAP asks for the detail only
	// after it has already been told the machine stopped on a fault.
	private lastStop: CeresStop | undefined;

	constructor(private readonly resolveExecutable: () => Promise<string>) {
		this.client = new CeresProtocolClient((event, body) => this.handleCeresEvent(event, body));
	}

	dispose(): void {
		this.client.stop();
	}

	// --- Sending -------------------------------------------------------------------------------

	private send(message: Omit<DapMessage, 'seq'>): void {
		this.messageEmitter.fire({ seq: this.sequence++, ...message } as vscode.DebugProtocolMessage);
	}

	private sendEvent(event: string, body?: Record<string, unknown>): void {
		this.send({ type: 'event', event, body });
	}

	private sendResponse(request: DapMessage, body?: Record<string, unknown>): void {
		this.send({
			type: 'response',
			request_seq: request.seq,
			command: request.command,
			success: true,
			body
		});
	}

	private sendError(request: DapMessage, message: string): void {
		this.send({
			type: 'response',
			request_seq: request.seq,
			command: request.command,
			success: false,
			message
		});
	}

	// --- Events from ceres ---------------------------------------------------------------------

	private handleCeresEvent(event: string, body: Record<string, unknown>): void {
		switch (event) {
			case 'output': {
				// A logpoint arrives as text on the console category: it is the debugger talking,
				// not the program, so it needs no decoding and no interleaving with program bytes.
				if (typeof body.text === 'string') {
					this.sendEvent('output', {
						category: String(body.category ?? 'console'),
						output: `${body.text}\n`
					});
					return;
				}

				const hex = String(body.hex ?? '');
				if (hex.length === 0) {
					return;
				}
				const bytes = new Uint8Array(hex.length / 2);
				for (let i = 0; i < bytes.length; i++) {
					bytes[i] = parseInt(hex.substr(i * 2, 2), 16);
				}
				// `stream: true` is the whole point: without it a character split across two port
				// writes decodes as two replacement characters, which is every accented letter in
				// the Spanish tutorial.
				const text = this.decoder.decode(bytes, { stream: true });
				if (text.length > 0) {
					this.sendEvent('output', { category: 'stdout', output: text });
				}
				return;
			}

			case 'stopped': {
				const stop = body as unknown as CeresStop;
				this.registers = undefined;
				this.lastStop = stop;
				this.sendEvent('stopped', {
					reason: this.mapStopReason(stop.reason),
					threadId: THREAD_ID,
					description: stop.description,
					text: stop.exception,
					allThreadsStopped: true,
					hitBreakpointIds: stop.breakpointId ? [stop.breakpointId] : undefined
				});
				return;
			}

			case 'exited':
				this.sendEvent('exited', { exitCode: Number(body.exitCode ?? 0) });
				return;

			case 'terminated':
			case 'processExited':
				if (!this.terminated) {
					this.terminated = true;
					this.sendEvent('terminated');
				}
				return;

			case 'protocolError':
				this.sendEvent('output', {
					category: 'console',
					output: `CeresASM debugger: ${String(body.message ?? 'protocol error')}\n`
				});
				return;

			default:
				return;
		}
	}

	private mapStopReason(reason: string): string {
		switch (reason) {
			case 'breakpoint': return 'breakpoint';
			case 'step': return 'step';
			case 'entry': return 'entry';
			case 'exception': return 'exception';
			case 'data breakpoint': return 'data breakpoint';
			case 'pause': return 'pause';
			// Neither of these is a DAP reason, but both mean "the machine has stopped and is not
			// coming back on its own", which is what 'pause' renders as.
			case 'halted': return 'pause';
			case 'step limit': return 'pause';
			default: return 'pause';
		}
	}

	// --- Requests from VSCode --------------------------------------------------------------------

	handleMessage(message: vscode.DebugProtocolMessage): void {
		const request = message as DapMessage;
		if (request.type !== 'request') {
			return;
		}

		this.dispatch(request).catch((error: unknown) => {
			this.sendError(request, error instanceof Error ? error.message : String(error));
		});
	}

	private async dispatch(request: DapMessage): Promise<void> {
		const args = (request.arguments ?? {}) as Record<string, unknown>;

		switch (request.command) {
			case 'initialize':
				this.sendResponse(request, {
					supportsConfigurationDoneRequest: true,
					supportsFunctionBreakpoints: true,
					supportsInstructionBreakpoints: true,
					supportsDisassembleRequest: true,
					supportsReadMemoryRequest: true,
					supportsWriteMemoryRequest: true,
					supportsSetVariable: true,
					supportsRestartRequest: true,
					supportsTerminateRequest: true,
					supportsSteppingGranularity: true,
					supportsEvaluateForHovers: true,
					supportsConditionalBreakpoints: true,
					supportsHitConditionalBreakpoints: true,
					supportsLogPoints: true,
					supportsDataBreakpoints: true,
					supportsExceptionInfoRequest: true,
					supportsGotoTargetsRequest: true,
					// The machine counts time in executed instructions rather than wall clock, so
					// re-running from a snapshot lands in exactly the same place. That is what
					// makes stepping backwards real here rather than an approximation.
					supportsStepBack: true,
					supportsValueFormattingOptions: false,
					// The machine's seven system exceptions, each one something a CASM program can
					// actually hit. All are checked by default, which is what the session does when
					// nobody has said otherwise.
					exceptionBreakpointFilters: EXCEPTION_FILTERS.map((filter) => ({
						filter,
						label: filter,
						default: true
					}))
				});
				return;

			case 'launch':
				await this.launch(request, args as unknown as CeresLaunchArguments);
				return;

			case 'setBreakpoints':
				await this.setBreakpoints(request, args);
				return;

			case 'setFunctionBreakpoints':
				await this.setFunctionBreakpoints(request, args);
				return;

			case 'setInstructionBreakpoints':
				await this.setInstructionBreakpoints(request, args);
				return;

			case 'setExceptionBreakpoints': {
				const filters = (args.filters ?? []) as string[];
				await this.client.send('setExceptionFilters', { filters });
				this.sendResponse(request, { breakpoints: filters.map(() => ({ verified: true })) });
				return;
			}

			case 'exceptionInfo': {
				const stop = this.lastStop;
				if (!stop || !stop.exception) {
					this.sendError(request, 'The machine did not stop on an exception');
					return;
				}
				this.sendResponse(request, {
					exceptionId: stop.exception,
					// 'always' rather than 'userUnhandled': every fault here goes to the BIOS stub,
					// which prints an E and halts, so nothing is ever really handled.
					breakMode: 'always',
					description: stop.description,
					details: {
						message: stop.exception,
						// Where it happened, which is not where the program counter now is.
						stackTrace: stop.exceptionLocation?.file
							? `at ${stop.exceptionLocation.file}:${stop.exceptionLocation.line}`
							: `at 0x${(stop.exceptionAddress ?? 0).toString(16)}`
					}
				});
				return;
			}

			case 'dataBreakpointInfo': {
				// VSCode asks this before offering "Break on Value Change" in the context menu.
				// Only a named global has an extent the session can watch; a register does not
				// live in memory at all.
				const name = String(args.name ?? '');
				const variable = (await this.fetchGlobals()).find((candidate) => candidate.name === name);
				if (!variable || variable.isConstant || variable.size === 0) {
					this.sendResponse(request, {
						dataId: null,
						description: variable?.isConstant
							? 'A constant occupies no memory, so it cannot change'
							: 'Only global variables can be watched'
					});
					return;
				}

				this.sendResponse(request, {
					// The address and size are what is actually watched; the name only makes the
					// stop message readable.
					dataId: `${variable.address}:${variable.size}:${variable.name}`,
					description: `${variable.name} (${variable.size} bytes)`,
					accessTypes: ['write'],
					canPersist: false
				});
				return;
			}

			case 'setDataBreakpoints': {
				const requested = (args.breakpoints ?? []) as { dataId: string }[];
				const watches = requested.map((breakpoint) => {
					const [address, size, ...label] = breakpoint.dataId.split(':');
					return {
						address: Number(address),
						size: Number(size),
						label: label.join(':')
					};
				});

				const body = await this.client.send('setDataBreakpoints', { watches });
				const results = (body.watches ?? []) as { verified: boolean; message?: string }[];
				this.sendResponse(request, {
					breakpoints: results.map((result) => ({
						verified: result.verified,
						message: result.message
					}))
				});
				return;
			}

			case 'gotoTargets': {
				// "Jump to cursor": the line has to have produced code, and the target is where
				// that line is *entered*, not merely its lowest address.
				const source = (args.source ?? {}) as { path?: string };
				const line = Number(args.line ?? 0);

				try {
					const body = await this.client.send('resolveLine', { file: source.path ?? '', line });
					const address = Number(body.address ?? 0);
					this.sendResponse(request, {
						targets: [{
							id: address,
							label: `line ${line}`,
							line,
							instructionPointerReference: this.toReference(address)
						}]
					});
				} catch {
					// No code on that line, so there is nowhere to jump to. An empty list is the
					// protocol's way of saying so, and VSCode simply offers nothing.
					this.sendResponse(request, { targets: [] });
				}
				return;
			}

			case 'goto':
				await this.client.send('goto', { address: Number(args.targetId ?? 0) });
				this.sendResponse(request);
				this.sendEvent('stopped', {
					reason: 'goto',
					threadId: THREAD_ID,
					allThreadsStopped: true
				});
				return;

			case 'configurationDone':
				await this.client.send('configurationDone');
				this.sendResponse(request);
				return;

			case 'threads':
				this.sendResponse(request, { threads: [{ id: THREAD_ID, name: 'ceres' }] });
				return;

			case 'stackTrace':
				await this.stackTrace(request);
				return;

			case 'scopes':
				this.sendResponse(request, {
					scopes: [
						{ name: 'Registers', variablesReference: Scope.Registers, expensive: false },
						{ name: 'Flags', variablesReference: Scope.Flags, expensive: false },
						{ name: 'Float registers', variablesReference: Scope.FloatRegisters, expensive: false },
						{ name: 'Globals', variablesReference: Scope.Globals, expensive: false }
					]
				});
				return;

			case 'variables':
				await this.variables(request, Number(args.variablesReference ?? 0));
				return;

			case 'setVariable':
				await this.setVariable(request, args);
				return;

			case 'continue':
				await this.client.send('continue');
				this.sendResponse(request, { allThreadsContinued: true });
				return;

			case 'next':
				await this.client.send(args.granularity === 'instruction' ? 'stepInstruction' : 'next');
				this.sendResponse(request);
				return;

			case 'stepIn':
				await this.client.send(args.granularity === 'instruction' ? 'stepInstruction' : 'stepIn');
				this.sendResponse(request);
				return;

			case 'stepOut':
				await this.client.send('stepOut');
				this.sendResponse(request);
				return;

			case 'stepBack':
				await this.client.send(
					args.granularity === 'instruction' ? 'stepBackInstruction' : 'stepBack');
				this.sendResponse(request);
				return;

			case 'reverseContinue':
				await this.client.send('reverseContinue');
				this.sendResponse(request, { allThreadsContinued: true });
				return;

			case 'pause':
				await this.client.send('pause');
				this.sendResponse(request);
				return;

			case 'evaluate':
				await this.evaluate(request, args);
				return;

			case 'readMemory':
				await this.readMemory(request, args);
				return;

			case 'writeMemory':
				await this.writeMemory(request, args);
				return;

			case 'disassemble':
				await this.disassemble(request, args);
				return;

			case 'restart':
				await this.client.send('restart');
				this.sendResponse(request);
				return;

			case 'terminate':
			case 'disconnect':
				this.client.stop();
				this.sendResponse(request);
				if (!this.terminated) {
					this.terminated = true;
					this.sendEvent('terminated');
				}
				return;

			default:
				this.sendError(request, `CeresASM does not implement '${request.command}'`);
				return;
		}
	}

	// --- Launch ---------------------------------------------------------------------------------

	private async launch(request: DapMessage, args: CeresLaunchArguments): Promise<void> {
		this.launchArguments = args;

		const executable = args.ceresPath && args.ceresPath.trim().length > 0
			? args.ceresPath.trim()
			: await this.resolveExecutable();

		const commandLine = ['debug', args.program, ...(args.sources ?? []), '--server'];
		if (args.memory && args.memory > 0) {
			commandLine.push('--memory', String(args.memory));
		}
		if (args.stopOnEntry === false) {
			commandLine.push('--no-stop-on-entry');
		}

		if (args.trace) {
			this.sendEvent('output', {
				category: 'console',
				output: `> ${executable} ${commandLine.join(' ')}\n`
			});
		}

		const cwd = args.cwd ?? path.dirname(args.program);
		this.initialized = await this.client.start(executable, commandLine, cwd);

		if (!this.initialized.hasDebugInfo) {
			// Worth saying out loud: without the line table the session still works, but only in
			// addresses, and every breakpoint the user sets will come back unverified.
			this.sendEvent('output', {
				category: 'console',
				output: 'This program carries no debug information, so breakpoints by line are not ' +
					'available. Assemble with --debug, or debug the .casm source directly.\n'
			});
		}

		this.sendResponse(request);
		// Tells VSCode to send the breakpoints it has; configurationDone follows and starts the
		// program, so nothing runs before they are in place.
		this.sendEvent('initialized');
	}

	// --- Breakpoints ----------------------------------------------------------------------------

	private async setBreakpoints(request: DapMessage, args: Record<string, unknown>): Promise<void> {
		const source = (args.source ?? {}) as { path?: string };
		const requested = (args.breakpoints ?? []) as {
			line: number;
			condition?: string;
			hitCondition?: string;
			logMessage?: string;
		}[];
		const file = source.path ?? '';

		// The long form carries the condition with each line; the session takes both shapes.
		const body = await this.client.send('setBreakpoints', {
			file,
			lines: requested.map((breakpoint) => ({
				line: breakpoint.line,
				condition: breakpoint.condition ?? '',
				hitCondition: breakpoint.hitCondition ?? '',
				logMessage: breakpoint.logMessage ?? ''
			}))
		});

		const results = (body.breakpoints ?? []) as {
			line: number;
			verified: boolean;
			id?: number;
			address?: number;
			message?: string;
		}[];

		this.sendResponse(request, {
			breakpoints: results.map((result) => ({
				id: result.id,
				verified: result.verified,
				line: result.line,
				message: result.message,
				instructionReference: result.address === undefined ? undefined : this.toReference(result.address)
			}))
		});
	}

	private async setFunctionBreakpoints(request: DapMessage, args: Record<string, unknown>): Promise<void> {
		const requested = (args.breakpoints ?? []) as { name: string }[];
		const body = await this.client.send('setFunctionBreakpoints', {
			names: requested.map((breakpoint) => breakpoint.name)
		});

		const results = (body.breakpoints ?? []) as { verified: boolean; id?: number; message?: string }[];
		this.sendResponse(request, {
			breakpoints: results.map((result) => ({
				id: result.id,
				verified: result.verified,
				message: result.message
			}))
		});
	}

	private async setInstructionBreakpoints(request: DapMessage, args: Record<string, unknown>): Promise<void> {
		const requested = (args.breakpoints ?? []) as { instructionReference: string; offset?: number }[];
		const addresses = requested.map((breakpoint) =>
			this.fromReference(breakpoint.instructionReference) + (breakpoint.offset ?? 0));

		const body = await this.client.send('setInstructionBreakpoints', { addresses });
		const results = (body.breakpoints ?? []) as { verified: boolean; id?: number }[];

		this.sendResponse(request, {
			breakpoints: results.map((result) => ({ id: result.id, verified: result.verified }))
		});
	}

	// --- State ----------------------------------------------------------------------------------

	private async stackTrace(request: DapMessage): Promise<void> {
		const body = await this.client.send('stackTrace');
		const frames = (body.frames ?? []) as CeresFrame[];

		this.sendResponse(request, {
			totalFrames: frames.length,
			stackFrames: frames.map((frame) => ({
				id: frame.id,
				// The caveat belongs in the name, where it is visible, rather than in a tooltip
				// nobody opens: these frames are inferred from watching CALL and RET go past.
				name: frame.isInterruptHandler ? `${frame.name} [interrupt]` : frame.name,
				line: frame.line ?? 0,
				column: 1,
				instructionPointerReference: this.toReference(frame.address),
				source: frame.file ? { name: path.basename(frame.file), path: frame.file } : undefined,
				presentationHint: frame.isInterruptHandler ? 'subtle' : undefined
			}))
		});
	}

	private async ensureRegisters(): Promise<CeresRegisters> {
		if (!this.registers) {
			this.registers = await this.client.send('registers') as unknown as CeresRegisters;
		}
		return this.registers;
	}

	private async variables(request: DapMessage, reference: number): Promise<void> {
		switch (reference) {
			case Scope.Registers: {
				const registers = await this.ensureRegisters();
				const names = ['r0', 'r1', 'r2', 'r3', 'r4', 'r5', 'r6', 'r7', 'r8', 'r9', 'r10', 'r11',
					'r12', 'lr', 'fp', 'sp'];
				const variables = registers.general.map((value, index) => ({
					name: names[index] ?? `r${index}`,
					value: this.hex(value),
					variablesReference: 0,
					evaluateName: index === 13 ? 'lr' : index === 14 ? 'fp' : index === 15 ? 'sp' : `r${index}`,
					memoryReference: this.toReference(value)
				}));
				variables.push({
					name: 'pc',
					value: this.hex(registers.pc),
					variablesReference: 0,
					evaluateName: 'pc',
					memoryReference: this.toReference(registers.pc)
				});
				this.sendResponse(request, { variables });
				return;
			}

			case Scope.FloatRegisters: {
				const registers = await this.ensureRegisters();
				this.sendResponse(request, {
					variables: registers.floating.map((value, index) => ({
						name: `f${index}`,
						value: String(value),
						variablesReference: 0,
						evaluateName: `f${index}`
					}))
				});
				return;
			}

			case Scope.Flags: {
				const registers = await this.ensureRegisters();
				const flag = (name: string, value: boolean) => ({
					name,
					value: value ? '1' : '0',
					variablesReference: 0
				});
				this.sendResponse(request, {
					variables: [
						flag('zero', registers.zero),
						flag('sign', registers.sign),
						flag('carry', registers.carry),
						flag('overflow', registers.overflow),
						flag('interrupt', registers.interrupt),
						flag('halting', registers.halting),
						flag('trap', registers.trap),
						{ name: 'ticks', value: String(registers.ticks), variablesReference: 0 }
					]
				});
				return;
			}

			case Scope.Globals: {
				const globals = await this.fetchGlobals();
				this.sendResponse(request, {
					variables: globals.map((variable) => ({
						name: variable.name,
						value: variable.value,
						type: variable.isConstant ? `const ${variable.type}` : variable.type,
						variablesReference: 0,
						evaluateName: variable.name,
						memoryReference: variable.isConstant ? undefined : this.toReference(variable.address)
					}))
				});
				return;
			}

			default:
				this.sendResponse(request, { variables: [] });
				return;
		}
	}

	private async setVariable(request: DapMessage, args: Record<string, unknown>): Promise<void> {
		const name = String(args.name ?? '');
		const raw = String(args.value ?? '').trim();
		const value = raw.startsWith('0x') || raw.startsWith('0X')
			? parseInt(raw.slice(2), 16)
			: parseInt(raw, 10);

		if (!Number.isFinite(value)) {
			this.sendError(request, `'${raw}' is not a number`);
			return;
		}

		await this.client.send('setRegister', { name, value: value >>> 0 });
		this.registers = undefined;
		this.sendResponse(request, { value: this.hex(value >>> 0) });
	}

	// --- Evaluate -------------------------------------------------------------------------------

	private async fetchGlobals(): Promise<CeresVariable[]> {
		const body = await this.client.send('globals');
		this.globals = (body.variables ?? []) as CeresVariable[];
		return this.globals;
	}

	private async evaluate(request: DapMessage, args: Record<string, unknown>): Promise<void> {
		const expression = String(args.expression ?? '').trim();

		// A line typed into the Debug Console starting with '>' is the program's input, not an
		// expression. The debugger owns the console, so there is no other way to reach a program
		// that reads from the terminal - which the tutorial's two games both do.
		if (args.context === 'repl' && expression.startsWith('>')) {
			await this.client.send('input', { text: expression.slice(1).trimStart() + '\n' });
			this.sendResponse(request, { result: '(sent to the program)', variablesReference: 0 });
			return;
		}

		// Everything else goes to the machine's own evaluator: registers, flags, symbols, typed
		// memory loads and arithmetic over them. Hovering something that is not an expression at
		// all - a mnemonic, a comment - fails, and VSCode simply shows no tooltip.
		const body = await this.client.send('evaluate', { expression });
		const address = body.address === undefined ? undefined : Number(body.address);

		this.sendResponse(request, {
			result: String(body.result ?? ''),
			type: String(body.type ?? ''),
			variablesReference: 0,
			memoryReference: address === undefined ? undefined : this.toReference(address)
		});
	}

	// --- Memory and disassembly -------------------------------------------------------------------

	private async readMemory(request: DapMessage, args: Record<string, unknown>): Promise<void> {
		const address = this.fromReference(String(args.memoryReference ?? '0')) + Number(args.offset ?? 0);
		const count = Number(args.count ?? 0);

		const body = await this.client.send('readMemory', { address, count });
		const hex = String(body.hex ?? '');
		const bytes = new Uint8Array(hex.length / 2);
		for (let i = 0; i < bytes.length; i++) {
			bytes[i] = parseInt(hex.substr(i * 2, 2), 16);
		}

		this.sendResponse(request, {
			address: this.toReference(address),
			// DAP carries memory as base64; the wire protocol carries it as hex, because hex is
			// the form a person reads in a dump and neither side needs the extra dependency.
			data: Buffer.from(bytes).toString('base64'),
			unreadableBytes: Math.max(0, count - bytes.length)
		});
	}

	private async writeMemory(request: DapMessage, args: Record<string, unknown>): Promise<void> {
		const address = this.fromReference(String(args.memoryReference ?? '0')) + Number(args.offset ?? 0);
		const bytes = Buffer.from(String(args.data ?? ''), 'base64');
		const hex = bytes.toString('hex');

		const body = await this.client.send('writeMemory', { address, hex });
		this.sendResponse(request, { bytesWritten: Number(body.written ?? 0) });
	}

	private async disassemble(request: DapMessage, args: Record<string, unknown>): Promise<void> {
		const origin = this.fromReference(String(args.memoryReference ?? '0')) + Number(args.offset ?? 0);
		const instructionOffset = Number(args.instructionOffset ?? 0);
		const count = Number(args.instructionCount ?? 16);

		// DAP asks for `instructionOffset` instructions *before* the origin; every instruction here
		// is four bytes, so that is exact rather than the guess it is on a variable-length ISA.
		const before = instructionOffset < 0 ? -instructionOffset : 0;
		const start = instructionOffset > 0 ? origin + instructionOffset * 4 : origin;

		const body = await this.client.send('disassemble', { address: start, before, count });
		const instructions = (body.instructions ?? []) as CeresInstruction[];

		this.sendResponse(request, {
			instructions: instructions.map((instruction) => ({
				address: this.toReference(instruction.address),
				instructionBytes: this.instructionBytes(instruction.raw),
				instruction: instruction.text,
				symbol: instruction.symbol || undefined,
				location: instruction.file
					? { name: path.basename(instruction.file), path: instruction.file }
					: undefined,
				line: instruction.line
			}))
		});
	}

	// --- Small helpers ----------------------------------------------------------------------------

	private hex(value: number): string {
		return `0x${(value >>> 0).toString(16).padStart(8, '0')}`;
	}

	private toReference(address: number): string {
		return `0x${(address >>> 0).toString(16)}`;
	}

	private fromReference(reference: string): number {
		const text = reference.trim();
		return text.startsWith('0x') || text.startsWith('0X')
			? parseInt(text.slice(2), 16) >>> 0
			: parseInt(text, 10) >>> 0;
	}

	private instructionBytes(raw: number): string {
		// Little-endian, the way they sit in memory and the way a hex dump shows them.
		const bytes = [raw & 0xff, (raw >>> 8) & 0xff, (raw >>> 16) & 0xff, (raw >>> 24) & 0xff];
		return bytes.map((byte) => byte.toString(16).padStart(2, '0')).join(' ');
	}
}
