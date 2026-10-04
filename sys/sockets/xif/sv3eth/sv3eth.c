/*****************************************************************************
*******************************************************************************/




/*
 * SV3 ethernet driver for FreeMiNT.
 *
 * This file belongs to FreeMiNT. It's not in the original MiNT 1.12
 * distribution. See the file CHANGES for a detailed log of changes.
 *
 * Copyright (c) 2023 Torbjorn and Henrik Gilda
 *
 * This file is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2, or (at your option)
 * any later version.
 *
 * This file is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 675 Mass Ave, Cambridge, MA 02139, USA.
 */

/*
 *	SV3 ethernet packet driver version 0.92
 *  SuperVidel3 FW v301 or higher is required by this driver
 *
 *	Usage:
 *		ifconfig en0 addr u.v.w.x
 *		route add u.v.w.x en0
 *
 *		20230905	/Torbjörn and Henrik Gildå
 *
 */

#include "global.h"

#include "buf.h"
#include "inet4/if.h"
#include "inet4/ifeth.h"

#include "netinfo.h"
#include "mint/asm.h"
#include "mint/delay.h"
#include "mint/mdelay.h"
#include "mint/sockio.h"
#include "mint/endian.h"

#include "sv_regs.h"

#include <mint/osbind.h>


// Max Ethernet frame size we accept, header+payload, no CRC.
// 1514 = standard (14 header + 1500 payload)
// 1518 = with support for 802.1Q VLAN-tag (14+4 header + 1500 payload)
#define SV3ETH_MAX_RX_LEN   1518UL

/*
 * From main.c
 */
extern long driver_init (void);

void SV3_mbox0_isr(void);

/*
 * Our interface structure
 */
static struct netif if_sv3eth;



/*
 * Prototypes for our service functions
 */
static long	sv3eth_open		(struct netif *);
static long	sv3eth_close	(struct netif *);
static long	sv3eth_output	(struct netif *, BUF *, const char *, short, short);
static long	sv3eth_ioctl	(struct netif *, short, long);
static long	sv3eth_config	(struct netif *, struct ifopt *);
static void sv3eth_timeout	(struct netif *);

//does actual sending of packets for sv3eth_output() and sv3eth_service()
static long send_packet			(struct netif *nif, BUF *nbuf, long buf_alloc_type, uint32 slot, uint32 last_packet);

// Service function to check for incoming and outgoing packets
static void sv3eth_service (struct netif * nif, uint32 int_src);

// For our interrupt
static void sv3eth_install_int (void);

void write_len_fifo_ctrl( uint32_t wdata );
void Init_BD(void);
int32 Check_Rx_Buffers(void);
void Init_DMA_slot_info(void);


/* Convert single hex char to short */
short ch2i(char);

// Convert long to hex ascii
void hex2ascii(ulong l, uchar* c);


// Variable for locking out interrupt when accessing the hardware
static volatile int in_use = 0;

// If this variable equals 1, then the interrupt will do nothing until init is done (=0)
static volatile int initializing = 1;

//static volatile int autoneg = 0;

// Keep track of when sending so we don't get fooled by TXEMPTY
//static volatile int sending = 0;

//handle for all logging
//static ushort loghandle;

uint32_t old_i6_int;

static char message[256];

//global MAC address in longword format for RX use
static unsigned long mac_addr[2];

//We keep track of which slot we checked last time this function was called.
//static uint32 cur_rx_slot = 0;

static volatile uint32	has_printed_functions = 0UL;

//static volatile uint32	in_queue = 0UL;

//static uchar upperleft[] = {27,'H',0};
//static uchar col40[] = {27,'Y',32,72,0};

//This is the pointer to the whole memory block for all the packet buffers needed.
//It is fixed at 0x9F000000.
volatile PS_DMA_BUFFERS* ps_dma_bufs = (PS_DMA_BUFFERS*)PS_DMA_BASE;
static volatile uint8_t	 tx_dma_pkt_flags[PS_DMA_BUFFER_PKTS];
static volatile uint32_t tx_dma_pos;

// Diagnostic counters, safe to increment from ISR context.
// They are never printed from ISR, root timeout or packet processing
// context, since c_conws() writes to fd 1 of whatever process happens
// to be current and may sleep. They are printed on request with
// "ifconfig en0 -f <file>", where <file> contains the line "stats 1".
// That runs sv3eth_config() in the context of the ifconfig process.
static volatile uint32_t bad_slot_count		= 0;   // slot/pos value from HW was >= PS_DMA_BUFFER_PKTS
static volatile uint32_t oversized_rx_count	= 0;   // RX packet len > SV3ETH_MAX_RX_LEN
static volatile uint32_t tx_fifo_full_count	= 0;   // Len FIFO was full when we tried to ACK an oversized RX packet
static volatile uint32_t rx_ack_fifo_full_count	= 0;   // Len FIFO was full when we tried to ACK a normal RX packet
static volatile uint32_t tx_build_hdr_fail_count	= 0;   // eth_build_hdr() failed in sv3eth_output()
static volatile uint32_t tx_too_large_count	= 0;   // TX packet too large, dropped
static volatile uint32_t tx_fifo_full_pre_count	= 0;   // send_packet(): Len FIFO full before packet copy
static volatile uint32_t tx_no_free_slot_count	= 0;   // send_packet(): all TX slots busy
static volatile uint32_t tx_fifo_full_post_count	= 0;   // send_packet(): Len FIFO full after packet copy
static volatile uint32_t rx_buf_alloc_fail_count	= 0;   // buf_alloc() failed for an RX packet
static volatile uint32_t rx_if_input_fail_count	= 0;   // if_input() failed, RX queue full

// Number of entries thrown away by sv3eth_flush_mailbox(), last call from
// driver_init() and from sv3eth_open() respectively.
static uint32_t init_flush_rx = 0;
static uint32_t init_flush_tx = 0;
static uint32_t open_flush_rx = 0;
static uint32_t open_flush_tx = 0;


/// Init internal flags array for the DMA buffers in SVRAM
void Init_DMA_slot_info(void)
{
	int i;
	for (i=0; i < PS_DMA_BUFFER_PKTS; i++)
	{
		tx_dma_pkt_flags[i] = 0;
	}
	tx_dma_pos = 0;
}



/*
 * Throw away everything waiting in the length mailbox. RX packets are
 * ACKed back to the Linux side so their slots are freed, and TX ACKs
 * free our TX slots. Must be called with the mailbox RX interrupt
 * disabled, and only from process context since it prints with c_conws().
 * The number of thrown away RX packets and TX ACKs are returned in
 * *rx_out and *tx_out.
 */
static void
sv3eth_flush_mailbox (const char *caller, uint32_t *rx_out, uint32_t *tx_out)
{
	uint32 rx_pkts = 0;
	uint32 tx_acks = 0;

	while ( (mbox0.stat_ctrl & MBOX_STAT_RX_LFIFO_EMPTY) == 0 )
	{
		//Read length mailbox to get info about next packet
		//Bottom word is number of bytes
		//Top word is RX slot index to read packet from
		uint32_t len_val = mbox0.len_fifo;
		uint32_t len     = len_val & 0xFFFF;
		uint32_t raw_pos = len_val >> 16;
		uint32_t pos;

		if ( raw_pos >= PS_DMA_BUFFER_PKTS )
		{
			bad_slot_count++;
		}
		pos = raw_pos & (PS_DMA_BUFFER_PKTS - 1);

		if ( len == 0xFFFF )
		{
			//This is a special encoding that tells us to clear the
			//TX flag for the given slot
			tx_dma_pkt_flags[pos] = 0;
			tx_acks++;
		}
		else
		{
			//This is a real RX-packet
			//Send ACK to the sender that we have handled this packet
			write_len_fifo_ctrl( (pos << 16) | 0xFFFFUL );
			rx_pkts++;
		}
	}

	*rx_out = rx_pkts;
	*tx_out = tx_acks;

	ksprintf(message, "%s: Deleted %lu RX pkts and %lu TX ACKs\r\n", caller, rx_pkts, tx_acks );
	c_conws(message);
}



/*
 * This gets called when someone makes an 'ifconfig up' on this interface
 * and the interface was down before.
 */
static long
sv3eth_open (struct netif *nif)
{
//	short		tmp;

//	in_use = 1;

	c_conws("SV3ETH up!\n\r");

	//Packets that arrived while the interface was down (e.g. during boot,
	//between driver_init() and 'ifconfig en0 up') are old. Throw them away,
	//otherwise the ISR gets the whole backlog at once and overflows the
	//MintNet receive queue and buffer pool.
	sv3eth_flush_mailbox("sv3eth_open", &open_flush_rx, &open_flush_tx);

	//Enable RX interrupt for length mailbox0
	mbox0.stat_ctrl |= MBOX_STAT_RX_LFIFO_IE;

	initializing = 0;
//	in_use = 0;
	
	return 0;
}

/*
 * Opposite of sv3eth_open(), is called when 'ifconfig down' on this interface
 * is done and the interface was up before.
 */
static long
sv3eth_close (struct netif *nif)
{
	//disable RX interrupt sources BEFORE removing I6 handler.
	//Mask CPU interrupts while doing it, so an IRQ6 that is already on its
	//way is not withdrawn in the middle of an IACK cycle (bus error).
	{
		ushort sr = spl7();
		mbox0.stat_ctrl &= ~MBOX_STAT_RX_LFIFO_IE;
		spl(sr);
	}

	c_conws("SV3ETH down!\n\r");

	initializing = 1;
	return 0;
}

/*
 * This routine is responsible for enqueing a packet for later sending.
 * The packet it passed in `buf', the destination hardware address and
 * length in `hwaddr' and `hwlen' and the type of the packet is passed
 * in `pktype'.
 *
 * `hwaddr' is guaranteed to be of type nif->hwtype and `hwlen' is
 * garuanteed to be equal to nif->hwlocal.len.
 *
 * `pktype' is currently one of (definitions in if.h):
 *	PKTYPE_IP for IP packets,
 *	PKTYPE_ARP for ARP packets,
 *	PKTYPE_RARP for reverse ARP packets.
 *
 * These constants are equal to the ethernet protocol types, ie. an
 * Ethernet driver may use them directly without prior conversion to
 * write them into the `proto' field of the ethernet header.
 *
 * If the hardware is currently busy, then you can use the interface
 * output queue (nif->snd) to store the packet for later transmission:
 *	if_enqueue (&nif->snd, buf, buf->info).
 *
 * `buf->info' specifies the packet's delivering priority. if_enqueue()
 * uses it to do some priority queuing on the packets, ie. if you enqueue
 * a high priority packet it may jump over some lower priority packets
 * that were already in the queue (ie that is *no* FIFO queue).
 *
 * You can dequeue a packet later by doing:
 *	buf = if_dequeue (&nif->snd);
 *
 * This will return NULL if no more packets are left in the queue.
 *
 * The buffer handling uses the structure BUF that is defined in buf.h.
 * Basically a BUF looks like this:
 *
 * typedef struct {
 *	long buflen;
 *	char *dstart;
 *	char *dend;
 *	...
 *	char data[0];
 * } BUF;
 *
 * The structure consists of BUF.buflen bytes. Up until BUF.data there are
 * some header fields as shown above. Beginning at BUF.data there are
 * BUF.buflen - sizeof (BUF) bytes (called userspace) used for storing the
 * packet.
 *
 * BUF.dstart must always point to the first byte of the packet contained
 * within the BUF, BUF.dend points to the first byte after the packet.
 *
 * BUF.dstart should be word aligned if you pass the BUF to any MintNet
 * functions! (except for the buf_* functions itself).
 *
 * BUF's are allocated by
 *	nbuf = buf_alloc (space, reserve, mode);
 *
 * where `space' is the size of the userspace of the BUF you need, `reserve'
 * is used to set BUF.dstart = BUF.dend = BUF.data + `reserve' and mode is
 * one of
 *	BUF_NORMAL for calls from kernel space,
 *	BUF_ATOMIC for calls from interrupt handlers.
 *
 * buf_alloc() returns NULL on failure.
 *
 * Usually you need to pre- or postpend some headers to the packet contained
 * in the passed BUF. To make sure there is enough space in the BUF for this
 * use
 *	nbuf = buf_reserve (obuf, reserve, where);
 *
 * where `obuf' is the BUF where you want to reserve some space, `reserve'
 * is the amount of space to reserve and `where' is one of
 *	BUF_RESERVE_START for reserving space before BUF.dstart
 *	BUF_RESERVE_END for reserving space after BUF.dend
 *
 * Note that buf_reserve() returns pointer to a new buffer `nbuf' (possibly
 * != obuf) that is a clone of `obuf' with enough space allocated. `obuf'
 * is no longer existant afterwards.
 *
 * However, if buf_reserve() returns NULL for failure then `obuf' is
 * untouched.
 *
 * buf_reserve() does not modify the BUF.dstart or BUF.dend pointers, it
 * only makes sure you have the space to do so.
 *
 * In the worst case (if the BUF is to small), buf_reserve() allocates a new
 * BUF and copies the old one to the new one (this is when `nbuf' != `obuf').
 *
 * To avoid this you should reserve enough space when calling buf_alloc(), so
 * buf_reserve() does not need to copy. This is what MintNet does with the BUFs
 * passed to the output function, so that copying is never needed. You should
 * do the same for input BUFs, ie allocate the packet as eg.
 *	buf = buf_alloc (nif->mtu+sizeof (eth_hdr)+100, 50, BUF_ATOMIC);
 *
 * Then up to nif->mtu plus the length of the ethernet header bytes long
 * frames may ne received and there are still 50 bytes after and before
 * the packet.
 *
 * If you have sent the contents of the BUF you should free it by calling
 *	buf_deref (`buf', `mode');
 *
 * where `buf' should be freed and `mode' is one of the modes described for
 * buf_alloc().
 *
 * Functions that can be called from interrupt:
 *	buf_alloc (..., ..., BUF_ATOMIC);
 *	buf_deref (..., BUF_ATOMIC);
 *	if_enqueue ();
 *	if_dequeue ();
 *	if_input ();
 *	eth_remove_hdr ();
 *	addroottimeout (..., ..., 1);
 */

 
static long
sv3eth_output (struct netif *nif, BUF *buf, const char *hwaddr, short hwlen, short pktype)
{
	BUF				*nbuf;
	int32			result;
	ushort			sr;
//	unsigned char	littlemem;
//	static	uchar	message[100];

//	unsigned long	timeval;


	//c_conws("SV3ETH_output\n\r");		//debug

	//*ETH_REG = (*ETH_REG) | 0x80;	//enable LED2

	/*
	 * Attach eth header. MintNet provides you with the eth_build_hdr
	 * function that attaches an ethernet header to the packet in
	 * buf. It takes the BUF (buf), the interface (nif), the hardware
	 * address (hwaddr) and the packet type (pktype).
	 *
	 * Returns NULL if the header could not be attached (the passed
	 * buf is thrown away in this case).
	 *
	 * Otherwise a pointer to a new BUF with the packet and attached
	 * header is returned and the old buf pointer is no longer valid.
	 */
	nbuf = eth_build_hdr (buf, nif, hwaddr, pktype);
	if ( ((uint32)nbuf) == 0UL)
	{
		tx_build_hdr_fail_count++;
		nif->out_errors++;
		//*ETH_REG = (*ETH_REG) & 0x7F;	//disable LED2
		return ENOMEM;
	}
	nif->out_packets++;
	
	/*
	 * Here you should either send the packet to the hardware or
	 * enqueue the packet and send the next packet as soon as
	 * the hardware is finished.
	 *
	 * If you are done sending the packet free it with buf_deref().
	 *
	 * Before sending it pass it to the Berkley packet filter, for tcpdump tasks.
	 */
	if (nif->bpf)
		bpf_input (nif, nbuf);


	//Mask interrupts in the CPU instead of disabling the interrupt source
	//in the mailbox. Clearing MBOX_STAT_RX_LFIFO_IE while an IRQ6 may
	//already be on its way to the CPU can leave an IACK cycle unanswered,
	//which ends in a bus error. With spl7() a pending mailbox interrupt is
	//simply taken when the old interrupt level is restored.
	sr = spl7();

	result = send_packet(nif, nbuf, BUF_NORMAL, tx_dma_pos, tx_dma_pos == (PS_DMA_BUFFER_PKTS-1) );

	spl(sr);

	if(result == 0L)
	{
		//Use the next TX slot for next packet to send
		tx_dma_pos = (tx_dma_pos + 1) & (PS_DMA_BUFFER_PKTS-1);
	}
	else if(result == -2L)
	{
//		//packet too large, just throw away
		//The packet has already been derefed by send_packet()

		//Turn on TX interrupt sources again
//		ETH_INT_MASK = ETH_INT_MASK_RXF | ETH_INT_MASK_RXE | ETH_INT_MASK_TXE | ETH_INT_MASK_TXB;

		tx_too_large_count++;
		nif->out_errors++;

//		int_on();
//		return EMSGSIZE;
		return E_OK;
	}
	else
	{
		//no free buffer, enqueue the nbuf and wait for TX interrupt
		//if_enqueue (&nif->snd, nbuf, nbuf->info);
//		in_queue++;

		//The reason is counted in send_packet()
		nif->out_errors++;

		//just throw away
		buf_deref(nbuf, BUF_NORMAL);
	}

	//Turn on TX and RX interrupt sources again
//	ETH_INT_MASK = ETH_INT_MASK_RXF | ETH_INT_MASK_RXE | ETH_INT_MASK_TXE | ETH_INT_MASK_TXB;

	//*ETH_REG = (*ETH_REG) & 0x7F;	//disable LED2
	return E_OK;
}



/*This function sends a packet, but it should only be called by sv3eth_output() or
  the sv3eth_service() routine.
*/
//There is a risk that the TX ISR gets here too, when the sv3eth_output() (working in usermode)
//is alredy writing a packet to the DMA buffers. Therefore TX interrupt must be shut off before 
//calling send_packet(). This assumes that a TX interrupt is not missed while the TXIE is disabled (check
//this in the MAC controller docs).
static long send_packet	(struct netif *nif, BUF *nbuf, long buf_alloc_type, uint32 slot, uint32 last_packet)
{
	uint32			 i;
	volatile	uint32 j;
	volatile	uint32 *src_data_pnt;
	//volatile	uint32 *orig_src_data_pnt;
	volatile	uint32 *eth_dst_pnt;
	uint32_t			 rounded_len;
	uint32_t			 origlen;
//	uchar	message[80];

	//c_conws( "SV3ETH send_packet!\r\n" );
	
	//calculate packet length
	origlen = (nbuf->dend) - (nbuf->dstart);

	//Round the packet length up to even 4 bytes
	rounded_len = (origlen + 3) & 0xFFFC;

	//If the packet is greater than 1536 we return error
	if (rounded_len > 1536UL)
	{
		//we have no intention to send this packet, so just free it
		buf_deref (nbuf, buf_alloc_type);
		return -2;
	}

	if ( (mbox0.stat_ctrl & MBOX_STAT_TX_LFIFO_FULL) == MBOX_STAT_TX_LFIFO_FULL )
	{
		//Len FIFO is full, so don't try to send packet
		//Do not buf_deref (nbuf, buf_alloc_type), since it is done outside this function
		tx_fifo_full_pre_count++;
		return -1;
	}

	//Check the wanted TX slot if it's free
	for ( i=0; i < PS_DMA_BUFFER_PKTS;  )
	{
		if ( tx_dma_pkt_flags[slot] == 0 )
		{
			//Found a free slot
			break;
		}
		i++;
		if ( i == PS_DMA_BUFFER_PKTS )
		{
			//Failed to find free slot
			//Do not buf_deref (nbuf, buf_alloc_type), since it is done outside this function
			tx_no_free_slot_count++;
			return -1;
		}
		slot++;
		if ( slot == PS_DMA_BUFFER_PKTS )
		{
			slot = 0;
		}
	}

	//Mark the chosen slot as used
	tx_dma_pkt_flags[slot] = 1;

	//Divide rounded len by 4
	uint32_t rounded_len_lwords = rounded_len >> 2;

	//Write packet data to DDR RAM DMA buffer
	eth_dst_pnt = (volatile uint32_t*)ps_dma_bufs->txbuffers[slot].buf;
	src_data_pnt = (volatile uint32_t*)nbuf->dstart;
	//orig_src_data_pnt = src_data_pnt;
	for(i=0; i < rounded_len_lwords; i++)
	{
		*eth_dst_pnt++ = *src_data_pnt++;
	}	

	/*
	ksprintf (message, "TX slot %2lu, %4lu bytes: %08lx %08lx %08lx %08lx ... %08lx %08lx\r\n",
					slot,
					origlen,
					orig_src_data_pnt[0],
					orig_src_data_pnt[1],
					orig_src_data_pnt[2],
					orig_src_data_pnt[3],
					orig_src_data_pnt[rounded_len_lwords-2],
					orig_src_data_pnt[rounded_len_lwords-1]
				);
	c_conws (message);
	*/
	
	//Dummy loop to wait for the Supervidel CT60-write-FIFO to empty
	for (j = 0; j < 1000; j++)
	{
		asm("nop;");
	}

	//Write slot and length of data to length-mailbox
	//This tells the linux side that it has a packet to send on to its network
	uint32_t pkt_info_word = (slot << 16) + (origlen & 0xFFFFUL);
	if ( (mbox0.stat_ctrl & MBOX_STAT_TX_LFIFO_FULL) == 0 )
	{
		//Len FIFO not full, send pkt info word
		mbox0.len_fifo = pkt_info_word;
	}
	else
	{
		//Len FIFO is full, discard packet
		tx_fifo_full_post_count++;
		//Do not buf_deref (nbuf, buf_alloc_type), since it is done outside this function
		return -1;
	}


	//ksprintf (message, "T%02lu 0x%08lx\r\n", slot, eth_tx_bd[slot].len_ctrl);
	//c_conws (message);

	buf_deref (nbuf, buf_alloc_type);						//free buf because contents of packet
															//is now in the DDR DMA buffer
	
	return 0L;
}





/*
 * MintNet notifies you of some noteable IOCLT's. Usually you don't
 * need to act on them because MintNet already has done so and only
 * tells you that an ioctl happened.
 *
 * One useful thing might be SIOCGLNKFLAGS and SIOCSLNKFLAGS for setting
 * and getting flags specific to your driver. For an example how to use
 * them look at slip.c
 */
static long
sv3eth_ioctl (struct netif *nif, short cmd, long arg)
{
	struct ifreq *ifr;


//	c_conws("ioctl\n\r");		//debug

	switch (cmd)
	{
		case SIOCSIFNETMASK:
		case SIOCSIFFLAGS:
		case SIOCSIFADDR:
			return 0;
		
		case SIOCSIFMTU:
			/*
			 * Limit MTU to 1500 bytes. MintNet has already set nif->mtu
			 * to the new value, we only limit it here.
			 */
			if (nif->mtu > ETH_MAX_DLEN)
				nif->mtu = ETH_MAX_DLEN;
			return 0;
		
		case SIOCSIFOPT:
			/*
			 * Interface configuration, handled by dummy_config()
			 */
			ifr = (struct ifreq *) arg;
			return sv3eth_config (nif, ifr->ifru.data);
	}
	
	return ENOSYS;
}




/*
 * Interface configuration via SIOCSIFOPT. The ioctl is passed a
 * struct ifreq *ifr. ifr->ifru.data points to a struct ifopt, which
 * we get as the second argument here.
 *
 * If the user MUST configure some parameters before the interface
 * can run make sure that dummy_open() fails unless all the necessary
 * parameters are set.
 *
 * Return values	meaning
 * ENOSYS		option not supported
 * ENOENT		invalid option value
 * 0			Ok
 */
static long
sv3eth_config (struct netif *nif, struct ifopt *ifo)
{
//	c_conws("Config\n\r");

# define STRNCMP(s)	(strncmp ((s), ifo->option, sizeof (ifo->option)))
	
	if (!STRNCMP ("hwaddr"))
	{
		uchar *cp;
		/*
		 * Set hardware address
		 */
		if (ifo->valtype != IFO_HWADDR)
			return ENOENT;
		memcpy (nif->hwlocal.adr.bytes, ifo->ifou.v_string, ETH_ALEN);
		cp = nif->hwlocal.adr.bytes;
		cp = cp;
		DEBUG (("sv3eth: hwaddr is %x:%x:%x:%x:%x:%x", cp[0], cp[1], cp[2], cp[3], cp[4], cp[5]));
	}
	else if (!STRNCMP ("braddr"))
	{
		uchar *cp;
		/*
		 * Set broadcast address
		 */
		if (ifo->valtype != IFO_HWADDR)
			return ENOENT;
		memcpy (nif->hwbrcst.adr.bytes, ifo->ifou.v_string, ETH_ALEN);
		cp = nif->hwbrcst.adr.bytes;
		cp = cp;
		DEBUG (("sv3eth: braddr is %x:%x:%x:%x:%x:%x", cp[0], cp[1], cp[2], cp[3], cp[4], cp[5]));
	}
	else if (!STRNCMP ("debug"))
	{
		/*
		 * turn debuggin on/off
		 */
		if (ifo->valtype != IFO_INT)
			return ENOENT;
		DEBUG (("sv3eth: debug level is %ld", ifo->ifou.v_long));
	}
	else if (!STRNCMP ("log"))
	{
		/*
		 * set log file
		 */
		if (ifo->valtype != IFO_STRING)
			return ENOENT;
		DEBUG (("sv3eth: log file is %s", ifo->ifou.v_string));
	}
	else if (!STRNCMP ("stats"))
	{
		/*
		 * Print diagnostic counters. We are called from the ioctl of
		 * the ifconfig process here, so c_conws() writes to its stdout.
		 */
		(void)nif;
		ksprintf (message, "sv3eth stats: bad_slot %lu, oversized_rx %lu, "
		          "rx_ack_fifo_full %lu, oversized_ack_fifo_full %lu\r\n",
		          bad_slot_count, oversized_rx_count,
		          rx_ack_fifo_full_count, tx_fifo_full_count);
		c_conws (message);
		ksprintf (message, "sv3eth stats: tx_build_hdr_fail %lu, tx_too_large %lu, "
		          "tx_fifo_full_pre %lu, tx_no_free_slot %lu, tx_fifo_full_post %lu\r\n",
		          tx_build_hdr_fail_count, tx_too_large_count,
		          tx_fifo_full_pre_count, tx_no_free_slot_count,
		          tx_fifo_full_post_count);
		c_conws (message);
		ksprintf (message, "sv3eth stats: rx_buf_alloc_fail %lu, rx_if_input_fail %lu\r\n",
		          rx_buf_alloc_fail_count, rx_if_input_fail_count);
		c_conws (message);
		ksprintf (message, "sv3eth stats: flushed at init %lu RX %lu TX-ACK, "
		          "at open %lu RX %lu TX-ACK\r\n",
		          init_flush_rx, init_flush_tx, open_flush_rx, open_flush_tx);
		c_conws (message);
		return 0;
	}

	return ENOSYS;
}




//This timeout function is called by the kernel after an addroottimeout call,
//and is called from kernel context. So it is not ok to call GEMDOS functions
//(c_conws() included) or other things that may stall.
//The diagnostic counters are printed with the "stats" option instead,
//see sv3eth_config().
static void
sv3eth_timeout (struct netif *nif)
{
	(void)nif;
}



// ============================
//  Len FIFO write with TX_FULL
//  flag control
// ============================
void write_len_fifo_ctrl( uint32_t wdata )
{
	while (mbox0.stat_ctrl & MBOX_STAT_TX_LFIFO_FULL);
	mbox0.len_fifo = wdata;
}


/*
 * Initialization. This is called when the driver is loaded. If you
 * link the driver with main.o and init.o then this must be called
 * driver_init() because main() calls a function with this name.
 *
 * You should probe for your hardware here, setup the interface
 * structure and register your interface.
 *
 * This function should return 0 on success and != 0 if initialization
 * fails.
 */
long driver_init (void)
{
//	char message[50];
	//static char eth_fname[128];

	long	ferr;
	short	fhandle;
	char	macbuf[13];

	c_conws("\r\n");
	c_conws("*************************************\n\r");
	c_conws("******   SV3 Ethernet driver   ******\n\r");
	c_conws("*************************************\n\r");
	//Bconin(2);

	//Check that the SV version is at least 301
	//Otherwise the FW isn't a SV3 with DDR DMA buffer support
	{
		uint32 sv_fw_version = SV_VERSION & 0x3FFUL;

		ksprintf (message, "SuperVidel FW version is %lu\n\r", sv_fw_version);
		c_conws  (message);

		if (sv_fw_version < 301 || sv_fw_version > 399)
		{
			c_conws( "This driver needs a SV3 with at least FW version 301!\n\r" );
			Bconin(2);
			return -1;		
		}
	}

	// Open sv3eth.inf to read the MAC address
	ferr = Fopen( "C:\\sv3eth.inf",0 );
	if ( ferr >= 0 )
	{
		fhandle = (short)(ferr & 0xffff);
		memset(macbuf, 0, 13);
		ferr = Fread(fhandle,12,macbuf);
		if(ferr < 0)
		{
			ksprintf (message, "Error reading C:\\sv3eth.inf\n\r");
			c_conws (message);
			Fclose(fhandle);
			return -1;
		}
		if(ferr < 12)
		{
			c_conws ("C:\\sv3eth.inf is less than 12 bytes long!\n\r");
			Fclose(fhandle);
			return -1;
		}
		Fclose(fhandle);

		//print what we read from sv3eth.inf
		c_conws("SV3ETH MAC is ");
		c_conws(macbuf);
		c_conws("\r\n");
	}
	else
	{
		c_conws("Could not open C:\\sv3eth.inf\n\r");
		c_conws("Using default MAC address 00:02:03:04:05:06\n\r");
		macbuf[0] = '0';
		macbuf[1] = '0';
		macbuf[2] = '0';
		macbuf[3] = '2';
		macbuf[4] = '0';
		macbuf[5] = '3';
		macbuf[6] = '0';
		macbuf[7] = '4';
		macbuf[8] = '0';
		macbuf[9] = '5';
		macbuf[10] = '0';
		macbuf[11] = '6';
	}

	//Bconin(2);

	macbuf[12] = 0;

	// Extract MAC address from macbuf
	if_sv3eth.hwlocal.adr.bytes[0] = (uchar)(ch2i(macbuf[0]) * 16 + ch2i(macbuf[1]));
	if_sv3eth.hwlocal.adr.bytes[1] = (uchar)(ch2i(macbuf[2]) * 16 + ch2i(macbuf[3]));
	if_sv3eth.hwlocal.adr.bytes[2] = (uchar)(ch2i(macbuf[4]) * 16 + ch2i(macbuf[5]));
	if_sv3eth.hwlocal.adr.bytes[3] = (uchar)(ch2i(macbuf[6]) * 16 + ch2i(macbuf[7]));
	if_sv3eth.hwlocal.adr.bytes[4] = (uchar)(ch2i(macbuf[8]) * 16 + ch2i(macbuf[9]));
	if_sv3eth.hwlocal.adr.bytes[5] = (uchar)(ch2i(macbuf[10]) * 16 + ch2i(macbuf[11]));



	//Create longword MAC address for RX use
	mac_addr[0] = (((unsigned long)(if_sv3eth.hwlocal.adr.bytes[0])) << 24) +
					  (((unsigned long)(if_sv3eth.hwlocal.adr.bytes[1])) << 16) +
					  (((unsigned long)(if_sv3eth.hwlocal.adr.bytes[2])) <<  8) +
					  (((unsigned long)(if_sv3eth.hwlocal.adr.bytes[3])));

	mac_addr[1] = (((unsigned long)(if_sv3eth.hwlocal.adr.bytes[4])) << 24) +
					  (((unsigned long)(if_sv3eth.hwlocal.adr.bytes[5])) << 16);


	//Init_BD();	
	Init_DMA_slot_info();
	

	/*********************************************
	 * Here comes functions not using the hardware
	 *********************************************
	 */

	/*
	 * Set interface name
	 */
	strcpy (if_sv3eth.name, "en");

	/*
	 * Set interface unit. if_getfreeunit("name") returns a yet
	 * unused unit number for the interface type "name".
	 */
	if_sv3eth.unit = if_getfreeunit ("en");

	/*
	 * Alays set to zero
	 */
	if_sv3eth.metric = 0;

	/*
	 * Initial interface flags, should be IFF_BROADCAST for
	 * Ethernet.
	 */
	if_sv3eth.flags = IFF_BROADCAST;

	/*
	 * Maximum transmission unit, should be >= 46 and <= 1500 for
	 * Ethernet
	 */
	if_sv3eth.mtu = 1500;

	/*
	 * Time in ms between calls to (*if_sv3eth.timeout) ();
	 */
	if_sv3eth.timer = 1000;
	
	/*
	 * Interface hardware type
	 */
	if_sv3eth.hwtype = HWTYPE_ETH;

	/*
	 * Hardware address length, 6 bytes for Ethernet
	 */
	if_sv3eth.hwlocal.len =
	if_sv3eth.hwbrcst.len = ETH_ALEN;
	
	/*
	 * Set interface broadcast address. For real ethernet
	 * drivers you must get them from the hardware of course!
	 */
	memcpy (if_sv3eth.hwbrcst.adr.bytes, "\377\377\377\377\377\377", ETH_ALEN);
	
	/*
	 * Set length of send and receive queue. IF_MAXQ is a good value.
	 */
	if_sv3eth.rcv.maxqlen = IF_MAXQ;
	if_sv3eth.snd.maxqlen = IF_MAXQ;

	/*
	 * Setup pointers to service functions
	 */
	if_sv3eth.open = sv3eth_open;
	if_sv3eth.close = sv3eth_close;
	if_sv3eth.output = sv3eth_output;
	if_sv3eth.ioctl = sv3eth_ioctl;

	/*
	 * Optional timer function that is called every 200ms.
	 */
	if_sv3eth.timeout = sv3eth_timeout;
	
	/*
	 * Here you could attach some more data your driver may need
	 */
	if_sv3eth.data = 0UL;
	
	/*
	 * Number of packets the hardware can receive in fast succession,
	 * 0 means unlimited.
	 */
	if_sv3eth.maxpackets = PS_DMA_BUFFER_PKTS;
	
	/*
	 * Register the interface.
	 */
	if_register (&if_sv3eth);
	
	/*
	 * And say we are alive...
	 */
	ksprintf (message, "sv3eth driver v0.9 (en%d)\n\r", if_sv3eth.unit);
	c_conws (message);

	//print out all function pointers
	if (!has_printed_functions)
	{
		ksprintf(message, "sv3eth_open 0x%08lx\r\n", (uint32)(sv3eth_open));
		c_conws(message);
		ksprintf(message, "sv3eth_close 0x%08lx\r\n", (uint32)(sv3eth_close));
		c_conws(message);
		ksprintf(message, "sv3eth_output 0x%08lx\r\n", (uint32)(sv3eth_output));
		c_conws(message);
		ksprintf(message, "sv3eth_ioctl 0x%08lx\r\n", (uint32)(sv3eth_ioctl));
		c_conws(message);
		ksprintf(message, "sv3eth_config 0x%08lx\r\n", (uint32)sv3eth_config);
		c_conws(message);
		ksprintf(message, "send_packet 0x%08lx\r\n", (uint32)(send_packet));
		c_conws(message);
		ksprintf(message, "sv3eth_service 0x%08lx\r\n", (uint32)sv3eth_service);
		c_conws(message);

		has_printed_functions = 1UL;
	}

	#if (0)
	//Perform speed/reliability test of the length mailbox by setting the sv3mailboxhandler
	//program on the Linux side in loopback mode.
	{
		uint32_t	stat_reg;
		uint32_t	rxdata;
		uint32_t	b;
		uint32_t	i;
		uint32_t	w;
		uint32_t	k;
		uint32_t	errors = 0UL;
		uint32_t 	loopsize = 50000UL;
		uint32_t 	burstsize = 1UL;

		stat_reg = mbox0.stat_ctrl;

		//First empty the length mailbox of packets going to us
		c_conws("driver_init: mbox tst: Will empty the length mailbox RX-FIFO\n\r");
		while ( (mbox0.stat_ctrl & MBOX_STAT_RX_LFIFO_EMPTY) == 0 )
		{
			rxdata = mbox0.len_fifo;
		}
		c_conws("driver_init: mbox tst: Length mailbox RX-FIFO is now empty\n\r");

		//Then set the Linux loopback mode by writing 0xFFFFFFFF
		while (mbox0.stat_ctrl & MBOX_STAT_TX_LFIFO_FULL);
		mbox0.len_fifo = 0xFFFFFFFFUL;
		c_conws("driver_init: mbox tst: Has written 0xFFFFFFFF to length mailbox to set loopback mode\n\r");

		//Then write a unique test value 0xB16B00B5
		while (mbox0.stat_ctrl & MBOX_STAT_TX_LFIFO_FULL);
		mbox0.len_fifo = 0xB16B00B5UL;
		c_conws("driver_init: mbox tst: Has written 0xB16B00B5 to length mailbox\n\r");

		//Then read the len mailbox until we get a reply back from Linux side
		while (mbox0.stat_ctrl & MBOX_STAT_RX_LFIFO_EMPTY);
		rxdata = mbox0.len_fifo;
		if (rxdata == 0xB16B00B5UL)
		{
			c_conws("driver_init: mbox tst: Has received 0xB16B00B5 from length mailbox\n\r");
		}
		else
		{
			ksprintf(message, "driver_init: mbox tst: Has received wrong data from length mailbox. Expected 0xB16B00B5 got 0x%08lx\n\r", rxdata);
			c_conws(message);
		}

		//Do many writes and reads here and measure the return ratio
		for ( burstsize=1; burstsize < 64; burstsize *= 2 )
		{
			ksprintf(message, "driver_init: mbox test: Doing burst size %lu\r\n", burstsize );
			c_conws(message);

			errors = 0;

			for ( i=0; i < loopsize; i += burstsize )
			{
				//Write a burst of burstsize longwords
				k = i;
				for ( b=0; b < burstsize; b++ )
				{
					write_len_fifo_ctrl(k++);
				}

				//Read burstsize number of longwords
				k = i;
				for ( b=0; b < burstsize; b++ )
				{
					//Wait for reply for a limited number of read cycles
					w = 0;
					do
					{
						if (!(mbox0.stat_ctrl & MBOX_STAT_RX_LFIFO_EMPTY))
						{
							//Not empty RX FIFO, exit loop
							break;
						}
						else if (w == 10000)
						{
							ksprintf(message, "driver_init: mbox test: Got no reply on 0x%08lx\r\n", k);
							c_conws(message);
							errors++;
							break;
						}
						w++;
					} while (1);

					//If we got reply within time
					if ( w < 10000 )
					{
						rxdata = mbox0.len_fifo;

						if (rxdata != k )
						{
							ksprintf(message, "driver_init: mbox tst: wrong reply on 0x%08lx. Got 0x%08lx\r\n", k, rxdata);
							c_conws(message);
							errors++;
						}
						else
						{
							//ksprintf(message, "driver_init: mbox tst: correct reply 0x%08lx\r\n", rxdata);
							//c_conws(message);
						}
					}
					k++;
				}
			}

			ksprintf(message, "driver_init: mbox tst: Got %lu errors of %lu tests.\r\n", errors, loopsize );
			c_conws(message);
		}
		//Turn off loopback mode by writing 0xFFFFFFFE
		while (mbox0.stat_ctrl & MBOX_STAT_TX_LFIFO_FULL);
		mbox0.len_fifo = 0xFFFFFFFEUL;
		c_conws("driver_init: mbox tst: Has written 0xFFFFFFFE to length mailbox to clear loopback mode\n\r");

		rxdata = rxdata;
		stat_reg = stat_reg;

		Bconin(2);
	}
	#endif

	//Make sure the RX interrupt is off before we drain the mailbox and
	//install our vector. A CT60 reset clears this bit in the FPGA (the
	//IE bits of all four mailboxes), but don't rely on that. It is turned
	//on again in sv3eth_open(). Mask CPU interrupts while doing it, so an
	//IRQ6 that is already on its way is not withdrawn in the middle of an
	//IACK cycle.
	{
		ushort sr = spl7();
		mbox0.stat_ctrl &= ~MBOX_STAT_RX_LFIFO_IE;
		spl(sr);
	}

	//Clear any waiting RX packets in the mailbox
	//by sending a TX ACK reply for each such packet
	sv3eth_flush_mailbox("driver_init", &init_flush_rx, &init_flush_tx);


	// Install interrupt handler	
	sv3eth_install_int();

	c_conws("sv3eth: Init succeeded\n\r");
	return 0;
}


static void sv3eth_install_int (void)
{
//	uint32	old_sp;
	
//	old_sp = Super(0L);

	old_i6_int = (uint32_t)Setexc ((uint16_t)(((uint32_t)SV3_MBOX0_INT_VECTOR) >> 2), (uint32_t) SV3_mbox0_isr);

//	Super(old_sp);

	//c_conws("Installed ISR\r\n");	
}




/*
 * Interrupt routine
 */

volatile uint32_t isr_flag = 0;
volatile uint16_t isr_temp;

void __attribute__ ((interrupt)) SV3_mbox0_isr(void)
{
	uint32_t	stat_reg;

	stat_reg = mbox0.stat_ctrl;

	//Turn off mailbox interrupt source, so I6n goes inactive	
	mbox0.stat_ctrl = 0x00;
	isr_flag = 1;

	//Dummy read from motherboard to satisfy ABE-chip
	//isr_temp = *((volatile uint16*)0xffff8240);

	//c_conws( "Sv3eth ISR\r\n" );

	sv3eth_service( &if_sv3eth, stat_reg );	//do the work

	//Enable mailbox interrupt again
	mbox0.stat_ctrl |= MBOX_STAT_RX_LFIFO_IE;
}



//Returns the slot nr and length of an RX packet or -1 if there is none
int32 Check_Rx_Buffers()
{
	int32_t  retval = -1L;
	//uint32_t tmp1;
	//uint32_t tmp2;

	while ( (mbox0.stat_ctrl & MBOX_STAT_RX_LFIFO_EMPTY) == 0 )
	{
		//Length mailbox is not empty

		//Read length mailbox to get info about next packet
		//Bottom word is number of bytes
		//Top word is RX slot index to read packet from
		uint32_t len_val = mbox0.len_fifo;
		uint32_t len     = len_val & 0xFFFF;
		uint32_t raw_pos = len_val >> 16;
		uint32_t pos     = 0;
		//printf( "Mailbox0 pos is %u, length is %u\r\n", pos, len );
	
		// Defensive masking: raw_pos comes straight from the hardware
		// mailbox and is a full 16-bit value, but we only have
		// PS_DMA_BUFFER_PKTS slots. Count it if it was ever out of range,
		// then mask it so we never index tx_dma_pkt_flags[] or
		// rxbuffers[] out of bounds.
		if ( raw_pos >= PS_DMA_BUFFER_PKTS )
		{
			bad_slot_count++;
		}
		pos = raw_pos & (PS_DMA_BUFFER_PKTS - 1);

		if ( len == 0xFFFF )
		{
			//This is a special encoding that tells us to clear the
			//TX flag for the given slot
			tx_dma_pkt_flags[pos] = 0;
		}
		else if ( len > SV3ETH_MAX_RX_LEN )		//1518 bytes
		{
			//ksprintf( message, "Too much data %u bytes in rx packet\r\n", len );
			//c_conws( message );
			oversized_rx_count++;

			//Send ACK to the sender that we have handled this packet
			if ( (mbox0.stat_ctrl & MBOX_STAT_TX_LFIFO_FULL) == 0 )
			{
				//Len FIFO not full, send ACK msg
				mbox0.len_fifo = (pos << 16) | 0xFFFFUL;
			}
			else
			{
				//Len FIFO is full
				tx_fifo_full_count++;
				//ksprintf( message, "Check_Rx_Buffers: Len FIFO full\r\n" );
				//c_conws( message );
			}
			continue;
		}
		else
		{
			//Found a valid RX packet. Re-encode with the masked pos,
			//so callers (sv3eth_service) never see an out-of-range slot.
			retval = (int32_t)((pos << 16) | len);
			break;
		}

	}

	return retval;
}




/* Service function called by the interrupt handler
 * In here we do receive only and no transmission of queued packets
 * because no TX packets are queued in software.
 */
static void sv3eth_service (struct netif * nif, uint32 int_src)
{
//	uchar	intstat;
//	uchar		oldpnr;
//	uchar	failpnr;
//	ushort	fifo_reg;
//	ushort	status, bytecount, longcnt;
	short	type;
//	char	packetnr;
//	char	message[80];
//	long	tmp, *dpnt;
	BUF		*b;
//	volatile uint32	j;

//	long		timeval;

	
	// Check for received packet infos in the mailbox length fifo
	if ( (mbox0.stat_ctrl & MBOX_STAT_RX_LFIFO_EMPTY) == 0 )
	{
		//c_conws( "RX frame!\r\n" );

		int32_t slot_len = Check_Rx_Buffers();
		while (slot_len != -1)
		{
			volatile uint8_t	*src;
			//volatile uint32_t	*origsrc;
			//uint32_t	*dest;
			uint32_t	length;
			uint32_t	alloc_size;
			//uint32_t	len_longs;
			uint32_t	slot;

			//ksprintf (message, "R%02li 0x%08lx\n\r", slot, eth_rx_bd[slot].len_ctrl);
			//c_conws(message);

			slot      = slot_len >> 16;		//slot nr in upper word
			length    = slot_len & 0xFFFF;	//length in lower word
			//len_longs = (length + 3UL) >> 2;	//round up to whole longwords

			src = (volatile uint8_t*)ps_dma_bufs->rxbuffers[slot].buf;
			//origsrc = src;

			// Filter all packets that are not IPv4 or ARP.
			// The type field is located at offset 12 in the ethernet header
			// Also filter all packets with us as src
			{
				uint8_t *eth_head = (uint8_t*)src;
				uint16_t raw_type = ((uint16_t)eth_head[12] << 8) | eth_head[13];
				uint16_t src_mac[3];

				// Filter out IPV6, UNKNOWN packets immediately
				// If it's not IPv4 (0x0800) or ARP (0x0806), drop it before BPF_input or eth_remove_hdr
				if (raw_type != 0x0800 && raw_type != 0x0806)
				{
					goto ACK_PACKET;
				}

				//Filter looped packets with our own MAC as src
				src_mac[0] = ((uint16_t*)src)[3];
				src_mac[1] = ((uint16_t*)src)[4];
				src_mac[2] = ((uint16_t*)src)[5];

				if ( (src_mac[0] == (uint16_t)(mac_addr[0] >> 16)) &&
					 (src_mac[1] == (uint16_t)(mac_addr[0] & 0xFFFF)) &&
					 (src_mac[2] == (uint16_t)(mac_addr[1] >> 16)) )
				{
					//ksprintf( message, "Sv3eth: removed looped packet, src mac %04x %04x %04x\r\n", src_mac[0], src_mac[1], src_mac[2] );
					//c_conws(message);
					goto ACK_PACKET;
				}
			}			

			//Allocate packet buffer from mintnet buffers
			//buf = buf_alloc (space, reserve, mode);
 			//where `space' is the size of the userspace of the BUF you need, `reserve'
 			//is used to set BUF.dstart = BUF.dend = BUF.data + `reserve' and
			//mode is one of
 			//		BUF_NORMAL for calls from kernel space,
 			//		BUF_ATOMIC for calls from interrupt handlers.			

			//Allocate size with 128 bytes margin, and round length up to whole 4 bytes before
			alloc_size = ((length + 3UL) & ~3UL) + 128UL;
			b = buf_alloc ( alloc_size, 64UL, BUF_ATOMIC );

			if ( ((uint32_t)b) == 0UL )
			{
				// Allocation failed
				rx_buf_alloc_fail_count++;
				nif->in_errors++;
				//ksprintf (message, "buf_alloc RX failed, %lu \n\r", 1518UL + 128UL);
				//c_conws(message);
			}
			else
			{
				// 1. Set dstart to point where the Ethernet header should begin
				b->dstart -= 14;
				
				// 2. Set dend to exactly dstart + length so the size is 100% correct
				b->dend = b->dstart + length;

				// 3. STRAIGHT LONGWORD COPY
				// Copy 32 bits at a time to eliminate alignment bugs while keeping decent speed.
				{
					uint32_t *src_lwords  = (uint32_t*)ps_dma_bufs->rxbuffers[slot].buf;
					uint32_t *dest_lwords = (uint32_t*)b->dstart;
					uint32_t lword_count  = (length + 3UL) >> 2; // Round up to nearest whole longword
					uint32_t i;

					for (i = 0; i < lword_count; i++)
					{
						*dest_lwords++ = *src_lwords++;
					}
				}

				// 4. Pass packet to BPF if active
				if (nif->bpf)
					bpf_input (nif, b);
	
				//Print start of frame for debugging
				//{
				//	uint8_t *d = (uint8_t*)b->dstart;
				//	ksprintf(message, "SV3ETH MAC %08lx%04x RX DATA: DST %02x %02x %02x %02x %02x %02x | SRC %02x %02x %02x %02x %02x %02x | TYPE: %02x %02x\r\n",
				//			mac_addr[0],							//First 8 digits of our own MAC
				//			(uint16_t)(mac_addr[1] >> 16),			//Last 4 digits of our own MAC
				//			d[0], d[1], d[2], d[3], d[4], d[5],    // Destination MAC
				//			d[6], d[7], d[8], d[9], d[10], d[11],  // Source MAC
				//			d[12], d[13]); 
				//	c_conws(message);
				//}

				// 5. Process hardware header
				type = eth_remove_hdr(b);
				
				// 6. Enqueue and pass packet to MiNT-Net
				if ( !if_input(nif, b, 0UL, type) )
				{
					nif->in_packets++;
				}
				else
				{
					rx_if_input_fail_count++;
					nif->in_errors++;
					//c_conws("Input packet failed when receiving!\n\r");
				}
			}
				
			ACK_PACKET :

			//Send ACK to the (petalinux side) sender that we have handled this packet
			if ( (mbox0.stat_ctrl & MBOX_STAT_TX_LFIFO_FULL) == 0 )
			{
				//Len FIFO not full, send ACK msg
				mbox0.len_fifo = slot_len | 0xFFFFUL;
			}
			else
			{
				//Len FIFO is full
				rx_ack_fifo_full_count++;
				//ksprintf( message, "sv3eth_service: Len FIFO full. Cannot ACK RX pkt\r\n" );
				//c_conws( message );
			}
			
			slot_len = Check_Rx_Buffers();		
		}
	}
	else
	{
		//c_conws("Sv3eth_service: Len mailbox empty, nothing to do!\n\r");	
	}
	
	/*
	// Check for transmitted packets
	if ((int_src & (ETH_INT_TXB || ETH_INT_TXE)) != 0)	// Transmit complete or error
	{
		if(int_src & ETH_INT_TXE)				// TX eror set => failed!
		{
			nif->out_errors++;
		}

		//A Transmit Error means that the slot is free to be used.
		//TODO: Check first that the slot that is in turn to be used is free.
		//Then we dequeue a packet and send it, if one exists. Then we toggle the slot index variable.
		//Finally we check again if the new slot is free. If so we dequeue a packet again.
		if (((uint32)(eth_tx_bd[slot_index].len_ctrl & ETH_TX_BD_READY)) != 0UL)
			return;

		b = if_dequeue(&nif->snd);				//fetch a previously enqueued packet
		if( ((uint32)b) != 0UL)
		{
			//Possible errors returned are -1 and -2, meaning "no free slot" and "too large packet" respectively.
			//-1 isn't possible because of the check above, and -2 isn't possible either, because
			//we don't enqueue packets in sv3eth_output() that are too large.
			send_packet(nif, b, BUF_ATOMIC, slot_index, slot_index == (ETH_PKT_BUFFS-1) );
			slot_index = (slot_index + 1) & (ETH_PKT_BUFFS-1);

			//in_queue--;

			//ksprintf (message, "Dequeued, %u left\n\r", nif->snd.qlen);
			//c_conws(message);
		}
	}
	*/
}



/* Convert one ASCII char to a hex nibble */
short ch2i(char c)
{
	if(c >= '0' && c <= '9')
		return (short)(c - '0');
	if(c >= 'A' && c <= 'F')
		return (short)(c - 'A' + 10);
	if(c >= 'a' && c <= 'f')
		return (short)(c - 'a' + 10);
	return 0;
}



// Convert an unsigned long to ASCII
void hex2ascii(ulong l, uchar* c)
{
	short	i;
	uchar	t;
	
	for(i=7; i!=0; i--)
	{
		t = (uchar)(l & 0xf);				// Keep only lower nibble
		if(t < 0xA)
			c[i] = t + '0';					// Output '0' to '9'
		else
			c[i] = t + 'A' - 0xA;			// Output 'A' to 'F'
		c[i] = '0';
		l = l >> 4;
	}
}


