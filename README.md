# BoxRally Race Manager (brrm)

> ⚠️ **Software amatoriale.** Questo è un progetto personale nato per gestire il cronometraggio delle gare BoxRally: non ha alcuna pretesa di efficienza, completezza o correttezza, ed è pubblicato così com'è. Usalo a tuo rischio e nessuna garanzia è data o implicata.

Software di cronometraggio per le gare di **Soap Box** del campionato **BoxRally**, scritto originariamente in **Gambas 2** e migrato a **Gambas 3 + Qt6**.
Il sistema è progettato per funzionare in coppia con un **Arduino** che pilota una fotocellula, e si compone di due applicazioni distinte, una per ciascuna postazione di gara.

## Installazione (Debian / Ubuntu)

I `.deb` precompilati sono pubblicati su un repository APT ospitato su GitHub Pages, aggiornato ad ogni push su `main` (versionamento: `<ver-base>+YYYYMMDDHHMM.<short-sha>`).

Richiede una distro che fornisca Gambas 3 + Qt6 (es. Debian 13 trixie o successive):

```sh
sudo install -d /etc/apt/keyrings
curl -fsSL https://ale-rinaldi.github.io/brrm/brrm-apt-key.asc \
  | sudo gpg --dearmor -o /etc/apt/keyrings/brrm-apt.gpg
echo "deb [signed-by=/etc/apt/keyrings/brrm-apt.gpg] https://ale-rinaldi.github.io/brrm ./" \
  | sudo tee /etc/apt/sources.list.d/brrm.list
sudo apt update
sudo apt install brrm brrm-partenza
```

Pagina di landing: <https://ale-rinaldi.github.io/brrm/>.

## Componenti del progetto

Il repository contiene due progetti Gambas indipendenti:

| Cartella | Ruolo | Versione |
|---|---|---|
| `brrm/` | Postazione **arrivo** (cronometraggio completo) | 0.0.6 |
| `brrm-partenza/` | Postazione **partenza** | 0.0.4 |

Entrambi usano le componenti `gb.qt6`, `gb.form`, `gb.net`, `gb.net.curl`. Il codice originale era Gambas 2 (su `gb.qt`, `gb.qt.ext`); la migrazione a Gambas 3 + Qt6 è ora su `main`. La firma RSA-SHA256 dei JWT per Google Sheets richiede `openssl` da CLI (dipendenza del `.deb`).

## Architettura

```
   ┌──────────────┐  fotocellula   ┌──────────────┐
   │  Fotocellula │ ───trigger───▶ │   Arduino    │
   └──────────────┘                └──────┬───────┘
                                          │ seriale (/dev/ttyUSB* o /dev/ttyACM*)
                                          ▼
                                   ┌──────────────┐
                                   │  brrm /      │  ← Linux + Gambas 3 + Qt6
                                   │  brrm-       │
                                   │  partenza    │
                                   └──────────────┘
```

Ogni evento seriale ricevuto dall'Arduino viene interpretato come un passaggio in fotocellula: l'orario di sistema viene messo in coda (`BArrivi`) e, quando l'operatore lo gestisce, può essere associato al numero dell'equipaggio.

## Funzionalità — postazione arrivo (`brrm/`)

Griglia principale con 4 colonne: **Equipaggio · Partenza · Arrivo · Tempo**.

- **ARRIVO** / **PARTENZA** — finestre dedicate per inserire numero e orario.
- **Coda fotocellula** — gli eventi seriali alimentano una coda di arrivi che apre automaticamente la finestra `FArrivo` per l'associazione al numero equipaggio.
- **FotON / FotOFF** — abilita/disabilita la registrazione automatica dalla fotocellula (utile per testare o sospendere). In modalità OFF il pulsante lampeggia rosso.
- **Annulla partenza / Annulla arrivo** — selezione contestuale: si clicca il pulsante, poi la riga in griglia.
- **Inverti arrivo** — scambio degli orari di arrivo tra due equipaggi (per correggere assegnazioni errate).
- **Cambia numero** — rinomina locale il numero equipaggio di una riga (sposta partenza+arrivo, valida che il nuovo numero non esista). Operazione solo locale: non viene propagata a brrm-partenza.
- **MODIFICA ORDINE** — riordino manuale della lista equipaggi (sposta su / sposta giù / rimuovi).
- **Importa partenze** — carica orari di partenza da CSV (utile quando il sync via web non è disponibile e si lavora offline con scambio file).
- **ESPORTA** — esportazione risultati in CSV (formato `numero,hh,mm,ss,mmm` — durata gara con precisione al millisecondo, calcolata in int).
- **RESET!** — azzera l'intera sessione, con doppia conferma.
- **Indicatore IN ARRIVO** — etichetta grande che mostra il numero del prossimo equipaggio atteso (primo senza arrivo registrato).
- **Tempo griglia** — durata gara aggiornata in tempo reale, formato `mm:ss.mmm`.
- **Impostazioni…** — apre la form di configurazione (sync + foglio Google).
- **Persistenza sessione** — lo stato viene salvato continuamente in `~/.brrm-session` (formato v1, scrittura atomica) e ricaricato all'avvio.

## Funzionalità — postazione partenza (`brrm-partenza/`)

Versione ridotta dedicata al solo registro partenze.

- Griglia a 2 colonne: **Equipaggio · Tempo di partenza**.
- Coda fotocellula e gestione FotON/FotOFF identica alla postazione arrivo.
- **PARTENZA** — registra l'orario corrente, apre il dialog per associare il numero equipaggio.
- **Annulla partenza** contestuale.
- **Cambia numero** — riassegna una partenza ad un equipaggio diverso (verifica unicità; emette annullamento del vecchio + nuovo evento sincronizzato).
- **Inverti partenze** — scambia gli orari di due equipaggi (stesso pattern UX di *Inverti arrivo* su brrm; sincronizza entrambi gli orari swappati).
- **ESPORTA** — esportazione orari in CSV (formato `numero,hh,mm,ss,mmm`).
- **RESET!** — azzera la sessione (con doppia conferma).
- **Impostazioni…** — apre la form di configurazione.
- Persistenza in `~/.brrm-partenza-session` (formato v1, scrittura atomica).

## Sync partenza ↔ arrivo via Internet (opzionale)

Quando le due postazioni **non sono sulla stessa LAN**, è possibile sincronizzarle attraverso il servizio bridge **brrm-align** (vedi `server/`). La sincronizzazione è **opzionale** e **fire-and-forget**: un guasto di rete non blocca mai la registrazione locale. Un'etichetta `SYNC OK` (verde) / `SYNC FAIL` (rosso) accanto allo stato Arduino indica lo stato del bridge; passandoci sopra il mouse il tooltip mostra URL, session e il dettaglio dell'ultimo esito (es. `Probe OK`, `Errore di rete`, `HTTP 401`).

**Eventi sincronizzati:** registrazione partenza, annullamento partenza, cambio numero equipaggio (annullamento + nuovo), inversione di due partenze (entrambi i nuovi orari).

**Architettura:**
- Partenza → eventi via `POST /events?session=…` (idempotente, seq monotono, outbox locale persistito per ritrasmissione).
- Arrivo ← stream `GET /stream?session=…&since=<lastSeq>` (Server-Sent Events long-poll, riconnessione con backoff esponenziale 1→60 s, watchdog 15 s sui keepalive del server).
- Probe iniziale (`GET /ping` autenticato) all'avvio + heartbeat 30 s lato partenza per rilevare drop silenziosi anche quando non si stanno facendo eventi: la label SYNC resta nascosta finché il probe non dà un esito definitivo (niente flicker rosso prematuro).
- Cache server in-memory con TTL 5 min; in caso di gap (TTL scaduta) la postazione arrivo si allinea automaticamente al `min_seq` corrente.

Persistenza locale: `~/.brrm-partenza-outbox`, `~/.brrm-partenza-align-seq`, `~/.brrm-align-lastseq` (tutte scrittura atomica).

**Configurazione** — dalla form *Impostazioni…* in entrambe le app, oppure scrivendo direttamente `~/.config/brrm/align.conf`:

```ini
url = https://www.boxrally.eu/brrm-align
username = brrm
password = <password>
session = gara-2026-05-11
```

La stessa `session` deve essere usata sia su partenza che su arrivo. Senza il file (o con campi vuoti), l'etichetta `SYNC` resta nascosta e il software funziona esattamente come prima.

## Scrittura su Google Sheets (opzionale)

Lato **arrivo**, brrm può aggiornare un foglio Google in tempo reale ad ogni evento (registrazione/annullamento partenza, registrazione/annullamento arrivo, cambio numero, inversione arrivi). Il foglio funge da classifica live condivisibile con organizzazione/pubblico senza esporre il software stesso. L'integrazione è opzionale, **fire-and-forget**: la scrittura è async, un guasto di rete non blocca mai brrm.

Una label `SHEET OK` (verde) / `SHEET FAIL` (rosso) sotto a `SYNC` indica lo stato; il tooltip riporta il dettaglio dell'ultimo esito.

**Eventi sincronizzati al foglio:** ogni `SetArrivo`, `SetPartenza` (anche da sync), annullamento, `Cambia numero`, `Inverti arrivi`. Idempotente: brrm cerca il numero equipaggio nella colonna configurata e o aggiorna la riga trovata, o appende sulla prima riga con cella equipaggio vuota.

**Autenticazione:** Service Account Google Cloud (JSON key). Il file JSON contiene `client_email` e una `private_key` RSA. brrm firma localmente un JWT RS256 (via `openssl dgst` — unica via in Gambas, gb.openssl non espone primitive di firma asimmetrica), lo scambia con Google per un access token OAuth2 con cache 1h, e usa quel token negli header `Authorization: Bearer …` delle chiamate REST a Sheets v4 API. Niente flusso browser, niente refresh token. Per creare il SA: Google Cloud Console → enable Sheets API → Service Account → Keys → JSON, poi condividere il foglio col `client_email` del SA con ruolo Editor.

**Affidabilità:** ogni errore HTTP (4xx, 5xx, timeout, rete giù) schedula un retry con backoff esponenziale 1→60 s. La coda eventi in memoria non viene svuotata su fallimento — gli eventi rimangono pending finché non passa. Il token scaduto (401) viene invalidato e ri-richiesto trasparentemente.

**Configurazione** — dalla form *Impostazioni…* o scrivendo `~/.config/brrm/sheet.conf`:

```ini
credentials = /percorso/al/service-account.json
document_id = <ID del foglio (segmento URL fra /d/ e /edit)>
sheet_name = <nome della scheda>
equipaggio_column = A
# Colonne opzionali (vuoto = disattivata). I range "split" indicano la prima
# colonna; le tre o quattro successive vengono usate in sequenza.
partenza_split_column = B      # B,C,D,E = h,m,s,ms
partenza_unixms_column = F     # singola colonna con timestamp UNIX in ms
arrivo_split_column = G        # G,H,I,J = h,m,s,ms
arrivo_unixms_column = K
tempo_split_column = L         # L,M,N = m,s,ms (m può superare 60)
tempo_ms_column = O            # totale durata in ms
```

Senza il file (o senza `credentials`/`document_id`/`sheet_name`/`equipaggio_column`), la label `SHEET` resta nascosta.

**Precisione:** tutta la matematica dei timestamp e dei delta è su interi (millisecondi). I valori scritti sul foglio coincidono al ms con quelli mostrati nella UI di brrm.

## Dettagli tecnici

- **Porta seriale:** auto-discovery del primo `/dev/ttyUSB*` o `/dev/ttyACM*` disponibile; `Timer4` controlla ogni 2 s e ricollega in caso di disconnessione.
- **Capienza:** fino a 10000 equipaggi/numeri (array statici).
- **Numeri validi:** 1 — 9999.
- **Formato sessione persistente:** file `v1` con header magic + righe `numero|orario_partenza|orario_arrivo` (su `brrm`) o `numero|orario_partenza` (su `brrm-partenza`), scrittura atomica tramite tmp + `rename(2)`.
- **Millisecondi:** catturati via `Format$(Time, "hh:nn:ss.uuu")` al momento dell'evento (Gambas `Str$` su Date time-only li tronca, quindi servono Format/uuu o math sulla rappresentazione interna). I delta tempo sono calcolati in int da `Hour*3.6e6 + Minute*60000 + Second*1000 + millis`, niente float.
- **Limite cross-midnight:** `partenze[]` e `arrivi[]` sono `Date` *time-only* (HH:MM:SS.zzz, niente data). La differenza arrivo - partenza presume durata gara < 24h: se il time-of-day dell'arrivo e' minore di quello della partenza, brrm interpreta come "arrivo dopo mezzanotte" e somma 86_400_000 ms al delta. Lo stesso per l'UNIX timestamp scritto sul foglio: se l'orario di un evento e' successivo all'ora corrente di Now(), brrm assume che sia stato registrato il giorno prima. **Funziona finche' la gara dura < 24h** (sufficiente per Soap Box). Gare piu' lunghe richiederebbero di memorizzare full datetime invece di solo time-of-day.
- **Polling UI:** `Timer1` aggiorna la griglia ogni 91 ms.
- **Priorità real-time:** lo script di lancio installato dal pacchetto `.deb` usa `chrt -f 50` quando possibile (vedi `package.sh`), con permessi configurati via `/etc/security/limits.d/`.

## Requisiti

- Linux (per i device `/dev/tty*`)
- Gambas 3 + Qt6 (es. Debian 13 trixie con `gambas3-runtime`, `gambas3-gb-qt6`, `gambas3-gb-form`, `gambas3-gb-net`, `gambas3-gb-net-curl`)
- `openssl` CLI (per la firma JWT del flusso Google Sheets — dichiarato come dipendenza del `.deb`)
- Arduino collegato via USB con firmware che emetta un byte sulla seriale al trigger fotocellula (opzionale: senza Arduino l'app gira ma la coda fotocellula resta vuota)

## Come si esegue

Da pacchetto installato (`apt install brrm brrm-partenza`): voce di menu *BRRM* / *BRRM Partenza* o da terminale `brrm` / `brrm-partenza`.

Da sorgente: `./build.sh` compila entrambi i progetti producendo `brrm/brrm.gambas` e `brrm-partenza/brrm-partenza.gambas`, eseguibili con `gbr3 <path>.gambas`. In alternativa aprire il progetto nell'IDE `gambas3` e premere *Esegui*.
