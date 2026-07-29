//go:build !gui

package main

// Senza il tag `gui` la finestra non esiste: il programma non porta Gio, non
// richiede cgo, e compila su una macchina spoglia. Il flag -finestra lo dice
// invece di fallire in silenzio.
const conFinestra = false

func finestra(c *Cronometro, slave string) {
	panic("compilato senza il tag gui") // non raggiungibile: main lo controlla
}
