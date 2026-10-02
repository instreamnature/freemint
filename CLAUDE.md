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
  `rx_if_input_fail` som kom vid uppstart. Återstår: test med XaAES.

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

1. FPGA-sidan: undersök hur skrivningar till mailboxregistren (`0x80013000`)
   kvitteras mot CT60, och om samtidig åtkomst från ARM-sidan eller
   klockdomänövergången kan ge utebliven TA eller TEA.
2. Mjukvarutest: ta bort läs-modifiera-skriv av `stat_ctrl` i
   `sv3eth_output` och skydda `send_packet()` med `splhigh()`/`spl()` i
   stället. Färre mailboxåtkomster bör ge färre krascher. Nästa fel-PC visar
   om det är andra mailboxåtkomster som drabbas.
3. (Klart) `in_errors` = `rx_if_input_fail`, alltså full mottagningskö,
   främst vid uppstart (avbrottsbiten kvar i mailboxen efter reset) och
   under stora nedladdningar. Rättning av avbrottsbiten väntar.
4. Möjliga senare ändringar: ISR med assembler-omslag som i SV2, eller låta
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
