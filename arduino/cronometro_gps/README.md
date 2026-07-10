# Cronometro di precisione GPS-sincronizzato

Cronometraggio del passaggio di auto con timestamp UTC assoluto e precisione
al millisecondo, basato su Arduino Nano (ATmega328P), GPS NEO-M8N e RTC DS3231.

## Principio di funzionamento

La base di tempo fine e' il **Timer1** dell'ATmega328P, clockato esternamente dal
pin **32K** del DS3231 (32.768 kHz). Il Timer1 lavora in **CTC mode**:

- conta i tick frazionari dentro il secondo corrente;
- al passaggio della fotocellula l'hardware latcha `ICR1` (Input Capture) nel
  momento esatto dell'evento, indipendentemente dalla latenza software;
- `OCR1A` definisce il rollover:
  - `32767` in `RTC_ACTIVE` -> rollover ogni secondo (incrementa il riferimento);
  - `49151` in `PPS_ACTIVE` -> rete di sicurezza: scatta solo se il PPS e' perso
    (a ~1.5 s dall'ultimo PPS), permettendo di rilevare la perdita di fix senza
    mai perdere un secondo.

Il "tick di secondo" autorevole, in ordine di preferenza:

1. **PPS** del GPS — allineato all'inizio del secondo UTC (~ns di jitter);
2. **rollover CTC** del Timer1 — ancorato al cristallo del DS3231 (~2 ppm).

L'**NMEA** (solo `RMC` + `GGA` ridotto) serve per: bootstrap del tempo assoluto,
sanity check periodico e diagnostica (satelliti, HDOP, altitudine).

### Macchina a stati

```
INIT -> WAIT_RTC_SYNC --(rollover RTC)--> RTC_ACTIVE <--(PPS perso / PPS torna)--> PPS_ACTIVE
                       \--(timeout)------> UNSYNCED  --(PPS o NMEA)--> ...
```

- **RTC_ACTIVE**: tempo mantenuto dal Timer1/DS3231 (precisione ~ms). Cronometraggio attivo.
- **PPS_ACTIVE**: tempo agganciato al PPS (precisione ~30 us). Cronometraggio attivo.
- **UNSYNCED**: nessun riferimento valido (RTC vergine + no fix). Cronometraggio sospeso.

Dopo un riavvio a gara in corso, il DS3231 — mantenuto allineato al millisecondo
durante `PPS_ACTIVE` — consente di riprendere i rilevamenti entro ~1 s
dall'accensione, prima ancora del fix GPS.

## Collegamenti (Arduino Nano)

| Dispositivo  | Pin dispositivo | Pin Nano | Note                              |
|--------------|-----------------|----------|-----------------------------------|
| NEO-M8N      | TX              | D4       | SoftwareSerial RX                 |
| NEO-M8N      | RX              | D3       | SoftwareSerial TX                 |
| NEO-M8N      | PPS             | D2       | INT0                              |
| DS3231       | SDA             | A4       | I2C                               |
| DS3231       | SCL             | A5       | I2C                               |
| DS3231       | 32K             | D5       | Timer1 clock (T1), serve pull-up  |
| Fotocellula  | OUT             | D8       | ICP1, NPN NO, fronte di discesa   |

GND comune obbligatorio fra tutti i dispositivi. Verificare la tensione di
alimentazione del modulo GPS (3.3V sul chip nudo, 5V sulle breakout con regolatore).

### Interfacciamento fotocellula (Omron E3Z-T61, NPN open-collector, Dark-ON)

La E3Z-T61 ha uscita **NPN open-collector**: tira a massa quando il fascio è
interrotto (Dark-ON → fronte di discesa = evento catturato), altrimenti resta
in alta impedenza. Serve quindi una pull-up sull'uscita, e conviene
**rinforzare** quella interna dell'ATmega (`INPUT_PULLUP`, ~20-50 kΩ, debole)
con una pull-up esterna verso i +5V per fronti puliti e immunità al rumore su
cavi lunghi:

```
                       +5V (Arduino)
                        |
                      [4.7k]        <- pull-up esterna (fino a 1-2.2k se cavo lungo/rumoroso)
                        |
   E3Z nero (OUT) ------+-------- D8 (ICP1)
   E3Z marrone -- +12..24V (alim. sensore)
   E3Z blu ------- GND -- comune -- GND Arduino
```

- **CRITICO**: la pull-up va verso i **+5V**, MAI verso i 12-24V. L'uscita NPN
  tira solo a massa, quindi sul pin non arrivano mai piu' di 5V. (Un modello
  PNP, es. `-T81`, sorgerebbe 24V e richiederebbe partitore/optoisolatore.)
- Alimentare il sensore a 12-24V; **GND in comune** con l'Arduino.
- Si puo' lasciare `INPUT_PULLUP` nel firmware (le pull-up vanno in parallelo,
  domina l'esterna) oppure passare a `pinMode(8, INPUT)` e usare solo l'esterna.
- Opzionale, se resta rumore: filtro RC (1 kΩ in serie + 100 nF verso GND sul
  pin 8), in aggiunta al noise canceller del Timer1 gia' attivo.

## Protocollo seriale (115200 baud, USB-CDC, righe terminate da `\n`)

Protocollo terso a tag di singolo carattere: nessun invio ciclico, le
diagnostiche sono su richiesta (request/response), il passaggio è asincrono.

### Arduino → PC (asincrono / risposte)

| Msg     | Formato                         | Esempio                                            |
|---------|---------------------------------|----------------------------------------------------|
| Passage | `P<unix>.<ms><G\|R>`            | `P1718000000.347G` (G=GPS, R=RTC)                  |
| Pong    | `K<src>[<unix>.<ms>]`           | `KG1718000000.347` / `KN` (src `G`/`R`/`N`)        |
| Diag    | `D<CSV >=12 campi>`             | `D1718000000,G,-1,187.3,A,9,0.93,142,0,12,3,57,1,1718000000,1,1717999998` |
| Id      | `Y<ver>`                        | `Y2`                                               |
| Warn    | `W<nmea>,<ref>`                 | `W1718000001,1718000003` (evento desync)           |

Ordine colonne `D` (posizionale): `unix, src, dev_tcnt, nmea_ms, fix, sat,
hdop, alt, nmea_persi, since_pps_s, since_rtc_write_s, uptime_s`. Dalla versione
firmware `Y2` seguono 4 campi di **diagnostica estesa** (GPS e RTC assoluti,
raccolti solo alla ricezione di `@`, quindi solo a schermata diagnostica aperta):
`gps_ok, gps_unix, rtc_ok, rtc_unix` (`gps_ok`/`rtc_ok` = 0/1; unix in secondi
UTC, 0 se non valido). Il PC calcola da questi gli scarti GPS-RTC e rispetto al
proprio orologio; un firmware precedente non li invia (12 campi) e il PC mostra
"non disponibili".

- `src` — `G` (PPS), `R` (RTC), `N` (nessun tempo valido);
- `dev_tcnt` — deviazione di TCNT1 da 32768 al PPS (ideale 0 / +-1);
- `nmea_ms` — latenza dell'RMC dopo il PPS;
- `fix` — `A` valido / `V` non valido; `sat`; `hdop`; `alt` (metri);
- `nmea_persi` — contatore di secondi senza RMC valido;
- `since_pps_s` / `since_rtc_write_s` / `uptime_s` — secondi.

### PC → Arduino (1 byte + `\n`)

| Comando | Byte | Risposta              |
|---------|------|-----------------------|
| Ping    | `?`  | `K<src>[<unix>.<ms>]` |
| Diag    | `@`  | `D<...>`              |
| Id      | `#`  | `Y<ver>`              |

## Dipendenze (Library Manager)

- `RTClib` (Adafruit)
- `TinyGPSPlus` (Mikal Hart)
- `SoftwareSerial`, `Wire` (incluse nel core AVR)

## Verifica dei checksum NMEA

I comandi `$PUBX,40,...` configurano il NEO-M8N per emettere solo `RMC` (1 Hz) e
`GGA` (ogni 5 fix). Il checksum NMEA e' lo XOR di tutti i caratteri tra `$` e `*`.
Tutti i checksum nello sketch sono stati verificati; in particolare `GGA` a rate 5
risulta `*5F`. Per ricalcolarli:

```sh
python3 checksum_pubx.py
```

| Comando                          | Checksum |
|----------------------------------|----------|
| `PUBX,40,GLL,0,0,0,0,0,0`        | `5C`     |
| `PUBX,40,GSA,0,0,0,0,0,0`        | `4E`     |
| `PUBX,40,GSV,0,0,0,0,0,0`        | `59`     |
| `PUBX,40,VTG,0,0,0,0,0,0`        | `5E`     |
| `PUBX,40,RMC,0,1,0,0,0,0`        | `46`     |
| `PUBX,40,GGA,0,5,0,0,0,0`        | `5F`     |

## Punti che richiedono verifica sull'hardware reale

Non verificabili via software, da controllare al primo collaudo:

1. **Pin 32K del DS3231**: assicurarsi che la breakout lo esponga e che ci sia
   una pull-up (molte board ce l'hanno; in caso contrario 10 kOhm a Vcc).
   Verificare con oscilloscopio/frequenzimetro il segnale a 32.768 kHz su D5.
2. **PPS del NEO-M8N**: attivo solo con fix valido. Senza impulsi su D2,
   probabilmente il GPS non ha ancora il fix (non e' un bug del codice).
3. **Scenari di boot** da provare: (a) RTC programmato + fix; (b) RTC programmato
   + no fix; (c) RTC vergine + fix; (d) RTC vergine + no fix.
4. **Day-of-week**: si scrive con la convenzione RTClib (domenica=7). Il registro
   DOW non incide sul calendario (RTClib ricalcola il giorno dalla data).
