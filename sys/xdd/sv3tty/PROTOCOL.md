# sv3tty – serial channel between the 68060 and PetaLinux on SuperVidel 3

Status: draft, protocol version 1. Nothing implemented yet.

sv3tty gives the Falcon (TOS or FreeMiNT) virtual serial ports (16 by
default, up to 255) that end up as tty devices (`/dev/ttySV0`, `/dev/ttySV1`,
...) on the PetaLinux side
of the SuperVidel 3. No network, no SSH, and no Ethernet traffic is involved.
The data goes through **mailbox channel 1**, which is not used by `sv3eth`
(that uses channel 0).

Three drivers implement this document:

| Side | Driver | Notes |
|---|---|---|
| Linux (ARM) | `sv3tty.ko` | tty driver, separate module from `sv3mboxdriver` |
| TOS | `SV3TTY.PRG` | TSR in AUTO, extends `Bconmap`, publishes ports in the RSVF cookie |
| FreeMiNT | `sv3tty.xdd` | tty device driver modelled on `sys/xdd/mfp` and `sys/xdd/scc` |

The protocol code (framing, credits, resync) is shared between the TOS and
MiNT drivers in a common C file.


## 1. Hardware

### Mailbox channels

The FPGA has four **mailbox channels**, numbered 0..3. Each channel is a pair
of FIFOs, and each FIFO is bidirectional (one queue per direction):

- a **length FIFO** (also called command or header FIFO), depth 256 words,
  which carries short control words, and
- a **data FIFO**, depth 1024 words, which carries payload.

| Channel | Length FIFO (ARM) | Data FIFO (ARM) | 68060 base | Used by |
|---|---|---|---|---|
| 0 | `C01` | `C00` | `0x80013000` | `sv3eth` / `sv3mboxdriver` |
| 1 | `C11` | `C10` | `0x80013040` | **sv3tty** |
| 2 | `C21` | `C20` | `0x80013080` | free |
| 3 | `C31` | `C30` | `0x800130C0` | free |

On the ARM side every FIFO is a separate Xilinx AXI mailbox IP
(`xlnx,mailbox-2.1`), so a channel is two devices. Only the length FIFOs
(`Cx1`) have their interrupt routed to the ARM; the data FIFOs (`Cx0`) have
none. The receiver therefore always learns about a message through the length
FIFO and then fetches the payload from the data FIFO.

On the 68060 side the same channel appears as one register block with both
FIFOs in it (`len_fifo` and `data_fifo`), see below. In `sv_regs.h` these
blocks are called `mbox0` .. `mbox3`; `mboxN` is channel N.

In the rest of this document, "length FIFO" and "data FIFO" always mean the
FIFOs of channel 1.

### 68060 side

Channel 1, `MBOX_BASE1` = `0x80013040` (see `sv_regs.h`):

| Offset | Register | Use |
|---|---|---|
| `0x00` | `stat_ctrl` | status bits below |
| `0x10` | `data_fifo` | payload words |
| `0x20` | `len_fifo` | header words |

| Bit | Name |
|---|---|
| `0x0001` | `MBOX_STAT_RX_DFIFO_EMPTY` |
| `0x0002` | `MBOX_STAT_TX_DFIFO_FULL` |
| `0x0004` | `MBOX_STAT_RX_LFIFO_EMPTY` |
| `0x0008` | `MBOX_STAT_TX_LFIFO_FULL` |
| `0x0010` | `MBOX_STAT_RX_LFIFO_IE` – **must stay 0 for channel 1** |

Behaviour of the register block (`ct60_mailboxes.vhd`) that the drivers must
respect:

- Writing `data_fifo` or `len_fifo` always "succeeds". The CPU must check
  `TX_*FIFO_FULL` first, otherwise the word is lost.
- Reading `data_fifo` or `len_fifo` when the FIFO is empty returns a stale
  value. Always check `RX_*FIFO_EMPTY` first.
- In `stat_ctrl`, only bit 4 (`RX_LFIFO_IE`) is writable.

On the 68060 side, only channel 0 has its interrupt routed to the IACK logic
in the FPGA (`int_interrupt4` of `zturn_bus_interface`, vector `0xC9` = 201).
Adding channel 1 would be a small change: drive `int_array(1)` in
`ct60_mailboxes.vhd` (the loop is already there, commented out) and connect
`mb_int_array(1)` to `int_interrupt5` in `zt_main.tve`, which gives vector
`0xCA` = 202 (address `0x328`). The 68060 drivers therefore **poll** channel 1 at 200 Hz (Timer C under TOS, a
kernel timeout under MiNT). Each tick drains everything that is waiting. This
gives a theoretical maximum of roughly 1024 words per 5 ms, which is far more
than a terminal needs. Interrupts can be added later if the FPGA gets an
interrupt line for channel 1; the protocol does not change.

### ARM side

Channel 1 (confirmed in the Vivado block design 2026-10-08):

| Node | Address | Use |
|---|---|---|
| `mailbox32_axis_axi4_C10` | `0x43820000` | data FIFO, depth 1024, no interrupt |
| `mailbox32_axis_axi4_C11` | `0x43830000` | length FIFO, depth 256, interrupt 1 |

In `system-user.dtsi`, C11 is currently `generic-uio` and has to become
`compatible = "vendor,sv3tty"` (with C10 as a second reg region or a
phandle).

Xilinx mailbox register map (both nodes): `WRDATA 0x00`, `RDDATA 0x08`,
`STATUS 0x10` (bit 0 empty, bit 1 full, bit 2 STA, bit 3 RTA), `ERROR 0x14`,
`SIT 0x18`, `RIT 0x1C`, `IS 0x20`, `IE 0x24`, `IP 0x28`, `CTRL 0x2C`
(bit 0 reset send FIFO, bit 1 reset receive FIFO).

### Reset behaviour

The AXI4-Stream register slices between the mailboxes and the 68060 register
interface are reset by the Falcon reset button, the Xilinx mailboxes are not.
Words can therefore be lost or left behind in either direction when the Falcon
is reset or crashes. Section 5 (resync) handles this.


## 2. Message format

A message is one **header word** in the length FIFO, optionally followed by
**payload words** in the data FIFO.

```
header (len_fifo):  31      24 23      16 15                0
                   +----------+----------+-------------------+
                   |   type   |   port   |   arg / length    |
                   +----------+----------+-------------------+
```

- `type` – message type, see section 3.
- `port` – port number 0..254. `0xFF` means "the whole link" (used by
  resync). The protocol allows 255 ports. The number of ports a driver
  offers is set at load time (an inf file under TOS, a module parameter on
  Linux), default 16.
- `arg` – a 16-bit argument. For types with payload it is the payload length
  in **bytes**.

**Types with payload** have bit 7 of `type` set (`0x80`..`0xEF`). They are
followed by `ceil(length / 4)` words in the data FIFO. A receiver that does
not know a type with bit 7 set skips that many data words. Unknown types
without payload are ignored. This lets later versions add types without
breaking older drivers.

### Payload packing

Bytes are packed most significant byte first:

```
word = (b0 << 24) | (b1 << 16) | (b2 << 8) | b3
```

On the 68060 this is plain memory order (`move.l` from a byte buffer). On the
ARM side, use `get_unaligned_be32()` / `put_unaligned_be32()`. Unused bytes
in the last word are 0 and are ignored.

### Ordering rules

1. The sender writes **all payload words first and the header last**.
2. The two FIFOs cross clock domains independently, so the receiver can still
   see the header before the payload. After reading a header with payload, the
   receiver waits for each data word (`RX_DFIFO_EMPTY` = 0 / `STATUS.empty` = 0)
   with a timeout of about 1 ms. A timeout is a link error and starts a resync
   (section 5).
3. A sender never interleaves two messages. On the 68060 side, the timer
   routine and `Bconout`/`write` share the channel, so a message is written
   with interrupts masked (`spl7()` / SR `0x2700`). Under no circumstances may
   `MBOX_STAT_RX_LFIFO_IE` be changed with interrupts enabled (see the IACK
   problem in `sv3eth`).


## 3. Message types

| Type | Name | Direction | Port | Arg | Payload |
|---|---|---|---|---|---|
| `0x01` | `CREDIT` | both | port | bytes freed | – |
| `0x02` | `OPEN` | 68060 → Linux | port | 0 | – |
| `0x03` | `CLOSE` | 68060 → Linux | port | 0 | – |
| `0x04` | `CARRIER` | Linux → 68060 | port | 1 = Linux side open, 0 = closed | – |
| `0x05` | `WINSZ` | 68060 → Linux | port | `(rows << 8) \| cols` | – |
| `0x06` | `BREAK` | both | port | duration in ms (0 = default) | – |
| `0x80` | `DATA` | both | port | length 1..256 | bytes |
| `0xF0` | `RESYNC` | both | `0xFF` | protocol version | marker word |
| `0xF1` | `RESYNC_ACK` | both | `0xFF` | protocol version | marker word |

### DATA

Carries 1..256 bytes for one port. The sender may only send as many bytes as it
has credit for on that port (section 4). Combining bytes into larger messages
is recommended. The 68060 side may for example collect `Bconout` bytes and send
them on the next timer tick, or as soon as 64 bytes are waiting.

### OPEN / CLOSE

They correspond to DTR/DCD on a modem line. `OPEN` tells Linux that a program on
the Falcon is using the port. The Linux tty then reports carrier, and a getty
blocked in `open()` continues and prints its login prompt. `CLOSE`
hangs up: Linux calls `tty_port_tty_hangup()`, the shell gets `SIGHUP`, and
init starts a new getty that waits for the next `OPEN`.

`agetty` (util-linux, used by PetaLinux) opens the tty without waiting for
carrier, so it prints its prompt while the port is still closed on the Falcon,
and that output is discarded. Therefore Linux also hangs up the tty on `OPEN`,
unless `CLOCAL` is set on it. init then starts a fresh getty, whose prompt
reaches the Falcon. Programs that set `CLOCAL` (custom daemons) are not
affected.

- MiNT: `OPEN` on the first `open()` of the device, `CLOSE` on the last `close()`.
- TOS: BIOS has no open/close. `OPEN` is sent on the first access to the
  device (`Bconout`, `Bconin`, `Bconstat`, `Bcostat`). `CLOSE` is sent when DTR
  is dropped through `Rsconf` or the HSMODEM ioctl. A small tool can do this from the
  command line.

### CARRIER

Sent by Linux in answer to `OPEN` and whenever it changes. It tells the 68060
side whether anything on Linux has the port open (for example, no getty is configured for it). Terminal programs can
show it as DCD.

### WINSZ

Sent after `OPEN` and whenever the terminal size changes. Linux applies it with
`tty_do_resize()`, so that `stty size`, `top`, `less` and so on fit the Falcon screen.

### BREAK

Linux → 68060: result of `tcsendbreak()`, may be ignored. 68060 → Linux:
inserted as `TTY_BREAK` into the flip buffer (with `BRKINT` this gives
`SIGINT`).


## 4. Flow control (credits)

All ports share the same two FIFOs. If one port's reader stops reading, it
must never block the FIFO for the other ports. Therefore:

- **Invariant:** a receiver always drains the FIFOs completely and moves
  `DATA` payloads into a per-port receive buffer. It never stops reading the
  FIFOs because a buffer is full.
- Credits ensure that the per-port receive buffer always has room. A
  sender may only send `DATA` for a port as long as it holds credit for it.
  Credit is counted in bytes. Each `DATA` message consumes its length.
- A closed port has no credit in either direction. Credit is granted
  **when a port is opened**, not at resync. With up to 255 ports, sending an
  initial `CREDIT` for every port at resync would nearly fill the 256-entry
  length FIFO, and receive buffers would have to exist for ports nobody uses.
  The receive buffers are therefore allocated at open and freed at close.
- When a port is opened (recommended window: 1024 bytes per direction):
  1. The 68060 side allocates its receive buffer, sends `OPEN`, and directly
     after it `CREDIT` with its window. It then sends `WINSZ`.
  2. Linux, on receiving `OPEN`, sets its send credit for the port to 0 and
     drops any pending output from an earlier session. It then answers with
     `CREDIT` (its window) and `CARRIER`.
  3. The 68060 side **discards all `DATA` for the port until the first
     `CREDIT` from Linux after its `OPEN` has arrived**. Messages in one
     direction are ordered, so anything Linux sent before it saw the `OPEN`
     (left over from an earlier session) arrives before that `CREDIT`.
     Likewise, the 68060 side sends no `DATA` before it has that `CREDIT`.
- When a port is closed, the 68060 side sends `CLOSE`, frees its receive
  buffer and sets both credits for the port to 0. Linux does the same on
  receiving `CLOSE`. `DATA` that arrives for a closed port is discarded and
  no credit is returned for it.
- `CARRIER` does not affect credit. Linux accepts data on a port even if no
  program has the tty open; it is then discarded and the credit returned at
  once, so the 68060 side never stalls.
- When the reader consumes data, the receiver returns credit. To limit
  traffic, it sends `CREDIT` when at least a quarter of the window has been
  freed, or when the buffer has become empty.
This replaces RTS/CTS. On Linux, the tty layer calls `throttle()` when its
input buffer is almost full. The driver then stops returning credit, and
`unthrottle()` sends what has accumulated.

A full data or header FIFO is handled by waiting, not by dropping:
- 68060 side: the timer routine never waits. It sends what fits and leaves the rest
  for the next tick. `Bconout` may wait (TOS semantics).
- Linux side: no busy-waiting in interrupt context. Pending output is
  moved to a work item or timer.


## 5. Resync

Purpose: get both sides into a known state after a Falcon reset or crash,
loading or unloading the Linux module, or a link error (data timeout, bad
header).

The marker word is `0xA55AC33C`. It is not printable ASCII, so it is unlikely
(but not impossible) to appear in left-over terminal data.

Each side has a link state, **WAIT** or **UP**. A side starts in WAIT.

- **Starting a resync:** write the marker word to the data FIFO, then header
  `0xF0FF0001` (`RESYNC`, port `0xFF`, version 1), and go to WAIT.
- **In WAIT**, all headers except `RESYNC` and `RESYNC_ACK` are discarded,
  **without** reading their payload. The data FIFO contents are dealt with by
  the marker search below. This way, a payload word lost at a Falcon reset
  cannot shift the framing.
- **On a `RESYNC` header (in any state):** discard data words up to and
  including the marker word, reset all port state, write the marker word and
  header `0xF1FF0001` (`RESYNC_ACK`), and go to UP.
- **On a `RESYNC_ACK` header:** discard data words up to and including the
  marker word. If in WAIT, reset all port state and go to UP. If already UP,
  do nothing more.

"Reset all port state" means: all ports are closed from the protocol's point
of view, credits are 0, and send buffers are cleared. On Linux, every port that
the 68060 side had open gets a hangup. No `CREDIT` is sent as part of a resync.
The 68060 side then reopens every port that is still open locally with the
normal open sequence (`OPEN`, `CREDIT`, `WINSZ`, see section 4), and Linux
answers each with `CREDIT` and `CARRIER`.

Who starts a resync:

- **68060 driver:** at startup, and after a link error (data timeout, bad
  header). If no `RESYNC_ACK` arrives within 1 s (Linux not running or the
  module not loaded), it repeats every 2 s.
- **Linux driver:** once when the module is loaded (in case the 68060 driver
  is already running), and after a link error. It does **not** repeat. If the
  68060 driver is not running, the 68060 driver will start a resync itself
  when it is started.

Example: the Linux module is loaded while the Falcon is off. Its `RESYNC`
stays in the FIFO. When the 68060 driver starts, it sends its own `RESYNC`
and then finds the old one from Linux. It answers it with `RESYNC_ACK` and goes
to UP. Linux answers the 68060 `RESYNC` with `RESYNC_ACK` and goes to UP. Each
side then receives a `RESYNC_ACK` while already UP and ignores it. Because
messages in one direction are ordered, an `OPEN` that the 68060 side sends
after its resync always reaches Linux after the resets.

If the marker word is not found within about 10 ms, the side logs an error
and continues anyway.

On the ARM side the receive FIFOs can also be cleared through `CTRL`, but this
does not remove words already in the CT60-domain register slices. The marker
word is needed anyway.


## 6. Linux driver (`sv3tty.ko`) – how the protocol maps to the tty layer

| tty layer | sv3tty |
|---|---|
| `tty_operations.write()` | copy into the per-port send kfifo, send `DATA` as far as credit allows |
| `write_room()` | free space in the send kfifo |
| `chars_in_buffer()` | bytes in the send kfifo |
| `throttle()` / `unthrottle()` | hold / release `CREDIT` |
| `tty_port_operations.carrier_raised()` | 68060 side has sent `OPEN` |
| `dtr_rts()` | ignored |
| `set_termios()` | baud rate etc. ignored (no physical line), `CLOCAL` respected |
| `break_ctl()` | `BREAK` |
| received `DATA` | `tty_insert_flip_string()` + `tty_flip_buffer_push()` |
| received `CLOSE` | `tty_port_tty_hangup()` |
| received `WINSZ` | `tty_do_resize()` |

Echo, line editing, `^C` → `SIGINT` and similar are handled by the Linux line
discipline (`N_TTY`). The Falcon side only transports raw bytes.

### Port usage (configuration, not protocol)

Which program runs on each port is decided in `/etc/inittab` (or
systemd units) on PetaLinux. Proposal:

- `ttySV0`: `getty` with a normal login, like the Z-turn USB console.
  `TERM=vt52`.
- One or more ports with autologin as an unprivileged user (no root),
  for simple Falcon programs that read files like
  `/sys/class/thermal/thermal_zone0/temp` or run `uptime`.
  Such a program should switch off echo (`stty -echo raw`) or, later, use a small
  command server on a dedicated port that answers one command with framed output.
- Remaining ports: login, or unused.


## 7. Open items

- TOS: which `Bconmap` device numbers to use, and the names in the RSVF cookie
  (proposal: `SV3TTY0` .. `SV3TTY15`).
- MiNT: device names (proposal: `u:\dev\sv3tty0`, ...), and whether the TOS
  TSR should detect MiNT and stay inactive. MiNT only lets drivers install
  BIOS devices 0..15 (`BDEVMAP_MAX` in `sys/bios.c`), so under MiNT at most
  ports 9..15 can also be offered as BIOS devices to old TOS programs.
- 68060 interrupt for channel 1 in the FPGA (optional, later).
