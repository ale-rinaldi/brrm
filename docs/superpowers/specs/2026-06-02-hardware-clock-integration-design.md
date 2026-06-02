# Integrazione hardware "Clock interno" con brrm

Data: 2026-06-02
Branch: `feature/hardware-clock`

## Obiettivo

Integrare il nuovo cronometro GPS-sincronizzato (`arduino/cronometro_gps`) con
le applicazioni desktop Gambas **brrm** (arrivo) e **brrm-partenza** (partenza),
affiancandolo all'hardware esistente senza romperlo.

Le app devono poter scegliere fra due tipi di hardware:

- **Legacy** — comportamento attuale identico: l'Arduino e' un semplice trigger,
  il PC marca il tempo con il proprio orologio, seriale a 9600 baud.
- **Clock interno** — il nuovo sistema: l'Arduino invia il timestamp assoluto
  (UTC) al millisecondo, healthcheck bidirezionale, label di stato ricca e modal
  diagnostica su richiesta. Seriale a 115200 baud.

## Contesto del codice esistente

- **brrm / brrm-partenza**: Gambas 3 / Qt6. Avvio da `FMain`.
- **Seriale legacy** (`FMain.Arduino_Read`): qualunque byte in arrivo =
  passaggio fotocellula; timestamp prodotto dal PC con
  `Format$(Time, "hh:nn:ss.uuu")`; accodato in `BArrivi` (ListBox) e drenato da
  `TimerArrivoQueue`. `SerialPort` ricreato a ogni riconnessione (workaround bug
  Gambas 3.20, `Object.Attach($hSerial, Me, "Arduino")`).
- **Presenza Arduino** (`TimerArduino`, 2 s): controlla solo che la porta sia
  aperta e che il device `/dev/tty*` esista ancora → `SetArduinoStatus()`.
- **`LabelArduino`**: 2 stati (`ARDUINO OK` verde / `NO ARDUINO` rosso), nessun
  click handler.
- **Config**: file `key=value` in `~/.config/brrm/*.conf` (condiviso da entrambe
  le app). `FSettings` = `TabStrip` (brrm 3 tab, brrm-partenza 2). Salvataggio via
  `MFile.AtomicSave` + chiamata a `FMain.XxxReinit()`.
- **Codice condiviso**: i moduli in `brrm-utils/.src` (`MFile`, `MLog`, `MTime`,
  `MAlignSync`, `JsonHelpers`) sono **symlink** (git mode 120000) verso le due
  app. `brrm-utils` e' la source-of-truth.
- **Test**: progetto `brrm-utils-test`. CI: `build.sh` compila entrambe le app.
- **Firmware attuale** (`arduino/cronometro_gps/cronometro_gps.ino`): free-running,
  emette `PASSAGGIO:` **e** una riga `DIAG:`/`DIAG_NOPPS:` ogni secondo; nessun
  comando in ingresso.

## Decisioni di design (concordate)

1. **Ambito**: firmware + brrm + brrm-partenza, in un'unica passata.
2. **Modello protocollo**: `PASSAGGIO` resta asincrono (evento); le diagnostiche
   diventano request/response; nessun invio ciclico. Healthcheck ping/pong.
3. **Label**: 5 stati (GPS verde / RTC giallo / no-sync arancio / non risponde
   grigio / assente rosso). In modalita' legacy resta a 2 stati.
4. **Tempo d'arrivo**: in modalita' clock interno vince il tempo assoluto
   dell'Arduino (UTC→locale per display/storage, unix-ms verso l'export Sheets).
   Il clock del PC non e' piu' usato per il timing.
5. **Settings**: nuovo tab "Hardware" + `~/.config/brrm/hardware.conf`.
6. **Codice condiviso**: nuovo modulo `MClock` in `brrm-utils`, symlinkato nelle
   due app (lo stato seriale resta in `FMain`).
7. **Tag per passaggio**: ogni `PASSAGGIO` porta la sorgente (`G`/`R`) attiva alla
   cattura.
8. **Modal**: dati live finche' aperta (refresh ~1 s) + pulsante Aggiorna manuale.
9. **Encoding**: ASCII terso, delimitato da `\n`, tag a singolo carattere,
   parsing sul primo byte.
10. **Pong con timestamp**: la risposta al ping porta anche il timestamp assoluto
    corrente (stessa precisione del passaggio), cosi' brrm ha un "tempo live"
    dall'Arduino ogni ~1 s da mostrare e, in futuro, da usare per disciplinare il
    clock di sistema del PC.

## Protocollo seriale (modalita' clock interno)

USB-CDC a 115200 baud, affidabile a livello link → nessun checksum applicativo.
Ogni messaggio e' una riga terminata da `\n`. Parsing per primo byte.

### Arduino → PC (asincrono / risposte)

| Msg     | Formato                         | Esempio                                            | Note |
|---------|---------------------------------|----------------------------------------------------|------|
| Passage | `P<unix>.<ms><G\|R>`            | `P1718000000.347G`                                 | `ms` 3 cifre zero-pad; ultimo char = sorgente |
| Pong    | `K<src>[<unix>.<ms>]`           | `KG1718000000.347` / `KN`                          | risposta a ping; con timestamp live (assente se `src=N`) |
| Diag    | `D<CSV posizionale>`            | `D1718000000,G,-1,187.3,A,9,0.93,142,0,12,3,57`    | su richiesta |
| Id      | `Y<ver>`                        | `Y1`                                               | identita' firmware |
| Warn    | `W<nmea>,<ref>`                 | `W1718000001,1718000003`                           | evento desync, bassa frequenza |

Ordine colonne `D` (posizionale, senza chiavi):
`unix, src, dev_tcnt, nmea_ms, fix, sat, hdop, alt, nmea_persi, since_pps_s, since_rtc_write_s, uptime_s`

- `src` ∈ `G` (PPS), `R` (RTC), `N` (nessun tempo valido).
- `dev_tcnt`: deviazione di TCNT1 da 32768 al PPS (indicatore drift cristallo).
- `nmea_ms`: ritardo dell'RMC dopo il PPS.
- `fix`: `A`/`V`. `since_pps_s`/`since_rtc_write_s`/`uptime_s`: campi nuovi per la
  modal (vedi firmware).
- **Timestamp nel pong**: `K` include `<unix>.<ms>` con la stessa granularita' del
  passaggio (ms). Quando `src=N` non c'e' tempo valido e il timestamp e' omesso
  (`KN`). La precisione al ms e' quella esternamente significativa: il jitter di
  trasporto USB-CDC (~ms) rende inutile inviare la frazione sotto il ms.
  L'allineamento vero del clock di sistema (misura round-trip dell'offset:
  `t_send` → `?`, `K<...>` → `t_recv`) e' lavoro futuro; il timestamp nel pong ne
  e' il mattone abilitante.

### PC → Arduino (comandi, 1 byte + `\n`)

| Comando | Byte | Risposta |
|---------|------|----------|
| Ping    | `?`  | `K<src>[<unix>.<ms>]` |
| Diag    | `@`  | `D<...>` |
| Id      | `#`  | `Y<ver>` |

Il parser dell'Arduino accumula fino a `\n`, riconosce il comando, ignora ignoti.

## Componenti

### A. Firmware `cronometro_gps.ino`

1. **Rimuovere l'emissione ciclica**: eliminare da `loop()` le due chiamate
   `emettiDiagnostica(...)` (post-PPS e heartbeat). Lo stato diagnostico continua
   a essere mantenuto nelle variabili, ma non viene piu' spinto.
2. **Lettore comandi su `Serial` (USB)**: in `loop()`, accumulare una riga di
   comando (buffer piccolo, `\n`-terminato) e gestire `?`/`@`/`#`. `Serial` finora
   era solo output; aggiungere la lettura non tocca SoftwareSerial (GPS). La
   risposta a `?` (`K`) include il timestamp assoluto corrente
   (`unix_riferimento` + frazione da `TCNT1/32.768`, formattata a ms), omesso se
   `src=N`.
3. **`PASSAGGIO` taggato**: nuova `volatile uint8_t cattura_fonte`, impostata
   nella ISR di capture insieme a `unix_cattura` (`PPS_ACTIVE`→`'G'`, altrimenti
   `'R'`). Output compatto `P<unix>.<ms><src>`.
4. **`DIAG` arricchito** con `since_pps_s`, `since_rtc_write_s`, `uptime_s`
   (variabili gia' disponibili o derivabili da `millis()`/`ultimo_pps_ms`/
   `ultimo_aggiornamento_rtc_ms`). DIAG e' on-demand: costo irrilevante.
5. **`Y<ver>`** su comando `#` per identificazione positiva del firmware.
6. **Tag tersi**: `P`/`K`/`D`/`Y`/`W` come sopra. ISR invariati; baud 115200.

### B. `brrm-utils/MClock.module` (symlinkato nelle due app)

Helper puri (stile module del repo, unit-testabili):

- `ParseLine(sLine) → record tipizzato` — dispatch sul primo byte; estrae i campi
  di `P`/`K`/`D`/`Y`/`W`; per `K` ritorna sorgente + timestamp opzionale; valida
  (tag noto, numerici parsabili) e segnala righe malformate da scartare.
- `UnixMsToLocalDate(unixSec, ms) → Date` — epoch UTC + ms → `Date` locale Gambas,
  corretta per fuso/DST.
- `LabelState(mode, devicePresent, portOpen, lastPongAgeMs, src) → enum` — calcola
  uno dei 5 stati label.
- Costanti dei comandi (`?`/`@`/`#`) e dei tag.

Lo **stato per-app** (handle `SerialPort`, buffer di riga, ultimo stato, istante
ultimo pong) resta in `FMain`: cosi' il workaround Gambas 3.20 sul ciclo di vita
del `SerialPort` non viene toccato.

### C. `FMain` (entrambe le app)

- **Modalita'**: legge `~/.config/brrm/hardware.conf` (`mode = legacy|internal`),
  default **legacy** se assente. Campo `$hwMode`.
- **Apertura seriale**: `Speed` 9600 (legacy) / 115200 (internal); device
  autodetect invariato (`MFile.FindArduinoDevice`).
- **`Arduino_Read`** con branch su modalita':
  - *legacy*: identico a oggi (byte → passaggio col clock PC).
  - *internal*: accumula in `$serialBuf`, split su `\n`, dispatch per riga via
    `MClock.ParseLine`:
    - `P` → se `FotON`: `MClock.UnixMsToLocalDate` → enqueue in `BArrivi`
      portando **unix-ms + sorgente**, log; altrimenti log "ignorata".
      (Gate `FotON`/`FotOFF` preservato.)
    - `K` → aggiorna `$clockSrc`, `$lastPongMs` e `$arduinoTimeLive` (timestamp
      dal pong, se presente) per il display del tempo live.
    - `D` → aggiorna `$lastDiag` (per la modal).
    - `Y` → aggiorna `$fwVersion`.
    - `W` → `MLog` (evento desync).
- **Healthcheck** (`TimerArduino`):
  - *internal*: invia `?` ogni ~1 s; marca **non risponde** se nessun `K` da ~3 s;
    calcola la label a 5 stati via `MClock.LabelState`.
  - *legacy*: invariato (porta aperta + `/dev` esiste).
- **Tempo d'arrivo**: l'unix-ms dell'Arduino e' la fonte di verita'. Convertito in
  locale per il flusso stringa esistente (`FArrivo.setOrario`), e l'unix-ms grezzo
  alimenta l'export Sheets (`arrivo_unixms_column`).
- **Label + click**: `SetClockLabel(state)` per i 5 stati (vedi sotto); handler di
  click su `LabelArduino` (`MouseUp`; se problematico in Gambas, convertire in un
  Button con aspetto label) apre la modal. In legacy il click e' no-op.
- **`HardwareReinit()`** (nuovo, sul modello `AlignReinit`/`SheetReinit`/
  `LogReinit`): richiamato da `FSettings`; rilegge la modalita', chiude/riapre la
  seriale al baud corretto, resetta label e stato healthcheck.

#### Stati label (modalita' internal)

| Condizione                          | Testo              | Colore  |
|-------------------------------------|--------------------|---------|
| Nessun device / porta non aperta    | `NO ARDUINO`       | rosso   |
| Porta aperta, nessun `K` entro 3 s  | `ARDUINO ?`        | grigio  |
| Connesso, sorgente GPS/PPS          | `ARDUINO · GPS`    | verde   |
| Connesso, sorgente RTC              | `ARDUINO · RTC`    | giallo  |
| Connesso, nessun tempo valido (`N`) | `ARDUINO · NO SYNC`| arancio |

In modalita' legacy: 2 stati come oggi (`ARDUINO OK` / `NO ARDUINO`).

### D. Modal diagnostica `FClockDiag`

Nuova form, **symlinkata nelle due app** (stesso pattern dei moduli; fallback a
duplicazione se i symlink di form danno problemi in Gambas). Apertura: invia `#`
+ `@`; poi **auto-refresh ~1 s finche' aperta** (stop alla chiusura) + pulsante
*Aggiorna* manuale. Sezioni:

- **Sorgente**: header colorato come la label.
- **GPS**: fix `A`/`V`, satelliti, HDOP, altitudine, ritardo NMEA (ms), contatore
  NMEA persi.
- **RTC**: stato sync da GPS + eta' (`since_rtc_write_s`), deviazione TCNT (drift).
- **Tempo**: UTC + locale, uptime, stato macchina (da `src` / DIAG). Il tempo
  live arriva anche dal pong (`$arduinoTimeLive`, ~1 s) anche a modal chiusa.
- **Link**: device path, baud, eta' ultimo `K`, versione firmware (`Y`).

### E. Settings — tab "Hardware"

- Nuovo tab (brrm: 4°; brrm-partenza: 3°): selettore Legacy / Clock interno
  (RadioButton o ComboBox) + hint read-only (device rilevato, baud derivato).
- Salvataggio: scrive `hardware.conf` (`mode = legacy|internal`), poi chiama
  `FMain.HardwareReinit()`.

### F. Logging (`MLog`)

- Mantenere `arduino_online`/`arduino_offline`.
- Aggiungere transizioni di sorgente (`gps`↔`rtc`↔`nosync`), warning desync (da
  `W`), e la modalita' all'avvio. Per-passaggio: loggare la sorgente insieme
  all'evento `fotocellula`. Solo transizioni/eventi, niente ciclico.

## Sicurezza / retrocompatibilita'

- Default **legacy** quando `hardware.conf` manca → installazioni esistenti
  invariate. Percorso legacy intatto (9600, clock PC, label 2 stati, niente ping).
- Se l'utente seleziona "internal" ma il device non e' firmware clock: `?` non
  riceve `K` → label grigia `ARDUINO ?`; il parser accetta solo righe `P` valide,
  quindi byte spuri non possono essere scambiati per passaggi. `#`/`Y` permette a
  brrm di avvisare "questo non e' un Arduino clock".
- La lettura da `Serial` aggiunta al firmware non riguarda il firmware legacy
  (firmware diverso); la modalita' internal di brrm parla solo col nuovo firmware.

## Testing

- `brrm-utils-test`: unit test per `MClock.ParseLine` (tutti i tag, righe
  malformate), `UnixMsToLocalDate` (DST + zero-pad ms), `LabelState` (transizioni
  fra i 5 stati).
- `build.sh`: gia' compila entrambe le app in CI.
- Firmware: verifica manuale del round-trip protocollo su hardware reale.

## Fuori ambito

- Riscrittura del ciclo di vita `SerialPort` (resta in `FMain`, invariato).
- Modifiche al protocollo legacy.
- Identificazione automatica della modalita' dal device (la modalita' e'
  esplicita in `hardware.conf`); `#`/`Y` serve solo per l'avviso.
