# brrm-align

Bridge HTTP fra postazione partenza e postazione arrivo quando i due PC non sono sulla stessa rete locale. Mantiene un buffer in-memory degli eventi di partenza per 5 minuti.

## Protocollo

### `POST /events?session=<id>`

Pubblica un evento (chiamato da brrm-partenza). Body JSON:

```json
{
  "seq": 42,
  "type": "partenza",
  "numero": 5,
  "orario": "13:40:00.123",
  "ts_origine": "2026-05-11T12:40:00.123Z"
}
```

- `seq`: sequence number monotono per-sessione (il client lo genera). Idempotente: pubblicando lo stesso `seq` due volte, la seconda sovrascrive.
- `orario`: `null` = annullamento.

Risposta: `{ "ack": true, "seq": 42 }`.

### `GET /stream?session=<id>&since=<seq>`

Server-Sent Events. Restituisce subito tutti gli eventi con `seq > since` presenti in cache, poi tiene la connessione aperta. Per ogni nuovo evento, manda un frame `event: event`. Ogni 10 s manda un frame `event: keepalive` per permettere al client di rilevare disconnessioni di rete.

Il primo frame è sempre `event: meta` con `{min_seq, max_seq, server_time}`: se il client manda `since < min_seq`, sa di aver perso eventi (TTL scaduta) e può rifare snapshot completo da `min_seq`.

## Auth

HTTP Basic. Credenziali via env: `BRRM_ALIGN_USER` (default `brrm`) e `BRRM_ALIGN_PASSWORD` (obbligatorio).

## Deploy

Si gira via Docker Compose dietro Traefik. Crea un file `.env` accanto a `compose.yaml`:

```ini
BRRM_ALIGN_USER=brrm
BRRM_ALIGN_PASSWORD=cambialamipls
```

Poi:

```sh
docker compose up -d --build
```

Le label Traefik instradano `https://www.boxrally.eu/brrm-align/*` al container (porta 8000) con `StripPrefix` per il path. Richiede una rete Docker esterna `traefik` già esistente sul host (creata dallo stack Traefik).

Healthcheck pubblico (senza auth): `GET /brrm-align/health`.

## Sviluppo locale

```sh
python -m venv .venv && source .venv/bin/activate
pip install -r requirements.txt
BRRM_ALIGN_PASSWORD=test uvicorn app:app --reload --port 8000
```
