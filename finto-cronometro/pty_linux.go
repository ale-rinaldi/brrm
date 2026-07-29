//go:build linux

package main

import (
	"fmt"
	"os"
	"syscall"
	"unsafe"
)

// Creazione della pseudo-seriale su Linux, con la sola libreria standard.
const (
	tioctlSPTLCK = 0x40045431 // TIOCSPTLCK: sblocca lo slave (arg = 0)
	tioctlGPTN   = 0x80045430 // TIOCGPTN: ne ricava il numero
)

func apriPTY() (*os.File, string, error) {
	master, err := os.OpenFile("/dev/ptmx", os.O_RDWR, 0)
	if err != nil {
		return nil, "", fmt.Errorf("apertura di /dev/ptmx: %w", err)
	}
	fd := master.Fd()
	var zero int32
	if err := ioctl(fd, tioctlSPTLCK, uintptr(unsafe.Pointer(&zero))); err != nil {
		master.Close()
		return nil, "", fmt.Errorf("TIOCSPTLCK: %w", err)
	}
	var n int32
	if err := ioctl(fd, tioctlGPTN, uintptr(unsafe.Pointer(&n))); err != nil {
		master.Close()
		return nil, "", fmt.Errorf("TIOCGPTN: %w", err)
	}
	return master, fmt.Sprintf("/dev/pts/%d", n), nil
}

func ioctl(fd, richiesta, arg uintptr) error {
	if _, _, e := syscall.Syscall(syscall.SYS_IOCTL, fd, richiesta, arg); e != 0 {
		return e
	}
	return nil
}
