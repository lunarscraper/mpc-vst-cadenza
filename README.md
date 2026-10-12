# mpc-vst-cadenza

**Cadenza** / **Cadenza FX**: die Offline-Engine von [Cadenza](https://github.com/Andrea-Cavallo/cadenza)
(Andrea Cavallo, MIT) als VST2-MIDI-Generator für den Plugin-Host von MPC OS (Force, MPC Live/One/X/Key).
Aus Tonart, Skala, Stil und Seed entsteht eine gemeinsame Akkordfolge und darauf sieben Spuren, die das
Plugin im Takt der MPC über einen eigenen MIDI-Port an Synth-Tracks schickt, jede Spur auf ihrem eigenen
MIDI-Kanal.

| Spur | Kanal | Rolle (wie in Cadenza) |
|---|---|---|
| 1 GROOVE | 1 | Bass, synkopierter Groove |
| 2 ROLLING | 2 | Acid-/TB-303-Bass, fast jeder 16tel, Slides |
| 3 SUB | 3 | tiefer Sub-Bass, sparsam, gebunden |
| 4 ARP | 4 | Akkord-Arpeggio mit Septime/None |
| 5 MELODY | 5 | Lead-Melodie aus Kontur-Phrasen (Bogen, Frage-Antwort, Spannung, Abstieg) |
| 6 PAD | 6 | Akkordfläche (Drop-2-Voicing) |
| 7 LEAD | 7 | Staccato-Stab |

Aufbau, Build und Installation wie bei [mpc-vst-drumgen](https://github.com/lunarscraper/mpc-vst-drumgen)
(handgeschriebene VST2-Hülle, eigener ALSA-MIDI-Port, Steuerung per CC, Insert-Effekt-Variante).

## Zwei Varianten

| | Ordner | Datei | Typ | Workflow | ALSA-MIDI-Port |
|---|---|---|---|---|---|
| Cadenza | `vst/` | `cadenza.so` | Instrument (belegt einen der 8 Plugin-Slots) | VST release (draft) | `Cadenza` |
| Cadenza FX | `vst-fx/` | `cadenza_fx.so` | Insert-Effekt (belegt keinen Instrumenten-Slot) | VST release FX (draft) | `Cadenza FX` |

Beide entstehen aus derselben `vst/cadenza_vst.cpp`; `vst-fx/vst.json` setzt nur `"effect": true`.
Sie lassen sich nebeneinander installieren und teilen sich die Preset-Datei `cadenza_BANK.txt`.

## Wie der Generator arbeitet

`vst/cadenza_engine.h` ist eine 1:1-Portierung des Offline-Modus von Cadenza nach C++:

1. **Seed → SHA-256 → Entscheidungen.** Wie im Original wird jede Wahl aus dem SHA-256 von
   Seed + Tonart + Skala + Spurname abgeleitet: Akkordfolge, Groove-Paletten, Arp-Muster, Melodie-Motiv
   und -Konturen.
2. **Eine Akkordfolge für alle.** 4 Akkorde, je 4 Takte; alle sieben Spuren halten sich daran.
3. **16 Takte Arrangement.** Das 4-Takt-Motiv läuft viermal mit Cadenzas Entwicklung:
   INTRO (ausgedünnt) → BUILD (Füllnoten) → PEAK (dichter, Oktave hoch) → RELEASE.
4. **Renderer** wie im Original: Velocity-Raster, Akzente, Ghost-Notes, Timing-Versatz, Gate-Längen,
   Slides (überlappende Noten), Dynamikkurven, optional CC74-Filterfahrt und CC65-Portamento.

Gleicher Seed + Tonart + Skala + Stil + BPM ergibt immer dasselbe. `ref/parity.sh` vergleicht den
Port Ereignis für Ereignis (Note, Tick, Velocity, CC) mit dem Go-Original: 3000 zufällige
Kombinationen, keine Abweichung.

### Ergänzungen gegenüber Cadenza

- **Skala GOA PHRYG** (phrygisch-dominant, 1 b2 3 4 5 b6 b7) mit eigenem Akkordfolgen-Pool rund um das
  Goa-Pendel I – bII (z. B. E – F – E – F, E – Am – F – E, I – bII – bvii – I). Gilt überall dort als
  Moll, wo Cadenza nach Moll unterscheidet (dunklere Grooves, Arp- und Bass-Profile).
- **Skala HARM MIN** (harmonisch Moll; in Cadenza vorhanden, aber nicht auswählbar).
- **VARIATION** pro Spur (0 = `ORIG`, Cadenzas eigenes Muster; 1–99 = gleiche Harmonie, neues Motiv),
  **DICE** pro Spur, **VARIATE** für alle Spuren: entspricht den Regen-Tasten der Cadenza-Desktop-App.
- **FORM**: alle 16 Takte (`16 BARS`) oder nur ein Teil (`INTRO`, `BUILD`, `PEAK`, `RELEASE`) als
  4-Takt-Schleife, live umschaltbar.
- **PAD = CHORD** (Voreinstellung): Jeder Pad-Anschlag spielt den ganzen Akkord im Drop-2-Voicing.
  Cadenza selbst spielt das Pad einstimmig (`SINGLE`).
- **SWING**, **GATE** (25–200 %, Slides bleiben gebunden), **OCTAVE** und **CHANNEL** pro Spur.
- **Fehlerbehebung:** Im Original bleibt der Sub-Bass bei etwa jedem vierten Seed in einer
  Endlosschleife hängen (`ensureBassMaxDensityBounded`, wenn nur noch Akzente und Slides übrig sind).
  Der Port dünnt dort weiter aus (erst Slides ohne Akzent, dann alles außer dem Takt-Downbeat).

### Tempo

Cadenza wählt Grooves und Profile nach BPM (Grenzen bei 116, 118, 120, 124, 126, 128 und 130 BPM).
**TEMPO REF** = `HOST` nimmt das Projekttempo: beim Start der Wiedergabe wird ein Muster, das mit einem
anderen Tempo erzeugt wurde, neu berechnet. Das Projekt speichert das Tempo, mit dem das Muster
entstanden ist; ein geladenes Projekt spielt also dasselbe. Alternativ ein festes Bezugstempo 100–160.

## Auf der Force einsetzen

1. **Cadenza FX** als Insert auf einen beliebigen Track legen (z. B. den Bass-Track oder den Master).
   Er reicht das Audio unverändert durch.
2. Einmalig: Preferences → MIDI → beim Port `Cadenza FX` **Track** einschalten.
3. Bei jedem Synth-Track, der eine Spur spielen soll: MIDI-Eingang `Cadenza FX`, Kanal der Spur
   (1 Groove-Bass, 2 Rolling/Acid, 3 Sub, 4 Arp, 5 Melodie, 6 Pad, 7 Lead).
4. Play drücken. Nicht benutzte Spuren stören nicht; sie lassen sich auf LANES auch ausschalten.

Typische Kombination: eine der drei Bass-Spuren (passend zum Stil) + Arp + Pad + eine Melodie-Spur.
Der Stil (GROOVE / ROLLING / SUB) formt alle Spuren gemeinsam: Artikulation, Pad-Rhythmus und
-Lage, Lead-Dichte.

Wie bei Euclidier FX und DrumGen FX steuert das Insert-Plugin über seinen eigenen MIDI-Port beliebige
Tracks an, auch Plugin-Tracks; eine Cadenza-FX-Instanz reicht für alle sieben Spuren.

### Seiten und Q-Links

| Seite | Inhalt | Q-Links (nur Regler, keine Schalter) |
|---|---|---|
| MAIN | Key, Scale, Style, Seed, Generate, Variate, Prog, Form, Akkorde, Status, aktueller Takt aller 7 Spuren als Text | Key, Scale, Style, Seed, Prog, Form, Swing, Gate |
| LANES 1-4 / 5-7 | pro Spur: On, Channel, Octave, Variation, Dice, Steps | Octave und Variation der Spuren |
| FEEL | Bass Feel, Tempo Ref, Swing, Gate, Pad, CC Out | alle sechs |
| PRESETS | Preset 1–32, Load, Save, Control Ch, Status | Preset, Control Ch |

Skin-Vorschau (ohne Live-Werte): `docs/skin-main.png`, `docs/skin-lanes-1-4.png`,
`docs/skin-lanes-5-7.png`, `docs/skin-feel.png`, `docs/skin-presets.png`.

- **SEED** 0–9999, **GENERATE** würfelt einen neuen Seed (Variationen zurück auf `ORIG`).
- **PROG**: `SEED` = Akkordfolge aus dem Seed wie bei Cadenza, `1`–`12` = Vorlage direkt wählen
  (bei Skalen mit 8 Vorlagen bleibt sie bei 8 stehen).
- **BASS FEEL** ist Cadenzas Offline-Stil der Groove-Bass-Spur: `MELODIC` (Voreinstellung wie im
  Original), `HYPNOTIC` (2 Noten pro Viertel, gebunden), `DRIVING` (Staccato, mehr Slides), `MINIMAL`.
- **CC OUT**: `OFF` (Voreinstellung), `SWEEP` (CC74-Filterfahrt wie in Cadenzas MIDI-Dateien),
  `PORTA` (CC65 vor Slides, CC5 Portamento-Zeit), `BOTH`. Nur einschalten, wenn der Synth darauf hört.
- Die Textzeilen zeigen den laufenden Takt: Name der ersten Note, dann 16 Steps,
  `X` = laut (Velocity ≥ 100), `x` = normal, `-` = leise/Ghost, `.` = Pause.
- **STATUS**: Takt in der Schleife, Teil (INTRO/BUILD/PEAK/RELEASE) und das Tempo des Musters.

## Steuerung per MIDI-CC (beide Varianten)

Wie bei DrumGen: zweiter MIDI-Port „Control In“, der sich selbst mit allen Hardware-MIDI-Eingängen
verbindet (auch später eingesteckten). Ausgewertet werden nur CCs auf **Control Ch** (OFF, 1–16;
Voreinstellung 16; wird mit dem Projekt gespeichert). CCs, die an den Track geschickt werden, zählen
ebenfalls.

CC-Nummer = 10 × Spur + Funktion, also z. B. Spur 5 Variation = CC 53:

| Endziffer | 0 | 1 | 2 | 3 | 4 |
|---|---|---|---|---|---|
| Funktion | On | Channel | Octave | Variation | Dice |

Global: CC 100 Key, 101 Scale, 102 Style, 103 Bass Feel, 104 Seed, 105 Prog, 106 Generate,
107 Variate, 108 Form, 109 Swing, 110 Gate, 111 Pad, 112 CC Out, 113 Tempo Ref, 114 Preset,
115 Load, 116 Save. Der Wertebereich 0–127 wird auf den Parameter verteilt; Schalter schalten ab 64
ein, Tasten lösen ab 64 aus. Mit einem Faderfox EC4: Key, Scale, Form und die Variationen auf
Drehregler, Generate/Variate auf Taster.

## Presets und Projekt

Das Projekt speichert alle Einstellungen und das Tempo des Musters; da alles aus den Einstellungen
folgt, spielt ein geladenes Projekt genau dasselbe. Presets (32 Plätze) speichern dasselbe ohne
Control Ch; Datei: `/sdcard/vst/cadenza_BANK.txt`.

## Technik

- Neu berechnet wird nie im Audio-Thread (alle sieben Spuren dauern auf x86 etwa 0,2 ms, auf dem
  Gerät ein Vielfaches): Bedienung am Display rechnet im UI-Thread, CCs und Tempowechsel
  wecken einen eigenen Worker-Thread; der Audio-Thread tauscht nur die fertigen Abläufe ein und
  beendet dabei gehaltene Noten der geänderten Spuren.
- Takt und Step kommen aus der MPC-Transportposition (96 Pulse pro Viertel, PpqClock aus Euclidier/
  DrumGen); die 16-Takt-Form beginnt bei Takt 1 des Projekts, auch nach Loop oder Sprung.
- Überlappende Slide-Noten derselben Tonhöhe werden gezählt: das Note-Off kommt erst mit dem
  letzten Note-On, es bleiben keine Noten hängen und keine werden zu früh abgeschnitten.

## Build

GitHub Actions → „VST release (draft)“ bzw. „VST release FX (draft)“ → Run workflow (wie bei DrumGen;
`dry_run` baut nur das ZIP und die Skin-Vorschau als Artefakt). Lokal: `vst/build.sh` bzw.
`vst-fx/build.sh` (Docker, armhf), Offline-Test `vst/test.sh` / `vst-fx/test.sh` (x86, ASan/UBSan:
gespielte Noten = Engine, Swing, Form, Spuren, Kanal, Oktave, Variation, Goa, Pad-Akkorde, CC74,
CC-Steuerung, Tempo, Chunk, Presets, alle Tonarten/Skalen/Stile ohne hängende Noten).
Parameterliste und Skin-Layout erzeugt `vst/gen_layout.py`; `vst/captions.py` zeichnet nach dem
Skin-Bau alle Beschriftungen in Titillium Web in der Größe der Knob-Namen. Die Reihenfolge der
Parameter ist der VST-Index und darf sich nie ändern (nur hinten anhängen).

Paritätstest gegen das Original: `ref/parity.sh [Anzahl]` (braucht Go ≥ 1.22 und g++).

Diagnose: `/tmp/cadenza_vst.log` (jede Neuberechnung mit Dauer und Akkorden, alle 5 s eine
Statuszeile mit Pulsen, Noten, empfangenen CCs).

## Lizenzen

- mpc-vst-cadenza: MIT (`LICENSE`).
- Cadenza (Engine-Vorbild, `ref/go/`): © 2026 Andrea Cavallo, MIT (`LICENSE-cadenza`).
- Schrift Titillium Web (`vst/fonts/`): SIL Open Font License 1.1 (`vst/fonts/OFL.txt`).

## Hinweis

Entwickelt mit Unterstützung von Claude (Anthropic)
