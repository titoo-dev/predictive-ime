import { Logo } from "./Logo";
import { GithubIcon } from "./Nav";

const GITHUB = "https://github.com/titoo-dev/predictive-ime";

export default function Footer() {
  return (
    <footer className="border-t hairline">
      {/* Bande finale : rappel de l'outro de la vidéo */}
      <div className="relative overflow-hidden">
        <div aria-hidden className="absolute inset-0 -z-10">
          <div className="blob anim-float-a" style={{ width: 500, height: 500, left: "20%", top: -300, background: "var(--blob-b)", opacity: 0.3 }} />
          <div className="blob anim-float-c" style={{ width: 500, height: 500, right: "15%", bottom: -300, background: "var(--blob-a)", opacity: 0.3 }} />
        </div>
        <div className="mx-auto max-w-6xl px-5 py-20 text-center sm:py-28">
          <div className="inline-flex scale-125 sm:scale-150">
            <Logo size={40} />
          </div>
          <p className="mt-10 text-[28px] font-semibold tracking-[-0.04em] sm:text-[40px]">
            Écrivez plus vite. <span className="grad-text">Sans rien envoyer.</span>
          </p>
          <div className="mt-8 flex flex-wrap items-center justify-center gap-3">
            <a href="#install" className="btn-primary">
              Installer Predict
            </a>
            <a href={GITHUB} target="_blank" rel="noreferrer" className="btn-secondary">
              <GithubIcon />
              Voir le code
            </a>
          </div>
        </div>
      </div>

      <div className="border-t hairline">
        <div className="mx-auto flex max-w-6xl flex-col items-center justify-between gap-4 px-5 py-8 text-[13px] text-ink-2 sm:flex-row">
          <p>
            Code sous licence MIT · Modèle CC BY-SA 4.0, dérivé de corpus ouverts.
          </p>
          <ul className="flex items-center gap-5">
            <li>
              <a className="transition hover:text-ink" href={`${GITHUB}#readme`} target="_blank" rel="noreferrer">
                Documentation
              </a>
            </li>
            <li>
              <a className="transition hover:text-ink" href={`${GITHUB}/blob/main/docs/internals.md`} target="_blank" rel="noreferrer">
                Algorithme &amp; benchmarks
              </a>
            </li>
            <li>
              <a className="transition hover:text-ink" href={`${GITHUB}/blob/main/CONTRIBUTING.md`} target="_blank" rel="noreferrer">
                Contribuer
              </a>
            </li>
          </ul>
        </div>
      </div>
    </footer>
  );
}
