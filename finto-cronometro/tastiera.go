package main

import (
	"bufio"
	"fmt"
	"os"
	"strings"
)

// Comando da tastiera, nel terminale in cui il simulatore è già partito.
//
// Non è un ripiego della GUI: per questo strumento è spesso la via più corta —
// il terminale è già aperto, non c'è una finestra da trovare fra le altre, e
// funziona attraverso ssh, che è come si prova un nodo su un Raspberry.
//
// Una riga per comando invece dei singoli tasti: leggere un tasto alla volta
// richiede di mettere il terminale in modo raw, e un programma che esce male
// lascerebbe la shell senza eco — un fastidio vero per uno strumento che si
// interrompe con Ctrl-C dieci volte al giorno. Con Invio si perde mezzo secondo e
// non si rompe niente.
func tastiera(c *Cronometro, slave string) {
	aiuto := func() {
		fmt.Println()
		fmt.Println("  Invio (o p)  passaggio")
		fmt.Println("  g            base tempo: PPS, GPS agganciato")
		fmt.Println("  r            base tempo: RTC, holdover")
		fmt.Println("  n            base tempo: nessun tempo")
		fmt.Println("  w [secondi]  avviso di scostamento dell'orologio (default 3)")
		fmt.Println("  s            stato")
		fmt.Println("  ?            questo aiuto")
		fmt.Println()
	}
	stato := func() {
		acc, sca := c.Conteggi()
		fmt.Printf("  %s · base tempo %s · passaggi %d · scartati %d\n",
			slave, nomeSorgente(c.Sorgente()), acc, sca)
	}
	aiuto()
	stato()

	sc := bufio.NewScanner(os.Stdin)
	for {
		fmt.Print("> ")
		if !sc.Scan() {
			return // stdin chiuso: il programma resta vivo sulla seriale
		}
		riga := strings.TrimSpace(sc.Text())
		campi := strings.Fields(riga)
		cmd := ""
		if len(campi) > 0 {
			cmd = strings.ToLower(campi[0])
		}
		switch cmd {
		case "", "p":
			// Invio nudo è il passaggio: è il comando che si dà cento volte, e
			// deve costare un tasto.
			if c.Passaggio() {
				fmt.Println("  passaggio mandato")
			} else {
				// Dirlo, invece di lasciar credere che il comando non funzioni.
				fmt.Println("  scartato: troppo vicino al precedente (antirimbalzo 1 s)")
			}
		case "g":
			c.ImpostaSorgente(PPS)
			stato()
		case "r":
			c.ImpostaSorgente(RTC)
			stato()
		case "n":
			c.ImpostaSorgente(Nessuna)
			stato()
		case "w":
			secondi := 3
			if len(campi) > 1 {
				if n, err := parseInt(campi[1]); err == nil && n != 0 {
					secondi = n
				} else {
					fmt.Println("  secondi non validi (e diversi da zero)")
					continue
				}
			}
			c.Scostamento(secondi)
			fmt.Printf("  avviso di scostamento mandato: %+d s\n", secondi)
		case "s":
			stato()
		case "?", "h", "aiuto":
			aiuto()
		case "q", "esci":
			fmt.Println("  ciao")
			os.Exit(0)
		default:
			fmt.Printf("  comando ignoto: %q — «?» per l'aiuto\n", cmd)
		}
	}
}

func nomeSorgente(s Sorgente) string {
	switch s {
	case PPS:
		return "PPS (GPS agganciato)"
	case RTC:
		return "RTC (holdover)"
	default:
		return "nessun tempo"
	}
}

func parseInt(s string) (int, error) {
	var n int
	_, err := fmt.Sscanf(s, "%d", &n)
	return n, err
}
