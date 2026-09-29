import type { CSSProperties } from "react";
import Reveal from "./Reveal";

export default function Privacy() {
  return (
    <section className="relative overflow-hidden border-t hairline py-20 sm:py-28">
      <div aria-hidden className="absolute inset-0 -z-10 overflow-hidden">
        <div className="blob anim-float-b" style={{ width: 600, height: 600, left: "10%", top: -200, background: "var(--blob-c)", opacity: 0.3 }} />
        <div className="blob anim-float-a" style={{ width: 520, height: 520, right: "5%", bottom: -260, background: "var(--blob-a)", opacity: 0.3 }} />
      </div>
      <div className="mx-auto max-w-6xl px-5">
        <div className="grid items-center gap-12 lg:grid-cols-2">
          <Reveal className="order-2 lg:order-1">
            <Shield />
          </Reveal>
          <Reveal delay={100} className="order-1 lg:order-2">
            <p className="text-[12px] font-semibold uppercase tracking-[0.3em] text-ink-2">
              Hors ligne, par construction
            </p>
            <h2 className="mt-3 text-[34px] font-semibold leading-tight tracking-[-0.04em] sm:text-[48px]">
              Zéro télémétrie.
              <br />
              <span className="grad-text">Vos mots restent sur votre PC.</span>
            </h2>
            <p className="mt-4 text-[16px] leading-relaxed text-ink-2 sm:text-[17px]">
              Un clavier prédictif voit tout ce que vous écrivez. Predict ne
              parle donc à personne : pas de compte, pas de serveur, pas de
              réseau. Le modèle vit dans un dossier local, les mots appris dans
              un journal que vous pouvez lire et effacer.
            </p>
            <dl className="mt-8 grid grid-cols-3 gap-4">
              <Stat n="0" label="requête réseau" />
              <Stat n="5" label="candidats max" />
              <Stat n="2" label="langues + auto" />
            </dl>
          </Reveal>
        </div>
      </div>
    </section>
  );
}

function Stat({ n, label }: { n: string; label: string }) {
  return (
    <div className="flex flex-col rounded-xl border hairline bg-white/70 p-4">
      <dt className="order-2 text-[12px] text-ink-2">{label}</dt>
      <dd className="order-1 grad-text text-[32px] font-semibold tracking-[-0.04em]">{n}</dd>
    </div>
  );
}

function Shield() {
  return (
    <div className="relative mx-auto aspect-square w-full max-w-[420px]">
      <svg viewBox="0 0 400 400" className="h-full w-full" fill="none" aria-hidden>
        <defs>
          <linearGradient id="sh" x1="0" y1="0" x2="1" y2="1">
            <stop offset="0" stopColor="var(--m-blue)" />
            <stop offset="0.5" stopColor="var(--m-purple)" />
            <stop offset="1" stopColor="var(--m-pink)" />
          </linearGradient>
          <radialGradient id="glow">
            <stop offset="0" stopColor="var(--m-blue-l)" stopOpacity="0.25" />
            <stop offset="1" stopColor="var(--m-blue-l)" stopOpacity="0" />
          </radialGradient>
        </defs>
        <circle cx="200" cy="200" r="190" fill="url(#glow)" />
        {/* Orbites pointillées */}
        <g className="anim-spin-slow">
          <circle cx="200" cy="200" r="170" stroke="rgba(0,0,0,0.1)" strokeDasharray="2 8" />
          <circle cx="370" cy="200" r="6" fill="var(--m-amber-l)" />
        </g>
        <g className="anim-spin-rev">
          <circle cx="200" cy="200" r="135" stroke="rgba(0,0,0,0.08)" />
          <rect x="194" y="59" width="12" height="12" rx="4" fill="var(--m-teal-l)" />
        </g>
        {/* Bouclier tracé */}
        <g className="draw" strokeWidth="3" strokeLinecap="round" strokeLinejoin="round">
          <path
            d="M200 100 l70 26 v58 c0 48 -30 84 -70 100 c-40 -16 -70 -52 -70 -100 v-58 z"
            stroke="url(#sh)"
            style={{ "--len": 560 } as CSSProperties}
          />
          <path
            d="M172 196 l20 20 l40 -44"
            stroke="url(#sh)"
            style={{ "--len": 90, "--d": "1s" } as CSSProperties}
          />
        </g>
        {/* Paquets qui ne partent pas : barrés */}
        <g className="anim-bob" style={{ animationDelay: "0.4s" }}>
          <rect x="40" y="150" width="78" height="30" rx="8" fill="white" stroke="rgba(0,0,0,0.16)" />
          <text x="79" y="169" textAnchor="middle" fontSize="11" fontFamily="var(--font-mono)" fill="var(--text-2)">cloud</text>
          <path d="M48 153 L110 177" stroke="var(--m-pink)" strokeWidth="2.5" strokeLinecap="round" />
        </g>
        <g className="anim-bob" style={{ animationDelay: "1.1s" }}>
          <rect x="284" y="236" width="84" height="30" rx="8" fill="white" stroke="rgba(0,0,0,0.16)" />
          <text x="326" y="255" textAnchor="middle" fontSize="11" fontFamily="var(--font-mono)" fill="var(--text-2)">tracker</text>
          <path d="M292 239 L360 263" stroke="var(--m-pink)" strokeWidth="2.5" strokeLinecap="round" />
        </g>
        <g className="anim-bob">
          <rect x="132" y="318" width="136" height="30" rx="8" fill="var(--text)" />
          <text x="200" y="337" textAnchor="middle" fontSize="11" fontFamily="var(--font-mono)" fill="white">~/ime-predictord</text>
        </g>
      </svg>
    </div>
  );
}
