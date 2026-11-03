/*
 * charset.c
 *
 * Conversion between the Atari ST character set and UTF-8, see charset.h.
 */

#include "charset.h"


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


/// @brief		Unicode code point of an Atari character
/// @param c	Atari character
/// @return		Code point
unsigned short atari_to_unicode(unsigned char c)
{
	if (c < 0x80)
		return c;
	if (c >= 0xC2 && c <= 0xDC)
		return 0x05D0 + (c - 0xC2);		/* Hebrew alef.. */
	return atari_high[c - 0x80];
}


/// @brief		Atari character for a Unicode code point
/// @param u	Code point
/// @return		Atari character, or '?' if there is none
unsigned char unicode_to_atari(unsigned long u)
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


/// @brief		Encode an Atari character as UTF-8
/// @param c	Atari character
/// @param out	Gets 1..3 bytes
/// @return		Number of bytes
int atari_to_utf8(unsigned char c, unsigned char *out)
{
	unsigned short u = atari_to_unicode(c);

	if (u < 0x80)
	{
		out[0] = (unsigned char)u;
		return 1;
	}
	if (u < 0x800)
	{
		out[0] = 0xC0 | (u >> 6);
		out[1] = 0x80 | (u & 0x3F);
		return 2;
	}
	out[0] = 0xE0 | (u >> 12);
	out[1] = 0x80 | ((u >> 6) & 0x3F);
	out[2] = 0x80 | (u & 0x3F);
	return 3;
}


/// @brief		Feed one UTF-8 byte to the decoder
/// @param d	Decoder state
/// @param b	Byte from Linux
/// @param out	Gets the completed Atari characters (at most 2)
/// @return		Number of characters written to out
int utf8_to_atari(struct utf8_decoder *d, unsigned char b, unsigned char *out)
{
	int n = 0;

	if (b < 0x80)
	{
		// Plain ASCII (also ends a broken sequence)
		if (d->more)
			out[n++] = '?';
		d->more = 0;
		out[n++] = b;
	}
	else if ((b & 0xC0) == 0x80)
	{
		// Continuation byte
		if (d->more)
		{
			d->code = (d->code << 6) | (b & 0x3F);
			if (--d->more == 0)
				out[n++] = unicode_to_atari(d->code);
		}
		else
			out[n++] = '?';
	}
	else
	{
		// Start of a sequence: 2, 3 or 4 bytes
		if (d->more)
			out[n++] = '?';
		if ((b & 0xE0) == 0xC0)
		{
			d->code = b & 0x1F;
			d->more = 1;
		}
		else if ((b & 0xF0) == 0xE0)
		{
			d->code = b & 0x0F;
			d->more = 2;
		}
		else if ((b & 0xF8) == 0xF0)
		{
			d->code = b & 0x07;
			d->more = 3;
		}
		else
		{
			d->more = 0;
			out[n++] = '?';
		}
	}
	return n;
}
