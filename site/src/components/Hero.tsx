import HeroDemo from "./HeroDemo";
import Reveal from "./Reveal";

export default function Hero() {
  return (
    <section id="top" className="relative overflow-hidden">
      {/* Fond Mica + blobs de la vidéo */}
      <div
        aria-hidden
        className="absolute inset-0 -z-20"
        style={{
          background:
            "linear-gradient(160deg, var(--mica-2) 0%, var(--mica) 50%, var(--mica-3) 100%)",
        }}
      />
      <div aria-hidden className="absolute inset-0 -z-10 overflow-hidden">
        <div className="blob anim-float-a" style={{ width: 700, height: 700, left: -260, top: -300, background: "var(--blob-a)" }} />
        <div className="blob anim-float-b" style={{ width: 640, height: 640, right: -240, top: -180, background: "var(--blob-b)" }} />
        <div className="blob anim-float-c" style={{ width: 560, height: 560, left: "45%", bottom: -380, background: "var(--blob-c)" }} />
        <div className="bg-dots absolute inset-0 opacity-60" />
        <div className="bg-grid absolute inset-0" />
        <OrbitalShapes />
      </div>

      <div className="mx-auto max-w-6xl px-5 pt-20 pb-16 sm:pt-28 sm:pb-24">
        <div className="mx-auto max-w-3xl text-center">
          <Reveal>
            <span className="chip">
              <span className="relative flex h-2 w-2">
                <span className="anim-pulse-ring absolute inline-flex h-full w-full rounded-full bg-m-green" />
                <span className="relative inline-flex h-2 w-2 rounded-full bg-m-green" />
              </span>
              <span>Saisie prédictive · Français / English</span><span className="hidden sm:inline">· Windows &amp; Linux</span>
            </span>
          </Reveal>
          <Reveal delay={80}>
            <h1 className="mt-6 text-[44px] font-semibold leading-[1.02] tracking-[-0.045em] sm:text-[72px] md:text-[84px]">
              Écrivez plus vite.
              <br />
              <span className="grad-text-anim">Sans rien envoyer.</span>
            </h1>
          </Reveal>
          <Reveal delay={160}>
            <p className="mx-auto mt-6 max-w-xl text-[17px] leading-relaxed text-ink-2 sm:text-[19px]">
              Predict complète vos mots, propose le suivant, corrige et accorde
              pendant que vous tapez — dans toutes vos applications. Un modèle
              local, zéro télémétrie.
            </p>
          </Reveal>
          <Reveal delay={240}>
            <div className="mt-8 flex flex-wrap items-center justify-center gap-3">
              <a href="#install" className="btn-primary">
                <WindowsIcon />
                Télécharger pour Windows
              </a>
              <a href="#install" className="btn-secondary">
                <LinuxIcon />
                Installer sur Linux
              </a>
            </div>
          </Reveal>
          <Reveal delay={320}>
            <p className="mt-5 text-[13px] text-ink-3">
              Open source · MIT · Windows 10 1803+ / 11 · fcitx5
            </p>
          </Reveal>
        </div>

        <Reveal delay={380} className="mt-14 sm:mt-20">
          <HeroDemo />
        </Reveal>
      </div>
    </section>
  );
}

/** Formes Material en orbite, comme les « shapes » de la vidéo. */
function OrbitalShapes() {
  return (
    <svg
      className="absolute left-1/2 top-[38%] hidden h-[1100px] w-[1100px] -translate-x-1/2 -translate-y-1/2 opacity-70 md:block"
      viewBox="0 0 1000 1000"
      fill="none"
      aria-hidden
    >
      <defs>
        <linearGradient id="ring" x1="0" x2="1">
          <stop offset="0" stopColor="var(--m-blue)" stopOpacity="0.35" />
          <stop offset="0.5" stopColor="var(--m-purple)" stopOpacity="0.15" />
          <stop offset="1" stopColor="var(--m-pink)" stopOpacity="0.35" />
        </linearGradient>
      </defs>
      <g className="anim-spin-slow">
        <circle cx="500" cy="500" r="420" stroke="url(#ring)" strokeWidth="1" strokeDasharray="4 10" />
        <circle cx="920" cy="500" r="9" fill="var(--m-blue-l)" />
        <rect x="70" y="486" width="24" height="24" rx="7" fill="var(--m-amber-l)" transform="rotate(20 82 498)" />
      </g>
      <g className="anim-spin-rev">
        <circle cx="500" cy="500" r="300" stroke="url(#ring)" strokeWidth="1" />
        <circle cx="500" cy="200" r="7" fill="var(--m-pink-l)" />
        <path d="M500 786 l14 24 h-28 z" fill="var(--m-teal-l)" />
      </g>
      <g className="anim-spin-slow" style={{ animationDuration: "90s" }}>
        <circle cx="500" cy="500" r="490" stroke="rgba(0,0,0,0.06)" strokeWidth="1" />
        <circle cx="10" cy="500" r="6" fill="var(--m-purple-l)" />
      </g>
    </svg>
  );
}

export function WindowsIcon({ className = "h-4 w-4" }: { className?: string }) {
  return (
    <svg viewBox="0 0 24 24" fill="currentColor" className={className} aria-hidden>
      <path d="M3 4.5 11 3.4v8.1H3zm9 -1.3L21 2v9.5h-9zM3 12.5h8v8.1L3 19.5zm9 0h9V22l-9-1.2z" />
    </svg>
  );
}

export function LinuxIcon({ className = "h-4 w-4" }: { className?: string }) {
  return (
    <svg viewBox="0 0 24 24" fill="none" stroke="currentColor" strokeWidth="1.8" strokeLinecap="round" strokeLinejoin="round" className={className} aria-hidden>
      <path d="M4 17l6-6-6-6" />
      <path d="M12 19h8" />
    </svg>
  );
}
