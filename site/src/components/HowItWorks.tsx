import type { CSSProperties } from "react";
import Reveal from "./Reveal";

/**
 * Schéma animé : frappe → frontend (TSF / fcitx5) → core partagé → predictord → barre.
 * Les flux animés (stroke-dashoffset) montrent la requête et la réponse JSON.
 */
export default function HowItWorks() {
  return (
    <section id="how" className="relative scroll-mt-20 overflow-hidden border-t hairline py-20 sm:py-28">
      <div aria-hidden className="bg-grid absolute inset-0 -z-10" />
      <div className="mx-auto max-w-6xl px-5">
        <div className="grid items-center gap-12 lg:grid-cols-[1fr_1.2fr]">
          <Reveal>
            <p className="text-[12px] font-semibold uppercase tracking-[0.3em] text-ink-2">
              Sous le capot
            </p>
            <h2 className="mt-3 text-[34px] font-semibold leading-tight tracking-[-0.04em] sm:text-[48px]">
              Un moteur, <span className="grad-text">deux systèmes</span>
            </h2>
            <p className="mt-4 text-[16px] leading-relaxed text-ink-2 sm:text-[17px]">
              Sur Windows, Predict est un service texte natif (TSF) ; sur Linux,
              un moteur fcitx5. Les deux partagent la même logique de saisie et
              interrogent <code className="rounded-md bg-white px-1.5 py-0.5 font-mono text-[14px] text-ink">predictord</code>,
              un démon n-gramme local, par une socket Unix. Une ligne JSON dans un
              sens, cinq candidats dans l&apos;autre.
            </p>
            <ul className="mt-6 space-y-3 text-[15px] text-ink-2">
              <Li c="var(--m-blue)">Réponse en quelques millisecondes, sans réseau.</Li>
              <Li c="var(--m-purple)">Modèle CC BY-SA construit à partir de corpus ouverts.</Li>
              <Li c="var(--m-teal)">Réglages rechargés à chaud : <code className="font-mono text-[13.5px]">config.json</code>, dictionnaire, snippets.</Li>
              <Li c="var(--m-pink)">Une barre dessinée en Direct2D qui suit l&apos;accent, le thème et le DPI de Windows 11.</Li>
            </ul>
          </Reveal>

          <Reveal delay={120}>
            <Pipeline />
          </Reveal>
        </div>
      </div>
    </section>
  );
}

function Li({ c, children }: { c: string; children: React.ReactNode }) {
  return (
    <li className="flex items-start gap-3">
      <span className="mt-[7px] inline-block h-2 w-2 shrink-0 rounded-full" style={{ background: c }} />
      <span>{children}</span>
    </li>
  );
}

function Pipeline() {
  const box = { fill: "white", stroke: "rgba(0,0,0,0.16)", strokeWidth: 1 };
  const sans = "var(--font-sans)";
  const mono = "var(--font-mono)";
  return (
    <div className="card p-4 sm:p-6">
      <svg viewBox="0 0 640 360" className="w-full" fill="none" aria-label="Schéma : frappe, frontend, core, daemon, candidats">
        <defs>
          <linearGradient id="pipe-grad" x1="0" x2="1">
            <stop offset="0" stopColor="var(--m-blue)" />
            <stop offset="1" stopColor="var(--m-purple)" />
          </linearGradient>
          <linearGradient id="pipe-grad-2" x1="1" x2="0">
            <stop offset="0" stopColor="var(--m-pink)" />
            <stop offset="1" stopColor="var(--m-purple)" />
          </linearGradient>
          <filter id="soft" x="-20%" y="-20%" width="140%" height="140%">
            <feDropShadow dx="0" dy="6" stdDeviation="8" floodColor="#000" floodOpacity="0.08" />
          </filter>
        </defs>

        {/* Clavier */}
        <g filter="url(#soft)">
          <rect x="20" y="40" width="120" height="70" rx="12" {...box} />
        </g>
        <g fill="rgba(0,0,0,0.1)">
          {[0, 1, 2, 3, 4].map((i) => (
            <rect key={i} x={34 + i * 19} y="56" width="14" height="12" rx="3" />
          ))}
          {[0, 1, 2, 3].map((i) => (
            <rect key={i} x={44 + i * 19} y="72" width="14" height="12" rx="3" />
          ))}
          <rect x="52" y="88" width="56" height="10" rx="3" fill="var(--accent)" />
        </g>
        <text x="80" y="128" textAnchor="middle" fontSize="11" fill="var(--text-2)" fontFamily={sans}>
          vous tapez
        </text>

        {/* Frontends */}
        <g filter="url(#soft)">
          <rect x="200" y="20" width="150" height="46" rx="10" {...box} />
          <rect x="200" y="84" width="150" height="46" rx="10" {...box} />
        </g>
        <Label x={275} y={43} title="Windows · TSF" sub="predict-tsf.dll" c="var(--m-blue)" />
        <Label x={275} y={107} title="Linux · fcitx5" sub="engine" c="var(--m-teal)" />

        {/* Core partagé */}
        <g filter="url(#soft)">
          <rect x="200" y="160" width="150" height="56" rx="10" fill="var(--text)" />
        </g>
        <text x="275" y="183" textAnchor="middle" fontSize="12" fontWeight="600" fill="#fff" fontFamily={sans}>
          core/ partagé
        </text>
        <text x="275" y="201" textAnchor="middle" fontSize="9" fill="rgba(255,255,255,0.6)" fontFamily={mono}>
          état · frappe · candidats
        </text>

        {/* Daemon */}
        <g filter="url(#soft)">
          <rect x="480" y="140" width="140" height="96" rx="12" {...box} />
        </g>
        <circle cx="502" cy="164" r="9" fill="var(--accent-soft)" stroke="var(--accent)" />
        <circle cx="502" cy="164" r="3" fill="var(--accent)" />
        <text x="518" y="168" fontSize="12" fontWeight="600" fill="var(--text)" fontFamily={sans}>predictord</text>
        <text x="494" y="192" fontSize="10" fill="var(--text-2)" fontFamily={sans}>n-gramme · Lefff</text>
        <text x="494" y="207" fontSize="10" fill="var(--text-2)" fontFamily={sans}>mots appris · récence</text>
        <text x="494" y="222" fontSize="10" fill="var(--text-2)" fontFamily={sans}>100 % local</text>

        {/* Barre de candidats */}
        <g filter="url(#soft)">
          <rect x="115" y="270" width="320" height="44" rx="12" {...box} />
        </g>
        {["vous", "vais", "veux", "voudrais", "voulais"].map((w, i) => (
          <g key={w}>
            <rect
              x={125 + i * 62}
              y="280"
              width="56"
              height="24"
              rx="7"
              fill={i === 0 ? "var(--accent-soft)" : "transparent"}
              stroke={i === 0 ? "var(--accent)" : "transparent"}
              strokeWidth="1.25"
            />
            <text
              x={153 + i * 62}
              y="296"
              textAnchor="middle"
              fontSize="11"
              fontWeight={i === 0 ? 600 : 500}
              fill={i === 0 ? "var(--accent)" : "var(--text)"}
              fontFamily={sans}
            >
              {w}
            </text>
          </g>
        ))}
        <text x="275" y="338" textAnchor="middle" fontSize="11" fill="var(--text-2)" fontFamily={sans}>
          la barre, dans toutes vos applications
        </text>

        {/* Flux (tracés statiques gris + flux animés colorés) */}
        <g stroke="rgba(0,0,0,0.12)" strokeWidth="1.5">
          <path d="M140 60 C170 60 170 43 200 43" />
          <path d="M140 90 C170 90 170 107 200 107" />
          <path d="M275 66 V84" />
          <path d="M275 130 V160" />
          <path d="M275 216 V270" />
        </g>
        <g strokeWidth="2" strokeLinecap="round" className="draw">
          <path d="M140 60 C170 60 170 43 200 43" stroke="var(--m-blue)" style={{ "--len": 70 } as CSSProperties} />
          <path d="M140 90 C170 90 170 107 200 107" stroke="var(--m-teal)" style={{ "--len": 70, "--d": ".2s" } as CSSProperties} />
          <path d="M275 66 V84" stroke="var(--m-blue)" style={{ "--len": 18, "--d": ".5s" } as CSSProperties} />
          <path d="M275 130 V160" stroke="url(#pipe-grad)" style={{ "--len": 30, "--d": ".7s" } as CSSProperties} />
        </g>
        <g strokeWidth="2" strokeLinecap="round" className="anim-flow">
          <path d="M350 178 H480" stroke="var(--m-purple)" />
        </g>
        <g strokeWidth="2" strokeLinecap="round" className="anim-flow" style={{ animationDirection: "reverse" }}>
          <path d="M480 198 H350" stroke="var(--m-pink)" />
        </g>
        <g strokeWidth="2" strokeLinecap="round" className="anim-flow">
          <path d="M275 216 V270" stroke="url(#pipe-grad-2)" />
        </g>
        <text x="415" y="170" textAnchor="middle" fontSize="9" fill="var(--m-purple)" fontFamily={mono}>
          {'{"context","prefix"}'}
        </text>
        <text x="415" y="212" textAnchor="middle" fontSize="9" fill="var(--m-pink)" fontFamily={mono}>
          {'{"candidates":[…]}'}
        </text>
      </svg>
    </div>
  );
}

function Label({ x, y, title, sub, c }: { x: number; y: number; title: string; sub: string; c: string }) {
  return (
    <g>
      <circle cx={x - 58} cy={y} r="4" fill={c} />
      <text x={x - 46} y={y - 2} fontSize="12" fontWeight="600" fill="var(--text)" fontFamily="var(--font-sans)">
        {title}
      </text>
      <text x={x - 46} y={y + 11} fontSize="9.5" fill="var(--text-2)" fontFamily="var(--font-mono)">
        {sub}
      </text>
    </g>
  );
}
