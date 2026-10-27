/*
 * sv3tty_tos.c
 *
 * sv3tty driver for plain TOS, a TSR for the AUTO folder: virtual serial
 * ports to PetaLinux on SuperVidel 3 over mailbox channel 1, see
 * PROTOCOL.md and sv3tty_proto.c.
 *
 * The ports are BIOS devices: programs use them with Bconin, Bconout,
 * Bconstat, Bcostat and Iorec, or as AUX: after Bconmap(dev). Their names
 * SV3TTY0..SV3TTY15 are in the RSVF list, where terminal programs look
 * for serial ports.
 *
 * With HSMODEM's DRVIN.PRG loaded (it must come before this driver in the
 * AUTO folder), the ports are installed the way HSMODEM's own drivers
 * (SCC.PRG, MFP.PRG) do it, which terminal programs like CoNnect expect:
 *  - BIOS devices from DRVIN's Bconmap extension, Bconmap(-401, &row),
 *    one per port (DRVIN keeps its own copy of the Bconmap table).
 *  - GEMDOS devices U:\DEV\SV3TTY0.. with Dcntl(0xCD00 = DEV_M_INSTALL),
 *    which DRVIN provides as in MagiC. The device functions are in ddev.S
 *    (entry points) and below (sv3t_dd_*).
 *  - RSVF entries of type 0xE1: port, GEMDOS, BIOS, and a pointer to the
 *    port's function table (MX_DDEV) in the 4 bytes before the name.
 * Without DRVIN, the driver extends the Bconmap table itself (on a Falcon,
 * devices 6..8 = MFP, SCC B, SCC A, so port 0 is device 9) and the RSVF
 * entries are BIOS only (type 0xA0).
 *
 * Install:
 *  - Needs the SupV cookie from sv_xbios.prg (so it must come after
 *    sv_xbios.prg in the AUTO folder) and SuperVidel firmware 300..399.
 *  - Installs the cookie 'SV3T', which points to struct sv3t_info. Tools
 *    (sv3tstat.tos, sv3tclos.ttp, sv3taux.ttp) use it, and it keeps the
 *    driver from being installed twice. sv3ttest.tos only runs with the
 *    driver in its -b mode.
 *  - Hooks Timer C (vector 0x114, XBRA id 'SV3T') and stays resident.
 *
 * Interrupt:
 *  - Each Timer C tick calls sv3p_poll. The tick runs at IPL 6, so the
 *    work per tick is limited (tick_budget messages each way); the rest is
 *    done at the next tick. When idle, a tick reads one mailbox status
 *    register.
 *  - Only the interrupt touches the mailbox. The BIOS and GEMDOS device
 *    functions only use the port buffers (IORECs).
 *
 * Opening and closing (hang up):
 *  - The first BIOS or GEMDOS access to a port opens it (OPEN to Linux).
 *  - The port is closed (CLOSE, Linux hangs up the session) when the last
 *    GEMDOS file on it is closed, when DTR is dropped through ioctl
 *    (TIOCCDTR, TIOCCTLSET, TIOCIBAUD 0), or through info.close_port
 *    (sv3tclos.ttp). The BIOS has no close.
 *  - Bconout waits while the transmit buffer is full. When the link to
 *    Linux is down, characters are thrown away, so a program does not hang
 *    when Linux is not running.
 */

#include <stdio.h>
#include <osbind.h>
#include <mint/mintbind.h>		/* Dcntl */

#include "sv3tty_tos.h"


#define NPORTS				SV3T_MAX_PORTS
#define BUFSIZE				1024		/* Per port and direction */
#define TICK_BUDGET			16			/* Messages per tick, each way */
#define MAX_OLD_MAPTAB		32			/* Bconmap rows of other drivers we can keep */
#define FAKE_BAUDRATE		115200L		/* Speed of a port until a program sets one */

#define TIMERC_VEC			(*(void **)0x114L)	/* Timer C, 200 Hz */
#define XBIOS_VEC			(*(long *)0xB8L)	/* trap #14 */
#define MCH_COOKIE			0x5F4D4348L			/* '_MCH', machine type */
#define XBRA_MAGIC			0x58425241L			/* 'XBRA' */
#define DRVIN_XBRA			0x52535645L			/* 'RSVE', DRVIN's XBRA id */

#define DEV_M_INSTALL		0xCD00		/* Dcntl: install a MagiC style device */
#define EINVFN				(-32L)
#define EBADARG				(-64L)		/* Also ERANGE */

/* ioctl commands (FreeMiNT sys/mint/ioctl.h, HSMODEM SERSOFST.TXT) */
#define FIONREAD			0x4601
#define FIONWRITE			0x4602
#define TIOCFLUSH			0x5408
#define TIOCIBAUD			0x5412
#define TIOCOBAUD			0x5413
#define TIOCCBRK			0x5414
#define TIOCSBRK			0x5415
#define TIOCGFLAGS			0x5416
#define TIOCSFLAGS			0x5417
#define TIOCOUTQ			0x5418
#define TIOCCTLMAP			0x5481
#define TIOCCTLGET			0x5482
#define TIOCCTLSET			0x5483
#define TIONOTSEND			0x5486
#define TIOCCDTR			0x54F9
#define TIOCSDTR			0x54FA

/* Control lines for TIOCCTLMAP/GET/SET (HSMODEM) */
#define TIOCMH_DTR			0x0002
#define TIOCMH_RTS			0x0004
#define TIOCMH_CTS			0x0008
#define TIOCMH_CD			0x0010

#define TF_8BIT_1STOP		0x0001		/* TIOCGFLAGS: 8 data bits, 1 stop bit */

/* Error codes from install() */
#define ERR_INSTALLED		1
#define ERR_NO_SUPV			2
#define ERR_NOT_SV3			3
#define ERR_COOKIE_JAR		4
#define ERR_NO_BCONMAP		5
#define ERR_MAPTAB_SIZE		6
#define ERR_DRVIN_BCONMAP	7

/* One row of the Bconmap table: the BIOS functions of a device */
typedef struct
{
	long			bconstat;
	long			bconin;
	long			bcostat;
	long			bconout;
	long			rsconf;
	SV3P_IOREC		*iorec;		/* Receive IOREC, the transmit IOREC follows it */
} MAPTAB;

/* What Bconmap(-2) returns */
typedef struct
{
	MAPTAB			*maptab;
	short			maptabsize;	/* Rows; row 0 is BIOS device 6 */
} BCONMAP;

/* One entry of the RSVF list (HSMODEM convention, see FreeMiNT's
 * sys/mint/rsvf.h): terminal programs find the serial ports and their
 * names here. An entry without RSVF_PORT is a link: data points to the
 * next list, or is 0 at the end. */
struct rsvf
{
	char			*data;		/* Port name, or link to the next list */
	unsigned char	type;
	unsigned char	res1;
	unsigned char	bdev;		/* BIOS device number */
	unsigned char	res2;
};
#define RSVF_PORT			0x80	/* Entry is a port */
#define RSVF_GEMDOS			0x40	/* The port is a GEMDOS device (U:\DEV\) */
#define RSVF_BIOS			0x20	/* The port is a BIOS device */
#define RSVF_FTABLE			0x01	/* Pointer to the function table before the name */

/* A port name with the pointer to its function table in front of it, as
 * RSVF_FTABLE wants it */
struct port_name
{
	long			*ftable;	/* The port's MX_DDEV */
	char			name[10];	/* "SV3TTY0".."SV3TTY15" */
	char			gemdos[16];	/* "u:\dev\sv3tty0".. */
};

/* mintlib: memory used by the program, for Ptermres */
extern long _PgmSize;

/* timerc.S */
extern void sv3t_timerc(void);
extern void *sv3t_old_timerc;
#ifdef SV3T_TRACE
extern unsigned long sv3t_irq_pc;			/* PC and SR the timer interrupted */
extern unsigned short sv3t_irq_sr;
extern unsigned long sv3t_irq_regs[15];		/* d0-d7/a0-a6 the timer interrupted */
extern unsigned short *sv3t_irq_frame;		/* Its exception frame (8 bytes) */
#endif

/* bios.S: Bconstat, Bconin, Bcostat, Bconout entry points of each port */
extern long sv3t_bios_table[NPORTS][4];
extern void sv3t_rsconf_asm(void);
extern long sv3t_bconmap_add(MAPTAB *row);

/* ddev.S: one MX_DDEV (12 function pointers) per port */
extern long sv3t_ddev_table[NPORTS][12];

static struct sv3p			proto;
static struct sv3p_port		ports[NPORTS];
static unsigned char		bufs[NPORTS][2][BUFSIZE];
static struct sv3t_info		info;
static unsigned short		sv_firmware;
static MAPTAB				new_maptab[MAX_OLD_MAPTAB + NPORTS];
static MAPTAB				port_rows[NPORTS];			/* Our rows, for DRVIN */
static struct rsvf			rsvf_list[NPORTS + 1];		/* Our ports, then a link */
static unsigned char		port_type[NPORTS];			/* RSVF type of each port */
static struct port_name		port_names[NPORTS];
static unsigned char		gemdos_opens[NPORTS];		/* Open GEMDOS files per port */
static unsigned char		dtr[NPORTS];				/* DTR as set through ioctl */
static long					baudrate[NPORTS];			/* Speed set through ioctl, 0 = default */

/* Speeds a program may set (TIOCIBAUD/TIOCOBAUD), ascending, 0 ends. They
 * mean nothing for the link, but terminal programs show them and probe the
 * highest one with a too high value, as HSMODEM's drivers expect. */
static const long			baud_table[] =
{
	300, 600, 1200, 2400, 4800, 9600, 19200, 38400, 57600, 115200, 0
};



/// @brief		Raise the interrupt level to 7 (no interrupts)
/// @return		The old status register, for ipl_restore
static inline unsigned short ipl7(void)
{
	unsigned short sr;

	__asm__ volatile ("move.w %%sr,%0\n\tori.w #0x0700,%%sr" : "=d"(sr) : : "memory");
	return sr;
}


/// @brief		Restore the interrupt level
/// @param sr	Status register from ipl7
static inline void ipl_restore(unsigned short sr)
{
	__asm__ volatile ("move.w %0,%%sr" : : "d"(sr) : "memory");
}


#ifdef SV3T_TRACE
/*
 * Trace build (sv3ttydb.prg): every call of the BIOS and GEMDOS device
 * functions is written as a line of text to port 15, which the driver opens
 * itself; read it on Linux with "cat /dev/ttySV15". The timer interrupt
 * sends it, so the last lines arrive even when a program hangs.
 * Identical lines in a row are written once, followed by "(repeated N)"
 * when another line comes, and every 1000 repeats. Lines that do not fit in
 * the port's buffer are dropped and counted. Values are hex.
 */
#include <stdarg.h>

#define TRACE_PORT			(NPORTS - 1)

static char				trace_line[100];
static char				trace_last[100];
static int				trace_len;
static unsigned long	trace_repeat;
static unsigned long	trace_dropped;

/// @brief		Append a character to the line being built
static void tr_c(char c)
{
	if (trace_len < (int)sizeof(trace_line) - 3)
		trace_line[trace_len++] = c;
}

/// @brief		Append a string
static void tr_s(const char *s)
{
	while (*s)
		tr_c(*s++);
}

/// @brief		Append a space and a number in hex
static void tr_x(unsigned long v)
{
	static const char hex[] = "0123456789abcdef";
	int i, started = 0;

	tr_c(' ');
	for (i = 28; i >= 0; i -= 4)
	{
		int d = (int)((v >> i) & 15);

		if (d || started || i == 0)
		{
			tr_c(hex[d]);
			started = 1;
		}
	}
}

/// @brief		Send a finished text to the trace port, if it fits
static void tr_send(const char *s, int len)
{
	if (sv3p_tx_room(&proto, TRACE_PORT) >= len)
		sv3p_write(&proto, TRACE_PORT, (const unsigned char *)s, len);
	else
		trace_dropped++;
}

/// @brief		Send "(repeated N)" for the repeats so far
static void tr_flush_repeat(void)
{
	char b[40];
	int len = 0, i;
	unsigned long v = trace_repeat;
	char num[12];
	int n = 0;

	if (!trace_repeat)
		return;
	do
	{
		num[n++] = '0' + (char)(v % 10);
		v /= 10;
	} while (v);
	for (i = 0; "  (repeated "[i]; i++)
		b[len++] = "  (repeated "[i];
	while (n)
		b[len++] = num[--n];
	b[len++] = ')';
	b[len++] = '\r';
	b[len++] = '\n';
	tr_send(b, len);
	trace_repeat = 0;
}

/// @brief		Finish the line: send it, or count it as a repeat
static void tr_end(void)
{
	unsigned short sr = ipl7();
	int i;

	trace_line[trace_len] = 0;
	for (i = 0; trace_line[i] && trace_line[i] == trace_last[i]; i++)
		;
	if (trace_line[i] == trace_last[i])
	{
		// Same as the last line
		if (++trace_repeat >= 1000)
			tr_flush_repeat();
	}
	else
	{
		tr_flush_repeat();
		for (i = 0; i <= trace_len; i++)
			trace_last[i] = trace_line[i];
		trace_line[trace_len++] = '\r';
		trace_line[trace_len++] = '\n';
		tr_send(trace_line, trace_len);
	}
	trace_len = 0;
	ipl_restore(sr);
}

/// @brief		Trace a call: name, then n values in hex
static void tr(const char *name, int n, ...)
{
	va_list ap;

	va_start(ap, n);
	tr_s(name);
	while (n--)
		tr_x(va_arg(ap, unsigned long));
	va_end(ap);
	tr_end();
}

#define TR(...)				tr(__VA_ARGS__)

/* traps.S: GEMDOS, BIOS and XBIOS hooks that call sv3t_trace_trap */
extern void sv3t_trap1(void), sv3t_trap13(void), sv3t_trap14(void);
extern void *sv3t_old_trap1, *sv3t_old_trap13, *sv3t_old_trap14;

#define TRAP1_VEC			(*(void **)0x84L)
#define TRAP13_VEC			(*(void **)0xB4L)
#define TRAP14_VEC			(*(void **)0xB8L)

/* System calls are logged once port 0 has been opened through GEMDOS, so
 * the trace is not filled with what happens before the test */
static volatile unsigned char	trap_trace_on;

/// @brief		Log a GEMDOS, BIOS or XBIOS call (traps.S): trap number,
///				function number, the next 8 bytes of arguments as two longs,
///				and the caller's PC. A program stuck in a loop of calls shows
///				up as one line followed by "(repeated N)".
/// @param trap	Trap number: 1, 13 or 14
/// @param args	The call's arguments, function number first
/// @param pc	Address after the trap instruction
void sv3t_trace_trap(long trap, unsigned short *args, long pc)
{
	if (!trap_trace_on)
		return;
	TR("trap fn a b pc", 5, trap, (long)args[0],
	   *(long *)(args + 1), *(long *)(args + 3), pc);
}

static unsigned char	trace_isr_link = 0xFF;

/// @brief		Append a string to an interrupt trace line
static int isr_str(char *b, int len, const char *s)
{
	while (*s && len < 90)
		b[len++] = *s++;
	return len;
}

/// @brief		Append a space and a hex number to an interrupt trace line
static int isr_hex(char *b, int len, unsigned long v)
{
	static const char hex[] = "0123456789abcdef";
	int i, started = 0;

	if (len < 80)
		b[len++] = ' ';
	for (i = 28; i >= 0 && len < 90; i -= 4)
	{
		int d = (int)((v >> i) & 15);

		if (d || started || i == 0)
		{
			b[len++] = hex[d];
			started = 1;
		}
	}
	return len;
}

/// @brief			Append a space and a value as exactly 'digits' hex digits
static int isr_fix(char *b, int len, unsigned long v, int digits)
{
	static const char hex[] = "0123456789abcdef";
	int i;

	if (len + 1 + digits > 96)
		return len;
	b[len++] = ' ';
	for (i = (digits - 1) * 4; i >= 0; i -= 4)
		b[len++] = hex[(v >> i) & 15];
	return len;
}

/// @brief			Send a line: a label and an address, then 'n' values read
///					from it: words if 'words', else longs
static void isr_dump(const char *label, const void *p, int n, int words)
{
	char b[100];
	int len = 0, i;

	len = isr_str(b, len, label);
	len = isr_hex(b, len, (unsigned long)p);
	b[len++] = ':';
	for (i = 0; i < n; i++)
	{
		if (words)
			len = isr_fix(b, len, ((const unsigned short *)p)[i], 4);
		else
			len = isr_fix(b, len, ((const unsigned long *)p)[i], 8);
	}
	b[len++] = '\r';
	b[len++] = '\n';
	tr_send(b, len);
}

static unsigned long	trace_last_pc;
#else
#define TR(...)				((void)0)
#endif


/// @brief		Called from the Timer C interrupt (timerc.S), 200 times a
///				second, at IPL 6. Must stay short; sv3p_poll limits itself
///				to tick_budget messages each way.
void sv3t_tick(void)
{
	info.ticks++;
	sv3p_poll(&proto);

#ifdef SV3T_TRACE
	// Throw away what Linux sends on the trace port (the tty's echo of the
	// trace), so its credit keeps coming back
	{
		unsigned char junk[32];

		while (sv3p_read(&proto, TRACE_PORT, junk, sizeof(junk)) > 0)
			;
	}

	// Heartbeat every 2 s, written from the interrupt itself: shows whether
	// Timer C still runs when a program hangs. Its own buffer is used, since
	// a program may be building trace_line right now; the program side sends
	// with interrupts off, so the two never write at the same time.
	// A line when the link changes, written once it is up. (A line queued
	// before then is thrown away by the resync, which empties the ports.)
	if (proto.link_up != trace_isr_link)
	{
		char b[100];
		int len = 0;

		trace_isr_link = proto.link_up;
		len = isr_str(b, len, "sv3ttydb link");
		len = isr_hex(b, len, proto.link_up);
		len = isr_str(b, len, " drvin");
		len = isr_hex(b, len, info.drvin);
		len = isr_str(b, len, " nbios");
		len = isr_hex(b, len, info.nbios);
		len = isr_str(b, len, " gemdos");
		len = isr_hex(b, len, info.gemdos_ports);
		len = isr_str(b, len, " rsvf");
		len = isr_hex(b, len, info.rsvf_ports);
		b[len++] = '\r';
		b[len++] = '\n';
		tr_send(b, len);
	}

	// Once a second, with where the CPU was when the timer came (timerc.S):
	// during a hang this shows the loop's PC and its interrupt level
	if (info.ticks % 200 == 0)
	{
		char b[100];
		int len = 0;

		len = isr_str(b, len, "beat t");
		len = isr_hex(b, len, info.ticks);
		len = isr_str(b, len, " hz");
		len = isr_hex(b, len, *(volatile unsigned long *)0x4baL);
		len = isr_str(b, len, " tx");
		len = isr_hex(b, len, proto.tx_msgs);
		len = isr_str(b, len, " drop");
		len = isr_hex(b, len, trace_dropped);
		len = isr_str(b, len, " pc");
		len = isr_hex(b, len, sv3t_irq_pc);
		len = isr_str(b, len, " sr");
		len = isr_hex(b, len, sv3t_irq_sr);
		len = isr_str(b, len, " rw");		// hdv_rw: the hard disk driver's Rwabs
		len = isr_hex(b, len, *(volatile unsigned long *)0x476L);
		b[len++] = '\r';
		b[len++] = '\n';
		tr_send(b, len);

		// Same PC as a second ago: probably a loop, so show its registers,
		// the code around the PC and the top of its stack
		if (sv3t_irq_pc == trace_last_pc && !(sv3t_irq_pc & 1))
		{
			const unsigned short *pc = (const unsigned short *)sv3t_irq_pc;

			isr_dump("d", &sv3t_irq_regs[0], 8, 0);
			isr_dump("a", &sv3t_irq_regs[8], 7, 0);
			isr_dump("code", pc - 8, 16, 1);
			isr_dump("code", pc + 8, 16, 1);
			isr_dump("stack", sv3t_irq_frame + 4, 8, 0);
			isr_dump("stack", sv3t_irq_frame + 20, 8, 0);
		}
		trace_last_pc = sv3t_irq_pc;
	}
#endif
}



/* ================================================================== */
/* Opening and closing ports                                          */
/* ================================================================== */

/// @brief		Open a port if it is not open yet (OPEN to Linux, sent by the
///				timer interrupt). Interrupts are off while the port is set
///				up, since the timer interrupt reads the port state.
/// @param i	Port number
static void port_open(int i)
{
	unsigned short sr;

	if (ports[i].open)
		return;
	sr = ipl7();
	if (!ports[i].open)
		sv3p_open(&proto, (unsigned short)i, bufs[i][0], BUFSIZE, bufs[i][1], BUFSIZE, 0, 0);
	dtr[i] = 1;
	ipl_restore(sr);
	TR("port_open", 1, (long)i);
}


/// @brief		Close a port: the timer interrupt sends CLOSE to Linux, which
///				hangs up the session on the tty. The next access opens the
///				port again (and Linux starts a fresh getty). Also reached
///				through info.close_port; supervisor mode only.
/// @param port	Port number
/// @return		0, or -1 for a bad port
static long close_port(long port)
{
	unsigned short sr;

	if ((unsigned long)port >= NPORTS)
		return -1;
	sr = ipl7();
	sv3p_close(&proto, (unsigned short)port);
	dtr[port] = 0;
	ipl_restore(sr);
	TR("port_close", 1, port);
	return 0;
}


/// @brief		Read received bytes. sv3p_read also updates the credit to
///				return to Linux, which the timer interrupt sends and clears,
///				so interrupts are off briefly.
/// @param i	Port number
/// @param buf	Destination
/// @param len	Max number of bytes
/// @return		Number of bytes read
static long port_read(int i, unsigned char *buf, long len)
{
	unsigned short sr;
	long n;

	sr = ipl7();
	n = sv3p_read(&proto, (unsigned short)i, buf, len);
	ipl_restore(sr);
	return n;
}



/* ================================================================== */
/* BIOS functions, called through bios.S                              */
/* ================================================================== */

/// @brief			Rsconf (bios.S): the ports have no baud rate etc. Changes
///					are ignored and 0 is returned (for speed -2, "current
///					speed", that is 19200 baud).
/// @param speed	Speed code, -1 = no change, -2 = ask
/// @param flowctl	Flow control
/// @return			0
long sv3t_rsconf(long speed, long flowctl)
{
	TR("rsconf speed flow", 2, speed, flowctl);
	(void)speed;
	(void)flowctl;
	return 0;
}


/*
 * Each port has its own entry points in bios.S, which give the port number;
 * the device number is not used (DRVIN does not pass it). The first access
 * opens the port, since the BIOS has no open.
 */

/// @brief		Bconstat: is a character waiting?
/// @param port	Port number
/// @return		-1 if a character is waiting, 0 otherwise
long sv3t_bconstat(long port)
{
	long r;

	port_open((int)port);
	r = sv3p_rx_count(&proto, (unsigned short)port) ? -1 : 0;
	TR("bconstat", 2, port, r);
	return r;
}


/// @brief		Bconin: wait for a character and return it
/// @param port	Port number
/// @return		The character (0..255)
long sv3t_bconin(long port)
{
	unsigned char c;

	port_open((int)port);
	TR("bconin> wait", 1, port);
	while (!port_read((int)port, &c, 1))
		;
	TR("bconin< got", 2, port, (long)c);
	return c;
}


/// @brief		Bcostat: can a character be sent?
/// @param port	Port number
/// @return		-1 if there is room (or the link is down, then Bconout
///				throws the character away), 0 otherwise
long sv3t_bcostat(long port)
{
	long r;

	port_open((int)port);
	if (!proto.link_up)
		r = -1;
	else
		r = sv3p_tx_room(&proto, (unsigned short)port) ? -1 : 0;
	TR("bcostat", 2, port, r);
	return r;
}


/// @brief		Bconout: send a character. Waits while the transmit buffer is
///				full; the timer interrupt empties it. With the link down the
///				character is thrown away.
/// @param port	Port number
/// @param c	Character
/// @return		-1
long sv3t_bconout(long port, long c)
{
	unsigned char b = (unsigned char)c;

	port_open((int)port);
	TR("bconout", 2, port, (long)b);
	while (!sv3p_write(&proto, (unsigned short)port, &b, 1))
	{
		if (!proto.link_up)
			break;
	}
	return -1;
}



/* ================================================================== */
/* GEMDOS device functions (DRVIN), called through ddev.S            */
/* ================================================================== */

/// @brief		A program opens U:\DEV\SV3TTYn
/// @param port	Port number
/// @return		0
long sv3t_dd_open(long port)
{
	port_open((int)port);
	if (gemdos_opens[port] < 255)
		gemdos_opens[port]++;
	TR("dd_open", 2, port, (long)gemdos_opens[port]);
#ifdef SV3T_TRACE
	if (port == 0)
		trap_trace_on = 1;
#endif
	return 0;
}


/// @brief		A program closes U:\DEV\SV3TTYn. The last close hangs up the
///				port, like a modem line with HUPCL.
/// @param port	Port number
/// @return		0
long sv3t_dd_close(long port)
{
	TR("dd_close", 2, port, (long)gemdos_opens[port]);
	if (gemdos_opens[port] && --gemdos_opens[port] == 0)
		close_port(port);
	return 0;
}


/// @brief		Read: what is waiting, at most len bytes. Does not wait, like
///				HSMODEM's drivers; programs use FIONREAD or stat to see if
///				there is something.
/// @param port	Port number
/// @param buf	Destination
/// @param len	Max number of bytes
/// @return		Number of bytes read
long sv3t_dd_read(long port, unsigned char *buf, long len)
{
	long n;

	port_open((int)port);
	n = port_read((int)port, buf, len);
	TR("dd_read buf len n", 4, port, (long)buf, len, n);
	return n;
}


/// @brief		Write: as much as fits, at most len bytes. Does not wait.
///				With the link down the bytes are thrown away.
/// @param port	Port number
/// @param buf	Bytes
/// @param len	Number of bytes
/// @return		Number of bytes taken
long sv3t_dd_write(long port, const unsigned char *buf, long len)
{
	long n;

	port_open((int)port);
	if (!proto.link_up)
		n = len;
	else
		n = sv3p_write(&proto, (unsigned short)port, buf, len);
	TR("dd_write buf len n first", 5, port, (long)buf, len, n, len > 0 ? (long)buf[0] : -1L);
	return n;
}


/// @brief			Is the port ready for reading or writing?
/// @param port		Port number
/// @param rwflag	0 = read, otherwise write
/// @param apcode	Process to wake up later (not used under TOS)
/// @param unsel	Gets the result too, if not 0
/// @return			1 if ready, 0 otherwise
long sv3t_dd_stat(long port, long rwflag, long apcode, long *unsel)
{
	long ready;

	(void)apcode;
	port_open((int)port);
	if (rwflag)
		ready = (!proto.link_up || sv3p_tx_room(&proto, (unsigned short)port)) ? 1 : 0;
	else
		ready = sv3p_rx_count(&proto, (unsigned short)port) ? 1 : 0;
	if (unsel)
		*unsel = ready;
	TR("dd_stat rw ap unsel r", 5, port, rwflag, apcode, (long)unsel, ready);
	return ready;
}


/// @brief		ioctl on U:\DEV\SV3TTYn: the serial port commands of HSMODEM,
///				as FreeMiNT's scc.xdd implements them. There is no real line,
///				so speed and format have no effect (a speed must still be one
///				in baud_table); dropping DTR hangs up the port.
/// @param port	Port number
/// @param cmd	Command
/// @param arg	Argument
/// @return		0, EBADARG for a speed not in baud_table, or EINVFN for
///				unknown commands
static long dd_ioctl(long port, long cmd, long *arg)
{
	struct sv3p_port *p = &ports[port];
	long n;
	int i;

	port_open((int)port);

	switch (cmd)
	{
	case FIONREAD:
		*arg = sv3p_rx_count(&proto, (unsigned short)port);
		return 0;

	case FIONWRITE:
		*arg = proto.link_up ? sv3p_tx_room(&proto, (unsigned short)port) : BUFSIZE - 1;
		return 0;

	case TIOCOUTQ:
	case TIONOTSEND:
		*arg = (BUFSIZE - 1) - sv3p_tx_room(&proto, (unsigned short)port);
		return 0;

	case TIOCIBAUD:
	case TIOCOBAUD:
		// -1: ask for the speed; 0: drop DTR (hang up)
		if (*arg == -1)
		{
			*arg = baudrate[port] ? baudrate[port] : FAKE_BAUDRATE;
			return 0;
		}
		if (*arg == 0)
		{
			close_port(port);
			return 0;
		}

		// Set the speed. A speed not in the table is refused, as scc.xdd
		// does: the next lower one (or the lowest) is returned in *arg with
		// EBADARG. Programs probe the highest speed this way.
		for (i = 0; baud_table[i]; i++)
		{
			if (*arg == baud_table[i])
			{
				baudrate[port] = *arg;
				return 0;
			}
		}
		n = baud_table[0];
		for (i = 0; baud_table[i]; i++)
		{
			if (*arg > baud_table[i])
				n = baud_table[i];
		}
		*arg = n;
		return EBADARG;

	case TIOCGFLAGS:
		*(unsigned short *)arg = TF_8BIT_1STOP;
		return 0;

	case TIOCSFLAGS:
	case TIOCFLUSH:
	case TIOCCBRK:
	case TIOCSBRK:
		return 0;

	case TIOCCTLMAP:
		// Lines we know: DTR (hang up), RTS, CTS, CD (Linux side open)
		arg[0] = TIOCMH_DTR | TIOCMH_RTS | TIOCMH_CTS | TIOCMH_CD;
		for (i = 1; i < 6; i++)
			arg[i] = 0;
		return 0;

	case TIOCCTLGET:
		n = TIOCMH_RTS | TIOCMH_CTS;
		if (dtr[port])
			n |= TIOCMH_DTR;
		if (p->carrier)
			n |= TIOCMH_CD;
		*arg = n;
		return 0;

	case TIOCCTLSET:
		// arg[0] = mask of lines to change, arg[1] = their new values
		if (arg[0] & TIOCMH_DTR)
		{
			if (arg[1] & TIOCMH_DTR)
				port_open((int)port);
			else
				close_port(port);
		}
		return 0;

	case TIOCCDTR:
		close_port(port);
		return 0;

	case TIOCSDTR:
		port_open((int)port);
		return 0;
	}
	return EINVFN;
}


/// @brief		ioctl entry point (ddev.S), see dd_ioctl. The trace build logs
///				the argument's first long before and after the call.
/// @param port	Port number
/// @param cmd	Command
/// @param arg	Argument
/// @return		0, or EINVFN for unknown commands
long sv3t_dd_ioctl(long port, long cmd, long *arg)
{
#ifdef SV3T_TRACE
	long before = arg ? *arg : 0;
	long r = dd_ioctl(port, cmd, arg);

	TR("dd_ioctl cmd arg before after r", 6, port, cmd, (long)arg, before, arg ? *arg : 0, r);
	return r;
#else
	return dd_ioctl(port, cmd, arg);
#endif
}



/* ================================================================== */
/* Installation                                                       */
/* ================================================================== */

/// @brief			Add a cookie to the cookie jar. Supervisor mode, with
///					interrupts off.
/// @param id		Cookie id
/// @param value	Cookie value
/// @return			1 on success, 0 if there is no jar or it is full
static int add_cookie(long id, long value)
{
	long *c = *(long **)0x5a0L;
	long n = 0;

	if (!c)
		return 0;

	// Find the end marker. Its value is the size of the jar in entries.
	while (c[n * 2])
		n++;
	if (n + 1 >= c[n * 2 + 1])
		return 0;

	// New end marker one entry further, then our cookie in place of the old
	c[n * 2 + 2] = 0;
	c[n * 2 + 3] = c[n * 2 + 1];
	c[n * 2 + 1] = value;
	c[n * 2]     = id;
	return 1;
}


/// @brief		Free entries in the cookie jar. Supervisor mode.
/// @return		Number of cookies that can still be added
static long cookie_jar_free(void)
{
	long *c = *(long **)0x5a0L;
	long n = 0;

	if (!c)
		return 0;
	while (c[n * 2])
		n++;
	// The end marker's value is the size of the jar; the marker itself
	// takes one entry
	return c[n * 2 + 1] - n - 1;
}


/// @brief			Change the value of an existing cookie. Supervisor mode.
/// @param id		Cookie id
/// @param value	New value
/// @return			1 if the cookie was found, 0 otherwise
static int set_cookie_value(long id, long value)
{
	long *c = *(long **)0x5a0L;

	if (!c)
		return 0;
	for (; c[0]; c += 2)
	{
		if (c[0] == id)
		{
			c[1] = value;
			return 1;
		}
	}
	return 0;
}


/// @brief		Is HSMODEM's DRVIN.PRG loaded? It hooks the XBIOS (for its
///				Bconmap extension) with the XBRA id 'RSVE'. The XBRA chain
///				of trap #14 is followed: before each handler are 'XBRA', the
///				id and the address of the next handler.
/// @return		1 if DRVIN is found
static int drvin_loaded(void)
{
	long v = XBIOS_VEC;
	int n;

	for (n = 0; n < 32 && v; n++)
	{
		long *x = (long *)v;

		if (x[-3] != XBRA_MAGIC)
			return 0;
		if (x[-2] == DRVIN_XBRA)
			return 1;
		v = x[-1];
	}
	return 0;
}


/// @brief		Fill in our Bconmap rows and make the IORECs valid. Programs
///				may look at the IORECs through Iorec before a port is opened.
static void init_rows(void)
{
	int i;

	for (i = 0; i < NPORTS; i++)
	{
		MAPTAB *m = &port_rows[i];
		SV3P_IOREC *r = SV3P_RX(&ports[i]);

		m->bconstat = sv3t_bios_table[i][0];
		m->bconin   = sv3t_bios_table[i][1];
		m->bcostat  = sv3t_bios_table[i][2];
		m->bconout  = sv3t_bios_table[i][3];
		m->rsconf   = (long)sv3t_rsconf_asm;
		m->iorec    = r;				// io[0], io[1] follows

		r[0].bufaddr = bufs[i][0];
		r[0].buflen  = BUFSIZE;
		r[1].bufaddr = bufs[i][1];
		r[1].buflen  = BUFSIZE;
	}
}


/// @brief		BIOS devices without DRVIN: add our rows to the end of the
///				Bconmap table. A new, larger table is built in our memory
///				with the old rows copied; the old table is left as it is,
///				since others may point into it.
/// @return		0, or an ERR_ code
static long install_bconmap(void)
{
	BCONMAP *bm;
	long mch = 0;
	short old;
	unsigned short sr;
	int i;

	// Bconmap(0) returns 0 when Bconmap exists
	if (Bconmap(0) != 0)
		return ERR_NO_BCONMAP;
	bm = (BCONMAP *)Bconmap(-2);

	// Falcon TOS reports 1 row, but has 3 (MFP, SCC B, SCC A). FreeMiNT
	// corrects it the same way (init.c). Another size means another driver
	// has already extended the table, and then it is right.
	old = bm->maptabsize;
	sv3t_get_cookie(MCH_COOKIE, &mch);
	if (old == 1 && (mch >> 16) == 3)
		old = 3;
	if (old < 0 || old > MAX_OLD_MAPTAB)
		return ERR_MAPTAB_SIZE;

	for (i = 0; i < old; i++)
		new_maptab[i] = bm->maptab[i];
	for (i = 0; i < NPORTS; i++)
	{
		new_maptab[old + i] = port_rows[i];
		info.bdev[i] = (unsigned char)(6 + old + i);
	}
	info.nbios      = NPORTS;
	info.old_maptab = old;

	sr = ipl7();
	bm->maptab     = new_maptab;
	bm->maptabsize = old + NPORTS;
	ipl_restore(sr);

	return 0;
}


/// @brief		BIOS devices with DRVIN: Bconmap(-401, &row) adds one device
///				to DRVIN's Bconmap table and returns its number (6 + index),
///				as HSMODEM's SCC.PRG does it. DRVIN's table has room for 10
///				devices (6..15) and answers -12 when it is full, so the
///				ports that do not fit get no BIOS device; they are GEMDOS
///				devices only. Rows cannot be taken out of DRVIN's table
///				again, so once one is added the driver must stay installed.
/// @return		0, or ERR_DRVIN_BCONMAP if not even one port got a device
static long install_bconmap_drvin(void)
{
	long d;
	int i;

	for (i = 0; i < NPORTS; i++)
	{
		d = sv3t_bconmap_add(&port_rows[i]);
		if (d < 6 || d > 255)
			break;
		info.bdev[i] = (unsigned char)d;
	}
	info.nbios      = (unsigned char)i;
	info.old_maptab = 0;
	return i ? 0 : ERR_DRVIN_BCONMAP;
}


/// @brief		GEMDOS devices with DRVIN: U:\DEV\SV3TTYn through
///				Dcntl(DEV_M_INSTALL). If a name is taken (e.g. from an
///				earlier run), it is deleted and tried once more, as
///				HSMODEM's SCC.PRG does.
/// @return		Number of ports installed in U:\DEV
static int install_gemdos(void)
{
	int i, n = 0;

	for (i = 0; i < NPORTS; i++)
	{
		char *g = port_names[i].gemdos;
		long r;

#ifdef SV3T_TRACE
		if (i == TRACE_PORT)
			continue;			// Used for the trace
#endif
		r = Dcntl(DEV_M_INSTALL, g, (long)sv3t_ddev_table[i]);
		if (r < 0)
		{
			(void)Fdelete(g);
			r = Dcntl(DEV_M_INSTALL, g, (long)sv3t_ddev_table[i]);
		}
		if (r >= 0)
		{
			port_type[i] |= RSVF_GEMDOS | RSVF_FTABLE;
			n++;
		}
	}
	return n;
}


/// @brief		Port names: "SV3TTYn" for RSVF, "u:\dev\sv3ttyn" for GEMDOS,
///				and the RSVF type of each port (BIOS only for now;
///				install_gemdos adds the GEMDOS flags)
static void init_names(void)
{
	static const char upper[] = "SV3TTY", lower[] = "u:\\dev\\sv3tty";
	int i, k;

	for (i = 0; i < NPORTS; i++)
	{
		struct port_name *pn = &port_names[i];
		char num[3] = { 0, 0, 0 };
		int len = 0;

		if (i >= 10)
			num[len++] = '0' + i / 10;
		num[len++] = '0' + i % 10;
		num[len] = 0;

		for (k = 0; upper[k]; k++)
			pn->name[k] = upper[k];
		pn->name[k] = num[0]; pn->name[k + 1] = num[1]; pn->name[k + 2] = num[2];

		for (k = 0; lower[k]; k++)
			pn->gemdos[k] = lower[k];
		pn->gemdos[k] = num[0]; pn->gemdos[k + 1] = num[1]; pn->gemdos[k + 2] = num[2];

		pn->ftable   = sv3t_ddev_table[i];
		port_type[i] = RSVF_PORT | (info.bdev[i] ? RSVF_BIOS : 0);
	}
}


/// @brief		Put our ports in the RSVF list, so terminal programs find
///				them by name. If an RSVF cookie exists (e.g. from DRVIN),
///				our list ends with a link to the old list and the cookie
///				points to ours; otherwise our list ends there and the cookie
///				is added. Supervisor mode, interrupts off.
///
///				Only complete ports are listed: with DRVIN those that are
///				both BIOS and GEMDOS devices (type 0xE1, as HSMODEM's own
///				ports), without DRVIN those with a BIOS device. With all 16
///				ports listed, CoNnect ignored the whole list and fell back
///				to its own port list. Not yet known which of these it did
///				not accept: ports without a BIOS device or without GEMDOS,
///				8 character names (SV3TTY10..), or 18 entries in all.
///				HSMODEM's own lists have none of them.
/// @return		1 on success, 0 if the cookie jar is full
static int install_rsvf(void)
{
	unsigned char want = info.drvin ? (RSVF_BIOS | RSVF_GEMDOS) : RSVF_BIOS;
	long old = 0;
	int i, n = 0;

	for (i = 0; i < NPORTS; i++)
	{
		if ((port_type[i] & want) != want)
			continue;
		rsvf_list[n].data = port_names[i].name;
		rsvf_list[n].type = port_type[i];
		rsvf_list[n].res1 = 0;
		rsvf_list[n].bdev = info.bdev[i];
		rsvf_list[n].res2 = 0;
		n++;
	}
	info.rsvf_ports = (unsigned char)n;

	// Last entry: a link to the old list, or the end of the list
	sv3t_get_cookie(RSVF_COOKIE, &old);
	rsvf_list[n].data = (char *)old;
	rsvf_list[n].type = 0;

	if (old)
		return set_cookie_value(RSVF_COOKIE, (long)rsvf_list);
	return add_cookie(RSVF_COOKIE, (long)rsvf_list);
}


/// @brief		Install the driver. Called through Supexec, since the cookie
///				jar, the SuperVidel registers and the vectors need supervisor
///				mode.
/// @return		0, or an ERR_ code
static long install(void)
{
	unsigned short sr;
	long r;
	int ok;

	if (sv3t_get_cookie(SV3T_COOKIE, 0))
		return ERR_INSTALLED;

	// The SupV cookie tells that a SuperVidel is there, so its registers
	// can be read without a bus error
	if (!sv3t_get_cookie(SUPV_COOKIE, 0))
		return ERR_NO_SUPV;
	sv_firmware = (unsigned short)(SV3T_SV_VERSION & 0x3FF);
	if (sv_firmware < 300 || sv_firmware > 399)
		return ERR_NOT_SV3;

	// Protocol state, with a small budget per timer tick
	sv3p_init(&proto, ports, NPORTS);
	proto.rx_budget = TICK_BUDGET;
	proto.tx_budget = TICK_BUDGET;

	info.magic       = SV3T_COOKIE;
	info.version     = SV3T_INFO_VERSION;
	info.nports      = NPORTS;
	info.sv_firmware = sv_firmware;
	info.tick_budget = TICK_BUDGET;
	info.proto       = &proto;
	info.ticks       = 0;
	info.close_port  = close_port;
	info.drvin       = (unsigned char)drvin_loaded();

	// Room for our cookie, and for RSVF if there is none yet. Checked first,
	// since rows given to DRVIN cannot be taken back: after that the driver
	// must stay installed.
	if (cookie_jar_free() < (sv3t_get_cookie(RSVF_COOKIE, 0) ? 1 : 2))
		return ERR_COOKIE_JAR;

	// The ports as BIOS devices. Before the cookie, so a failure leaves
	// nothing behind.
	init_rows();
	r = info.drvin ? install_bconmap_drvin() : install_bconmap();
	if (r)
		return r;
	info.first_dev = info.bdev[0];

	sr = ipl7();
	ok = add_cookie(SV3T_COOKIE, (long)&info);
	ipl_restore(sr);
	if (!ok)
		return ERR_COOKIE_JAR;

	// Port names, GEMDOS devices (with DRVIN) and the RSVF list. Without
	// RSVF (full cookie jar) the ports still work through their BIOS
	// device numbers.
	init_names();
	if (info.drvin)
		info.gemdos_ports = (unsigned char)install_gemdos();
	sr = ipl7();
	info.rsvf_ok = (unsigned char)install_rsvf();
	ipl_restore(sr);

#ifdef SV3T_TRACE
	// The trace port: opened by the driver itself, sent once the link is up
	// The timer interrupt writes a first line once the link is up.
	port_open(TRACE_PORT);
#endif

	// Start the resync before the timer is hooked, so nothing runs twice
	sv3p_start(&proto);

	// Hook Timer C. The old handler must be stored before ours can run, so
	// both are done with interrupts off. (Setexc does the same under TOS.)
	sr = ipl7();
	sv3t_old_timerc = TIMERC_VEC;
	TIMERC_VEC      = (void *)sv3t_timerc;
#ifdef SV3T_TRACE
	// The system call hooks; they log nothing until port 0 is opened
	sv3t_old_trap1  = TRAP1_VEC;
	TRAP1_VEC       = (void *)sv3t_trap1;
	sv3t_old_trap13 = TRAP13_VEC;
	TRAP13_VEC      = (void *)sv3t_trap13;
	sv3t_old_trap14 = TRAP14_VEC;
	TRAP14_VEC      = (void *)sv3t_trap14;
#endif
	ipl_restore(sr);

	return 0;
}


int main(void)
{
	long r;
	char line[100];

#ifdef SV3T_TRACE
	(void)Cconws("SV3TTY driver for TOS, TRACE build: calls are logged to ttySV15\r\n");
#else
	(void)Cconws("SV3TTY driver for TOS\r\n");
#endif

	r = Supexec(install);
	switch (r)
	{
	case 0:
		break;
	case ERR_INSTALLED:
		(void)Cconws("sv3tty: already installed\r\n");
		break;
	case ERR_NO_SUPV:
		(void)Cconws("sv3tty: no SupV cookie, sv_xbios.prg must be loaded first. Not installed.\r\n");
		break;
	case ERR_NOT_SV3:
		sprintf(line, "sv3tty: SuperVidel firmware %u is not a SuperVidel 3. Not installed.\r\n",
				sv_firmware);
		(void)Cconws(line);
		break;
	case ERR_NO_BCONMAP:
		(void)Cconws("sv3tty: this TOS has no Bconmap. Not installed.\r\n");
		break;
	case ERR_MAPTAB_SIZE:
		(void)Cconws("sv3tty: the Bconmap table is too large. Not installed.\r\n");
		break;
	case ERR_DRVIN_BCONMAP:
		(void)Cconws("sv3tty: DRVIN gave no BIOS device numbers. Not installed.\r\n");
		break;
	default:
		(void)Cconws("sv3tty: cookie jar full. Not installed.\r\n");
		break;
	}
	if (r)
	{
		sv3t_wait_key(SV3T_START_WAIT);
		return 1;
	}

	sprintf(line, "sv3tty: SuperVidel firmware %u, %d ports\r\n", sv_firmware, NPORTS);
	(void)Cconws(line);
	sprintf(line, "sv3tty: ports 0..%u = BIOS devices %u..%u%s\r\n",
			info.nbios - 1, info.bdev[0], info.bdev[info.nbios - 1],
			info.drvin ? " (from DRVIN, which has room up to 15)" : "");
	(void)Cconws(line);
	if (info.drvin)
	{
		int i;

		sprintf(line, "sv3tty: %u ports in U:\\DEV\\ (SV3TTY0..)", info.gemdos_ports);
		(void)Cconws(line);
		if (info.gemdos_ports < NPORTS)
		{
			(void)Cconws(", not:");
			for (i = 0; i < NPORTS; i++)
			{
				if (!(port_type[i] & RSVF_GEMDOS))
				{
					sprintf(line, " %d", i);
					(void)Cconws(line);
				}
			}
		}
		(void)Cconws("\r\n");
	}
	if (info.rsvf_ok)
	{
		sprintf(line, "sv3tty: %u ports in the RSVF list\r\n", info.rsvf_ports);
		(void)Cconws(line);
	}
	else
		(void)Cconws("sv3tty: cookie jar full, port names not in the RSVF list\r\n");
	(void)Cconws("sv3tty: Timer C hooked, waiting for link\r\n");

	// Time to read the messages when started from the desktop. The timer
	// is already hooked, so the link comes up meanwhile.
	sv3t_wait_key(SV3T_START_WAIT);

	Ptermres(_PgmSize, 0);
	return 0;
}
