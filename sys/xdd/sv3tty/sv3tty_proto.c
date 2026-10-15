/*
 * sv3tty_proto.c
 *
 * 68060 side of the sv3tty protocol: virtual serial ports to PetaLinux on
 * SuperVidel 3 over mailbox channel 1. See PROTOCOL.md for the protocol and
 * sv3tty_proto.h for how to use these functions.
 *
 * Summary:
 *  - A message is a header word in the length FIFO,
 *    [31..24 type][23..16 port][15..0 arg], optionally preceded by payload
 *    words in the data FIFO. Payload words are always written first.
 *  - Flow control is credit based, per port and direction.
 *  - OPEN/CLOSE work as DTR, CARRIER from Linux works as DCD.
 *  - RESYNC/RESYNC_ACK with a marker word in the data FIFO bring both sides
 *    into a known state after a Falcon reset or a link error.
 *
 * The 68060 side has no interrupt for channel 1, so everything is polled.
 * Waits are counted in loop iterations, not in _hz_200 ticks, because
 * _hz_200 does not advance when this code runs from the Timer C interrupt.
 */

#include "sv3tty_proto.h"


/* _hz_200 system variable, supervisor mode only */
#define SV3P_HZ200				(*(volatile unsigned long *)0x4baL)

/* Max status reads while waiting for room in a full FIFO, or for a payload
 * word to arrive. One read over the CT60 bus takes well under a
 * microsecond, so this is a few ms. Linux normally empties its FIFOs
 * within microseconds. */
#define SV3P_SPIN_LOOPS			20000L

/* Max data words thrown away while looking for the marker at resync */
#define SV3P_MARKER_MAX_SKIP	4096

/* Default for the max headers read and DATA messages sent in one sv3p_poll
 * (s->rx_budget, s->tx_budget) */
#define SV3P_BUDGET				256

/* Time before our RESYNC is repeated when Linux does not answer */
#define SV3P_RESYNC_FIRST		200		/* 1 s */
#define SV3P_RESYNC_RETRY		400		/* 2 s */



/* ================================================================== */
/* Ring buffer helpers                                                */
/* ================================================================== */

/*
 * The buffers use the TOS IOREC layout, see SV3P_IOREC in sv3tty_proto.h:
 * head = index of the byte last read, tail = index of the byte last
 * written, empty when head == tail, capacity buflen - 1.
 */

/// @brief		Empty a ring buffer
/// @param r	Ring buffer
static void ring_reset(SV3P_IOREC *r)
{
	r->head = 0;
	r->tail = 0;
}


/// @brief		Number of bytes in a ring buffer
/// @param r	Ring buffer
/// @return		Bytes that can be read
static unsigned long ring_count(const SV3P_IOREC *r)
{
	long n = (long)r->tail - (long)r->head;

	if (n < 0)
		n += r->buflen;
	return (unsigned long)n;
}


/// @brief		Free space in a ring buffer
/// @param r	Ring buffer
/// @return		Bytes that can be written
static unsigned long ring_room(const SV3P_IOREC *r)
{
	return (unsigned long)(r->buflen - 1) - ring_count(r);
}


/// @brief		Copy bytes into a ring buffer, as many as fit. The tail index
///				is updated once, after the data, so a reader on the other
///				side (interrupt or program) never sees unwritten bytes.
/// @param r	Ring buffer
/// @param src	Bytes to write
/// @param len	Number of bytes
/// @return		Number of bytes written
static unsigned long ring_put(SV3P_IOREC *r, const unsigned char *src, unsigned long len)
{
	unsigned long n = ring_room(r);
	unsigned long i;
	short t = r->tail;

	if (len < n)
		n = len;
	for (i = 0; i < n; i++)
	{
		if (++t >= r->buflen)
			t = 0;
		r->bufaddr[t] = src[i];
	}
	r->tail = t;
	return n;
}


/// @brief		Copy bytes out of a ring buffer without removing them
/// @param r	Ring buffer
/// @param dst	Destination
/// @param len	Max number of bytes
/// @return		Number of bytes copied
static unsigned long ring_peek(const SV3P_IOREC *r, unsigned char *dst, unsigned long len)
{
	unsigned long n = ring_count(r);
	unsigned long i;
	short h = r->head;

	if (len < n)
		n = len;
	for (i = 0; i < n; i++)
	{
		if (++h >= r->buflen)
			h = 0;
		dst[i] = r->bufaddr[h];
	}
	return n;
}


/// @brief		Remove bytes from a ring buffer (after ring_peek)
/// @param r	Ring buffer
/// @param n	Number of bytes, at most ring_count(r)
static void ring_skip(SV3P_IOREC *r, unsigned long n)
{
	long h = (long)r->head + (long)n;

	if (h >= r->buflen)
		h -= r->buflen;
	r->head = (short)h;
}


/// @brief		Set up a ring buffer
/// @param r	Ring buffer
/// @param buf	Memory for it
/// @param size	Size in bytes, 16..32767
static void ring_init(SV3P_IOREC *r, unsigned char *buf, unsigned long size)
{
	r->bufaddr   = buf;
	r->buflen    = (short)size;
	r->head      = 0;
	r->tail      = 0;
	r->low_water = (short)(size / 4);
	r->hi_water  = (short)(size * 3 / 4);
}



/* ================================================================== */
/* Mailbox access                                                     */
/* ================================================================== */

/// @brief			Wait until a FIFO towards Linux has room for one word
/// @param s		Protocol state
/// @param fullbit	SV3P_TX_DFIFO_FULL or SV3P_TX_LFIFO_FULL
/// @return			0 when there is room, -1 if Linux does not read
static int mb_wait_room(struct sv3p *s, unsigned long fullbit)
{
	long loops;

	for (loops = 0; loops < SV3P_SPIN_LOOPS; loops++)
	{
		if ((s->mb->stat_ctrl & fullbit) == 0)
			return 0;
	}
	return -1;
}


/// @brief		Read one payload word, waiting a short while for it. The header
///				and its payload come through different FIFOs, so the header
///				can arrive a little before the payload.
/// @param s	Protocol state
/// @param w	Gets the word
/// @return		0 on success, -1 if the word did not arrive
static int mb_read_data(struct sv3p *s, unsigned long *w)
{
	long loops;

	for (loops = 0; loops < SV3P_SPIN_LOOPS; loops++)
	{
		if ((s->mb->stat_ctrl & SV3P_RX_DFIFO_EMPTY) == 0)
		{
			*w = s->mb->data_fifo;
			return 0;
		}
	}
	return -1;
}


/// @brief			Send one message: payload words first (MSB first packing),
///					header last
/// @param s		Protocol state
/// @param hdr		Header word
/// @param payload	Payload bytes, or 0 if len is 0
/// @param len		Payload length in bytes
/// @return			0 on success, -1 if Linux does not read the FIFOs
static int mb_send(struct sv3p *s, unsigned long hdr, const unsigned char *payload, unsigned long len)
{
	unsigned long i;

	for (i = 0; i < len; i += 4)
	{
		unsigned long w;

		// The last word may hold less than 4 bytes, the rest is 0
		w  = (unsigned long)payload[i] << 24;
		w |= (i + 1 < len) ? (unsigned long)payload[i + 1] << 16 : 0;
		w |= (i + 2 < len) ? (unsigned long)payload[i + 2] << 8  : 0;
		w |= (i + 3 < len) ? (unsigned long)payload[i + 3]       : 0;

		if (mb_wait_room(s, SV3P_TX_DFIFO_FULL))
			return -1;
		s->mb->data_fifo = w;
	}

	if (mb_wait_room(s, SV3P_TX_LFIFO_FULL))
		return -1;
	s->mb->len_fifo = hdr;

	s->tx_msgs++;
	s->tx_bytes += len;
	return 0;
}


/// @brief		Send RESYNC or RESYNC_ACK: the marker word, then the header
/// @param s	Protocol state
/// @param type	SV3P_T_RESYNC or SV3P_T_RESYNC_ACK
/// @return		0 on success, -1 if Linux does not read the FIFOs
static int mb_send_marker(struct sv3p *s, unsigned char type)
{
	if (mb_wait_room(s, SV3P_TX_DFIFO_FULL))
		return -1;
	s->mb->data_fifo = SV3P_MARKER;

	if (mb_wait_room(s, SV3P_TX_LFIFO_FULL))
		return -1;
	s->mb->len_fifo = SV3P_HDR(type, SV3P_LINK_PORT, SV3P_PROTO_VERSION);

	s->tx_msgs++;
	return 0;
}



/* ================================================================== */
/* Link and port state                                                */
/* ================================================================== */

/// @brief		Close all ports from the protocol's point of view. Ports that
///				the program has open stay open and are announced again with
///				OPEN by sv3p_poll.
/// @param s	Protocol state
static void reset_ports(struct sv3p *s)
{
	unsigned short i;

	for (i = 0; i < s->nports; i++)
	{
		struct sv3p_port *p = &s->ports[i];

		p->announced   = 0;
		p->credited    = 0;
		p->carrier     = 0;
		p->tx_credit   = 0;
		p->credit_owed = 0;
		if (p->open)
		{
			ring_reset(SV3P_RX(p));
			ring_reset(SV3P_TX(p));
		}
	}
}


/// @brief		Something is wrong with the framing, or Linux does not read.
///				Go to WAIT and start a resync.
/// @param s	Protocol state
static void link_error(struct sv3p *s)
{
	s->link_up         = 0;
	s->resync_pending  = 1;
	s->ack_pending     = 0;
	s->resync_interval = SV3P_RESYNC_FIRST;
	reset_ports(s);
}


/// @brief		At resync: throw away data words up to and including the marker
/// @param s	Protocol state
static void skip_to_marker(struct sv3p *s)
{
	unsigned long w;
	int n;

	for (n = 0; n < SV3P_MARKER_MAX_SKIP; n++)
	{
		if (mb_read_data(s, &w))
		{
			s->rx_timeouts++;
			return;
		}
		if (w == SV3P_MARKER)
			return;
		s->skipped_words++;
	}
}



/* ================================================================== */
/* Receive (Linux -> 68060)                                           */
/* ================================================================== */

/// @brief		Handle RESYNC or RESYNC_ACK from Linux (PROTOCOL.md section 5)
/// @param s	Protocol state
/// @param type	SV3P_T_RESYNC or SV3P_T_RESYNC_ACK
/// @param arg	Protocol version of the Linux driver
static void rx_resync(struct sv3p *s, unsigned char type, unsigned short arg)
{
	skip_to_marker(s);
	s->peer_version = arg;

	// A RESYNC_ACK while already UP answers an old RESYNC of ours
	if (type == SV3P_T_RESYNC_ACK && s->link_up)
		return;

	reset_ports(s);
	s->link_up        = 1;
	s->resync_pending = 0;
	if (type == SV3P_T_RESYNC)
		s->ack_pending = 1;
	s->resyncs++;
}


/// @brief		Handle DATA: read the payload and put it in the port's rx buffer
/// @param s	Protocol state
/// @param p	Port, or 0 if the port number is not one of ours
/// @param len	Payload length in bytes
static void rx_data(struct sv3p *s, struct sv3p_port *p, unsigned short len)
{
	unsigned char *buf = s->scratch;
	unsigned long i, w, n;

	if (len == 0 || len > SV3P_MAX_PAYLOAD)
	{
		s->rx_bad_hdr++;
		link_error(s);
		return;
	}

	// Always read all payload words, so the data FIFO stays in step
	for (i = 0; i < len; i += 4)
	{
		if (mb_read_data(s, &w))
		{
			s->rx_timeouts++;
			link_error(s);
			return;
		}
		buf[i] = (unsigned char)(w >> 24);
		if (i + 1 < len) buf[i + 1] = (unsigned char)(w >> 16);
		if (i + 2 < len) buf[i + 2] = (unsigned char)(w >> 8);
		if (i + 3 < len) buf[i + 3] = (unsigned char)w;
	}

	s->rx_bytes += len;

	// Not our port, not open, or still left from before our OPEN: discard,
	// and no credit back (PROTOCOL.md section 4)
	if (!p || !p->open || !p->announced || !p->credited)
	{
		s->rx_dropped += len;
		return;
	}

	n = ring_put(SV3P_RX(p), buf, len);
	p->rx_total += n;
	if (n < len)
	{
		// Linux sent more than our window. Should not happen.
		s->rx_dropped += len - n;
		p->credit_owed += len - n;
	}
}


/// @brief		Handle one header word from the length FIFO
/// @param s	Protocol state
/// @param hdr	Header word
static void rx_msg(struct sv3p *s, unsigned long hdr)
{
	unsigned char  type   = (unsigned char)(hdr >> 24);
	unsigned char  portnr = (unsigned char)(hdr >> 16);
	unsigned short arg    = (unsigned short)hdr;
	struct sv3p_port *p   = 0;

	s->rx_msgs++;

	// RESYNC and RESYNC_ACK are handled in every state
	if (type == SV3P_T_RESYNC || type == SV3P_T_RESYNC_ACK)
	{
		rx_resync(s, type, arg);
		return;
	}

	// In WAIT, headers are thrown away without their payload. The data FIFO
	// is cleaned up by the marker search at the next resync.
	if (!s->link_up)
		return;

	if (portnr < s->nports)
		p = &s->ports[portnr];

	switch (type)
	{
	case SV3P_T_DATA:
		rx_data(s, p, arg);
		break;

	case SV3P_T_CREDIT:
		// Linux has room for more data on this port. The first CREDIT after
		// our OPEN also marks the end of data left from an earlier session.
		if (p && p->announced)
		{
			p->credited   = 1;
			p->tx_credit += arg;
		}
		break;

	case SV3P_T_CARRIER:
		// Something on Linux has opened or closed the tty
		if (p)
			p->carrier = (arg != 0);
		break;

	case SV3P_T_BREAK:
		if (p && p->open)
			p->got_break = 1;
		break;

	default:
		// Unknown type from a newer protocol version: skip its payload
		s->rx_unknown++;
		if (type >= 0x80 && type < 0xF0)
		{
			unsigned long i, w;

			for (i = 0; i < arg; i += 4)
			{
				if (mb_read_data(s, &w))
				{
					s->rx_timeouts++;
					link_error(s);
					return;
				}
			}
		}
		break;
	}
}


/// @brief		Handle all headers waiting in the length FIFO
/// @param s	Protocol state
static void rx_all(struct sv3p *s)
{
	int n;

	for (n = 0; n < s->rx_budget; n++)
	{
		if (s->mb->stat_ctrl & SV3P_RX_LFIFO_EMPTY)
			break;
		rx_msg(s, s->mb->len_fifo);
	}
}



/* ================================================================== */
/* Transmit (68060 -> Linux)                                          */
/* ================================================================== */

/// @brief		Send the pending messages of one port, in protocol order:
///				CLOSE, OPEN + CREDIT, WINSZ, CREDIT, DATA
/// @param s	Protocol state
/// @param i	Port number
/// @return		0 if all could be sent, -1 if Linux does not read
static int tx_port(struct sv3p *s, unsigned short i)
{
	struct sv3p_port *p = &s->ports[i];
	unsigned char *buf = s->scratch;
	unsigned long n;

	// The program has closed the port: tell Linux (hangs up the tty)
	if (!p->open)
	{
		if (p->announced)
		{
			if (mb_send(s, SV3P_HDR(SV3P_T_CLOSE, i, 0), 0, 0))
				return -1;
			p->announced = 0;
			p->credited  = 0;
			p->tx_credit = 0;
		}
		return 0;
	}

	// Newly opened (or reopened after a resync): OPEN, then our window
	if (!p->announced)
	{
		ring_reset(SV3P_RX(p));
		p->credit_owed = 0;
		p->credited    = 0;
		p->tx_credit   = 0;
		if (mb_send(s, SV3P_HDR(SV3P_T_OPEN, i, 0), 0, 0))
			return -1;
		p->announced = 1;
		if (mb_send(s, SV3P_HDR(SV3P_T_CREDIT, i, SV3P_RX(p)->buflen - 1), 0, 0))
			return -1;
		if (p->rows && p->cols)
			p->send_winsz = 1;
	}

	// Terminal size
	if (p->send_winsz)
	{
		if (mb_send(s, SV3P_HDR(SV3P_T_WINSZ, i, ((unsigned short)p->rows << 8) | p->cols), 0, 0))
			return -1;
		p->send_winsz = 0;
	}

	// Return credit for data the program has read, a quarter window at a time
	if (p->credit_owed && p->credit_owed >= (unsigned long)(SV3P_RX(p)->buflen - 1) / 4)
	{
		if (mb_send(s, SV3P_HDR(SV3P_T_CREDIT, i, p->credit_owed), 0, 0))
			return -1;
		p->credit_owed = 0;
	}

	// Data, as much as the credit and the budget of this poll allow, in
	// messages of at most 256 bytes. Control messages above are always
	// sent, they are few and some must go together (OPEN + CREDIT).
	while (s->tx_left && p->credited && p->tx_credit && ring_count(SV3P_TX(p)))
	{
		s->tx_left--;
		n = SV3P_MAX_PAYLOAD;
		if (p->tx_credit < n)
			n = p->tx_credit;
		n = ring_peek(SV3P_TX(p), buf, n);
		if (mb_send(s, SV3P_HDR(SV3P_T_DATA, i, n), buf, n))
			return -1;
		ring_skip(SV3P_TX(p), n);
		p->tx_credit -= n;
	}

	return 0;
}


/// @brief		Send everything that is pending
/// @param s	Protocol state
static void tx_all(struct sv3p *s)
{
	unsigned short n;

	// Our RESYNC: at start, after a link error, and repeated while Linux
	// does not answer
	if (!s->link_up)
	{
		if (s->resync_pending || (SV3P_HZ200 - s->resync_time) >= s->resync_interval)
		{
			// The first RESYNC is repeated after 1 s, later ones every 2 s
			if (!s->resync_pending)
				s->resync_interval = SV3P_RESYNC_RETRY;
			s->resync_pending = 0;
			s->resync_time    = SV3P_HZ200;
			if (mb_send_marker(s, SV3P_T_RESYNC))
				s->tx_timeouts++;
		}
		return;
	}

	// Answer to a RESYNC from Linux
	if (s->ack_pending)
	{
		if (mb_send_marker(s, SV3P_T_RESYNC_ACK))
		{
			s->tx_timeouts++;
			link_error(s);
			return;
		}
		s->ack_pending = 0;
	}

	// All ports, round robin, so no port can starve the others
	s->tx_left = s->tx_budget;
	for (n = 0; n < s->nports; n++)
	{
		unsigned short i = (s->tx_next + n) % s->nports;

		if (tx_port(s, i))
		{
			s->tx_timeouts++;
			link_error(s);
			return;
		}
	}
	s->tx_next = (s->tx_next + 1) % s->nports;
}



/* ================================================================== */
/* Public functions                                                   */
/* ================================================================== */

/// @brief			Initialise the protocol state
/// @param s		Protocol state
/// @param ports	Array of nports port entries
/// @param nports	Number of ports, 1..255
void sv3p_init(struct sv3p *s, struct sv3p_port *ports, unsigned short nports)
{
	unsigned char *b;
	unsigned long i;

	b = (unsigned char *)s;
	for (i = 0; i < sizeof(*s); i++)
		b[i] = 0;
	b = (unsigned char *)ports;
	for (i = 0; i < sizeof(*ports) * nports; i++)
		b[i] = 0;

	if (nports > SV3P_MAX_PORTS)
		nports = SV3P_MAX_PORTS;

	s->mb              = (SV3P_MBOX *)SV3P_MBOX_BASE;
	s->ports           = ports;
	s->nports          = nports;
	s->resync_interval = SV3P_RESYNC_FIRST;
	s->rx_budget       = SV3P_BUDGET;
	s->tx_budget       = SV3P_BUDGET;
}


/// @brief		Start a resync. Call once at startup.
/// @param s	Protocol state
void sv3p_start(struct sv3p *s)
{
	link_error(s);
	sv3p_poll(s);
}


/// @brief		Handle everything Linux has sent, then send what is pending
/// @param s	Protocol state
void sv3p_poll(struct sv3p *s)
{
	rx_all(s);
	tx_all(s);
}


/// @brief			Open a port. The OPEN is sent by sv3p_poll when the link is up.
/// @param s		Protocol state
/// @param port		Port number
/// @param rxbuf	Receive buffer. It holds rxsize - 1 bytes, which is the
///					window granted to Linux.
/// @param rxsize	16..32767
/// @param txbuf	Transmit buffer
/// @param txsize	16..32767
/// @param rows		Terminal rows for WINSZ, 0 = do not send
/// @param cols		Terminal columns for WINSZ
/// @return			0, or -1 for a bad port or size
int sv3p_open(struct sv3p *s, unsigned short port,
			  unsigned char *rxbuf, unsigned long rxsize,
			  unsigned char *txbuf, unsigned long txsize,
			  unsigned char rows, unsigned char cols)
{
	struct sv3p_port *p;

	if (port >= s->nports)
		return -1;
	if (rxsize < 16 || rxsize > 32767 || txsize < 16 || txsize > 32767)
		return -1;

	p = &s->ports[port];
	ring_init(SV3P_RX(p), rxbuf, rxsize);
	ring_init(SV3P_TX(p), txbuf, txsize);
	p->rx_total  = 0;
	p->rows      = rows;
	p->cols      = cols;
	p->got_break = 0;
	p->announced = 0;		// sv3p_poll sends OPEN
	p->open      = 1;
	return 0;
}


/// @brief		Close a port. The CLOSE is sent by sv3p_poll.
/// @param s	Protocol state
/// @param port	Port number
void sv3p_close(struct sv3p *s, unsigned short port)
{
	if (port < s->nports)
		s->ports[port].open = 0;
}


/// @brief		Queue bytes for sending to Linux
/// @param s	Protocol state
/// @param port	Port number
/// @param buf	Bytes
/// @param len	Number of bytes
/// @return		Number of bytes taken
long sv3p_write(struct sv3p *s, unsigned short port, const unsigned char *buf, long len)
{
	if (port >= s->nports || !s->ports[port].open || len <= 0)
		return 0;
	return (long)ring_put(SV3P_TX(&s->ports[port]), buf, (unsigned long)len);
}


/// @brief		Take received bytes. The space they used is credited back to
///				Linux by sv3p_poll.
/// @param s	Protocol state
/// @param port	Port number
/// @param buf	Destination
/// @param len	Max number of bytes
/// @return		Number of bytes copied
long sv3p_read(struct sv3p *s, unsigned short port, unsigned char *buf, long len)
{
	struct sv3p_port *p;
	unsigned long n;

	if (port >= s->nports || !s->ports[port].open || len <= 0)
		return 0;

	p = &s->ports[port];
	n = ring_peek(SV3P_RX(p), buf, (unsigned long)len);
	ring_skip(SV3P_RX(p), n);
	if (p->announced)
		p->credit_owed += n;
	return (long)n;
}


/// @brief		Number of received bytes waiting
/// @param s	Protocol state
/// @param port	Port number
/// @return		Bytes that sv3p_read can return
long sv3p_rx_count(struct sv3p *s, unsigned short port)
{
	if (port >= s->nports || !s->ports[port].open)
		return 0;
	return (long)ring_count(SV3P_RX(&s->ports[port]));
}


/// @brief		Free space in the tx buffer
/// @param s	Protocol state
/// @param port	Port number
/// @return		Bytes that sv3p_write can take
long sv3p_tx_room(struct sv3p *s, unsigned short port)
{
	if (port >= s->nports || !s->ports[port].open)
		return 0;
	return (long)ring_room(SV3P_TX(&s->ports[port]));
}


/// @brief		Set a new terminal size, sent with WINSZ
/// @param s	Protocol state
/// @param port	Port number
/// @param rows	Rows
/// @param cols	Columns
void sv3p_set_winsz(struct sv3p *s, unsigned short port, unsigned char rows, unsigned char cols)
{
	if (port >= s->nports)
		return;
	s->ports[port].rows       = rows;
	s->ports[port].cols       = cols;
	s->ports[port].send_winsz = 1;
}
