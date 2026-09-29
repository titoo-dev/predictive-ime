"use client";

import { useEffect, useState } from "react";

type Mode = "idle" | "next" | "complete" | "emoji";

type Frame = {
  committed: string;
  preedit: string;
  mode: Mode;
  candidates: string[];
  emojiQuery?: string;
  emojis?: string[];
};

const EMOJIS = ["❤️", "💙", "💜", "💚", "🧡", "💛", "🩷", "💗", "💖", "💘", "💝", "🫀"];

const sleep = (ms: number) => new Promise((r) => setTimeout(r, ms));

/**
 * Rejoue le scénario de la vidéo : mot suivant, complétion, emoji.
 * Pur état React, aucune dépendance.
 */
export default function HeroDemo() {
  const [f, setF] = useState<Frame>({
    committed: "",
    preedit: "",
    mode: "idle",
    candidates: [],
  });
  const [pressed, setPressed] = useState<string | null>(null);

  useEffect(() => {
    let alive = true;
    const set = (patch: Partial<Frame>) =>
      alive && setF((p) => ({ ...p, ...patch }));

    const type = async (word: string, committed: string) => {
      for (let i = 1; i <= word.length; i++) {
        if (!alive) return;
        set({ committed, preedit: word.slice(0, i), mode: "idle", candidates: [] });
        await sleep(70 + Math.random() * 60);
      }
    };

    const run = async () => {
      while (alive) {
        let c = "";
        const steps: Array<[string, string[]]> = [
          ["je", ["vais", "suis", "pense", "ne", "peux"]],
          ["vais", ["au", "à", "faire", "bien", "vous"]],
          ["au", ["travail", "bureau", "moins", "restaurant", "marché"]],
          ["travail", ["aujourd'hui", "demain", "ce", "et", "à"]],
        ];
        set({ committed: "", preedit: "", mode: "idle", candidates: [] });
        await sleep(600);
        for (const [w, next] of steps) {
          await type(w, c);
          c += w + " ";
          set({ committed: c, preedit: "", mode: "next", candidates: next });
          await sleep(900);
        }
        // Complétion intra-mot
        await type("aujourd'", c);
        set({ mode: "complete", candidates: ["aujourd'hui"] });
        await sleep(1100);
        if (alive) setPressed("Espace");
        await sleep(160);
        if (alive) setPressed(null);
        c += "aujourd'hui ";
        set({ committed: c, preedit: "", mode: "next", candidates: ["et", "!", ".", "pour", "à"] });
        await sleep(900);
        // Emoji
        if (alive) setPressed("Win + ;");
        await sleep(200);
        if (alive) setPressed(null);
        set({ mode: "emoji", candidates: [], emojiQuery: "", emojis: EMOJIS });
        await sleep(500);
        const q = "coeur";
        for (let i = 1; i <= q.length; i++) {
          if (!alive) return;
          set({ emojiQuery: q.slice(0, i) });
          await sleep(110);
        }
        await sleep(800);
        if (alive) setPressed("Entrée");
        await sleep(160);
        if (alive) setPressed(null);
        c += "❤️";
        set({ committed: c, mode: "idle", emojiQuery: undefined, emojis: undefined });
        await sleep(2600);
      }
    };
    run();
    return () => {
      alive = false;
    };
  }, []);

  return (
    <div className="relative mx-auto w-full max-w-[720px]">
      {/* Fenêtre façon Windows 11 */}
      <div
        className="overflow-hidden rounded-2xl border hairline bg-white"
        style={{ boxShadow: "0 30px 80px rgba(0,0,0,0.14), 0 2px 6px rgba(0,0,0,0.06)" }}
      >
        <div className="flex h-10 items-center justify-between border-b hairline px-4 text-[12px] text-ink-2">
          <span className="flex items-center gap-2">
            <span className="inline-block h-3.5 w-3.5 rounded-[4px] bg-[#005fb8]" />
            Sans titre — Bloc-notes
          </span>
          <span className="flex items-center gap-4 text-ink-3">
            <span>—</span>
            <span>▢</span>
            <span>✕</span>
          </span>
        </div>
        <div className="relative min-h-[220px] px-6 pt-6 pb-24 font-sans text-[20px] leading-relaxed sm:text-[22px]">
          <span>{f.committed}</span>
          <span
            className="border-b-2"
            style={{ borderColor: f.preedit ? "var(--accent)" : "transparent" }}
          >
            {f.preedit}
          </span>
          <span className="anim-caret ml-px inline-block h-[1.15em] w-[2px] translate-y-[4px] bg-ink" />

          {/* Barre de candidats */}
          {(f.mode === "next" || f.mode === "complete") && f.candidates.length > 0 && (
            <div
              className="absolute left-6 mt-3 flex items-center gap-1.5 rounded-xl border hairline bg-[rgba(252,252,252,0.96)] p-1.5 text-[14px] backdrop-blur"
              style={{ boxShadow: "0 12px 30px rgba(0,0,0,0.12)" }}
            >
              {f.candidates.map((w, i) => (
                <span
                  key={w + i}
                  className="rounded-lg px-2.5 py-1 font-medium"
                  style={
                    i === 0
                      ? {
                          background: "var(--accent-soft)",
                          color: "var(--accent)",
                          outline: "1.5px solid var(--accent)",
                          outlineOffset: -1.5,
                        }
                      : { color: "var(--text)" }
                  }
                >
                  {w}
                </span>
              ))}
            </div>
          )}

          {/* Sélecteur d'emojis */}
          {f.mode === "emoji" && (
            <div
              className="absolute left-6 mt-3 w-[300px] rounded-2xl border hairline bg-[rgba(252,252,252,0.98)] p-2.5 backdrop-blur"
              style={{ boxShadow: "0 16px 40px rgba(0,0,0,0.14)" }}
            >
              <div className="mb-2 flex items-center gap-2 rounded-lg bg-[#f3f3f3] px-3 py-1.5 text-[14px]">
                <SearchIcon />
                <span className="flex-1">
                  {f.emojiQuery}
                  <span className="anim-caret inline-block h-[1em] w-[1.5px] translate-y-[2px] bg-ink" />
                </span>
                <span className="text-[11px] text-ink-3">1/1</span>
              </div>
              <div className="grid grid-cols-6 gap-1 text-[22px]">
                {(f.emojis ?? []).map((e, i) => (
                  <span
                    key={i}
                    className="grid aspect-square place-items-center rounded-lg"
                    style={
                      i === 0
                        ? { background: "var(--accent-soft)", outline: "1.5px solid var(--accent)", outlineOffset: -1.5 }
                        : undefined
                    }
                  >
                    {e}
                  </span>
                ))}
              </div>
            </div>
          )}
        </div>
      </div>

      {/* Touche pressée */}
      <div className="pointer-events-none absolute -bottom-5 right-6 h-8">
        <span
          className="key !h-8 !min-w-[70px] !text-[12px] transition"
          style={{
            opacity: pressed ? 1 : 0,
            transform: pressed ? "translateY(0)" : "translateY(6px)",
          }}
        >
          {pressed ?? "Espace"}
        </span>
      </div>
    </div>
  );
}

function SearchIcon() {
  return (
    <svg viewBox="0 0 24 24" className="h-4 w-4 text-ink-3" fill="none" stroke="currentColor" strokeWidth="2" strokeLinecap="round" aria-hidden>
      <circle cx="11" cy="11" r="7" />
      <path d="m20 20-3.5-3.5" />
    </svg>
  );
}
