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

Entrambi usano le componenti `gb.qt6`, `gb.form`, `gb.net`. Il codice originale era Gambas 2 (su `gb.qt`, `gb.qt.ext`); la migrazione a Gambas 3 + Qt6 vive sul branch `migrate-gambas3`.

## Architettura

```
   ┌──────────────┐  fotocellula   ┌──────────────┐
   │  Fotocellula │ ───trigger───▶ │   Arduino    │
   └──────────────┘                └──────┬───────┘
                                          │ seriale (/dev/ttyUSB0)
                                          ▼
                                   ┌──────────────┐
                                   │   brrm       │  ← Linux + Gambas 2
                                   │  (PC gara)   │
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
- **MODIFICA ORDINE** — riordino manuale della lista equipaggi (sposta su / sposta giù / rimuovi).
- **ESPORTA** — esportazione in CSV con dialog di selezione file via `zenity` (formato: `'numero','minuti','secondi','centesimi'`).
- **RESET!** — azzera l'intera sessione, con doppia conferma.
- **Indicatore IN ARRIVO** — etichetta grande che mostra il numero del prossimo equipaggio atteso (primo senza arrivo registrato).
- **Persistenza sessione** — lo stato viene salvato continuamente in `~/.brrm-session` e ricaricato all'avvio.

## Funzionalità — postazione partenza (`brrm-partenza/`)

Versione ridotta dedicata al solo registro partenze.

- Griglia a 2 colonne: **Equipaggio · Tempo di partenza**.
- Coda fotocellula e gestione FotON/FotOFF identica alla postazione arrivo.
- **Annulla partenza** contestuale e **RESET** della sessione.
- Persistenza in `~/.brrm-partenza-session`.
- *Non implementati* (presenti in UI ma vuoti): pulsante Esporta, modifica ordine.

## Sync partenza ↔ arrivo via Internet (opzionale)

Quando le due postazioni **non sono sulla stessa LAN**, è possibile sincronizzarle attraverso il servizio bridge **brrm-align** (vedi `server/`). La sincronizzazione è **opzionale** e **fire-and-forget**: un guasto di rete non blocca mai la registrazione locale. Un'etichetta `SYNC OK` / `SYNC FAIL` accanto allo stato Arduino indica lo stato del bridge.

Architettura: la postazione partenza pubblica gli eventi su `POST /events?session=…` (idempotente, sequence number monotono, outbox locale per ritrasmissione), la postazione arrivo li riceve via SSE long-poll su `GET /stream?session=…&since=<lastSeq>` con riconnessione automatica (backoff esponenziale 1→60 s) e watchdog di 15 s. Il bridge mantiene una cache in-memory con TTL 5 min; gli stati locali sono persistiti in `~/.brrm-partenza-outbox`, `~/.brrm-partenza-align-seq`, `~/.brrm-align-lastseq`.

Per attivare il sync, creare su **entrambe le postazioni** il file `~/.config/brrm/align.conf` con lo stesso `session` e le stesse credenziali:

```ini
url=https://www.boxrally.eu/brrm-align
username=brrm
password=<password>
session=gara-2026-05-11
```

Senza il file, l'etichetta `SYNC` resta nascosta e il software funziona esattamente come prima.

## Dettagli tecnici

- **Porta seriale hardcoded:** `/dev/ttyUSB0` (Arduino USB su Linux).
- **Capienza:** fino a 10000 equipaggi/numeri (array statici).
- **Numeri validi:** 1 — 9999.
- **Formato sessione persistente:** record concatenati da `-`, ognuno `numero orario_partenza orario_arrivo`, salvati via `SHELL "echo … > file"`.
- **Helper `twoDecs`** — normalizza i centesimi a esattamente 2 cifre per l'export CSV.
- **Polling UI:** un `Timer1` aggiorna la griglia ogni 91 ms.

## Requisiti

- Linux (per `/dev/ttyUSB0` e `zenity`)
- Gambas 2 (testato con 2.19 — 2.23)
- Arduino collegato via USB con firmware che emetta un byte sulla seriale al trigger fotocellula
- `zenity` installato (per il dialog di export CSV)

## Come si esegue

Aprire la cartella del progetto desiderato (`brrm/` o `brrm-partenza/`) nell'IDE Gambas 2 e premere "Esegui", oppure compilare con `gbc2` / `gba2` ed eseguire l'eseguibile prodotto.
