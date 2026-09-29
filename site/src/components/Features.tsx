import type { CSSProperties, ReactNode } from "react";
import Reveal from "./Reveal";

type Feature = {
  color: string;
  title: string;
  body: ReactNode;
  icon: ReactNode;
};

const stroke = {
  fill: "none",
  stroke: "currentColor",
  strokeWidth: 1.75,
  strokeLinecap: "round" as const,
  strokeLinejoin: "round" as const,
};

const FEATURES: Feature[] = [
  {
    color: "var(--m-blue)",
    title: "Complétion de mot",
    body: (
      <>
        <code>aujourd&apos;</code> → <code>aujourd&apos;hui</code>. Jusqu&apos;à cinq
        suggestions classées, la meilleure s&apos;applique avec Espace.
      </>
    ),
    icon: (
      <svg viewBox="0 0 48 48" {...stroke}>
        <path d="M6 30h14" style={{ "--len": 20 } as CSSProperties} />
        <path d="M24 30h18" strokeDasharray="3 4" style={{ "--len": 24, "--d": ".3s" } as CSSProperties} />
        <path d="M8 18l6 -8 6 8" style={{ "--len": 26, "--d": ".5s" } as CSSProperties} />
        <path d="M10 15h8" style={{ "--len": 10, "--d": ".8s" } as CSSProperties} />
        <rect x="26" y="8" width="16" height="14" rx="4" style={{ "--len": 60, "--d": ".6s" } as CSSProperties} />
      </svg>
    ),
  },
  {
    color: "var(--m-purple)",
    title: "Mot suivant",
    body: (
      <>
        Dès que vous validez un mot, un n-gramme local propose la suite, avec la
        phrase entière comme contexte.
      </>
    ),
    icon: (
      <svg viewBox="0 0 48 48" {...stroke}>
        <rect x="5" y="16" width="14" height="16" rx="4" style={{ "--len": 60 } as CSSProperties} />
        <path d="M22 24h10" style={{ "--len": 12, "--d": ".4s" } as CSSProperties} />
        <path d="M29 19l5 5-5 5" style={{ "--len": 16, "--d": ".6s" } as CSSProperties} />
        <rect x="36" y="20" width="8" height="8" rx="2" strokeDasharray="2 3" style={{ "--len": 32, "--d": ".8s" } as CSSProperties} />
      </svg>
    ),
  },
  {
    color: "var(--m-teal)",
    title: "Correction automatique",
    body: (
      <>
        <code>dont</code> → <code>don&apos;t</code>, <code>im</code> → <code>I&apos;m</code>,
        accents oubliés restaurés, sans quitter le clavier.
      </>
    ),
    icon: (
      <svg viewBox="0 0 48 48" {...stroke}>
        <circle cx="24" cy="24" r="16" style={{ "--len": 101 } as CSSProperties} />
        <path d="M16 24l6 6 11-12" style={{ "--len": 28, "--d": ".6s" } as CSSProperties} />
      </svg>
    ),
  },
  {
    color: "var(--m-orange)",
    title: "Accord grammatical",
    body: (
      <>
        <code>les petits chat</code> → <code>chats</code>. Le lexique Lefff accorde
        genre et nombre avec le déterminant qui gouverne.
      </>
    ),
    icon: (
      <svg viewBox="0 0 48 48" {...stroke}>
        <rect x="6" y="18" width="18" height="12" rx="6" style={{ "--len": 56 } as CSSProperties} />
        <rect x="24" y="18" width="18" height="12" rx="6" style={{ "--len": 56, "--d": ".3s" } as CSSProperties} />
        <path d="M14 24h20" style={{ "--len": 20, "--d": ".7s" } as CSSProperties} />
        <path d="M30 8l3 4M36 8l3 4" style={{ "--len": 10, "--d": "1s" } as CSSProperties} />
      </svg>
    ),
  },
  {
    color: "var(--m-pink)",
    title: "Il apprend vos mots",
    body: (
      <>
        Prénoms, jargon, tics de langage : appris à la volée et reclassés sur
        votre usage, sur l&apos;échelle du modèle lui-même.
      </>
    ),
    icon: (
      <svg viewBox="0 0 48 48" {...stroke}>
        <path d="M24 6l5.2 10.6 11.7 1.7-8.5 8.2 2 11.6L24 32.6l-10.4 5.5 2-11.6-8.5-8.2 11.7-1.7z" style={{ "--len": 130 } as CSSProperties} />
      </svg>
    ),
  },
  {
    color: "var(--m-green)",
    title: "Emojis par leur nom",
    body: (
      <>
        <code>coeur</code> → ❤️, <code>feu</code> → 🔥. Un raccourci, un mot-clé
        CLDR, Entrée. Les favoris en premier.
      </>
    ),
    icon: (
      <svg viewBox="0 0 48 48" {...stroke}>
        <circle cx="24" cy="24" r="16" style={{ "--len": 101 } as CSSProperties} />
        <path d="M16 27c2 4 5 6 8 6s6-2 8-6" style={{ "--len": 20, "--d": ".6s" } as CSSProperties} />
        <path d="M18 19h.01M30 19h.01" strokeWidth="3" style={{ "--len": 4, "--d": ".9s" } as CSSProperties} />
      </svg>
    ),
  },
  {
    color: "var(--m-indigo)",
    title: "Français, English ou auto",
    body: (
      <>
        <kbd className="key">Ctrl</kbd> <kbd className="key">Maj</kbd>{" "}
        <kbd className="key">L</kbd> bascule la langue dans la barre. Les
        contractions anglaises sont du vocabulaire de première classe.
      </>
    ),
    icon: (
      <svg viewBox="0 0 48 48" {...stroke}>
        <circle cx="24" cy="24" r="16" style={{ "--len": 101 } as CSSProperties} />
        <path d="M8 24h32" style={{ "--len": 32, "--d": ".4s" } as CSSProperties} />
        <path d="M24 8c-5 5-5 27 0 32M24 8c5 5 5 27 0 32" style={{ "--len": 70, "--d": ".6s" } as CSSProperties} />
      </svg>
    ),
  },
  {
    color: "var(--m-amber)",
    title: "Mémoire du document",
    body: (
      <>
        Les mots déjà présents avant le curseur remontent : un texte se répète,
        noms propres et vocabulaire du sujet reviennent en tête.
      </>
    ),
    icon: (
      <svg viewBox="0 0 48 48" {...stroke}>
        <path d="M12 6h16l8 8v28H12z" style={{ "--len": 100 } as CSSProperties} />
        <path d="M28 6v8h8" style={{ "--len": 16, "--d": ".5s" } as CSSProperties} />
        <path d="M17 22h14M17 28h14M17 34h8" style={{ "--len": 36, "--d": ".7s" } as CSSProperties} />
      </svg>
    ),
  },
  {
    color: "var(--m-teal-l)",
    title: "Typographie française",
    body: (
      <>
        Espace fine insécable avant <code>; : ! ?</code> et dans les guillemets
        « », majuscule automatique en début de phrase. En option.
      </>
    ),
    icon: (
      <svg viewBox="0 0 48 48" {...stroke}>
        <path d="M8 12h32" style={{ "--len": 32 } as CSSProperties} />
        <path d="M24 12v26" style={{ "--len": 26, "--d": ".3s" } as CSSProperties} />
        <path d="M16 38h16" style={{ "--len": 16, "--d": ".6s" } as CSSProperties} />
        <path d="M34 30l-3 6M40 30l-3 6" style={{ "--len": 14, "--d": ".9s" } as CSSProperties} />
      </svg>
    ),
  },
];

export default function Features() {
  return (
    <section id="features" className="relative scroll-mt-20 border-t hairline py-20 sm:py-28">
      <div className="mx-auto max-w-6xl px-5">
        <Reveal className="max-w-2xl">
          <p className="text-[12px] font-semibold uppercase tracking-[0.3em] text-ink-2">
            Fonctionnalités
          </p>
          <h2 className="mt-3 text-[34px] font-semibold leading-tight tracking-[-0.04em] sm:text-[48px]">
            Tout ce qu&apos;il fait{" "}
            <span className="grad-text">pendant que vous écrivez</span>
          </h2>
          <p className="mt-4 text-[16px] text-ink-2 sm:text-[17px]">
            Une barre de cinq candidats, jamais plus. Chaque suggestion est
            classée par un modèle n-gramme local, éclairée par le contexte de la
            phrase et par vos propres habitudes.
          </p>
        </Reveal>

        <ul className="mt-12 grid gap-4 sm:grid-cols-2 lg:grid-cols-3">
          {FEATURES.map((f, i) => (
            <Reveal as="li" key={f.title} delay={i * 60} className="h-full">
              <article
                className="card group flex h-full flex-col p-6"
                style={{ "--c": f.color } as CSSProperties}
              >
                <div
                  className="draw grid h-12 w-12 place-items-center rounded-xl [&>svg]:h-7 [&>svg]:w-7"
                  style={{
                    color: f.color,
                    background: `color-mix(in srgb, ${f.color} 10%, white)`,
                    border: `1px solid color-mix(in srgb, ${f.color} 25%, transparent)`,
                  }}
                >
                  {f.icon}
                </div>
                <h3 className="mt-5 text-[17px] font-semibold tracking-[-0.02em]">{f.title}</h3>
                <p className="mt-2 text-[14.5px] leading-relaxed text-ink-2 [&_code]:rounded-md [&_code]:bg-[#f3f3f3] [&_code]:px-1.5 [&_code]:py-0.5 [&_code]:font-mono [&_code]:text-[13px] [&_code]:text-ink">
                  {f.body}
                </p>
              </article>
            </Reveal>
          ))}
        </ul>
      </div>
    </section>
  );
}
