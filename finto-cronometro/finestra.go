//go:build gui

package main

import (
	"fmt"
	"image/color"
	"os"

	"gioui.org/app"
	"gioui.org/layout"
	"gioui.org/op"
	"gioui.org/text"
	"gioui.org/unit"
	"gioui.org/widget"
	"gioui.org/widget/material"
)

// La finestra nativa, dietro il tag di build `gui`.
//
// Dietro un tag perché Gio porta ventidue moduli, ottanta pacchetti e richiede
// cgo — su Linux anche gli header di X11 o Wayland. Chi vuole solo provare un
// passaggio da terminale non deve pagare quel prezzo né trovarsi una build che
// non compila su una macchina spoglia:
//
//	go build -o finto-cronometro .                 → tastiera + pannello web
//	go build -tags gui -o finto-cronometro-gui .   → anche la finestra
//
// Non Wails: quello incorpora un motore di browser (WebView2, WebKit). Non
// toglierebbe il browser, lo nasconderebbe, e aggiungerebbe npm e la sua CLI alla
// build per mostrare la stessa pagina che già c'è.

const conFinestra = true

// finestra apre la finestra e ci gira dentro. Ritorna quando la si chiude.
//
// Gio è immediate-mode: non esistono oggetti-widget con uno stato da aggiornare,
// si ridisegna tutto a ogni frame leggendo lo stato vero. Per questo pannello è
// un vantaggio — i contatori non possono andare fuori sincrono con il cronometro,
// perché non ne tengono una copia.
func finestra(c *Cronometro, slave string) {
	go func() {
		w := new(app.Window)
		w.Option(app.Title("finto cronometro"), app.Size(unit.Dp(420), unit.Dp(430)))
		if err := ciclo(w, c, slave); err != nil {
			fmt.Fprintln(os.Stderr, "finestra:", err)
		}
		os.Exit(0)
	}()
	app.Main()
}

func ciclo(w *app.Window, c *Cronometro, slave string) error {
	th := material.NewTheme()
	var ops op.Ops
	var (
		bPassaggio, bG, bR, bN, bW widget.Clickable
		esito                      string
	)

	for {
		switch e := w.Event().(type) {
		case app.DestroyEvent:
			return e.Err
		case app.FrameEvent:
			gtx := app.NewContext(&ops, e)

			if bPassaggio.Clicked(gtx) {
				if c.Passaggio() {
					esito = "passaggio mandato"
				} else {
					// Dirlo, invece di lasciar credere che il pulsante non funzioni.
					esito = "scartato: troppo vicino al precedente (antirimbalzo 1 s)"
				}
			}
			if bG.Clicked(gtx) {
				c.ImpostaSorgente(PPS)
				esito = ""
			}
			if bR.Clicked(gtx) {
				c.ImpostaSorgente(RTC)
				esito = ""
			}
			if bN.Clicked(gtx) {
				c.ImpostaSorgente(Nessuna)
				esito = ""
			}
			if bW.Clicked(gtx) {
				c.Scostamento(3)
				esito = "avviso di scostamento mandato: +3 s"
			}

			acc, sca := c.Conteggi()
			src := c.Sorgente()

			layout.UniformInset(unit.Dp(16)).Layout(gtx, func(gtx layout.Context) layout.Dimensions {
				return layout.Flex{Axis: layout.Vertical}.Layout(gtx,
					rigida(material.Body2(th, slave).Layout),
					spazio(12),
					rigida(etichetta(th, "BASE TEMPO")),
					spazio(6),
					layout.Rigid(func(gtx layout.Context) layout.Dimensions {
						return layout.Flex{Spacing: layout.SpaceEvenly}.Layout(gtx,
							layout.Flexed(1, bottone(th, &bG, "PPS", src == PPS, verde)),
							spazio(6),
							layout.Flexed(1, bottone(th, &bR, "RTC", src == RTC, ambra)),
							spazio(6),
							layout.Flexed(1, bottone(th, &bN, "nessuno", src == Nessuna, rosso)),
						)
					}),
					spazio(4),
					rigida(nota(th, "L'ora è sempre quella di sistema: cambia la qualità dichiarata.")),
					spazio(18),
					rigida(etichetta(th, "FASCIO")),
					spazio(6),
					layout.Rigid(func(gtx layout.Context) layout.Dimensions {
						b := material.Button(th, &bPassaggio, "PASSAGGIO")
						b.Background = blu
						b.TextSize = unit.Sp(22)
						b.Inset = layout.UniformInset(unit.Dp(22))
						return b.Layout(gtx)
					}),
					spazio(8),
					rigida(material.Body2(th,
						fmt.Sprintf("accettati %d     scartati dall'antirimbalzo %d", acc, sca)).Layout),
					spazio(4),
					rigida(func(gtx layout.Context) layout.Dimensions {
						l := material.Body2(th, esito)
						if esito != "" && esito[0] == 's' {
							l.Color = rosso
						}
						return l.Layout(gtx)
					}),
					spazio(18),
					rigida(etichetta(th, "GUASTI DA PROVARE")),
					spazio(6),
					layout.Rigid(func(gtx layout.Context) layout.Dimensions {
						return material.Button(th, &bW, "scostamento orologio +3 s").Layout(gtx)
					}),
				)
			})
			e.Frame(gtx.Ops)
		}
	}
}

var (
	verde  = color.NRGBA{R: 0x16, G: 0xa3, B: 0x4a, A: 0xff}
	ambra  = color.NRGBA{R: 0xd9, G: 0x77, B: 0x06, A: 0xff}
	rosso  = color.NRGBA{R: 0xb9, G: 0x1c, B: 0x1c, A: 0xff}
	blu    = color.NRGBA{R: 0x03, G: 0x69, B: 0xa1, A: 0xff}
	grigio = color.NRGBA{R: 0x64, G: 0x74, B: 0x8b, A: 0xff}
)

// bottone: acceso col suo colore quando è quello selezionato, spento altrimenti.
// Il colore è lo stesso della pagina web, così passare da una all'altra non
// richiede di reimparare cosa vuol dire il verde.
func bottone(th *material.Theme, cl *widget.Clickable, testo string, sel bool, c color.NRGBA) layout.Widget {
	return func(gtx layout.Context) layout.Dimensions {
		b := material.Button(th, cl, testo)
		if !sel {
			b.Background = color.NRGBA{A: 0x18}
			b.Color = th.Palette.Fg
		} else {
			b.Background = c
		}
		return b.Layout(gtx)
	}
}

func etichetta(th *material.Theme, testo string) layout.Widget {
	return func(gtx layout.Context) layout.Dimensions {
		l := material.Caption(th, testo)
		l.Color = grigio
		return l.Layout(gtx)
	}
}

func nota(th *material.Theme, testo string) layout.Widget {
	return func(gtx layout.Context) layout.Dimensions {
		l := material.Caption(th, testo)
		l.Color = grigio
		l.Alignment = text.Start
		return l.Layout(gtx)
	}
}

func rigida(wd layout.Widget) layout.FlexChild { return layout.Rigid(wd) }

func spazio(dp int) layout.FlexChild {
	return layout.Rigid(layout.Spacer{Height: unit.Dp(dp), Width: unit.Dp(dp)}.Layout)
}
