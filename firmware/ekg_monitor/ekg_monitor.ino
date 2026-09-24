/*
 * ESP32 EKG-Monitor
 * AD8232 an GPIO35, Abtastung mit 250 Hz, Anzeige im Browser.
 * Autor: Khaled Abu Mustafa, 2026 – MIT-Lizenz
 * Kein Medizinprodukt, nur für Lern- und Experimentierzwecke.
 */

#include <WiFi.h>
#include <WebServer.h>

// WLAN-Zugangsdaten stehen in secrets.h (wird nicht hochgeladen, siehe secrets.example.h)
#include "secrets.h"

// AD8232
const int EKG_PIN = 35;
const int SAMPLE_RATE = 250;            // Messungen pro Sekunde
const int BUF_SIZE = 2048;              // Ringpuffer (~8 s Reserve)

volatile uint16_t puffer[BUF_SIZE];     // Messwerte
volatile uint32_t schreibIndex = 0;     // wie viele Werte insgesamt gemessen
uint32_t leseIndex = 0;                 // bis wohin schon gesendet

WebServer server(80);

const char webpage[] PROGMEM = R"rawliteral(
<!DOCTYPE html>
<html lang="de">
<head>
<meta charset="UTF-8">
<meta name="viewport" content="width=device-width, initial-scale=1">
<title>ESP32 EKG</title>
<style>
  * { box-sizing: border-box; }
  body { margin: 0; font-family: system-ui, Arial, sans-serif; background: #f4f4f4; color: #222; }
  .wrap { max-width: 1100px; margin: 0 auto; padding: 16px; }
  header { display: flex; flex-wrap: wrap; justify-content: space-between; align-items: baseline; gap: 8px; }
  h1 { font-size: 20px; margin: 0; }
  #status { font-size: 14px; color: #777; }
  #status.warnung { color: #c00; font-weight: 600; }
  .werte { display: flex; flex-wrap: wrap; gap: 10px; margin: 12px 0; }
  .kachel { background: #fff; border: 1px solid #ddd; border-radius: 8px; padding: 8px 14px; min-width: 120px; }
  .label { font-size: 12px; color: #777; }
  .zahl { font-size: 26px; font-weight: 600; font-variant-numeric: tabular-nums; }
  .zahl small { font-size: 14px; font-weight: 400; color: #777; }
  #puls { color: #d62828; }
  canvas { display: block; width: 100%; height: 360px; background: #fff; border: 1px solid #ddd;
           border-radius: 8px; cursor: grab; touch-action: pan-y; }
  .steuerung { display: flex; flex-wrap: wrap; gap: 8px; align-items: center; margin-top: 12px; }
  button, select { font-size: 15px; padding: 8px 14px; border-radius: 6px; border: 1px solid #bbb;
                   background: #fff; color: #222; cursor: pointer; }
  button.aktiv { background: #d62828; border-color: #d62828; color: #fff; }
  input[type=range] { flex: 1; min-width: 200px; accent-color: #d62828; }
  .hinweis { font-size: 13px; color: #777; margin-top: 8px; line-height: 1.5; }
</style>
</head>
<body>
<div class="wrap">
  <header>
    <h1>ESP32 EKG Monitor</h1>
    <span id="status">Verbinde ...</span>
  </header>

  <div class="werte">
    <div class="kachel"><div class="label">Puls</div><div class="zahl"><span id="puls">--</span> <small>BPM</small></div></div>
    <div class="kachel"><div class="label">Aufnahme</div><div class="zahl" id="dauer">0:00</div></div>
    <div class="kachel"><div class="label">Ansicht</div><div class="zahl" id="position">LIVE</div></div>
  </div>

  <canvas id="ekg"></canvas>

  <div class="steuerung">
    <button id="pauseBtn">Pause</button>
    <select id="zoom">
      <option value="2">2 s</option>
      <option value="4" selected>4 s</option>
      <option value="8">8 s</option>
      <option value="15">15 s</option>
    </select>
    <input type="range" id="regler" min="0" max="0" value="0">
    <button id="csvBtn">CSV speichern</button>
  </div>
  <div class="hinweis">
    Zurückschauen: Regler oder Kurve mit der Maus / dem Finger ziehen. Tastatur: Leertaste = Pause, Pfeiltasten = blättern.<br>
    Raster wie EKG-Papier: kleines Kästchen = 40 ms, großes = 200 ms. Zahlen oben = Abstand zwischen zwei Herzschlägen.
  </div>
</div>

<script>
  const FS = 250;                   // muss zur SAMPLE_RATE passen
  const MAX_WERTE = FS * 600;       // max. 10 Minuten Verlauf im Browser

  const $ = id => document.getElementById(id);
  const canvas = $('ekg');
  const ctx = canvas.getContext('2d');
  const regler = $('regler');

  let roh = [];          // Originalwerte (für CSV)
  let gefiltert = [];    // geglättete Werte (für Anzeige)
  let verworfen = 0;     // wie viele alte Werte schon gelöscht wurden
  let letzte5 = [];      // Speicher für den 50-Hz-Filter
  let pausiert = false;
  let ansichtEnde = 0;   // rechter Rand der Ansicht (Messwert-Nummer), wenn pausiert
  let fensterSek = 4;
  let verbunden = false;
  let elektrodeLos = false;
  let reglerAktiv = false;
  let ziehen = null;

  // ---------- Daten holen ----------

  // Mittelwert über 5 Werte (= 20 ms) löscht 50-Hz-Netzbrummen
  function glaette(w) {
    letzte5.push(w);
    if (letzte5.length > 5) letzte5.shift();
    return letzte5.reduce((a, b) => a + b, 0) / letzte5.length;
  }

  async function hole() {
    try {
      const antwort = await fetch('/ekg', { cache: 'no-store' });
      const neu = (await antwort.text()).split(',').filter(s => s.length).map(Number);
      for (const w of neu) { roh.push(w); gefiltert.push(glaette(w)); }
      if (roh.length > MAX_WERTE) {
        const weg = roh.length - MAX_WERTE;
        roh.splice(0, weg); gefiltert.splice(0, weg); verworfen += weg;
      }
      const letzte = roh.slice(-50);
      elektrodeLos = letzte.length === 50 && letzte.every(w => w >= 4090 || w <= 5);
      verbunden = true;
    } catch (e) {
      verbunden = false;
    }
    setTimeout(hole, 100);
  }

  // ---------- Hilfsfunktionen ----------

  function zeit(sek, stellen) {
    sek = Math.max(0, sek);
    const m = Math.floor(sek / 60);
    const s = (sek - m * 60).toFixed(stellen);
    return m + ':' + s.padStart(stellen ? 3 + stellen : 2, '0');
  }

  // Findet die R-Zacken (höchste Spitzen) in einem Ausschnitt
  function findeSpitzen(g) {
    let min = Infinity, max = -Infinity;
    for (const w of g) { if (w < min) min = w; if (w > max) max = w; }
    if (max - min < 100) return [];            // kein echtes Signal
    const schwelle = min + 0.6 * (max - min);
    const spitzen = [];
    let letzte = -FS;
    for (let i = 1; i < g.length; i++) {
      if (g[i - 1] < schwelle && g[i] >= schwelle && i - letzte > FS * 0.3) {
        let top = i;
        for (let k = i; k < Math.min(g.length, i + FS * 0.1); k++) if (g[k] > g[top]) top = k;
        spitzen.push(top);
        letzte = top;
      }
    }
    return spitzen;
  }

  // Zeichnet Rasterlinien alle "schritt" Messwerte (quadratische Kästchen)
  function raster(schritt, farbe, links, rechts, px, W, H) {
    const abstand = schritt * px;
    if (abstand < 4) return;                   // zu eng -> weglassen
    ctx.strokeStyle = farbe;
    ctx.lineWidth = 1;
    ctx.beginPath();
    for (let i = Math.ceil(links / schritt) * schritt; i <= rechts; i += schritt) {
      const x = Math.round((i - links) * px) + 0.5;
      ctx.moveTo(x, 0); ctx.lineTo(x, H);
    }
    for (let y = H; y >= 0; y -= abstand) {
      const yy = Math.round(y) + 0.5;
      ctx.moveTo(0, yy); ctx.lineTo(W, yy);
    }
    ctx.stroke();
  }

  // ---------- Zeichnen ----------

  function zeichne() {
    const dpr = window.devicePixelRatio || 1;
    const W = canvas.clientWidth, H = canvas.clientHeight;
    if (canvas.width !== Math.round(W * dpr) || canvas.height !== Math.round(H * dpr)) {
      canvas.width = Math.round(W * dpr);
      canvas.height = Math.round(H * dpr);
    }
    ctx.setTransform(dpr, 0, 0, dpr, 0, 0);
    ctx.clearRect(0, 0, W, H);

    const n = fensterSek * FS;                              // Werte im Fenster
    const letzterIndex = verworfen + gefiltert.length;      // neuester Wert
    const fruehestes = verworfen + Math.min(n, gefiltert.length);
    let rechts = pausiert ? Math.round(ansichtEnde) : letzterIndex;
    rechts = Math.min(Math.max(rechts, fruehestes), letzterIndex);
    if (pausiert && !ziehen) ansichtEnde = rechts;
    const links = rechts - n;
    const px = W / n;                                       // Pixel pro Messwert
    const unten = H - 20;                                   // Platz für Zeitachse

    // EKG-Papier-Raster
    raster(10, '#f7dcdc', links, rechts, px, W, unten);     // 40 ms
    raster(50, '#eeb4b4', links, rechts, px, W, unten);     // 200 ms

    // Zeitachse
    const sekPx = FS * px;
    const jede = sekPx < 16 ? 5 : sekPx < 40 ? 2 : 1;
    ctx.fillStyle = '#777';
    ctx.font = '11px system-ui, Arial';
    ctx.textAlign = 'center';
    for (let s = Math.ceil(links / FS / jede) * jede; s * FS <= rechts; s += jede) {
      const x = (s * FS - links) * px;
      if (x > 15 && x < W - 15) ctx.fillText(s + ' s', x, H - 5);
    }

    // Kurve
    const a = Math.max(links, verworfen) - verworfen;
    const aus = gefiltert.slice(a, rechts - verworfen);
    const x0 = (a + verworfen - links) * px;
    let bpm = null;

    if (aus.length > 1) {
      let min = Infinity, max = -Infinity;
      for (const w of aus) { if (w < min) min = w; if (w > max) max = w; }
      const bereich = Math.max(max - min, 100);
      const mitte = (max + min) / 2;
      const yMin = mitte - bereich * 0.6, yMax = mitte + bereich * 0.6;
      const y = w => unten - (w - yMin) / (yMax - yMin) * unten;

      ctx.beginPath();
      for (let k = 0; k < aus.length; k++) {
        const xx = x0 + k * px;
        k === 0 ? ctx.moveTo(xx, y(aus[k])) : ctx.lineTo(xx, y(aus[k]));
      }
      ctx.strokeStyle = '#d62828';
      ctx.lineWidth = 1.8;
      ctx.lineJoin = 'round';
      ctx.stroke();

      // R-Zacken markieren + Abstand in ms
      const spitzen = elektrodeLos ? [] : findeSpitzen(aus);
      ctx.fillStyle = '#222';
      ctx.font = '12px system-ui, Arial';
      for (let i = 0; i < spitzen.length; i++) {
        const xs = x0 + spitzen[i] * px;
        ctx.beginPath();
        ctx.arc(xs, y(aus[spitzen[i]]), 3, 0, Math.PI * 2);
        ctx.fill();
        if (i > 0) {
          const ms = Math.round((spitzen[i] - spitzen[i - 1]) * 1000 / FS);
          const xm = (xs + x0 + spitzen[i - 1] * px) / 2;
          if (xs - (x0 + spitzen[i - 1] * px) > 50) ctx.fillText(ms + ' ms', xm, 14);
        }
      }
      if (spitzen.length >= 2) {
        const abstand = (spitzen[spitzen.length - 1] - spitzen[0]) / (spitzen.length - 1);
        bpm = Math.round(60 * FS / abstand);
      }
    }

    // Hinweis im Bild
    if (elektrodeLos || !verbunden) {
      ctx.fillStyle = 'rgba(255,255,255,0.8)';
      ctx.fillRect(0, H / 2 - 24, W, 48);
      ctx.fillStyle = '#c00';
      ctx.font = '600 18px system-ui, Arial';
      ctx.fillText(verbunden ? 'Elektrode prüfen' : 'Keine Verbindung zum ESP32', W / 2, H / 2 + 6);
    }

    // Anzeigen oben
    $('puls').textContent = bpm ? bpm : '--';
    $('dauer').textContent = zeit(letzterIndex / FS, 0);
    $('position').textContent = pausiert ? zeit(rechts / FS, 1) : 'LIVE';
    const st = $('status');
    st.textContent = !verbunden ? 'Keine Verbindung' : elektrodeLos ? 'Elektrode prüfen!' : 'Verbunden · ' + FS + ' Hz';
    st.className = (!verbunden || elektrodeLos) ? 'warnung' : '';

    // Regler
    regler.min = fruehestes;
    regler.max = letzterIndex;
    if (!reglerAktiv) regler.value = rechts;

    requestAnimationFrame(zeichne);
  }

  // ---------- Bedienung ----------

  function setzePause(p) {
    pausiert = p;
    if (p) ansichtEnde = verworfen + gefiltert.length;
    $('pauseBtn').textContent = p ? 'Weiter (Live)' : 'Pause';
    $('pauseBtn').classList.toggle('aktiv', p);
  }

  $('pauseBtn').addEventListener('click', e => { setzePause(!pausiert); e.target.blur(); });

  $('zoom').addEventListener('change', e => { fensterSek = Number(e.target.value); e.target.blur(); });

  regler.addEventListener('pointerdown', () => reglerAktiv = true);
  regler.addEventListener('change', () => reglerAktiv = false);
  regler.addEventListener('input', () => {
    if (!pausiert) setzePause(true);
    ansichtEnde = Number(regler.value);
  });

  // Kurve ziehen zum Blättern
  canvas.addEventListener('pointerdown', e => {
    if (!pausiert) setzePause(true);
    ziehen = { x: e.clientX, ende: ansichtEnde };
    canvas.setPointerCapture(e.pointerId);
    canvas.style.cursor = 'grabbing';
  });
  canvas.addEventListener('pointermove', e => {
    if (!ziehen) return;
    ansichtEnde = ziehen.ende - (e.clientX - ziehen.x) * (fensterSek * FS) / canvas.clientWidth;
  });
  const loslassen = () => { ziehen = null; canvas.style.cursor = ''; };
  canvas.addEventListener('pointerup', loslassen);
  canvas.addEventListener('pointercancel', loslassen);

  // Tastatur
  document.addEventListener('keydown', e => {
    if (e.target.tagName === 'SELECT' || e.target.tagName === 'INPUT') return;
    if (e.code === 'Space') { e.preventDefault(); setzePause(!pausiert); }
    if (e.code === 'ArrowLeft' || e.code === 'ArrowRight') {
      e.preventDefault();
      if (!pausiert) setzePause(true);
      ansichtEnde += (e.code === 'ArrowLeft' ? -1 : 1) * fensterSek * FS / 4;
    }
  });

  // Gesamte Aufnahme als CSV herunterladen
  $('csvBtn').addEventListener('click', e => {
    const zeilen = ['zeit_s;wert'];
    for (let i = 0; i < roh.length; i++) zeilen.push(((verworfen + i) / FS).toFixed(3) + ';' + roh[i]);
    const link = document.createElement('a');
    link.href = URL.createObjectURL(new Blob([zeilen.join('\n')], { type: 'text/csv' }));
    link.download = 'ekg.csv';
    link.click();
    e.target.blur();
  });

  hole();
  requestAnimationFrame(zeichne);
</script>
</body>
</html>
)rawliteral";


// Läuft als eigene Task: misst exakt alle 4 ms, egal was der Webserver macht
void abtastTask(void*) {
  TickType_t letzte = xTaskGetTickCount();
  const TickType_t periode = pdMS_TO_TICKS(1000 / SAMPLE_RATE);
  for (;;) {
    puffer[schreibIndex % BUF_SIZE] = analogRead(EKG_PIN);
    schreibIndex++;
    vTaskDelayUntil(&letzte, periode);
  }
}

// Schickt alle neuen Werte seit der letzten Anfrage, z.B. "2011,2015,2090,"
void handleEKG() {
  uint32_t ende = schreibIndex;
  if (ende - leseIndex > BUF_SIZE) leseIndex = ende - BUF_SIZE;
  String antwort;
  antwort.reserve((ende - leseIndex) * 5);
  while (leseIndex < ende) {
    antwort += puffer[leseIndex % BUF_SIZE];
    antwort += ',';
    leseIndex++;
  }
  server.send(200, "text/plain", antwort);
}

void handleRoot() {
  server.send(200, "text/html; charset=utf-8", webpage);
}

void setup() {
  Serial.begin(115200);
  analogReadResolution(12);

  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
  Serial.print("Verbinde mit WLAN");
  while (WiFi.status() != WL_CONNECTED) { delay(500); Serial.print("."); }
  Serial.println("\nIP-Adresse: " + WiFi.localIP().toString());

  xTaskCreatePinnedToCore(abtastTask, "ekg", 4096, NULL, 2, NULL, 1);

  server.on("/", handleRoot);
  server.on("/ekg", handleEKG);
  server.begin();
  Serial.println("Webserver gestartet!");
}

void loop() {
  server.handleClient();
}
