import Features from "@/components/Features";
import Footer from "@/components/Footer";
import Hero from "@/components/Hero";
import HowItWorks from "@/components/HowItWorks";
import Install from "@/components/Install";
import Nav from "@/components/Nav";
import Privacy from "@/components/Privacy";
import VideoSection from "@/components/VideoSection";
import { getWindowsRelease } from "@/lib/release";

export default async function Home() {
  const release = await getWindowsRelease();
  return (
    <>
      <Nav />
      <main className="flex-1">
        <Hero release={release} />
        <VideoSection />
        <Features />
        <HowItWorks />
        <Privacy />
        <Install release={release} />
      </main>
      <Footer />
    </>
  );
}
