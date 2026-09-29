import type { WindowsRelease } from "@/lib/release";
import { WindowsIcon } from "./Hero";

type Props = {
  release: WindowsRelease;
  /** Ligne de détails sous le bouton (version, taille, SHA-256). */
  details?: boolean;
  className?: string;
};

/** Bouton « Télécharger pour Windows » : .exe direct de la dernière release. */
export default function DownloadButton({ release, details = true, className = "" }: Props) {
  const label = release.available ? "Télécharger pour Windows" : "Voir les versions";
  return (
    <div className={`inline-flex flex-col items-center gap-2 ${className}`}>
      <a
        href={release.url}
        {...(release.available ? { download: release.fileName ?? true } : { target: "_blank", rel: "noreferrer" })}
        className="btn-primary group relative"
        aria-label={
          release.available
            ? `Télécharger l'installeur Windows ${release.version ?? ""} (${release.sizeMb ?? "?"} Mo)`
            : "Voir les versions sur GitHub"
        }
      >
        <WindowsIcon />
        {label}
        <DownloadArrow />
      </a>
      {details && (
        <span className="text-[12.5px] text-ink-3">
          {release.available ? (
            <>
              v{release.version} · {release.sizeMb} Mo · Windows 10 1803+ / 11 x64
              {release.sha256Url && (
                <>
                  {" · "}
                  <a href={release.sha256Url} className="underline decoration-dotted underline-offset-2 hover:text-ink">
                    SHA-256
                  </a>
                </>
              )}
            </>
          ) : (
            <>Installeur Windows en cours de publication — les sources et le modèle sont sur GitHub.</>
          )}
        </span>
      )}
    </div>
  );
}

/** Flèche de téléchargement : la flèche « tombe » dans le plateau au survol. */
function DownloadArrow() {
  return (
    <svg viewBox="0 0 24 24" className="h-4 w-4" fill="none" stroke="currentColor" strokeWidth="2" strokeLinecap="round" strokeLinejoin="round" aria-hidden>
      <g className="transition-transform duration-300 group-hover:translate-y-[3px]">
        <path d="M12 4v11" />
        <path d="m7 10 5 5 5-5" />
      </g>
      <path d="M4 19h16" />
    </svg>
  );
}
