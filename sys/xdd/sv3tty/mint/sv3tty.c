/*
 * sv3tty.c
 *
 * FreeMiNT driver (sv3tty.xdd) for the sv3tty virtual serial ports: up to
 * 16 ports to PetaLinux on the SuperVidel 3 ARM, over mailbox channel 1.
 * The protocol is described in ../PROTOCOL.md and implemented in
 * ../sv3tty_proto.c, which this driver shares with the TOS driver.
 *
 * Each port is a Unix terminal device, U:\DEV\TTYSV0 .. TTYSV15 (/dev/ttySV0
 * ..), with the same number as the tty on the Linux side. The kernel's tty
 * layer does the line discipline (termios, stty, job control), so programs
 * like cu, bash or top work on it.
 *
 * Mailbox channel 1 has no interrupt, so the driver polls it from a root
 * timeout that adds itself again, about every kernel tick (20 ms). The poll
 * moves data between the mailbox and the port buffers, and wakes processes
 * waiting to read or write. The mailbox channel 0 (sv3eth) and the mailbox
 * interrupt enable bits are never touched.
 *
 * Opening a port sends OPEN to Linux, closing the last file on it sends
 * CLOSE; Linux then hangs up the session on its tty, like a modem line with
 * HUPCL. A terminal size set with TIOCSWINSZ is sent to Linux, so programs
 * there use the whole window.
 *
 * The protocol functions are not reentrant. They are only called from the
 * root timeout and from the device functions, which all run in the kernel
 * without preemption, never from an interrupt, so no locking is needed.
 *
 * This file is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2, or (at your option)
 * any later version.
 */

# include "mint/mint.h"

# include "mint/asm.h"
# include "mint/dcntl.h"
# include "mint/file.h"
# include "mint/ioctl.h"
# include "mint/proc.h"
# include "mint/signal.h"
# include "mint/ssystem.h"
# include "mint/stat.h"
# include "cookie.h"

# include "libkern/libkern.h"

# include "../sv3tty_proto.h"


/*
 * version
 */

# define VER_MAJOR	0
# define VER_MINOR	10
# define VER_STATUS


/*
 * settings
 */

# define NPORTS			16			/* Ports installed: ttySV0 .. ttySV15 */
# define BUFSIZE		2048		/* Per port and direction */
# define POLL_MS		5			/* Poll interval; the kernel rounds it up to a tick */
# define POLL_BUDGET	16			/* Messages per poll, each way */
# define FAKE_BAUDRATE	115200L		/* Speed reported; the link has none */

# define SUPV_COOKIE	0x53757056L	/* 'SupV': installed by sv_xbios.prg */
# define SV3T_COOKIE	0x53563354L	/* 'SV3T': the TOS driver sv3tty.prg */

/* SuperVidel firmware version register. 300..399 is a SuperVidel 3, which
 * has the mailboxes. Only read it when the SupV cookie exists. */
# define SV_VERSION		(*(volatile unsigned long *)0x8001007CUL)


/*
 * messages
 */

# define MSG_VERSION	str (VER_MAJOR) "." str (VER_MINOR) str (VER_STATUS)
# define MSG_BUILDDATE	__DATE__

# define MSG_BOOT	\
	"\033p SV3TTY virtual serial ports to SuperVidel 3 Linux, version " MSG_VERSION " \033q\r\n"

# define MSG_GREET	\
	"\275 " MSG_BUILDDATE "\r\n\r\n"

# define MSG_MINT	\
	"\033pMiNT too old!\033q\r\n"

# define MSG_NO_SUPV	\
	"\033pNo SupV cookie: sv_xbios.prg must be loaded first.\033q\r\n"

# define MSG_NOT_SV3	\
	"\033pThis is not a SuperVidel 3.\033q\r\n"

# define MSG_TOS_DRIVER	\
	"\033pThe TOS driver sv3tty.prg is loaded; remove it from the AUTO folder.\033q\r\n"

# define MSG_FAILURE	\
	"\7\r\nSorry, driver NOT installed - initialization failed!\r\n\r\n"


/****************************************************************************/
/* BEGIN kernel interface */

struct kerinfo *kernel;

/* END kernel interface */
/****************************************************************************/

/****************************************************************************/
/* BEGIN definition part */

/* Our state of one port, next to the protocol's struct sv3p_port */
struct port
{
	FILEPTR			*open;			/* Open files on the port, a list */
	struct tty		tty;			/* Used by the kernel's tty layer */
	short			iosleepers;		/* Processes sleeping in read or write */
	short			lockpid;		/* Process holding an F_SETLK lock, 0 = none */
};

/*
 * device driver routines - top half
 */
static long _cdecl	sv3tty_open		(FILEPTR *f);
static long _cdecl	sv3tty_close	(FILEPTR *f, int pid);
static long _cdecl	sv3tty_twrite	(FILEPTR *f, const char *buf, long bytes);
static long _cdecl	sv3tty_tread	(FILEPTR *f, char *buf, long bytes);
static long _cdecl	sv3tty_writeb	(FILEPTR *f, const char *buf, long bytes);
static long _cdecl	sv3tty_readb	(FILEPTR *f, char *buf, long bytes);
static long _cdecl	sv3tty_lseek	(FILEPTR *f, long where, int whence);
static long _cdecl	sv3tty_ioctl	(FILEPTR *f, int mode, void *buf);
static long _cdecl	sv3tty_datime	(FILEPTR *f, ushort *timeptr, int rwflag);
static long _cdecl	sv3tty_select	(FILEPTR *f, long proc, int mode);
static void _cdecl	sv3tty_unselect	(FILEPTR *f, long proc, int mode);

/*
 * device driver map
 *
 * The tty layer calls write/read (twrite/tread) with one long per character
 * in canonical mode, and writeb/readb with bytes in raw mode.
 */
static DEVDRV tty_devtab =
{
	sv3tty_open,
	sv3tty_twrite, sv3tty_tread, sv3tty_lseek, sv3tty_ioctl, sv3tty_datime,
	sv3tty_close,
	sv3tty_select, sv3tty_unselect,
	sv3tty_writeb, sv3tty_readb
};


/*
 * debugging stuff
 */

# ifdef DEV_DEBUG
#  define DEBUG(x)	KERNEL_DEBUG x
#  define TRACE(x)	KERNEL_TRACE x
#  define ALERT(x)	KERNEL_ALERT x
# else
#  define DEBUG(x)
#  define TRACE(x)
#  define ALERT(x)	KERNEL_ALERT x
# endif

/* END definition part */
/****************************************************************************/

/****************************************************************************/
/* BEGIN global data definition & access implementation */

static struct sv3p			proto;
static struct sv3p_port		ports[NPORTS];		/* Protocol state of the ports */
static struct port			pt[NPORTS];			/* Our state of the ports */
static unsigned char		bufs[NPORTS][2][BUFSIZE];

/* END global data & access implementation */
/****************************************************************************/

/****************************************************************************/
/* BEGIN port helpers - top half */

/// @brief		The sleep/wake condition of a port: processes waiting for data
///				or for room sleep on it, the poll wakes them
/// @param i	Port number
/// @return		Condition for sleep() and wake()
INLINE long port_cond(int i)
{
	return (long) &pt[i].tty.state;
}


/// @brief		Open a port towards Linux, if it is not open already. The poll
///				sends OPEN once the link is up.
/// @param i	Port number
static void port_up(int i)
{
	if (ports[i].open)
		return;

	sv3p_open(&proto, (unsigned short) i, bufs[i][0], BUFSIZE, bufs[i][1], BUFSIZE,
			  (unsigned char) pt[i].tty.wsiz.ws_row, (unsigned char) pt[i].tty.wsiz.ws_col);
	DEBUG (("sv3tty: port %d up", i));
}


/// @brief		Hang up a port: CLOSE to Linux, which hangs up the session on
///				its tty. Data not yet sent is thrown away.
/// @param i	Port number
static void port_down(int i)
{
	if (!ports[i].open)
		return;

	sv3p_close(&proto, (unsigned short) i);
	DEBUG (("sv3tty: port %d down", i));

	/* Processes waiting on the port see the change */
	wake (IO_Q, port_cond (i));
}


/// @brief		Hang up a port and open it again, for a hangup (DTR off, speed
///				0) while the port stays open. Linux hangs up its tty when the
///				new OPEN arrives.
/// @param i	Port number
static void port_hangup(int i)
{
	port_down(i);
	port_up(i);
}

/* END port helpers - top half */
/****************************************************************************/

/****************************************************************************/
/* BEGIN polling - runs as a root timeout */

/// @brief		Poll the mailbox, then wake the processes that can go on: those
///				waiting for data when data has arrived, and those waiting for
///				room when there is room. Adds itself again as a root timeout.
/// @param p	Process (not used)
/// @param arg	Not used
static void sv3tty_poll(PROC *p, long arg)
{
	int i;

	sv3p_poll(&proto);

	for (i = 0; i < NPORTS; i++)
	{
		struct port *q = &pt[i];
		long rx, room;

		if (!ports[i].open || !q->open)
			continue;

		rx   = sv3p_rx_count(&proto, (unsigned short) i);
		room = sv3p_tx_room(&proto, (unsigned short) i);

		if (rx && q->tty.rsel)
			wakeselect (q->tty.rsel);

		if (room && q->tty.wsel)
			wakeselect (q->tty.wsel);

		if (q->iosleepers && (rx || room))
			wake (IO_Q, port_cond (i));
	}

	if (!addroottimeout (POLL_MS, sv3tty_poll, 0))
		ALERT (("sv3tty: no memory for the poll timeout, ports stopped"));
}

/* END polling */
/****************************************************************************/

/****************************************************************************/
/* BEGIN initialization - top half */

/// @brief		Driver entry point, called by the kernel when it loads the
///				driver: checks the hardware, installs the devices and starts
///				the poll
/// @param k	Kernel interface
/// @return		1 when installed, NULL when not
DEVDRV * _cdecl
init_xdd (struct kerinfo *k)
{
	long value;
	unsigned long firmware;
	int i, installed = 0;

	struct dev_descr tty_dev_descriptor =
	{
		&tty_devtab,
		0,		/* dinfo -> fc.aux, our port number */
		O_TTY,		/* flags */
		NULL,		/* struct tty * */
		sizeof(DEVDRV),	/* drvsize */
		S_IFCHR |
		S_IRUSR |
		S_IWUSR |
		S_IRGRP |
		S_IWGRP |
		S_IROTH |
		S_IWOTH,	/* fmode */
		NULL,		/* bdevmap */
		0,		/* bdev */
		0		/* reserved */
	};

	kernel = k;

	c_conws (MSG_BOOT);
	c_conws (MSG_GREET);

	DEBUG (("%s: enter init", __FILE__));

	if ((MINT_MAJOR == 0)
		|| ((MINT_MAJOR == 1) && ((MINT_MINOR < 15) || (MINT_KVERSION < 2))))
	{
		c_conws (MSG_MINT);
		goto failure;
	}

	/* The mailboxes are on a SuperVidel 3; sv_xbios.prg tells us it is a
	 * SuperVidel at all, so its registers can be read */
	if (s_system (S_GETCOOKIE, SUPV_COOKIE, (long) &value) != 0)
	{
		c_conws (MSG_NO_SUPV);
		goto failure;
	}
	firmware = SV_VERSION;
	if (firmware < 300 || firmware > 399)
	{
		c_conws (MSG_NOT_SV3);
		goto failure;
	}

	/* The TOS driver polls the same mailbox from Timer C */
	if (s_system (S_GETCOOKIE, SV3T_COOKIE, (long) &value) == 0)
	{
		c_conws (MSG_TOS_DRIVER);
		goto failure;
	}

	/* Protocol state; the resync with Linux starts at once */
	sv3p_init(&proto, ports, NPORTS);
	proto.rx_budget = POLL_BUDGET;
	proto.tx_budget = POLL_BUDGET;
	sv3p_start(&proto);

	/* The devices, U:\DEV\TTYSV0 .. */
	for (i = 0; i < NPORTS; i++)
	{
		char name[32];

		ksprintf (name, "u:\\dev\\ttySV%d", i);

		tty_dev_descriptor.dinfo = i;
		tty_dev_descriptor.tty = &pt[i].tty;
		if (d_cntl (DEV_INSTALL, name, (long) &tty_dev_descriptor) >= 0)
		{
			DEBUG (("%s: %s installed", __FILE__, name));
			installed++;
		}
		else
			ALERT (("sv3tty: %s could not be installed", name));
	}

	if (!installed)
		goto failure;

	if (!addroottimeout (POLL_MS, sv3tty_poll, 0))
		goto failure;

	return (DEVDRV *) 1;

failure:
	c_conws (MSG_FAILURE);
	return NULL;
}

/* END initialization - top half */
/****************************************************************************/

/****************************************************************************/
/* BEGIN device driver routines - top half */

/// @brief		Open a port. The first open brings the port up towards Linux.
/// @param f	File
/// @return		E_OK, or EACCES for a bad port
static long _cdecl
sv3tty_open (FILEPTR *f)
{
	ushort dev = f->fc.aux;
	struct port *q;

	DEBUG (("sv3tty_open [%i]: enter (%x)", dev, f->flags));

	if (dev >= NPORTS)
		return EACCES;

	q = &pt[dev];
	if (!q->open)
		port_up(dev);

	f->pos = 0;
	f->next = q->open;
	q->open = f;
	f->flags |= O_TTY;

	DEBUG (("sv3tty_open: return E_OK (added %p)", f));
	return E_OK;
}


/// @brief		Close a file on a port. When the last one is closed, the port
///				is hung up.
/// @param f	File
/// @param pid	Closing process
/// @return		E_OK
static long _cdecl
sv3tty_close (FILEPTR *f, int pid)
{
	ushort dev = f->fc.aux;
	struct port *q = &pt[dev];

	DEBUG (("sv3tty_close [%i]: enter", dev));

	if ((f->flags & O_LOCK) && ((q->lockpid == pid) || (f->links <= 0)))
	{
		f->flags &= ~O_LOCK;
		q->lockpid = 0;

		/* wake anyone waiting for this lock */
		wake (IO_Q, (long) q);
	}

	if (f->links <= 0)
	{
		FILEPTR **temp = &q->open;

		/* remove the FILEPTR from the linked list */
		while (*temp)
		{
			if (*temp == f)
			{
				*temp = f->next;
				f->next = NULL;
				break;
			}
			temp = &(*temp)->next;
		}

		if (!q->open)
			port_down(dev);
	}

	return E_OK;
}


/// @brief		Write in canonical mode: one long per character, from the
///				kernel's tty layer. Sleeps while the buffer is full, unless
///				O_NDELAY.
/// @param f	File
/// @param buf	Characters, one long each
/// @param bytes Size of buf in bytes (4 per character)
/// @return		Bytes of buf taken, or EWOULDBLOCK
static long _cdecl
sv3tty_twrite (FILEPTR *f, const char *buf, long bytes)
{
	ushort dev = f->fc.aux;
	int ndelay = f->flags & O_NDELAY;
	const long *r = (const long *) buf;
	long done = 0;

	TRACE (("sv3tty_twrite [%i]: enter (%p, %ld)", dev, buf, bytes));

	port_up(dev);
	while (bytes > 0)
	{
		unsigned char c = (unsigned char) *r;

		if (sv3p_write(&proto, dev, &c, 1) == 1)
		{
			r++;
			done += 4;
			bytes -= 4;
			continue;
		}

		/* Buffer full: the poll sends it and wakes us */
		if (ndelay)
			break;

		pt[dev].iosleepers++;
		sleep (IO_Q, port_cond (dev));
		pt[dev].iosleepers--;
		port_up(dev);
	}

	if (ndelay && !done)
		done = EWOULDBLOCK;

	return done;
}


/// @brief		Read in canonical mode: one long per character, to the kernel's
///				tty layer. Sleeps until all are read, unless O_NDELAY.
/// @param f	File
/// @param buf	Gets the characters, one long each
/// @param bytes Size of buf in bytes (4 per character)
/// @return		Bytes of buf filled, or EWOULDBLOCK
static long _cdecl
sv3tty_tread (FILEPTR *f, char *buf, long bytes)
{
	ushort dev = f->fc.aux;
	int ndelay = f->flags & O_NDELAY;
	long *r = (long *) buf;
	long done = 0;

	TRACE (("sv3tty_tread [%i]: enter (%p, %ld)", dev, buf, bytes));

	port_up(dev);
	while (bytes > 0)
	{
		unsigned char c;

		if (sv3p_read(&proto, dev, &c, 1) == 1)
		{
			*r++ = c;
			done += 4;
			bytes -= 4;
			continue;
		}

		if (ndelay)
			break;

		pt[dev].iosleepers++;
		sleep (IO_Q, port_cond (dev));
		pt[dev].iosleepers--;
		port_up(dev);
	}

	if (ndelay && !done)
		done = EWOULDBLOCK;

	return done;
}


/// @brief		Write in raw mode: bytes. Sleeps while the buffer is full,
///				unless O_NDELAY.
/// @param f	File
/// @param buf	Bytes
/// @param bytes Number of bytes
/// @return		Bytes taken, or EWOULDBLOCK
static long _cdecl
sv3tty_writeb (FILEPTR *f, const char *buf, long bytes)
{
	ushort dev = f->fc.aux;
	int ndelay = f->flags & O_NDELAY;
	long done = 0;

	TRACE (("sv3tty_writeb [%i]: enter (%p, %ld)", dev, buf, bytes));

	port_up(dev);
	while (bytes > 0)
	{
		long n = sv3p_write(&proto, dev, (const unsigned char *) buf, bytes);

		buf += n;
		done += n;
		bytes -= n;
		if (!bytes || ndelay)
			break;

		/* Buffer full: the poll sends it and wakes us */
		pt[dev].iosleepers++;
		sleep (IO_Q, port_cond (dev));
		pt[dev].iosleepers--;
		port_up(dev);
	}

	if (ndelay && !done)
		done = EWOULDBLOCK;

	return done;
}


/// @brief		Timeout function for readb: wakes the reader when VTIME is up
/// @param p	Process
/// @param cond	The port's sleep condition
static void
timerwake (PROC *p, long cond)
{
	wake (IO_Q, cond);
}


/// @brief		Read in raw mode: bytes. Blocking reads follow VMIN and VTIME,
///				as in uart.xdd:
///				- VTIME 0: wait until VMIN bytes are there (VMIN 0: no wait).
///				- VMIN 0, VTIME > 0: wait at most VTIME tenths of a second
///				  for the first byte.
///				- both > 0: VTIME is the longest pause between bytes.
/// @param f	File
/// @param buf	Gets the bytes
/// @param bytes Max number of bytes
/// @return		Bytes read (0 at a VTIME timeout), or EWOULDBLOCK
static long _cdecl
sv3tty_readb (FILEPTR *f, char *buf, long bytes)
{
	ushort dev = f->fc.aux;
	struct tty *tty = &pt[dev].tty;
	int ndelay = f->flags & O_NDELAY;
	long done;

	TRACE (("sv3tty_readb [%i]: enter (%p, %ld)", dev, buf, bytes));

	if (!bytes)
		return 0;

	port_up(dev);

	if (!ndelay)
	{
		if (tty->vtime == 0)
		{
			/* VMIN > 0 waits for VMIN bytes, VMIN 0 does not wait */
			while (sv3p_rx_count(&proto, dev) < (long) tty->vmin)
			{
				pt[dev].iosleepers++;
				sleep (IO_Q, port_cond (dev));
				pt[dev].iosleepers--;
				port_up(dev);
			}
		}
		else if (tty->vmin == 0)
		{
			/* VTIME is the longest wait for the first byte */
			if (!sv3p_rx_count(&proto, dev))
			{
				TIMEOUT *t = addtimeout (tty->vtime * 100L, timerwake);

				if (!t)
					return ENOMEM;
				t->arg = port_cond (dev);

				pt[dev].iosleepers++;
				sleep (IO_Q, port_cond (dev));
				pt[dev].iosleepers--;

				canceltimeout (t);
			}
		}
		else
		{
			/* VTIME is the longest pause between bytes */
			while (sv3p_rx_count(&proto, dev) < (long) tty->vmin)
			{
				long before = sv3p_rx_count(&proto, dev);
				TIMEOUT *t = addtimeout (tty->vtime * 100L, timerwake);

				if (!t)
					return ENOMEM;
				t->arg = port_cond (dev);

				pt[dev].iosleepers++;
				sleep (IO_Q, port_cond (dev));
				pt[dev].iosleepers--;

				canceltimeout (t);
				port_up(dev);

				/* Nothing new: the pause timed out */
				if (before && sv3p_rx_count(&proto, dev) == before)
					break;
			}
		}
	}

	done = sv3p_read(&proto, dev, (unsigned char *) buf, bytes);

	if (ndelay && !done)
		done = EWOULDBLOCK;

	TRACE (("sv3tty_readb: leave (%ld)", done));
	return done;
}


/// @brief		Seek: terminal devices are always at position 0
/// @return		0
static long _cdecl
sv3tty_lseek (FILEPTR *f, long where, int whence)
{
	return 0;
}


/// @brief		ioctl on a port. Calls not handled here return ENOSYS, and the
///				kernel then hands them to its tty layer.
/// @param f	File
/// @param mode	Command
/// @param buf	Argument
/// @return		E_OK, an error, or ENOSYS for the tty layer
static long _cdecl
sv3tty_ioctl (FILEPTR *f, int mode, void *buf)
{
	ushort dev = f->fc.aux;
	struct port *q = &pt[dev];
	long r = E_OK;

	DEBUG (("sv3tty_ioctl [%i]: (%x, (%c %i), %p)", dev, mode, (char) (mode >> 8), (mode & 0xff), buf));

	switch (mode)
	{
		case FIONREAD:
		{
			*(long *) buf = sv3p_rx_count(&proto, dev);
			break;
		}
		case FIONWRITE:
		{
			*(long *) buf = sv3p_tx_room(&proto, dev);
			break;
		}
		case FIOEXCEPT:
		{
			*(long *) buf = 0;
			break;
		}
		case TIONOTSEND:
		case TIOCOUTQ:
		{
			/* Bytes not yet sent to Linux */
			*(long *) buf = ports[dev].open ? (BUFSIZE - 1) - sv3p_tx_room(&proto, dev) : 0;
			break;
		}
		case TIOCFLUSH:
		{
			/* Throw away received data. (Data to send is not thrown
			 * away: it is usually gone within a tick anyway.) */
			long flushtype;
			unsigned char junk[64];

			if (!buf || !(*(long *) buf & 3))
				flushtype = 3;
			else
				flushtype = *(long *) buf;

			if (flushtype & 1)
				while (sv3p_read(&proto, dev, junk, sizeof(junk)) > 0)
					;
			break;
		}
		case TIOCIBAUD:
		case TIOCOBAUD:
		{
			/* -1 asks for the speed, 0 hangs up; other speeds mean
			 * nothing for the link and are accepted */
			long *speed = (long *) buf;

			if (*speed == -1)
				*speed = FAKE_BAUDRATE;
			else if (*speed == 0)
				port_hangup(dev);
			break;
		}
		case TIOCGFLAGS:
		{
			*(ushort *) buf = TF_8BIT | TF_1STOP;
			break;
		}
		case TIOCSFLAGS:
		case TIOCSTOP:
		case TIOCSTART:
		case TIOCCBRK:
		case TIOCSBRK:
		{
			/* Format, flow control and break mean nothing here */
			break;
		}
		case TIOCGVMIN:
		{
			ushort *v = buf;

			v[0] = q->tty.vmin;
			v[1] = q->tty.vtime;
			break;
		}
		case TIOCSVMIN:
		{
			ushort *v = buf;

			if (v[0] > BUFSIZE / 2)
				v[0] = BUFSIZE / 2;
			q->tty.vmin = v[0];
			q->tty.vtime = v[1];
			break;
		}
		case TIOCSWINSZ:
		{
			/* Send the size to Linux, then let the tty layer store it and
			 * signal SIGWINCH here */
			struct winsize *ws = (struct winsize *) buf;
			int rows = ws->ws_row, cols = ws->ws_col;

			if (rows > 255)
				rows = 255;
			if (cols > 255)
				cols = 255;
			if (rows > 0 && cols > 0)
				sv3p_set_winsz(&proto, dev, (unsigned char) rows, (unsigned char) cols);
			r = ENOSYS;
			break;
		}
		case TIOCCDTR:
		{
			port_hangup(dev);
			break;
		}
		case TIOCSDTR:
		{
			port_up(dev);
			break;
		}
		case TIOCMGET:
		{
			/* Carrier when something on Linux has the tty open */
			long val = TIOCM_DTR | TIOCM_RTS | TIOCM_CTS | TIOCM_DSR;

			if (ports[dev].carrier)
				val |= TIOCM_CD;
			*(long *) buf = val;
			break;
		}
		case F_SETLK:
		case F_SETLKW:
		{
			struct flock *lck = (struct flock *) buf;
			int cpid = p_getpid ();

			while (q->lockpid && q->lockpid != cpid)
			{
				if (mode == F_SETLKW && lck->l_type != F_UNLCK)
					sleep (IO_Q, (long) q);
				else
					return ELOCKED;
			}

			if (lck->l_type == F_UNLCK)
			{
				if (!(f->flags & O_LOCK) || q->lockpid != cpid)
					return ENSLOCK;

				q->lockpid = 0;
				f->flags &= ~O_LOCK;

				/* wake anyone waiting for this lock */
				wake (IO_Q, (long) q);
			}
			else
			{
				q->lockpid = cpid;
				f->flags |= O_LOCK;
			}
			break;
		}
		case F_GETLK:
		{
			struct flock *lck = (struct flock *) buf;

			if (q->lockpid)
			{
				lck->l_type = F_WRLCK;
				lck->l_start = lck->l_len = 0;
				lck->l_pid = q->lockpid;
			}
			else
				lck->l_type = F_UNLCK;
			break;
		}
		default:
		{
			/* Fcntl hands it to tty_ioctl */
			r = ENOSYS;
			break;
		}
	}

	DEBUG (("sv3tty_ioctl: return %li", r));
	return r;
}


/// @brief		Date and time of the device: now
/// @return		E_OK, or EACCES when asked to set them
static long _cdecl
sv3tty_datime (FILEPTR *f, ushort *timeptr, int rwflag)
{
	if (rwflag)
		return EACCES;

	*timeptr++ = timestamp;
	*timeptr = datestamp;

	return E_OK;
}


/// @brief		select: ready to read when data is waiting, to write when the
///				buffer has room. Otherwise the process is remembered, and the
///				poll wakes it.
/// @param f	File
/// @param proc	Selecting process
/// @param mode	O_RDONLY or O_WRONLY
/// @return		1 if ready, 0 if not, 2 if another process already selects
static long _cdecl
sv3tty_select (FILEPTR *f, long proc, int mode)
{
	ushort dev = f->fc.aux;
	struct tty *tty = (struct tty *) f->devinfo;

	TRACE (("sv3tty_select [%i]: enter (%li, %i)", dev, proc, mode));

	port_up(dev);

	if (mode == O_RDONLY)
	{
		if (sv3p_rx_count(&proto, dev))
			return 1;

		if (tty)
		{
			if (tty->rsel)
				return 2;	/* collision */
			tty->rsel = proc;
		}
		return 0;
	}
	else if (mode == O_WRONLY)
	{
		if (sv3p_tx_room(&proto, dev))
			return 1;

		if (tty)
		{
			if (tty->wsel)
				return 2;	/* collision */
			tty->wsel = proc;
		}
		return 0;
	}

	/* default -- we don't know this mode, return 0 */
	return 0;
}


/// @brief		Forget a selecting process
/// @param f	File
/// @param proc	Process
/// @param mode	O_RDONLY or O_WRONLY
static void _cdecl
sv3tty_unselect (FILEPTR *f, long proc, int mode)
{
	struct tty *tty = (struct tty *) f->devinfo;

	if (tty)
	{
		if (mode == O_RDONLY && tty->rsel == proc)
			tty->rsel = 0;
		else if (mode == O_WRONLY && tty->wsel == proc)
			tty->wsel = 0;
	}
}

/* END device driver routines - top half */
/****************************************************************************/
