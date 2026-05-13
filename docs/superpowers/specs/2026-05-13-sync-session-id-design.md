# Sync brrm-align: session_id per gestire il reset del seq

**Status:** design approvato dall'utente
**Componenti coinvolti:** `server/app.py`, `brrm-partenza/.src/FMain.class`, `brrm/.src/FMain.class`
**Autore:** Alessandro Rinaldi
**Data:** 2026-05-13

## Problema

Il protocollo di sync corrente identifica gli eventi con un solo `seq` monotono per-sessione (`session` è una stringa statica di config in `align.conf`, default `"default"`). Quando il `seq` lato partenza riparte da 1 (es. file `~/.local/share/brrm/partenza-align-seq` cancellato o azzerato), il sync si rompe:

- **Server (`EventCache.add`)**: rilevando lo stesso `seq` di un evento già in cache, *sovrascrive* l'entry — comportamento pensato per il replay sicuro ma che qui distrugge silenziosamente eventi vecchi.
- **Arrivo (`FMain.class:307`)**: scarta i nuovi eventi finché `iSeq <= $alignLastSeq`. Inoltre il gap-detection del `meta` (riga 299) lo allinea a `min_seq-1` della cache, peggiorando il caso: se la cache contiene ancora eventi vecchi con seq grandi, il client non risale mai i seq bassi della nuova sessione.

Risultato: dopo un reset del seq, l'arrivo è cieco finché la partenza non riemette tanti eventi quanti ne aveva pre-reset.

Lo stato applicativo lato arrivo (`partenze[]`, `LEquipaggi`) **non** va resettato: è una proprietà del sistema, non del bug. Solo il dedup va fixato.

## Soluzione

Aggiungere un identificativo monotono `session_id` accoppiato al `seq`. La dedup confronta `(session_id, seq)` lessicograficamente invece del solo `seq`. Un reset del seq genera un nuovo `session_id` maggiore del precedente; il client arrivo, vedendo eventi con `session_id` superiore, li accetta a prescindere dal `seq` della vecchia sessione.

La `session` di config (`align.conf`) resta invariata nel significato e nell'uso: identifica il pairing partenza/arrivo, è statica, sopravvive ai riavvii del software. Il nuovo `session_id` è una dimensione *aggiuntiva* gestita automaticamente, mai esposta all'utente.

### Monotonia del session_id

Generazione: `next_session = max(now_ms, last_session + 1)` dove `now_ms` è il timestamp Unix in millisecondi e `last_session` è recuperato dalla persistenza locale. La componente `last_session + 1` è la difesa contro l'orologio che torna indietro (NTP, fuso, RTC scarica): garantisce monotonia anche se il timestamp non è affidabile.

Il `session_id` è persistito in modo che sopravviva alla cancellazione del file `partenza-align-seq`. Se il seq è azzerato/perso ma `last_session` è ancora disponibile, il nuovo `session_id` è strettamente maggiore. Se anche `last_session` è perso, parte da 0 e il `max` con `now_ms` dà comunque un valore plausibilmente progressivo.

Implementazione: i due valori (`session_id`, `seq`) vivono in un unico file `partenza-align-state` con formato a due righe `session=<int>\nseq=<int>`, salvato atomicamente con `MFile.AtomicSave`. **Un file unico** evita stati incoerenti dove un `AtomicSave` riesce e l'altro no.

Migrazione: alla prima `AlignInit` dopo l'upgrade, se esiste il vecchio `partenza-align-seq` e non esiste il nuovo `partenza-align-state`, leggi il vecchio seq, genera un `session_id = now_ms`, scrivi il nuovo file e cancella il vecchio.

### Wire format

**POST /events?session=&lt;config&gt;**

Body esteso:
```json
{
  "session_id": 1734567890123,
  "seq": 42,
  "type": "partenza",
  "numero": 5,
  "orario": "10:23:45.678",
  "ts_origine": "2026-05-13T10:23:45.678Z"
}
```

`session_id` è un intero ≥ 1 (Pydantic `Field(ge=1)`).

**GET /stream?session=&lt;config&gt;&since_sess=&lt;N&gt;&since=&lt;S&gt;**

Nuovo parametro `since_sess` (default 0). Il server restituisce eventi con `(sess, seq) > (since_sess, since)`. Backward compat non richiesta: deploy coordinato di server + entrambi i client.

Frame `meta` esteso:
```json
{
  "type": "meta",
  "min_sess": 1734567890123,
  "min_seq": 1,
  "max_sess": 1734567890123,
  "max_seq": 42,
  "server_time": "1715600000.123"
}
```

`min_sess`/`min_seq` = coppia dell'evento più vecchio in cache; `max_sess`/`max_seq` = del più recente. Se cache vuota, tutti e quattro a 0.

### Server (`server/app.py`)

**Modello `Event`**: aggiungere `session_id: int = Field(ge=1, description="Session id monotono generato dalla partenza")`.

**`EventCache._events`**: la lista interna resta `list[StoredEvent]` ma è ordinata per `(event.session_id, event.seq)`.

**`EventCache.add`**: dedup ora confronta la coppia `(session_id, seq)`:
```python
for i, stored in enumerate(bucket):
    if stored.event.session_id == event.session_id and stored.event.seq == event.seq:
        bucket[i] = StoredEvent(event=event, received_at=time.monotonic())
        break
else:
    bucket.append(StoredEvent(event=event, received_at=time.monotonic()))
    bucket.sort(key=lambda s: (s.event.session_id, s.event.seq))
```

**`EventCache.get_since`**: firma diventa `(session_config, since_sess, since_seq) -> (events, min_sess, min_seq, max_sess, max_seq)`. Filtro: `(s.event.session_id, s.event.seq) > (since_sess, since_seq)`.

**`StreamMeta`**: aggiungere `min_sess` e `max_sess`.

**Endpoint `/stream`**: leggere il nuovo `since_sess`, tracciare `last_seen` come tupla `(sess, seq)`, mandare il nuovo `meta`, applicare il filtro lessicografico nel loop.

**Endpoint `/events`**: nessuna modifica strutturale, ma il body validato include ora `session_id`.

### Partenza (`brrm-partenza/.src/FMain.class`)

**Variabile di istanza**: aggiungere `Private $alignSession As Long` (distinta da `$alignSession As String` di config — rinominare quella in `$alignSessionConfig` per evitare ambiguità).

**`AlignInit`**:

1. Se esiste `~/.local/share/brrm/partenza-align-state`: caricalo (formato a due righe `session=N\nseq=M`), assegna a `$alignSession` e `$alignSeq`. Done.
2. Altrimenti, se esiste il vecchio `partenza-align-seq` (migrazione): leggi il seq dal vecchio file, genera `$alignSession = Max(currentMillis(), 1)` immediatamente, scrivi il nuovo file con `(session=$alignSession, seq=$alignSeq)`, elimina il vecchio. La generazione subito qui è necessaria perché il seq migrato è > 0 e gli eventi futuri non triggerebbero la regola "seq riparte da 1".
3. Altrimenti: `$alignSession = 0`, `$alignSeq = 0`, nessuna scrittura. Sarà il primo `AlignEnqueue` a generare e persistere.

**`AlignEnqueue`**, regola di generazione:

> Prima di incrementare `$alignSeq`: se `$alignSeq == 0`, calcola `$alignSession = Max(currentMillis(), $alignSession + 1)`.

Questo copre sia il fresh start (file mai esistito, `$alignSession = 0` → formula dà `currentMillis()`), sia futuri casi in cui qualcosa azzeri il seq mantenendo in memoria il vecchio session_id (formula dà `last + 1` come fallback se l'orologio è tornato indietro). Dopo la generazione, incrementa il seq, includi `session_id` nel JSON, salva `partenza-align-state` atomicamente con entrambi i valori.

Rationale: il design garantisce "session_id nuovo ⇔ seq riparte da 1". Generare al primo `AlignEnqueue` post-init evita di bruciare un session_id se la partenza viene aperta e chiusa senza emettere eventi.

**`AlignReinit`**: resetta `$alignSession = 0` insieme a `$alignSeq = 0` (poi `AlignInit` ricarica entrambi dal file se esiste).

**Outbox**: nessun cambiamento. Gli eventi pendenti già includono il `session_id` con cui sono stati emessi (catturato al momento dell'enqueue). Vengono spediti così come sono.

### Arrivo (`brrm/.src/FMain.class`)

**Variabili di istanza**: aggiungere `Private $alignLastSession As Long` (accoppiato al `$alignLastSeq` esistente).

**`AlignInit`**:

1. Carica `~/.local/share/brrm/arrivo-align-state` (formato `session=N\nseq=M`). Migrazione dal vecchio `arrivo-align-lastseq` (solo seq, session=0).
2. Se non esiste: entrambi a 0.

**Query stream**: `URL = $alignUrl & "/stream?session=" & $alignSessionConfig & "&since_sess=" & CStr($alignLastSession) & "&since=" & CStr($alignLastSeq)`.

**Gestione `meta`**: gap detection lessicografica. Se `(min_sess, min_seq)` è popolato (entrambi > 0) e `(last_session, last_seq) < (min_sess, min_seq - 1)` (con il `-1` applicato al seq nella coppia minima), riallinea a `(min_sess, min_seq - 1)`. Caso degenere `min_seq == 1`: riallinea a `(min_sess, 0)`.

```
' Pseudo-codice
If iMinSess > 0 And If iMinSeq > 0 Then
  If $alignLastSession < iMinSess Or If ($alignLastSession = iMinSess And $alignLastSeq < iMinSeq - 1) Then
    $alignLastSession = iMinSess
    $alignLastSeq = iMinSeq - 1
    SaveAlignState()
  Endif
Endif
```

**Gestione `event`**: dedup lessicografica.
```
' Pseudo-codice
Try iSess = CLong(ev["session_id"])
Try iSeq = CLong(ev["seq"])
If iSess < $alignLastSession Then Return
If iSess = $alignLastSession And If iSeq <= $alignLastSeq Then Return
' applica l'evento
$alignLastSession = iSess
$alignLastSeq = iSeq
SaveAlignState()
```

Niente reset di `partenze[]` o `LEquipaggi` allo switch di sessione: lo stato applicativo è preservato per scelta progettuale.

## Test plan

Test manuale, su test machine Gambas 3 (boxrally@192.168.1.171) o ambiente dev locale via Docker.

1. **Scenario base post-fix**: partenza emette eventi 1..5 con session_id S1, arrivo li riceve. Cancello manualmente `partenza-align-seq` (simulando il reset), riavvio partenza, emetto un evento → deve avere session_id S2 > S1 e seq=1, arrivo lo riceve e applica.

2. **Monotonia con orologio indietro**: come sopra ma sposto manualmente l'orologio della partenza indietro di un'ora prima del reset → S2 deve essere comunque > S1 (grazie al `last_session + 1`).

3. **Backward gap (TTL server)**: partenza emette eventi 1..10 con S1, arrivo si disconnette, server TTL svuota la cache, partenza emette 11..15 con S1, arrivo riconnette → meta dà `min_sess=S1, min_seq=11`, arrivo riallinea a `(S1, 10)`, accetta 11..15. (Scenario senza cambio sessione, verifica che il fix non rompa il comportamento esistente.)

4. **Eventi tardivi vecchia sessione**: con S2 attivo lato arrivo, simulo un POST tardivo con `(S1, X)` → server lo accetta in cache, arrivo lo ignora silenziosamente.

5. **Outbox attraverso il reset**: partenza offline ha 3 eventi in outbox con S1, viene resettato il seq (e si genera S2), 2 nuovi eventi in outbox con S2 → tutti spediti, arrivo accetta in ordine.

6. **Test unitari `brrm-utils-test`**: se vengono estratti helper puri per la generazione/serializzazione del session_id, aggiungere test in linea con il pattern esistente (`gb.test`, `Assert.Equals`).

## Out of scope

- Backward compat con server o client vecchi. Deploy coordinato.
- UI: il `session_id` non è mai esposto all'utente né in settings né in label.
- Cleanup automatico di `session_id` molto vecchi lato cache server: il TTL esistente di 5 minuti li elimina già.
- Modifica della semantica di `session` di config.
