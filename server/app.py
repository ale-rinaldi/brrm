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
from __future__ import annotations

import asyncio
import os
import secrets
import time
from collections import defaultdict
from contextlib import asynccontextmanager
from typing import Optional

from fastapi import Depends, FastAPI, HTTPException, Query, Request, status
from fastapi.responses import JSONResponse
from fastapi.security import HTTPBasic, HTTPBasicCredentials
from pydantic import BaseModel, Field
from sse_starlette.sse import EventSourceResponse

# --- Config -----------------------------------------------------------------

USERNAME = os.environ.get("BRRM_ALIGN_USER", "brrm")
PASSWORD = os.environ.get("BRRM_ALIGN_PASSWORD")
if not PASSWORD:
    raise RuntimeError("BRRM_ALIGN_PASSWORD env var non impostata")

EVENT_TTL_SEC = float(os.environ.get("BRRM_ALIGN_TTL_SEC", "300"))  # 5 min
KEEPALIVE_SEC = float(os.environ.get("BRRM_ALIGN_KEEPALIVE_SEC", "10"))
POLL_INTERVAL_SEC = 0.5  # quanto spesso lo stream loop controlla nuovi eventi

# --- Modelli ----------------------------------------------------------------


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


class StoredEvent(BaseModel):
    event: Event
    received_at: float  # monotonic time per TTL


class StreamMeta(BaseModel):
    """Metadata mandato come primo evento SSE su ogni connessione."""

    type: str = "meta"
    min_sess: int = Field(description="session_id piu' vecchio in cache (0 se vuota)")
    min_seq: int = Field(description="seq dell'evento piu' vecchio in cache")
    max_sess: int = Field(description="session_id piu' recente in cache")
    max_seq: int = Field(description="seq dell'evento piu' recente in cache")
    server_time: str


# --- Storage ----------------------------------------------------------------


class EventCache:
    """Cache in-memory per-sessione con TTL e notifica async sui nuovi eventi."""

    def __init__(self):
        # session_id -> list[StoredEvent] ordinata per seq crescente
        self._events: dict[str, list[StoredEvent]] = defaultdict(list)
        # session_id -> asyncio.Event triggered ogni volta che arriva un evento
        self._notifiers: dict[str, asyncio.Event] = defaultdict(asyncio.Event)
        self._lock = asyncio.Lock()

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

    def notifier_for(self, session_id: str) -> asyncio.Event:
        return self._notifiers[session_id]

    def invalidate_for_tests(self) -> None:
        """Resetta la cache. SOLO per uso nei test."""
        self._events.clear()
        self._notifiers.clear()

    def _evict_expired(self, session_id: str) -> None:
        """Rimuove eventi piu' vecchi di EVENT_TTL_SEC. Chiamare con lock acquisito."""
        bucket = self._events.get(session_id)
        if not bucket:
            return
        cutoff = time.monotonic() - EVENT_TTL_SEC
        # Trova primo indice non scaduto (lista ordinata per seq, ma il received_at
        # non e' strettamente monotono se arrivano eventi out-of-order; uso filter).
        kept = [s for s in bucket if s.received_at >= cutoff]
        if len(kept) != len(bucket):
            self._events[session_id] = kept


cache = EventCache()


# --- Auth -------------------------------------------------------------------

security = HTTPBasic()


def authenticate(credentials: HTTPBasicCredentials = Depends(security)) -> str:
    user_ok = secrets.compare_digest(credentials.username, USERNAME)
    pass_ok = secrets.compare_digest(credentials.password, PASSWORD)
    if not (user_ok and pass_ok):
        raise HTTPException(
            status_code=status.HTTP_401_UNAUTHORIZED,
            detail="Invalid credentials",
            headers={"WWW-Authenticate": "Basic"},
        )
    return credentials.username


# --- App --------------------------------------------------------------------


@asynccontextmanager
async def lifespan(app: FastAPI):
    """Eviction periodica della cache: ogni 60s rimuove eventi oltre TTL."""
    async def evictor():
        while True:
            await asyncio.sleep(60)
            async with cache._lock:
                for session_id in list(cache._events.keys()):
                    cache._evict_expired(session_id)
                    if not cache._events[session_id]:
                        del cache._events[session_id]

    task = asyncio.create_task(evictor())
    try:
        yield
    finally:
        task.cancel()
        try:
            await task
        except asyncio.CancelledError:
            pass


app = FastAPI(
    title="brrm-align",
    description="Bridge partenza<->arrivo via Internet",
    lifespan=lifespan,
)


@app.get("/health")
async def health():
    """Endpoint pubblico senza auth, per healthcheck del container."""
    return {"status": "ok"}


@app.get("/ping")
async def ping(_user: str = Depends(authenticate)):
    """Probe veloce per i client (testa rete + credenziali). Risposta minima."""
    return {"ok": True}


@app.post("/events")
async def post_event(
    event: Event,
    session: str = Query(default="default"),
    _user: str = Depends(authenticate),
):
    await cache.add(session, event)
    return {"ack": True, "seq": event.seq}


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


