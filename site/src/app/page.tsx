import Features from "@/components/Features";
import Footer from "@/components/Footer";
import Hero from "@/components/Hero";
import HowItWorks from "@/components/HowItWorks";
import Install from "@/components/Install";
import Nav from "@/components/Nav";
import Privacy from "@/components/Privacy";
import VideoSection from "@/components/VideoSection";

export default function Home() {
  return (
    <>
      <Nav />
      <main className="flex-1">
        <Hero />
        <VideoSection />
        <Features />
        <HowItWorks />
        <Privacy />
        <Install />
      </main>
      <Footer />
    </>
  );
}
