#ifndef CRONOMETRO_LOGIC_H
#define CRONOMETRO_LOGIC_H

// =====================================================================
//  Logica decisionale PURA del cronometro GPS/RTC.
//
//  Nessuna dipendenza hardware (niente Wire/Serial/registri/ISR): tutte
//  le funzioni prendono valori gia' letti e restituiscono decisioni. Il
//  .ino applica gli effetti (I2C, Serial, registri Timer1) attorno a
//  queste decisioni. Cosi' la parte critica e' verificabile con unit test
//  sull'host (vedi test/test_logic.cpp), senza un simulatore AVR.
// =====================================================================

#include <stdint.h>

// TOP del Timer1 in RTC_ACTIVE (CTC): 0..32767 = 32768 tick = 1 secondo
// esatto a 32.768 kHz. In PPS_ACTIVE il TOP e' piu' alto (OCR1A=49151),
// quindi la correzione di confine RTC non va applicata (vedi capturedUnix).
static const uint16_t CRONO_TOP_RTC = 32767;

// --- Fix GPS utilizzabile -------------------------------------------------
// Un fix e' affidabile solo se la posizione e' valida e RECENTE. In
// TinyGPSPlus le proprieta' date/time restano "latched" (valide) anche dopo
// la perdita del fix, quindi da sole non bastano: un GPS senza fix emette
// comunque frasi NMEA con orario/data (spesso sfasati di ~20 anni per GPS
// week rollover) che non vanno MAI usate come tempo. locAgeMs = eta' in ms
// dell'ultimo fix di posizione.
inline bool nmeaFixUsable(bool locValid, uint32_t locAgeMs) {
  return locValid && locAgeMs < 5000;
}

// --- Decisione sul tempo NMEA ---------------------------------------------
struct NmeaTimeDecision {
  bool     updateRef;      // impostare unix_riferimento = newRef
  uint32_t newRef;
  bool     resetTcnt;      // azzerare TCNT1 (allineamento al bootstrap)
  bool     setInitialized; // tempo_inizializzato = true
  bool     setRtcActive;   // passare a RTC_ACTIVE (se in UNSYNCED/WAIT_RTC_SYNC)
  bool     emitWarn;       // emettere W<t_nmea>,<current> (desync)
};

// Decide cosa fare di un orario assoluto ricavato dall'NMEA.
//   fixUsable          : nmeaFixUsable(...) -- se falso la funzione e' un no-op
//   tempoInizializzato : abbiamo gia' un riferimento temporale valido?
//   current            : unix_riferimento attuale
// Senza fix NON si tocca il tempo: in fallback il riferimento resta quello
// dell'RTC (avanzato dal Timer1 a 32 kHz), che l'NMEA farlocco (no-fix, week
// rollover) non deve corrompere.
inline NmeaTimeDecision decideNmeaTime(uint32_t t_nmea, bool fixUsable,
                                       bool tempoInizializzato, uint32_t current) {
  NmeaTimeDecision d = {false, 0, false, false, false, false};
  if (!fixUsable) return d;
  if (!tempoInizializzato) {
    // Bootstrap diretto da GPS (RTC non valido o non ancora sincronizzato).
    d.updateRef = true;  d.newRef = t_nmea;
    d.resetTcnt = true;  d.setInitialized = true;  d.setRtcActive = true;
    return d;
  }
  // Sanity check: l'NMEA arriva ~1s dopo il secondo che rappresenta, quindi
  // uno scarto di 0 o -1 e' fisiologico; oltre e' un vero disallineamento.
  int32_t diff = (int32_t)t_nmea - (int32_t)current;
  if (diff != 0 && diff != -1) {
    d.updateRef = true;  d.newRef = t_nmea;
    d.emitWarn  = true;
  }
  return d;
}

// --- Secondo di cattura fotocellula (ROBUSTO al confine del secondo) ------
// Nella ISR di input-capture ICR1 e' l'istante esatto del fronte (latchato
// dall'hardware), ma la ISR puo' girare DOPO che il secondo e' cambiato.
// Bisogna appaiare cattura_timer al secondo giusto anche negli edge case al
// rollover.
//
//   ref            : unix_riferimento letto nella ISR
//   tcntNow        : TCNT1 letto nella ISR (subito dopo ICR1)
//   catturaTimer   : ICR1 (frazione di secondo del fronte)
//   comparePending : flag OCF1A (match COMPA avvenuto ma ISR COMPA non ancora
//                    servita -> unix_riferimento non ancora incrementato)
//   rtcMode        : true in RTC_ACTIVE (confine = COMPA, con skew match/wrap);
//                    false in PPS_ACTIVE (confine = ISR PPS, reset+incremento
//                    atomici -> nessuno skew, decremento semplice)
//
// --- Secondo corrente "vero" (incremento atomico al ritorno a 0) ----------
// Ricostruisce, dallo stato osservabile, il secondo che unix_riferimento
// AVREBBE se venisse incrementato esattamente quando TCNT1 torna a 0. E' cio'
// che serve per appaiare correttamente riferimento e frazione (usato da
// capture, pong e diag).
//
// In RTC_ACTIVE ci sono due sfasamenti da compensare:
//   - TIMER1_COMPA (che fa unix_riferimento++) scatta al match (TCNT1=32767),
//     un tick PRIMA che il contatore torni a 0;
//   - TIMER1_CAPT ha priorita' MAGGIORE di COMPA, quindi al confine puo' girare
//     prima che la COMPA incrementi (COMPA "pending" = OCF1A alto).
// In PPS_ACTIVE l'incremento e' gia' atomico col reset (ISR del PPS) e OCR1A e'
// alto (TCNT1 puo' superare 32767): nessuna correzione qui, ci pensa la
// saturazione frazione>=32768 di stampaTempo.
inline uint32_t secondoCorrente(uint32_t ref, uint16_t tcntNow,
                                bool comparePending, bool rtcMode) {
  if (!rtcMode) return ref;
  if (comparePending && tcntNow < CRONO_TOP_RTC) {
    // Wrap avvenuto (TCNT1 rientrato) ma COMPA non ancora servita: il
    // riferimento e' 1 indietro rispetto al secondo corrente.
    return ref + 1;
  }
  if (!comparePending && tcntNow == CRONO_TOP_RTC) {
    // COMPA gia' servita (ha incrementato al match) ma il contatore e' ancora
    // sull'ultimo tick del secondo precedente: il riferimento e' 1 avanti.
    return ref - 1;
  }
  return ref;
}

// Secondo Unix del passaggio fotocellula. Parte dal secondo corrente coerente
// (secondoCorrente) e decide se il fronte appartiene a quel secondo o al
// precedente: se il contatore attuale non e' ancora tornato "sotto" il valore
// catturato il fronte e' del secondo corrente, altrimenti del precedente.
inline uint32_t capturedUnix(uint32_t ref, uint16_t tcntNow, uint16_t catturaTimer,
                             bool comparePending, bool rtcMode) {
  uint32_t w = secondoCorrente(ref, tcntNow, comparePending, rtcMode);
  return (tcntNow >= catturaTimer) ? w : (w - 1);
}

// --- Antirimbalzo retriggerabile "a fascio libero" ------------------------
// Il debounce a finestra fissa dall'ultimo rilevamento non basta a una
// partenza spinta da fermo: il veicolo puo' occupare il fascio per >1s e/o le
// gambe del pilota che spinge da dietro generano una seconda interruzione
// oltre la finestra -> due passaggi per una sola partenza.
//
// Semantica corretta: si emette il PRIMO fronte (arrivo del veicolo); poi si
// resta "occupati" finche' il fascio non e' libero per graceMs CONTINUATIVI.
// Ogni attivita' (fascio bloccato ora, o nuovo fronte) riarma la grazia. Cosi'
// veicolo + gambe confluiscono in un solo passaggio, col tempo del primo fronte.
struct AntirimbalzoStato {
  bool     occupato;            // dentro un passaggio + finestra di grazia
  uint32_t ultima_attivita_ms;  // ultimo istante di fascio bloccato o fronte
};
struct AntirimbalzoEsito {
  bool               emetti;    // true = nuovo passaggio da emettere ('P')
  AntirimbalzoStato  stato;     // stato aggiornato (da riassegnare al chiamante)
};

// nowMs          : millis() corrente
// fascioBloccato : true se il fascio e' interrotto ADESSO (pin ICP1 a livello)
// fronte         : true se in questa iterazione e' stato catturato un fronte
// graceMs        : fascio libero necessario a chiudere il passaggio
inline AntirimbalzoEsito aggiornaAntirimbalzo(AntirimbalzoStato s, uint32_t nowMs,
                                              bool fascioBloccato, bool fronte,
                                              uint32_t graceMs) {
  AntirimbalzoEsito e;
  e.emetti = false;
  // Attivita' (fascio bloccato ora, o fronte catturato) riarma la grazia.
  if (fascioBloccato || fronte) {
    s.ultima_attivita_ms = nowMs;
  }
  // Passaggio chiuso quando il fascio e' libero da >= graceMs (nessuna attivita').
  if (s.occupato && (uint32_t)(nowMs - s.ultima_attivita_ms) >= graceMs) {
    s.occupato = false;
  }
  // Un fronte mentre siamo liberi = nuovo passaggio: emette ed entra in occupato.
  // Un fronte mentre siamo occupati (veicolo lungo / gambe del pilota) e' soppresso.
  if (fronte && !s.occupato) {
    e.emetti    = true;
    s.occupato  = true;
  }
  e.stato = s;
  return e;
}

// --- Bootstrap RTC: anno plausibile ---------------------------------------
// A boot l'RTC viene usato come sorgente solo se porta una data sensata.
inline bool rtcBootstrapYearOk(int year) {
  return year >= 2024;
}

// --- Guardia rollover di minuto per la scrittura RTC ----------------------
// Scrivere l'RTC a cavallo del cambio minuto puo' lasciarlo su un minuto
// ambiguo: si rimanda al prossimo secondo utile.
inline bool syncAttraversaMinuto(uint32_t prossimoSec) {
  return (prossimoSec % 60) == 0;
}

// --- Packing BCD dei 7 byte del DS3231 (registri 0x00..0x06) --------------
// dow: convenzione RTClib.dayOfTheWeek() (domenica=0); il DS3231 usa 1..7,
// quindi domenica->7. year2000 = anno - 2000. L'ora e' in formato 24h (bit6=0).
inline void packRtcBcd(uint8_t sec, uint8_t minute, uint8_t hour, uint8_t dow,
                       uint8_t day, uint8_t month, uint8_t year2000, uint8_t out[7]) {
  out[0] = ((sec      / 10) << 4) | (sec      % 10);
  out[1] = ((minute   / 10) << 4) | (minute   % 10);
  out[2] = ((hour     / 10) << 4) | (hour     % 10);
  out[3] = (dow == 0) ? 7 : dow;
  out[4] = ((day      / 10) << 4) | (day      % 10);
  out[5] = ((month    / 10) << 4) | (month    % 10);
  out[6] = ((year2000 / 10) << 4) | (year2000 % 10);
}

// --- Azzeramento OSF (Oscillator Stop Flag, status reg 0x0F bit7) ----------
inline uint8_t osfCleared(uint8_t status) {
  return status & 0x7F;
}

#endif // CRONOMETRO_LOGIC_H
