/*
 * Start an Application Processor. This must be placed on a 4KB boundary
 * somewhere in the 1st MB of conventional memory (APBOOTSTRAP). However,
 * due to some shortcuts below it's restricted further to within the 1st
 * 64KB. The AP starts in real-mode, with
 *   CS selector set to the startup memory address/16;
 *   CS base set to startup memory address;
 *   CS limit set to 64KB;
 *   CPL and IP set to 0.
 */
#include "mem.h"

#define NOP		BYTE $0x90		/* NOP */
#define LGDT(gdtptr)	BYTE $0x0F;		/* LGDT */			\
			BYTE $0x01; BYTE $0x16;					\
			WORD $gdtptr
#define FARJUMP16(s, o)	BYTE $0xEA;		/* far jump to ptr16:16 */	\
			WORD $o; WORD $s;					\
			NOP; NOP; NOP
#define FARJUMP32(s, o)	BYTE $0x66;		/* far jump to ptr32:16 */	\
			BYTE $0xEA; LONG $o; WORD $s

#define	DELAY		BYTE $0xEB;		/* JMP .+2 */			\
			BYTE $0x00
#define INVD		BYTE $0x0F; BYTE $0x08
#define WBINVD		BYTE $0x0F; BYTE $0x09

/*
 * Macros for calculating offsets within the page directory base
 * and page tables. Note that these are assembler-specific, producing
 * byte offsets, hence the '*BY2PTE'.
 */
#define PDO(a)		(((((a))>>21) & 0x01FF)*BY2PTE)
#define PTO(a)		(((((a))>>12) & 0x01FF)*BY2PTE)
#define PDPTO(a)	(((((a))>>30) & 3)*BY2PTE)

TEXT apbootstrap(SB), 1, $0
	FARJUMP16(0, _apbootstrap(SB))
TEXT _apvector(SB), 1, $0				/* address APBOOTSTRAP+0x08 */
	LONG $0
TEXT _appdb(SB), 1, $0				/* address APBOOTSTRAP+0x0C */
	LONG $0
TEXT _appdpt(SB), 1, $0				/* address APBOOTSTRAP+0x10 */
	LONG $0
TEXT _apapic(SB), 1, $0				/* address APBOOTSTRAP+0x14 */
	LONG $0
TEXT _apbootstrap(SB), 1, $0			/* address APBOOTSTRAP+0x18 */
	MOVW	CS, AX
	MOVW	AX, DS				/* initialise DS */

	LGDT(gdtptr(SB))			/* load a basic gdt */

	MOVL	CR0, AX
	ORL	$1, AX
	MOVL	AX, CR0				/* turn on protected mode */
	DELAY					/* JMP .+2 */

	BYTE $0xB8; WORD $SELECTOR(1, SELGDT, 0)/* MOVW $SELECTOR(1, SELGDT, 0), AX */
	MOVW	AX, DS
	MOVW	AX, ES
	MOVW	AX, FS
	MOVW	AX, GS
	MOVW	AX, SS

	FARJUMP32(SELECTOR(2, SELGDT, 0), _ap32-KZERO(SB))

/*
 * For Pentiums and higher, the code that enables paging must come from
 * pages that are identity mapped. 
 * To this end double map KZERO at virtual 0 and undo the mapping once virtual
 * nirvana has been obtained.
 */
TEXT _ap32(SB), 1, $0
	MOVL	_appdpt-KZERO(SB), BX		/* physical address of PDPT */
	MOVL	_appdb-KZERO(SB), CX		/* physical address of PDB */
	MOVL	(PDO(KZERO))(CX), DX		/* double-map KZERO at offset 0 in PDB */
	MOVL	DX, (PDO(0))(CX)
	MOVL	(PDPTO(KZERO))(BX), DX		/* double-map KZERO at 0 */
	MOVL	DX, (PDPTO(0))(BX)

	MOVL	CR4, DX
	ORL	$(1<<5), DX	/* PAE */
	MOVL	DX, CR4

	MOVL	BX, CR3				/* load and flush the mmu */

	MOVL	CR0, DX
	ORL	$0x80010000, DX			/* PG|WP */
	ANDL	$~0x6000000A, DX		/* ~(CD|NW|TS|MP) */

	MOVL	$_appg(SB), AX
	MOVL	DX, CR0				/* turn on paging */
	JMP*	AX

TEXT _appg(SB), 1, $0
	MOVL	BX, AX				/* physical address of PDPT */
	ORL	$KZERO, AX
	MOVL	$0, (PDPTO(0))(AX)		/* undo double-map of KZERO at 0 */
	ORL	$KZERO, CX				/* virtual address of PDB page 3 (KSEG) */
	MOVL	$0, (PDO(0))(CX)		/* undo double-map of KZERO in page directory  */
	MOVL	BX, CR3				/* load and flush the mmu */

	MOVL	$(MACHADDR+MACHSIZE-4), SP

	MOVL	$0, AX
	PUSHL	AX
	POPFL

	MOVL	_apapic(SB), AX
	MOVL	AX, (SP)
	MOVL	_apvector(SB), AX
	CALL*	AX
_aphalt:
	HLT
	JMP	_aphalt

TEXT gdt(SB), 1, $0
	LONG $0x0000; LONG $0
	LONG $0xFFFF; LONG $(SEGG|SEGB|(0xF<<16)|SEGP|SEGPL(0)|SEGDATA|SEGW)
	LONG $0xFFFF; LONG $(SEGG|SEGD|(0xF<<16)|SEGP|SEGPL(0)|SEGEXEC|SEGR)
TEXT gdtptr(SB), 1, $0
	WORD	$(3*8-1)
	LONG	$gdt-KZERO(SB)
