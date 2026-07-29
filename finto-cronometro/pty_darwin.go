//go:build darwin

package main

import (
	"fmt"
	"os"
	"syscall"
	"unsafe"
)

// Creazione della pseudo-seriale su macOS, con la sola libreria standard.
//
// Nessuna dipendenza: le tre ioctl che servono hanno numeri stabili
// nell'ABI di Darwin, e importare un pacchetto per tre costanti in un
// programma che ne ha zero non vale il prezzo.
const (
	tioctlPTYGrant = 0x20007454 // TIOCPTYGRANT: dà i permessi allo slave
	tioctlPTYUnlk  = 0x20007452 // TIOCPTYUNLK: lo sblocca
	tioctlPTYGname = 0x40807453 // TIOCPTYGNAME: ne ricava il nome (128 byte)
)

// apriPTY apre una coppia master/slave e ritorna il master e il PERCORSO dello
// slave, che è quello da dare a brrm come se fosse un Arduino.
func apriPTY() (*os.File, string, error) {
	master, err := os.OpenFile("/dev/ptmx", os.O_RDWR, 0)
	if err != nil {
		return nil, "", fmt.Errorf("apertura di /dev/ptmx: %w", err)
	}
	fd := master.Fd()
	if err := ioctl(fd, tioctlPTYGrant, 0); err != nil {
		master.Close()
		return nil, "", fmt.Errorf("TIOCPTYGRANT: %w", err)
	}
	if err := ioctl(fd, tioctlPTYUnlk, 0); err != nil {
		master.Close()
		return nil, "", fmt.Errorf("TIOCPTYUNLK: %w", err)
	}
	var nome [128]byte
	if err := ioctl(fd, tioctlPTYGname, uintptr(unsafe.Pointer(&nome[0]))); err != nil {
		master.Close()
		return nil, "", fmt.Errorf("TIOCPTYGNAME: %w", err)
	}
	n := 0
	for n < len(nome) && nome[n] != 0 {
		n++
	}
	return master, string(nome[:n]), nil
}

func ioctl(fd, richiesta, arg uintptr) error {
	if _, _, e := syscall.Syscall(syscall.SYS_IOCTL, fd, richiesta, arg); e != 0 {
		return e
	}
	return nil
}
