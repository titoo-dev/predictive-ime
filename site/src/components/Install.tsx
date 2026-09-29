import type { WindowsRelease } from "@/lib/release";
import DownloadButton from "./DownloadButton";
import { LinuxIcon, WindowsIcon } from "./Hero";
import Reveal from "./Reveal";

export default function Install({ release }: { release: WindowsRelease }) {
  return (
    <section id="install" className="scroll-mt-20 border-t hairline py-20 sm:py-28">
      <div className="mx-auto max-w-6xl px-5">
        <Reveal className="mx-auto max-w-2xl text-center">
          <p className="text-[12px] font-semibold uppercase tracking-[0.3em] text-ink-2">
            Installer
          </p>
          <h2 className="mt-3 text-[34px] font-semibold leading-tight tracking-[-0.04em] sm:text-[48px]">
            Prêt en <span className="grad-text">deux minutes</span>
          </h2>
          <p className="mt-4 text-[16px] text-ink-2 sm:text-[17px]">
            Un installeur sur Windows, quatre commandes sur Linux. Le modèle est
            livré dans l'installeur (téléchargé une fois sur Linux), puis tout se
            passe hors ligne.
          </p>
        </Reveal>

        <div className="mt-12 grid gap-5 lg:grid-cols-2">
          <Reveal className="min-w-0">
            <Platform
              icon={<WindowsIcon className="h-5 w-5" />}
              title="Windows 10 1803+ / 11"
              sub="Service texte natif (TSF) + predictord"
              color="var(--m-blue)"
            >
              <ol className="mt-4 space-y-3 text-[14.5px] text-ink-2">
                <Step n={1}>
                  Téléchargez et lancez{" "}
                  <code>{release.fileName ?? "predictive-ime-x64.exe"}</code>. Il
                  enregistre le service, installe le modèle (livré dedans, rien à
                  télécharger) et la tâche de démarrage, puis ouvre le panneau
                  d&apos;administration.
                </Step>
                <Step n={2}>
                  Appuyez sur <kbd className="key">Win</kbd>{" "}
                  <kbd className="key">Espace</kbd> et choisissez{" "}
                  <strong className="text-ink">Predict</strong>.
                </Step>
                <Step n={3}>
                  Tapez. <kbd className="key">Win</kbd> <kbd className="key">;</kbd>{" "}
                  ouvre les emojis, l&apos;icône de la barre des tâches met en pause.
                </Step>
              </ol>
              <DownloadButton release={release} className="mt-6 !items-start" />
              <p className="mt-4 text-[12.5px] text-ink-3">Ou depuis les sources :</p>
              <pre className="code mt-2">
                <span className="c"># Build Tools + zstd, puis :</span>
                {"\n"}
                <span className="k">.\scripts\build-windows.ps1</span>
                {"\n"}
                <span className="k">.\scripts\setup-windows.ps1</span>
                {"\n"}
                <span className="k">Start-ScheduledTask</span> -TaskName ime-predictord
              </pre>
            </Platform>
          </Reveal>

          <Reveal delay={100} className="min-w-0">
            <Platform
              icon={<LinuxIcon className="h-5 w-5" />}
              title="Linux · fcitx5"
              sub="Arch, Fedora, Debian/Ubuntu, openSUSE"
              color="var(--m-teal)"
            >
              <pre className="code mt-4">
                <span className="c"># 1. build & install</span>
                {"\n"}
                <span className="k">cmake</span> -B build -DBUILD_UI=OFF
                {"\n"}
                <span className="k">cmake</span> --build build -j
                {"\n"}
                <span className="k">sudo cmake</span> --install build
                {"\n\n"}
                <span className="c"># 2. le modèle</span>
                {"\n"}
                <span className="k">sudo mkdir</span> -p /usr/share/ime-predictord
                {"\n"}
                <span className="k">curl</span> -fsSL <span className="s">$MODEL_URL</span> | zstd -d \{"\n"}
                {"  "}| <span className="k">sudo tar</span> -C /usr/share/ime-predictord -xf -
                {"\n\n"}
                <span className="c"># 3. le démon</span>
                {"\n"}
                <span className="k">systemctl</span> --user enable --now ime-predictord.service
              </pre>
              <p className="mt-4 text-[14.5px] text-ink-2">
                Ajoutez <strong className="text-ink">Predict</strong> dans{" "}
                <code>fcitx5-configtool</code>, redémarrez fcitx5. Le sélecteur
                d&apos;emojis répond à <kbd className="key">Super</kbd>{" "}
                <kbd className="key">;</kbd> même quand une autre méthode est
                active.
              </p>
            </Platform>
          </Reveal>
        </div>
      </div>
    </section>
  );
}

function Platform({
  icon,
  title,
  sub,
  color,
  children,
}: {
  icon: React.ReactNode;
  title: string;
  sub: string;
  color: string;
  children: React.ReactNode;
}) {
  return (
    <div className="card h-full min-w-0 p-6 sm:p-8 [&_code]:rounded-md [&_code]:bg-[#f3f3f3] [&_code]:px-1.5 [&_code]:py-0.5 [&_code]:font-mono [&_code]:text-[13px] [&_code]:text-ink [&_pre_code]:bg-transparent">
      <div className="flex items-center gap-3">
        <span
          className="grid h-11 w-11 place-items-center rounded-xl text-white"
          style={{ background: color, boxShadow: `0 8px 20px color-mix(in srgb, ${color} 35%, transparent)` }}
        >
          {icon}
        </span>
        <div>
          <h3 className="text-[18px] font-semibold tracking-[-0.02em]">{title}</h3>
          <p className="text-[13px] text-ink-2">{sub}</p>
        </div>
      </div>
      {children}
    </div>
  );
}

function Step({ n, children }: { n: number; children: React.ReactNode }) {
  return (
    <li className="flex gap-3">
      <span className="mt-0.5 grid h-6 w-6 shrink-0 place-items-center rounded-full bg-ink text-[12px] font-semibold text-white">
        {n}
      </span>
      <span>{children}</span>
    </li>
  );
}

