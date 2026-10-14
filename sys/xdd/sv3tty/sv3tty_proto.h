/*
 * sv3tty_proto.h
 *
 * 68060 side of the sv3tty protocol: virtual serial ports to PetaLinux on
 * SuperVidel 3 over mailbox channel 1. See PROTOCOL.md for the protocol.
 *
 * This code is shared by the TOS test program, the TOS driver and the
 * FreeMiNT driver. It is plain C without library calls, so it can be used
 * in a TSR and in the kernel.
 *
 * The functions are not reentrant. The caller must make sure that they are
 * never called at the same time, e.g. from a timer interrupt and from a
 * program (raise the IPL around the calls in that case).
 * All functions must be called in supervisor mode.
 */

#ifndef SV3TTY_PROTO_H
#define SV3TTY_PROTO_H


/* ------------------------------------------------------------------ */
/* Mailbox channel 1 registers (68060 side), see ct60_mailboxes.vhd     */
/* ------------------------------------------------------------------ */

#define SV3P_MBOX_BASE			0x80013040UL	/* Channel 1 */

typedef struct
{
	volatile unsigned long	stat_ctrl;		/* Status, bit 4 = IE (never touched here) */
	volatile unsigned long	unused1[3];
	volatile unsigned long	data_fifo;		/* Payload words */
	volatile unsigned long	unused2[3];
	volatile unsigned long	len_fifo;		/* Header words */
	volatile unsigned long	unused3[3];
} SV3P_MBOX;

#define SV3P_RX_DFIFO_EMPTY		0x0001UL
#define SV3P_TX_DFIFO_FULL		0x0002UL
#define SV3P_RX_LFIFO_EMPTY		0x0004UL
#define SV3P_TX_LFIFO_FULL		0x0008UL


/* ------------------------------------------------------------------ */
/* Protocol definitions (must match sv3tty.c on the Linux side)         */
/* ------------------------------------------------------------------ */

#define SV3P_PROTO_VERSION		1
#define SV3P_MAX_PORTS			255
#define SV3P_LINK_PORT			0xFF
#define SV3P_MAX_PAYLOAD		256
#define SV3P_MARKER				0xA55AC33CUL

#define SV3P_T_CREDIT			0x01
#define SV3P_T_OPEN				0x02
#define SV3P_T_CLOSE			0x03
#define SV3P_T_CARRIER			0x04
#define SV3P_T_WINSZ			0x05
#define SV3P_T_BREAK			0x06
#define SV3P_T_DATA				0x80
#define SV3P_T_RESYNC			0xF0
#define SV3P_T_RESYNC_ACK		0xF1

#define SV3P_HDR(type, port, arg) \
	(((unsigned long)(type) << 24) | ((unsigned long)(port) << 16) | ((unsigned long)(arg) & 0xFFFFUL))


/* ------------------------------------------------------------------ */
/* Data structures                                                    */
/* ------------------------------------------------------------------ */

/* Ring buffer in the TOS IOREC layout, so a port's two buffers can be given
 * to programs through Bconmap/Iorec, and programs that look at the buffer
 * fill level directly (e.g. FreeMiNT's Bcostat) see the right values.
 *
 * As in the TOS serial ports, head is the index of the byte last READ and
 * tail the index of the byte last WRITTEN. Empty when head == tail. To write:
 * t = tail + 1 (0 at buflen), full if t == head, else bufaddr[t] = c and
 * tail = t. To read: h = head + 1 (0 at buflen), c = bufaddr[h], head = h.
 * So a buffer holds buflen - 1 bytes.
 *
 * Each index is written by one side only (the producer writes tail, the
 * consumer head), and only after the data, so no lock is needed when the
 * producer is an interrupt and the consumer a program, or the other way. */
typedef struct
{
	unsigned char	*bufaddr;
	short			buflen;
	volatile short	head;			/* Index of the byte last read */
	volatile short	tail;			/* Index of the byte last written */
	short			low_water;		/* Not used by us, set for programs */
	short			hi_water;
} SV3P_IOREC;

struct sv3p_port
{
	unsigned char	open;			/* Opened by the program on our side */
	unsigned char	announced;		/* OPEN has been sent since the last resync */
	unsigned char	credited;		/* First CREDIT from Linux after our OPEN has arrived */
	unsigned char	carrier;		/* Last CARRIER from Linux: something has the tty open */
	unsigned char	send_winsz;		/* A WINSZ message is pending */
	unsigned char	rows;
	unsigned char	cols;
	unsigned char	got_break;		/* Linux has sent a BREAK (cleared by the program) */
	unsigned long	tx_credit;		/* Bytes we may still send to Linux */
	unsigned long	credit_owed;	/* Bytes read by the program, not yet credited back */
	unsigned long	rx_total;		/* Bytes received since the port was opened */
	/* io[0]: data from Linux, its capacity (buflen - 1) is our window.
	 * io[1]: data to Linux. They follow each other, like the two IORECs
	 * of a TOS serial port, so &io[0] can be used as the port's IOREC. */
	SV3P_IOREC		io[2];
};

#define SV3P_RX(p)				(&(p)->io[0])
#define SV3P_TX(p)				(&(p)->io[1])

struct sv3p
{
	SV3P_MBOX		*mb;
	unsigned short	nports;
	unsigned short	tx_next;		/* Round-robin start port when sending */
	struct sv3p_port *ports;
	unsigned char	link_up;		/* UP after a resync, WAIT otherwise */
	unsigned char	resync_pending;	/* Our RESYNC must be (re)sent */
	unsigned char	ack_pending;	/* RESYNC_ACK must be sent */
	unsigned long	resync_time;	/* _hz_200 when our last RESYNC was sent */
	unsigned long	resync_interval;

	/* Statistics */
	unsigned long	rx_msgs;
	unsigned long	tx_msgs;
	unsigned long	rx_bytes;
	unsigned long	tx_bytes;
	unsigned long	rx_dropped;		/* Bytes thrown away (closed port, no room) */
	unsigned long	rx_bad_hdr;
	unsigned long	rx_timeouts;	/* A payload or marker word did not arrive */
	unsigned long	rx_unknown;
	unsigned long	tx_timeouts;	/* Linux did not read the FIFO */
	unsigned long	resyncs;		/* Times the link came UP */
	unsigned long	skipped_words;	/* Old data words thrown away at resync */
	unsigned short	peer_version;
};


/* ------------------------------------------------------------------ */
/* Functions                                                          */
/* ------------------------------------------------------------------ */

/* Initialise the state. ports must point to nports zeroed or unused entries. */
void sv3p_init(struct sv3p *s, struct sv3p_port *ports, unsigned short nports);

/* Start a resync. Call once at startup. */
void sv3p_start(struct sv3p *s);

/* Handle everything Linux has sent and send what is pending. Never waits for
 * long. Call often: from the main loop, or from a timer at 200 Hz. */
void sv3p_poll(struct sv3p *s);

/* Open a port. rxbuf/txbuf must stay valid until sv3p_close. Their sizes must
 * be 16..32767. The rx buffer holds rxsize - 1 bytes, which is the window
 * granted to Linux.
 * rows/cols: terminal size for WINSZ (0 = do not send).
 * Returns 0, or -1 for a bad port or size. The OPEN is sent by sv3p_poll. */
int sv3p_open(struct sv3p *s, unsigned short port,
			  unsigned char *rxbuf, unsigned long rxsize,
			  unsigned char *txbuf, unsigned long txsize,
			  unsigned char rows, unsigned char cols);

/* Close a port. CLOSE is sent by sv3p_poll. */
void sv3p_close(struct sv3p *s, unsigned short port);

/* Queue bytes for sending. Returns the number of bytes taken (may be less
 * than len when the tx buffer is full). */
long sv3p_write(struct sv3p *s, unsigned short port, const unsigned char *buf, long len);

/* Take received bytes. Returns the number of bytes copied to buf. */
long sv3p_read(struct sv3p *s, unsigned short port, unsigned char *buf, long len);

/* Number of received bytes waiting */
long sv3p_rx_count(struct sv3p *s, unsigned short port);

/* Free space in the tx buffer */
long sv3p_tx_room(struct sv3p *s, unsigned short port);

/* New terminal size, sent with WINSZ */
void sv3p_set_winsz(struct sv3p *s, unsigned short port, unsigned char rows, unsigned char cols);

#endif /* SV3TTY_PROTO_H */
