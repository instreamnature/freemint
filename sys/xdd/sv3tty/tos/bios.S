|
| bios.S
|
| Entry points for the Bconmap table (maptab) of the sv3tty TOS driver.
|
| The BIOS (and FreeMiNT's callout1/callout2) calls a maptab function with
| the arguments of the BIOS call as 16 bit words on the stack:
|     4(sp) = device number, 6(sp) = character (Bconout only)
| and takes the result from d0. The C functions take 32 bit arguments, so
| these wrappers read the words and call the C functions. gcc keeps
| d2-d7/a2-a6, and the callers treat d0-d2/a0-a2 as scratch.
|
| Rsconf: the ports have no baud rate etc. Changes are ignored and 0 is
| returned (for speed -2, "current speed", that is 19200 baud).
|

	.text
	.even

	.globl	_sv3t_bconstat_asm
	.globl	_sv3t_bconin_asm
	.globl	_sv3t_bcostat_asm
	.globl	_sv3t_bconout_asm
	.globl	_sv3t_rsconf_asm

	.globl	_sv3t_bconstat
	.globl	_sv3t_bconin
	.globl	_sv3t_bcostat
	.globl	_sv3t_bconout

| long Bconstat(dev)
_sv3t_bconstat_asm:
	moveq	#0,d0
	move.w	4(sp),d0			| Device
	move.l	d0,-(sp)
	jsr		_sv3t_bconstat
	addq.l	#4,sp
	rts

| long Bconin(dev)
_sv3t_bconin_asm:
	moveq	#0,d0
	move.w	4(sp),d0
	move.l	d0,-(sp)
	jsr		_sv3t_bconin
	addq.l	#4,sp
	rts

| long Bcostat(dev)
_sv3t_bcostat_asm:
	moveq	#0,d0
	move.w	4(sp),d0
	move.l	d0,-(sp)
	jsr		_sv3t_bcostat
	addq.l	#4,sp
	rts

| long Bconout(dev, c)
_sv3t_bconout_asm:
	moveq	#0,d0
	moveq	#0,d1
	move.w	6(sp),d0			| Character
	move.w	4(sp),d1			| Device
	move.l	d0,-(sp)
	move.l	d1,-(sp)
	jsr		_sv3t_bconout
	addq.l	#8,sp
	rts

| long Rsconf(speed, flowctl, ucr, rsr, tsr, scr)
_sv3t_rsconf_asm:
	moveq	#0,d0
	rts
