# Integrazione hardware "Clock interno" — Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Affiancare il cronometro GPS (`arduino/cronometro_gps`) al sistema legacy, con selettore Legacy/Clock-interno, protocollo seriale terso ping/pong (timestamp nel pong), label a 5 stati e modal diagnostica, su brrm e brrm-partenza.

**Architecture:** Helper puri (time + protocollo) nei moduli condivisi `brrm-utils` (symlinkati e unit-testati in `brrm-utils-test`); lo stato seriale e la UI restano in `FMain`/forms di ciascuna app; il firmware passa da free-running a evento+request/response.

**Tech Stack:** Gambas 3 / Qt6 (`gb.test` per gli unit test), Arduino C++ (ATmega328P), seriale USB-CDC 115200.

**Spec di riferimento:** `docs/superpowers/specs/2026-06-02-hardware-clock-integration-design.md`

**Branch:** `feature/hardware-clock` (già attivo).

## Convenzioni del repo (leggere prima di iniziare)

- I moduli condivisi vivono in `brrm-utils/.src/` e sono **symlinkati** in `brrm/.src/`, `brrm-partenza/.src/`, `brrm-utils-test/.src/`. Si crea il symlink con `ln -s ../../brrm-utils/.src/NOME brrm/.src/NOME`. Git li versiona come symlink (mode 120000).
- **Bug Gambas 3.20**: le `Private Function` in un modulo acceduto via symlink danno risultati errati. Nei moduli condivisi usare **solo funzioni `Public`** e inlinare le validazioni (vedi `MAlignSync.DecodeStateFile`).
- Niente `Float` nelle conversioni temporali: aritmetica intera (`\` divisione intera, `Mod` modulo).
- Unit test: `bash dev/run-tests.sh` (Linux con `gbc3`/`gbx3`) o `bash dev/run-tests-docker.sh` (Mac). API asserzioni: `Assert.Equals(actual, expected, msg)`. Il totale va dichiarato in `brrm-utils-test/.src/Main.module` con `Test.Plan(N)` e ogni modulo test espone `RunAll()`.
- Build app: `bash build.sh` (compila brrm + brrm-partenza con `gbc3`).

## File Structure

| File | Responsabilità | Azione |
|---|---|---|
| `brrm-utils/.src/MTime.module` | aggiunge inverso unix→Date | Modify |
| `brrm-utils/.src/MClock.module` | parsing protocollo + stato label (puro) | Create |
| `brrm-utils-test/.src/TestCivilFromDays.module` | test `CivilFromDays` | Create |
| `brrm-utils-test/.src/TestUnixMsToDate.module` | test `UnixMsToDateTZ` | Create |
| `brrm-utils-test/.src/TestMClock.module` | test parsing + label state | Create |
| `brrm-utils-test/.src/Main.module` | registra i nuovi RunAll + Plan | Modify |
| `brrm-utils-test/.src/MClock.module` | symlink a brrm-utils | Create (symlink) |
| `arduino/cronometro_gps/cronometro_gps.ino` | protocollo evento+request/response | Modify |
| `brrm/.src/MClock.module` | symlink | Create (symlink) |
| `brrm/.src/FMain.class` | mode, parsing internal, healthcheck, label, click | Modify |
| `brrm/.src/FMain.form` | colori/click label (se serve Button) | Modify |
| `brrm/.src/FClockDiag.class` + `.form` | modal diagnostica | Create |
| `brrm/.src/FSettings.class` + `.form` | tab Hardware | Modify |
| `brrm-partenza/.src/*` | mirror: symlink MClock/FClockDiag, FMain, FSettings | Modify/Create |

---

## FASE 1 — MTime: inverso unix→Date (puro, TDD)

### Task 1: `MTime.CivilFromDays` (inverso di `DaysFromEpoch`)

**Files:**
- Modify: `brrm-utils/.src/MTime.module` (append)
- Create: `brrm-utils-test/.src/TestCivilFromDays.module`
- Modify: `brrm-utils-test/.src/Main.module`

- [ ] **Step 1: Scrivi il test che fallisce**

Crea `brrm-utils-test/.src/TestCivilFromDays.module`:

```basic
' Test per MTime.CivilFromDays — inverso di DaysFromEpoch (Hinnant
' days_from_civil). Vettori verificati con datetime Python (UTC).
Public Sub RunAll()
  Dim r As Integer[]
  r = MTime.CivilFromDays(0)
  Assert.Equals(r[0], 1970, "day 0 -> anno 1970")
  Assert.Equals(r[1], 1,    "day 0 -> mese 1")
  Assert.Equals(r[2], 1,    "day 0 -> giorno 1")
  r = MTime.CivilFromDays(10957)
  Assert.Equals(r[0], 2000, "day 10957 -> 2000")
  Assert.Equals(r[1], 1,    "day 10957 -> mese 1")
  Assert.Equals(r[2], 1,    "day 10957 -> giorno 1")
  r = MTime.CivilFromDays(19723)
  Assert.Equals(r[0], 2024, "day 19723 -> 2024")
  Assert.Equals(r[1], 1,    "day 19723 -> mese 1")
  Assert.Equals(r[2], 1,    "day 19723 -> giorno 1")
  r = MTime.CivilFromDays(19884)
  Assert.Equals(r[0], 2024, "day 19884 -> 2024")
  Assert.Equals(r[1], 6,    "day 19884 -> mese 6")
  Assert.Equals(r[2], 10,   "day 19884 -> giorno 10")
End
```

In `brrm-utils-test/.src/Main.module`: cambia `Test.Plan(126)` in `Test.Plan(138)` e aggiungi dopo `TestOrarioWire.RunAll()` la riga `TestCivilFromDays.RunAll()`.

- [ ] **Step 2: Esegui i test, verifica il fallimento**

Run: `bash dev/run-tests.sh` (o `dev/run-tests-docker.sh` su Mac)
Expected: FAIL — compile error "Unknown identifier: CivilFromDays" oppure assert non eseguiti.

- [ ] **Step 3: Implementa `CivilFromDays`**

Appendi a `brrm-utils/.src/MTime.module`:

```basic
Public Function CivilFromDays(days As Long) As Integer[]
  ' Inverso di DaysFromEpoch (Howard Hinnant civil_from_days). Int puro.
  ' days = giorni dal 1970-01-01. Ritorna [anno, mese, giorno].
  Dim z As Long = days + 719468
  Dim era As Long
  If z >= 0 Then era = z \ 146097 Else era = (z - 146096) \ 146097
  Dim doe As Long = z - era * 146097
  Dim yoe As Long = (doe - doe \ 1460 + doe \ 36524 - doe \ 146096) \ 365
  Dim y As Long = yoe + era * 400
  Dim doy As Long = doe - (365 * yoe + yoe \ 4 - yoe \ 100)
  Dim mp As Long = (5 * doy + 2) \ 153
  Dim d As Long = doy - (153 * mp + 2) \ 5 + 1
  Dim m As Long
  If mp < 10 Then m = mp + 3 Else m = mp - 9
  If m <= 2 Then y = y + 1
  Return [CInt(y), CInt(m), CInt(d)]
End
```

- [ ] **Step 4: Esegui i test, verifica il successo**

Run: `bash dev/run-tests.sh`
Expected: PASS — `PASS: 138/138 test.`

- [ ] **Step 5: Commit**

```bash
git add brrm-utils/.src/MTime.module brrm-utils-test/.src/TestCivilFromDays.module brrm-utils-test/.src/Main.module
git commit -m "MTime: aggiunge CivilFromDays (inverso di DaysFromEpoch)"
```

### Task 2: `MTime.UnixMsToDateTZ` / `UnixMsToDate`

**Files:**
- Modify: `brrm-utils/.src/MTime.module` (append)
- Create: `brrm-utils-test/.src/TestUnixMsToDate.module`
- Modify: `brrm-utils-test/.src/Main.module`

- [ ] **Step 1: Scrivi il test che fallisce**

Crea `brrm-utils-test/.src/TestUnixMsToDate.module` (tz esplicito = 0 → UTC, deterministico):

```basic
' Test per MTime.UnixMsToDateTZ — unix ms (UTC) -> Date locale, con
' offset di fuso esplicito (tz in secondi a ovest di UTC, POSIX) per
' determinismo. Vettori verificati con datetime Python (UTC).
Public Sub RunAll()
  Dim d As Date
  ' epoch 0 UTC = 1970-01-01 00:00:00.000
  d = MTime.UnixMsToDateTZ(0, 0)
  Assert.Equals(Year(d),  1970, "epoch0 anno")
  Assert.Equals(Month(d), 1,    "epoch0 mese")
  Assert.Equals(Day(d),   1,    "epoch0 giorno")
  Assert.Equals(Hour(d),  0,    "epoch0 ora")
  Assert.Equals(Minute(d),0,    "epoch0 minuto")
  Assert.Equals(Second(d),0,    "epoch0 secondo")
  Assert.Equals(MTime.MillisOf(d), 0, "epoch0 ms")
  ' 1718000000.347 UTC = 2024-06-10 06:13:20.347
  d = MTime.UnixMsToDateTZ(1718000000347, 0)
  Assert.Equals(Year(d),  2024, "v1 anno")
  Assert.Equals(Month(d), 6,    "v1 mese")
  Assert.Equals(Day(d),   10,   "v1 giorno")
  Assert.Equals(Hour(d),  6,    "v1 ora")
  Assert.Equals(Minute(d),13,   "v1 minuto")
  Assert.Equals(Second(d),20,   "v1 secondo")
  Assert.Equals(MTime.MillisOf(d), 347, "v1 ms")
  ' 1735689599000 UTC = 2024-12-31 23:59:59.000
  d = MTime.UnixMsToDateTZ(1735689599000, 0)
  Assert.Equals(Year(d),  2024, "v2 anno")
  Assert.Equals(Month(d), 12,   "v2 mese")
  Assert.Equals(Day(d),   31,   "v2 giorno")
  Assert.Equals(Hour(d),  23,   "v2 ora")
  Assert.Equals(Minute(d),59,   "v2 minuto")
  Assert.Equals(Second(d),59,   "v2 secondo")
  Assert.Equals(MTime.MillisOf(d), 0, "v2 ms")
  ' offset CEST: tz = -7200 (2h a est) -> epoch0 locale = 02:00
  d = MTime.UnixMsToDateTZ(0, -7200)
  Assert.Equals(Year(d), 1970, "tz anno")
  Assert.Equals(Day(d),  1,    "tz giorno")
  Assert.Equals(Hour(d), 2,    "tz ora (UTC+2)")
End
```

In `Main.module`: `Test.Plan(138)` → `Test.Plan(162)`; aggiungi `TestUnixMsToDate.RunAll()`.

- [ ] **Step 2: Esegui, verifica fallimento**

Run: `bash dev/run-tests.sh`
Expected: FAIL — "Unknown identifier: UnixMsToDateTZ".

- [ ] **Step 3: Implementa**

Appendi a `brrm-utils/.src/MTime.module`:

```basic
Public Function UnixMsToDateTZ(unixMs As Long, tzSec As Long) As Date
  ' Inverso di DateToUnixMs con fuso esplicito. tzSec = secondi a ovest
  ' di UTC (POSIX, come System.TimeZone): locale = UTC - tzSec.
  Dim localMs As Long = unixMs - tzSec * 1000
  Dim days As Long = localMs \ 86400000
  Dim msInDay As Long = localMs Mod 86400000
  If msInDay < 0 Then
    msInDay = msInDay + 86400000
    days = days - 1
  Endif
  Dim ymd As Integer[] = CivilFromDays(days)
  Dim hh As Integer = CInt(msInDay \ 3600000)
  Dim mm As Integer = CInt((msInDay \ 60000) Mod 60)
  Dim ss As Integer = CInt((msInDay \ 1000) Mod 60)
  Dim uuu As Integer = CInt(msInDay Mod 1000)
  Return Date(ymd[0], ymd[1], ymd[2], hh, mm, ss, uuu)
End

Public Function UnixMsToDate(unixMs As Long) As Date
  ' Wrapper col fuso di sistema corrente.
  Return UnixMsToDateTZ(unixMs, CLong(System.TimeZone))
End
```

- [ ] **Step 4: Esegui, verifica successo**

Run: `bash dev/run-tests.sh`
Expected: PASS — `PASS: 162/162 test.`

- [ ] **Step 5: Commit**

```bash
git add brrm-utils/.src/MTime.module brrm-utils-test/.src/TestUnixMsToDate.module brrm-utils-test/.src/Main.module
git commit -m "MTime: aggiunge UnixMsToDateTZ/UnixMsToDate (unix ms -> Date locale)"
```

---

## FASE 2 — MClock: parsing protocollo + stato label (puro, TDD)

### Task 3: `MClock.ParseLine` + `IsAllDigits` + costanti

**Files:**
- Create: `brrm-utils/.src/MClock.module`
- Create (symlink): `brrm-utils-test/.src/MClock.module`, `brrm/.src/MClock.module`, `brrm-partenza/.src/MClock.module`
- Create: `brrm-utils-test/.src/TestMClock.module`
- Modify: `brrm-utils-test/.src/Main.module`

- [ ] **Step 1: Crea il modulo e i symlink**

Crea `brrm-utils/.src/MClock.module` con lo scheletro (le funzioni vere arrivano allo Step 3):

```basic
' MClock — parser del protocollo seriale "clock interno" e calcolo dello
' stato label. Funzioni pure, unit-testabili. Source-of-truth: gli altri
' progetti puntano qui via symlink. Solo Public (bug Gambas 3.20 sulle
' Private function nei moduli symlinkati).

' Comandi PC -> Arduino (1 byte + newline)
Public Const CMD_PING As String = "?"
Public Const CMD_DIAG As String = "@"
Public Const CMD_ID As String = "#"

' Stati label (modalita' internal)
Public Const HW_ABSENT As Integer = 0
Public Const HW_UNRESPONSIVE As Integer = 1
Public Const HW_GPS As Integer = 2
Public Const HW_RTC As Integer = 3
Public Const HW_NOSYNC As Integer = 4

Public Const PONG_TIMEOUT_MS As Long = 3000

Public Function IsAllDigits(s As String) As Boolean
  If s = "" Then Return False
  Dim i As Integer
  Dim c As Integer
  For i = 1 To Len(s)
    c = Asc(Mid$(s, i, 1))
    If c < Asc("0") Or If c > Asc("9") Then Return False
  Next
  Return True
End
```

Crea i symlink (source-of-truth = brrm-utils):

```bash
ln -s ../../brrm-utils/.src/MClock.module brrm-utils-test/.src/MClock.module
ln -s ../../brrm-utils/.src/MClock.module brrm/.src/MClock.module
ln -s ../../brrm-utils/.src/MClock.module brrm-partenza/.src/MClock.module
```

- [ ] **Step 2: Scrivi i test che falliscono**

Crea `brrm-utils-test/.src/TestMClock.module`:

```basic
' Test per MClock.ParseLine / IsAllDigits / LabelState.
Public Sub RunAll()
  Dim r As String[]

  ' --- IsAllDigits ---
  Assert.Equals(MClock.IsAllDigits("123"), True,  "digits ok")
  Assert.Equals(MClock.IsAllDigits("12a"), False, "non-digit")
  Assert.Equals(MClock.IsAllDigits(""),    False, "vuoto")

  ' --- Passage P<unix>.<ms><src> ---
  r = MClock.ParseLine("P1718000000.347G")
  Assert.Equals(r[0], "P",          "P kind")
  Assert.Equals(r[1], "1718000000", "P unix")
  Assert.Equals(r[2], "347",        "P ms")
  Assert.Equals(r[3], "G",          "P src G")
  r = MClock.ParseLine("P1718000000.000R")
  Assert.Equals(r[3], "R", "P src R")
  Assert.Equals(IsNull(MClock.ParseLine("P1718000000.347X")), True, "P src invalido -> Null")
  Assert.Equals(IsNull(MClock.ParseLine("P17a8000000.347G")), True, "P unix non-num -> Null")

  ' --- Pong K<src>[<unix>.<ms>] ---
  r = MClock.ParseLine("KG1718000000.347")
  Assert.Equals(r[0], "K",          "K kind")
  Assert.Equals(r[1], "G",          "K src")
  Assert.Equals(r[2], "1718000000", "K unix")
  Assert.Equals(r[3], "347",        "K ms")
  r = MClock.ParseLine("KN")
  Assert.Equals(r[0], "K", "KN kind")
  Assert.Equals(r[1], "N", "KN src")
  Assert.Equals(r.Count, 2, "KN senza timestamp")

  ' --- Diag D<csv 12 campi> ---
  r = MClock.ParseLine("D1718000000,G,-1,187.3,A,9,0.93,142,0,12,3,57")
  Assert.Equals(r[0], "D",          "D kind")
  Assert.Equals(r.Count, 13,        "D = kind + 12 campi")
  Assert.Equals(r[1], "1718000000", "D unix")
  Assert.Equals(r[5], "A",          "D fix")
  Assert.Equals(IsNull(MClock.ParseLine("D1,2,3")), True, "D campi insufficienti -> Null")

  ' --- Id Y<ver> ---
  r = MClock.ParseLine("Y1")
  Assert.Equals(r[0], "Y", "Y kind")
  Assert.Equals(r[1], "1", "Y ver")

  ' --- Warn W<nmea>,<ref> ---
  r = MClock.ParseLine("W123,456")
  Assert.Equals(r[0], "W",   "W kind")
  Assert.Equals(r[1], "123", "W nmea")
  Assert.Equals(r[2], "456", "W ref")

  ' --- malformati ---
  Assert.Equals(IsNull(MClock.ParseLine("")),   True, "vuoto -> Null")
  Assert.Equals(IsNull(MClock.ParseLine("Z9")), True, "tag ignoto -> Null")
  Assert.Equals(IsNull(MClock.ParseLine("P")),  True, "P vuoto -> Null")
End
```

In `Main.module`: `Test.Plan(162)` → `Test.Plan(188)`; aggiungi `TestMClock.RunAll()`.

- [ ] **Step 3: Implementa `ParseLine`**

Appendi a `brrm-utils/.src/MClock.module`:

```basic
Public Function ParseLine(sLine As String) As String[]
  ' Parsa una riga del protocollo. Dispatch sul primo byte. Ritorna un
  ' String[] [kind, campi...] oppure Null se malformata. Validazione
  ' inlinata (no Private function: bug Gambas 3.20 sui symlink).
  sLine = Trim$(sLine)
  If sLine = "" Then Return Null
  Dim tag As String = Left$(sLine, 1)
  Dim rest As String = Mid$(sLine, 2)
  Dim parts As String[]

  Select Case tag
    Case "P"   ' P<unix>.<ms><src>, src = ultimo char (G|R), ms 3 cifre
      If Len(rest) < 6 Then Return Null
      Dim src As String = Right$(rest, 1)
      If src <> "G" And If src <> "R" Then Return Null
      Dim body As String = Left$(rest, Len(rest) - 1)
      Dim dot As Integer = InStr(body, ".")
      If dot < 2 Then Return Null
      Dim unixp As String = Left$(body, dot - 1)
      Dim msp As String = Mid$(body, dot + 1)
      If Not IsAllDigits(unixp) Then Return Null
      If Len(msp) <> 3 Or If Not IsAllDigits(msp) Then Return Null
      Return [tag, unixp, msp, src]

    Case "K"   ' K<src>[<unix>.<ms>]
      If rest = "" Then Return Null
      Dim ks As String = Left$(rest, 1)
      If ks <> "G" And If ks <> "R" And If ks <> "N" Then Return Null
      Dim ktail As String = Mid$(rest, 2)
      If ktail = "" Then Return [tag, ks]
      Dim kdot As Integer = InStr(ktail, ".")
      If kdot < 2 Then Return Null
      Dim ku As String = Left$(ktail, kdot - 1)
      Dim km As String = Mid$(ktail, kdot + 1)
      If Not IsAllDigits(ku) Then Return Null
      If Len(km) <> 3 Or If Not IsAllDigits(km) Then Return Null
      Return [tag, ks, ku, km]

    Case "D"   ' D<csv 12 campi>
      parts = Split(rest, ",")
      If parts.Count <> 12 Then Return Null
      Dim outd As New String[]
      outd.Add(tag)
      Dim f As String
      For Each f In parts
        outd.Add(f)
      Next
      Return outd

    Case "Y"   ' Y<ver>
      If rest = "" Then Return Null
      Return [tag, rest]

    Case "W"   ' W<nmea>,<ref>
      parts = Split(rest, ",")
      If parts.Count <> 2 Then Return Null
      Return [tag, parts[0], parts[1]]

    Case Else
      Return Null
  End Select
End
```

- [ ] **Step 4: Esegui, verifica successo**

Run: `bash dev/run-tests.sh`
Expected: PASS — `PASS: 188/188 test.`

- [ ] **Step 5: Commit**

```bash
git add brrm-utils/.src/MClock.module brrm-utils-test/.src/MClock.module brrm/.src/MClock.module brrm-partenza/.src/MClock.module brrm-utils-test/.src/TestMClock.module brrm-utils-test/.src/Main.module
git commit -m "MClock: modulo condiviso + ParseLine protocollo terso + symlink"
```

### Task 4: `MClock.LabelState`

**Files:**
- Modify: `brrm-utils/.src/MClock.module` (append)
- Modify: `brrm-utils-test/.src/TestMClock.module` (append asserzioni a RunAll)
- Modify: `brrm-utils-test/.src/Main.module` (Plan)

- [ ] **Step 1: Scrivi i test che falliscono**

In `TestMClock.module`, prima dell'ultima `End` di `RunAll()`, aggiungi:

```basic
  ' --- LabelState(devicePresent, portOpen, lastPongAgeMs, src) ---
  Assert.Equals(MClock.LabelState(False, False, 0,    "G"), MClock.HW_ABSENT,       "no device -> ABSENT")
  Assert.Equals(MClock.LabelState(True,  False, 0,    "G"), MClock.HW_ABSENT,       "porta chiusa -> ABSENT")
  Assert.Equals(MClock.LabelState(True,  True,  5000, "G"), MClock.HW_UNRESPONSIVE, "pong vecchio -> UNRESPONSIVE")
  Assert.Equals(MClock.LabelState(True,  True,  500,  ""),  MClock.HW_UNRESPONSIVE, "src vuoto -> UNRESPONSIVE")
  Assert.Equals(MClock.LabelState(True,  True,  500,  "G"), MClock.HW_GPS,          "src G -> GPS")
  Assert.Equals(MClock.LabelState(True,  True,  500,  "R"), MClock.HW_RTC,          "src R -> RTC")
  Assert.Equals(MClock.LabelState(True,  True,  500,  "N"), MClock.HW_NOSYNC,       "src N -> NOSYNC")
```

In `Main.module`: `Test.Plan(188)` → `Test.Plan(195)`.

- [ ] **Step 2: Esegui, verifica fallimento**

Run: `bash dev/run-tests.sh`
Expected: FAIL — "Unknown identifier: LabelState".

- [ ] **Step 3: Implementa**

Appendi a `brrm-utils/.src/MClock.module`:

```basic
Public Function LabelState(devicePresent As Boolean, portOpen As Boolean, lastPongAgeMs As Long, src As String) As Integer
  If Not devicePresent Or If Not portOpen Then Return HW_ABSENT
  If lastPongAgeMs > PONG_TIMEOUT_MS Then Return HW_UNRESPONSIVE
  Select Case src
    Case "G"
      Return HW_GPS
    Case "R"
      Return HW_RTC
    Case "N"
      Return HW_NOSYNC
    Case Else
      Return HW_UNRESPONSIVE
  End Select
End
```

- [ ] **Step 4: Esegui, verifica successo**

Run: `bash dev/run-tests.sh`
Expected: PASS — `PASS: 195/195 test.`

- [ ] **Step 5: Commit**

```bash
git add brrm-utils/.src/MClock.module brrm-utils-test/.src/TestMClock.module brrm-utils-test/.src/Main.module
git commit -m "MClock: aggiunge LabelState (5 stati) + test"
```

---

## FASE 3 — Firmware: evento + request/response

> Nessun harness di unit test per il firmware. Verifica manuale con monitor seriale a 115200 (es. `picocom -b 115200 /dev/ttyUSB0` o l'IDE Arduino). Se disponibile, compilare con `arduino-cli compile --fqbn arduino:avr:nano arduino/cronometro_gps`.

### Task 5: PASSAGGIO taggato + helper tempo assoluto + stop emissione ciclica

**Files:**
- Modify: `arduino/cronometro_gps/cronometro_gps.ino`

- [ ] **Step 1: Aggiungi la sorgente alla cattura**

Nella sezione variabili cattura, aggiungi accanto a `cattura_timer`:

```cpp
volatile uint8_t cattura_fonte = 'R';   // 'G' (PPS) o 'R' (RTC) al momento della cattura
```

Nella `ISR(TIMER1_CAPT_vect)`, dopo `unix_cattura = snap;` e prima di `evento_fotocellula = true;`, aggiungi:

```cpp
  cattura_fonte = (stato_attuale == PPS_ACTIVE) ? 'G' : 'R';
```

- [ ] **Step 2: Helper per stampare il tempo assoluto (riuso PASSAGGIO + PONG)**

Aggiungi (prima di `loop()`) una funzione che stampa `<unix>.<ms>` dato secondo/frazione, senza newline:

```cpp
// Stampa "<unix>.<ms a 3 cifre>" su Serial (no newline). Applica la
// saturazione frazione>=32768 come la stampa del passaggio.
void stampaTempo(uint32_t secondo, uint16_t frazione) {
  while (frazione >= 32768) { frazione -= 32768; secondo += 1; }
  uint16_t ms = (uint16_t)(frazione / 32.768f);
  if (ms > 999) ms = 999;
  Serial.print(secondo);
  Serial.print('.');
  if (ms < 100) Serial.print('0');
  if (ms < 10)  Serial.print('0');
  Serial.print(ms);
}
```

- [ ] **Step 3: PASSAGGIO compatto e taggato**

Nel blocco `if (evento_fotocellula)` di `loop()`, sostituisci la stampa attuale del passaggio (da `Serial.print(F("PASSAGGIO:"))` fino a `Serial.println(ms);`) con:

```cpp
      noInterrupts();
      uint8_t fonte = cattura_fonte;
      interrupts();
      Serial.print('P');
      stampaTempo(secondo, frazione);
      Serial.println((char) fonte);
```

(`secondo`/`frazione` sono già letti atomicamente sopra; rimuovi il vecchio calcolo `ms` duplicato se ora inutilizzato, dato che `stampaTempo` lo rifà.)

- [ ] **Step 4: Rimuovi l'emissione ciclica della diagnostica**

In `loop()` elimina i due blocchi che chiamano `emettiDiagnostica(...)`:
- il blocco `if (pps_appena_arrivato) { ... emettiDiagnostica(true); }`
- il blocco heartbeat `if (pps_assente && ...) { emettiDiagnostica(false); ... }`

Mantieni `emettiDiagnostica` definita (verrà richiamata su comando nel Task 6) ma adattane la firma nel Task 6. `pps_appena_arrivato` può restare settata nell'ISR (innocua).

- [ ] **Step 5: Verifica manuale + commit**

Collega l'hardware, apri il monitor a 115200. Atteso: niente più righe `DIAG:` cicliche; al passaggio fotocellula esce `P<unix>.<ms>G` (o `R`).

```bash
git add arduino/cronometro_gps/cronometro_gps.ino
git commit -m "firmware: PASSAGGIO compatto taggato (G/R) + stop emissione ciclica DIAG"
```

### Task 6: Lettore comandi `?`/`@`/`#` + PONG con timestamp + DIAG on-demand arricchito + ID

**Files:**
- Modify: `arduino/cronometro_gps/cronometro_gps.ino`

- [ ] **Step 1: Funzione che fornisce la sorgente corrente come char**

Aggiungi prima di `loop()`:

```cpp
char fonteCorrente() {
  if (!tempo_inizializzato) return 'N';
  return (stato_attuale == PPS_ACTIVE) ? 'G' : 'R';
}
```

- [ ] **Step 2: Risposte PONG / ID e DIAG on-demand**

Aggiungi le funzioni di risposta. PONG include il timestamp live (omesso se `N`):

```cpp
void inviaPong() {
  char s = fonteCorrente();
  Serial.print('K');
  Serial.print(s);
  if (s != 'N') {
    noInterrupts();
    uint32_t sec = unix_riferimento;
    uint16_t fr  = TCNT1;
    interrupts();
    stampaTempo(sec, fr);
  }
  Serial.println();
}

void inviaId() {
  Serial.println(F("Y1"));
}
```

Riscrivi `emettiDiagnostica` come `inviaDiag()` compatta (tag `D`, CSV posizionale, 12 campi nell'ordine dello spec, con i 3 campi nuovi):

```cpp
void inviaDiag() {
  noInterrupts();
  uint32_t secondo   = unix_riferimento;
  uint16_t tcnt_diag = diag_tcnt_al_pps;
  uint16_t persi     = contatore_nmea_persi;
  unsigned long tpps = ultimo_pps_ms;
  unsigned long trtc = ultimo_aggiornamento_rtc_ms;
  interrupts();
  unsigned long ora = millis();
  Serial.print('D');
  Serial.print(secondo);                 Serial.print(',');
  Serial.print(fonteCorrente());         Serial.print(',');
  Serial.print((int16_t) tcnt_diag - 32768); Serial.print(',');
  Serial.print(ultimo_ritardo_ticks / 32.768f, 1); Serial.print(',');
  Serial.print(gps.location.isValid() ? 'A' : 'V'); Serial.print(',');
  Serial.print(gps.satellites.isValid() ? gps.satellites.value() : 0); Serial.print(',');
  if (gps.hdop.isValid()) Serial.print(gps.hdop.hdop(), 2); else Serial.print(F("0")); Serial.print(',');
  if (gps.altitude.isValid()) Serial.print(gps.altitude.meters(), 0); else Serial.print(F("0")); Serial.print(',');
  Serial.print(persi);                   Serial.print(',');
  Serial.print((ora - tpps) / 1000);     Serial.print(',');                 // since_pps_s
  Serial.print(trtc == 0 ? 0 : (ora - trtc) / 1000); Serial.print(',');     // since_rtc_write_s
  Serial.println(ora / 1000);                                               // uptime_s
}
```

(Nota: i campi `hdop`/`alt` mancanti diventano `0` invece di `--` per mantenere il CSV a 12 campi numerici parsabili.)

- [ ] **Step 3: Reader dei comandi in `loop()`**

Aggiungi una variabile globale buffer:

```cpp
char cmdBuf[8];
uint8_t cmdLen = 0;
```

In cima a `loop()`, aggiungi la gestione comandi (USB `Serial`):

```cpp
  while (Serial.available() > 0) {
    char c = Serial.read();
    if (c == '\n' || c == '\r') {
      if (cmdLen > 0) {
        char cmd = cmdBuf[0];
        if      (cmd == '?') inviaPong();
        else if (cmd == '@') inviaDiag();
        else if (cmd == '#') inviaId();
        cmdLen = 0;
      }
    } else if (cmdLen < sizeof(cmdBuf) - 1) {
      cmdBuf[cmdLen++] = c;
    }
  }
```

- [ ] **Step 4: Mantieni i WARN tersi**

Nel sanity check NMEA, sostituisci la stampa `WARN: desync ...` con la forma compatta:

```cpp
            Serial.print('W');
            Serial.print(t_nmea);
            Serial.print(',');
            Serial.println(attuale);
```

(Rimuovi le righe `INFO:` di boot/bootstrap, oppure lasciale: brrm le ignora perché `ParseLine` scarta i tag ignoti. Per pulizia, rimuoverle.)

- [ ] **Step 5: Verifica manuale + commit**

Monitor a 115200: inviando `?\n` deve rispondere `KG1718...` (o `KN`); `@\n` → `D...,...` (12 campi); `#\n` → `Y1`.

```bash
git add arduino/cronometro_gps/cronometro_gps.ino
git commit -m "firmware: comandi ?/@/# con PONG+timestamp, DIAG on-demand arricchito, ID"
```

---

## FASE 4 — brrm: integrazione FMain

### Task 7: Modalità hardware + baud + `HardwareReinit`

**Files:**
- Modify: `brrm/.src/FMain.class`

- [ ] **Step 1: Campi di stato e caricamento config**

Aggiungi tra le dichiarazioni private (vicino a `$hSerial`):

```basic
' --- Hardware clock (modalita' internal) ---
Private $hwMode As String           ' "legacy" | "internal"
Private $serialBuf As String        ' buffer di riga (modalita' internal)
Private $lastPongMs As Long         ' millis-equivalente (Timer) ultimo pong
Public ClockSrc As String           ' "G"/"R"/"N"/"" (per label e modal)
Public ClockTimeLive As String      ' "<unix>.<ms>" dall'ultimo pong
Public ClockDiagRaw As String       ' ultima riga "D..." ricevuta
Public ClockFwVer As String         ' versione firmware ("Y<ver>")
Public ClockDevice As String        ' device path corrente
```

Aggiungi la sub di caricamento (mode default `legacy`):

```basic
Public Sub LoadHardwareConfig()
  Dim sPath As String = User.Home &/ ".config/brrm/hardware.conf"
  Dim sLine, sKey, sVal As String
  Dim iEq As Integer
  $hwMode = "legacy"
  If Not Exist(sPath) Then Return
  For Each sLine In Split(File.Load(sPath), gb.NewLine)
    sLine = Trim$(sLine)
    If sLine = "" Or If Left$(sLine, 1) = "#" Then Continue
    iEq = InStr(sLine, "=")
    If iEq < 2 Then Continue
    sKey = LCase$(Trim$(Left$(sLine, iEq - 1)))
    sVal = LCase$(Trim$(Mid$(sLine, iEq + 1)))
    If sKey = "mode" And If sVal = "internal" Then $hwMode = "internal"
  Next
End
```

Chiama `LoadHardwareConfig()` in `Form_Open` prima dell'avvio dei timer Arduino (cerca dove parte `TimerArduino`/`AlignInit()`; mettila lì vicino).

- [ ] **Step 2: Baud per modalità in `ArduinoOpen`**

In `ArduinoOpen`, sostituisci `$hSerial.Speed = 9600` con:

```basic
  If $hwMode = "internal" Then
    $hSerial.Speed = 115200
  Else
    $hSerial.Speed = 9600
  Endif
  $serialBuf = ""
  ClockSrc = ""
```

- [ ] **Step 3: `HardwareReinit` (chiamata da FSettings)**

Aggiungi:

```basic
Public Sub HardwareReinit()
  ' Rilegge la modalita', resetta lo stato e forza la riconnessione al
  ' baud corretto. La label viene ricalcolata al prossimo TimerArduino.
  LoadHardwareConfig()
  ClockSrc = ""
  ClockDiagRaw = ""
  ClockFwVer = ""
  $lastPongMs = 0
  ArduinoClose()
  If $hwMode = "internal" Then
    TimerArduino.Delay = 1000
  Else
    TimerArduino.Delay = 2000
  Endif
End
```

- [ ] **Step 4: Compila**

Run: `bash build.sh`
Expected: `OK -> brrm/brrm.gambas` (brrm-partenza può ancora fallire se MClock non symlinkato lì — lo è dal Task 3, quindi entrambi OK).

- [ ] **Step 5: Commit**

```bash
git add brrm/.src/FMain.class
git commit -m "brrm/FMain: modalita' hardware, baud per modalita', HardwareReinit"
```

### Task 8: `Arduino_Read` — parsing internal + arrivo dal tempo Arduino

**Files:**
- Modify: `brrm/.src/FMain.class`

- [ ] **Step 1: Branch su modalità in `Arduino_Read`**

Dopo aver letto `s` dalla seriale (dove oggi c'è il calcolo di `sNow`), sostituisci il corpo che gestisce il passaggio con un branch:

```basic
  If $hwMode = "internal" Then
    HandleInternalBytes(s)
    Return
  Endif
  ' --- legacy: identico a prima ---
  Dim sNow As String
  sNow = Format$(Time, "hh:nn:ss.uuu")
  If FotON = TRUE THEN
    BArrivi.Add(sNow)
    Try MLog.Write("fotocellula", "source=arduino orario=" & sNow)
  Else
    Try MLog.Write("fotocellula_ignorata", "source=arduino motivo=FotOFF orario=" & sNow)
  Endif
```

- [ ] **Step 2: Buffer di riga + dispatch**

Aggiungi:

```basic
Private Sub HandleInternalBytes(sChunk As String)
  Dim iNl As Integer
  Dim sLine As String
  $serialBuf &= sChunk
  Do
    iNl = InStr($serialBuf, "\n")
    If iNl = 0 Then Break
    sLine = Left$($serialBuf, iNl - 1)
    $serialBuf = Mid$($serialBuf, iNl + 1)
    DispatchClockLine(Trim$(sLine))
  Loop
  ' Protezione anti-crescita se non arrivano newline
  If Len($serialBuf) > 256 Then $serialBuf = ""
End

Private Sub DispatchClockLine(sLine As String)
  Dim r As String[] = MClock.ParseLine(sLine)
  If r = Null Then Return
  Select Case r[0]
    Case "P"
      HandlePassage(r)
    Case "K"
      $lastPongMs = MTimerNowMs()
      ClockSrc = r[1]
      If r.Count >= 4 Then ClockTimeLive = r[2] & "." & r[3] Else ClockTimeLive = ""
    Case "D"
      ClockDiagRaw = sLine
    Case "Y"
      ClockFwVer = r[1]
    Case "W"
      Try MLog.Write("clock_desync", "nmea=" & r[1] & " ref=" & r[2])
  End Select
End

Private Function MTimerNowMs() As Long
  ' Riferimento monotono in ms per l'eta' del pong.
  Return CLong(Timer * 1000)
End
```

> Nota: `Timer` in Gambas è un Float di secondi dall'avvio del programma; va bene come orologio monotono per misurare l'età del pong.

- [ ] **Step 3: `HandlePassage` — arrivo dal tempo assoluto Arduino**

```basic
Private Sub HandlePassage(r As String[])
  ' r = ["P", unix, ms, src]. Converte UTC->locale e instrada come arrivo.
  Dim unixMs As Long = CLong(r[1]) * 1000 + CLong(r[2])
  Dim dLocal As Date = MTime.UnixMsToDate(unixMs)
  Dim sNow As String = MTime.FormatDateMs(dLocal)   ' "hh:nn:ss.uuu" (time-only)
  If FotON Then
    BArrivi.Add(sNow)
    Try MLog.Write("fotocellula", "source=clock orario=" & sNow & " unixms=" & CStr(unixMs) & " src=" & r[3])
  Else
    Try MLog.Write("fotocellula_ignorata", "source=clock motivo=FotOFF orario=" & sNow)
  Endif
End
```

> `FormatDateMs` di una Date con parte-data restituisce `"mm/dd/yyyy hh:nn:ss.uuu"`. Per restare compatibili con il flusso `BArrivi`→`FArrivo.setOrario` (che si aspetta `hh:nn:ss.uuu`), passa una Date time-only: `Time(Hour(dLocal), Minute(dLocal), Second(dLocal), MTime.MillisOf(dLocal))`. Sostituisci la riga `dLocal`/`sNow` con:

```basic
  Dim dFull As Date = MTime.UnixMsToDate(unixMs)
  Dim dLocal As Date = Time(Hour(dFull), Minute(dFull), Second(dFull), MTime.MillisOf(dFull))
  Dim sNow As String = MTime.FormatDateMs(dLocal)
```

- [ ] **Step 4: Compila + commit**

Run: `bash build.sh` → Expected OK.

```bash
git add brrm/.src/FMain.class
git commit -m "brrm/FMain: parsing internal, arrivo dal timestamp assoluto Arduino"
```

### Task 9: Healthcheck (ping) + label a 5 stati

**Files:**
- Modify: `brrm/.src/FMain.class`

- [ ] **Step 1: Ping + valutazione nel `TimerArduino_Timer`**

All'inizio di `TimerArduino_Timer`, dopo aver determinato `bOk` (porta aperta + device esiste), inserisci il branch internal PRIMA del ritorno legacy:

```basic
  If $hwMode = "internal" Then
    If bOk Then
      Try Print #$hSerial, MClock.CMD_PING      ' invia "?" + newline
    Endif
    Dim age As Long
    If $lastPongMs = 0 Then age = 999999 Else age = MTimerNowMs() - $lastPongMs
    Dim st As Integer = MClock.LabelState(Exist(ClockDevice) Or bOk, bOk, age, ClockSrc)
    SetClockLabel(st)
    ' Reconnect se la porta è caduta (come legacy)
    If Not bOk Then
      ArduinoClose()
      WAIT 0.1
      If $shuttingDown Then Return
      Dim sDev As String = MFile.FindArduinoDevice()
      If sDev <> "" Then
        ClockDevice = sDev
        ArduinoOpen(sDev)
      Endif
    Endif
    Return
  Endif
  ' --- da qui in giu': logica legacy invariata ---
```

- [ ] **Step 2: `SetClockLabel` (5 stati)**

Aggiungi:

```basic
Private Sub SetClockLabel(st As Integer)
  If $shuttingDown Then Return
  Dim bOnline As Boolean = (st <> MClock.HW_ABSENT)
  If bOnline <> $arduinoOnline Then
    $arduinoOnline = bOnline
    If bOnline Then Try MLog.Write("arduino_online", "") Else Try MLog.Write("arduino_offline", "")
  Endif
  Select Case st
    Case MClock.HW_GPS
      LabelArduino.Background = &H00CC00&
      LabelArduino.Foreground = &HFFFFFF&
      LabelArduino.Text = "ARDUINO · GPS"
    Case MClock.HW_RTC
      LabelArduino.Background = &HCCCC00&
      LabelArduino.Foreground = &H000000&
      LabelArduino.Text = "ARDUINO · RTC"
    Case MClock.HW_NOSYNC
      LabelArduino.Background = &HCC8800&
      LabelArduino.Foreground = &HFFFFFF&
      LabelArduino.Text = "ARDUINO · NO SYNC"
    Case MClock.HW_UNRESPONSIVE
      LabelArduino.Background = &H888888&
      LabelArduino.Foreground = &HFFFFFF&
      LabelArduino.Text = "ARDUINO ?"
    Case Else  ' HW_ABSENT
      LabelArduino.Background = &HCC0000&
      LabelArduino.Foreground = &HFFFFFF&
      LabelArduino.Text = "NO ARDUINO"
  End Select
End
```

- [ ] **Step 3: Compila + verifica logica**

Run: `bash build.sh` → Expected OK. (Verifica funzionale su hardware al Task 14.)

- [ ] **Step 4: Commit**

```bash
git add brrm/.src/FMain.class
git commit -m "brrm/FMain: healthcheck ping/pong + label a 5 stati"
```

### Task 10: Click sulla label → apertura modal

**Files:**
- Modify: `brrm/.src/FMain.class`

- [ ] **Step 1: Handler di click**

Aggiungi:

```basic
Public Sub LabelArduino_MouseUp()
  If $hwMode <> "internal" Then Return
  RequestId()
  RequestDiag()
  FClockDiag.Show
End

Public Sub RequestDiag()
  If $hSerial <> Null And If $hSerial.Status >= 1 Then Try Print #$hSerial, MClock.CMD_DIAG
End

Public Sub RequestId()
  If $hSerial <> Null And If $hSerial.Status >= 1 Then Try Print #$hSerial, MClock.CMD_ID
End
```

> Se in fase di test la `Label` non emette `MouseUp` (variabilità Qt), convertire `LabelArduino` in un `Button` nel `.form` (stessa posizione/`MoveScaled`) e rinominare l'handler `LabelArduino_Click` → la logica resta identica. La label resta nella lista `$rsBtmCtrl`/`$rsRgtCtrl` per il layout responsivo.

- [ ] **Step 2: Compila + commit**

Run: `bash build.sh` → Expected OK.

```bash
git add brrm/.src/FMain.class
git commit -m "brrm/FMain: click su LabelArduino apre la modal diagnostica"
```

---

## FASE 5 — brrm: modal diagnostica `FClockDiag`

### Task 11: Form modal + popolamento live

**Files:**
- Create: `brrm/.src/FClockDiag.form`, `brrm/.src/FClockDiag.class`
- Create (symlink): `brrm-partenza/.src/FClockDiag.form`, `brrm-partenza/.src/FClockDiag.class`

- [ ] **Step 1: Crea il form**

Crea `brrm/.src/FClockDiag.form` (label-valore aggiornate via codice; un `TimerDiag` per il refresh):

```
# Gambas Form File 3.0

{ Form Form
  MoveScaled(0,0,64,60)
  Text = ("Diagnostica clock")
  { LblSrc Label
    MoveScaled(1,1,62,4)
    Alignment = Align.Center
    Font = Font["+4"]
    Text = ("—")
  }
  { LblGps Label
    MoveScaled(1,6,62,5)
    Text = ("GPS: —")
  }
  { LblRtc Label
    MoveScaled(1,12,62,4)
    Text = ("RTC: —")
  }
  { LblTime Label
    MoveScaled(1,17,62,4)
    Text = ("Tempo: —")
  }
  { LblLink Label
    MoveScaled(1,22,62,4)
    Text = ("Link: —")
  }
  { ButtonRefresh Button
    MoveScaled(1,52,18,5)
    Text = ("Aggiorna")
  }
  { ButtonClose Button
    MoveScaled(45,52,18,5)
    Text = ("Chiudi")
  }
  { TimerDiag #Timer
    #X = 600
    #Y = 320
    Delay = 1000
  }
}
```

- [ ] **Step 2: Crea la class**

Crea `brrm/.src/FClockDiag.class`:

```basic
' Modal diagnostica del clock interno. Legge i dati cache da FMain
' (popolati da DIAG/PONG/ID) e li renderizza. Refresh live ~1s finche'
' aperta: chiede a FMain di inviare "@" e ri-renderizza.

Public Sub Form_Open()
  FClockDiag.Center()
  TimerDiag.Start()
  Render()
End

Public Sub TimerDiag_Timer()
  Try FMain.RequestDiag()
  Render()
End

Public Sub ButtonRefresh_Click()
  Try FMain.RequestDiag()
  Try FMain.RequestId()
  Render()
End

Public Sub ButtonClose_Click()
  Me.Close
End

Public Sub Form_Close()
  TimerDiag.Stop
End

Private Sub Render()
  ' Header sorgente con colore coerente con la label
  Select Case FMain.ClockSrc
    Case "G"
      LblSrc.Text = "Sorgente: GPS (PPS)"
      LblSrc.Background = &H00CC00&
      LblSrc.Foreground = &HFFFFFF&
    Case "R"
      LblSrc.Text = "Sorgente: RTC"
      LblSrc.Background = &HCCCC00&
      LblSrc.Foreground = &H000000&
    Case "N"
      LblSrc.Text = "Sorgente: nessun tempo valido"
      LblSrc.Background = &HCC8800&
      LblSrc.Foreground = &HFFFFFF&
    Case Else
      LblSrc.Text = "Sorgente: —"
      LblSrc.Background = &H888888&
      LblSrc.Foreground = &HFFFFFF&
  End Select

  Dim d As String[] = MClock.ParseLine(FMain.ClockDiagRaw)
  If d <> Null And If d[0] = "D" Then
    ' D = [tag, unix, src, dev_tcnt, nmea_ms, fix, sat, hdop, alt, persi, since_pps, since_rtc, uptime]
    LblGps.Text = "GPS: fix=" & d[5] & "  sat=" & d[6] & "  hdop=" & d[7] & "  alt=" & d[8] & "m  nmea_delay=" & d[4] & "ms  persi=" & d[9]
    LblRtc.Text = "RTC: dev_tcnt=" & d[3] & "  ultimo_write=" & d[11] & "s fa  since_pps=" & d[10] & "s"
    LblTime.Text = "Tempo: unix=" & d[1] & "  uptime=" & d[12] & "s"
  Else
    LblGps.Text = "GPS: —"
    LblRtc.Text = "RTC: —"
    LblTime.Text = "Tempo: —"
  Endif
  LblTime.Text &= "   live=" & FMain.ClockTimeLive
  LblLink.Text = "Link: " & FMain.ClockDevice & "  fw=" & FMain.ClockFwVer
End
```

- [ ] **Step 3: Symlink nella seconda app**

```bash
ln -s ../../brrm/.src/FClockDiag.form brrm-partenza/.src/FClockDiag.form
ln -s ../../brrm/.src/FClockDiag.class brrm-partenza/.src/FClockDiag.class
```

> Se la compilazione di brrm-partenza fallisce per via dei symlink di form (Gambas a volte risolve male i percorsi relativi dei form), sostituisci i due symlink con copie reali dei file e annota in cima alla class "copia di brrm/FClockDiag — tenere sincronizzata".

- [ ] **Step 4: Compila + commit**

Run: `bash build.sh` → Expected OK per entrambi.

```bash
git add brrm/.src/FClockDiag.form brrm/.src/FClockDiag.class brrm-partenza/.src/FClockDiag.form brrm-partenza/.src/FClockDiag.class
git commit -m "FClockDiag: modal diagnostica clock (live-while-open), condivisa fra le app"
```

---

## FASE 6 — brrm: tab "Hardware" in FSettings

### Task 12: Selettore modalità + persistenza

**Files:**
- Modify: `brrm/.src/FSettings.form`
- Modify: `brrm/.src/FSettings.class`

- [ ] **Step 1: Aggiungi il tab al form**

In `brrm/.src/FSettings.form`, nel `TabStrip TabSettings`, incrementa `Count = 3` → `Count = 4` e, dopo l'ultimo `Index = 2` (tab Registro), aggiungi:

```
    Index = 3
    Text = ("Hardware")
    { LabelHw1 Label
      MoveScaled(1,1,30,3)
      Text = ("Tipo hardware fotocellula")
    }
    { ComboHwMode ComboBox
      MoveScaled(1,5,40,3)
      ReadOnly = True
    }
    { LabelHwHint Label
      MoveScaled(1,10,72,8)
      Foreground = &H666666&
      Text = ("Legacy: trigger semplice, tempo dal PC (9600 baud). Clock interno: timestamp assoluto dall'Arduino GPS (115200 baud), label di stato e diagnostica.")
    }
```

- [ ] **Step 2: Load/save in `FSettings.class`**

Aggiungi a `Form_Open` (dopo gli altri Load): `LoadHardwareConfig()`. Implementa:

```basic
Private Sub LoadHardwareConfig()
  ComboHwMode.Clear
  ComboHwMode.Add("Legacy")
  ComboHwMode.Add("Clock interno")
  Dim sPath As String = User.Home &/ ".config/brrm/hardware.conf"
  Dim sMode As String = "legacy"
  If Exist(sPath) Then
    Dim sLine, sKey, sVal As String
    Dim iEq As Integer
    For Each sLine In Split(File.Load(sPath), gb.NewLine)
      sLine = Trim$(sLine)
      If sLine = "" Or If Left$(sLine, 1) = "#" Then Continue
      iEq = InStr(sLine, "=")
      If iEq < 2 Then Continue
      sKey = LCase$(Trim$(Left$(sLine, iEq - 1)))
      sVal = LCase$(Trim$(Mid$(sLine, iEq + 1)))
      If sKey = "mode" Then sMode = sVal
    Next
  Endif
  If sMode = "internal" Then ComboHwMode.Index = 1 Else ComboHwMode.Index = 0
End

Private Function SaveHardwareConfig(sPath As String) As Boolean
  Dim sMode As String
  If ComboHwMode.Index = 1 Then sMode = "internal" Else sMode = "legacy"
  Try MFile.AtomicSave(sPath, "mode = " & sMode & gb.NewLine)
  If Error Then
    Message.Error("Impossibile salvare hardware.conf: " & Error.Text)
    Return False
  Endif
  Return True
End
```

In `ButtonSave_Click`, dopo gli altri salvataggi (prima di `Me.Close`):

```basic
  If Not SaveHardwareConfig(sDir &/ "hardware.conf") Then Return
  Try FMain.HardwareReinit()
```

- [ ] **Step 3: Compila + commit**

Run: `bash build.sh` → Expected OK.

```bash
git add brrm/.src/FSettings.form brrm/.src/FSettings.class
git commit -m "brrm/FSettings: tab Hardware (selettore Legacy/Clock interno + hardware.conf)"
```

---

## FASE 7 — brrm-partenza: mirror

### Task 13: Applica le modifiche FMain/FSettings a brrm-partenza

**Files:**
- Modify: `brrm-partenza/.src/FMain.class`, `brrm-partenza/.src/FSettings.form`, `brrm-partenza/.src/FSettings.class`

> `MClock` e `FClockDiag` sono già symlinkati (Task 3, 11). `hardware.conf` è lo stesso file `~/.config/brrm/hardware.conf`.

- [ ] **Step 1: FMain di brrm-partenza**

Applica gli stessi blocchi dei Task 7, 8, 9, 10 al file `brrm-partenza/.src/FMain.class`, con UNA differenza nell'instradamento del passaggio: brrm-partenza non usa `BArrivi`/`FArrivo`. Individua come la versione legacy registra un trigger (riga `Try ... = sNow` nel suo `Arduino_Read`, intorno alle righe 119-124) e instrada `HandlePassage` allo stesso punto. Versione partenza di `HandlePassage`:

```basic
Private Sub HandlePassage(r As String[])
  Dim unixMs As Long = CLong(r[1]) * 1000 + CLong(r[2])
  Dim dFull As Date = MTime.UnixMsToDate(unixMs)
  Dim dLocal As Date = Time(Hour(dFull), Minute(dFull), Second(dFull), MTime.MillisOf(dFull))
  Dim sNow As String = MTime.FormatDateMs(dLocal)
  ' >>> instrada sNow esattamente come fa il ramo legacy di brrm-partenza
  '     (stesso widget/coda usato oggi per la partenza), preservando il
  '     gate FotON se presente in questa app.
  Try MLog.Write("fotocellula", "source=clock orario=" & sNow & " unixms=" & CStr(unixMs) & " src=" & r[3])
End
```

> Apri il `brrm-partenza/.src/FMain.class` e replica nel `Sub` sopra la *stessa* azione che il ramo legacy esegue con la sua stringa orario (es. `BPartenze.Add(sNow)` o equivalente — usa il nome reale del widget di quel file).

- [ ] **Step 2: FSettings di brrm-partenza**

brrm-partenza ha 2 tab → diventa 3. In `FSettings.form` porta `Count = 2` → `Count = 3` e aggiungi il blocco `Index = 2 / Text = ("Hardware")` con `ComboHwMode`/`LabelHwHint` (identico al Task 12 Step 1, ma `Index = 2`). In `FSettings.class` aggiungi `LoadHardwareConfig`/`SaveHardwareConfig` (identici al Task 12) e l'hook in `ButtonSave_Click`.

- [ ] **Step 3: Compila + commit**

Run: `bash build.sh` → Expected OK per entrambi.

```bash
git add brrm-partenza/.src/FMain.class brrm-partenza/.src/FSettings.form brrm-partenza/.src/FSettings.class
git commit -m "brrm-partenza: mirror integrazione clock interno (FMain + tab Hardware)"
```

---

## FASE 8 — Integrazione finale

### Task 14: Build completo, test, verifica hardware

**Files:** nessuna modifica (verifica)

- [ ] **Step 1: Suite unit test verde**

Run: `bash dev/run-tests.sh` (o docker)
Expected: `PASS: 195/195 test.`

- [ ] **Step 2: Build di entrambe le app**

Run: `bash build.sh`
Expected: `Tutti i progetti compilati correttamente.`

- [ ] **Step 3: Verifica funzionale su hardware (checklist manuale)**

Con firmware `cronometro_gps` flashato e modalità "Clock interno" salvata:
- All'avvio senza fix: label `ARDUINO · NO SYNC` (arancio) o `ARDUINO ?` se nessun pong.
- Con fix GPS: label `ARDUINO · GPS` (verde); staccando l'antenna fino al fallback: `ARDUINO · RTC` (giallo).
- Stacco USB: `NO ARDUINO` (rosso) entro ~3-4 s.
- Click sulla label: modal con GPS/RTC/Tempo/Link popolati e aggiornati ~1s.
- Passaggio fotocellula: arrivo registrato con orario locale derivato dal timestamp Arduino (verifica coerenza al ms con l'orario atteso).
- Selettore su "Legacy": comportamento identico a prima (9600, tempo PC, label 2 stati, click inerte).

- [ ] **Step 4: Commit finale (se servono fix dalla verifica)**

```bash
git add -A
git commit -m "clock interno: fix da verifica hardware"
```

---

## Self-Review (eseguita)

**Copertura spec:** §protocollo→Task 5-6; §MClock→Task 3-4; §UnixMsToDate→Task 1-2 (spostato in MTime, nota sotto); §FMain (mode/baud/parse/healthcheck/label/click/arrivo)→Task 7-10; §modal→Task 11; §settings→Task 12; §brrm-partenza→Task 13; §test→Task 1-4,14; §sicurezza/default-legacy→Task 7. Tutto coperto.

**Deviazione dalla spec (consapevole):** `UnixMsToDate` vive in `MTime` (non `MClock`) — è il modulo tempo del repo e contiene già l'andata `DateToUnixMs`/`DaysFromEpoch`. `MClock` resta focalizzato su protocollo + label.

**Nota PONG timeout:** `PONG_TIMEOUT_MS=3000` con `TimerArduino.Delay=1000` in internal → unresponsive dopo ~3 ping mancati.

**Coerenza tipi:** `ParseLine` ritorna sempre `String[]` (o `Null`); `LabelState` ritorna `Integer` (costanti `HW_*`); `UnixMsToDate(Long)`/`UnixMsToDateTZ(Long,Long)` ritornano `Date`. I campi `Public` su `FMain` (`ClockSrc`, `ClockDiagRaw`, `ClockTimeLive`, `ClockFwVer`, `ClockDevice`) sono letti dalla modal con gli stessi nomi.
