package main

import (
	"bufio"
	"encoding/json"
	"flag"
	"fmt"
	"log"
	"net"
	"net/http"
	"os"
	"os/signal"
	"strconv"
	"strings"
	"syscall"
)

// finto-cronometro: un cronometro Arduino simulato su una pseudo-seriale, con una
// paginetta per comandarlo.
//
// Perché esiste: provare brrm senza hardware. Una gara intera — passaggi,
// diagnostica, degrado del tempo, avvisi di riallineamento — senza un Arduino, un
// GPS e un cavo. Il caso che serviva di più è il terzo: gli stati che sul campo
// non si sanno riprodurre a comando.
//
// Come si usa:
//
//	./finto-cronometro
//	→ pseudo-seriale: /dev/ttys004   pannello: http://127.0.0.1:8099
//
// e in brrm, Avanzate → Trasporti → porta seriale = quel percorso. Il rilevamento
// delle porte lo riconosce da sé, perché risponde al '#' come il firmware vero.

func main() {
	addr := flag.String("addr", "127.0.0.1:8099", "indirizzo del pannello di comando")
	headless := flag.Bool("headless", false, "non stampare l'indirizzo del pannello")
	gui := flag.Bool("finestra", false, "apri la finestra nativa invece del comando da tastiera (richiede il build con -tags gui)")
	flag.Parse()

	master, slave, err := apriPTY()
	if err != nil {
		log.Fatalf("non riesco a creare la pseudo-seriale: %v", err)
	}
	defer master.Close()

	// La scrittura sul filo è serializzata da un canale invece che da un mutex: le
	// righe arrivano dal ciclo di lettura, dal pannello e dagli avvisi, e un
	// interleaving parziale produrrebbe una riga illeggibile — che è precisamente
	// il caso che il parser di brrm scarta senza dire niente.
	righe := make(chan string, 64)
	go func() {
		for r := range righe {
			if _, err := master.WriteString(r + "\n"); err != nil {
				log.Printf("scrittura sulla seriale: %v", err)
				return
			}
		}
	}()

	c := NuovoCronometro(func(r string) { righe <- r })

	// Il ciclo di lettura dei comandi del PC.
	go func() {
		sc := bufio.NewScanner(master)
		for sc.Scan() {
			c.Comando(sc.Text())
		}
		// EOF sul master significa che nessuno tiene aperto lo slave. Non è un
		// errore: brrm si è disconnesso, e riaprirà.
	}()

	mux := http.NewServeMux()
	pannello(mux, c, slave)
	ln, err := net.Listen("tcp", *addr)
	if err != nil {
		log.Fatalf("bind %s: %v", *addr, err)
	}
	go http.Serve(ln, mux)

	fmt.Printf("pseudo-seriale: %s\n", slave)
	if !*headless {
		fmt.Printf("pannello:       http://%s\n", ln.Addr().String())
		fmt.Printf("\nIn brrm: Avanzate → Trasporti → porta seriale = %s\n", slave)
	}

	// Chiusura ordinata su Ctrl-C anche mentre la tastiera aspetta una riga.
	sig := make(chan os.Signal, 1)
	signal.Notify(sig, os.Interrupt, syscall.SIGTERM)
	go func() {
		<-sig
		fmt.Println()
		os.Exit(0)
	}()

	if *gui {
		if !conFinestra {
			log.Fatal("questo binario è compilato senza la finestra: rifallo con " +
				"`go build -tags gui`, oppure usa il comando da tastiera o il pannello web")
		}
		finestra(c, slave)
		return
	}

	// Il comando da tastiera è la via PRINCIPALE: il terminale è già aperto, non
	// c'è una finestra da cercare, e funziona attraverso ssh — che è come si prova
	// un nodo su un Raspberry. Il pannello nel browser resta per chi vuole il
	// pulsante grosso o comandarlo da un telefono.
	tastiera(c, slave)
}

// pannello registra la paginetta di comando e le sue azioni.
//
// Una pagina servita dal programma stesso invece di una GUI nativa: nessun
// toolkit, nessuna dipendenza, e funziona anche quando il simulatore gira su una
// macchina diversa da quella che si guarda — che capita, perché la seriale
// finta serve spesso su un portatile senza schermo comodo.
func pannello(mux *http.ServeMux, c *Cronometro, slave string) {
	mux.HandleFunc("/", func(w http.ResponseWriter, r *http.Request) {
		w.Header().Set("Content-Type", "text/html; charset=utf-8")
		// Sostituzione e non Fprintf: la pagina contiene «100%» nel CSS, e come
		// stringa di formato quel percento diventerebbe un verbo sconosciuto — cosa
		// che go vet dice, e che altrimenti si vedrebbe come pagina rotta.
		fmt.Fprint(w, strings.Replace(pagina, "{{SERIALE}}", slave, 1))
	})
	mux.HandleFunc("/stato", func(w http.ResponseWriter, r *http.Request) {
		acc, sca := c.Conteggi()
		writeJSON(w, map[string]any{
			"sorgente": string(c.Sorgente()),
			"seriale":  slave,
			"passaggi": acc,
			"scartati": sca,
		})
	})
	mux.HandleFunc("/passaggio", func(w http.ResponseWriter, r *http.Request) {
		ok := c.Passaggio()
		acc, sca := c.Conteggi()
		writeJSON(w, map[string]any{"accettato": ok, "passaggi": acc, "scartati": sca})
	})
	mux.HandleFunc("/sorgente", func(w http.ResponseWriter, r *http.Request) {
		switch r.URL.Query().Get("v") {
		case "G":
			c.ImpostaSorgente(PPS)
		case "R":
			c.ImpostaSorgente(RTC)
		case "N":
			c.ImpostaSorgente(Nessuna)
		default:
			http.Error(w, "sorgente non valida: usa G, R o N", http.StatusBadRequest)
			return
		}
		writeJSON(w, map[string]any{"sorgente": string(c.Sorgente())})
	})
	mux.HandleFunc("/scostamento", func(w http.ResponseWriter, r *http.Request) {
		n, err := strconv.Atoi(r.URL.Query().Get("s"))
		if err != nil || n == 0 {
			http.Error(w, "secondi di scostamento non validi (e diversi da zero)", http.StatusBadRequest)
			return
		}
		c.Scostamento(n)
		writeJSON(w, map[string]any{"scostamento": n})
	})
}

func writeJSON(w http.ResponseWriter, v any) {
	w.Header().Set("Content-Type", "application/json")
	json.NewEncoder(w).Encode(v)
}
