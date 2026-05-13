# Sync brrm-align: session_id Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Identificare gli eventi di sync con la coppia `(session_id, seq)` invece del solo `seq`, così un reset del seq lato partenza non rompe più il sync con l'arrivo.

**Architecture:** Tre componenti coinvolti — server FastAPI (`server/app.py`), client partenza (Gambas, `brrm-partenza`), client arrivo (Gambas, `brrm`). Gli helper puri vivono in `brrm-utils/` come pattern già consolidato (symlink dai progetti applicativi). Server e Gambas-helpers testati con TDD; integrazioni FMain.class verificate via smoke-test di compilazione + test plan manuale dello spec.

**Tech Stack:** Python 3 (FastAPI, pydantic, sse-starlette, pytest), Gambas 3 (gb.test, gb.net.curl).

**Spec di riferimento:** `docs/superpowers/specs/2026-05-13-sync-session-id-design.md`

---

## File Structure

**Create:**
- `brrm-utils/.src/MAlignSync.module` — Helper puri: `NextSessionId`, `EncodeStateFile`, `DecodeStateFile`
- `brrm-utils-test/.src/TestAlignSync.module` — Unit test Gambas
- `brrm-partenza/.src/MAlignSync.module` — Symlink → `../../brrm-utils/.src/MAlignSync.module`
- `brrm/.src/MAlignSync.module` — Symlink → `../../brrm-utils/.src/MAlignSync.module`
- `server/tests/__init__.py` — Marker package vuoto
- `server/tests/test_cache.py` — Unit test pytest per `EventCache` e modelli
- `dev/run-server-tests.sh` — Script che crea venv `/tmp` ed esegue pytest

**Modify:**
- `brrm-utils-test/.src/Main.module` — Aumenta `Test.Plan`, invoca `TestAlignSync.RunAll`
- `server/app.py` — Modello `Event`, classe `EventCache`, `StreamMeta`, endpoint `/stream` e `/events`
- `server/requirements.txt` — Aggiunge `pytest`, `httpx` come dipendenze (per i test)
- `brrm-partenza/.src/FMain.class` — Variabile `$alignSession`, `AlignInit`, `AlignEnqueue`, `AlignReinit`, helper `SaveAlignState`
- `brrm/.src/FMain.class` — Variabile `$alignLastSession`, `AlignInit`, `AlignStartStream`, gestione `meta`/`event` in `AlignHttp_Read`, helper `SaveAlignState`

---

## Task 1: Helper puri `MAlignSync` (TDD via brrm-utils-test)

**Files:**
- Create: `brrm-utils/.src/MAlignSync.module`
- Create: `brrm-utils-test/.src/TestAlignSync.module`
- Modify: `brrm-utils-test/.src/Main.module`

- [ ] **Step 1: Scrivere il test per `NextSessionId` (failing)**

Crea `brrm-utils-test/.src/TestAlignSync.module`:

```gambas
' Test per MAlignSync — generazione monotona di session_id e parsing
' del file di state (session=N\nseq=M).
Public Sub RunAll()
  ' --- NextSessionId: monotonia robusta -------------------------------
  Dim iNow As Long = MTime.DateToUnixMs(Now)

  ' lastSession=0: ritorna il timestamp corrente (>= iNow).
  Dim iFresh As Long = MAlignSync.NextSessionId(0)
  Assert.Ok(iFresh >= iNow, "NextSessionId(0) >= now ms")
  Assert.Ok(iFresh < iNow + 1000, "NextSessionId(0) entro 1s da now")

  ' lastSession nel futuro: ritorna lastSession+1 (orologio indietro).
  Dim iFuture As Long = iNow + 86400000  ' +1 giorno
  Assert.Equals(MAlignSync.NextSessionId(iFuture), iFuture + 1, _
    "NextSessionId con lastSession nel futuro: +1")

  ' lastSession vicino a now ma minore: ritorna now ms.
  Dim iPast As Long = iNow - 5000
  Dim iOut As Long = MAlignSync.NextSessionId(iPast)
  Assert.Ok(iOut >= iNow, "NextSessionId con lastSession nel passato: ritorna now")
End
```

- [ ] **Step 2: Aggiungere `TestAlignSync.RunAll` a `Main.module`**

Modifica `brrm-utils-test/.src/Main.module`:

```gambas
Public Sub Main()
  File.Save("/tmp/main-ran", "yes at " & CStr(Now))
  Test.Plan(87)  ' era 83, +4 per NextSessionId
  TestMillisOf.RunAll()
  TestMsInDay.RunAll()
  TestDaysFromEpoch.RunAll()
  TestBase64URLStr.RunAll()
  TestURLEncode.RunAll()
  TestJsonEscape.RunAll()
  TestPadMillis.RunAll()
  TestMFile.RunAll()
  TestAlignSync.RunAll()
End
```

- [ ] **Step 3: Eseguire i test (verificare fail su modulo mancante)**

Run: `./dev/run-tests-docker.sh` (su Mac) oppure `./dev/run-tests.sh` (su Linux con Gambas).
Expected: FAIL con errore di compilazione "MAlignSync not found" o simile.

- [ ] **Step 4: Creare `MAlignSync.module` con `NextSessionId`**

Crea `brrm-utils/.src/MAlignSync.module`:

```gambas
' MAlignSync — helper puri per il sync brrm-align:
'   - generazione di session_id monotono (resiste a orologio indietro)
'   - serializzazione/parsing del file di state (session=N\nseq=M)
'
' Source-of-truth: gli altri progetti (brrm, brrm-partenza, brrm-utils
' test) puntano qui via symlink. Mai duplicare il codice.

Public Function NextSessionId(lastSession As Long) As Long
  ' Genera il prossimo session_id: max(now_ms, lastSession+1).
  ' Il termine "lastSession+1" e' la difesa contro orologio che torna
  ' indietro (NTP, fuso, RTC scarica) — garantisce monotonia anche se
  ' il timestamp e' inaffidabile.
  Dim nowMs As Long = MTime.DateToUnixMs(Now)
  If nowMs > lastSession Then
    Return nowMs
  Else
    Return lastSession + 1
  Endif
End
```

- [ ] **Step 5: Eseguire i test (verificare pass su NextSessionId)**

Run: `./dev/run-tests-docker.sh`
Expected: PASS dei 3 test di `NextSessionId`. Il `Test.Plan(86)` potrebbe segnalare "test mancanti" perché Encode/Decode non sono ancora scritti.

- [ ] **Step 6: Aggiungere i test per `EncodeStateFile`/`DecodeStateFile` (failing)**

Aggiungi in fondo a `TestAlignSync.RunAll`:

```gambas
  ' --- EncodeStateFile -----------------------------------------------
  Assert.Equals(MAlignSync.EncodeStateFile(1734567890123, 42), _
    "session=1734567890123" & gb.NewLine & "seq=42" & gb.NewLine, _
    "EncodeStateFile produce due righe")
  Assert.Equals(MAlignSync.EncodeStateFile(0, 0), _
    "session=0" & gb.NewLine & "seq=0" & gb.NewLine, _
    "EncodeStateFile con zeri")

  ' --- DecodeStateFile ------------------------------------------------
  Dim iSess As Long
  Dim iSeq As Long
  Dim bOk As Boolean

  bOk = MAlignSync.DecodeStateFile("session=1734567890123" & gb.NewLine & "seq=42" & gb.NewLine, iSess, iSeq)
  Assert.Ok(bOk, "DecodeStateFile due righe ok")
  Assert.Equals(iSess, 1734567890123, "DecodeStateFile: session parsata")
  Assert.Equals(iSeq, 42, "DecodeStateFile: seq parsato")

  ' Tolleranza all'ordine delle righe
  iSess = 0 : iSeq = 0
  bOk = MAlignSync.DecodeStateFile("seq=7" & gb.NewLine & "session=99" & gb.NewLine, iSess, iSeq)
  Assert.Ok(bOk, "DecodeStateFile righe in ordine inverso ok")
  Assert.Equals(iSess, 99, "DecodeStateFile inverso: session")
  Assert.Equals(iSeq, 7, "DecodeStateFile inverso: seq")

  ' Tolleranza a spazi/empty trailing newline mancante
  iSess = 0 : iSeq = 0
  bOk = MAlignSync.DecodeStateFile("session=5" & gb.NewLine & "seq=3", iSess, iSeq)
  Assert.Ok(bOk, "DecodeStateFile senza newline finale")
  Assert.Equals(iSess, 5, "DecodeStateFile senza newline: session")
  Assert.Equals(iSeq, 3, "DecodeStateFile senza newline: seq")

  ' Malformati: ritorna False
  iSess = 0 : iSeq = 0
  bOk = MAlignSync.DecodeStateFile("", iSess, iSeq)
  Assert.NotOk(bOk, "DecodeStateFile: stringa vuota fallisce")

  bOk = MAlignSync.DecodeStateFile("session=abc" & gb.NewLine & "seq=1", iSess, iSeq)
  Assert.NotOk(bOk, "DecodeStateFile: session non numerica fallisce")

  bOk = MAlignSync.DecodeStateFile("session=1", iSess, iSeq)
  Assert.NotOk(bOk, "DecodeStateFile: solo session senza seq fallisce")
```

Aggiorna `Test.Plan` in `Main.module`: `Test.Plan(87 + 14)` = `Test.Plan(101)`.

- [ ] **Step 7: Eseguire test (failing)**

Run: `./dev/run-tests-docker.sh`
Expected: FAIL ("EncodeStateFile/DecodeStateFile not defined").

- [ ] **Step 8: Implementare Encode/DecodeStateFile**

Aggiungi a `MAlignSync.module`:

```gambas
Public Function EncodeStateFile(sess As Long, seq As Long) As String
  Return "session=" & CStr(sess) & gb.NewLine & "seq=" & CStr(seq) & gb.NewLine
End

Public Function DecodeStateFile(sContent As String, ByRef sess As Long, ByRef seq As Long) As Boolean
  ' Parsa "session=N\nseq=M" (o righe in ordine inverso, con/senza
  ' newline finale). Ritorna False se mancano session/seq o se non sono
  ' interi validi. sess/seq sono modificati per riferimento solo in caso
  ' di successo.
  Dim sLine As String
  Dim sKey As String
  Dim sVal As String
  Dim iEq As Integer
  Dim bHasSess As Boolean = False
  Dim bHasSeq As Boolean = False
  Dim outSess As Long
  Dim outSeq As Long

  For Each sLine In Split(sContent, gb.NewLine)
    sLine = Trim$(sLine)
    If sLine = "" Then Continue
    iEq = InStr(sLine, "=")
    If iEq < 2 Then Return False
    sKey = LCase$(Trim$(Left$(sLine, iEq - 1)))
    sVal = Trim$(Mid$(sLine, iEq + 1))
    If sVal = "" Then Return False
    ' Solo cifre (no segni, no decimali)
    If Not IsInteger(sVal) Then Return False
    Select Case sKey
      Case "session"
        outSess = CLong(sVal)
        bHasSess = True
      Case "seq"
        outSeq = CLong(sVal)
        bHasSeq = True
    End Select
  Next

  If Not (bHasSess And bHasSeq) Then Return False
  sess = outSess
  seq = outSeq
  Return True
End

Private Function IsInteger(s As String) As Boolean
  Dim i As Integer
  Dim c As String
  If Len(s) = 0 Then Return False
  For i = 1 To Len(s)
    c = Mid$(s, i, 1)
    If c < "0" Or c > "9" Then Return False
  Next
  Return True
End
```

- [ ] **Step 9: Eseguire test (verificare pass)**

Run: `./dev/run-tests-docker.sh`
Expected: PASS — output `PASS: 97/97 test.`

- [ ] **Step 10: Commit**

```bash
git add brrm-utils/.src/MAlignSync.module \
        brrm-utils-test/.src/TestAlignSync.module \
        brrm-utils-test/.src/Main.module
git commit -m "feat(brrm-utils): MAlignSync helpers + unit test

NextSessionId con max(now_ms, last+1) per monotonia robusta.
EncodeStateFile/DecodeStateFile per il file 'partenza-align-state'.
"
```

---

## Task 2: Symlink `MAlignSync` nei due progetti app

**Files:**
- Create: `brrm-partenza/.src/MAlignSync.module` (symlink)
- Create: `brrm/.src/MAlignSync.module` (symlink)

- [ ] **Step 1: Creare i symlink**

Run:
```bash
ln -s ../../brrm-utils/.src/MAlignSync.module brrm-partenza/.src/MAlignSync.module
ln -s ../../brrm-utils/.src/MAlignSync.module brrm/.src/MAlignSync.module
```

- [ ] **Step 2: Verificare la struttura**

Run: `ls -la brrm-partenza/.src/MAlignSync.module brrm/.src/MAlignSync.module`
Expected: Entrambi mostrano `-> ../../brrm-utils/.src/MAlignSync.module`.

- [ ] **Step 3: Smoke test di compilazione (i progetti devono ancora compilare con il symlink in più)**

Run: `./dev/run-tests-docker.sh` (verifica brrm-utils-test) e `docker run --rm -v "$PWD:/workspace" -w /workspace brrm-dev-test:latest bash -c "cd brrm && gbc3 -w -e -a -g -t -fpublic-control -fpublic-module -x -j1 && cd ../brrm-partenza && gbc3 -w -e -a -g -t -fpublic-control -fpublic-module -x -j1"`

Expected: Tutti compilano senza errori (il modulo c'è ma non è ancora usato).

- [ ] **Step 4: Commit**

```bash
git add brrm-partenza/.src/MAlignSync.module brrm/.src/MAlignSync.module
git commit -m "brrm/brrm-partenza: symlink MAlignSync da brrm-utils"
```

---

## Task 3: Server — modello `Event` + dedup `EventCache` (TDD pytest)

**Files:**
- Create: `server/tests/__init__.py`
- Create: `server/tests/test_cache.py`
- Create: `dev/run-server-tests.sh`
- Modify: `server/app.py`
- Modify: `server/requirements.txt`

- [ ] **Step 1: Creare lo scaffolding pytest e lo script di run**

Crea `server/tests/__init__.py` (file vuoto).

Crea `dev/run-server-tests.sh`:

```bash
#!/usr/bin/env bash
# Esegue pytest per server/ in una venv usa-e-getta dentro /tmp.
# La venv NON viene scritta nel project tree per non sporcare. Per CI
# basta avere python3 + pip nel PATH.
set -euo pipefail
cd "$(dirname "$0")/.."

VENV=/tmp/brrm-server-test-venv
if [ ! -d "$VENV" ]; then
  python3 -m venv "$VENV"
  "$VENV/bin/pip" install --quiet --upgrade pip
  "$VENV/bin/pip" install --quiet -r server/requirements.txt
  "$VENV/bin/pip" install --quiet pytest httpx
fi

# Assicura BRRM_ALIGN_PASSWORD per il side-effect di import del modulo.
export BRRM_ALIGN_PASSWORD="${BRRM_ALIGN_PASSWORD:-test}"

PYTHONPATH=server "$VENV/bin/pytest" -v server/tests/ "$@"
```

Run: `chmod +x dev/run-server-tests.sh`.

Modifica `server/requirements.txt` per includere pytest/httpx (per il file lock-down, non per il prod runtime — è ok perché requirements.txt è dev/test). Aggiungi alla fine:

```
pytest>=7
httpx>=0.27
```

- [ ] **Step 2: Scrivere il primo test (dedup `(session_id, seq)`) — failing**

Crea `server/tests/test_cache.py`:

```python
"""Unit test per EventCache e modello Event con session_id."""
import asyncio
import os

import pytest

# Import-time side effect: app.py richiede BRRM_ALIGN_PASSWORD.
os.environ.setdefault("BRRM_ALIGN_PASSWORD", "test")

from app import Event, EventCache  # noqa: E402


def _run(coro):
    return asyncio.get_event_loop().run_until_complete(coro)


def _ev(session_id: int, seq: int, numero: int = 1, orario: str | None = "10:00:00.000") -> Event:
    return Event(
        session_id=session_id,
        seq=seq,
        type="partenza",
        numero=numero,
        orario=orario,
        ts_origine="2026-05-13T10:00:00.000Z",
    )


def test_event_requires_session_id():
    """Il modello Event richiede session_id >= 1."""
    with pytest.raises(Exception):
        Event(
            seq=1,
            type="partenza",
            numero=1,
            orario="10:00:00.000",
            ts_origine="2026-05-13T10:00:00.000Z",
        )
    with pytest.raises(Exception):
        _ev(session_id=0, seq=1)


def test_dedup_on_session_seq_pair():
    """Stesso seq con session_id diverso NON sovrascrive."""
    cache = EventCache()
    _run(cache.add("S", _ev(session_id=100, seq=1, numero=5)))
    _run(cache.add("S", _ev(session_id=200, seq=1, numero=7)))
    events, _, _, _, _ = _run(cache.get_since("S", 0, 0))
    assert len(events) == 2
    numeri = sorted(e.numero for e in events)
    assert numeri == [5, 7]


def test_dedup_replaces_same_session_seq():
    """Stesso (session_id, seq) sovrascrive (replay sicuro)."""
    cache = EventCache()
    _run(cache.add("S", _ev(session_id=100, seq=1, numero=5)))
    _run(cache.add("S", _ev(session_id=100, seq=1, numero=99)))  # replay
    events, _, _, _, _ = _run(cache.get_since("S", 0, 0))
    assert len(events) == 1
    assert events[0].numero == 99
```

- [ ] **Step 3: Eseguire i test (failing)**

Run: `./dev/run-server-tests.sh`
Expected: FAIL — `Event` non accetta `session_id`, `EventCache.get_since` ha firma vecchia.

- [ ] **Step 4: Modificare `Event` e `EventCache` in `server/app.py`**

In `server/app.py`, sostituisci la classe `Event`:

```python
class Event(BaseModel):
    """Singolo evento di partenza inviato da brrm-partenza."""

    session_id: int = Field(
        ge=1,
        description="Session id monotono generato dalla partenza; cambia "
        "quando il seq riparte da 1",
    )
    seq: int = Field(ge=1, description="Sequence number monotono per-sessione")
    type: str = Field(description="Tipo evento (es. 'partenza')")
    numero: int = Field(ge=1, le=9999, description="Numero equipaggio")
    orario: Optional[str] = Field(
        default=None,
        description="HH:MM:SS.mmm dell'evento; None = annullamento",
    )
    ts_origine: str = Field(description="ISO 8601 UTC del momento di emissione")
```

Sostituisci `EventCache.add` e `EventCache.get_since` (e adatta `_evict_expired` se necessario — non cambia, opera per `received_at`):

```python
    async def add(self, session_id: str, event: Event) -> None:
        async with self._lock:
            bucket = self._events[session_id]
            # Idempotenza: stesso (event.session_id, event.seq) → sovrascrive.
            for i, stored in enumerate(bucket):
                if stored.event.session_id == event.session_id and stored.event.seq == event.seq:
                    bucket[i] = StoredEvent(event=event, received_at=time.monotonic())
                    break
            else:
                bucket.append(StoredEvent(event=event, received_at=time.monotonic()))
                bucket.sort(key=lambda s: (s.event.session_id, s.event.seq))
            notifier = self._notifiers[session_id]
            notifier.set()
            notifier.clear()

    async def get_since(
        self, session_id: str, since_sess: int, since_seq: int
    ) -> tuple[list[Event], int, int, int, int]:
        """Ritorna (eventi con (sess,seq) > (since_sess,since_seq),
        min_sess, min_seq, max_sess, max_seq)."""
        async with self._lock:
            self._evict_expired(session_id)
            bucket = self._events.get(session_id, [])
            if not bucket:
                return [], 0, 0, 0, 0
            min_sess = bucket[0].event.session_id
            min_seq = bucket[0].event.seq
            max_sess = bucket[-1].event.session_id
            max_seq = bucket[-1].event.seq
            new_events = [
                s.event
                for s in bucket
                if (s.event.session_id, s.event.seq) > (since_sess, since_seq)
            ]
            return new_events, min_sess, min_seq, max_sess, max_seq
```

- [ ] **Step 5: Eseguire i test (verificare pass sui primi tre)**

Run: `./dev/run-server-tests.sh`
Expected: PASS sui 3 test. Altri endpoint (`/stream`, `/events`) potrebbero usare la vecchia firma di `get_since` — non testati ancora, lasciamo per Task 4.

- [ ] **Step 6: Test su `get_since` lessicografico — failing**

Aggiungi a `test_cache.py`:

```python
def test_get_since_lexicographic():
    """get_since filtra (sess, seq) > (since_sess, since_seq)."""
    cache = EventCache()
    _run(cache.add("S", _ev(session_id=100, seq=1)))
    _run(cache.add("S", _ev(session_id=100, seq=2)))
    _run(cache.add("S", _ev(session_id=200, seq=1)))
    _run(cache.add("S", _ev(session_id=200, seq=2)))

    # Da (100, 1): deve ritornare (100,2), (200,1), (200,2).
    events, _, _, _, _ = _run(cache.get_since("S", 100, 1))
    coppie = [(e.session_id, e.seq) for e in events]
    assert coppie == [(100, 2), (200, 1), (200, 2)]

    # Da (150, 999): salta tutta la session 100, ritorna le session 200.
    events, _, _, _, _ = _run(cache.get_since("S", 150, 999))
    coppie = [(e.session_id, e.seq) for e in events]
    assert coppie == [(200, 1), (200, 2)]

    # Da (300, 0): cache esaurita, lista vuota.
    events, _, _, _, _ = _run(cache.get_since("S", 300, 0))
    assert events == []


def test_get_since_returns_min_max():
    """Meta (min/max sess/seq) della cache."""
    cache = EventCache()
    _run(cache.add("S", _ev(session_id=100, seq=5)))
    _run(cache.add("S", _ev(session_id=100, seq=6)))
    _run(cache.add("S", _ev(session_id=200, seq=1)))

    _, min_sess, min_seq, max_sess, max_seq = _run(cache.get_since("S", 0, 0))
    assert (min_sess, min_seq) == (100, 5)
    assert (max_sess, max_seq) == (200, 1)


def test_get_since_empty_cache_returns_zero_meta():
    cache = EventCache()
    events, min_sess, min_seq, max_sess, max_seq = _run(cache.get_since("S", 0, 0))
    assert events == []
    assert (min_sess, min_seq, max_sess, max_seq) == (0, 0, 0, 0)
```

- [ ] **Step 7: Eseguire test (verificare pass)**

Run: `./dev/run-server-tests.sh`
Expected: PASS — tutti i 7 test verdi.

- [ ] **Step 8: Commit**

```bash
git add server/app.py server/tests/__init__.py server/tests/test_cache.py \
        server/requirements.txt dev/run-server-tests.sh
git commit -m "feat(server): Event.session_id + dedup (session_id, seq)

EventCache.add deduplica sulla coppia; sovrascrive solo stesso (sess, seq).
EventCache.get_since accetta since_sess + since_seq, filtra lessicograficamente.
Test pytest in server/tests/ via dev/run-server-tests.sh (venv /tmp).
"
```

---

## Task 4: Server — endpoint `/stream` con `since_sess` + `StreamMeta` esteso

**Files:**
- Modify: `server/app.py`
- Modify: `server/tests/test_cache.py` (test end-to-end /events + /stream)

- [ ] **Step 1: Test end-to-end via TestClient — failing**

Aggiungi in cima a `server/tests/test_cache.py`:

```python
from fastapi.testclient import TestClient
from app import app  # noqa: E402

AUTH = ("brrm", "test")


def test_post_and_stream_meta_includes_session():
    """meta del /stream espone min_sess, max_sess oltre a min_seq/max_seq."""
    cache.invalidate_for_tests()
    client = TestClient(app)
    body = {
        "session_id": 1700000000000,
        "seq": 1,
        "type": "partenza",
        "numero": 5,
        "orario": "10:00:00.000",
        "ts_origine": "2026-05-13T10:00:00.000Z",
    }
    r = client.post("/events?session=test", json=body, auth=AUTH)
    assert r.status_code == 200

    # Stream con since_sess=0, since=0: dovrebbe restituire l'evento + meta.
    with client.stream("GET", "/stream?session=test&since_sess=0&since=0", auth=AUTH) as r:
        assert r.status_code == 200
        # Leggi solo il primo blocco SSE
        lines = []
        for raw in r.iter_lines():
            lines.append(raw)
            if raw == "" and len(lines) > 4:
                break
        joined = "\n".join(lines)
        assert "min_sess" in joined
        assert "max_sess" in joined
        assert "1700000000000" in joined


def test_stream_since_sess_filters_old_session():
    """since_sess > 0 esclude eventi di session_id inferiori."""
    cache.invalidate_for_tests()
    client = TestClient(app)
    # Due eventi: vecchia sessione + nuova sessione.
    for body in [
        {"session_id": 100, "seq": 1, "type": "partenza", "numero": 1, "orario": "10:00:00.000", "ts_origine": "2026-05-13T10:00:00.000Z"},
        {"session_id": 200, "seq": 1, "type": "partenza", "numero": 2, "orario": "10:00:01.000", "ts_origine": "2026-05-13T10:00:01.000Z"},
    ]:
        client.post("/events?session=test2", json=body, auth=AUTH).raise_for_status()

    with client.stream("GET", "/stream?session=test2&since_sess=100&since=1", auth=AUTH) as r:
        body_text = b""
        for chunk in r.iter_bytes():
            body_text += chunk
            if b"\"numero\":2" in body_text:
                break
        assert b"\"numero\":2" in body_text
        assert b"\"numero\":1" not in body_text
```

Aggiungi import `from app import cache` in alto, e implementa `cache.invalidate_for_tests()` nel prossimo step.

- [ ] **Step 2: Eseguire test (failing su `since_sess` query param + `min_sess` in meta)**

Run: `./dev/run-server-tests.sh`
Expected: FAIL — endpoint non accetta `since_sess`, meta non ha `min_sess`.

- [ ] **Step 3: Aggiornare `StreamMeta` e gli endpoint**

In `server/app.py`, sostituisci `StreamMeta`:

```python
class StreamMeta(BaseModel):
    """Metadata mandato come primo evento SSE su ogni connessione."""

    type: str = "meta"
    min_sess: int = Field(description="session_id piu' vecchio in cache (0 se vuota)")
    min_seq: int = Field(description="seq dell'evento piu' vecchio in cache")
    max_sess: int = Field(description="session_id piu' recente in cache")
    max_seq: int = Field(description="seq dell'evento piu' recente in cache")
    server_time: str
```

Sostituisci la funzione `stream`:

```python
@app.get("/stream")
async def stream(
    request: Request,
    session: str = Query(default="default"),
    since: int = Query(default=0, ge=0),
    since_sess: int = Query(default=0, ge=0),
    _user: str = Depends(authenticate),
):
    """SSE stream: snapshot iniziale + push di nuovi eventi + heartbeat."""

    async def event_generator():
        last_sess = since_sess
        last_seq = since

        # Frame iniziale: meta con min/max (sess, seq) attuali.
        events, min_sess, min_seq, max_sess, max_seq = await cache.get_since(
            session, last_sess, last_seq
        )
        meta = StreamMeta(
            min_sess=min_sess,
            min_seq=min_seq,
            max_sess=max_sess,
            max_seq=max_seq,
            server_time=f"{time.time():.3f}",
        )
        yield {"event": "meta", "data": meta.model_dump_json()}
        for ev in events:
            yield {"event": "event", "data": ev.model_dump_json()}
            if (ev.session_id, ev.seq) > (last_sess, last_seq):
                last_sess, last_seq = ev.session_id, ev.seq

        last_keepalive = time.monotonic()
        while True:
            if await request.is_disconnected():
                return

            notifier = cache.notifier_for(session)
            try:
                await asyncio.wait_for(notifier.wait(), timeout=POLL_INTERVAL_SEC)
            except asyncio.TimeoutError:
                pass

            new_events, _, _, _, _ = await cache.get_since(session, last_sess, last_seq)
            for ev in new_events:
                yield {"event": "event", "data": ev.model_dump_json()}
                if (ev.session_id, ev.seq) > (last_sess, last_seq):
                    last_sess, last_seq = ev.session_id, ev.seq
                last_keepalive = time.monotonic()

            now = time.monotonic()
            if now - last_keepalive >= KEEPALIVE_SEC:
                yield {"event": "keepalive", "data": f"{time.time():.3f}"}
                last_keepalive = now

    return EventSourceResponse(event_generator())
```

Aggiungi alla classe `EventCache` un metodo per resettare la cache nei test:

```python
    def invalidate_for_tests(self) -> None:
        """Resetta la cache. SOLO per uso nei test."""
        self._events.clear()
        self._notifiers.clear()
```

- [ ] **Step 4: Eseguire test (verificare pass)**

Run: `./dev/run-server-tests.sh`
Expected: PASS — tutti i test (9 totali) verdi.

- [ ] **Step 5: Aggiornare docstring del modulo**

Sostituisci la docstring di `server/app.py` (righe 1-22) per descrivere `session_id`:

```python
"""
brrm-align: bridge HTTP fra postazione partenza e postazione arrivo
quando i due PC non sono sulla stessa rete locale.

Protocollo:
  POST /events?session=<id>
    { session_id, seq, type, numero, orario, ts_origine }
    Pubblica un evento (chiamato da brrm-partenza). Idempotente: se
    arriva lo stesso (session_id, seq), sovrascrive (replay sicuro).
    session_id e' monotono per-partenza: cambia quando il seq riparte
    da 1 (es. dopo reset locale del file di state).

  GET /stream?session=<id>&since_sess=<S>&since=<N>
    Server-Sent Events. Restituisce subito tutti gli eventi con
    (session_id, seq) > (since_sess, since), poi tiene la connessione
    aperta. Heartbeat ogni 10 secondi.

Dedup: chiave (session_id, seq). Ordering: lessicografico sulla coppia.

Storage: in-memory dict per session_id (query param), TTL 5 minuti dal
momento di ricezione. Niente persistenza disco. brrm-arrivo gestisce i
gap (campi min_sess/min_seq nel meta della response stream).

Auth: HTTP Basic, credenziali da env BRRM_ALIGN_USER / BRRM_ALIGN_PASSWORD.
"""
```

- [ ] **Step 6: Smoke test manuale del server**

Run, in due terminali separati:

```bash
# Terminale 1 — server
cd server && BRRM_ALIGN_PASSWORD=test python3 -m uvicorn app:app --reload --port 8000
```

```bash
# Terminale 2 — POST + stream
curl -u brrm:test -X POST 'http://localhost:8000/events?session=demo' \
  -H 'content-type: application/json' \
  -d '{"session_id":1,"seq":1,"type":"partenza","numero":5,"orario":"10:00:00.000","ts_origine":"2026-05-13T10:00:00.000Z"}'

curl -u brrm:test --no-buffer 'http://localhost:8000/stream?session=demo&since_sess=0&since=0'
# Expected: meta con min_sess=1, max_sess=1, e l'evento numero=5.
```

Expected: il flusso ritorna meta + evento. Ferma il server (Ctrl+C in T1).

- [ ] **Step 7: Commit**

```bash
git add server/app.py server/tests/test_cache.py
git commit -m "feat(server): /stream accetta since_sess, meta espone min/max sess

StreamMeta include min_sess/max_sess. /stream legge since_sess (default 0),
filtra eventi con (sess,seq) > (since_sess,since). EventCache espone
invalidate_for_tests() per test isolation. Docstring del modulo aggiornata.
"
```

---

## Task 5: Partenza — integrazione `AlignInit`/`AlignEnqueue` + migrazione

**Files:**
- Modify: `brrm-partenza/.src/FMain.class`

Niente TDD su FMain.class (form-bound, non isolabile). Verifichiamo via compile + smoke test docker.

- [ ] **Step 1: Aggiungere la variabile `$alignSession`**

In `brrm-partenza/.src/FMain.class`, accanto a `Private $alignSeq As Long` (riga 31), aggiungi:

```gambas
Private $alignSession As Long
```

Rinomina `$alignSession As String` (riga 30) → `$alignSessionConfig As String`. Aggiorna *tutti* i suoi usi in FMain.class (verifica con `grep -n alignSession brrm-partenza/.src/FMain.class` — tutti gli usi di `$alignSession` esistenti diventano `$alignSessionConfig`).

- [ ] **Step 2: Aggiornare `AlignLoadConfig`**

In `AlignLoadConfig` (~riga 100), sostituisci tutti i `$alignSession =` con `$alignSessionConfig =`. Esempio:

```gambas
  $alignSessionConfig = "default"
  ...
      Case "session"
        $alignSessionConfig = sVal
```

- [ ] **Step 3: Aggiornare `AlignReinit`**

In `AlignReinit` (~riga 132), sostituisci:

```gambas
  $alignSessionConfig = ""
  $alignSession = 0
```

(dove prima c'era solo `$alignSession = ""`).

- [ ] **Step 4: Sostituire init del seq con caricamento del nuovo file di state**

In `AlignInit`, sostituisci:

```gambas
  $alignSeq = 0
  Try $alignSeq = CLong(Trim$(File.Load(User.Home &/ ".local/share/brrm/partenza-align-seq")))
```

con:

```gambas
  $alignSession = 0
  $alignSeq = 0
  LoadAlignState()
```

Aggiungi il metodo privato `LoadAlignState` (subito dopo `AlignInit`):

```gambas
Private Sub LoadAlignState()
  ' Carica (session, seq) dal file partenza-align-state. Se non esiste,
  ' migra dal vecchio partenza-align-seq (e genera subito un session_id,
  ' altrimenti il seq migrato > 0 non triggererebbe la regola "nuovo
  ' session_id quando seq riparte da 1").
  Dim sStatePath As String
  Dim sLegacyPath As String
  sStatePath = User.Home &/ ".local/share/brrm/partenza-align-state"
  sLegacyPath = User.Home &/ ".local/share/brrm/partenza-align-seq"

  If Exist(sStatePath) Then
    If MAlignSync.DecodeStateFile(File.Load(sStatePath), $alignSession, $alignSeq) Then Return
    ' File corrotto: tratta come assente.
    $alignSession = 0
    $alignSeq = 0
  Endif

  If Exist(sLegacyPath) Then
    Dim iLegacySeq As Long = 0
    Try iLegacySeq = CLong(Trim$(File.Load(sLegacyPath)))
    $alignSeq = iLegacySeq
    $alignSession = MAlignSync.NextSessionId(0)
    SaveAlignState()
    Try Kill sLegacyPath
  Endif
End

Private Sub SaveAlignState()
  MFile.AtomicSave(User.Home &/ ".local/share/brrm/partenza-align-state", _
    MAlignSync.EncodeStateFile($alignSession, $alignSeq))
End
```

- [ ] **Step 5: Aggiornare `AlignEnqueue`**

In `AlignEnqueue` (~riga 218), sostituisci:

```gambas
  $alignSeq = $alignSeq + 1
  ...
  sEvent = "{" & Chr(34) & "seq" & Chr(34) & ":" & $alignSeq
  ...
  MFile.AtomicSave(User.Home &/ ".local/share/brrm/partenza-align-seq", CStr($alignSeq))
```

con:

```gambas
  ' Genera un nuovo session_id se il seq sta per passare da 0 a 1
  ' (fresh start o reset esterno del file di state).
  If $alignSeq = 0 Then
    $alignSession = MAlignSync.NextSessionId($alignSession)
  Endif
  $alignSeq = $alignSeq + 1
  ...
  sEvent = "{" & Chr(34) & "session_id" & Chr(34) & ":" & $alignSession
  sEvent &= "," & Chr(34) & "seq" & Chr(34) & ":" & $alignSeq
  ...
  SaveAlignState()
```

Rimuovi la riga del salvataggio del vecchio file `partenza-align-seq`. (La riga di salvataggio dell'outbox `partenza-align-outbox` resta com'è.)

- [ ] **Step 6: Aggiornare il tooltip di `AlignSetStatus`**

Cerca tutti i `$alignSession & ...` rimasti — gli usi nel tooltip — e sostituiscili con `$alignSessionConfig`. Esempio (riga ~215):

```gambas
  LabelSync.Tooltip = detail & gb.NewLine & "URL: " & $alignUrl & gb.NewLine & "Session: " & $alignSessionConfig
```

Aggiungi anche, per debug, la session_id corrente:

```gambas
  LabelSync.Tooltip = detail & gb.NewLine & "URL: " & $alignUrl _
    & gb.NewLine & "Session: " & $alignSessionConfig _
    & gb.NewLine & "Run id: " & CStr($alignSession) _
    & gb.NewLine & "Seq: " & CStr($alignSeq)
```

- [ ] **Step 7: Aggiornare URL di POST**

Cerca `$alignUrl & "/events?session=" & $alignSession`. Sostituisci con:

```gambas
  $hAlignHttp.URL = $alignUrl & "/events?session=" & $alignSessionConfig
```

- [ ] **Step 8: Smoke test di compilazione**

Run:
```bash
cd brrm-partenza
docker run --rm -v "$PWD/..:/workspace" -w /workspace/brrm-partenza brrm-dev-test:latest \
  bash -c "gbc3 -w -e -a -g -t -fpublic-control -fpublic-module -x -j1"
cd ..
```

Expected: nessun errore di compilazione.

- [ ] **Step 9: Commit**

```bash
git add brrm-partenza/.src/FMain.class
git commit -m "feat(brrm-partenza): emette eventi con session_id monotono

AlignInit carica/migra il file di state (session,seq). AlignEnqueue
genera un nuovo session_id quando il seq passa da 0 a 1. Rename
\$alignSession As String -> \$alignSessionConfig per separare la session
di config dalla session_id dinamica.
"
```

---

## Task 6: Arrivo — integrazione `AlignInit`/stream/dedup + migrazione

**Files:**
- Modify: `brrm/.src/FMain.class`

- [ ] **Step 1: Aggiungere la variabile `$alignLastSession`**

Accanto a `Private $alignLastSeq As Long` (riga 29), aggiungi:

```gambas
Private $alignLastSession As Long
```

Rinomina `$alignSession As String` → `$alignSessionConfig As String` (stesso pattern di partenza). Aggiorna *tutti* gli usi in `brrm/.src/FMain.class`.

- [ ] **Step 2: Aggiornare `AlignLoadConfig`**

In `AlignLoadConfig`, sostituisci `$alignSession =` con `$alignSessionConfig =` (default `"default"` + case `"session"`).

- [ ] **Step 3: Aggiornare `AlignReinit`**

```gambas
  $alignSessionConfig = ""
  $alignLastSession = 0
```

- [ ] **Step 4: Caricare il nuovo file di state in `AlignInit`**

Sostituisci:

```gambas
  $alignLastSeq = 0
  Try $alignLastSeq = CLong(Trim$(File.Load(User.Home &/ ".local/share/brrm/arrivo-align-lastseq")))
```

con:

```gambas
  $alignLastSession = 0
  $alignLastSeq = 0
  LoadAlignState()
```

Aggiungi i metodi privati (dopo `AlignInit`):

```gambas
Private Sub LoadAlignState()
  ' Carica (session, seq) dal file arrivo-align-state. Se non esiste,
  ' migra dal vecchio arrivo-align-lastseq (solo seq, session_id=0:
  ' qualunque evento con session_id>=1 sara' "successivo", che e' il
  ' comportamento giusto).
  Dim sStatePath As String
  Dim sLegacyPath As String
  sStatePath = User.Home &/ ".local/share/brrm/arrivo-align-state"
  sLegacyPath = User.Home &/ ".local/share/brrm/arrivo-align-lastseq"

  If Exist(sStatePath) Then
    If MAlignSync.DecodeStateFile(File.Load(sStatePath), $alignLastSession, $alignLastSeq) Then Return
    $alignLastSession = 0
    $alignLastSeq = 0
  Endif

  If Exist(sLegacyPath) Then
    Try $alignLastSeq = CLong(Trim$(File.Load(sLegacyPath)))
    $alignLastSession = 0
    SaveAlignState()
    Try Kill sLegacyPath
  Endif
End

Private Sub SaveAlignState()
  MFile.AtomicSave(User.Home &/ ".local/share/brrm/arrivo-align-state", _
    MAlignSync.EncodeStateFile($alignLastSession, $alignLastSeq))
End
```

- [ ] **Step 5: Aggiornare URL dello stream**

Sostituisci (in `AlignStartStream`):

```gambas
  $hAlignHttp.URL = $alignUrl & "/stream?session=" & $alignSession & "&since=" & CStr($alignLastSeq)
```

con:

```gambas
  $hAlignHttp.URL = $alignUrl & "/stream?session=" & $alignSessionConfig _
    & "&since_sess=" & CStr($alignLastSession) _
    & "&since=" & CStr($alignLastSeq)
```

- [ ] **Step 6: Aggiornare `AlignHttp_Read` (case `meta` e `event`)**

In `AlignHttp_Read`, nella gestione `meta` sostituisci il blocco gap-detection (~righe 297-302) con:

```gambas
    Case "meta"
      ' Gap detection lessicografica. Se la cache server e' avanzata oltre
      ' la nostra (last_session, last_seq) - 1, allineiamo per accettare
      ' lo snapshot. (min_seq - 1 lavora anche cross-session: la formula
      ' "se sono prima del min, salta al min - 1 nello stesso sess".)
      Dim iMinSess As Long = 0
      Dim iMinSeq As Long = 0
      Try iMinSess = CLong(ev["min_sess"])
      Try iMinSeq = CLong(ev["min_seq"])
      If iMinSess > 0 And If iMinSeq > 0 Then
        Dim bGap As Boolean = False
        If $alignLastSession < iMinSess Then
          bGap = True
        Else If $alignLastSession = iMinSess And If $alignLastSeq < iMinSeq - 1 Then
          bGap = True
        Endif
        If bGap Then
          $alignLastSession = iMinSess
          $alignLastSeq = iMinSeq - 1
          SaveAlignState()
        Endif
      Endif
```

Nella gestione `event` (~righe 304-329), sostituisci il blocco di dedup con:

```gambas
    Case "event"
      Dim iSess As Long = 0
      iSeq = 0
      Try iSess = CLong(ev["session_id"])
      Try iSeq = CLong(ev["seq"])
      ' Dedup lessicografica: scarta se (sess, seq) <= (last_sess, last_seq).
      If iSess < $alignLastSession Then Return
      If iSess = $alignLastSession And If iSeq <= $alignLastSeq Then Return

      iNumero = 0
      Try iNumero = CInt(ev["numero"])
      If iNumero <= 0 Or iNumero >= MAX_NUMERO Then Return
      sOrario = ""
      Try sOrario = CStr(ev["orario"])
      Timer1.Stop
      If sOrario = "" Then
        partenze[iNumero] = 0
      Else
        dPartenza = 0
        Try dPartenza = sOrario
        If dPartenza <> 0 Then
          partenze[iNumero] = dPartenza
          If LEquipaggi.Find(iNumero) < 0 Then LEquipaggi.Add(iNumero)
        Endif
      Endif
      $alignLastSession = iSess
      $alignLastSeq = iSeq
      SaveAlignState()
      UpgradeSessionFile()
      Timer1.Start
      If $sheet Then $sheet.UpdateForEquipaggio(iNumero)
```

- [ ] **Step 7: Aggiornare il tooltip**

Sostituisci la riga del tooltip in `AlignSetStatus`:

```gambas
  LabelSync.Tooltip = detail & gb.NewLine & "URL: " & $alignUrl _
    & gb.NewLine & "Session: " & $alignSessionConfig _
    & gb.NewLine & "Ultimo (sess, seq): (" & CStr($alignLastSession) & ", " & CStr($alignLastSeq) & ")"
```

- [ ] **Step 8: Smoke test di compilazione**

Run:
```bash
docker run --rm -v "$PWD:/workspace" -w /workspace/brrm brrm-dev-test:latest \
  bash -c "gbc3 -w -e -a -g -t -fpublic-control -fpublic-module -x -j1"
```

Expected: nessun errore di compilazione.

- [ ] **Step 9: Commit**

```bash
git add brrm/.src/FMain.class
git commit -m "feat(brrm): consuma session_id e deduplica su (sess, seq)

AlignInit carica/migra arrivo-align-state. /stream passa since_sess.
AlignHttp_Read fa gap-detection e dedup lessicografica su (sess, seq).
\$alignSession As String -> \$alignSessionConfig.
"
```

---

## Task 7: Verifica end-to-end manuale (test plan dello spec)

**Setup:** server in docker locale o sulla test machine; partenza e arrivo via `dev/osx/run.sh` (Mac) o nativi (test machine Gambas 3, boxrally@192.168.1.171).

- [ ] **Scenario 1 — Reset del seq post-fix**

1. Avvia server: `cd server && BRRM_ALIGN_PASSWORD=test docker compose up`
2. Avvia partenza+arrivo con config `session=manual-test`, URL `http://localhost:8000`, user/pass `brrm/test`.
3. Sulla partenza, emetti 5 partenze (numeri 1-5). Verifica che l'arrivo le vede tutte.
4. Chiudi partenza, cancella `~/.local/share/brrm/partenza-align-state` con `rm`.
5. Riavvia partenza. Emetti 3 partenze (numeri 6-8).

Expected: l'arrivo vede 6, 7, 8 anche se i loro seq sono 1, 2, 3. Verifica con `cat ~/.local/share/brrm/arrivo-align-state` che il `session=` è cambiato.

- [ ] **Scenario 2 — Monotonia con orologio indietro**

1. Stato come Scenario 1, fine.
2. Spegni partenza. Sposta l'orologio del PC partenza indietro di un'ora (`sudo date -s '-1 hour'` su Linux; nel container Docker passare `-e TZ=...` o modificare prima di entrare).
3. Cancella `~/.local/share/brrm/partenza-align-state`. Riavvia partenza, emetti 1 partenza (numero 9).

Expected: il `session_id` del nuovo evento è > di quello precedente (visibile nel tooltip "Run id" di partenza o nello stream del server). L'arrivo accetta l'evento.

Ripristina l'orologio (`sudo ntpdate -u pool.ntp.org` o equivalente).

- [ ] **Scenario 3 — Backward gap (TTL server) senza cambio sessione**

1. Avvia partenza+arrivo. Emetti 3 partenze.
2. Ferma arrivo. Aspetta > 5 minuti (TTL del server) — oppure restart del server.
3. Emetti 2 nuove partenze (i loro seq sono 4 e 5 della stessa session).
4. Riavvia arrivo.

Expected: l'arrivo riceve il meta con `min_sess` = session corrente, `min_seq` = 4. Si allinea e accetta gli eventi 4, 5.

- [ ] **Scenario 4 — Eventi tardivi vecchia sessione**

1. Stato: arrivo ha appena ricevuto eventi della session S2.
2. Inietta manualmente con `curl` un evento per la session S1 (più bassa) — usa `(S1, X)` con X qualunque.

```bash
curl -u brrm:test -X POST 'http://localhost:8000/events?session=manual-test' \
  -H 'content-type: application/json' \
  -d '{"session_id":<S1>,"seq":99,"type":"partenza","numero":99,"orario":"10:00:00.000","ts_origine":"2026-05-13T10:00:00.000Z"}'
```

Expected: l'arrivo riceve il chunk SSE ma scarta silenziosamente (no movimento in UI, nessun cambio di `arrivo-align-state`).

- [ ] **Scenario 5 — Outbox attraverso il reset**

1. Sposta in offline la partenza (firewall il server o stoppalo).
2. Emetti 3 partenze sulla partenza (numeri 10-12). L'outbox `partenza-align-outbox` ha 3 entry con session S2.
3. Cancella `partenza-align-state` *senza* cancellare l'outbox. (Operazione anomala ma simula edge case.)
4. Riavvia partenza, emetti 2 partenze (numeri 13-14). Outbox ora ha entry con session S2 (vecchie) + S3 (nuove).
5. Riporta online il server.

Expected: tutti gli eventi vengono spediti; l'arrivo applica nell'ordine in cui arrivano. Se S3 arriva prima di S2, S2 successivi vengono scartati come "vecchia sessione". Variazione attesa di stato: equipaggi 10-12 *possono* non comparire (a seconda dell'ordine di flush); 13-14 compaiono sempre.

(Questo scenario è informativo: documenta il trade-off accettato — outbox non flushato al reset implica perdita possibile degli eventi precedenti.)

- [ ] **Scenario 6 — Unit test in CI**

Run sulla CI o localmente: `./dev/run-tests-docker.sh && ./dev/run-server-tests.sh`
Expected: tutto verde.

- [ ] **Step finale — Tag/PR**

Se tutti gli scenari passano, riposo. Non c'è commit aggiuntivo da fare in questo task.

---

## Note di esecuzione

- **Compilazione Gambas via Docker**: l'image `brrm-dev-test:latest` viene builddata automaticamente da `dev/run-tests-docker.sh` al primo run. Riutilizzarla per le compilazioni mirate in Task 5/6.
- **Auth del server nei test**: i test pytest impostano `BRRM_ALIGN_PASSWORD=test` via `os.environ.setdefault` prima di importare `app`. Il helper Bash fa lo stesso. In produzione la password è in env del container.
- **Ordering dei task**: 1 → 2 → 3 → 4 → 5/6 in parallelo o sequenziali → 7. I task 5 e 6 sono indipendenti uno dall'altro (toccano file diversi) ma entrambi dipendono dal symlink di Task 2 e dal server di Task 4.
- **Commit granularity**: un commit per task. Se durante un task emerge un fix collaterale (es. test esistente che si rompe), commit separato con messaggio chiaro.
