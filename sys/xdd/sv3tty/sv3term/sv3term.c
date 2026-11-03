/*
 * sv3term.c
 *
 * Terminal program for FreeMiNT: connects the console window it runs in
 * (Conholio, Toswin2, ...) to an sv3tty port, /dev/ttySVn, so a Linux
 * session on the SuperVidel 3 can be used in the window.
 *
 * - The port and the window are put in raw mode without echo; every key
 *   goes to Linux unchanged, and Linux does the echo and line editing.
 * - Characters are converted between the Atari character set and UTF-8
 *   (-n turns this off).
 * - The window size is sent to the port (TIOCSWINSZ), which the driver
 *   passes on to Linux, again when the window changes size (SIGWINCH).
 * - The default port is /dev/ttySV1, where Linux runs a getty with
 *   TERM=vt100, since the MiNT console windows emulate VT100/ANSI.
 * - Ctrl-T quits (-e x makes it Ctrl-x). Closing the port hangs up the
 *   session on Linux.
 *
 * Usage: sv3term [-p port] [-n] [-e x] [device]
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <termios.h>
#include <signal.h>
#include <errno.h>
#include <sys/ioctl.h>
#include <sys/select.h>

#include "../charset.h"


#define CTRL(c)			((c) & 0x1F)

static int				port_fd = -1;
static int				convert = 1;			/* Atari charset <-> UTF-8 */
static unsigned char	escape = CTRL('T');		/* Quits */

static struct termios	con_saved;				/* The window's own settings */
static int				con_saved_ok;

static volatile sig_atomic_t winch = 1;			/* Size to send; 1: send at start */
static volatile sig_atomic_t quit;


/// @brief		Put a terminal in raw mode: no echo, no line editing, no
///				signals from keys, no CR/NL changes, 8 bits
/// @param t	Settings to change
static void make_raw(struct termios *t)
{
	t->c_iflag &= ~(IGNBRK | BRKINT | PARMRK | ISTRIP | INLCR | IGNCR | ICRNL | IXON | IXOFF);
	t->c_oflag &= ~OPOST;
	t->c_lflag &= ~(ECHO | ECHONL | ICANON | ISIG | IEXTEN);
	t->c_cflag &= ~(CSIZE | PARENB);
	t->c_cflag |= CS8 | CREAD | CLOCAL;
	t->c_cc[VMIN]  = 1;
	t->c_cc[VTIME] = 0;
}


/// @brief		Give the window back its own settings. Also called at exit.
static void restore_console(void)
{
	if (con_saved_ok)
		tcsetattr(0, TCSAFLUSH, &con_saved);
	con_saved_ok = 0;
}


/// @brief		SIGWINCH: the window has changed size, send it at the next turn
static void on_winch(int sig)
{
	(void)sig;
	winch = 1;
}


/// @brief		SIGHUP, SIGTERM: quit at the next turn
static void on_quit(int sig)
{
	(void)sig;
	quit = 1;
}


/// @brief		Send the window size to the port, if the window has one
static void send_winsize(void)
{
	struct winsize ws;

	if (ioctl(0, TIOCGWINSZ, &ws) == 0 && ws.ws_row > 0 && ws.ws_col > 0)
		ioctl(port_fd, TIOCSWINSZ, &ws);
}


/// @brief		Write all of a buffer, also when the device takes it in parts
/// @param fd	File
/// @param buf	Bytes
/// @param len	Number of bytes
/// @return		0, or -1 on an error
static int write_all(int fd, const unsigned char *buf, long len)
{
	while (len > 0)
	{
		long n = write(fd, buf, len);

		if (n < 0)
		{
			if (errno == EINTR)
				continue;
			return -1;
		}
		buf += n;
		len -= n;
	}
	return 0;
}


/// @brief		Keys from the window to the port, converted to UTF-8
/// @return		0, 1 when the escape key was pressed, -1 on an error
static int keys_to_port(void)
{
	unsigned char in[256], out[3 * 256];
	long n, i, len = 0;

	n = read(0, in, sizeof(in));
	if (n <= 0)
		return (n < 0 && errno == EINTR) ? 0 : -1;

	for (i = 0; i < n; i++)
	{
		if (in[i] == escape)
		{
			write_all(port_fd, out, len);
			return 1;
		}
		if (convert)
			len += atari_to_utf8(in[i], out + len);
		else
			out[len++] = in[i];
	}
	return write_all(port_fd, out, len);
}


/// @brief		Data from the port to the window, converted from UTF-8
/// @return		0, or -1 when the port gives an error or end of file
static int port_to_window(void)
{
	static struct utf8_decoder dec;
	unsigned char in[512], out[2 * 512];
	long n, i, len = 0;

	n = read(port_fd, in, sizeof(in));
	if (n <= 0)
		return (n < 0 && errno == EINTR) ? 0 : -1;

	if (!convert)
		return write_all(1, in, n);

	for (i = 0; i < n; i++)
		len += utf8_to_atari(&dec, in[i], out + len);
	return write_all(1, out, len);
}


/// @brief		Show how to use the program
static void usage(void)
{
	fprintf(stderr,
		"usage: sv3term [-p port] [-n] [-e x] [device]\n"
		"  -p port  sv3tty port 0..15 (/dev/ttySVport), default 1\n"
		"  -n       no Atari <-> UTF-8 character conversion\n"
		"  -e x     quit with Ctrl-x instead of Ctrl-T\n"
		"  device   another device instead of /dev/ttySVport\n");
	exit(2);
}


int main(int argc, char **argv)
{
	char device[32];
	struct termios t;
	int c;

	strcpy(device, "/dev/ttySV1");

	while ((c = getopt(argc, argv, "p:ne:")) != -1)
	{
		switch (c)
		{
		case 'p':
			{
				int port = atoi(optarg);

				if (port < 0 || port > 15)
					usage();
				sprintf(device, "/dev/ttySV%d", port);
			}
			break;
		case 'n':
			convert = 0;
			break;
		case 'e':
			escape = CTRL((unsigned char)optarg[0]);
			break;
		default:
			usage();
		}
	}
	if (optind < argc)
	{
		if (strlen(argv[optind]) >= sizeof(device))
			usage();
		strcpy(device, argv[optind]);
	}

	// The port, in raw mode. (Its settings go back to the defaults when
	// the last file on it is closed, so they need not be restored.)
	port_fd = open(device, O_RDWR | O_NOCTTY);
	if (port_fd < 0)
	{
		perror(device);
		return 1;
	}
	if (tcgetattr(port_fd, &t) == 0)
	{
		make_raw(&t);
		tcsetattr(port_fd, TCSANOW, &t);
	}

	// The window, in raw mode, restored at exit
	if (tcgetattr(0, &con_saved) == 0)
	{
		con_saved_ok = 1;
		atexit(restore_console);
		t = con_saved;
		make_raw(&t);
		tcsetattr(0, TCSAFLUSH, &t);
	}

	signal(SIGWINCH, on_winch);
	signal(SIGHUP, on_quit);
	signal(SIGTERM, on_quit);

	printf("sv3term: connected to %s, Ctrl-%c quits\r\n", device, escape + '@');
	fflush(stdout);

	while (!quit)
	{
		fd_set rd;

		if (winch)
		{
			winch = 0;
			send_winsize();
		}

		FD_ZERO(&rd);
		FD_SET(0, &rd);
		FD_SET(port_fd, &rd);
		if (select(port_fd + 1, &rd, NULL, NULL, NULL) < 0)
		{
			if (errno == EINTR)
				continue;
			break;
		}

		if (FD_ISSET(port_fd, &rd) && port_to_window() < 0)
			break;
		if (FD_ISSET(0, &rd))
		{
			c = keys_to_port();
			if (c)
				break;
		}
	}

	restore_console();
	close(port_fd);
	printf("\r\nsv3term: disconnected\r\n");
	return 0;
}
