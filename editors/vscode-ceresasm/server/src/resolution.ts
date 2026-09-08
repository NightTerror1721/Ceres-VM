import { Position, Range } from 'vscode-languageserver/node';
import {
	ConstSymbol,
	findEnclosingNonLocalLabel,
	findEnclosingStruct,
	LabelSymbol,
	MacroSymbol,
	qualifierBefore,
	resolveImportPath,
	splitTopLevelArguments,
	StructField,
	StructSymbol,
	SymbolIndexer,
	TokenAtPosition,
	VariableSymbol
} from './symbolIndex';
import { URI } from 'vscode-uri';

export type ResolvedSymbol =
	| { kind: 'const'; symbol: ConstSymbol }
	| { kind: 'variable'; symbol: VariableSymbol }
	| { kind: 'label'; symbol: LabelSymbol }
	| { kind: 'macro'; symbol: MacroSymbol; candidates: MacroSymbol[] }
	| { kind: 'struct'; symbol: StructSymbol }
	| { kind: 'field'; symbol: StructSymbol; field: StructField; owner: StructSymbol };

// Where the definition of a resolved symbol lives. A field's is the field's own line, which is
// not the same place as its struct's.
export function definitionLocationOf(resolved: ResolvedSymbol): { uri: string; range: Range } {
	if (resolved.kind === 'field') {
		return { uri: resolved.field.uri, range: resolved.field.range };
	}
	return { uri: resolved.symbol.uri, range: resolved.symbol.range as Range };
}

// Shared by hover and go-to-definition: given the identifier token under the cursor, resolves it
// against the visible symbol set - this file's own declarations, plus everything an `import`
// makes visible, transitively. Callers are expected to have already ruled out reserved words
// (mnemonics, registers, keywords, types) before calling this.
export function resolveUserSymbol(
	indexer: SymbolIndexer,
	uri: string,
	position: Position,
	token: TokenAtPosition,
	lineText: string
): ResolvedSymbol | null {
	const { file, consts, variables, labels, macros, macrosByName, structs } = indexer.collectVisibleSymbols(uri);

	if (token.text.startsWith('.')) {
		// `Frame.field` and `module.NAME` are two tokens to the scanner - a name, then a dotted
		// one - because a local label is written the same way and there is no whitespace to tell
		// them apart. What decides is whether anything is written immediately before the dot.
		const qualifier = qualifierBefore(lineText, token);
		if (qualifier) {
			return resolveQualified(indexer, uri, qualifier, token.text.slice(1), structs);
		}

		const parent = findEnclosingNonLocalLabel(file, position.line);
		const qualifiedName = parent ? `${parent.declaredName}${token.text}` : token.text;
		const label = file.labels.get(qualifiedName);
		return label ? { kind: 'label', symbol: label } : null;
	}

	// Inside a struct body, a bare name is one of its fields - and the declaration is written
	// `count: u32`, which is the one place a field is not spelled with a dot.
	const enclosingStruct = findEnclosingStruct(file, position.line);
	if (enclosingStruct) {
		const field = enclosingStruct.fieldsByName.get(token.text);
		if (field) {
			return { kind: 'field', symbol: enclosingStruct, field, owner: enclosingStruct };
		}
	}

	const structSymbol = structs.get(token.text);
	if (structSymbol) {
		return { kind: 'struct', symbol: structSymbol };
	}

	const constSymbol = consts.get(token.text);
	if (constSymbol) {
		return { kind: 'const', symbol: constSymbol };
	}

	// The visible set, not just this file's: a `let` another unit declares `global` is reachable
	// from here, and a `ldv` naming it is the ordinary case rather than an exotic one.
	const variableSymbol = variables.get(token.text) ?? file.variables.get(token.text);
	if (variableSymbol) {
		return { kind: 'variable', symbol: variableSymbol };
	}

	const labelSymbol = file.labels.get(token.text) ?? labels.get(token.text);
	if (labelSymbol && labelSymbol.visibility !== 'local') {
		return { kind: 'label', symbol: labelSymbol };
	}

	const macroCandidates = macrosByName.get(token.text);
	if (macroCandidates && macroCandidates.length > 0) {
		const argsText = lineText.slice(token.endCharacter).replace(/^\s*,?\s*/, '');
		const argCount = argsText.trim().length === 0 ? 0 : splitTopLevelArguments(argsText).length;
		const exact = macros.get(`${token.text}/${argCount}`);
		return { kind: 'macro', symbol: exact ?? macroCandidates[0], candidates: macroCandidates };
	}

	return null;
}

// `Frame.field`, or `math.LIMIT` where `math` is what an `import ... as` called a module.
function resolveQualified(
	indexer: SymbolIndexer,
	uri: string,
	qualifier: string,
	member: string,
	structs: Map<string, StructSymbol>
): ResolvedSymbol | null {
	const owner = structs.get(qualifier);
	if (owner) {
		const field = owner.fieldsByName.get(member);
		return field ? { kind: 'field', symbol: owner, field, owner } : null;
	}

	const file = indexer.getFileIndex(uri);
	const importSpec = file.imports.find((spec) => spec.alias === qualifier);
	if (!importSpec) {
		return null;
	}

	const moduleUri = URI.file(resolveImportPath(uri, importSpec.importPath)).toString();
	const module = indexer.getFileIndex(moduleUri);

	const constSymbol = module.consts.get(member);
	if (constSymbol && constSymbol.isGlobal) {
		return { kind: 'const', symbol: constSymbol };
	}
	const structSymbol = module.structs.get(member);
	if (structSymbol && structSymbol.isGlobal) {
		return { kind: 'struct', symbol: structSymbol };
	}
	const variableSymbol = module.variables.get(member);
	if (variableSymbol && variableSymbol.isGlobal) {
		return { kind: 'variable', symbol: variableSymbol };
	}
	const labelSymbol = module.labels.get(member);
	if (labelSymbol && labelSymbol.visibility === 'global') {
		return { kind: 'label', symbol: labelSymbol };
	}

	return null;
}
