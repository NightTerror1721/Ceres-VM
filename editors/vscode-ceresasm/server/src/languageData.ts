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

	// Memory management unit - 0x08-0x0E
	mtp: { operands: 'rs', summary: 'Sets the page directory base register (PTBR) from `rs`, and flushes the TLB.' },
	mfp: { operands: 'rd', summary: 'Reads the page directory base register (PTBR) into `rd`. Zero until the first `mtp`.' },
	pgon: { summary: 'Enables paging: every subsequent load, store and instruction fetch is translated through the MMU.' },
	pgoff: { summary: 'Disables paging: addresses go straight to physical memory again.' },
	invlpg: { operands: 'rs', summary: 'Invalidates the TLB entry for the page containing the address in `rs`, if any.' },
	flpg: { summary: 'Flushes every TLB entry.' },
	mfpf: { operands: 'rd', summary: 'Reads the virtual address that last raised a page fault into `rd` - the CR2 equivalent.' },

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
	mod: { operands: 'rd, rs, rt|imm16', summary: 'Unsigned remainder. A float-register operand selects `fmod` automatically.' },
	imod: { operands: 'rd, rs, rt|s16', summary: 'Signed remainder.' },
	mulh: { operands: 'rd, rs, rt', summary: 'The high 32 bits of an unsigned product. `mul` keeps the low half and drops this one.' },
	imulh: { operands: 'rd, rs, rt', summary: 'The high 32 bits of a signed product.' },
	abs: { operands: 'rd, rs', summary: 'Absolute value of a signed integer. A float-register pair selects `fabs`. |INT_MIN| does not fit, so it answers with INT_MIN and sets Overflow.' },
	min: { operands: 'rd, rs, rt|imm16', summary: 'Unsigned minimum. A float-register operand selects `fmin` automatically.' },
	imin: { operands: 'rd, rs, rt|s16', summary: 'Signed minimum.' },
	max: { operands: 'rd, rs, rt|imm16', summary: 'Unsigned maximum. A float-register operand selects `fmax` automatically.' },
	imax: { operands: 'rd, rs, rt|s16', summary: 'Signed maximum.' },
	sqrt: { operands: 'fd, fs', summary: 'Square root. Float registers only; there is no integer form.' },
	neg: {
		operands: 'rd, rs',
		pseudo: true,
		summary: 'Pseudo-instruction: integer negation. Expands to `imul rd, rs, -1`, or to `fneg` for float registers.'
	},

	// Extended float arithmetic - 0x98-0x9F, 0xD2-0xD6
	// The primitives a software math library needs to build sin/log/exp/pow on top of: range
	// reduction, an explicit rounding mode, sign injection, and a fused multiply-accumulate.
	fmod: { operands: 'fd, fs, ft', summary: 'IEEE remainder of two floats. Same as `mod` with float registers. Traps on a zero divisor, like `fdiv`.' },
	fmin: { operands: 'fd, fs, ft', summary: 'Float minimum. Same as `min` with float registers.' },
	fmax: { operands: 'fd, fs, ft', summary: 'Float maximum. Same as `max` with float registers.' },
	fround: { operands: 'fd, fs', summary: 'Rounds to the nearest integer, ties to even.' },
	ffloor: { operands: 'fd, fs', summary: 'Rounds toward negative infinity.' },
	fceil: { operands: 'fd, fs', summary: 'Rounds toward positive infinity.' },
	ftrunc: { operands: 'fd, fs', summary: 'Truncates toward zero.' },
	fcopysign: { operands: 'fd, fs, ft', summary: 'The magnitude of `fs` with the sign of `ft`.' },
	fma: {
		operands: 'fd, fs, ft',
		summary: 'Fused multiply-accumulate: `fd = fd + fs * ft`. `fd` is read as the accumulator as well as written - the same flags and rounding as `fadd`, applied to `(fd, fs*ft)`.'
	},
	fclass: {
		operands: 'rd, fs',
		summary: 'Writes a one-hot bitmask classifying `fs` into `rd`: negative/positive infinity, normal, subnormal, zero, or NaN. A domain error shows up as one bit instead of a chain of comparisons.'
	},
	frecipe: {
		operands: 'fd, fs',
		summary: 'Reciprocal: `fd = 1 / fs`. Exact, not a hardware-style low-precision estimate - a software VM has no cycle cost to save by approximating. Traps on `fs == 0`, like `fdiv`.'
	},
	frsqrte: {
		operands: 'fd, fs',
		summary: 'Reciprocal square root: `fd = 1 / sqrt(fs)`. Exact, for the same reason as `frecipe`. Traps on `fs == 0`.'
	},

	// Logic and shifts - 0x30-0x3C, 0xD6
	and: { operands: 'rd, rs, rt|imm16', summary: 'Bitwise AND.' },
	or: { operands: 'rd, rs, rt|imm16', summary: 'Bitwise OR.' },
	xor: { operands: 'rd, rs, rt|imm16', summary: 'Bitwise XOR.' },
	not: { operands: 'rd, rs', summary: 'Bitwise NOT.' },
	shl: { operands: 'rd, rs, rt|imm16', summary: 'Logical shift left. The shift amount uses the low five bits of the operand.' },
	shr: { operands: 'rd, rs, rt|imm16', summary: 'Logical (zero-filling) shift right.' },
	sar: { operands: 'rd, rs, rt|imm16', summary: 'Arithmetic (sign-extending) shift right.' },
	rol: { operands: 'rd, rs, rt|imm16', summary: 'Rotate left. The amount uses the low five bits, so a rotation by 32 is a rotation by none.' },
	ror: { operands: 'rd, rs, rt|imm16', summary: 'Rotate right.' },
	clz: { operands: 'rd, rs', summary: 'Counts leading zero bits. Zero has thirty-two of them.' },
	ctz: { operands: 'rd, rs', summary: 'Counts trailing zero bits. Zero has thirty-two of them, the same as `clz`.' },
	popcnt: { operands: 'rd, rs', summary: 'Counts set bits.' },
	bswap: { operands: 'rd, rs', summary: 'Reverses the four bytes of a word.' },
	sxtb: { operands: 'rd, rs', summary: 'Sign-extends the low byte of a register, the way `ldrsb` does for memory.' },
	sxth: { operands: 'rd, rs', summary: 'Sign-extends the low half-word of a register.' },

	// Memory - 0x40-0x4E
	mov: {
		operands: 'destino, origen',
		summary:
			'Mueve cualquier cosa a cualquier sitio - one name for the whole of moving, with the opcode chosen by the shape of the operands: `mov r1, r2`, `mov f1, f2`, `mov r1, 42`, `mov r1, counter` (the address), `mov r1, [counter]` (the contents), `mov [counter], r1`, `mov r1, [r2 + 4]`, `mov [r2 + 4], r1`, `mov r1, u8[r2 + r3]`, `mov f32[r2 + 4], f1`. **The destination is always first**, which is what tells a load from a store. It deliberately does not cover `mtf` vs `itof`: same shape, opposite meanings.'
	},
	li: { operands: 'rd, imm16', summary: 'Loads a 16-bit immediate, zero-extended.' },
	lui: {
		operands: 'rd, imm16',
		summary: 'Loads a 16-bit immediate shifted left 16 bits. Paired with `ori` by the assembler to build 32-bit addresses (`la`, `ldv`, `stv`).'
	},
	ldr: { operands: 'rd, [rs + imm16|rt]', summary: 'Loads a 32-bit word. A float destination selects `fldr`; a register offset is an index and selects `ldrx`; an access type picks the width (`ldr r1, u8[r2]`); and a bracketed symbol loads a variable (`ldr r1, [counter]`).' },
	ldrb: { operands: 'rd, [rs + imm16|rt]', summary: 'Loads a byte, zero-extended. A register offset selects `ldrbx`.' },
	ldrh: { operands: 'rd, [rs + imm16|rt]', summary: 'Loads a half-word, zero-extended. A register offset selects `ldrhx`.' },
	ldrsb: { operands: 'rd, [rs + imm16|rt]', summary: 'Loads a byte, sign-extended.' },
	ldrsh: { operands: 'rd, [rs + imm16|rt]', summary: 'Loads a half-word, sign-extended.' },
	str: {
		operands: '[rd + imm16|rt], rs',
		summary: 'Stores a 32-bit word. **The destination comes first**, like every other instruction. A float source selects `fstr`; a register offset is an index and selects `strx`; an access type picks the width (`str u8[r2], r1`); and a bracketed symbol stores into a variable (`str [counter], r1`).'
	},
	strb: { operands: '[rd + imm16|rt], rs', summary: 'Stores a byte, same operand order as `str`.' },
	strh: { operands: '[rd + imm16|rt], rs', summary: 'Stores a half-word, same operand order as `str`.' },
	lea: { operands: 'rd, [rs + imm16]', summary: 'The old spelling of `la rd, [rs + imm16]`. Still parses; computes rs + imm16 into rd without accessing memory.' },

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
	lc: { operands: 'rd, imm32', pseudo: true, summary: 'The old spelling of `la` with a literal operand. Still parses; `la rd, imm32` is the same instruction.' },
	inc: { operands: 'rd', summary: 'Add one. Expands to addi rd, rd, 1.' },
	dec: { operands: 'rd', summary: 'Subtract one. Expands to subi rd, rd, 1.' },
	clr: { operands: 'rd', summary: 'Set to zero. Expands to li rd, 0.' },
	tst: { operands: 'rs', summary: 'Compare against zero. Expands to cmpi rs, 0.' },
	swap: { operands: 'rd, rs', summary: 'Exchange two registers, using no temporary.' },
	enter: {
		operands: 'imm16?',
		summary: 'Opens a stack frame in one instruction: pushes fp, points it at the saved word, and reserves imm16 bytes. A struct name works directly, since a struct name is its size. Bare `enter` reserves nothing and still saves fp. All or nothing: if the frame does not fit, nothing is pushed.'
	},
	leave: { operands: '', summary: 'Closes the frame: sp = fp, then pops fp. One instruction.' },
	call: { operands: 'label|rs', summary: 'Pushes the return address, then jumps. Reaches +/-8 MiB.' },
	bl: {
		operands: 'rd, label|rs',
		summary:
			'Branch and link: puts the return address in `rd` instead of on the stack, and the return is the `jp rd` that already exists. No memory traffic, so it is what a leaf wants - but nothing saves `rd` for you, so a function that calls anything else must. The labelled form reaches +/-512 KiB, because `rd` costs the displacement four bits.'
	},
	ret: { summary: 'Pops the return address into PC.' },
	cmp: { operands: 'ra, rb|imm16', summary: 'Compares and sets flags without writing a result. A float-register operand selects `fcmp`.' },

	// Stack - 0x70-0x75
	push: { operands: 'rs?', summary: 'Pushes a register. A float register selects `fpush`; **no operand at all** selects `pushf` and pushes the flags. An immediate is deliberately not accepted - that is `pushm`, and letting `push 5` mean a register mask would turn a diagnostic into a silent bug.' },
	pop: { operands: 'rd?', summary: 'Pops into a register. A float register selects `fpop`; no operand selects `popf`.' },
	pushf: { summary: 'Pushes the flags register. `push` with no operand is the same instruction.' },
	popf: { summary: 'Pops the flags register. `pop` with no operand is the same instruction.' },
	pushm: {
		operands: 'imm16',
		summary: 'Pushes every register whose bit is set in the mask - bit n means register n, so 0x0F00 is r8-r11. Stores from the highest set bit down, so `popm` with the same mask restores exactly what it saved. All or nothing: if the whole mask does not fit, nothing is pushed.'
	},
	popm: { operands: 'imm16', summary: 'Pops into every register whose bit is set, r0 first. The mirror of `pushm`.' },

	// Conversions - 0x80-0x85
	itof: { operands: 'fd, rs', summary: 'Converts an unsigned 32-bit integer to a float.' },
	iitof: { operands: 'fd, rs', summary: 'Converts a signed 32-bit integer to a float.' },
	ftoi: { operands: 'rd, fs', summary: 'Converts a float to an unsigned 32-bit integer.' },
	ftoii: { operands: 'rd, fs', summary: 'Converts a float to a signed 32-bit integer.' },
	mtf: { operands: 'fd, rs', summary: 'Moves the bit pattern from an integer register into a float register, without converting.' },
	mff: { operands: 'rd, fs', summary: 'Moves the bit pattern from a float register into an integer register, without converting.' },

	// I/O used to live here (0x90-0xA3, later 0xA0-0xB3): a dedicated `in`/`out` family addressing
	// 256 single-byte ports. It is retired. Devices are reached through ordinary loads and stores
	// now, at an address in the top 16 MiB of the address space - see the MMIO map in
	// docs/07-IO-Devices-and-Ports.md.

	// Pseudo-instructions
	la: {
		operands: 'rd, symbol|imm32|[rs + imm16]',
		pseudo: true,
		summary: 'Puts an address in `rd`, however it is written: the address of a `symbol`, a full 32-bit literal (both `lui` + `ori`, 8 B, targeting `rd`, so neither needs a scratch register), or `[rs + imm16]` computed at run time (one word, the real LEA opcode). `lc` and `lea` are the old spellings.'
	},
	ldv: {
		operands: 'rd, variable',
		pseudo: true,
		summary:
			"Loads a global variable's value, picking the load form from its declared type. **Two sizes:** within +/-32 KiB the linker relaxes it to one PC-relative word, touching no scratch register; further away it is `lui` + `ori` + a load (12 B), and only then does it clobber `at` (r13) - and only when the destination is a float register."
	},
	stv: {
		operands: 'rs, variable',
		pseudo: true,
		summary:
			"Stores a register into a global variable, picking the store form from its declared type. **Two sizes:** within +/-32 KiB the linker relaxes it to one PC-relative word; further away it is `lui` + `ori` + a store (12 B) and clobbers `at` (r13)."
	},
	ldvp: {
		operands: 'rd, variable',
		pseudo: true,
		summary:
			'The PC-relative load asked for by name: always one word, and a variable out of reach is an error instead of a longer instruction. `ldv` already relaxes to this on its own - write `ldvp` where four bytes is a requirement rather than a preference.'
	},
	stvp: {
		operands: 'rs, variable',
		pseudo: true,
		summary: 'The PC-relative store asked for by name. See `ldvp`.'
	}
};

export const KEYWORDS: Record<string, string> = {
	const: 'Declares a compile-time constant. Occupies no memory; substituted at its point of use.',
	let: "Declares a variable. Must appear inside a section (`@rodata`, `@data` or `@bss`); an initialiser must fill the declared type exactly.",
	global: 'Marks the declaration that follows as exported - a label, `const`, `let`, `macro` or `struct`. Without it nothing leaves its own file. Required exactly once as `global main:`.',
	macro: 'Begins a macro definition, up to the matching `endmacro`. Parameters are written `$name`; macros are keyed by name and argument count.',
	endmacro: 'Ends a `macro` definition.',
	import:
		'Imports another source file, resolved relative to the importing file. Makes visible whatever it declares `global`; import cycles are reported. `import "lib/math.casm" as math` names it, and then `math.LIMIT` is answered by exactly that module.',
	as: 'Names an import, so its declarations can be reached as `name.SYMBOL` when two modules would otherwise clash over a name.',
	alias: 'Names a register for the rest of the file: `alias cursor = r5`. Resolved in the parser, so it never leaves its own file and costs nothing at run time.',
	struct: 'Begins a record layout, up to the matching `endstruct`. It reserves no memory: it declares one constant per field holding that field\'s byte offset, plus its own name holding the total size - so `[fp + Frame.count]` is an ordinary displacement, `u8[Frame]` (or just `Frame`) reserves one, and `enter Frame` opens a stack frame of exactly that size.',
	endstruct: 'Ends a `struct` declaration.',
	align: 'Pads the current section up to a boundary: `align 16`. The boundary has to be a power of two.',
	org: 'Pads the current section up to an offset within it: `org 0x100`. It can only move forward - going back would mean overwriting something already emitted.',
	assert:
		'A constant expression that has to hold at assembly time: `assert Frame % 4 == 0`. It emits nothing; it either passes or stops the build with the expression that failed. This is how a rule that used to live in a comment becomes something the machine checks.',
	interrupt:
		'Binds an interrupt number to a handler label: `interrupt UserInterrupt0: timer_isr`. Resolved by the linker like any other operand pair and patched into the vector table by the loader, before the program\'s first instruction runs. Needs no section - it emits no code or data of its own. `interrupt 0` (the reset vector) is rejected; declare `global main` instead.'
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
	port: 'Alias for `u8`. From when devices were reached through port numbers; a device register is an address now, so `ptr` fits that better.',
	irq: 'Alias for `u8`: an interrupt vector number, 0-63.',
	byte: 'Alias for `u8`.',
	half: 'Alias for `u16`.',
	word: 'Alias for `u32`.'
};

// Addresses the linker inserts once the layout is fixed. They are ordinary global labels to
// everything downstream, but nothing declares them, so the symbol index would never find them.
export const LINKER_SYMBOLS: Record<string, string> = {
	__text_start: 'Where `.text` begins. Always `0x400` today.',
	__text_end: 'One past the last byte of `.text`.',
	__rodata_start: 'Where `.rodata` begins.',
	__rodata_end: 'One past the last byte of `.rodata`.',
	__data_start: 'Where `.data` begins.',
	__data_end: 'One past the last byte of `.data`.',
	__bss_start: 'Where `.bss` begins.',
	__bss_end: 'One past the last byte of `.bss`, and the end of the loaded image.',
	__heap_start:
		'The first free byte above the program - the same address as `__bss_end`, under the name that says what it is for. Everything from here up is free ground, with the stack growing down to meet it. There is deliberately no `__stack_top`: the stack starts at the size of memory, which `--memory` picks at run time. Read `sp` on entry instead.'
};

// The interrupt numbers `interrupt NUMBER: handler` accepts by name instead of a bare literal -
// the reserved 0-15 range's named entries, plus UserInterrupt0..47. Predefined as ordinary
// constants in every translation unit by the assembler itself (see
// defineBuiltinInterruptNames in translation_unit.cpp), which is why they're reserved words here
// too: a user `const` of the same name is a redefinition error in the real compiler.
export const INTERRUPT_NUMBERS: Record<string, string> = {
	Trap: 'Interrupt number 1. Raised by the `trap` instruction (equivalent to `int 1`).',
	IllegalInstruction: 'Interrupt number 2. Raised when the fetched opcode does not map to any known instruction.',
	MemoryFault:
		"Interrupt number 3. Raised by a store, or a block read, whose target overlaps the loaded program's `.text`.",
	DivisionByZero:
		'Interrupt number 4. Reserved, but not actually raised by anything: `div`/`idiv`/`mod`/`imod` set the Trap flag directly on a zero divisor instead of dispatching through the vector table.',
	StackOverflow:
		"Interrupt number 5. Raised when a `push`/`call` would write below the protected segment, or a `pop`/`ret` would read past the top of memory - or when an interrupt dispatch itself can't fit the two words it needs to save state.",
	AlignmentFault:
		'Interrupt number 6. Raised by a misaligned 16- or 32-bit memory access (`ldr`/`ldrh`/`str`/`strh`/... and their float forms). Byte-sized accesses never trigger this.',
	Syscall: 'Interrupt number 15. Reserved; nothing raises it yet.'
};

for (let i = 0; i < 48; i++) {
	const raisedBy =
		i === 0
			? ' Raised by the timer device when an armed countdown reaches zero.'
			: i === 1
				? ' Raised by the terminal device when `pushInput()` adds a byte to its input buffer.'
				: '';
	INTERRUPT_NUMBERS[`UserInterrupt${i}`] =
		`Interrupt number ${16 + i}, a user interrupt - masked unless \`sti\` was run.${raisedBy}`;
}

// The port map (and the inlay hints that named one from a bare number) is gone along with the
// in/out family it went with. A device's registers are addresses now, in the MMIO window
// mmio_bus.h reserves - see docs/07-IO-Devices-and-Ports.md.

export const SECTIONS: Record<string, string> = {
	text: 'Code section.',
	rodata: 'Immutable, initialised data.',
	data: 'Mutable, initialised data.',
	bss: 'Mutable, zero-filled at load time. Occupies no space in the file.'
};

// The three registers that have a role answer to it as well as to their number - the assembler
// accepts `sp`, `fp` and `at` anywhere a register can be written. `lr` is the deprecated spelling
// of `at`, kept so that sources written against the old name still parse.
const NAMED_REGISTERS: Record<string, number> = { sp: 15, fp: 14, at: 13, lr: 13 };

// r0-r15 use the low nibble as index; anything outside 0-15 is not a valid register operand.
export function describeRegister(name: string): string | null {
	const named = NAMED_REGISTERS[name.toLowerCase()];
	if (named !== undefined) {
		const description = describeIntegerRegister(named);
		return name.toLowerCase() === 'lr'
			? `${description}

\`lr\` is the deprecated spelling of \`at\`; nothing in the machine links through r13.`
			: description;
	}

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
	return describeIntegerRegister(index);
}

function describeIntegerRegister(index: number): string {
	switch (index) {
		case 13:
			return 'Assembler temporary (`at`). The assembler clobbers it when it has to materialise a 32-bit address - a far `stv`, or a far `ldv` into a float register. Nothing links through it: `call` pushes the return address on the stack.';
		case 14:
			return 'Frame pointer (`fp`). `enter` and `leave` are the only instructions that touch it.';
		case 15:
			return 'Stack pointer (`sp`). Initialised to the top of memory on reset; the stack grows down, and faults at the end of the loaded image.';
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
	return (
		describeRegister(text) !== null ||
		Boolean(MNEMONICS[text.toLowerCase()]) ||
		text in KEYWORDS ||
		text in TYPES ||
		text in LINKER_SYMBOLS ||
		text in INTERRUPT_NUMBERS
	);
}
