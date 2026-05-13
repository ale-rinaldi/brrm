"""Unit test per EventCache e modello Event con session_id."""
import asyncio
import contextlib
import os
import threading
import time

import anyio
import httpx
import pytest
import uvicorn

# Import-time side effect: app.py richiede BRRM_ALIGN_PASSWORD.
os.environ.setdefault("BRRM_ALIGN_PASSWORD", "test")

from app import Event, EventCache, app, cache  # noqa: E402

AUTH = ("brrm", "test")
_SERVER_PORT = 18765


@contextlib.contextmanager
def _live_server():
    """Avvia uvicorn su localhost per test SSE end-to-end. Cleanup auto."""
    config = uvicorn.Config(app, host="127.0.0.1", port=_SERVER_PORT, log_level="error")
    server = uvicorn.Server(config)
    thread = threading.Thread(target=server.run, daemon=True)
    thread.start()
    # Aspetta che il server sia pronto.
    for _ in range(30):
        try:
            httpx.get(f"http://127.0.0.1:{_SERVER_PORT}/health", timeout=0.2)
            break
        except Exception:
            time.sleep(0.1)
    try:
        yield f"http://127.0.0.1:{_SERVER_PORT}"
    finally:
        server.should_exit = True
        thread.join(timeout=3)


def _run(coro):
    return asyncio.run(coro)


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


@pytest.mark.anyio
async def test_post_and_stream_meta_includes_session():
    """meta del /stream espone min_sess, max_sess oltre a min_seq/max_seq."""
    cache.invalidate_for_tests()
    with _live_server() as base_url:
        async with httpx.AsyncClient(base_url=base_url, auth=AUTH) as client:
            body = {
                "session_id": 1700000000000,
                "seq": 1,
                "type": "partenza",
                "numero": 5,
                "orario": "10:00:00.000",
                "ts_origine": "2026-05-13T10:00:00.000Z",
            }
            r = await client.post("/events?session=test", json=body)
            assert r.status_code == 200

            # Stream con since_sess=0, since=0: dovrebbe restituire meta + evento.
            data = b""
            with anyio.move_on_after(5):
                async with client.stream(
                    "GET", "/stream?session=test&since_sess=0&since=0"
                ) as r:
                    assert r.status_code == 200
                    async for chunk in r.aiter_bytes():
                        data += chunk
                        if b"min_sess" in data and b"1700000000000" in data:
                            break

            assert b"min_sess" in data
            assert b"max_sess" in data
            assert b"1700000000000" in data


@pytest.mark.anyio
async def test_stream_since_sess_filters_old_session():
    """since_sess > 0 esclude eventi di session_id inferiori."""
    cache.invalidate_for_tests()
    with _live_server() as base_url:
        async with httpx.AsyncClient(base_url=base_url, auth=AUTH) as client:
            for body in [
                {
                    "session_id": 100, "seq": 1, "type": "partenza", "numero": 1,
                    "orario": "10:00:00.000", "ts_origine": "2026-05-13T10:00:00.000Z",
                },
                {
                    "session_id": 200, "seq": 1, "type": "partenza", "numero": 2,
                    "orario": "10:00:01.000", "ts_origine": "2026-05-13T10:00:01.000Z",
                },
            ]:
                (await client.post("/events?session=test2", json=body)).raise_for_status()

            data = b""
            with anyio.move_on_after(5):
                async with client.stream(
                    "GET", "/stream?session=test2&since_sess=100&since=1"
                ) as r:
                    async for chunk in r.aiter_bytes():
                        data += chunk
                        if b'"numero":2' in data:
                            break

            assert b'"numero":2' in data
            assert b'"numero":1' not in data
