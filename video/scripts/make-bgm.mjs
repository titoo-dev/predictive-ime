// Génère assets/bgm.wav : un lit musical électro-pop déterministe (aucune
// dépendance, aucun échantillon externe). 120 BPM, 36 s, stéréo 44.1 kHz.
//   node scripts/make-bgm.mjs
import { writeFileSync, mkdirSync } from "node:fs";

const SR = 44100, DUR = 36, BPM = 120;
const BEAT = 60 / BPM, BAR = BEAT * 4;
const N = Math.floor(SR * DUR);
const L = new Float32Array(N), R = new Float32Array(N);

// Moments des balayages de la vidéo : riser + crash à chaque coupe.
const CUTS = [4.6, 9.0, 17.0, 22.0, 27.6, 32.0, 34.6];
const DRUMS_FROM = 4.6;            // les percussions entrent au premier balayage
const BREAK = [27.6, 29.6];        // respiration sur la scène « hors ligne »

// Progression vi – IV – I – V en La mineur / Do majeur, une mesure par accord.
const A = 220;
const semi = (n) => A * Math.pow(2, n / 12);
const CHORDS = [
  { root: semi(0), notes: [0, 3, 7, 12] },      // Am
  { root: semi(-4), notes: [0, 4, 7, 12] },     // F
  { root: semi(3), notes: [0, 4, 7, 12] },      // C
  { root: semi(-2), notes: [0, 4, 7, 12] },     // G
];
const chordAt = (t) => CHORDS[Math.floor(t / BAR) % CHORDS.length];

// Bruit blanc déterministe (LCG).
let seed = 1234567;
const rnd = () => { seed = (seed * 1664525 + 1013904223) >>> 0; return seed / 4294967296 * 2 - 1; };

const add = (i, l, r) => { if (i >= 0 && i < N) { L[i] += l; R[i] += r; } };
const env = (t, a, d) => (t < a ? t / a : Math.exp(-(t - a) / d));

// ---- Kick : sinus glissant 150 → 45 Hz, 4 temps par mesure ----
function kick(t0) {
  const len = 0.35;
  let ph = 0;
  for (let i = 0; i < len * SR; i++) {
    const t = i / SR;
    const f = 45 + 110 * Math.exp(-t * 28);
    ph += 2 * Math.PI * f / SR;
    const v = Math.sin(ph) * Math.exp(-t * 9) * 0.9;
    add(Math.floor(t0 * SR) + i, v, v);
  }
}
// ---- Charley : bruit court, plus long en contretemps ----
function hat(t0, open) {
  const len = open ? 0.18 : 0.05;
  let hp = 0;
  for (let i = 0; i < len * SR; i++) {
    const t = i / SR;
    const n = rnd(); const v = (n - hp) * 0.5; hp = n;   // passe-haut grossier
    const g = v * Math.exp(-t * (open ? 22 : 70)) * 0.22;
    add(Math.floor(t0 * SR) + i, g * 0.9, g * 1.1);
  }
}
// ---- Clap : rafale de bruit sur 2 et 4 ----
function clap(t0) {
  for (let k = 0; k < 3; k++) {
    const off = k * 0.012;
    for (let i = 0; i < 0.16 * SR; i++) {
      const t = i / SR;
      const v = rnd() * Math.exp(-t * 26) * 0.25;
      add(Math.floor((t0 + off) * SR) + i, v, v);
    }
  }
}
// ---- Crash + riser autour d'une coupe ----
function riser(tCut) {
  const len = 0.5;
  for (let i = 0; i < len * SR; i++) {
    const t = i / SR; const p = t / len;
    const v = rnd() * Math.pow(p, 3) * 0.28;
    add(Math.floor((tCut - len) * SR) + i, v * (1 - p * 0.5), v * (0.5 + p * 0.5));
  }
  let lp = 0;
  for (let i = 0; i < 1.2 * SR; i++) {
    const t = i / SR;
    const n = rnd(); lp += (n - lp) * 0.35;
    const v = (n - lp) * Math.exp(-t * 3.2) * 0.3;
    add(Math.floor(tCut * SR) + i, v, v);
  }
}
// ---- Basse : dent de scie filtrée, croches, octave alternée ----
function bass(t0, f, len) {
  let lp = 0;
  for (let i = 0; i < len * SR; i++) {
    const t = i / SR;
    const ph = (t * f) % 1;
    const saw = 2 * ph - 1;
    const sub = Math.sin(2 * Math.PI * f * 0.5 * t);
    lp += ((saw * 0.6 + sub * 0.8) - lp) * 0.09;
    const v = lp * env(t, 0.004, 0.16) * 0.5;
    add(Math.floor(t0 * SR) + i, v, v);
  }
}
// ---- Nappe : trois dents de scie désaccordées par note, filtre lent ----
function pad(t0, chord, len, gain) {
  const freqs = chord.notes.map((n) => chord.root * Math.pow(2, n / 12));
  let lpL = 0, lpR = 0;
  for (let i = 0; i < len * SR; i++) {
    const t = i / SR;
    let sL = 0, sR = 0;
    for (const f of freqs) {
      sL += (2 * ((t * f * 0.996) % 1) - 1) + (2 * ((t * f * 1.004) % 1) - 1) * 0.6;
      sR += (2 * ((t * f * 1.003) % 1) - 1) + (2 * ((t * f * 0.995) % 1) - 1) * 0.6;
    }
    const cut = 0.025 + 0.02 * Math.sin(2 * Math.PI * t / 6);
    lpL += (sL - lpL) * cut; lpR += (sR - lpR) * cut;
    const e = t < 0.4 ? t / 0.4 : t > len - 0.3 ? (len - t) / 0.3 : 1;
    add(Math.floor(t0 * SR) + i, lpL * e * gain * 0.09, lpR * e * gain * 0.09);
  }
}
// ---- Arpège : triangle court, doubles-croches ----
function pluck(t0, f, panR) {
  const len = 0.22;
  for (let i = 0; i < len * SR; i++) {
    const t = i / SR;
    const ph = (t * f) % 1;
    const tri = 4 * Math.abs(ph - 0.5) - 1;
    const v = tri * Math.exp(-t * 16) * 0.19;
    add(Math.floor(t0 * SR) + i, v * (1 - panR), v * panR);
  }
}

// ---------------- Séquence ----------------
const inBreak = (t) => t >= BREAK[0] && t < BREAK[1];
for (let bar = 0; bar * BAR < DUR; bar++) {
  const tb = bar * BAR; const chord = chordAt(tb);
  const intro = tb < DRUMS_FROM;
  pad(tb, chord, BAR + 0.05, intro ? 0.8 : 1);
  for (let b = 0; b < 4; b++) {
    const t = tb + b * BEAT;
    const drums = t >= DRUMS_FROM && !inBreak(t) && t < DUR - 1.2;
    if (drums) {
      kick(t);
      if (b === 1 || b === 3) clap(t);
      hat(t, false); hat(t + BEAT / 2, true);
    }
    // basse : croches, octave sur le contretemps
    if (!inBreak(t)) {
      bass(t, chord.root / 2, BEAT / 2 - 0.02);
      bass(t + BEAT / 2, chord.root, BEAT / 2 - 0.02);
    }
    // arpège : doubles-croches sur les notes de l'accord, une octave plus haut
    for (let s = 0; s < 4; s++) {
      const idx = (b * 4 + s) % chord.notes.length;
      const f = chord.root * 2 * Math.pow(2, chord.notes[idx] / 12);
      pluck(t + s * BEAT / 4, f, (idx % 2) ? 0.7 : 0.3);
    }
  }
}
for (const c of CUTS) riser(c);

// ---- Sidechain (pompage sur chaque temps) + fondu + limiteur doux ----
for (let i = 0; i < N; i++) {
  const t = i / SR;
  let g = 1;
  if (t >= DRUMS_FROM && !inBreak(t)) {
    const phase = ((t - DRUMS_FROM) % BEAT) / BEAT;
    g = 0.55 + 0.45 * Math.min(1, phase * 3);
  }
  const fadeIn = Math.min(1, t / 0.8);
  const fadeOut = Math.min(1, Math.max(0, (DUR - t) / 2.2));
  const m = g * fadeIn * fadeOut;
  L[i] = Math.tanh(L[i] * m * 1.4) * 0.9;
  R[i] = Math.tanh(R[i] * m * 1.4) * 0.9;
}

// ---- WAV 16 bits stéréo ----
const buf = Buffer.alloc(44 + N * 4);
buf.write("RIFF", 0); buf.writeUInt32LE(36 + N * 4, 4); buf.write("WAVE", 8);
buf.write("fmt ", 12); buf.writeUInt32LE(16, 16); buf.writeUInt16LE(1, 20); buf.writeUInt16LE(2, 22);
buf.writeUInt32LE(SR, 24); buf.writeUInt32LE(SR * 4, 28); buf.writeUInt16LE(4, 32); buf.writeUInt16LE(16, 34);
buf.write("data", 36); buf.writeUInt32LE(N * 4, 40);
for (let i = 0; i < N; i++) {
  buf.writeInt16LE(Math.round(Math.max(-1, Math.min(1, L[i])) * 32767), 44 + i * 4);
  buf.writeInt16LE(Math.round(Math.max(-1, Math.min(1, R[i])) * 32767), 46 + i * 4);
}
mkdirSync("assets", { recursive: true });
writeFileSync("assets/bgm.wav", buf);
console.log("assets/bgm.wav", (buf.length / 1e6).toFixed(1), "MB");
