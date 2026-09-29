# Site vitrine de Predict

Landing page Next.js (App Router, Tailwind v4) pour Predict, style Vercel,
palette reprise de la vidéo `video/index.html` (Windows 11 clair + accents Material).

```sh
npm install
npm run dev      # http://localhost:3000
npm run build && npm run start
```

- `public/predict-motion.mp4` : rendu de `video/renders/predict-motion.mp4`
  ré-encodé pour le web (1600 px, H.264 CRF 26, ~2,5 Mo). Poster : `public/predict-poster.jpg`.
  Pour le régénérer après un nouveau rendu :

  ```sh
  ffmpeg -i ../video/renders/predict-motion.mp4 -vf scale=1600:-2 -c:v libx264 -preset slow -crf 26 \
    -pix_fmt yuv420p -movflags +faststart -c:a aac -b:a 96k public/predict-motion.mp4
  ffmpeg -ss 1.8 -i ../video/renders/predict-motion.mp4 -frames:v 1 -vf scale=1600:-2 -q:v 3 public/predict-poster.jpg
  ```

- `src/components/` : une section par fichier (`Hero`, `VideoSection`, `Features`,
  `HowItWorks`, `Privacy`, `Install`, `Footer`). `HeroDemo` rejoue le scénario de la
  vidéo en React pur ; `Reveal` déclenche les animations SVG (tracé `stroke-dashoffset`)
  à l'entrée dans le viewport. Tout respecte `prefers-reduced-motion`.
