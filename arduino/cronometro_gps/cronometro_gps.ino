// =====================================================================
//  Cronometro di precisione GPS-sincronizzato per cronometraggio auto
// =====================================================================
//
//  Architettura
//  ------------
//  Base di tempo: Timer1 dell'ATmega328P clockato dal pin 32K del DS3231
//  (32.768 kHz). Il Timer1 lavora in CTC mode con TOP variabile:
//    - OCR1A = 32767  in RTC_ACTIVE  -> rollover ogni secondo
//    - OCR1A = 49151  in PPS_ACTIVE  -> il CTC e' solo rete di sicurezza
//                                       (scatta solo se il PPS e' perso)
//
//  Sorgente del "tick di secondo", in ordine di preferenza:
//    1. PPS del GPS (NEO-M8N), allineato all'inizio del secondo UTC (~ns)
//    2. Rollover CTC del Timer1, ancorato al cristallo del DS3231 (~2 ppm)
//
//  L'NMEA (solo RMC + GGA ridotto) fornisce: bootstrap del tempo assoluto,
//  sanity check periodico e dati diagnostici (sat, hdop, alt).
//
//  La cattura del passaggio fotocellula avviene via Input Capture (ICP1,
//  pin 8): l'hardware latcha ICR1 all'istante esatto dell'evento,
//  indipendentemente dalla latenza software.
//
//  Macchina a stati
//  ----------------
//    INIT -> WAIT_RTC_SYNC -> RTC_ACTIVE <-> PPS_ACTIVE
//                          \-> UNSYNCED  -> (PPS/NMEA) -> ...
//
//  Collegamenti (Arduino Nano - ATmega328P)
//  ----------------------------------------
//    NEO-M8N TX  -> D4  (SoftwareSerial RX)
//    NEO-M8N RX  -> D3  (SoftwareSerial TX)
//    NEO-M8N PPS -> D2  (INT0)
//    DS3231 SDA  -> A4
//    DS3231 SCL  -> A5
//    DS3231 32K  -> D5  (Timer1 clock T1) - richiede pull-up (vedi README)
//    Fotocellula -> D8  (ICP1) - uscita NPN NO, fronte di discesa
//    GND comune a tutti i dispositivi.
//
//  Protocollo seriale (115200 baud, USB-CDC, righe terminate da '\n')
//  ------------------------------------------------------------------
//  Arduino -> PC (asincrono / risposte):
//    P<unix>.<ms><G|R>                 passaggio fotocellula (G=GPS, R=RTC)
//    K<src>[<unix>.<ms>]               pong (src G|R|N); timestamp se src!=N
//    D<unix,src,dev_tcnt,nmea_ms,fix,sat,hdop,alt,persi,since_pps,since_rtc,uptime>
//    Y<ver>                            identita' firmware
//    W<nmea>,<ref>                     evento desync (bassa frequenza)
//  PC -> Arduino (1 byte + '\n'):
//    '?' -> pong   '@' -> diag   '#' -> id
//
//  NOTA: i checksum dei comandi PUBX qui sotto sono stati verificati
//  (XOR dei caratteri tra '$' e '*'). Vedi README per il dettaglio.
// =====================================================================

#include <Wire.h>
#include <RTClib.h>
#include <TinyGPS++.h>
#include <SoftwareSerial.h>
#include "cronometro_logic.h"

RTC_DS3231 rtc;
TinyGPSPlus gps;
SoftwareSerial ss(4, 3);

// --- Macchina a stati ---
enum Stato : uint8_t {
  INIT,
  WAIT_RTC_SYNC,
  RTC_ACTIVE,
  PPS_ACTIVE,
  UNSYNCED
};
volatile Stato stato_attuale = INIT;

// --- Riferimento temporale ---
volatile uint32_t unix_riferimento = 0;
volatile bool tempo_inizializzato = false;
volatile bool compa_recente = false;

// --- Dati cattura fotocellula ---
volatile uint32_t unix_cattura     = 0;
volatile uint16_t cattura_timer    = 0;
volatile bool evento_fotocellula   = false;
volatile uint8_t cattura_fonte = 'R';   // 'G' (PPS) o 'R' (RTC) al momento della cattura

// --- Dati diagnostici ---
volatile uint16_t diag_tcnt_al_pps     = 0;
volatile bool pps_appena_arrivato      = false;
volatile unsigned long ultimo_pps_ms   = 0;
volatile bool pps_senza_nmea           = false;
volatile uint16_t contatore_nmea_persi = 0;
uint16_t ultimo_ritardo_ticks = 0;

// --- Gestione aggiornamento RTC ---
unsigned long ultimo_aggiornamento_rtc_ms = 0;
const unsigned long INTERVALLO_AGGIORNAMENTO_RTC_MS = 5UL * 60UL * 1000UL;

// --- Debounce e timeout ---
char cmdBuf[8];
uint8_t cmdLen = 0;

const unsigned long DEBOUNCE_TIME_MS     = 1000;
const unsigned long TIMEOUT_BOOTSTRAP_MS = 1500;
const unsigned long TIMEOUT_BUSY_WAIT_MS = 2000;
unsigned long ultimo_passaggio_valido = 0;

// ============================================================
// SETUP
// ============================================================
void setup() {
  Serial.begin(115200);
  ss.begin(9600);
  delay(1000);
  while (ss.available()) ss.read();

  // Configura il NEO-M8N: solo RMC (1Hz) + GGA (0.2Hz).
  // Checksum verificati con XOR dei caratteri tra '$' e '*'.
  ss.println(F("$PUBX,40,GLL,0,0,0,0,0,0*5C")); delay(50);
  ss.println(F("$PUBX,40,GSA,0,0,0,0,0,0*4E")); delay(50);
  ss.println(F("$PUBX,40,GSV,0,0,0,0,0,0*59")); delay(50);
  ss.println(F("$PUBX,40,VTG,0,0,0,0,0,0*5E")); delay(50);
  ss.println(F("$PUBX,40,RMC,0,1,0,0,0,0*46")); delay(50);
  ss.println(F("$PUBX,40,GGA,0,5,0,0,0,0*5F")); delay(50);

  // I2C a 400 kHz per accesso rapido al DS3231
  Wire.begin();
  Wire.setClock(400000);
  rtc.begin();
  // SQW non utilizzato (Timer1 in CTC fa lo stesso lavoro internamente).
  // Il pin 32K del DS3231 fornisce il clock a 32.768 kHz al pin 5 dell'Arduino.

  pinMode(5, INPUT);        // 32K dal DS3231 -> T1
  pinMode(8, INPUT_PULLUP); // Fotocellula -> ICP1
  pinMode(2, INPUT);        // PPS dal GPS -> INT0

  // Configurazione Timer1 in CTC mode
  noInterrupts();
  TCCR1A = 0;
  TCCR1B = 0;
  TCNT1  = 0;
  OCR1A  = 32767;                                     // 1s di rollover in RTC_ACTIVE
  TCCR1B |= (1 << WGM12);                             // CTC mode (Mode 4)
  TCCR1B |= (1 << CS12) | (1 << CS11) | (1 << CS10);  // Clock esterno T1, fronte salita
  TCCR1B &= ~(1 << ICES1);                            // Capture su fronte discesa
  TCCR1B |= (1 << ICNC1);                             // Noise canceller
  TIMSK1 |= (1 << ICIE1) | (1 << OCIE1A);             // Capture + Compare A
  interrupts();

  // Cambia stato PRIMA di abilitare PPS, per gestione corretta dell'edge case
  stato_attuale = WAIT_RTC_SYNC;

  // Abilita interrupt PPS solo ora, dopo che il Timer1 e' pronto
  attachInterrupt(digitalPinToInterrupt(2), ppsInterrupt, RISING);

  // Bootstrap RTC (puo' essere abortito se nel frattempo arriva un PPS)
  bootstrapRtc();
}

// ============================================================
// BOOTSTRAP RTC
// ============================================================
void bootstrapRtc() {
  // Se PPS e' arrivato durante il setup, abortiamo: aspetteremo il NMEA per
  // il bootstrap del tempo assoluto, manteniamo solo il timing dal PPS
  if (stato_attuale == PPS_ACTIVE) return;

  DateTime ora_iniziale = rtc.now();
  if (!rtcBootstrapYearOk(ora_iniziale.year())) {
    noInterrupts();
    if (stato_attuale != PPS_ACTIVE) {
      stato_attuale = UNSYNCED;
    }
    interrupts();
    return;
  }

  uint8_t sec_iniziale = leggiSecondiRtcVeloce();

  unsigned long inizio = millis();
  while (millis() - inizio < TIMEOUT_BOOTSTRAP_MS) {
    if (stato_attuale == PPS_ACTIVE) return;  // PPS arrivato, abortisci

    uint8_t sec_attuale = leggiSecondiRtcVeloce();
    if (sec_attuale != sec_iniziale) {
      // Reset TCNT1 ASAP, prima della lettura completa
      noInterrupts();
      TCNT1 = 0;
      interrupts();

      // Leggi RTC completo (TCNT1 conta dal rollover esatto)
      DateTime ora = rtc.now();
      uint32_t t_unix = ora.unixtime();

      // Aggiorna stato atomicamente, ma solo se nel frattempo PPS non e' arrivato
      noInterrupts();
      if (stato_attuale != PPS_ACTIVE) {
        unix_riferimento = t_unix;
        tempo_inizializzato = true;
        stato_attuale = RTC_ACTIVE;
        compa_recente = false;
      }
      interrupts();
      return;
    }
  }

  // Timeout: RTC non risponde o si e' inceppato
  noInterrupts();
  if (stato_attuale != PPS_ACTIVE) {
    stato_attuale = UNSYNCED;
  }
  interrupts();
}

uint8_t leggiSecondiRtcVeloce() {
  Wire.beginTransmission(0x68);
  Wire.write(0x00);
  Wire.endTransmission(false);
  Wire.requestFrom((uint8_t)0x68, (uint8_t)1);
  uint8_t bcd = Wire.read();
  return ((bcd >> 4) & 0x07) * 10 + (bcd & 0x0F);
}

// ============================================================
// ISR
// ============================================================

// ISR del PPS: il GPS dichiara l'inizio di un nuovo secondo UTC.
// NOTA: in caso di reset del GPS o glitch, un PPS puo' arrivare non allineato a
// un secondo UTC vero. In tal caso il sanity check NMEA correggera' l'orario.
void ppsInterrupt() {
  uint16_t tcnt_pre = TCNT1;
  TCNT1 = 0;
  OCR1A = 49151;  // Tolleranza 1.5s in PPS_ACTIVE

  if (!compa_recente) {
    if (tempo_inizializzato) {
      unix_riferimento++;
    }
  }
  compa_recente = false;

  if (stato_attuale != PPS_ACTIVE) {
    stato_attuale = PPS_ACTIVE;
  }

  diag_tcnt_al_pps = tcnt_pre;
  pps_appena_arrivato = true;
  ultimo_pps_ms = millis();

  if (pps_senza_nmea) {
    contatore_nmea_persi++;
  }
  pps_senza_nmea = true;
}

// ISR Compare Match A: rollover del Timer1 (TCNT1 raggiunge OCR1A).
// In RTC_ACTIVE: rollover normale di secondo (OCR1A=32767).
// In PPS_ACTIVE: scatta solo se PPS perso (OCR1A=49151).
ISR(TIMER1_COMPA_vect) {
  if (stato_attuale == PPS_ACTIVE) {
    // PPS perso: siamo a 1.5s dall'ultimo PPS. Recupera 0.5s.
    TCNT1 = 16384;
    if (tempo_inizializzato) {
      unix_riferimento++;
    }
    OCR1A = 32767;
    stato_attuale = RTC_ACTIVE;
    compa_recente = true;
  } else if (stato_attuale == RTC_ACTIVE) {
    if (tempo_inizializzato) {
      unix_riferimento++;
    }
    compa_recente = true;
  }
  // In WAIT_RTC_SYNC, UNSYNCED, INIT: TCNT1 si autoresetta ma non facciamo altro
}

// ISR Input Capture: scatta al passaggio della fotocellula
ISR(TIMER1_CAPT_vect) {
  uint16_t icr_val = ICR1;

  if (!tempo_inizializzato) {
    return;  // scarta capture pre-sync
  }

  cattura_timer = icr_val;
  // Letti subito dopo ICR1 e coerenti fra loro: la COMPA non puo' interlacciarsi
  // (interrupt disabilitati dentro questa ISR), quindi ref/OCF1A sono congelati.
  uint16_t tcnt_now = TCNT1;
  bool compare_pending = (TIFR1 & (1 << OCF1A)) != 0;
  bool rtc_mode = (stato_attuale == RTC_ACTIVE);
  unix_cattura = capturedUnix(unix_riferimento, tcnt_now, cattura_timer,
                              compare_pending, rtc_mode);
  cattura_fonte = (stato_attuale == PPS_ACTIVE) ? 'G' : 'R';
  evento_fotocellula = true;
}

// ============================================================
// HELPER TEMPO ASSOLUTO
// ============================================================
// Stampa "<unix>.<ms a 3 cifre>" su Serial (no newline). Applica la
// saturazione frazione>=32768 come la stampa del passaggio.
void stampaTempo(uint32_t secondo, uint16_t frazione) {
  while (frazione >= 32768) { frazione -= 32768; secondo += 1; }
  uint16_t ms = (uint16_t)(frazione / 32.768f);
  if (ms > 999) ms = 999;
  Serial.print(secondo);
  Serial.print('.');
  if (ms < 100) Serial.print('0');
  if (ms < 10)  Serial.print('0');
  Serial.print(ms);
}

// ============================================================
// HELPER FONTE CORRENTE
// ============================================================
char fonteCorrente() {
  if (!tempo_inizializzato) return 'N';
  return (stato_attuale == PPS_ACTIVE) ? 'G' : 'R';
}

// ============================================================
// RISPOSTE AI COMANDI
// ============================================================
void inviaPong() {
  char s = fonteCorrente();
  Serial.print('K');
  Serial.print(s);
  if (s != 'N') {
    noInterrupts();
    uint32_t ref = unix_riferimento;
    uint16_t fr  = TCNT1;
    bool ocf     = (TIFR1 & (1 << OCF1A)) != 0;
    interrupts();
    uint32_t sec = secondoCorrente(ref, fr, ocf, stato_attuale == RTC_ACTIVE);
    stampaTempo(sec, fr);
  }
  Serial.println();
}

void inviaId() {
  Serial.println(F("Y2"));
}

void inviaDiag() {
  noInterrupts();
  uint32_t ref_now   = unix_riferimento;
  uint16_t tcnt_now  = TCNT1;
  bool ocf_now       = (TIFR1 & (1 << OCF1A)) != 0;
  uint16_t tcnt_diag = diag_tcnt_al_pps;
  uint16_t persi     = contatore_nmea_persi;
  unsigned long tpps = ultimo_pps_ms;
  unsigned long trtc = ultimo_aggiornamento_rtc_ms;
  interrupts();
  uint32_t secondo = secondoCorrente(ref_now, tcnt_now, ocf_now,
                                     stato_attuale == RTC_ACTIVE);
  unsigned long ora = millis();
  Serial.print('D');
  Serial.print(secondo);                 Serial.print(',');
  Serial.print(fonteCorrente());         Serial.print(',');
  // Deviazione firmata da 32768 (tick ideali in 1s). Cast a long PRIMA
  // della sottrazione: un cast a int16_t manderebbe 32768 (il caso
  // perfetto) a -65536 per wrap-around.
  Serial.print((long) tcnt_diag - 32768L); Serial.print(',');
  Serial.print(ultimo_ritardo_ticks / 32.768f, 1); Serial.print(',');
  Serial.print(gps.location.isValid() ? 'A' : 'V'); Serial.print(',');
  Serial.print(gps.satellites.isValid() ? gps.satellites.value() : 0); Serial.print(',');
  if (gps.hdop.isValid()) Serial.print(gps.hdop.hdop(), 2); else Serial.print(F("0")); Serial.print(',');
  if (gps.altitude.isValid()) Serial.print(gps.altitude.meters(), 0); else Serial.print(F("0")); Serial.print(',');
  Serial.print(persi);                   Serial.print(',');
  Serial.print((ora - tpps) / 1000);     Serial.print(',');
  Serial.print(trtc == 0 ? 0 : (ora - trtc) / 1000); Serial.print(',');
  Serial.print(ora / 1000);              Serial.print(',');
  // --- Estensione diagnostica: GPS e RTC assoluti (unix UTC). Letture
  // "pesanti" (I2C sul DS3231, ricostruzione della data GPS) eseguite SOLO
  // qui, cioe' solo quando il PC chiede la diagnostica con '@' (schermata di
  // diagnostica aperta) - mai nel percorso di gara. Un firmware senza questi
  // campi (versione precedente) fa emettere al PC "non disponibili".
  // gps_ok vero solo con un fix di posizione RECENTE (coerente col campo
  // 'fix' A/V): gps.date/time.isValid() da soli restano latched anche senza
  // fix e riporterebbero un orario stantio/fittizio.
  bool gpsOk = nmeaFixUsable(gps.location.isValid(), gps.location.age())
               && gps.date.isValid() && gps.time.isValid();
  uint32_t gpsUnix = 0;
  if (gpsOk) {
    DateTime g(gps.date.year(), gps.date.month(), gps.date.day(),
               gps.time.hour(), gps.time.minute(), gps.time.second());
    gpsUnix = g.unixtime();
  }
  DateTime r = rtc.now();
  // rtc_ok = OSF pulito: l'RTC e' stato SINCRONIZZATO almeno una volta dopo
  // l'ultimo arresto dell'oscillatore (il fix GPS azzera l'OSF). rtc_unix e'
  // SEMPRE l'orario reale del DS3231 (l'oscillatore gira anche con OSF attivo /
  // orario mai impostato): cosi' il PC lo mostra e rende evidente un RTC da
  // sincronizzare, senza confonderlo con una batteria assente.
  bool rtcOk = !rtc.lostPower();
  Serial.print(gpsOk ? 1 : 0);           Serial.print(',');
  Serial.print(gpsUnix);                 Serial.print(',');
  Serial.print(rtcOk ? 1 : 0);           Serial.print(',');
  Serial.println(r.unixtime());
}

// ============================================================
// LOOP
// ============================================================
void loop() {
  // --- Comandi da PC via USB Serial ---
  while (Serial.available() > 0) {
    char c = Serial.read();
    if (c == '\n' || c == '\r') {
      if (cmdLen > 0) {
        char cmd = cmdBuf[0];
        if      (cmd == '?') inviaPong();
        else if (cmd == '@') inviaDiag();
        else if (cmd == '#') inviaId();
        cmdLen = 0;
      }
    } else if (cmdLen < sizeof(cmdBuf) - 1) {
      cmdBuf[cmdLen++] = c;
    }
  }

  // --- Lettura NMEA dal GPS ---
  while (ss.available() > 0) {
    if (gps.encode(ss.read())) {
      if (gps.time.isUpdated() && gps.time.isValid() && gps.date.isValid()) {
        ultimo_ritardo_ticks = TCNT1;

        noInterrupts();
        pps_senza_nmea = false;
        interrupts();

        DateTime oraGps(gps.date.year(), gps.date.month(), gps.date.day(),
                        gps.time.hour(), gps.time.minute(), gps.time.second());
        uint32_t t_nmea = oraGps.unixtime();

        // Il tempo NMEA si usa SOLO con un fix di posizione recente: senza fix
        // il GPS emette comunque data/ora (spesso sfasate di ~20 anni per week
        // rollover) che NON devono corrompere il riferimento -- in fallback il
        // tempo resta quello dell'RTC. Bootstrap ~200ms di errore fino al primo
        // PPS, poi riallineamento automatico.
        bool fixUsable = nmeaFixUsable(gps.location.isValid(), gps.location.age());

        noInterrupts();
        uint32_t attuale = unix_riferimento;
        bool inizializzato = tempo_inizializzato;
        interrupts();

        NmeaTimeDecision dec = decideNmeaTime(t_nmea, fixUsable, inizializzato, attuale);

        if (dec.emitWarn) {
          Serial.print('W');
          Serial.print(t_nmea);
          Serial.print(',');
          Serial.println(attuale);
        }
        if (dec.updateRef || dec.setInitialized || dec.resetTcnt) {
          noInterrupts();
          if (dec.resetTcnt) TCNT1 = 0;
          if (dec.updateRef) unix_riferimento = dec.newRef;
          if (dec.setInitialized) tempo_inizializzato = true;
          if (dec.setRtcActive
              && (stato_attuale == UNSYNCED || stato_attuale == WAIT_RTC_SYNC)) {
            stato_attuale = RTC_ACTIVE;
          }
          interrupts();
        }
      }
    }
  }

  // --- Gestione passaggio fotocellula ---
  if (evento_fotocellula) {
    unsigned long m = millis();
    if (m - ultimo_passaggio_valido > DEBOUNCE_TIME_MS) {

      noInterrupts();
      uint32_t secondo  = unix_cattura;
      uint16_t frazione = cattura_timer;
      interrupts();

      noInterrupts();
      uint8_t fonte = cattura_fonte;
      interrupts();
      Serial.print('P');
      stampaTempo(secondo, frazione);
      Serial.println((char) fonte);

      ultimo_passaggio_valido = m;

      // Aggiorna RTC se opportuno (durante il debounce, dopo aver stampato)
      tentaAggiornamentoRtc();
    }
    evento_fotocellula = false;
  }

}

// ============================================================
// AGGIORNAMENTO RTC AL MILLISECONDO
// ============================================================
void tentaAggiornamentoRtc() {
  if (stato_attuale != PPS_ACTIVE) return;

  // Verifica intervallo minimo tra aggiornamenti
  if (ultimo_aggiornamento_rtc_ms != 0
      && millis() - ultimo_aggiornamento_rtc_ms < INTERVALLO_AGGIORNAMENTO_RTC_MS) {
    return;
  }

  noInterrupts();
  uint32_t prossimo_sec = unix_riferimento + 1;
  interrupts();

  // Evita rollover di minuto (e quindi anche di ora, giorno, mese, anno):
  // scrivere a cavallo del cambio minuto puo' lasciare l'RTC su un minuto
  // ambiguo. Riproveremo al prossimo passaggio utile.
  if (syncAttraversaMinuto(prossimo_sec)) return;

  // Prepara i 7 byte BCD del nuovo timestamp. Day-of-week: convenzione RTClib
  // (dowToDS3231) -> domenica=7, altrimenti 1-6. Il registro DOW non incide sul
  // calendario (RTClib lo ricalcola dalla data in lettura), ma manteniamo la
  // stessa convenzione per coerenza.
  DateTime dt(prossimo_sec);
  uint8_t buf[7];
  packRtcBcd(dt.second(), dt.minute(), dt.hour(), dt.dayOfTheWeek(),
             dt.day(), dt.month(), (uint8_t)(dt.year() - 2000), buf);

  // Attesa fase 1: TCNT1 raggiunge zona "quasi rollover"
  unsigned long inizio_wait = millis();
  while (TCNT1 < 32500) {
    if (stato_attuale != PPS_ACTIVE) return;
    if (millis() - inizio_wait > TIMEOUT_BUSY_WAIT_MS) return;
  }

  // Attesa fase 2: rilevamento del rollover (TCNT1 transita da alto a basso)
  uint16_t prev = TCNT1;
  while (true) {
    uint16_t curr = TCNT1;
    if (curr < prev) break;  // rollover avvenuto
    prev = curr;
    if (stato_attuale != PPS_ACTIVE) return;
    if (millis() - inizio_wait > TIMEOUT_BUSY_WAIT_MS) return;
  }

  // Doppio check stato: se nel rollover il CTC ha trippato (PPS perso),
  // unix_riferimento e' stato incrementato dal CTC e i nostri BCD preparati
  // sono obsoleti. Aborto.
  if (stato_attuale != PPS_ACTIVE) return;

  // Rollover appena avvenuto. Scrittura I2C completa dei 7 byte.
  // Il countdown interno del DS3231 si resetta al byte secondi (primo byte).
  Wire.beginTransmission(0x68);
  Wire.write(0x00);
  for (uint8_t i = 0; i < 7; i++) {
    Wire.write(buf[i]);
  }
  Wire.endTransmission();

  // Azzera l'OSF (Oscillator Stop Flag, status reg 0x0F bit7). Da qui in poi
  // lostPower() torna 1 solo se l'oscillatore si ferma DAVVERO (es. batteria
  // tampone morta con alimentazione staccata): cosi' l'OSF diventa un
  // indicatore utile di "RTC sincronizzato e mai fermato da allora". Senza,
  // resterebbe attivo per sempre e non distinguerebbe un RTC mai sincronizzato
  // da uno con la batteria morta.
  Wire.beginTransmission(0x68);
  Wire.write(0x0F);
  Wire.endTransmission();
  Wire.requestFrom(0x68, 1);
  uint8_t st = Wire.available() ? Wire.read() : 0;
  Wire.beginTransmission(0x68);
  Wire.write(0x0F);
  Wire.write(osfCleared(st));
  Wire.endTransmission();

  ultimo_aggiornamento_rtc_ms = millis();
}
