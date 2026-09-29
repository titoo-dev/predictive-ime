import type { Metadata } from "next";
import { Geist, Geist_Mono } from "next/font/google";
import "./globals.css";

const geistSans = Geist({
  variable: "--font-geist-sans",
  subsets: ["latin"],
});

const geistMono = Geist_Mono({
  variable: "--font-geist-mono",
  subsets: ["latin"],
});

export const metadata: Metadata = {
  metadataBase: new URL(process.env.NEXT_PUBLIC_SITE_URL ?? "https://predictive-ime.dev"),
  title: "Predict — saisie prédictive français / anglais, hors ligne",
  description:
    "Predict complète vos mots, prédit le suivant, corrige et accorde pendant que vous écrivez. Windows et Linux. Zéro télémétrie : vos mots restent sur votre PC.",
  openGraph: {
    title: "Predict — Écrivez plus vite. Sans rien envoyer.",
    description:
      "Saisie prédictive français / anglais pour Windows (TSF) et Linux (fcitx5). Complétion, mot suivant, correction, accord grammatical, emojis. 100 % hors ligne.",
    images: ["/predict-poster.jpg"],
    locale: "fr_FR",
    type: "website",
  },
};

export default function RootLayout({ children }: LayoutProps<"/">) {
  return (
    <html
      lang="fr"
      suppressHydrationWarning
      className={`${geistSans.variable} ${geistMono.variable} h-full antialiased`}
    >
      <body className="min-h-full flex flex-col">{children}</body>
    </html>
  );
}
