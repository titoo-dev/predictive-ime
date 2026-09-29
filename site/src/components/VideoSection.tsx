"use client";

import { useRef, useState } from "react";
import Reveal from "./Reveal";

export default function VideoSection() {
  const ref = useRef<HTMLVideoElement>(null);
  const [playing, setPlaying] = useState(false);

  const play = () => {
    const v = ref.current;
    if (!v) return;
    v.play();
  };

  return (
    <section id="video" className="relative scroll-mt-20 overflow-hidden py-20 sm:py-28">
      <div className="mx-auto max-w-6xl px-5">
        <Reveal className="mx-auto max-w-2xl text-center">
          <p className="text-[12px] font-semibold uppercase tracking-[0.3em] text-ink-2">
            En 36 secondes
          </p>
          <h2 className="mt-3 text-[34px] font-semibold leading-tight tracking-[-0.04em] sm:text-[48px]">
            Voyez-le <span className="grad-text">pendant que vous écrivez</span>
          </h2>
          <p className="mt-4 text-[16px] text-ink-2 sm:text-[17px]">
            Win+Space, choisissez Predict, tapez. Le reste se passe dans la barre.
          </p>
        </Reveal>

        <Reveal delay={120} className="relative mt-12">
          {/* Halo coloré derrière le lecteur */}
          <div
            aria-hidden
            className="absolute -inset-6 -z-10 rounded-[32px] opacity-60 blur-2xl"
            style={{
              background:
                "linear-gradient(120deg, rgba(25,118,210,0.35), rgba(142,36,170,0.25), rgba(194,24,91,0.3))",
            }}
          />
          <div
            className="relative overflow-hidden rounded-2xl border hairline bg-black"
            style={{ boxShadow: "0 40px 100px rgba(0,0,0,0.18)" }}
          >
            <video
              ref={ref}
              className="block aspect-video w-full"
              poster="/predict-poster.jpg"
              preload="metadata"
              controls={playing}
              playsInline
              onPlay={() => setPlaying(true)}
              onEnded={() => setPlaying(false)}
            >
              <source src="/predict-motion.mp4" type="video/mp4" />
              Votre navigateur ne lit pas les vidéos HTML5.
            </video>
            {!playing && (
              <button
                type="button"
                onClick={play}
                aria-label="Lire la vidéo de présentation"
                className="group absolute inset-0 grid place-items-center bg-[rgba(243,243,243,0.05)] transition hover:bg-[rgba(243,243,243,0.12)]"
              >
                <span className="relative grid h-20 w-20 place-items-center">
                  <span className="anim-pulse-ring absolute inset-0 rounded-full bg-white/70" />
                  <span
                    className="relative grid h-20 w-20 place-items-center rounded-full bg-white text-ink transition group-hover:scale-105"
                    style={{ boxShadow: "0 16px 40px rgba(0,0,0,0.25)" }}
                  >
                    <svg viewBox="0 0 24 24" className="ml-1 h-8 w-8" fill="currentColor" aria-hidden>
                      <path d="M8 5.5v13l11-6.5z" />
                    </svg>
                  </span>
                </span>
              </button>
            )}
          </div>
        </Reveal>

        <Reveal delay={200} className="mt-8 flex flex-wrap items-center justify-center gap-2 text-[13px] text-ink-2">
          <span className="chip"><Dot c="var(--m-blue)" />Mot suivant prédit</span>
          <span className="chip"><Dot c="var(--m-purple)" />Complétion en un geste</span>
          <span className="chip"><Dot c="var(--m-teal)" />Contexte + accord grammatical</span>
          <span className="chip"><Dot c="var(--m-pink)" />Emojis par leur nom</span>
        </Reveal>
      </div>
    </section>
  );
}

function Dot({ c }: { c: string }) {
  return <span className="inline-block h-2 w-2 rounded-full" style={{ background: c }} />;
}
