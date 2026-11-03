/*
 * charset.h
 *
 * Conversion between the Atari ST character set, which TOS and the MiNT
 * console windows use, and UTF-8, which Linux uses. So that e.g. å, ä, ö
 * typed on the Atari arrive as the right characters in Linux, and the
 * other way.
 *
 * Plain C without library calls.
 */

#ifndef SV3TTY_CHARSET_H
#define SV3TTY_CHARSET_H

/* UTF-8 decoder state, kept between calls, since a sequence can be split
 * between two reads. Zero it before the first use. */
struct utf8_decoder
{
	unsigned long	code;		/* Code point being decoded */
	int				more;		/* Continuation bytes still expected */
};

/* Unicode code point of an Atari character */
unsigned short atari_to_unicode(unsigned char c);

/* Atari character for a code point, or '?' if there is none */
unsigned char unicode_to_atari(unsigned long u);

/* Encode an Atari character as UTF-8. out must have room for 3 bytes.
 * Returns the number of bytes written (1..3). */
int atari_to_utf8(unsigned char c, unsigned char *out);

/* Feed one UTF-8 byte to the decoder. Writes the Atari characters it
 * completes to out (room for 2 bytes: a broken sequence gives a '?' before
 * the next character) and returns how many. */
int utf8_to_atari(struct utf8_decoder *d, unsigned char b, unsigned char *out);

#endif /* SV3TTY_CHARSET_H */
