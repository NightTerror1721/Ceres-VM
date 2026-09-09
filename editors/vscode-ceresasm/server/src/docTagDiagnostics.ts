// The optional, deliberately-conservative half of Proposal B: comparing the operand shape written
// at a macro call site against the `@param` kind its doc comment declared. This is advisory only -
// `source: 'casm-doc-tags'`, severity Hint, never anything a real compile error could be mistaken
// for. `source: 'ceresasm'` in server.ts is the only diagnostic with actual authority; this one
// exists to catch a doc comment that has drifted from the macro it describes, nothing more.
//
// Every classification below is conservative on purpose: an operand this can't confidently place
// in one of the six kinds comes back `'unknown'`, and `'unknown'` never triggers a hint. A missed
// real mistake costs nothing; a false positive costs the feature's credibility.

import { Diagnostic, DiagnosticSeverity, Range } from 'vscode-languageserver/node';
import { TextDocument } from 'vscode-languageserver-textdocument';
import { OperandKind } from './docTags';
import { getCleanedLines, SymbolIndexer, VisibleSymbols } from './symbolIndex';

const STATEMENT_HEAD_RE = /^(\s*)([A-Za-z_][A-Za-z0-9_]*)\b/;
const REGISTER_RE = /^(?:r(?:[0-9]|1[0-5])|sp|fp|at|lr)$/i;
const FLOAT_REGISTER_RE = /^f(?:[0-9]|1[0-5])$/i;
const NUMERIC_RE = /^[+-]?(?:0[xX][0-9a-fA-F]+|0[bB][01]+|\d+(?:\.\d+)?(?:[eE][+-]?\d+)?)$/;
const CHAR_LITERAL_RE = /^'(?:\\.|[^'\\])'$/;
const BARE_NAME_RE = /^\.?[A-Za-z_][A-Za-z0-9_]*$/;

interface ArgSpan {
	text: string;
	start: number;
	end: number;
}

// The same bracket-aware top-level comma split `splitTopLevelArguments` (symbolIndex.ts) does,
// kept separate because a diagnostic also needs *where* each argument sits in the line, which
// nothing else needs.
function splitArgumentSpans(text: string, offset: number): ArgSpan[] {
	const spans: ArgSpan[] = [];
	let depth = 0;
	let start = 0;
	for (let i = 0; i <= text.length; i++) {
		const char = text[i];
		if (char === '[') {
			depth++;
		} else if (char === ']') {
			depth = Math.max(0, depth - 1);
		}
		if (i === text.length || (char === ',' && depth === 0)) {
			const raw = text.slice(start, i);
			const trimmed = raw.trim();
			if (trimmed.length > 0) {
				const trimmedStart = start + (raw.length - raw.trimStart().length);
				spans.push({ text: trimmed, start: offset + trimmedStart, end: offset + trimmedStart + trimmed.length });
			}
			start = i + 1;
		}
	}
	return spans;
}

// Register/float-register/immediate/memory operand are read straight off the text; a label or a
// plain constant needs the visible symbol table, because `NAME` alone doesn't say which of the
// two it is. Anything richer than that - `2 + N * 4`, `sizeof(buf)`, an undeclared name - is
// `'unknown'` rather than a guess.
function classifyOperand(text: string, visible: VisibleSymbols): OperandKind | 'unknown' {
	if (text.startsWith('[') && text.endsWith(']')) {
		return 'mem';
	}
	if (REGISTER_RE.test(text)) {
		return 'reg';
	}
	if (FLOAT_REGISTER_RE.test(text)) {
		return 'freg';
	}
	if (NUMERIC_RE.test(text) || CHAR_LITERAL_RE.test(text)) {
		return 'imm';
	}

	if (BARE_NAME_RE.test(text)) {
		if (visible.labels.has(text)) {
			return 'label';
		}
		const constSymbol = visible.consts.get(text);
		if (constSymbol) {
			const value = constSymbol.valueText.trim();
			if (REGISTER_RE.test(value)) {
				return 'reg';
			}
			if (FLOAT_REGISTER_RE.test(value)) {
				return 'freg';
			}
			return 'imm';
		}
	}

	return 'unknown';
}

const KIND_DESCRIPTION: Record<OperandKind, string> = {
	reg: 'a register',
	freg: 'a float register',
	imm: 'an immediate value or constant',
	mem: 'a memory operand',
	label: 'a label',
	any: 'anything'
};

function isClearMismatch(declared: OperandKind, actual: OperandKind | 'unknown'): actual is OperandKind {
	return declared !== 'any' && actual !== 'unknown' && actual !== declared;
}

// One Hint diagnostic per call-site argument whose written shape clearly disagrees with what the
// callee's own `@param` tag declared - only for a macro whose *every* parameter is tagged (a
// partially-tagged macro is exactly the case `macroSignature`/`signatureFor` already refuse to
// treat as typed, for the same reason: don't look more precise than the doc comment actually is).
export function provideDocTagHints(document: TextDocument, indexer: SymbolIndexer): Diagnostic[] {
	const diagnostics: Diagnostic[] = [];
	const lines = getCleanedLines(indexer, document.uri);
	const visible = indexer.collectVisibleSymbols(document.uri);

	for (let lineNumber = 0; lineNumber < lines.length; lineNumber++) {
		const lineText = lines[lineNumber];
		const headMatch = STATEMENT_HEAD_RE.exec(lineText);
		if (!headMatch) {
			continue;
		}

		const candidates = visible.macrosByName.get(headMatch[2]);
		if (!candidates || candidates.length === 0) {
			continue;
		}

		const spans = splitArgumentSpans(lineText.slice(headMatch[0].length), headMatch[0].length);
		const macro = candidates.find((candidate) => candidate.arity === spans.length);
		if (!macro) {
			continue; // No overload takes this many arguments - the real compiler already says so.
		}

		const byName = new Map(macro.docParams.map((tag) => [tag.name, tag] as const));
		const fullyTagged = macro.docParams.length === macro.params.length && macro.params.every((paramName) => byName.has(paramName));
		if (!fullyTagged) {
			continue;
		}

		for (let i = 0; i < spans.length; i++) {
			const tag = byName.get(macro.params[i]);
			if (!tag) {
				continue;
			}

			const actual = classifyOperand(spans[i].text, visible);
			if (!isClearMismatch(tag.kind, actual)) {
				continue;
			}

			diagnostics.push({
				severity: DiagnosticSeverity.Hint,
				range: Range.create(lineNumber, spans[i].start, lineNumber, spans[i].end),
				message:
					`'${macro.name}' documents ${macro.params[i]} as ${KIND_DESCRIPTION[tag.kind]} (\`${tag.kind}\`), but ` +
					`\`${spans[i].text}\` looks like ${KIND_DESCRIPTION[actual]}. This is a hint from the /// doc comment, ` +
					'not something the compiler checked.',
				source: 'casm-doc-tags'
			});
		}
	}

	return diagnostics;
}
