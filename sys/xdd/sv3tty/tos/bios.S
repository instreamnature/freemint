|
| bios.S
|
| Entry points for the Bconmap table (maptab) of the sv3tty TOS driver.
|
| Each port has its own Bconstat, Bconin, Bcostat and Bconout entry points,
| which pass the port number to common C functions. They do not read the
| device number from the stack, since it is not always there:
|  - TOS calls a maptab function with the BIOS call's words on the stack:
|    4(sp) = device, 6(sp) = character (Bconout).
|  - HSMODEM's DRVIN.PRG (fast path, for rows unchanged since they were
|    added with Bconmap(-401)) calls Bconstat/Bconin/Bcostat with nothing
|    on the stack, and Bconout at its entry + 4, with a0 pointing to the
|    character.
| So Bconout starts with "lea 6(sp),a0", exactly 4 bytes, as HSMODEM's
| SCC.PRG does: called by TOS, a0 then points to the character; called by
| DRVIN at +4, a0 already does. Both continue with the character at (a0).
|
| The result is returned in d0. d0-d2/a0-a2 may be changed (BIOS
| convention); gcc keeps d2-d7/a2-a6.
|
| Rsconf: the ports have no baud rate etc. Changes are ignored and 0 is
| returned (for speed -2, "current speed", that is 19200 baud).
|

	.text
	.even

	.globl	_sv3t_bios_table
	.globl	_sv3t_rsconf_asm

	.globl	_sv3t_bconstat
	.globl	_sv3t_bconin
	.globl	_sv3t_bcostat
	.globl	_sv3t_bconout


| Entry points of one port
	.macro	PORT_BIOS n
bconstat_\n:
	moveq	#\n,d0
	bra.w	do_bconstat
bconin_\n:
	moveq	#\n,d0
	bra.w	do_bconin
bcostat_\n:
	moveq	#\n,d0
	bra.w	do_bcostat
bconout_\n:
	lea		6(sp),a0			| Must be 4 bytes: DRVIN enters at +4
	moveq	#\n,d1
	bra.w	do_bconout
	.endm

	.irp	n, 0,1,2,3,4,5,6,7,8,9,10,11,12,13,14,15
	PORT_BIOS \n
	.endr


| long sv3t_bconstat(port), port in d0
do_bconstat:
	move.l	d0,-(sp)
	jsr		_sv3t_bconstat
	addq.l	#4,sp
	rts

| long sv3t_bconin(port), port in d0
do_bconin:
	move.l	d0,-(sp)
	jsr		_sv3t_bconin
	addq.l	#4,sp
	rts

| long sv3t_bcostat(port), port in d0
do_bcostat:
	move.l	d0,-(sp)
	jsr		_sv3t_bcostat
	addq.l	#4,sp
	rts

| long sv3t_bconout(port, c), port in d1, a0 points to the character
do_bconout:
	moveq	#0,d0
	move.w	(a0),d0
	move.l	d0,-(sp)
	move.l	d1,-(sp)
	jsr		_sv3t_bconout
	addq.l	#8,sp
	rts


| long Rsconf(speed, flowctl, ucr, rsr, tsr, scr): words at 4(sp)..
| Calls sv3t_rsconf(speed, flowctl) (signed), which returns 0.
	.globl	_sv3t_rsconf

_sv3t_rsconf_asm:
	move.w	4(sp),d0			| speed
	ext.l	d0
	move.w	6(sp),d1			| flowctl
	ext.l	d1
	move.l	d1,-(sp)
	move.l	d0,-(sp)
	jsr		_sv3t_rsconf
	addq.l	#8,sp
	rts


| Bconstat, Bconin, Bcostat, Bconout of each port, for the maptab rows
	.macro	PORT_ROW n
	.dc.l	bconstat_\n, bconin_\n, bcostat_\n, bconout_\n
	.endm

	.even
_sv3t_bios_table:
	.irp	n, 0,1,2,3,4,5,6,7,8,9,10,11,12,13,14,15
	PORT_ROW \n
	.endr


|
| long sv3t_bconmap_add(MAPTAB *row)
|
| HSMODEM's DRVIN.PRG extends XBIOS Bconmap: Bconmap(-401, row) adds a
| device with the functions in row to DRVIN's Bconmap table and returns its
| BIOS device number (6 + index), or -12 when the table is full. Called from
| C; the XBIOS may change d0-d2/a0-a2, and gcc expects d2/a2 to be kept.
|
	.globl	_sv3t_bconmap_add

_sv3t_bconmap_add:
	movem.l	d2/a2,-(sp)
	move.l	12(sp),-(sp)		| row
	move.w	#-401,-(sp)
	move.w	#44,-(sp)			| Bconmap
	trap	#14
	addq.l	#8,sp
	movem.l	(sp)+,d2/a2
	rts
