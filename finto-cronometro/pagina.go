package main

// La pagina di comando: un file solo, senza build e senza dipendenze.
//
// Grande e con pochi comandi, perché si usa mentre si guarda brrm sull'altro
// schermo: il pulsante del passaggio deve essere colpibile senza mirare.
const pagina = `<!doctype html>
<html lang="it">
<head>
<meta charset="utf-8">
<meta name="viewport" content="width=device-width,initial-scale=1">
<title>finto cronometro</title>
<style>
  :root { color-scheme: light dark; }
  body { font-family: ui-sans-serif, system-ui, sans-serif; margin: 0; padding: 1.5rem;
         max-width: 34rem; margin-inline: auto; }
  h1 { font-size: 1.1rem; margin: 0 0 0.25rem; }
  .seriale { font-family: ui-monospace, monospace; font-size: 0.85rem; opacity: 0.75;
             word-break: break-all; margin-bottom: 1.25rem; }
  fieldset { border: 1px solid #8884; border-radius: 0.5rem; margin: 0 0 1rem; padding: 0.75rem; }
  legend { font-size: 0.8rem; text-transform: uppercase; letter-spacing: 0.04em; opacity: 0.7; }
  .fonti { display: flex; gap: 0.5rem; flex-wrap: wrap; }
  .fonti button { flex: 1; min-width: 7rem; padding: 0.6rem; font-size: 0.9rem;
                  border: 1px solid #8886; border-radius: 0.4rem; background: transparent;
                  cursor: pointer; }
  .fonti button.sel { background: #16a34a; color: #fff; border-color: #16a34a; font-weight: 600; }
  .fonti button.sel.warn { background: #d97706; border-color: #d97706; }
  .fonti button.sel.off  { background: #b91c1c; border-color: #b91c1c; }
  #passaggio { width: 100%; padding: 2rem; font-size: 1.5rem; font-weight: 700;
               border: 0; border-radius: 0.6rem; background: #0369a1; color: #fff;
               cursor: pointer; }
  #passaggio:active { transform: scale(0.99); }
  .conta { display: flex; gap: 1.5rem; margin-top: 0.75rem; font-size: 0.9rem; }
  .conta b { font-variant-numeric: tabular-nums; }
  .esito { min-height: 1.4rem; margin-top: 0.5rem; font-size: 0.85rem; }
  .esito.scartato { color: #b91c1c; }
  .riga { display: flex; gap: 0.5rem; align-items: center; }
  .riga input { width: 5rem; padding: 0.35rem; }
  .riga button { padding: 0.35rem 0.7rem; }
  p.nota { font-size: 0.8rem; opacity: 0.75; margin: 0.5rem 0 0; }
</style>
</head>
<body>
<h1>finto cronometro</h1>
<div class="seriale">{{SERIALE}}</div>

<fieldset>
  <legend>base tempo</legend>
  <div class="fonti">
    <button data-v="G" id="bG">PPS &middot; GPS agganciato</button>
    <button data-v="R" id="bR" class="warn">RTC &middot; holdover</button>
    <button data-v="N" id="bN" class="off">nessun tempo</button>
  </div>
  <p class="nota">L'ora &egrave; sempre quella di sistema: cambia la QUALIT&Agrave; dichiarata,
     che &egrave; quella che brrm usa per decidere quanto fidarsi del tempo.
     Con &laquo;nessun tempo&raquo; il passaggio non viene nemmeno mandato, come fa il firmware.</p>
</fieldset>

<fieldset>
  <legend>fascio</legend>
  <button id="passaggio">PASSAGGIO</button>
  <div class="conta">
    <span>accettati <b id="acc">0</b></span>
    <span>scartati dall'antirimbalzo <b id="sca">0</b></span>
  </div>
  <div class="esito" id="esito"></div>
  <p class="nota">Antirimbalzo di 1 secondo, retriggerabile come nel firmware:
     due colpi ravvicinati contano come uno.</p>
</fieldset>

<fieldset>
  <legend>guasti da provare</legend>
  <div class="riga">
    <label for="sec">scostamento dell'orologio</label>
    <input id="sec" type="number" value="3" step="1">
    <span>s</span>
    <button id="scosta">manda W</button>
  </div>
  <p class="nota">&Egrave; l'avviso che il device manda quando si riallinea: i passaggi
     registrati appena prima sono sfasati di quel tanto. In brrm compare come banda
     nella schermata di gara.</p>
</fieldset>

<script>
const j = (u) => fetch(u).then((r) => r.json());

function segna(s) {
  for (const v of ['G', 'R', 'N']) {
    document.getElementById('b' + v).classList.toggle('sel', v === s);
  }
}

async function stato() {
  const s = await j('/stato');
  segna(s.sorgente);
  document.getElementById('acc').textContent = s.passaggi;
  document.getElementById('sca').textContent = s.scartati;
}

for (const b of document.querySelectorAll('.fonti button')) {
  b.onclick = async () => { await j('/sorgente?v=' + b.dataset.v); stato(); };
}

document.getElementById('passaggio').onclick = async () => {
  const r = await j('/passaggio');
  document.getElementById('acc').textContent = r.passaggi;
  document.getElementById('sca').textContent = r.scartati;
  const e = document.getElementById('esito');
  // Dire che l'antirimbalzo l'ha inghiottito, invece di lasciar credere che il
  // pulsante non funzioni: è lo stesso principio dei messaggi di brrm.
  e.textContent = r.accettato ? 'passaggio mandato' : 'scartato: troppo vicino al precedente';
  e.className = 'esito' + (r.accettato ? '' : ' scartato');
};

document.getElementById('scosta').onclick = async () => {
  const s = document.getElementById('sec').value;
  const r = await fetch('/scostamento?s=' + s);
  const e = document.getElementById('esito');
  e.textContent = r.ok ? 'avviso di scostamento mandato' : await r.text();
  e.className = 'esito' + (r.ok ? '' : ' scartato');
};

// La tastiera: la barra spaziatrice fa un passaggio. Con un mouse solo e due
// schermi, mirare il pulsante mentre si guarda brrm è la parte scomoda.
document.addEventListener('keydown', (ev) => {
  if (ev.code === 'Space' && ev.target.tagName !== 'INPUT') {
    ev.preventDefault();
    document.getElementById('passaggio').click();
  }
});

stato();
setInterval(stato, 2000);
</script>
</body>
</html>
`
