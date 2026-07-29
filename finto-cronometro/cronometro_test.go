package main

import (
	"strings"
	"testing"
	"time"
)

// raccolta cattura le righe che il cronometro manda sul filo.
func raccolta() (*Cronometro, *[]string) {
	var righe []string
	c := NuovoCronometro(func(r string) { righe = append(righe, r) })
	return c, &righe
}

// TestPassaggioNelFormatoDelFirmware: i millesimi vanno a TRE cifre, perché il
// parser di brrm legge la parte dopo il punto come millisecondi — «.5»
// significherebbe 5 ms invece di 500, ed è un errore che si vede solo confrontando
// due tempi vicini.
func TestPassaggioNelFormatoDelFirmware(t *testing.T) {
	c, righe := raccolta()
	c.Adesso = func() time.Time { return time.Unix(1718000000, 50*int64(time.Millisecond)) }
	if !c.Passaggio() {
		t.Fatal("il primo passaggio deve essere accettato")
	}
	if len(*righe) != 1 || (*righe)[0] != "P1718000000.050G" {
		t.Fatalf("atteso P1718000000.050G, ottenuto %v", *righe)
	}
}

// TestAntirimbalzoRetriggerabile replica DEBOUNCE_TIME_MS del firmware: la finestra
// si sposta anche sui colpi SCARTATI, altrimenti un fascio interrotto a
// intermittenza produrrebbe un passaggio al secondo.
func TestAntirimbalzoRetriggerabile(t *testing.T) {
	c, righe := raccolta()
	base := time.Unix(1718000000, 0)
	ora := base
	c.Adesso = func() time.Time { return ora }

	if !c.Passaggio() {
		t.Fatal("il primo passa")
	}
	ora = base.Add(900 * time.Millisecond)
	if c.Passaggio() {
		t.Error("a 900 ms deve essere scartato")
	}
	// Retriggerabile: il secondo colpo ha spostato la finestra, quindi a 1,7 s dal
	// PRIMO — ma solo 800 ms dal secondo — si scarta ancora.
	ora = base.Add(1700 * time.Millisecond)
	if c.Passaggio() {
		t.Error("la finestra si sposta sui colpi scartati: a 800 ms dal precedente si scarta")
	}
	ora = base.Add(2800 * time.Millisecond)
	if !c.Passaggio() {
		t.Error("passato un secondo pieno dall'ultimo colpo, deve passare")
	}
	if len(*righe) != 2 {
		t.Errorf("attese 2 righe sul filo, ottenute %d: %v", len(*righe), *righe)
	}
	acc, sca := c.Conteggi()
	if acc != 2 || sca != 2 {
		t.Errorf("attesi 2 accettati e 2 scartati, ottenuti %d e %d", acc, sca)
	}
}

// TestSenzaTempoNonMandaPassaggi: il firmware non ha un istante da dichiarare,
// quindi tace. Il fascio è stato interrotto e nessuno lo sa — ed è precisamente il
// guasto che si vuole poter provare.
func TestSenzaTempoNonMandaPassaggi(t *testing.T) {
	c, righe := raccolta()
	c.ImpostaSorgente(Nessuna)
	if !c.Passaggio() {
		t.Fatal("il fascio è stato interrotto: l'antirimbalzo non c'entra")
	}
	if len(*righe) != 0 {
		t.Errorf("senza base tempo non si manda niente, mandato %v", *righe)
	}
	// E il pong risponde comunque, senza istante: è così che brrm sa che il device
	// è vivo e non sa che ora è.
	c.Comando("?")
	if len(*righe) != 1 || (*righe)[0] != "KN" {
		t.Errorf("atteso KN, ottenuto %v", *righe)
	}
}

// TestDiagnosticaSedidiCampiCoerenti: i campi sono posizionali e brrm li mappa per
// indice, quindi il numero e l'ordine sono protocollo. E i valori devono essere
// COERENTI fra loro: una diagnostica che mostri nove satelliti senza fix
// insegnerebbe a non fidarsi di quella schermata.
func TestDiagnosticaSedidiCampiCoerenti(t *testing.T) {
	c, righe := raccolta()
	c.Comando("@")
	if len(*righe) != 1 {
		t.Fatalf("attesa una riga, ottenute %v", *righe)
	}
	campi := strings.Split(strings.TrimPrefix((*righe)[0], "D"), ",")
	if len(campi) != 16 {
		t.Fatalf("attesi 16 campi, ottenuti %d: %v", len(campi), campi)
	}
	// Con il GPS agganciato: fix A, satelliti, e zero secondi dall'ultimo PPS.
	if campi[1] != "G" || campi[4] != "A" || campi[5] == "0" || campi[9] != "0" {
		t.Errorf("con PPS attesi src=G fix=A sat>0 since_pps=0, ottenuto %v", campi[:10])
	}

	*righe = nil
	c.ImpostaSorgente(RTC)
	c.Comando("@")
	campi = strings.Split(strings.TrimPrefix((*righe)[0], "D"), ",")
	// In holdover: nessun fix, nessun satellite, e gps_ok a zero.
	if campi[1] != "R" || campi[4] != "V" || campi[5] != "0" || campi[12] != "0" {
		t.Errorf("in holdover attesi src=R fix=V sat=0 gps_ok=0, ottenuto %v", campi[:13])
	}
}

// TestIdentitaSiDichiaraSimulata: il rilevamento delle porte di brrm riconosce il
// cronometro dalla risposta al '#'. Il suffisso «-sim» serve a poter vedere in un
// log di gara che quei tempi non vengono da un cronometro vero.
func TestIdentitaSiDichiaraSimulata(t *testing.T) {
	c, righe := raccolta()
	c.Comando("#")
	if len(*righe) != 1 || !strings.HasPrefix((*righe)[0], "Y") {
		t.Fatalf("atteso Y<versione>, ottenuto %v", *righe)
	}
	if !strings.Contains((*righe)[0], "-sim") {
		t.Errorf("la versione deve dichiararsi simulata, è %q", (*righe)[0])
	}
}

// TestScostamentoNelFormatoDelFirmware: W<nmea>,<ref>, e la differenza è lo
// scostamento chiesto. È l'avviso più importante che il device sa dare.
func TestScostamentoNelFormatoDelFirmware(t *testing.T) {
	c, righe := raccolta()
	c.Adesso = func() time.Time { return time.Unix(1718000000, 0) }
	c.Scostamento(3)
	if len(*righe) != 1 || (*righe)[0] != "W1718000003,1718000000" {
		t.Fatalf("atteso W1718000003,1718000000, ottenuto %v", *righe)
	}
	*righe = nil
	c.Scostamento(-2)
	if (*righe)[0] != "W1717999998,1718000000" {
		t.Errorf("uno scostamento negativo va dichiarato tale, ottenuto %v", *righe)
	}
}

// TestComandiIgnotiSiIgnorano: il firmware non risponde a quel che non conosce, e
// un simulatore più loquace del vero nasconderebbe un errore di brrm.
func TestComandiIgnotiSiIgnorano(t *testing.T) {
	c, righe := raccolta()
	for _, cmd := range []string{"", "  ", "X", "PIPPO", "!"} {
		c.Comando(cmd)
	}
	if len(*righe) != 0 {
		t.Errorf("nessuna risposta attesa, ottenuto %v", *righe)
	}
}
