|
| ddev.S
|
| GEMDOS device functions of the sv3tty TOS driver, for HSMODEM's DRVIN.PRG.
|
| DRVIN gives plain TOS MagiC's way of installing devices in U:\DEV\:
| Dcntl(0xCD00 = DEV_M_INSTALL, "u:\dev\name", &mx_ddev). An MX_DDEV is a
| table of 12 function pointers (open, close, read, write, stat, seek,
| datime, ioctl, delete, getc, getline, putc). The functions are called
| with their arguments in registers, as MagiC does (seen in HSMODEM's
| SCC.PRG and its RSVF entries):
|     a0 = file descriptor (not used here)
|     read/write: a1 = buffer, d0 = length          -> d0 = bytes done
|     stat:       a1 = unsel, d0 = rwflag, d1 = apcode -> d0 = ready
|     ioctl:      d0 = command (word), a1 = argument -> d0 = 0 or error
| d0-d2/a0-a2 may be changed, all other registers must be kept (gcc keeps
| d2-d7/a2-a6 itself).
|
| Each port has its own entry points, which put the port number in d2 and
| jump to common code that calls the C function (sv3tty_tos.c) with the
| arguments on the stack. sv3t_ddev_table holds one MX_DDEV per port.
|

	.text
	.even

	.globl	_sv3t_ddev_table

	.globl	_sv3t_dd_open
	.globl	_sv3t_dd_close
	.globl	_sv3t_dd_read
	.globl	_sv3t_dd_write
	.globl	_sv3t_dd_stat
	.globl	_sv3t_dd_ioctl


| Entry points of one port
	.macro	PORT_ENTRIES n
dd_open_\n:
	moveq	#\n,d2
	bra.w	dd_open
dd_close_\n:
	moveq	#\n,d2
	bra.w	dd_close
dd_read_\n:
	moveq	#\n,d2
	bra.w	dd_read
dd_write_\n:
	moveq	#\n,d2
	bra.w	dd_write
dd_stat_\n:
	moveq	#\n,d2
	bra.w	dd_stat
dd_ioctl_\n:
	moveq	#\n,d2
	bra.w	dd_ioctl
	.endm

| MX_DDEV of one port: open, close, read, write, stat, seek, datime,
| ioctl, delete, getc, getline, putc
	.macro	PORT_DDEV n
	.dc.l	dd_open_\n, dd_close_\n, dd_read_\n, dd_write_\n, dd_stat_\n
	.dc.l	dd_seek, 0, dd_ioctl_\n, dd_delete, 0, 0, 0
	.endm


	.irp	n, 0,1,2,3,4,5,6,7,8,9,10,11,12,13,14,15
	PORT_ENTRIES \n
	.endr


| long open(port)
dd_open:
	move.l	d2,-(sp)
	jsr		_sv3t_dd_open
	addq.l	#4,sp
	rts

| long close(port)
dd_close:
	move.l	d2,-(sp)
	jsr		_sv3t_dd_close
	addq.l	#4,sp
	rts

| long read(port, buf, len)
dd_read:
	move.l	d0,-(sp)
	move.l	a1,-(sp)
	move.l	d2,-(sp)
	jsr		_sv3t_dd_read
	lea		12(sp),sp
	rts

| long write(port, buf, len)
dd_write:
	move.l	d0,-(sp)
	move.l	a1,-(sp)
	move.l	d2,-(sp)
	jsr		_sv3t_dd_write
	lea		12(sp),sp
	rts

| long stat(port, rwflag, apcode, unsel)
dd_stat:
	move.l	a1,-(sp)
	move.l	d1,-(sp)
	ext.l	d0
	move.l	d0,-(sp)
	move.l	d2,-(sp)
	jsr		_sv3t_dd_stat
	lea		16(sp),sp
	rts

| long ioctl(port, cmd, arg)
dd_ioctl:
	move.l	a1,-(sp)
	and.l	#0xFFFF,d0
	move.l	d0,-(sp)
	move.l	d2,-(sp)
	jsr		_sv3t_dd_ioctl
	lea		12(sp),sp
	rts

| seek: not possible on a serial port, always at 0
dd_seek:
	moveq	#0,d0
	rts

| delete: the device cannot be deleted (EINVFN)
dd_delete:
	moveq	#-32,d0
	rts


	.even
_sv3t_ddev_table:
	.irp	n, 0,1,2,3,4,5,6,7,8,9,10,11,12,13,14,15
	PORT_DDEV \n
	.endr
