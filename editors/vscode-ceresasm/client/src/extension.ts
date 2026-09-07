import * as path from 'path';
import {
	CancellationToken,
	commands,
	debug,
	DebugAdapterDescriptor,
	DebugAdapterDescriptorFactory,
	DebugAdapterInlineImplementation,
	DebugConfiguration,
	DebugConfigurationProvider,
	DebugSession,
	ExtensionContext,
	ProviderResult,
	window,
	workspace,
	WorkspaceFolder
} from 'vscode';
import {
	LanguageClient,
	LanguageClientOptions,
	ServerOptions,
	TransportKind
} from 'vscode-languageclient/node';

import { resolveCeresExecutable } from './compilerPath';
import { CeresDebugAdapter, CeresLaunchArguments } from './debugAdapter';

let client: LanguageClient | undefined;

// Fills in what a user pressing F5 with no launch.json has left out, so debugging a `.casm` file
// takes no configuration at all.
class CeresConfigurationProvider implements DebugConfigurationProvider {
	resolveDebugConfiguration(
		_folder: WorkspaceFolder | undefined,
		config: DebugConfiguration,
		_token?: CancellationToken
	): ProviderResult<DebugConfiguration> {
		if (!config.type && !config.request && !config.name) {
			const editor = window.activeTextEditor;
			if (editor?.document.languageId !== 'casm') {
				return undefined;
			}
			config.type = 'casm';
			config.name = 'Debug current CASM file';
			config.request = 'launch';
			config.program = '${file}';
			config.stopOnEntry = true;
		}

		if (!config.program) {
			void window.showErrorMessage('CeresASM: no program to debug. Set "program" in launch.json.');
			return undefined;
		}

		return config;
	}
}

class CeresDebugAdapterFactory implements DebugAdapterDescriptorFactory {
	createDebugAdapterDescriptor(session: DebugSession): ProviderResult<DebugAdapterDescriptor> {
		const configuration = session.configuration as unknown as CeresLaunchArguments;
		// Inline, so there is no second Node process and no Content-Length framing to get wrong.
		return new DebugAdapterInlineImplementation(
			new CeresDebugAdapter(() => resolveCeresExecutable(configuration.program))
		);
	}
}

export function activate(context: ExtensionContext): void {
	const serverModule = context.asAbsolutePath(path.join('server', 'out', 'server.js'));

	const serverOptions: ServerOptions = {
		run: { module: serverModule, transport: TransportKind.ipc },
		debug: {
			module: serverModule,
			transport: TransportKind.ipc,
			options: { execArgv: ['--nolazy', '--inspect=6009'] }
		}
	};

	const clientOptions: LanguageClientOptions = {
		documentSelector: [{ scheme: 'file', language: 'casm' }],
		synchronize: {
			fileEvents: workspace.createFileSystemWatcher('**/*.casm')
		}
	};

	client = new LanguageClient(
		'ceresAsmLanguageServer',
		'CeresASM Language Server',
		serverOptions,
		clientOptions
	);

	context.subscriptions.push(
		commands.registerCommand('ceresAsm.restartServer', async () => {
			if (!client) {
				return;
			}
			await client.stop();
			await client.start();
			void window.showInformationMessage('CeresASM language server restarted.');
		})
	);

	// The debugger owns the Debug Console, so a program that reads from its terminal has no other
	// way to be fed. Typing `>text` into the console does the same thing.
	context.subscriptions.push(
		commands.registerCommand('ceresAsm.sendInput', async () => {
			const session = debug.activeDebugSession;
			if (!session || session.type !== 'casm') {
				void window.showInformationMessage('CeresASM: no debug session is running.');
				return;
			}

			const text = await window.showInputBox({
				prompt: 'Send a line to the program’s terminal input',
				placeHolder: 'The program reads this from port 0x02'
			});
			if (text === undefined) {
				return;
			}

			await session.customRequest('evaluate', { expression: `>${text}`, context: 'repl' });
		})
	);

	context.subscriptions.push(
		debug.registerDebugConfigurationProvider('casm', new CeresConfigurationProvider()),
		debug.registerDebugAdapterDescriptorFactory('casm', new CeresDebugAdapterFactory())
	);

	void client.start();
}

export function deactivate(): Thenable<void> | undefined {
	return client ? client.stop() : undefined;
}
