// Unit test della logica pura del cronometro (cronometro_logic.h).
// Compilazione/esecuzione: ./run.sh  (oppure: c++ -std=c++17 test_logic.cpp && ./a.out)
//
// Il test piu' importante e' quello su capturedUnix: un SIMULATORE FISICO
// indipendente ricostruisce lo stato osservabile nella ISR (ref, TCNT1, OCF1A)
// a partire da uno scenario fisico (istante del fronte, latenza della ISR,
// latenza di servizio della COMPA) e verifica che capturedUnix restituisca
// SEMPRE il secondo giusto, anche a cavallo del rollover.

#include "../cronometro_logic.h"
#include <cstdio>
#include <cstdint>

static int g_fail = 0;
static int g_checks = 0;

#define CHECK(cond, msg) do {                                        \
    g_checks++;                                                      \
    if (!(cond)) { g_fail++; std::printf("FAIL: %s (%s:%d)\n", msg, __FILE__, __LINE__); } \
  } while (0)

#define CHECK_EQ(got, exp, msg) do {                                 \
    g_checks++;                                                      \
    long long _g = (long long)(got), _e = (long long)(exp);         \
    if (_g != _e) { g_fail++;                                        \
      std::printf("FAIL: %s: got %lld, expected %lld (%s:%d)\n", msg, _g, _e, __FILE__, __LINE__); } \
  } while (0)

// ---------------------------------------------------------------------------
// Simulatore fisico del confine del secondo in RTC_ACTIVE.
//
// Timeline in tick assoluti (32768 tick = 1 s). true_second(T) = T / 32768,
// tcnt(T) = T % 32768. La COMPA scatta al match (tcnt==32767, ossia
// T % 32768 == 32767) e incrementa unix_riferimento; la puo' servire in
// ritardo di L tick (interrupt disabilitati, es. SoftwareSerial). OCF1A e'
// alto dal match finche' la COMPA non e' servita.
//
//   base_unix : unix del secondo 0 della timeline
//   Te        : tick assoluto del fronte fotocellula
//   Tr        : tick assoluto in cui la ISR di capture legge lo stato (>= Te)
//   L         : latenza di servizio della COMPA (tick)
// Ritorna in out_* lo stato osservabile e in out_expected il secondo corretto.
static void simulaRtc(uint32_t base_unix, uint64_t Te, uint64_t Tr, uint64_t L,
                      uint32_t &ref, uint16_t &now, uint16_t &icr,
                      bool &ocf, uint32_t &expected) {
  icr = (uint16_t)(Te % 32768);
  now = (uint16_t)(Tr % 32768);
  expected = base_unix + (uint32_t)(Te / 32768);

  // Numero di match (tcnt==32767) avvenuti con tick <= Tr: match k al tick
  // 32767 + k*32768. ref = base_unix + numero di match SERVITI (tick+L <= Tr).
  uint64_t matches_serviti = 0;
  uint64_t k = 0;
  bool pending = false;
  while (true) {
    uint64_t match_tick = 32767ull + k * 32768ull;
    if (match_tick > Tr) break;
    if (match_tick + L <= Tr) matches_serviti++;   // COMPA gia' servita
    else pending = true;                            // match avvenuto, non servito
    k++;
  }
  ref = base_unix + (uint32_t)matches_serviti;
  ocf = pending;
}

static void test_capturedUnix_rtc_sweep() {
  const uint32_t base = 1700000000u;
  // Timeline attorno al confine tra il secondo 100 e il 101.
  const uint64_t T0 = 100ull * 32768ull;

  int casi = 0;
  // Fronte negli ultimi tick del secondo 100 e nei primi del 101.
  for (uint64_t Te = T0 + 32600; Te <= T0 + 32768 + 200; Te++) {
    // Latenza della ISR di capture dopo il fronte (0 .. ~2000 tick ~= 60ms:
    // copre abbondantemente i ~33 tick/ms di blocco di SoftwareSerial).
    for (uint64_t delay = 0; delay <= 2000; delay += 7) {
      uint64_t Tr = Te + delay;
      // Latenza di servizio della COMPA (sempre < 32768 -> al piu' 1 pending).
      static const uint64_t Ls[] = {0, 1, 2, 5, 40, 300, 2000, 20000};
      for (uint64_t L : Ls) {
        uint32_t ref, expected; uint16_t now, icr; bool ocf;
        simulaRtc(base, Te, Tr, L, ref, now, icr, ocf, expected);
        uint32_t got = capturedUnix(ref, now, icr, ocf, /*rtcMode=*/true);
        if (got != expected) {
          std::printf("FAIL rtc-sweep: Te=%llu delay=%llu L=%llu -> "
                      "ref=%u now=%u icr=%u ocf=%d got=%u exp=%u\n",
                      (unsigned long long)Te, (unsigned long long)delay,
                      (unsigned long long)L, ref, now, icr, (int)ocf, got, expected);
          g_fail++;
        }
        casi++;
      }
    }
  }
  g_checks += casi;
  std::printf("  capturedUnix RTC sweep: %d combinazioni fisiche verificate\n", casi);
}

// PPS_ACTIVE: reset di TCNT1 e unix_riferimento++ atomici alla ISR del PPS
// (nessuno skew, priorita' > CAPT). ref = base + numero di PPS avvenuti <= Tr.
static void test_capturedUnix_pps_sweep() {
  const uint32_t base = 1700000000u;
  const uint64_t T0 = 100ull * 32768ull;
  int casi = 0;
  for (uint64_t Te = T0 + 32600; Te <= T0 + 32768 + 200; Te++) {
    for (uint64_t delay = 0; delay <= 2000; delay += 7) {
      uint64_t Tr = Te + delay;
      uint16_t icr = (uint16_t)(Te % 32768);
      uint16_t now = (uint16_t)(Tr % 32768);
      uint32_t expected = base + (uint32_t)(Te / 32768);
      // PPS al tick k*32768 (wrap): ref = base + numero di wrap <= Tr.
      uint32_t ref = base + (uint32_t)(Tr / 32768);
      uint32_t got = capturedUnix(ref, now, icr, /*comparePending=*/false, /*rtcMode=*/false);
      if (got != expected) {
        std::printf("FAIL pps-sweep: Te=%llu delay=%llu -> ref=%u now=%u icr=%u got=%u exp=%u\n",
                    (unsigned long long)Te, (unsigned long long)delay, ref, now, icr, got, expected);
        g_fail++;
      }
      casi++;
    }
  }
  g_checks += casi;
  std::printf("  capturedUnix PPS sweep: %d combinazioni fisiche verificate\n", casi);
}

static void test_capturedUnix_casi_espliciti() {
  const uint32_t N = 1700000100u;  // "secondo N"
  // 1) Meta' secondo, nessun confine.
  CHECK_EQ(capturedUnix(N, 16000, 15000, false, true), N, "rtc mid-second");
  // 2) Fronte a fine secondo, ISR prima del rollover.
  CHECK_EQ(capturedUnix(N, 32760, 32750, false, true), N, "rtc fine-secondo pre-rollover");
  // 3) BUG STORICO: fronte a 32760, wrap avvenuto, COMPA NON servita
  //    (CAPT ha priorita' > COMPA). ref ancora N, now piccolo, ocf set.
  CHECK_EQ(capturedUnix(N, 5, 32760, true, true), N, "rtc wrap con COMPA pending (era -1s)");
  // 4) Stesso fronte, ma COMPA gia' servita (ref=N+1), ocf clear.
  CHECK_EQ(capturedUnix(N + 1, 5, 32760, false, true), N, "rtc wrap con COMPA servita");
  // 5a) Fronte NEL nuovo secondo (dopo il wrap), COMPA pending: now>=icr.
  CHECK_EQ(capturedUnix(N, 200, 100, true, true), N + 1, "rtc fronte nel nuovo secondo (pending)");
  // 5b) Fronte nel VECCHIO secondo ma ISR dopo il wrap con COMPA pending:
  //     now(100) < icr(200) -> il fronte e' del secondo precedente.
  CHECK_EQ(capturedUnix(N, 100, 200, true, true), N, "rtc fronte pre-wrap con COMPA pending");
  // 6) Tick esatto 32767, COMPA non servita (match, non ancora wrappato).
  CHECK_EQ(capturedUnix(N, 32767, 32767, true, true), N, "rtc tick 32767 ocf set");
  // 7) Tick esatto 32767, COMPA servita (ref avanti di 1, wrap 1 tick indietro).
  CHECK_EQ(capturedUnix(N + 1, 32767, 32767, false, true), N, "rtc tick 32767 ocf clear");
  // 8) ISR molto in ritardo: fronte nel secondo precedente (icr > now).
  CHECK_EQ(capturedUnix(N + 1, 3000, 30000, false, true), N, "rtc ISR ritardata, fronte sec. prec.");
  // PPS
  CHECK_EQ(capturedUnix(N, 16000, 15000, false, false), N, "pps mid-second");
  CHECK_EQ(capturedUnix(N + 1, 5, 32760, false, false), N, "pps wrap (reset+incr atomici)");
}

static void test_decideNmeaTime() {
  // Senza fix: NO-OP anche con orario NMEA assurdo (il bug del 2043).
  NmeaTimeDecision d = decideNmeaTime(2313941504u /*2043*/, /*fixUsable=*/false,
                                      /*inizializzato=*/true, 1783699875u /*2026*/);
  CHECK(!d.updateRef && !d.emitWarn && !d.setInitialized, "nmea no-fix = no-op (anti 2043)");

  // Con fix, non ancora inizializzato: bootstrap.
  d = decideNmeaTime(1783699875u, true, false, 0);
  CHECK(d.updateRef && d.newRef == 1783699875u && d.resetTcnt && d.setInitialized
        && d.setRtcActive && !d.emitWarn, "nmea bootstrap con fix");

  // Con fix, inizializzato, scarto 0 o -1: nessun aggiornamento.
  d = decideNmeaTime(1000, true, true, 1000);
  CHECK(!d.updateRef && !d.emitWarn, "nmea scarto 0 -> no-op");
  d = decideNmeaTime(1000, true, true, 1001);
  CHECK(!d.updateRef && !d.emitWarn, "nmea scarto -1 -> no-op");

  // Con fix, inizializzato, vero disallineamento: aggiorna + W.
  d = decideNmeaTime(2000, true, true, 1000);
  CHECK(d.updateRef && d.newRef == 2000 && d.emitWarn, "nmea desync -> update+warn");

  // Con fix ma NMEA farlocco 2043 mentre siamo a 2026: correggerebbe (con fix
  // vero e' un caso legittimo di risincronizzazione).
  d = decideNmeaTime(2313941504u, true, true, 1783699875u);
  CHECK(d.updateRef && d.emitWarn, "nmea con fix, grande scarto -> update+warn");
}

static void test_helpers() {
  CHECK(!nmeaFixUsable(false, 0), "fix: location non valida");
  CHECK(nmeaFixUsable(true, 0), "fix: valido e fresco");
  CHECK(nmeaFixUsable(true, 4999), "fix: 4999ms ok");
  CHECK(!nmeaFixUsable(true, 5000), "fix: 5000ms scaduto");

  CHECK(!rtcBootstrapYearOk(2023), "anno 2023 no");
  CHECK(rtcBootstrapYearOk(2024), "anno 2024 si");
  CHECK(rtcBootstrapYearOk(2043), "anno 2043 si");

  CHECK(syncAttraversaMinuto(60), "60 attraversa minuto");
  CHECK(syncAttraversaMinuto(120), "120 attraversa minuto");
  CHECK(!syncAttraversaMinuto(61), "61 non attraversa");
  CHECK(!syncAttraversaMinuto(1783699875u), "sec generico non attraversa");

  CHECK_EQ(osfCleared(0xFF), 0x7F, "osf 0xFF -> 0x7F");
  CHECK_EQ(osfCleared(0x00), 0x00, "osf 0x00 -> 0x00");
  CHECK_EQ(osfCleared(0x88), 0x08, "osf 0x88 -> 0x08");

  // BCD: 2026-07-10 16:00:09, venerdi (dow=5 in convenzione RTClib).
  uint8_t buf[7];
  packRtcBcd(9, 0, 16, 5, 10, 7, 26, buf);
  CHECK_EQ(buf[0], 0x09, "bcd sec");
  CHECK_EQ(buf[1], 0x00, "bcd min");
  CHECK_EQ(buf[2], 0x16, "bcd hour");
  CHECK_EQ(buf[3], 5,    "bcd dow venerdi");
  CHECK_EQ(buf[4], 0x10, "bcd day");
  CHECK_EQ(buf[5], 0x07, "bcd month");
  CHECK_EQ(buf[6], 0x26, "bcd year");
  // dow domenica (0) -> 7.
  packRtcBcd(0, 0, 0, 0, 1, 1, 24, buf);
  CHECK_EQ(buf[3], 7, "bcd dow domenica -> 7");
}

int main() {
  std::printf("== capturedUnix (confine del secondo) ==\n");
  test_capturedUnix_casi_espliciti();
  test_capturedUnix_rtc_sweep();
  test_capturedUnix_pps_sweep();
  std::printf("== decideNmeaTime ==\n");
  test_decideNmeaTime();
  std::printf("== helpers (fix/anno/minuto/osf/bcd) ==\n");
  test_helpers();

  std::printf("\n%d controlli, %d fallimenti\n", g_checks, g_fail);
  if (g_fail == 0) std::printf("TUTTI I TEST OK\n");
  return g_fail == 0 ? 0 : 1;
}
