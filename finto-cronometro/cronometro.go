package main

import (
	"fmt"
	"strings"
	"sync"
	"time"
)

// Il cronometro simulato: parla il protocollo del firmware
// (arduino/cronometro_gps) su una pseudo-seriale, e si comanda da una paginetta.
//
// Serve a provare brrm senza hardware: una gara intera, i passaggi, la
// diagnostica, il degrado del tempo. NON è un modello del firmware — non simula
// il PPS, i tick del timer, l'NMEA — è un modello del suo COMPORTAMENTO SUL FILO,
// che è l'unica cosa che brrm vede.
//
// L'orologio è sempre quello di sistema. La «sorgente» dichiarata cambia solo la
// qualità che il passaggio porta con sé, che è ciò che brrm usa per decidere
// quanto fidarsi di quel tempo.

// Sorgente è la base tempo dichiarata, con le stesse lettere del firmware.
type Sorgente byte

const (
	// PPS: GPS agganciato. Il firmware dichiara ~30 µs; qui il tempo è quello di
	// sistema, quindi la lettera è una DICHIARAZIONE, non una misura.
	PPS Sorgente = 'G'
	// RTC: holdover sull'orologio interno, precisione al millisecondo.
	RTC Sorgente = 'R'
	// Nessuna: il device non sa che ora è, e i passaggi non portano un tempo
	// utilizzabile. È lo stato all'accensione prima del primo aggancio.
	Nessuna Sorgente = 'N'
)

// versione è quella che il simulatore dichiara al comando '#'.
//
// Il suffisso «-sim» è deliberato: se un giorno un log di gara vero contiene
// questa versione, si deve poter vedere a colpo d'occhio che quei tempi non
// vengono da un cronometro.
const versione = "2-sim"

// antirimbalzo replica DEBOUNCE_TIME_MS del firmware: un passaggio entro questa
// finestra dal precedente viene SCARTATO, e la finestra si sposta ogni volta —
// «a fascio libero», come dice il commit del firmware. È ciò che nella realtà
// evita i doppi rilevamenti di un'auto che copre la fotocellula a intermittenza,
// ed è simulato perché è proprio il comportamento che si vuole poter provare.
const antirimbalzo = time.Second

// Cronometro tiene lo stato del device simulato.
type Cronometro struct {
	mu       sync.Mutex
	sorgente Sorgente
	acceso   time.Time
	// ultimo è l'istante dell'ultimo fascio interrotto, ACCETTATO O NO: è quel
	// che rende l'antirimbalzo retriggerabile.
	ultimo   time.Time
	passaggi int
	scartati int
	// scostamento è lo sfasamento da dichiarare nel prossimo W, in secondi. Zero
	// = nessun avviso da mandare.
	scostamento int
	// Adesso è iniettabile per i test.
	Adesso func() time.Time
	// scrivi manda una riga sul filo. La imposta il chiamante.
	scrivi func(string)
}

func NuovoCronometro(scrivi func(string)) *Cronometro {
	return &Cronometro{
		sorgente: PPS,
		acceso:   time.Now(),
		Adesso:   time.Now,
		scrivi:   scrivi,
	}
}

func (c *Cronometro) adesso() time.Time {
	if c.Adesso != nil {
		return c.Adesso()
	}
	return time.Now()
}

// Sorgente e ImpostaSorgente: l'interruttore PPS / RTC / nessun tempo.
func (c *Cronometro) Sorgente() Sorgente {
	c.mu.Lock()
	defer c.mu.Unlock()
	return c.sorgente
}

func (c *Cronometro) ImpostaSorgente(s Sorgente) {
	c.mu.Lock()
	c.sorgente = s
	c.mu.Unlock()
}

// Passaggio simula un'interruzione del fascio. Ritorna false se l'antirimbalzo
// l'ha inghiottito, così la GUI può dirlo invece di far sembrare che il pulsante
// non funzioni.
func (c *Cronometro) Passaggio() (accettato bool) {
	c.mu.Lock()
	ora := c.adesso()
	// La finestra si sposta anche sui rilevamenti SCARTATI: è il senso di
	// «retriggerabile», e senza questo un'auto che copre il fascio a
	// intermittenza produrrebbe un passaggio ogni secondo.
	troppoPresto := !c.ultimo.IsZero() && ora.Sub(c.ultimo) < antirimbalzo
	c.ultimo = ora
	if troppoPresto {
		c.scartati++
		c.mu.Unlock()
		return false
	}
	c.passaggi++
	src := c.sorgente
	c.mu.Unlock()

	// Senza base tempo il firmware non manda un passaggio: non avrebbe un istante
	// da dichiarare. Il fascio è stato interrotto e nessuno lo sa — che è
	// esattamente il guasto da poter provare.
	if src == Nessuna {
		return true
	}
	c.scrivi(fmt.Sprintf("P%s%c", unixConMs(ora), src))
	return true
}

// Comando risponde a una riga arrivata dal PC, con la stessa semantica del
// firmware: '?' pong, '@' diagnostica, '#' identità. Tutto il resto si ignora.
func (c *Cronometro) Comando(riga string) {
	riga = strings.TrimSpace(riga)
	if riga == "" {
		return
	}
	switch riga[0] {
	case '?':
		c.pong()
	case '@':
		c.scrivi(c.diagnostica())
	case '#':
		c.scrivi("Y" + versione)
	}
}

func (c *Cronometro) pong() {
	c.mu.Lock()
	src := c.sorgente
	c.mu.Unlock()
	if src == Nessuna {
		// Il firmware risponde comunque, ma senza istante: è così che brrm capisce
		// che il device è vivo e non sa che ora è.
		c.scrivi("KN")
		return
	}
	c.scrivi(fmt.Sprintf("K%c%s", src, unixConMs(c.adesso())))
}

// Scostamento fa mandare un W al prossimo giro, come il firmware quando l'ora
// del GPS e il suo riferimento interno divergono e lui si riallinea.
//
// È l'avviso più importante che il device sa dare — dice che i passaggi
// registrati appena prima sono sfasati di quel tanto — e poterlo provocare a
// comando è metà del motivo per cui questo simulatore esiste.
func (c *Cronometro) Scostamento(secondi int) {
	ora := c.adesso().Unix()
	c.scrivi(fmt.Sprintf("W%d,%d", ora+int64(secondi), ora))
}

// diagnostica compone la riga D con i SEDICI campi posizionali che brrm si
// aspetta, nell'ordine del firmware:
//
//	unix,src,dev_tcnt,nmea_ms,fix,sat,hdop,alt,nmea_persi,
//	since_pps_s,since_rtc_write_s,uptime_s,gps_ok,gps_unix,rtc_ok,rtc_unix
//
// I valori sono plausibili, non inventati a caso: con il GPS agganciato il fix è
// 'A' e i satelliti ci sono, in holdover il fix è 'V' e i secondi dall'ultimo
// PPS crescono. Una diagnostica che mostrasse dodici satelliti senza fix
// insegnerebbe a non fidarsi di quella schermata.
func (c *Cronometro) diagnostica() string {
	c.mu.Lock()
	src := c.sorgente
	acceso := c.acceso
	c.mu.Unlock()

	ora := c.adesso()
	uptime := int(ora.Sub(acceso).Seconds())
	unix := ora.Unix()

	fix, sat, hdop, alt := "V", 0, "0", "0"
	sincePPS := uptime // mai agganciato: quanto è acceso
	gpsOk, gpsUnix := 0, int64(0)
	if src == PPS {
		fix, sat, hdop, alt = "A", 9, "0.90", "312"
		sincePPS = 0
		gpsOk, gpsUnix = 1, unix
	}
	// L'RTC è presente in entrambi gli stati: è il device che ce l'ha a bordo.
	rtcOk, rtcUnix := 1, unix
	if src == Nessuna {
		rtcOk, rtcUnix = 0, 0
	}
	campi := []string{
		itoa64(unix), string(src),
		"0",   // dev_tcnt: deviazione dai tick ideali, zero = perfetto
		"0.0", // nmea_ms: ritardo dell'NMEA rispetto al secondo
		fix, itoa(sat), hdop, alt,
		"0", // nmea_persi
		itoa(sincePPS),
		"0", // since_rtc_write_s
		itoa(uptime),
		itoa(gpsOk), itoa64(gpsUnix), itoa(rtcOk), itoa64(rtcUnix),
	}
	return "D" + strings.Join(campi, ",")
}

// Conteggi per la GUI: quanti passaggi accettati e quanti inghiottiti
// dall'antirimbalzo.
func (c *Cronometro) Conteggi() (accettati, scartati int) {
	c.mu.Lock()
	defer c.mu.Unlock()
	return c.passaggi, c.scartati
}

// unixConMs formatta l'istante come <unix>.<ms> con i millisecondi a TRE cifre.
//
// Le tre cifre non sono cosmetica: il parser di brrm legge la parte dopo il punto
// come millesimi, quindi «.5» significherebbe 5 ms e non 500. È il genere di
// errore che si vede solo confrontando due tempi vicini.
func unixConMs(t time.Time) string {
	return fmt.Sprintf("%d.%03d", t.Unix(), t.Nanosecond()/1_000_000)
}

func itoa(n int) string     { return fmt.Sprintf("%d", n) }
func itoa64(n int64) string { return fmt.Sprintf("%d", n) }
