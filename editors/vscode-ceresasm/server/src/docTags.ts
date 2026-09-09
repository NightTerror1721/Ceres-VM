// Parses `@param`/`@return` lines out of a `///` doc comment already collected by
// `collectLeadingDoc`. This is advisory metadata only - written by whoever wrote the macro, read
// only by the editor - never something the real assembler sees or checks; see docs on macros
// (`$param` is substituted verbatim, never type-checked) for why that stays true.

export type OperandKind = 'reg' | 'freg' | 'imm' | 'mem' | 'label' | 'any';

const OPERAND_KINDS: readonly OperandKind[] = ['reg', 'freg', 'imm', 'mem', 'label', 'any'];

function isOperandKind(text: string): text is OperandKind {
	return (OPERAND_KINDS as readonly string[]).includes(text);
}

export interface ParamTag {
	name: string; // e.g. `$value`, exactly as written in the macro header
	kind: OperandKind;
	description: string;
}

export interface ReturnTag {
	kind: OperandKind | 'void';
	description: string;
}

export interface ParsedDoc {
	// The prose lines, with every `@param`/`@return` line removed - what hover shows as the
	// symbol's description, same as before this file existed.
	doc: string | undefined;
	params: ParamTag[];
	returns: ReturnTag | undefined;
}

// `@param $value reg` or `@param $value reg - some description text`. `-`/`—` both work.
const PARAM_TAG_RE = /^@param\s+(\S+)\s+(\S+)\s*(?:[-—]\s*(.*))?$/;
const RETURN_TAG_RE = /^@return\s+(\S+)\s*(?:[-—]\s*(.*))?$/;

// A tag line whose kind isn't one of the six recognized ones (a typo, most likely) is left in the
// prose rather than silently dropped - a malformed tag should stay visible, not disappear.
export function parseDocTags(rawDoc: string | undefined): ParsedDoc {
	if (rawDoc === undefined) {
		return { doc: undefined, params: [], returns: undefined };
	}

	const proseLines: string[] = [];
	const params: ParamTag[] = [];
	let returns: ReturnTag | undefined;

	for (const line of rawDoc.split('\n')) {
		const trimmed = line.trim();

		const paramMatch = PARAM_TAG_RE.exec(trimmed);
		if (paramMatch) {
			const [, name, kindText, description] = paramMatch;
			if (isOperandKind(kindText)) {
				params.push({ name, kind: kindText, description: description ?? '' });
				continue;
			}
		}

		const returnMatch = RETURN_TAG_RE.exec(trimmed);
		if (returnMatch) {
			const [, kindText, description] = returnMatch;
			if (kindText === 'void' || isOperandKind(kindText)) {
				returns = { kind: kindText, description: description ?? '' };
				continue;
			}
		}

		proseLines.push(line);
	}

	while (proseLines.length > 0 && proseLines[proseLines.length - 1].trim() === '') {
		proseLines.pop();
	}
	while (proseLines.length > 0 && proseLines[0].trim() === '') {
		proseLines.shift();
	}

	return { doc: proseLines.length > 0 ? proseLines.join('\n') : undefined, params, returns };
}

export function describeOperandKind(kind: OperandKind | 'void'): string {
	switch (kind) {
		case 'reg': return 'register';
		case 'freg': return 'float register';
		case 'imm': return 'immediate or constant expression';
		case 'mem': return 'memory operand';
		case 'label': return 'label';
		case 'any': return 'any operand';
		case 'void': return 'nothing';
	}
}
