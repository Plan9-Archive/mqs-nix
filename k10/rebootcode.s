#include "mem.h"
#include "amd64l.h"

MODE $64

/*
 * Turn off MMU, then memmove the new kernel to its correct location
 * in physical memory.  Then jumps the to start of the kernel.
 */

TEXT	main(SB), 1, $-4
	MOVL	RARG, DI		/* destination */
	MOVL	p2+8(FP), SI		/* source */
	MOVL	n+16(FP), BX		/* byte count */

	/* load zero length idt */
	MOVL	$_idtptr64p<>(SB), AX
	MOVL	(AX), IDTR

	/* load temporary gdt */
	MOVL	$_gdtptr64p<>(SB), CX
	MOVL	(CX), GDTR
	/* move stack below destination */
	MOVL	DI, SP

	/* load CS with 32bit code segment */
	PUSHQ	$SSEL(SiU32CS, 0)
	PUSHQ	$_warp32<>(SB)
	RETFQ
//
//TEXT	wave(SB), 1, $-4
//	MOVL $0x3f8, DX
//	MOVB $'X', AL
//	OUTB
//	MFENCE
//	RET

MODE $32

TEXT	_warp32<>(SB), 1, $-4

	/* load 32bit data segments */
	MOVL	$SSEL(2, 0), AX
	MOVW	AX, DS
	MOVW	AX, ES
	MOVW	AX, FS
	MOVW	AX, GS
	MOVW	AX, SS

	/* turn off paging */
	MOVL	CR0, AX
	ANDL	$0x7fffffff, AX		/* ~(PG) */
	MOVL	AX, CR0

	MOVL	$0, AX
	MOVL	AX, CR3

	/* disable long mode */
	MOVL	$0xc0000080, CX		/* Extended Feature Enable */
	RDMSR
	ANDL	$0xfffffeff, AX		/* Long Mode Disable */
	WRMSR

	/* diable pae */
	MOVL	CR4, AX
	ANDL	$0xffffff5f, AX		/* ~(PAE|PGE) */
	MOVL	AX, CR4

	MOVL	BX, CX			/* byte count */
	MOVL	DI, AX			/* save entry point */

/*
 * the source and destination may overlap.
 * determine whether to copy forward or backwards
 */
	CMPL	SI, DI
	JGT	_forward
	MOVL	SI, DX
	ADDL	CX, DX
	CMPL	DX, DI
	JGT	_back

_forward:
	CLD
	REP;	MOVSB

_startkernel:
	JMP*	AX

_back:
	ADDL	CX, DI
	ADDL	CX, SI
	SUBL	$1, DI
	SUBL	$1, SI
	STD
	REP;	MOVSB
	JMP	_startkernel

TEXT _gdt<>(SB), 1, $-4
	QUAD	$0
	QUAD	$(SdL|SdG|SdP|Sd4G|SdDPL0|SdS|SdCODE|SdR)	/* 64-bit executable */
	QUAD	$(SdG|SdD|Sd4G|SdP|SdDPL0|SdS|SdW)			/* 32-bit data */
	QUAD	$(SdG|SdD|Sd4G|SdP|SdDPL0|SdS|SdCODE|SdW)	/* 32-bit exec */

TEXT _gdtptr64p<>(SB), 1, $-4
	WORD	$(4*8-1)
	QUAD	$_gdt<>(SB)

TEXT _idtptr64p<>(SB), 1, $-4
	WORD	$0
	QUAD	$0
