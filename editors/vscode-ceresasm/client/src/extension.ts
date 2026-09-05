import * as path from 'path';
import { commands, ExtensionContext, window, workspace } from 'vscode';
import {
	LanguageClient,
	LanguageClientOptions,
	ServerOptions,
	TransportKind
} from 'vscode-languageclient/node';

let client: LanguageClient | undefined;

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

	void client.start();
}

export function deactivate(): Thenable<void> | undefined {
	return client ? client.stop() : undefined;
}
