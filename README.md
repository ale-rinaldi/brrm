# BoxRally Race Manager (brrm)

Software di cronometraggio per le gare di **Soap Box** del campionato **BoxRally**, scritto in **Gambas 2**.
Il sistema è progettato per funzionare in coppia con un **Arduino** che pilota una fotocellula, e si compone di due applicazioni distinte, una per ciascuna postazione di gara.

## Componenti del progetto

Il repository contiene due progetti Gambas indipendenti:

| Cartella | Ruolo | Versione | Compilato con |
|---|---|---|---|
| `brrm/` | Postazione **arrivo** (cronometraggio completo) | 0.0.6 | Gambas 2.23.1 |
| `brrm-partenza/` | Postazione **partenza** | 0.0.4 | Gambas 2.19.0 |

Entrambi usano le librerie `gb.qt`, `gb.form`, `gb.net`, `gb.qt.ext`.

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
