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
| The trace build (SV3T_TRACE) also stores the interrupted registers, SR,
| PC and the address of the exception frame, so its heartbeat can show
| where the CPU was when the timer came.
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
#ifdef SV3T_TRACE
	movem.l	d0-d7/a0-a6,_sv3t_irq_regs	| Interrupted registers
	move.l	sp,_sv3t_irq_frame			| Exception frame; its end is the old sp
#endif
	movem.l	d0-d1/a0-a1,-(sp)
#ifdef SV3T_TRACE
	| The interrupted SR and PC, from the exception frame above the saved
	| registers
	move.w	16(sp),_sv3t_irq_sr
	move.l	18(sp),_sv3t_irq_pc
#endif
	jsr		_sv3t_tick
	movem.l	(sp)+,d0-d1/a0-a1
	move.l	_sv3t_old_timerc(pc),-(sp)	| Continue in the previous handler
	rts

#ifdef SV3T_TRACE
	.globl	_sv3t_irq_pc
	.globl	_sv3t_irq_sr
	.globl	_sv3t_irq_regs
	.globl	_sv3t_irq_frame

	.bss
	.even
_sv3t_irq_pc:	.ds.l	1			| Interrupted PC
_sv3t_irq_sr:	.ds.w	1			| Interrupted SR
	.even
_sv3t_irq_regs:	.ds.l	15			| Interrupted d0-d7/a0-a6
_sv3t_irq_frame: .ds.l	1			| Address of the exception frame
#endif
