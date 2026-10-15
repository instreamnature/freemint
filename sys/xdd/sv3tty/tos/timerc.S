|
| timerc.S
|
| Timer C (200 Hz) interrupt hook for the sv3tty TOS driver.
|
| The handler is put in vector 0x114 with an XBRA header in front of it, so
| other programs can find it and the previous handler. It calls the C
| function sv3t_tick(), then jumps to the previous handler, which does
| TOS' own Timer C work, acknowledges the MFP and ends with RTE.
|
| Only d0-d1/a0-a1 are saved: the C compiler keeps all other registers
| itself. The C code uses no floating point, so the FPU is not touched.
|

	.text
	.even

	.globl	_sv3t_timerc
	.globl	_sv3t_old_timerc
	.globl	_sv3t_tick

	.dc.l	0x58425241				| 'XBRA'
	.dc.l	0x53563354				| 'SV3T', our id
_sv3t_old_timerc:
	.dc.l	0						| Previous Timer C handler
_sv3t_timerc:
	movem.l	d0-d1/a0-a1,-(sp)
	jsr		_sv3t_tick
	movem.l	(sp)+,d0-d1/a0-a1
	move.l	_sv3t_old_timerc(pc),-(sp)	| Continue in the previous handler
	rts
