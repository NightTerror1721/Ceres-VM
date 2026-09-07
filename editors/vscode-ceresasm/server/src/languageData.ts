// Static reference data for hover and completion: mnemonic/register/keyword documentation.
// Sourced from the top-level README's instruction set tables, not re-derived from the compiler,
// since this is pure documentation text rather than behaviour that has to match it exactly.

export interface MnemonicDoc {
	summary: string;
	operands?: string;
	pseudo?: boolean;
}

export const MNEMONICS: Record<string, MnemonicDoc> = {
	// System - 0x00-0x07
	nop: { summary: 'No operation.' },
	halt: {
		summary:
			'Suspends the machine until an interrupt arrives. Arm the timer and enable interrupts with `sti` first, or nothing will wake it.'
	},
	trap: { summary: 'Raises interrupt 1.' },
	reset: { summary: 'Raises interrupt 0, the reset vector.' },
	int: { operands: 'imm8', summary: 'Raises the given interrupt number.' },
	iret: { summary: 'Restores flags and PC from the stack; returns from an interrupt handler.' },
	cli: { summary: 'Clears the Interrupt flag, masking user interrupts (16-63).' },
	sti: { summary: 'Sets the Interrupt flag, unmasking user interrupts (16-63).' },

	// Arithmetic - 0x10-0x28
	add: { operands: 'rd, rs, rt|imm16', summary: 'Addition. A float-register operand selects `fadd` automatically.' },
	adc: { operands: 'rd, rs, rt|imm16', summary: 'Addition with carry.' },
	sub: { operands: 'rd, rs, rt|imm16', summary: 'Subtraction. A float-register operand selects `fsub` automatically.' },
	sbc: { operands: 'rd, rs, rt|imm16', summary: 'Subtraction with borrow.' },
	mul: { operands: 'rd, rs, rt|imm16', summary: 'Unsigned multiplication. A float-register operand selects `fmul`.' },
	imul: { operands: 'rd, rs, rt|s16', summary: 'Signed multiplication.' },
	div: {
		operands: 'rd, rs, rt|imm16',
		summary: 'Unsigned division. A float-register operand selects `fdiv`. Division by zero sets the Trap flag and leaves the destination unchanged.'
	},
	idiv: {
		operands: 'rd, rs, rt|s16',
		summary: 'Signed division. Division by zero sets the Trap flag and leaves the destination unchanged.'
	},
	mod: { operands: 'rd, rs, rt|imm16', summary: 'Unsigned remainder.' },
	imod: { operands: 'rd, rs, rt|s16', summary: 'Signed remainder.' },
	neg: {
		operands: 'rd, rs',
		pseudo: true,
		summary: 'Pseudo-instruction: integer negation. Expands to `imul rd, rs, -1`, or to `fneg` for float registers.'
	},

	// Logic and shifts - 0x30-0x3C
	and: { operands: 'rd, rs, rt|imm16', summary: 'Bitwise AND.' },
	or: { operands: 'rd, rs, rt|imm16', summary: 'Bitwise OR.' },
	xor: { operands: 'rd, rs, rt|imm16', summary: 'Bitwise XOR.' },
	not: { operands: 'rd, rs', summary: 'Bitwise NOT.' },
	shl: { operands: 'rd, rs, rt|imm16', summary: 'Logical shift left. The shift amount uses the low five bits of the operand.' },
	shr: { operands: 'rd, rs, rt|imm16', summary: 'Logical (zero-filling) shift right.' },
	sar: { operands: 'rd, rs, rt|imm16', summary: 'Arithmetic (sign-extending) shift right.' },

	// Memory - 0x40-0x4E
	mov: { operands: 'rd, rs', summary: 'Register-to-register copy. A float-register operand selects `fmov`.' },
	li: { operands: 'rd, imm16', summary: 'Loads a 16-bit immediate, zero-extended.' },
	lui: {
		operands: 'rd, imm16',
		summary: 'Loads a 16-bit immediate shifted left 16 bits. Paired with `ori` by the assembler to build 32-bit addresses (`la`, `ldv`, `stv`).'
	},
	ldr: { operands: 'rd, [rs + imm16]', summary: 'Loads a 32-bit word. A float destination selects `fldr`.' },
	ldrb: { operands: 'rd, [rs + imm16]', summary: 'Loads a byte, zero-extended.' },
	ldrh: { operands: 'rd, [rs + imm16]', summary: 'Loads a half-word, zero-extended.' },
	ldrsb: { operands: 'rd, [rs + imm16]', summary: 'Loads a byte, sign-extended.' },
	ldrsh: { operands: 'rd, [rs + imm16]', summary: 'Loads a half-word, sign-extended.' },
	str: {
		operands: '[rd + imm16], rs',
		summary: 'Stores a 32-bit word. Base address in rd, value in rs: imm16 occupies the field rt would otherwise use. A float source selects `fstr`.'
	},
	strb: { operands: '[rd + imm16], rs', summary: 'Stores a byte, same operand order as `str`.' },
	strh: { operands: '[rd + imm16], rs', summary: 'Stores a half-word, same operand order as `str`.' },
	lea: { operands: 'rd, [rs + imm16]', summary: 'Loads the effective address rs + imm16 into rd, without accessing memory.' },

	// Control flow - 0x50-0x67
	jp: { operands: 'label|rs', summary: 'Unconditional jump. A register operand selects the register form (`jpr`) automatically.' },
	jz: { operands: 'label|rs', summary: 'Jump if the Zero flag is set.' },
	jnz: { operands: 'label|rs', summary: 'Jump if the Zero flag is clear.' },
	jc: { operands: 'label|rs', summary: 'Jump if the Carry flag is set.' },
	jnc: { operands: 'label|rs', summary: 'Jump if the Carry flag is clear.' },
	js: { operands: 'label|rs', summary: 'Jump if the Sign flag is set.' },
	jns: { operands: 'label|rs', summary: 'Jump if the Sign flag is clear.' },
	jo: { operands: 'label|rs', summary: 'Jump if the Overflow flag is set.' },
	jno: { operands: 'label|rs', summary: 'Jump if the Overflow flag is clear.' },
	jmp: { operands: 'label|rs', summary: 'Unconditional jump. Alias of jp.' },
	jeq: { operands: 'label|rs', summary: 'Jump if the compared values were equal. Alias of jz.' },
	jne: { operands: 'label|rs', summary: 'Jump if the compared values differed. Alias of jnz.' },
	jgr: { operands: 'label|rs', summary: 'Jump if greater (signed).' },
	jge: { operands: 'label|rs', summary: 'Jump if greater or equal (signed).' },
	jls: { operands: 'label|rs', summary: 'Jump if less (signed).' },
	jle: { operands: 'label|rs', summary: 'Jump if less or equal (signed).' },
	jab: { operands: 'label|rs', summary: 'Jump if above (unsigned).' },
	jae: { operands: 'label|rs', summary: 'Jump if above or equal (unsigned).' },
	jbl: { operands: 'label|rs', summary: 'Jump if below (unsigned).' },
	jbe: { operands: 'label|rs', summary: 'Jump if below or equal (unsigned).' },
	ifeq: { operands: 'rs, rt|imm, label', summary: 'Compare, then jump if equal.' },
	ifne: { operands: 'rs, rt|imm, label', summary: 'Compare, then jump if not equal.' },
	ifgr: { operands: 'rs, rt|imm, label', summary: 'Compare, then jump if greater (signed).' },
	ifge: { operands: 'rs, rt|imm, label', summary: 'Compare, then jump if greater or equal (signed).' },
	ifls: { operands: 'rs, rt|imm, label', summary: 'Compare, then jump if less (signed).' },
	ifle: { operands: 'rs, rt|imm, label', summary: 'Compare, then jump if less or equal (signed).' },
	ifab: { operands: 'rs, rt|imm, label', summary: 'Compare, then jump if above (unsigned).' },
	ifae: { operands: 'rs, rt|imm, label', summary: 'Compare, then jump if above or equal (unsigned).' },
	ifbl: { operands: 'rs, rt|imm, label', summary: 'Compare, then jump if below (unsigned).' },
	ifbe: { operands: 'rs, rt|imm, label', summary: 'Compare, then jump if below or equal (unsigned).' },
	lc: { operands: 'rd, imm32', summary: 'Load a full 32-bit constant. Expands to lui + ori.' },
	inc: { operands: 'rd', summary: 'Add one. Expands to addi rd, rd, 1.' },
	dec: { operands: 'rd', summary: 'Subtract one. Expands to subi rd, rd, 1.' },
	clr: { operands: 'rd', summary: 'Set to zero. Expands to li rd, 0.' },
	tst: { operands: 'rs', summary: 'Compare against zero. Expands to cmpi rs, 0.' },
	swap: { operands: 'rd, rs', summary: 'Exchange two registers, using no temporary.' },
	enter: { operands: '', summary: 'Open a stack frame: push fp, then mov fp, sp.' },
	leave: { operands: '', summary: 'Close a stack frame: mov sp, fp, then pop fp.' },
	call: { operands: 'label|rs', summary: 'Pushes the return address, then jumps.' },
	ret: { summary: 'Pops the return address into PC.' },
	cmp: { operands: 'ra, rb|imm16', summary: 'Compares and sets flags without writing a result. A float-register operand selects `fcmp`.' },

	// Stack - 0x70-0x75
	push: { operands: 'rs', summary: 'Pushes a register. A float register selects `fpush`.' },
	pop: { operands: 'rd', summary: 'Pops into a register. A float register selects `fpop`.' },
	pushf: { summary: 'Pushes the flags register.' },
	popf: { summary: 'Pops the flags register.' },

	// Conversions - 0x80-0x85
	itof: { operands: 'fd, rs', summary: 'Converts an unsigned 32-bit integer to a float.' },
	iitof: { operands: 'fd, rs', summary: 'Converts a signed 32-bit integer to a float.' },
	ftoi: { operands: 'rd, fs', summary: 'Converts a float to an unsigned 32-bit integer.' },
	ftoii: { operands: 'rd, fs', summary: 'Converts a float to a signed 32-bit integer.' },
	mtf: { operands: 'fd, rs', summary: 'Moves the bit pattern from an integer register into a float register, without converting.' },
	mff: { operands: 'rd, fs', summary: 'Moves the bit pattern from a float register into an integer register, without converting.' },

	// I/O - 0x90-0xA3
	in: { operands: 'rd, imm8|rs', summary: 'Reads a 32-bit word from an I/O port.' },
	inb: { operands: 'rd, imm8|rs', summary: 'Reads a byte from a port, zero-extended.' },
	inh: { operands: 'rd, imm8|rs', summary: 'Reads a half-word from a port, zero-extended.' },
	insb: { operands: 'rd, imm8|rs', summary: 'Reads a byte from a port, sign-extended.' },
	insh: { operands: 'rd, imm8|rs', summary: 'Reads a half-word from a port, sign-extended.' },
	inm: { operands: 'imm8|rs, addr, size', summary: 'Reads a block of `size` bytes from the port into memory at `addr` (DMA-style).' },
	out: { operands: 'imm8|rs, rs', summary: 'Writes a 32-bit word to an I/O port.' },
	outb: { operands: 'imm8|rs, rs', summary: 'Writes a byte to a port.' },
	outh: { operands: 'imm8|rs, rs', summary: 'Writes a half-word to a port.' },
	outm: { operands: 'imm8|rs, addr, size', summary: 'Writes a block of `size` bytes from memory at `addr` to the port.' },

	// Pseudo-instructions
	la: {
		operands: 'rd, symbol',
		pseudo: true,
		summary: 'Loads the 32-bit address of `symbol`. Expands to `lui` + `ori` (8 B); clobbers r12.'
	},
	ldv: {
		operands: 'rd, variable',
		pseudo: true,
		summary: "Loads a global variable's value, picking the load form from its declared type. Expands to `lui` + `ori` + a load (12 B); clobbers r12."
	},
	stv: {
		operands: 'rs, variable',
		pseudo: true,
		summary: "Stores a register into a global variable, picking the store form from its declared type. Expands to `lui` + `ori` + a store (12 B); clobbers r12."
	}
};

export const KEYWORDS: Record<string, string> = {
	const: 'Declares a compile-time constant. Occupies no memory; substituted at its point of use.',
	let: "Declares a variable. Must appear inside a section (`@rodata`, `@data` or `@bss`); an initialiser must fill the declared type exactly.",
	global: 'Marks the declaration that follows as exported - a label, `const`, `let`, `macro` or `struct`. Without it nothing leaves its own file. Required exactly once as `global main:`.',
	macro: 'Begins a macro definition, up to the matching `endmacro`. Parameters are written `$name`; macros are keyed by name and argument count.',
	endmacro: 'Ends a `macro` definition.',
	import: 'Imports another source file, resolved relative to the importing file. Makes its constants and macros visible; import cycles are reported.'
};

export const TYPES: Record<string, string> = {
	u8: 'Unsigned 8-bit integer.',
	u16: 'Unsigned 16-bit integer.',
	u32: 'Unsigned 32-bit integer.',
	i8: 'Signed 8-bit integer.',
	i16: 'Signed 16-bit integer.',
	i32: 'Signed 32-bit integer.',
	f32: 'IEEE-754 32-bit float.',
	char: 'Alias for `u8`.',
	bool: 'Alias for `u8`.',
	string: 'Alias for an unsized `u8[]`.',
	ptr: 'Alias for `u32`: a memory address.',
	port: 'Alias for `u8`: an I/O port number.',
	irq: 'Alias for `u8`: an interrupt vector number, 0-63.',
	byte: 'Alias for `u8`.',
	half: 'Alias for `u16`.',
	word: 'Alias for `u32`.'
};

export const SECTIONS: Record<string, string> = {
	text: 'Code section.',
	rodata: 'Immutable, initialised data.',
	data: 'Mutable, initialised data.',
	bss: 'Mutable, zero-filled at load time. Occupies no space in the file.'
};

// r0-r15 use the low nibble as index; anything outside 0-15 is not a valid register operand.
export function describeRegister(name: string): string | null {
	const match = /^([rRfF])(\d{1,2})$/.exec(name);
	if (!match) {
		return null;
	}
	const index = Number(match[2]);
	if (index < 0 || index > 15) {
		return null;
	}
	const isFloat = match[1].toLowerCase() === 'f';
	if (isFloat) {
		return `32-bit IEEE-754 float register (bank ${index}/15).`;
	}
	switch (index) {
		case 12:
			return 'General purpose. Used as scratch by the assembler when materialising a 32-bit address for `la`, `ldv` and `stv` - do not expect it to survive those.';
		case 13:
			return 'General purpose. Named Link Register (LR) in the README, but nothing in the machine or assembler treats it specially.';
		case 14:
			return 'General purpose. Named Frame Pointer (FP) in the README; defined, but no instruction touches it automatically.';
		case 15:
			return 'Stack pointer (SP). Initialised to the top of memory on reset; the stack grows down.';
		default:
			return `General purpose integer register (bank ${index}/15).`;
	}
}

export const MNEMONIC_NAMES = Object.keys(MNEMONICS);

// Reserved words (registers, mnemonics, CASM keywords, types) can never be a user-declared
// symbol, so every provider that resolves identifiers against the symbol index gates on this
// first - it also means these are never re-classified by semantic tokens, only by the TextMate
// grammar, which already colours them correctly and unambiguously.
export function isReservedWord(text: string): boolean {
	return describeRegister(text) !== null || Boolean(MNEMONICS[text.toLowerCase()]) || text in KEYWORDS || text in TYPES;
}
