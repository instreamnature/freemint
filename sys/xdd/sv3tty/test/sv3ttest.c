/*
 * sv3ttest.c
 *
 * Test program for sv3tty under plain TOS: a simple terminal on one
 * sv3tty port (default port 0, i.e. /dev/ttySV0 on PetaLinux).
 *
 * The program polls mailbox channel 1 in its main loop. It runs in user
 * mode and only enters supervisor mode through Supexec for the mailbox
 * access. (Running the whole program in supervisor mode with Super(0L) put
 * all interrupts on the program's small stack and crashed at exit.)
 * It does not hook any vector, does not use interrupts and never touches
 * the interrupt enable bit of the mailbox, so if something goes wrong only
 * this program hangs.
 *
 * Keyboard input is sent to the port, and data from the port is written to
 * the screen with Bconout(2), so TOS' built-in VT52 emulation interprets the
 * escape sequences (use TERM=st52 or vt52 on Linux).
 *
 * *** Do not run it with NVDI loaded. ***
 * With NVDI, no text output of any TOS program reaches the screen on our
 * setup (SuperVidel 3 screen mode), neither printf, Cconws nor Bconout. The
 * program still runs and talks to Linux, but nothing is shown. Boot without
 * NVDI to use it.
 *
 * Characters 0x80..0xFF are converted between the Atari character set and
 * UTF-8, which Linux uses (so that e.g. å, ä, ö work). F10 turns this off
 * and on.
 *
 * ANSI colour codes (ESC [ ... m, from ls --color and coloured prompts) are
 * translated to the VT52 colour sequences of TOS (ESC b / ESC c). Other ANSI
 * sequences are dropped. The default theme is white on black.
 * Note: on a SuperVidel screen mode (tested 640x480x16, mode 0x403A) TOS 4
 * ignores ESC b / ESC c, so the theme and the colours have no effect there;
 * the ANSI codes are still removed instead of showing up as garbage.
 * TOS' ESC K also damages other characters there when the cursor is on an
 * odd column, which is why erase_eol() exists.
 *
 * Keys:
 *   UNDO       quit (sends CLOSE, so Linux hangs up the session)
 *   HELP       show link state, statistics and what TOS reports about the screen
 *   F7         erase to end of line (ESC K) with spaces / by TOS, see erase_eol()
 *   F8         dark/light theme (clears the screen)
 *   F9         debug: show received control bytes in hex, e.g. [1B]
 *   F10        character set conversion off/on
 *   Backspace  sent as DEL (0x7F), the erase character Linux expects
 *   Arrow keys are sent as VT52 sequences (ESC A/B/C/D).
 *
 * Usage: SV3TTEST.TOS [port]
 *
 * Requires a SuperVidel 3 with the sv3tty module loaded on PetaLinux.
 * Without SuperVidel 3 the mailbox access gives a bus error.
 */

#include <stdio.h>
#include <stdlib.h>
#include <osbind.h>
#include <mint/falcon.h>

#include "../sv3tty_proto.h"
#include "../tos/sv3tty_tos.h"


#define NPORTS				16
#define RXSIZE				1024		/* Our window */
#define TXSIZE				1024

#define SCAN_BACKSPACE		0x0E
#define SCAN_F7				0x41
#define SCAN_F8				0x42
#define SCAN_F9				0x43
#define SCAN_F10			0x44
#define SCAN_UNDO			0x61
#define SCAN_HELP			0x62
#define SCAN_UP				0x48
#define SCAN_DOWN			0x50
#define SCAN_LEFT			0x4B
#define SCAN_RIGHT			0x4D

static struct sv3p			proto;
static struct sv3p_port		ports[NPORTS];

/* The protocol state shown by HELP: our own, or the driver's in -b mode */
static struct sv3p			*ps = &proto;

/* -b mode: BIOS device of the port (through the sv3tty driver), else -1 */
static long					bios_dev = -1;


/// @brief		Send bytes to the port: through the BIOS in -b mode (Bconout
///				waits while the driver's buffer is full), else into our own
///				protocol buffer
/// @param port	Port number
/// @param buf	Bytes
/// @param n	Number of bytes
static void port_write(unsigned short port, const unsigned char *buf, long n)
{
	long i;

	if (bios_dev >= 0)
	{
		for (i = 0; i < n; i++)
			Bconout((short)bios_dev, buf[i]);
	}
	else
		sv3p_write(&proto, port, buf, n);
}
static unsigned char		rxbuf[RXSIZE];
static unsigned char		txbuf[TXSIZE];

static int					convert = 1;	/* Atari charset <-> UTF-8 */
static int					dark = 1;		/* Dark theme: white on black */
static int					debug = 0;		/* Show received control bytes in hex */
static int					fix_eol = 1;	/* Do ESC K with spaces, see erase_eol() */

/*
 * Colours. VT52 in TOS sets the text colours with ESC b n (foreground) and
 * ESC c n (background), where n is a hardware palette index 0..15. In the
 * default TOS palette: 0 white, 1 red, 2 green, 3 yellow, 4 blue, 5 magenta,
 * 6 cyan, 7 light grey, 8 dark grey, 9..14 light red..light cyan, 15 black.
 * ANSI colour codes from Linux (ESC [ ... m, used by ls --color and coloured
 * prompts) are translated to these indexes.
 */
static const unsigned char	ansi_normal[8] = { 15, 1, 2, 3, 4, 5, 6, 7 };	/* black..white */
static const unsigned char	ansi_bright[8] = { 8, 9, 10, 11, 12, 13, 14, 0 };

static int					fg_ansi = -1;	/* ANSI colour 0..7, -1 = theme default */
static int					bg_ansi = -1;
static int					fg_bright, bg_bright;
static int					bold, reverse;


/*
 * Unicode code points for the Atari ST character set, 0x80..0xFF.
 * 0xC2..0xDC are Hebrew letters and are filled in by atari_to_unicode().
 */
static const unsigned short atari_high[128] =
{
	/* 0x80 */ 0x00C7, 0x00FC, 0x00E9, 0x00E2, 0x00E4, 0x00E0, 0x00E5, 0x00E7,
	/* 0x88 */ 0x00EA, 0x00EB, 0x00E8, 0x00EF, 0x00EE, 0x00EC, 0x00C4, 0x00C5,
	/* 0x90 */ 0x00C9, 0x00E6, 0x00C6, 0x00F4, 0x00F6, 0x00F2, 0x00FB, 0x00F9,
	/* 0x98 */ 0x00FF, 0x00D6, 0x00DC, 0x00A2, 0x00A3, 0x00A5, 0x00DF, 0x0192,
	/* 0xA0 */ 0x00E1, 0x00ED, 0x00F3, 0x00FA, 0x00F1, 0x00D1, 0x00AA, 0x00BA,
	/* 0xA8 */ 0x00BF, 0x2310, 0x00AC, 0x00BD, 0x00BC, 0x00A1, 0x00AB, 0x00BB,
	/* 0xB0 */ 0x00E3, 0x00F5, 0x00D8, 0x00F8, 0x0153, 0x0152, 0x00C0, 0x00C3,
	/* 0xB8 */ 0x00D5, 0x00A8, 0x00B4, 0x2020, 0x00B6, 0x00A9, 0x00AE, 0x2122,
	/* 0xC0 */ 0x0133, 0x0132, 0,      0,      0,      0,      0,      0,
	/* 0xC8 */ 0,      0,      0,      0,      0,      0,      0,      0,
	/* 0xD0 */ 0,      0,      0,      0,      0,      0,      0,      0,
	/* 0xD8 */ 0,      0,      0,      0,      0,      0x00A7, 0x2227, 0x221E,
	/* 0xE0 */ 0x03B1, 0x03B2, 0x0393, 0x03C0, 0x03A3, 0x03C3, 0x00B5, 0x03C4,
	/* 0xE8 */ 0x03A6, 0x0398, 0x03A9, 0x03B4, 0x222E, 0x03C6, 0x2208, 0x2229,
	/* 0xF0 */ 0x2261, 0x00B1, 0x2265, 0x2264, 0x2320, 0x2321, 0x00F7, 0x2248,
	/* 0xF8 */ 0x00B0, 0x2219, 0x00B7, 0x221A, 0x207F, 0x00B2, 0x00B3, 0x00AF
};



/// @brief		Unicode code point for an Atari character 0x80..0xFF
/// @param c	Atari character
/// @return		Code point
static unsigned short atari_to_unicode(unsigned char c)
{
	if (c >= 0xC2 && c <= 0xDC)
		return 0x05D0 + (c - 0xC2);		/* Hebrew alef.. */
	return atari_high[c - 0x80];
}


/// @brief		Atari character for a Unicode code point
/// @param u	Code point
/// @return		Atari character, or '?' if there is none
static unsigned char unicode_to_atari(unsigned long u)
{
	unsigned int c;

	if (u < 0x80)
		return (unsigned char)u;
	for (c = 0x80; c <= 0xFF; c++)
	{
		if (atari_to_unicode((unsigned char)c) == u)
			return (unsigned char)c;
	}
	return '?';
}


/// @brief		Send one Atari character to the port, as UTF-8 if conversion
///				is on
/// @param port	Our port
/// @param c	Atari character
static void send_char(unsigned short port, unsigned char c)
{
	unsigned char utf[3];
	unsigned short u;

	if (!convert || c < 0x80)
	{
		port_write(port, &c, 1);
		return;
	}

	u = atari_to_unicode(c);
	if (u < 0x800)
	{
		utf[0] = 0xC0 | (u >> 6);
		utf[1] = 0x80 | (u & 0x3F);
		port_write(port, utf, 2);
	}
	else
	{
		utf[0] = 0xE0 | (u >> 12);
		utf[1] = 0x80 | ((u >> 6) & 0x3F);
		utf[2] = 0x80 | (u & 0x3F);
		port_write(port, utf, 3);
	}
}


static char *linea_base(void);


/// @brief		Erase to end of line (VT52 ESC K) by writing spaces.
///				TOS' own ESC K damages other characters on the line when the
///				cursor is on an odd column in our 16-colour screen mode (some
///				of their bit planes are cleared). Plain characters are drawn
///				correctly, so spaces are written instead, with wrap turned off
///				so the last column does not move the cursor to the next line,
///				and the cursor is put back with ESC Y.
static void erase_eol(void)
{
	char *la = linea_base();
	short x  = *(short *)(la - 28);		/* V_CUR_XY: cursor column */
	short y  = *(short *)(la - 26);		/* V_CUR_XY: cursor row */
	short mx = *(short *)(la - 44);		/* V_CEL_MX: last column */
	unsigned char stat = *(unsigned char *)(la - 6);	/* V_STAT_0, bit 3 = wrap */
	short i;

	Bconout(2, 0x1B); Bconout(2, 'w');			// Wrap off
	for (i = x; i <= mx; i++)
		Bconout(2, ' ');
	Bconout(2, 0x1B); Bconout(2, 'Y');			// Cursor back
	Bconout(2, 32 + y);
	Bconout(2, 32 + x);
	if (stat & 0x08)
	{
		Bconout(2, 0x1B); Bconout(2, 'v');		// Wrap on again
	}
}


/// @brief		Send the current colours to the VT52 screen (ESC b, ESC c)
static void vt_colors(void)
{
	int fg, bg, t;

	// Theme default, or the ANSI colour chosen by Linux
	if (fg_ansi < 0)
		fg = dark ? 0 : 15;
	else
		fg = (fg_bright || bold) ? ansi_bright[fg_ansi] : ansi_normal[fg_ansi];
	if (bg_ansi < 0)
		bg = dark ? 15 : 0;
	else
		bg = bg_bright ? ansi_bright[bg_ansi] : ansi_normal[bg_ansi];

	if (reverse)
	{
		t  = fg;
		fg = bg;
		bg = t;
	}

	Bconout(2, 0x1B); Bconout(2, 'b'); Bconout(2, 0x30 | fg);
	Bconout(2, 0x1B); Bconout(2, 'c'); Bconout(2, 0x30 | bg);
}


/// @brief		Back to the theme colours. Optionally clear the screen, so the
///				whole screen gets the new background.
/// @param clear	1 to clear the screen (ESC E)
static void theme_apply(int clear)
{
	fg_ansi = bg_ansi = -1;
	fg_bright = bg_bright = bold = reverse = 0;
	vt_colors();
	if (clear)
	{
		Bconout(2, 0x1B);
		Bconout(2, 'E');
	}
}


/// @brief			Apply an ANSI SGR sequence (ESC [ p1;p2;... m)
/// @param par		Parameters
/// @param npar		Number of parameters (0 means a single 0)
static void ansi_sgr(const int *par, int npar)
{
	int i, p;

	if (npar == 0)
		npar = 1;				// ESC [ m is the same as ESC [ 0 m

	for (i = 0; i < npar; i++)
	{
		p = par[i];
		if (p == 0)
		{
			fg_ansi = bg_ansi = -1;
			fg_bright = bg_bright = bold = reverse = 0;
		}
		else if (p == 1)			bold = 1;
		else if (p == 22)			bold = 0;
		else if (p == 7)			reverse = 1;
		else if (p == 27)			reverse = 0;
		else if (p >= 30 && p <= 37) { fg_ansi = p - 30;  fg_bright = 0; }
		else if (p == 39)			fg_ansi = -1;
		else if (p >= 40 && p <= 47) { bg_ansi = p - 40;  bg_bright = 0; }
		else if (p == 49)			bg_ansi = -1;
		else if (p >= 90 && p <= 97) { fg_ansi = p - 90;  fg_bright = 1; }
		else if (p >= 100 && p <= 107) { bg_ansi = p - 100; bg_bright = 1; }
		else if ((p == 38 || p == 48) && i + 2 < npar && par[i + 1] == 5)
		{
			// 256-colour form ESC [ 38;5;n m: use the first 16 only
			int n = par[i + 2];

			if (n < 16)
			{
				if (p == 38) { fg_ansi = n & 7; fg_bright = n >= 8; }
				else         { bg_ansi = n & 7; bg_bright = n >= 8; }
			}
			i += 2;
		}
		else if ((p == 38 || p == 48) && i + 1 < npar && par[i + 1] == 2)
		{
			i += 4;					// 24-bit colour ESC [ 38;2;r;g;b m: ignored
		}
		// Everything else (underline, blink, ...) is ignored
	}
	vt_colors();
}


/// @brief		Write one (Atari) character to the screen. ANSI CSI sequences
///				(ESC [ ...) are filtered out here: colour codes are translated
///				to VT52, all others are dropped, since VT52 does not know them.
///				VT52 sequences (ESC A, ESC Y r c, ...) pass through unchanged.
/// @param c	Character
static void term_out(unsigned char c)
{
	static int state;				/* 0 normal, 1 after ESC, 2 in CSI */
	static int par[16];
	static int npar, have_digit;

	switch (state)
	{
	case 0:
		if (c == 0x1B)
			state = 1;				// Wait for the next byte
		else
			Bconout(2, c);
		break;

	case 1:
		if (c == '[')
		{
			// Start of an ANSI CSI sequence
			state      = 2;
			npar       = 0;
			have_digit = 0;
			par[0]     = 0;
		}
		else if (c == 'K' && fix_eol)
		{
			// Erase to end of line, done with spaces (see erase_eol)
			state = 0;
			erase_eol();
		}
		else
		{
			// A VT52 sequence: pass ESC and this byte on
			state = 0;
			Bconout(2, 0x1B);
			Bconout(2, c);
		}
		break;

	case 2:
		if (c >= '0' && c <= '9')
		{
			if (npar < 16)
				par[npar] = par[npar] * 10 + (c - '0');
			have_digit = 1;
		}
		else if (c == ';')
		{
			if (npar < 16)
				npar++;
			if (npar < 16)
				par[npar] = 0;
			have_digit = 0;
		}
		else if (c >= 0x40 && c <= 0x7E)
		{
			// Final byte. Only SGR (m) is used, the rest is dropped.
			if (have_digit || npar > 0)
				npar++;
			if (npar > 16)
				npar = 16;
			if (c == 'm')
				ansi_sgr(par, npar);
			state = 0;
		}
		else if (c < 0x20 || npar >= 16)
		{
			state = 0;				// Broken sequence
		}
		// Private markers and intermediates ('?', '>', ' ', ...) are skipped
		break;
	}
}


/// @brief		Show a received byte in debug mode: control characters and
///				bytes >= 0x7F in hex, e.g. [1B], printable ASCII as it is.
///				A line feed also starts a new screen line.
/// @param b	Byte from Linux
static void debug_byte(unsigned char b)
{
	static const char hex[] = "0123456789ABCDEF";

	if (b >= 0x20 && b < 0x7F)
	{
		Bconout(2, b);
		return;
	}
	Bconout(2, '[');
	Bconout(2, hex[b >> 4]);
	Bconout(2, hex[b & 15]);
	Bconout(2, ']');
	if (b == 0x0A)
	{
		Bconout(2, 0x0D);
		Bconout(2, 0x0A);
	}
}


/// @brief		Write one byte from the port to the screen. With conversion on,
///				UTF-8 sequences are decoded and shown as Atari characters.
///				The decoder state is kept between calls, since a sequence
///				can be split between two reads.
/// @param b	Byte from Linux
static void show_byte(unsigned char b)
{
	static unsigned long code;		/* Code point being decoded */
	static int more;				/* Continuation bytes still expected */

	if (debug)
	{
		debug_byte(b);
		return;
	}

	if (!convert)
	{
		term_out(b);
		return;
	}

	if (b < 0x80)
	{
		// Plain ASCII (also ends a broken sequence)
		if (more)
			term_out('?');
		more = 0;
		term_out(b);
	}
	else if ((b & 0xC0) == 0x80)
	{
		// Continuation byte
		if (more)
		{
			code = (code << 6) | (b & 0x3F);
			if (--more == 0)
				term_out(unicode_to_atari(code));
		}
		else
			term_out('?');
	}
	else
	{
		// Start of a sequence: 2, 3 or 4 bytes
		if (more)
			term_out('?');
		if ((b & 0xE0) == 0xC0)
		{
			code = b & 0x1F;
			more = 1;
		}
		else if ((b & 0xF0) == 0xE0)
		{
			code = b & 0x0F;
			more = 2;
		}
		else if ((b & 0xF8) == 0xF0)
		{
			code = b & 0x07;
			more = 3;
		}
		else
		{
			more = 0;
			term_out('?');
		}
	}
}


/// @brief		Is the sv3tty TOS driver (TSR) installed? It uses the same
///				mailbox, so without -b this program must not run at the
///				same time. Called through Supexec, since the cookie jar is
///				only readable in supervisor mode.
/// @return		The driver's struct sv3t_info, or 0 if it is not installed
static long find_driver(void)
{
	long value;

	if (sv3t_get_cookie(SV3T_COOKIE, &value))
		return value;
	return 0;
}


/// @brief		Wait for a key, so the text can be read before the program ends
///				when it was started from the desktop
static void wait_key(void)
{
	fflush(stdout);
	(void)Cconws("Press a key\r\n");
	(void)Bconin(2);
}


/// @brief		sv3p_start in supervisor mode, called through Supexec
/// @return		0
static long super_start(void)
{
	sv3p_start(&proto);
	return 0;
}


/// @brief		sv3p_poll in supervisor mode, called through Supexec. Only the
///				mailbox and _hz_200 need supervisor mode; sv3p_read/write
///				only touch our own buffers and run in user mode.
/// @return		0
static long super_poll(void)
{
	sv3p_poll(&proto);
	return 0;
}


/// @brief		Line-A base address. The text screen size is stored at
///				negative offsets from it (V_CEL_MX, V_CEL_MY).
/// @return		Line-A variable base
static char *linea_base(void)
{
	register char *a0 __asm__("a0");

	__asm__ volatile (".dc.w 0xa000" : "=r"(a0) : : "d0", "d1", "d2", "a1", "a2", "memory");
	return a0;
}


/// @brief		Get the size of the TOS text screen
/// @param rows	Gets the number of rows
/// @param cols	Gets the number of columns
static void screen_size(unsigned char *rows, unsigned char *cols)
{
	char *la = linea_base();
	short mx = *(short *)(la - 44);		/* V_CEL_MX: last column */
	short my = *(short *)(la - 42);		/* V_CEL_MY: last row */

	*cols = (unsigned char)(mx + 1);
	*rows = (unsigned char)(my + 1);
}


/// @brief		Write a string to the screen with Bconout
/// @param str	Text
static void con_puts(const char *str)
{
	while (*str)
		Bconout(2, (unsigned char)*str++);
}


/// @brief		Write a statistics line to the screen. (It is not sent to the
///				port: a logged in shell would take it as commands.)
/// @param port	Our port (unused)
/// @param line	Text, ending with \r\n
static void stats_line(unsigned short port, const char *line)
{
	(void)port;
	con_puts(line);
}


/// @brief		Show link state and statistics on the screen. Line wrap is off
///				in the TOS console, so every line is kept below 80 characters.
/// @param port	Our port
static void show_stats(unsigned short port)
{
	struct sv3p_port *p = &ps->ports[port];
	char *la = linea_base();
	char line[100];

	sprintf(line, "\r\n[sv3ttest] link %s (peer v%u), port %u%s\r\n",
			ps->link_up ? "UP" : "WAIT", ps->peer_version, port,
			bios_dev >= 0 ? " (through the driver)" : "");
	stats_line(port, line);
	sprintf(line, "  port:   announced %u credited %u carrier %u tx_credit %lu credit_owed %lu\r\n",
			p->announced, p->credited, p->carrier, p->tx_credit, p->credit_owed);
	stats_line(port, line);
	sprintf(line, "  buffer: rx_buf %ld tx_buf %ld rx_total %lu (rx head %d tail %d)\r\n",
			sv3p_rx_count(ps, port), (long)(SV3P_TX(p)->buflen - 1) - sv3p_tx_room(ps, port),
			p->rx_total, SV3P_RX(p)->head, SV3P_RX(p)->tail);
	stats_line(port, line);
	sprintf(line, "  rx:     msgs %lu bytes %lu dropped %lu\r\n",
			ps->rx_msgs, ps->rx_bytes, ps->rx_dropped);
	stats_line(port, line);
	sprintf(line, "  rx err: bad_hdr %lu timeouts %lu unknown %lu\r\n",
			ps->rx_bad_hdr, ps->rx_timeouts, ps->rx_unknown);
	stats_line(port, line);
	sprintf(line, "  tx:     msgs %lu bytes %lu timeouts %lu\r\n",
			ps->tx_msgs, ps->tx_bytes, ps->tx_timeouts);
	stats_line(port, line);
	sprintf(line, "  link:   resyncs %lu skipped_words %lu\r\n",
			ps->resyncs, ps->skipped_words);
	stats_line(port, line);

	// What TOS believes about the screen (to understand the VT52 problems)
	sprintf(line, "  screen: planes %d lin_wr %d rez %d mode %04x base %08lx\r\n",
			*(short *)la, *(short *)(la + 2),
			(int)Getrez(), (unsigned short)VsetMode(-1), (unsigned long)Logbase());
	stats_line(port, line);
	sprintf(line, "  vt52:   fg %d bg %d cursor %d,%d stat0 %02x\r\n",
			*(short *)(la - 36), *(short *)(la - 38),
			*(short *)(la - 28), *(short *)(la - 26),
			*(unsigned char *)(la - 6));
	stats_line(port, line);
}


/// @brief		Send one key to the port
/// @param port	Our port
/// @param key	Bconin(2) value: scan code in bits 16..23, ASCII in bits 0..7
/// @return		0 to continue, 1 to quit
static int handle_key(unsigned short port, long key)
{
	unsigned char scan  = (unsigned char)(key >> 16);
	unsigned char ascii = (unsigned char)key;
	unsigned char seq[2];

	switch (scan)
	{
	case SCAN_UNDO:
		return 1;

	case SCAN_HELP:
		show_stats(port);
		return 0;

	case SCAN_F7:
		fix_eol = !fix_eol;
		con_puts(fix_eol ? "\r\n[sv3ttest] ESC K done with spaces\r\n"
						 : "\r\n[sv3ttest] ESC K done by TOS\r\n");
		return 0;

	case SCAN_F8:
		// Switch theme and clear the screen, so all of it gets the new
		// background. Ctrl-L on Linux redraws most programs.
		dark = !dark;
		theme_apply(1);
		return 0;

	case SCAN_F9:
		debug = !debug;
		con_puts(debug ? "\r\n[sv3ttest] debug on: control bytes shown in hex\r\n"
					   : "\r\n[sv3ttest] debug off\r\n");
		return 0;

	case SCAN_F10:
		convert = !convert;
		con_puts(convert ? "\r\n[sv3ttest] charset conversion Atari <-> UTF-8 on\r\n"
						 : "\r\n[sv3ttest] charset conversion off\r\n");
		return 0;

	case SCAN_BACKSPACE:
		// Linux uses DEL as erase character by default
		seq[0] = 0x7F;
		port_write(port, seq, 1);
		return 0;

	case SCAN_UP:		seq[1] = 'A'; break;
	case SCAN_DOWN:		seq[1] = 'B'; break;
	case SCAN_RIGHT:	seq[1] = 'C'; break;
	case SCAN_LEFT:		seq[1] = 'D'; break;

	default:
		if (ascii)
			send_char(port, ascii);
		return 0;
	}

	// Arrow key: VT52 sequence ESC A..D
	seq[0] = 0x1B;
	port_write(port, seq, 2);
	return 0;
}


int main(int argc, char *argv[])
{
	unsigned short port = 0;
	unsigned char rows, cols;
	unsigned char last_link = 0xFF, last_carrier = 0xFF;
	unsigned char buf[256];
	struct sv3t_info *info;
	int argi = 1;
	long n;
	int quit = 0;

	// -b: use the port through the sv3tty driver's BIOS device
	if (argc > argi && argv[argi][0] == '-' && argv[argi][1] == 'b')
	{
		bios_dev = 0;
		argi++;
	}
	if (argc > argi)
		port = (unsigned short)atoi(argv[argi]);
	if (port >= NPORTS)
	{
		printf("Port must be 0..%d\n", NPORTS - 1);
		wait_key();
		return 1;
	}

	info = (struct sv3t_info *)Supexec(find_driver);
	if (bios_dev < 0 && info)
	{
		printf("sv3ttest: the sv3tty driver is installed and uses the mailbox.\n"
			   "Use sv3ttest -b [port] to go through the driver instead.\n");
		wait_key();
		return 1;
	}
	if (bios_dev >= 0)
	{
		if (!info || info->version != SV3T_INFO_VERSION || port >= info->nports)
		{
			printf("sv3ttest -b: the sv3tty driver (version %d, %d ports) is not installed\n",
				   SV3T_INFO_VERSION, NPORTS);
			wait_key();
			return 1;
		}
		bios_dev = info->first_dev + port;
		ps       = info->proto;
	}

	screen_size(&rows, &cols);

	// Theme colours and a cleared screen before anything is written
	theme_apply(1);

	if (bios_dev >= 0)
		printf("sv3ttest: terminal on BIOS device %ld = sv3tty port %u (/dev/ttySV%u)\n",
			   bios_dev, port, port);
	else
		printf("sv3ttest: terminal on sv3tty port %u (/dev/ttySV%u), screen %ux%u\n",
			   port, port, cols, rows);
	printf("UNDO quit, HELP statistics, F7 ESC K fix, F8 dark/light, F9 debug, F10 charset\n");
	fflush(stdout);

	// Without -b the program does the protocol itself. It runs in user mode;
	// the mailbox and _hz_200 can only be reached in supervisor mode, so
	// sv3p_start and sv3p_poll are called through Supexec, which runs them
	// on the system's supervisor stack.
	// With -b the driver does all that; the first Bconstat opens the port.
	if (bios_dev < 0)
	{
		sv3p_init(&proto, ports, NPORTS);
		sv3p_open(&proto, port, rxbuf, RXSIZE, txbuf, TXSIZE, rows, cols);
		Supexec(super_start);
	}
	else
		(void)Bconstat((short)bios_dev);

	while (!quit)
	{
		if (bios_dev < 0)
			Supexec(super_poll);

		// Report changes of the link state and of carrier
		if (ps->link_up != last_link)
		{
			last_link = ps->link_up;
			con_puts(last_link ? "\r\n[sv3ttest] link up\r\n" : "\r\n[sv3ttest] waiting for link...\r\n");
		}
		if (ps->ports[port].carrier != last_carrier)
		{
			last_carrier = ps->ports[port].carrier;
			if (ps->link_up)
				con_puts(last_carrier ? "[sv3ttest] carrier up\r\n" : "[sv3ttest] carrier down\r\n");
		}
		if (ps->ports[port].got_break)
		{
			ps->ports[port].got_break = 0;
			con_puts("[sv3ttest] break\r\n");
		}

		// Data from Linux to the screen (VT52 emulation of TOS)
		if (bios_dev >= 0)
		{
			// Through the driver: Bconin for each character waiting
			n = 0;
			while (n < (long)sizeof(buf) && Bconstat((short)bios_dev))
				buf[n++] = (unsigned char)Bconin((short)bios_dev);
		}
		else
			n = sv3p_read(&proto, port, buf, sizeof(buf));
		if (n > 0)
		{
			long i;

			for (i = 0; i < n; i++)
				show_byte(buf[i]);
		}

		// Keyboard to Linux
		while (!quit && Bconstat(2))
			quit = handle_key(port, Bconin(2));
	}

	// Without -b: send CLOSE and let it go out. With -b the port stays open
	// in the driver (closing comes in the next step of the driver).
	if (bios_dev < 0)
	{
		sv3p_close(&proto, port);
		Supexec(super_poll);
	}

	// Back to the normal TOS colours (black on white) for the desktop
	dark = 0;
	theme_apply(1);

	printf("\nsv3ttest: closed\n");
	wait_key();
	return 0;
}
