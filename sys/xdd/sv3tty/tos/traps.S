|
| traps.S
|
| Trace build only (sv3ttydb.prg): hooks on trap #1 (GEMDOS), trap #13
| (BIOS) and trap #14 (XBIOS), each with an XBRA header, that log every call
| through the C function sv3t_trace_trap() and then continue in the previous
| handler. Used to see what a program calls when it hangs.
|
| On the 68060 the exception frame is format 0, 8 bytes: SR (word), PC
| (long), format/vector (word). The call's arguments are on the user stack
| (USP) when the caller was in user mode, else right above the frame.
|
| d0-d2/a0-a2 are saved, since the call's registers must reach the previous
| handler unchanged (MagiC style device functions and DRVIN use registers).
| The C code uses no floating point.
|

	.text
	.even

	.globl	_sv3t_trap1
	.globl	_sv3t_trap13
	.globl	_sv3t_trap14
	.globl	_sv3t_old_trap1
	.globl	_sv3t_old_trap13
	.globl	_sv3t_old_trap14
	.globl	_sv3t_trace_trap

SAVED	= 24						| Bytes of saved registers

| One hook: n = trap number
	.macro	TRAP_HOOK n
	.dc.l	0x58425241				| 'XBRA'
	.dc.l	0x53563354				| 'SV3T', our id
_sv3t_old_trap\n:
	.dc.l	0						| Previous handler
_sv3t_trap\n:
	movem.l	d0-d2/a0-a2,-(sp)
	move.l	usp,a0					| Arguments on the user stack...
	btst	#5,SAVED(sp)			| ...unless called in supervisor mode
	beq.s	1f
	lea		SAVED+8(sp),a0			| Then they are above the frame
1:
	move.l	SAVED+2(sp),-(sp)		| Caller's PC
	move.l	a0,-(sp)				| Arguments
	move.l	#\n,-(sp)				| Trap number
	jsr		_sv3t_trace_trap
	lea		12(sp),sp
	movem.l	(sp)+,d0-d2/a0-a2
	move.l	_sv3t_old_trap\n(pc),-(sp)	| Continue in the previous handler
	rts
	.endm

	TRAP_HOOK 1
	TRAP_HOOK 13
	TRAP_HOOK 14
