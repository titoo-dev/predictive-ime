// Dernière release Windows publiée sur GitHub : l'installeur .exe, sa taille,
// sa version et l'empreinte SHA-256 (déposée par scripts/package-windows.ps1).
// Interrogé à la génération de la page et rafraîchi toutes les heures (ISR) ;
// sans release (ou API injoignable), le bouton renvoie vers la page des
// versions plutôt que vers un 404.

export const REPO = "titoo-dev/predictive-ime";
export const RELEASES_URL = `https://github.com/${REPO}/releases`;

export type WindowsRelease = {
  available: boolean;
  url: string; // .exe direct, ou page des releases en repli
  version: string | null; // "0.1.0"
  sizeMb: number | null;
  sha256Url: string | null;
  publishedAt: string | null; // ISO
  fileName: string | null;
};

const FALLBACK: WindowsRelease = {
  available: false,
  url: RELEASES_URL,
  version: null,
  sizeMb: null,
  sha256Url: null,
  publishedAt: null,
  fileName: null,
};

type Asset = { name: string; browser_download_url: string; size: number };
type Release = { tag_name: string; name: string; published_at: string; assets: Asset[] };

export async function getWindowsRelease(): Promise<WindowsRelease> {
  try {
    // Les releases « model-v1 » ne portent que le modèle : on parcourt la
    // liste et on garde la première qui contient un installeur Windows.
    const res = await fetch(`https://api.github.com/repos/${REPO}/releases?per_page=10`, {
      headers: {
        Accept: "application/vnd.github+json",
        ...(process.env.GITHUB_TOKEN ? { Authorization: `Bearer ${process.env.GITHUB_TOKEN}` } : {}),
      },
      next: { revalidate: 3600 },
    });
    if (!res.ok) return FALLBACK;
    const releases = (await res.json()) as Release[];
    for (const r of releases) {
      const exe = r.assets.find((a) => /^predictive-ime-.*-x64\.exe$/i.test(a.name));
      if (!exe) continue;
      const sha = r.assets.find((a) => a.name === `${exe.name}.sha256`);
      const version = exe.name.match(/predictive-ime-(.+)-x64\.exe$/i)?.[1] ?? r.tag_name.replace(/^v/, "");
      return {
        available: true,
        url: exe.browser_download_url,
        version,
        sizeMb: Math.round((exe.size / 1048576) * 10) / 10,
        sha256Url: sha?.browser_download_url ?? null,
        publishedAt: r.published_at,
        fileName: exe.name,
      };
    }
    return FALLBACK;
  } catch {
    return FALLBACK;
  }
}
