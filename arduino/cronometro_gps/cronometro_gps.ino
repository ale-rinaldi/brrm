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
//  Output seriale (115200 baud)
//  ----------------------------
//    PASSAGGIO:<unix>.<ms a 3 cifre>
//    DIAG:<unix>,<fonte>,<dev_tcnt>,<ritardo_nmea_ms>,<fix>,<sat>,<hdop>,<alt>,<nmea_persi>
//    DIAG_NOPPS:<uptime>s,<fonte>,-,-,<fix>,<sat>,<hdop>,<alt>,<nmea_persi>
//
//  NOTA: i checksum dei comandi PUBX qui sotto sono stati verificati
//  (XOR dei caratteri tra '$' e '*'). Vedi README per il dettaglio.
// =====================================================================

#include <Wire.h>
#include <RTClib.h>
#include <TinyGPS++.h>
#include <SoftwareSerial.h>

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
const unsigned long DEBOUNCE_TIME_MS     = 1000;
const unsigned long INTERVALLO_HB_MS     = 1000;
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
  if (ora_iniziale.year() < 2024) {
    Serial.println(F("INFO: RTC non programmato"));
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
        Serial.print(F("INFO: RTC sincronizzato, unix="));
        Serial.println(t_unix);
      }
      interrupts();
      return;
    }
  }

  // Timeout: RTC non risponde o si e' inceppato
  Serial.println(F("WARN: timeout bootstrap RTC"));
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
  uint32_t snap = unix_riferimento;
  if (TCNT1 < cattura_timer) {
    snap--;
  }
  unix_cattura = snap;
  evento_fotocellula = true;
}

// ============================================================
// LOOP
// ============================================================
void loop() {
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

        if (!tempo_inizializzato) {
          // Bootstrap diretto da GPS (RTC non valido o ancora non sincronizzato).
          // Allineamo TCNT1=0 al momento dell'NMEA: errore sistematico ~200ms
          // fino al primo PPS, poi riallineamento automatico.
          noInterrupts();
          TCNT1 = 0;
          unix_riferimento = t_nmea;
          tempo_inizializzato = true;
          if (stato_attuale == UNSYNCED || stato_attuale == WAIT_RTC_SYNC) {
            stato_attuale = RTC_ACTIVE;
          }
          interrupts();
          Serial.print(F("INFO: bootstrap da GPS, unix="));
          Serial.println(t_nmea);
        } else {
          // Sanity check
          noInterrupts();
          uint32_t attuale = unix_riferimento;
          interrupts();

          int32_t diff = (int32_t)t_nmea - (int32_t)attuale;
          if (diff != 0 && diff != -1) {
            Serial.print(F("WARN: desync NMEA="));
            Serial.print(t_nmea);
            Serial.print(F(" ref="));
            Serial.println(attuale);
            noInterrupts();
            unix_riferimento = t_nmea;
            interrupts();
          }
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

      // Saturazione: in PPS_ACTIVE TCNT1 puo' arrivare fino a 49151 (1.5s).
      // Il while gestisce sia drift sia capture avvenute oltre la fine del secondo.
      while (frazione >= 32768) {
        frazione -= 32768;
        secondo  += 1;
      }

      uint16_t ms = (uint16_t)(frazione / 32.768f);
      if (ms > 999) ms = 999;

      Serial.print(F("PASSAGGIO:"));
      Serial.print(secondo);
      Serial.print('.');
      if (ms < 100) Serial.print('0');
      if (ms < 10)  Serial.print('0');
      Serial.println(ms);

      ultimo_passaggio_valido = m;

      // Aggiorna RTC se opportuno (durante il debounce, dopo aver stampato)
      tentaAggiornamentoRtc();
    }
    evento_fotocellula = false;
  }

  // --- Emissione diagnostica dopo PPS ---
  if (pps_appena_arrivato) {
    pps_appena_arrivato = false;
    emettiDiagnostica(true);
  }

  // --- Heartbeat diagnostico se PPS perso o non ancora arrivato ---
  static unsigned long ultimo_heartbeat = 0;
  noInterrupts();
  unsigned long t_pps = ultimo_pps_ms;
  interrupts();

  unsigned long ora = millis();
  bool pps_assente = (t_pps == 0) || (ora - t_pps > 2000);

  if (pps_assente && (ora - ultimo_heartbeat >= INTERVALLO_HB_MS)) {
    emettiDiagnostica(false);
    ultimo_heartbeat = ora;
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
  if ((prossimo_sec % 60) == 0) return;

  // Prepara i 7 byte BCD del nuovo timestamp
  DateTime dt(prossimo_sec);
  uint8_t buf[7];
  buf[0] = ((dt.second()  / 10) << 4) | (dt.second()  % 10);
  buf[1] = ((dt.minute()  / 10) << 4) | (dt.minute()  % 10);
  buf[2] = ((dt.hour()    / 10) << 4) | (dt.hour()    % 10);  // 24h mode (bit6=0)
  // Day-of-week: convenzione RTClib (dowToDS3231) -> domenica=7, altrimenti 1-6.
  // Il registro DOW non incide sul calendario (RTClib ricalcola il DOW dalla
  // data in lettura), ma manteniamo la stessa convenzione per coerenza.
  uint8_t dow = dt.dayOfTheWeek();
  buf[3] = (dow == 0) ? 7 : dow;
  buf[4] = ((dt.day()     / 10) << 4) | (dt.day()     % 10);
  buf[5] = ((dt.month()   / 10) << 4) | (dt.month()   % 10);
  uint16_t y = dt.year() - 2000;
  buf[6] = ((y / 10) << 4) | (y % 10);

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

  ultimo_aggiornamento_rtc_ms = millis();
  Serial.println(F("INFO: RTC aggiornato"));
}

// ============================================================
// DIAGNOSTICA
// ============================================================
void emettiDiagnostica(bool pps_attivo) {
  noInterrupts();
  uint32_t secondo   = unix_riferimento;
  uint16_t tcnt_diag = diag_tcnt_al_pps;
  uint16_t persi     = contatore_nmea_persi;
  Stato s            = stato_attuale;
  interrupts();

  const __FlashStringHelper* fonte;
  switch (s) {
    case PPS_ACTIVE:    fonte = F("PPS"); break;
    case RTC_ACTIVE:    fonte = F("RTC"); break;
    case WAIT_RTC_SYNC: fonte = F("WAIT"); break;
    case UNSYNCED:      fonte = F("NONE"); break;
    default:            fonte = F("INIT"); break;
  }

  Serial.print(pps_attivo ? F("DIAG:") : F("DIAG_NOPPS:"));

  if (tempo_inizializzato) {
    Serial.print(secondo);
  } else {
    Serial.print(millis() / 1000);
    Serial.print('s');
  }
  Serial.print(',');
  Serial.print(fonte);
  Serial.print(',');

  if (pps_attivo) {
    Serial.print((int16_t)tcnt_diag - 32768);
  } else {
    Serial.print('-');
  }
  Serial.print(',');

  if (pps_attivo) {
    Serial.print(ultimo_ritardo_ticks / 32.768f, 1);
  } else {
    Serial.print('-');
  }
  Serial.print(',');

  Serial.print(gps.location.isValid() ? 'A' : 'V');
  Serial.print(',');
  Serial.print(gps.satellites.isValid() ? gps.satellites.value() : 0);
  Serial.print(',');

  if (gps.hdop.isValid()) {
    Serial.print(gps.hdop.hdop(), 2);
  } else {
    Serial.print(F("--"));
  }
  Serial.print(',');

  if (gps.altitude.isValid()) {
    Serial.print(gps.altitude.meters(), 0);
  } else {
    Serial.print(F("--"));
  }
  Serial.print(',');

  Serial.println(persi);
}
