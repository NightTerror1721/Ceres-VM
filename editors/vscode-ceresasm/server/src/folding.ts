import { FoldingRange, FoldingRangeKind } from 'vscode-languageserver/node';
import { TextDocument } from 'vscode-languageserver-textdocument';
import { stripComments } from './symbolIndex';

const SECTION_RE = /^\s*@(text|rodata|data|bss)\b/;
const LABEL_RE = /^\s*(global\s+)?(\.?[A-Za-z_][A-Za-z0-9_]*)\s*:/;
const MACRO_RE = /^\s*macro\b/;
const ENDMACRO_RE = /^\s*endmacro\b/;

function trimTrailingBlankLines(lines: string[], start: number, end: number): number {
	let trimmed = end;
	while (trimmed > start && lines[trimmed].trim().length === 0) {
		trimmed--;
	}
	return trimmed;
}

export function provideFoldingRanges(document: TextDocument): FoldingRange[] {
	const lines = stripComments(document.getText()).split(/\r\n|\r|\n/);
	const ranges: FoldingRange[] = [];

	const sectionLines: number[] = [];
	const labelLines: number[] = [];
	const nonLocalLabelLines: number[] = [];
	for (let i = 0; i < lines.length; i++) {
		if (SECTION_RE.test(lines[i])) {
			sectionLines.push(i);
			continue;
		}
		const labelMatch = LABEL_RE.exec(lines[i]);
		if (labelMatch) {
			labelLines.push(i);
			if (!labelMatch[2].startsWith('.')) {
				nonLocalLabelLines.push(i);
			}
		}
	}

	// Sections have no end marker of their own: each one folds up to the line before the next
	// section directive (or end of file).
	for (let i = 0; i < sectionLines.length; i++) {
		const start = sectionLines[i];
		const end = trimTrailingBlankLines(lines, start, i + 1 < sectionLines.length ? sectionLines[i + 1] - 1 : lines.length - 1);
		if (end > start) {
			ranges.push({ startLine: start, endLine: end, kind: FoldingRangeKind.Region });
		}
	}

	// Global/file-scoped labels fold up to the next label of that same kind (or section, or end of
	// file) - never up to just the next local `.name` label, so a subroutine still folds as one
	// piece even though it starts with a local label on the very next line (the common case).
	const nonLocalBoundaries = [...nonLocalLabelLines, ...sectionLines, lines.length].sort((a, b) => a - b);
	for (const start of nonLocalLabelLines) {
		const nextBoundary = nonLocalBoundaries.find((line) => line > start) ?? lines.length;
		const end = trimTrailingBlankLines(lines, start, nextBoundary - 1);
		if (end > start) {
			ranges.push({ startLine: start, endLine: end, kind: FoldingRangeKind.Region });
		}
	}

	// Local `.name` labels nest inside their enclosing non-local one, folding up to whichever
	// comes first: the next label of any kind, the next section, or end of file.
	const allBoundaries = [...labelLines, ...sectionLines, lines.length].sort((a, b) => a - b);
	for (const start of labelLines) {
		if (nonLocalLabelLines.includes(start)) {
			continue;
		}
		const nextBoundary = allBoundaries.find((line) => line > start) ?? lines.length;
		const end = trimTrailingBlankLines(lines, start, nextBoundary - 1);
		if (end > start) {
			ranges.push({ startLine: start, endLine: end, kind: FoldingRangeKind.Region });
		}
	}

	// macro ... endmacro
	let macroStart: number | null = null;
	for (let i = 0; i < lines.length; i++) {
		if (macroStart === null && MACRO_RE.test(lines[i])) {
			macroStart = i;
		} else if (macroStart !== null && ENDMACRO_RE.test(lines[i])) {
			if (i > macroStart) {
				ranges.push({ startLine: macroStart, endLine: i, kind: FoldingRangeKind.Region });
			}
			macroStart = null;
		}
	}

	// Block comments spanning more than one line. Comments are already blanked to spaces by
	// stripComments, so scan the original text for these instead of the cleaned lines.
	const rawLines = document.getText().split(/\r\n|\r|\n/);
	let blockCommentStart: number | null = null;
	for (let i = 0; i < rawLines.length; i++) {
		const line = rawLines[i];
		if (blockCommentStart === null) {
			const start = line.indexOf('/*');
			if (start === -1) {
				continue;
			}
			const end = line.indexOf('*/', start + 2);
			if (end === -1) {
				blockCommentStart = i;
			}
			// If it closes on the same line, there's nothing to fold.
		} else {
			const end = line.indexOf('*/');
			if (end !== -1) {
				if (i > blockCommentStart) {
					ranges.push({ startLine: blockCommentStart, endLine: i, kind: FoldingRangeKind.Comment });
				}
				blockCommentStart = null;
			}
		}
	}

	return ranges;
}
