# sv3eth – nätverksdrivrutin (XIF) för SuperVidel 3 under FreeMiNT

Den här filen sammanfattar felsökningen hittills, så att arbetet kan fortsätta
utan att allt behöver förklaras igen. Svara på svenska.

## Projekt och hårdvara

- Fork av `freemint/freemint`. Drivrutinen ligger i `sys/sockets/xif/sv3eth/sv3eth.c`
  och byggs till `sv3eth.xif`. Gränssnittet heter `en0`.
- Dator: Atari Falcon030 med CT60 (68060), FreeMiNT 1.19, MintNet (`inet4.xdd`).
- Nätverkshårdvara: SuperVidel 3 med Zynq 7020. Ethernet går till ARM-sidan
  (PetaLinux). Paket utväxlas med 68060 via en mailbox i FPGA:n och
  paketbuffertar i FPGA:ns DDR (`PS_DMA_BASE` = `0x9F000000`, 64 RX- och
  64 TX-slottar à 2 KB).
- Mailboxens `len_fifo`: övre 16 bitar = slot-index, nedre 16 bitar = längd.
  Längd `0xFFFF` betyder ACK (TX-slot ledig / RX-slot kvitterad).
- CT60:ns boot-flash mappar `0x80000000`–`0xBFFFFFFF` som no cache / no burst.
  FPGA:ns bussgränssnitt klarar burst om fyra 32-bitsord (MOVE16).
- Reset av Falcon (reset-knappen) nollställer FPGA-logiken i 90 MHz-domänen
  men **inte** mailboxarna, som ligger i ARM-sidans 200 MHz-resetdomän.
  PetaLinux fortsätter köra vid Falcon-reset.
- Bygge: 
  - Miljö: Cygwin64 på Windows 10. Repot ligger i `~/gitrepos/freemint`.
  - Bygg från `~/gitrepos/freemint/sys/sockets/xif/sv3eth` med `make`.
  - Resultat: `.compile_060/sv3eth.xif`.
  - Överföring till och från Falcon: `ftp` i Cygwin. Inloggningsuppgifter
    finns i `~/.netrc`, aldrig i repot.
  - Sökvägen på falcon där `sv3eth.xif` ska läggas är `/c/mint/1-19-cur/ct60/`.
    Det är kanske lämpligt att döpa om den förra aktiva sv3eth.xif till `sv3ethXX.xix`
    där XX är ett tvåsiffrigt löpnummer, och xix är en inaktiv drivrutin. Det får bara
    finnas en aktiv XIF-drivare som kommunicerar med SV3 nätverkshårdvaran.

## Symptom

- Med minnesskydd på: förr eller senare "Operating system has been killed.
  You must reboot your system." Ibland efter en timme, ibland efter några minuter.
- Även (troligen med minnesskydd av) en gång:
  `pid 94 (conholio060): system stack not in proc structure` /
  `FATAL ERROR. You must reboot the system.`
  Det är FreeMiNT-kärnans kontroll (troligen i `sys/proc.c` kring `sleep()`)
  av att processens sparade supervisor-stackpekare ligger inom dess kärnstack.
  Processen som nämns är bara den som var aktuell, inte nödvändigtvis boven.
  Kontrollen görs oavsett minnesskydd.
- Kraschar inträffade först bara när datorn stått orörd. Med urkopplad
  nätverkskabel: ingen krasch på över två timmar.
- 2026-09-29: första kraschen under aktiv användning. Drivrutinen från 27/9
  (med `c_conws` kvar i TX-vägen) var laddad. Via `curl` från Cygwin
  gjordes en FTP-session: listning och RNFR/RNTO av `sv3eth.xif` till
  `sv3eth21.xix` (gick igenom helt). Några sekunder efter att sessionen var
  klar kom "Operating system has been killed", när fönstret i GUI:t
  uppdaterades med musen. Alltså inte under själva överföringen.
- "Operating system has been killed" visar aldrig adress, PC eller
  registerdump. Förklaring: se "Om Operating system killed" nedan.
- 2026-09-29 20:47–21:55: krasch med drivrutinen utan `c_conws`. Alla nya
  räknare var 0 och `out-errors` 0. Krasch efter cirka 20 300 paket in och
  10 700 ut, vid lugn tomgångstrafik (2–5 paket/s). `in-errors` gick från
  356 till 409 och ökade bara under en trafiktopp (upp till ~100 paket/s),
  när användaren surfade och laddade ner en fil på några MB på Falcon.
- Det finns ingen skärmsläckare på Falcon.
- 2026-09-30: `pid 98 (conholio060): system stack not in proc structure`
  med drivrutinen med uppdelade RX-räknare, minnesskydd **på**, efter
  enbart reset-knapp (PetaLinux igång). Ingen logg från den körningen.
  Före kraschen: `rx_if_input_fail` 272 direkt efter start (= `in-errors`),
  övriga räknare 0. `out-errors` som inte syns i drivrutinens räknare kommer
  från `arp_free()` (`sys/sockets/inet4/arp.c:178`), alltså paket som
  kastas när ARP inte besvaras. Normalt beteende.

- 2026-10-01: första kraschen i konsolläge (utan XaAES), debugnivå 1,
  minnesskydd på, kort efter att `sv3log.sh` startats och några `ls`/`ifconfig`:
  `pid 0 (MiNT): MEMORY VIOLATION: type=hardware RW=?? AA=0 PC=20E08A80 BP=108AFBC`
  följt av "Operating system killed". AA=0 tyder på NULL-pekare. BP är
  kärnans basepage (~`0x0108B000`), så PC ligger högt i FastRAM där moduler
  laddas. Nästa steg: slå upp PC mot `sv3eth.xif` med hjälp av
  funktionsadresserna som `driver_init()` skriver vid start (`/kern/bootlog`
  eller "Write boot log" i bootmenyn → `/c/mint/1-19-cur/boot.log`).
  I en omlänkad fil med symboler ligger `init` på `0xE4`, `sv3eth_open` på
  `0x112` och `_etext` på `0x1650`.
- **Uppslaget (2026-10-01):** `boot.log` från samma uppstart gav
  `sv3eth_open 0x20E08866` m.fl. Alla sju adresser stämmer med drivrutinen på
  `0x20E08754`. PC `0x20E08A80` = offset `0x32C` i `sv3eth_output`,
  instruktionen direkt **efter** `move.l d0,0x80013000`, alltså skrivningen
  `mbox0.stat_ctrl &= ~MBOX_STAT_RX_LFIFO_IE` (sv3eth.c:383). 68060
  rapporterar bussfel på skrivningar via store-bufferten i efterhand, med PC
  på nästa instruktion. Instruktionen på `0x32C` läser bara RAM.
  `type=hardware`, `RW=??`, `AA=0` passar ett sådant fördröjt skrivbussfel.
  Troligen **ingen NULL-pekare**.
- **Slutsats (trolig):** en mailboxåtkomst (`0x80013000`) får ibland bussfel
  (TEA/timeout). Med pid 0 aktuell ger det "Operating system killed". Annars
  får aktuell process `SIGBUS`, och om bussfelet sker i ISR:en eller i
  `if_doinput()` på `if.c`:s statiska stack ger signalleveransen
  "system stack not in proc structure". Förklarar båda kraschtyperna och att
  nätverkstrafik krävs.
- Funktionsadresser vid denna uppstart: open `0x20E08866`, close `0x20E088A4`,
  output `0x20E08A28`, ioctl `0x20E08E6A`, config `0x20E08C68`,
  send_packet `0x20E088D4`, service `0x20E08FEA`. CT60 har 512 MB FastRAM.
  Omlänkning med symboler: samma länkkommando som Makefile men utan `-s`,
  sedan `m68k-atari-mint-nm -n` och `objdump -d`.

- Kraschen i konsolläge kom efter högst ~10 minuter.
- **TEA (2026-10-02):** FPGA:ns VHDL driver inte TEA, och CT60:ns TEA från
  expansionskontakten är inte routad på SuperVidel-kortet. Det är tillåtet
  (slavar kan avsluta med enbart TA). Bussfelet kommer alltså troligen från
  CT60:ns egen timeout-logik, som drar TEA när ingen TA kommer i tid. Trolig
  orsak: FPGA:n svarar ibland inte med TA i tid eller alls (missad TS vid
  asynkron sampling, handskakning mot ARM-sidans 200 MHz-domän för
  mailboxregistren, eller arbitrering). Förslag: fristående stresstest under
  TOS som skriver/läser `0x80013000` i loop, för att framkalla bussfelet utan
  nätverk.

- **IACK-hypotesen (2026-10-02, trolig):** `stat_ctrl` ligger i CT60-domänen,
  så skrivningen i sig bör vara snabb. Men `sv3eth_output` **stängde av** IE
  med avbrott påslagna i CPU:n. Om FPGA:n just dragit IRQ6 tar 68060
  avbrottet efter skrivningen (sparad PC = nästa instruktion = `0x32C`),
  men när IACK-cykeln körs har FPGA:n släppt IRQ6. Ingen svarar med TA,
  CT60:ns timeout drar TEA och det blir bussfel. Passar PC, `AA=0`,
  `RW=??` och slumpmässigheten.
- **Åtgärd (2026-10-02):** i `sv3eth_output` ersätts IE-växlingen med
  `sr = spl7(); send_packet(...); spl(sr);` (kräver `#include "mint/asm.h"`),
  samma mönster som `lance.c`. Byggd, 5 926 byte. Föregående version sparas
  som `sv3eth23.xix`. ISR:ens egen `stat_ctrl = 0` sker på nivå 6 och är
  därför säker.
- **Resultat (2026-10-02):** i konsolläge 3 timmar utan krasch (32k paket in,
  13k ut), mot högst 10 minuter före ändringen. FTP av en 3 MB zip från
  Falcon till PC: md5sum stämmer, ~1,3 MB/s, alla räknare 0 utom de
  `rx_if_input_fail` som kom vid uppstart.
- **XaAES-test (2026-10-02):** ca 4,5 timmar med XaAES utan krasch, ca 66k
  paket in och 36k ut (förut krasch inom ~1 timme). Kraschen räknas som löst.
- Efter commit `8766d573`: gammal utkommenterad RX-kod borttagen ur
  `sv3eth_service()`, tömningen av mailboxen utbruten till
  `sv3eth_flush_mailbox()` och anropad i både `driver_init()` och
  `sv3eth_open()`, `spl7()` runt IE-avstängningen även i `driver_init()` och
  `sv3eth_close()`, och en fjärde `stats`-rad med antal kastade poster vid
  init och open. Tömningen vid open tog **inte** bort felen vid uppstart
  (24 `rx_if_input_fail` efter första försöket). Texten från `sv3eth_open`
  syns inte i `boot.log`, eftersom `ifconfig en0 up` körs från `/etc/rc`
  efter att bootloggen stängts.
- Commit `22cb297b` (2026-10-03). Därefter: inga fel vid uppstart, och
  flushed = 56 RX både vid init och open. Trolig orsak: ARM-sidans
  Linux-drivrutin fyller sina lediga RX-slottar och väntar, och 8 av 64
  slottar har gått förlorade där (aldrig kvitterade före Falcon-reset eller
  krasch). Tidigare raderade `driver_init` 61. Nästa steg: åtgärda i
  Linux-drivrutinen, t.ex. med ett "Falcon har startat om"-ord från
  `driver_init()` som frigör alla RX-slottar, eller timeout per slot.
- Hastighet: FTP av 16 MB från `/ram/` på Falcon gav 2,59–2,61 MB/s (från
  disk ~1,35 MB/s), så disken var flaskhalsen vid de tidigare mätningarna.

## Linux-drivrutinen på ARM-sidan (sv3mboxdriver)

- Virtuell Ubuntu: `instream@192.168.10.17` (ssh-nyckel `id_ed25519` från
  Cygwin, `-o BatchMode=yes`). Kodträd:
  `/home/instream/SV3_petalinux/SV3_petalinux_2023.1/project-spec/meta-user/recipes-modules/sv3mboxdriver/files/sv3mboxdriver.c`
  (git-repo i `/home/instream/SV3_petalinux`).
- Linux "TX" = Falcon RX. `find_next_free_tx_slot()` sätter
  `tx_psdma_buf_flags[pos]`. Den frigörs bara av ACK från Falcon i
  `sv3mboxdriver_poll()`. Ingen timeout och ingen återställning.
- Mailbox-FIFO-djup 1024 (`pl.dtsi`), så FIFO-overflow är inte orsaken.
- **Trolig orsak till förlorade slottar:** ett ord i CT60-domänens
  hållregister mellan AXI-stream och `len_fifo` försvinner vid Falcon-reset
  (Xilinx-mailboxen nollställs inte). RX-info som tappas kvitteras aldrig.
  **Bekräftat (2026-10-05):** i Vivado-blockschemat sitter AXI4-Stream
  Register Slices på AXIS-bussarna mellan mailboxarna och
  68060-registergränssnittet (90 MHz CT60-domänen, ARM-sidan 200 MHz), och
  de nollställs med `ct60_reset`. De behövdes för att klara timing. Ord i
  dem (och i registergränssnittet) försvinner vid Falcon-reset, i båda
  riktningarna. En Falcon-krasch mellan läsning av `len_fifo` och ACK
  förlorar också en slot, så rättningen görs i mjukvara (handslag med
  `0xFFFEFFFF`, Falcon faller tillbaka på tömning med ACK om svar uteblir).
- **Förslag:** omstartshandslag. `driver_init()` skickar `0xFFFEFFFF`,
  Linux frigör alla TX-slottar och svarar med samma ord, och Falcon kastar
  allt utan ACK tills svaret kommer.
- Andra fynd: `dev_info()` per paket (irq/xmit/poll), en stor
  prestandabroms. `pos` och `len` från mailboxen kontrolleras inte
  (out-of-bounds på `tx_psdma_buf_flags[]` och DMA-området).
- **Gjort (2026-10-05), ej committat i Ubuntu-repot:** `sv3mboxdriver.c` har
  handslaget (`SV3_RESYNC_WORD` i `poll()`), kontroll av `pos`/`len`,
  `rx_dropped++`, `eth_hw_addr_set()` i `set_mac_address`, och `dev_info`
  per paket bortkommenterade (`queue full` via `net_ratelimit()`). Byggd med
  `petalinux-build -c sv3mboxdriver -x compile` (efter
  `source ~/Petalinux2023.1/settings.sh`). `.ko` hamnar i
  `build/tmp/work/zynq_generic_7z020-xilinx-linux-gnueabi/sv3mboxdriver/1.0-r0/`.
- Modulen är en laddbar modul i rootfs (`CONFIG_sv3mboxdriver=y` i
  `project-spec/configs/rootfs_config`, `KERNEL_MODULE_AUTOLOAD`). På SV3:
  `/lib/modules/6.1.0-xilinx-v2023.1/extra/sv3mboxdriver.ko`. Snabb
  uppdatering: `sudo scp` av `.ko` till SV3 från användarens ssh-session på
  Ubuntu (Claude saknar nyckel till `root@192.168.10.86`). Backup:
  `/home/root/sv3mboxdriver.ko.orig`. Root-hemkatalog på SV3 är `/home/root`.
  **Starta inte om SV3 med `reboot`** (FPGA:n programmeras om och Falcon
  fryser): `shutdown -h now` och strömcykla. Fullt: `petalinux-build` +
  `rsync_rootfs_to_zturn.sh`.
- `dmesg`-meddelandet `queue full, stopping kernel TX queue` innan Falcon
  har startat MiNT är normalt.
- Efter strömcykling men före handslaget: flushed 62/62, alltså 2 slottar
  förlorade redan vid strömpåslaget, i riktningen Linux → Falcon. Orsaken är
  okänd. `RESET`-instruktionen och Ctrl-Alt-Del nollställer **inte**
  FPGA-logiken, bara reset-knappen och strömpåslaget gör det.
- **Resultat handslag (2026-10-05):** `sv3eth.c` med
  `sv3eth_resync_mailbox()` (7 157 byte, föregående som `sv3eth26.xix`).
  Efter reset-knapp: `resync OK, threw away 58 RX`, flushed at init 0 och at
  open **64**, inga in-/out-errors vid uppstart, och `dmesg` visar
  `resync from 060, all TX slots freed`. Hastighet med tyst Linux-modul:
  ~2,6 MB/s från `/ram/`, alltså oförändrad.

## Avbrottsvägen i hårdvaran (spår, ej bekräftat)

SV3 ger avbrott på nivå 6 med egen vektor (201, `0x324`). Falcons MFP ger
också vektoriserade avbrott på nivå 6. ABE-chipet (CPLD på CT60/CT63/CT60e)
sitter mellan moderkortsbussen och CT60-bussen, tar emot avbrott från både
moderkortet och CT60:ns expansionskontakter (där SuperVidel sitter) och
avgör om en IACK-cykel ska gå till SuperVidel eller till moderkortet (MFP).
SV2-drivrutinen gjorde en dummyläsning av `0xFFFF8240` "to satisfy
ABE-chip". Om IACK någon gång hamnar fel kan processorn få fel vektor och
hoppa till fel undantagshanterare, vilket skulle kunna ge "fel stack"-krascher.
Ren spekulation än så länge.
- Kraschen kräver alltså nätverkstrafik, men antal paket vid krasch varierar
  kraftigt (under 1 000 till cirka 48 000). Det talar för en sällsynt
  slumpmässig händelse snarare än ett fast tröskelvärde.

## Åtgärdat hittills

- RX-buffertspill: tidigare accepterades längd upp till 2048 men bufferten
  rymde bara cirka 1596 byte. Nu `SV3ETH_MAX_RX_LEN` = 1518 i både
  kontrollen och `buf_alloc`, och `buf_alloc` görs efter faktisk längd.
- Slot-index från mailboxen maskas nu med `& (PS_DMA_BUFFER_PKTS - 1)` i
  `Check_Rx_Buffers()` och i tömningsloopen i `driver_init()`.
- `ksprintf`/`c_conws` borttagna ur ISR-kedjan. Ersatta med räknarna
  `bad_slot_count`, `oversized_rx_count`, `tx_fifo_full_count`.
- `sv3eth_timeout()` gör inte längre fil-I/O eller `Tgettime`/`Tgetdate`.
  Den anropas från `if_slowtimeout()` i `sys/sockets/inet4/if.c`, som är en
  root-timeout (`addroottimeout`). Där får man inte blockera eller göra
  processbundna GEMDOS-anrop. Returtypen är `void`.
- Läsningen av `0xFFFF8240` i ISR:en borttagen (gjorde ingen skillnad).

## Uteslutet

- De tre diagnosräknarna har varit 0 i alla körningar.
- Buf-läcka vid misslyckad `if_input()`: `if_input()` tar alltid över bufferten,
  även vid fel (se kommentar i `dummyeth.c`). Anropa **inte** `buf_deref()`
  efter `if_input()`.
- Negativa/för stora index: all indexaritmetik är osignerad och maskerad.
- Fil-I/O i timeout-funktionen: kraschar även utan den.
- `mprot040.c` skriver aldrig till TTR-registren, bara läser dem.

## Viktigt om felmeddelandet (sys/arch/sig_mach.c:136-143)

`FATAL("system stack not in proc structure")` utlöses bara när
stackpekaren ligger **ovanför** `curproc->stack + STKSIZE`, alltså helt
utanför processens kärnstack. En överskriden stack (under) ger i stället
`ALERT("stack overflow")`. Kärnstacken är 16 KB + 8 KB (`sys/mint/proc.h`).
Kraschen tyder alltså på att koden kört på en annan stack.

Kandidat: `if_doinput()` i `sys/sockets/inet4/if.c` byter till en delad
statisk stack (`static char stack[8192]`) och kör hela IP/TCP-hanteringen,
inklusive `sv3eth_output`, där. Om något i den kedjan somnar (t.ex.
`c_conws` = `Fwrite(1, ...)` till aktuell process stdout) kan en ny
`if_doinput` starta på samma stack och skriva över den första.

2026-09-29: alla `c_conws` borttagna ur TX-vägen och `sv3eth_timeout`.
De är ersatta med separata räknare, som skrivs ut med
`ifconfig en0 -f /h/root/sv3stat.opt` (filen innehåller raden `stats 1`).
Dessutom är `nif->out_errors` nu ökad vid TX-drop. `sv3log.sh` loggar även
räknarna. Resultat: kraschade ändå och räknarna var 0, så de `c_conws`-
ställena kördes aldrig. **`c_conws`-hypotesen stämmer inte** för den här
kraschen. Själva stack-iakttagelsen ovan gäller fortfarande.

2026-09-30: `in_errors` uppdelad i `rx_buf_alloc_fail` (buffertpoolen tom)
och `rx_if_input_fail` (mottagningskön full). De visas på en tredje
`stats`-rad. Byggd (5 950 byte), föregående version sparad som
`sv3eth22.xix`.

Känd bugg, ej åtgärdad: i `send_packet` frigörs inte TX-slotten om Len
FIFO är full efter kopieringen (`tx_fifo_full_post`), så slotten läcker.

## Om "Operating system killed" (sys/arch/mprot.x:100-165)

- Meddelandet skrivs vid ett minnesskyddsfel (bus error) när den aktuella
  processen är **pid 0** (MiNT själv) eller har `F_OS_SPECIAL`. Felet sker
  alltså i kärnkod utan vanlig aktuell process, t.ex. tomgång, root-timeout
  eller interrupt ovanpå det.
- Kärnan bygger en alert med Type, PC, Addr och BP, men bara om
  `debug_level >= 1` (ALERT_LEVEL). Debug/trace level i bootmenyn stod på 0,
  då skapas ingen alert alls. Ändrad till 1 2026-09-30.
- `_ALERT()` (`sys/debug.c:291`) skriver först till `u:\pipe\alert` för
  XaAES. När GEM körs lyckas det, och sedan stoppar `FATAL()` datorn innan
  XaAES hinner visa alerten. Informationen går förlorad.
- Utan GEM finns ingen `u:\pipe\alert`. Då skrivs
  `MEMORY VIOLATION: type=... AA=... PC=... BP=...` direkt på skärmen.
  Krävs alltså: debugnivå 1 **och** körning utan GEM (FTP-server och
  `sv3log.sh` från textkonsolen).

### Start utan XaAES (för att få se alerten)

- `/sbin/init` på Falcon är Minix/BSD-stil. Den läser `/etc/rc`,
  `/etc/rc.single` och `/etc/ttytab` (`/etc/rc.boot` finns inte). Det finns
  ingen `/etc/inittab`.
- XaAES startas från `/etc/ttytab`, rad
  `console "/c/mint/1-16-1/xaaes/xaloader.prg" tw52 on secure`. Loadern
  laddar ändå modulen från kärnans sysdir (`/c/mint/1-19-cur/xaaes/`), se
  `xaaes/src.km/xaloader/xaloader.c`.
- Konsolläge: kommentera bort xaloader-raden och aktivera
  `console "/usr/sbin/getty console" vt52 on secure`. `INIT=/sbin/init` i
  `mint.cnf` ska vara kvar. Då startar nätverk och FTP via `/etc/rc`.
  Fungerar (2026-10-01).
- `INIT=u:\bin\bash` fungerar dåligt: ingen `TERM` och ingen
  tangentbordstabell.

## Övriga iakttagelser

- ISR:en (`__attribute__((interrupt))`) sparar d0-d1/a0-a2 och fp0-fp1 och
  använder lite stack (kontrollerat med `objdump`). SV2-drivrutinen använder
  i stället ett assembler-omslag (`svethlana_i6.S`) som sparar d0-d7/a0-a6
  och inte rör FPU:n.
- MintNets buffertpool (`sys/sockets/buf.c`) och `addroottimeout` skyddas
  med `splhigh()`/`spl7()` och får anropas från ISR:en.

## Tidigare hypotes (troligen fel, se ovan)

Kärnstacken överskrids. På 68060 finns ingen separat interruptstack, så
interrupt läggs på den aktuella processens kärnstack i proc-strukturen. Vid
tomgång ligger t.ex. Conholio och väntar djupt i kärnan (`Fselect` → `sleep()`),
och ISR-kedjan (`SV3_mbox0_isr` → `sv3eth_service` → `buf_alloc`/`if_input`)
körs ovanpå den redan djupa stacken. Ej bekräftat.

Alternativa spår: VRAM/grafiktrafik i samma DDR som DMA-buffertarna, eller
att grafikdrivrutinens VRAM-allokering kan överlappa `0x9F000000`
(skärmsläckare finns inte, så den kan strykas).

## Nästa steg

1. (Klart) Kraschen löst med `spl7()` i stället för IE-växling, och
   handslaget löser de förlorade slottarna.
2. Committa `sv3eth.c` (handslaget) och `sv3mboxdriver.c` i respektive repo.
3. Hastighet: väntslingan på 1000 `nop` i `send_packet`, MOVE16-kopiering,
   och läckan i `send_packet` vid `tx_fifo_full_post`.
4. (Historik) `in_errors` vid uppstart, nu borta efter handslaget: ibland `rx_if_input_fail`, ibland
   `rx_buf_alloc_fail` (62–272 st). **Inte** p.g.a. kvarstående IE: enligt
   VHDL-koden nollställer CT60-reset de fyra IE-bitarna (en per mailbox;
   bara mbox0 används). Mailbox-FIFO:erna i Xilinx-IP:t nollställs däremot
   inte. Trolig orsak: ARM-sidan samlar paket mellan `driver_init()` och
   `ifconfig en0 up`. När IE slås på i `sv3eth_open()` tar ISR:en hela
   backloggen på nivå 6 i ett svep, så att mottagningskön (60) eller
   buffertpoolen tar slut. `IFF_UP|IFF_RUNNING` sätts direkt efter
   `open` (if.c:390). Förslag: töm och kvittera mailboxen i
   `sv3eth_open()` innan IE slås på.
5. Möjliga senare ändringar: ISR med assembler-omslag som i SV2, eller låta
   ISR:en bara kvittera och schemalägga `sv3eth_service` via
   `addroottimeout(0, ..., 1)`.

(Klart: stackstorlek och kontrollen i `sig_mach.c` utredda, ISR-prologen
kontrollerad med `objdump`. `-fstack-usage` behövs inte.)

Senare, när kraschen är löst: TX-kö via `if_enqueue()` när alla TX-slottar är
upptagna. Kräver ny dequeue-logik vid TX-ACK i `Check_Rx_Buffers()`. Den gamla
dequeue-koden i filen är från SV2 och fungerar inte med mailboxen.

## Loggning

- Skriptet `/c/sv3log.sh` på Falcon loggar `ifconfig en0` var tionde sekund
  till `/c/sv3log.txt` med `sync` efter varje skrivning. Varje session börjar
  med en rubrikrad.
- Falcon har IP `192.168.10.10`. Loggen kan hämtas via FTP, t.ex.
  `curl --netrc 'ftp://192.168.10.10/%2Fc/sv3log.txt' -o sv3log.txt`
  FTP-servern startar i `/h/home/instream`, och sökvägar i en `ftp://`-URL
  räknas därifrån. `%2F` (kodat `/`) först i sökvägen gör den absolut.
  I råa FTP-kommandon (`-Q 'RNFR ...'`) skrivs vanliga `/c/...`.

## Viktiga regler i koden

- Funktioner som får anropas från interrupt (enligt kommentaren i filen):
  `buf_alloc(..., BUF_ATOMIC)`, `buf_deref(..., BUF_ATOMIC)`, `if_enqueue()`,
  `if_dequeue()`, `if_input()`, `eth_remove_hdr()`, `addroottimeout(..., 1)`.
- Inga BIOS/GEMDOS-anrop i ISR-kedjan eller i root-timeouts.
- Ändra en sak i taget mellan testkörningar.
