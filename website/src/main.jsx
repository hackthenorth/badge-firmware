import React, { useState } from "react";
import { createRoot } from "react-dom/client";
import htnLogo from "../../assets/boot/htn_logo_horizontal.png";
import solanaLogo from "../../assets/boot/solana_logo_mark.svg";
import solanaWordmark from "./assets/solana-logo.svg";
import connectIcon from "../../assets/icons/badgebump.png";
import chessIcon from "../../assets/icons/chess.png";
import scannerSticker from "../../assets/icons/scanner-sticker.webp";
import eightBallIcon from "../../assets/icons/eightball.png";
import flappyIcon from "../../assets/icons/flappy.png";
import tetrisIcon from "../../assets/icons/tetris.png";
import badgeFront from "./assets/badge-front.png";
import badgeBack from "./assets/badge-back.png";
import appsLauncherPhoto from "./assets/apps-launcher.webp";
import partnershipBadgePhoto from "./assets/partnership-badge.webp";
import jazaLogo from "./assets/jaza-logo.svg";
import jazaBatteriesPhoto from "./assets/jaza-batteries.webp";
import processorPhoto from "./assets/spec-processor.png";
import displayPhoto from "./assets/spec-display.png";
import nfcPhoto from "./assets/spec-nfc.png";
import bluetoothPhoto from "./assets/spec-bluetooth.png";
import buttonsPhoto from "./assets/spec-buttons.png";
import ledsPhoto from "./assets/spec-leds.png";
import gettingStartedPhoto from "./assets/getting-started-badge.png";
import CloseupViewer from "./CloseupViewer.jsx";
import "./styles.css";
import SyncPage from "./sync/SyncPage.jsx";

const IDE_PATH = "/ide/";
const MANUAL_EMBED_URL = "https://hackthenorth.notion.site/ebd/3ceb70e03de78004b281e0035daf012b";
const MANUAL_URL = "https://hackthenorth.notion.site/hacker-badge-instruction-manual";
const ANNOUNCEMENT_URL = "https://youtube.com/shorts/_CE9c1ZKdpA?feature=share";
const ANNOUNCEMENT_EMBED_URL = "https://www.youtube-nocookie.com/embed/_CE9c1ZKdpA?autoplay=1&playsinline=1";
const ANNOUNCEMENT_THUMBNAIL_URL = "https://i.ytimg.com/vi/_CE9c1ZKdpA/hq2.jpg";

function shouldEmbedManual() {
  const userAgent = window.navigator.userAgent;
  const isIOS = /iPad|iPhone|iPod/.test(userAgent)
    || (window.navigator.platform === "MacIntel" && window.navigator.maxTouchPoints > 1);
  const isSafari = /Safari/.test(userAgent)
    && !/Chrome|Chromium|CriOS|Edg|OPR|Android/.test(userAgent);

  return !isIOS && !isSafari;
}

function ArrowIcon() {
  return (
    <svg viewBox="0 0 16 16" aria-hidden="true">
      <path d="M3 8h9M8.5 4.5 12 8l-3.5 3.5" />
    </svg>
  );
}

function BrandLockup({ page }) {
  if (page !== "home") {
    return (
      <a className="brand-lockup" href="/" aria-label="Go to the badge homepage">
        <span className="brand brand-htn"><img src={htnLogo} alt="Hack the North" /></span>
        <span className="collaboration-cross" aria-hidden="true" />
        <span className="brand brand-solana"><img src={solanaWordmark} alt="Solana" /></span>
      </a>
    );
  }

  return (
    <div className="brand-lockup" aria-label="Hack the North in collaboration with Solana">
      <a className="brand brand-htn" href="https://hackthenorth.com/" aria-label="Visit Hack the North">
        <img src={htnLogo} alt="Hack the North" />
      </a>
      <span className="collaboration-cross" aria-hidden="true" />
      <a className="brand brand-solana" href="https://solana.com/" aria-label="Visit Solana">
        <img src={solanaWordmark} alt="Solana" />
      </a>
    </div>
  );
}

function Shell({ children, page = "home" }) {
  return (
    <div className={`site-shell${page === "sync" ? " sync-site-shell" : ""}`}>
      <a className="skip-link" href="#content">Skip to content</a>
      <header className="site-header">
        <BrandLockup page={page} />
        <nav aria-label="Main navigation">
          {page === "sync" ? <a className="sync-home" href="/">← Badge home</a> : <>
          <a className={page === "custom-flash" ? "active" : ""} href="/custom-flash">Custom Flash</a>
          <a className={page === "ktnh" ? "active" : ""} href="/ktnh">KTNH</a>
          <a className={page === "rules" ? "active" : ""} href="/rules">Rules</a>
          <a className={`nav-manual${page === "manual" ? " active" : ""}`} href="/manual" aria-current={page === "manual" ? "page" : undefined}>
            <span className="nav-manual-prefix">Instruction </span>Manual
          </a>
          <a className="nav-button" href={IDE_PATH}>Open IDE <ArrowIcon /></a>
          <a className="nav-button nav-sync" href="/sync" aria-current={page === "sync" ? "page" : undefined}>
            <span className="nav-sync-prefix">Sync your badge</span><span className="nav-sync-short">Sync</span> <ArrowIcon />
          </a>
          </>}
        </nav>
      </header>
      {children}
      {page !== "sync" && <footer className="site-footer">
        <BrandLockup page={page} />
        <div className="footer-links">
          <a href="/sync">Sync your badge</a>
          <a href="/custom-flash">Custom Flash</a>
          <a href="/rules">Badge rules</a>
          <a href="/ktnh">KTNH game manual</a>
          <a href="/manual">Instruction manual</a>
          <a href={IDE_PATH}>Badge IDE</a>
        </div>
      </footer>}
    </div>
  );
}

function BadgeDiagram() {
  const [side, setSide] = useState("front");

  return (
    <div className="badge-stage">
      <div className="orbit orbit-one" />
      <div className="orbit orbit-two" />
      <div className="trace trace-a"><i /><i /><i /></div>
      <div className="trace trace-b"><i /><i /></div>
      <div
        className="badge-viewer"
        role="img"
        aria-label={`${side === "front" ? "Front" : "Back"} view of the Hack the North 2026 badge`}
      >
        <img
          className={`badge-product-image${side === "front" ? " is-active" : ""}`}
          src={badgeFront}
          alt=""
          aria-hidden={side !== "front"}
        />
        <img
          className={`badge-product-image${side === "back" ? " is-active" : ""}`}
          src={badgeBack}
          alt=""
          aria-hidden={side !== "back"}
        />
      </div>
      <div className="badge-side-switch" role="group" aria-label="Choose badge side">
        <button
          type="button"
          aria-pressed={side === "front"}
          onClick={() => setSide("front")}
        >
          Front
        </button>
        <button
          type="button"
          aria-pressed={side === "back"}
          onClick={() => setSide("back")}
        >
          Back
        </button>
      </div>
      <a className="badge-closeup-link" href={`/closeup?side=${side}`}>Ultra Closeup <ArrowIcon /></a>
    </div>
  );
}

function AnnouncementPlayer() {
  const [isPlaying, setIsPlaying] = useState(false);

  return (
    <div className="announcement-player">
      {isPlaying ? (
        <iframe
          src={ANNOUNCEMENT_EMBED_URL}
          title="The Hacker Badge announcement"
          allow="accelerometer; autoplay; clipboard-write; encrypted-media; gyroscope; picture-in-picture; web-share"
          referrerPolicy="strict-origin-when-cross-origin"
          allowFullScreen
        />
      ) : (
        <button
          className="announcement-play"
          type="button"
          onClick={() => setIsPlaying(true)}
          aria-label="Play The Hacker Badge announcement"
        >
          <img src={ANNOUNCEMENT_THUMBNAIL_URL} alt="" />
          <span className="announcement-play-icon" aria-hidden="true"><i /></span>
          <span className="announcement-play-label" aria-hidden="true">Play announcement</span>
        </button>
      )}
    </div>
  );
}

function HomePage() {
  const specs = [
    { slug: "processor", value: "ESP32-C3", label: "The brain", detail: "A tiny, capable computer at the heart of every badge.", photo: processorPhoto, alt: "Close-up of the badge's ESP32-C3 processor" },
    { slug: "display", value: "Full-colour display", label: "320 × 240 screen", detail: "A bright screen for apps, games, and moments throughout the event.", photo: displayPhoto, alt: "The badge's colour display showing the app launcher" },
    { slug: "nfc", value: "NFC Reader", label: "Tap ready", detail: "Interact with NFC stickers around the event to unlock surprises.", photo: nfcPhoto, alt: "The floral NFC tap symbol on the badge" },
    { slug: "bluetooth", value: "Bluetooth", label: "Connect with others", detail: "Connect with other attendees throughout the event using Bluetooth.", photo: bluetoothPhoto, alt: "The badge's ESP32 module and antenna for Bluetooth connectivity" },
    { slug: "buttons", value: "8 Buttons", label: "Super tactile", detail: "Super-tactile buttons, fun for playing games or just fidgeting.", photo: buttonsPhoto, alt: "Close-up of the badge's tactile directional buttons" },
    { slug: "leds", value: "6 RGB LEDs", label: "Lights all around", detail: "The badge is surrounded by super-bright LED pixels to bring it to life.", photo: ledsPhoto, alt: "The badge's RGB LEDs glowing blue and pink" },
  ];

  const faqs = [
    {
      question: "Does everyone get one?",
      answer: "Hackers, sponsors, judges, organizers, and volunteer leads receive a Hacker Badge. Volunteers, general attendees, and visitors receive a keepsake PCB badge without the working electronic components. You can also choose a non-digital badge if you prefer.",
    },
    {
      question: "Can I keep it after the event?",
      answer: "Yes. Your badge is yours to keep after the event, and it is personalized just for you.",
    },
    {
      question: "How is it powered?",
      answer: "The badge runs on two AA alkaline batteries, provided by Jaza. Battery life varies by use. Spare batteries are available throughout the weekend at the badge help desk.",
    },
    {
      question: "Is it safe?",
      answer: "The badge was designed with safety in mind, including a lead-free finish on its circuit board. That said, it's still an experimental electronic device and should be handled with care. Keep it dry, use only AA alkaline batteries, and do not wear it while sleeping. If it becomes hot, smells unusual, smokes, appears damaged, or behaves unexpectedly, turn it off and bring it to the badge help desk. See the badge rules for full guidance.",
    },
  ];

  return (
    <Shell>
      <main id="content" className="home-page">
        <section className="hero">
          <div className="hero-copy">
            <p className="eyebrow"><span>HACK THE NORTH 2026</span></p>
            <h1>Meet Your<br />Hacker Badge</h1>
            <p className="lede">
              Meet the Hack the North 2026 Hacker Badge: a fully fledged computer with an ESP32, NFC, a screen, and more. It is yours to keep after the event.
            </p>
            <div className="hero-actions">
              <a className="button button-primary" href="#about">Explore the badge <ArrowIcon /></a>
              <a className="button button-secondary" href="/manual">Instruction manual</a>
            </div>
          </div>
          <BadgeDiagram />
        </section>

        <section className="announcement-section" aria-labelledby="announcement-title">
          <div className="announcement-copy">
            <h2 id="announcement-title">Watch the reveal.</h2>
            <p>
              See the 2026 Hacker Badge come to life, then keep scrolling to discover everything packed inside.
            </p>
            <a className="text-link" href={ANNOUNCEMENT_URL} target="_blank" rel="noreferrer">
              Watch on YouTube <ArrowIcon />
            </a>
          </div>
          <div className="announcement-player-frame">
            <div className="announcement-player-meta" aria-hidden="true">
              <span>HACK THE NORTH // 2026</span>
              <span>THE HACKER BADGE</span>
            </div>
            <AnnouncementPlayer />
          </div>
        </section>

        <section id="about" className="intro-section section-pad">
          <div className="section-kicker">
            <span>01</span>
            <p>WHAT IS THE BADGE?</p>
          </div>
          <div className="intro-copy">
            <h2>A badge made<br /><em>for Hack the North.</em></h2>
            <div className="body-copy">
              <p>
                For the first time ever, every hacker at Hack the North will receive a Hacker Badge: a custom PCB with an ESP32, screen, buttons, lights, NFC, Bluetooth, and plenty more inside.
              </p>
              <p>
                It is your interactive companion and your portal to the event. Play games, discover hidden experiences, meet new people, and unlock more as the weekend unfolds. Best of all, it is yours to keep after the event!
              </p>
            </div>
          </div>
        </section>

        <section id="getting-started" className="steps-section section-pad">
          <div className="section-heading">
            <div className="section-kicker steps-kicker">
              <span>02</span>
              <p>GETTING STARTED</p>
            </div>
            <h2><span>Getting started</span><br /><span>with your badge.</span></h2>
            <div className="steps-photo">
              <img src={gettingStartedPhoto} alt="The Hacker Badge displaying its welcome screen" width="3970" height="2989" loading="lazy" />
            </div>
          </div>
          <ol className="steps-list">
            <li>
              <span className="step-number">01</span>
              <div><h3>Complete your badge information during RSVP</h3><p>During RSVP, you gave us your name and the contact information you want programmed onto your badge.</p></div>
            </li>
            <li>
              <span className="step-number">02</span>
              <div><h3>Receive your badge at registration</h3><p>Pick up your personalized Hacker Badge when you arrive at Hack the North.</p></div>
            </li>
            <li>
              <span className="step-number">03</span>
              <div><h3>Power on and get started</h3><p>Insert the two supplied AA alkaline batteries, matching the + / − markings. Switch the badge on and follow the welcome experience.</p></div>
            </li>
          </ol>
        </section>

        <section id="features" className="features-section section-pad">
          <div className="features-header">
            <div>
              <p className="eyebrow"><span>FEATURES</span></p>
              <h2>Small board.<br /><em>Big weekend.</em></h2>
            </div>
            <p className="features-intro">A pocket-sized piece of hardware packed with everything you need to play, connect, and discover.</p>
          </div>
          <div className="spec-grid">
            {specs.map((spec) => (
              <article className={`spec-card spec-card-${spec.slug}`} key={spec.slug}>
                <img className="spec-photo" src={spec.photo} alt={spec.alt} loading="lazy" />
                <div className="spec-copy">
                  <p>{spec.label}</p>
                  <strong>{spec.value}</strong>
                  <span>{spec.detail}</span>
                </div>
              </article>
            ))}
          </div>
        </section>

        <section className="apps-section section-pad">
          <div className="section-kicker">
            <span>03</span>
            <p>APPS & EXPERIENCES</p>
          </div>
          <div className="apps-heading">
            <div className="apps-heading-copy">
              <h2>Tons of apps<br /><em>to discover.</em></h2>
              <p>Your Hacker Badge starts with apps including Connect, Scanner, and Snake. Explore the venue to unlock more.</p>
            </div>
            <div className="apps-heading-photo">
              <img src={appsLauncherPhoto} alt="The badge launcher showing a grid of apps" />
            </div>
          </div>
          <div className="app-grid">
            <article className="app-card app-card-connect">
              <div className="app-icon"><img src={connectIcon} alt="Two badges connecting" /></div>
              <div>
                <p className="app-label">CONNECT</p>
                <h3>Share your info with a bump.</h3>
                <p>Open Connect on both badges, then gently bump them to exchange the contact details you added during RSVP. You can also hold the badges close and press A on both.</p>
              </div>
            </article>
            <article className="app-card app-card-chess">
              <div className="app-icon"><img src={chessIcon} alt="Chess app icon" /></div>
              <div>
                <p className="app-label">MULTIPLAYER CHESS</p>
                <h3>Chess, badge to badge.</h3>
                <p>Connect with other hackers to play chess together, each on your own badge in real time.</p>
              </div>
            </article>
            <article className="app-card app-card-tetris">
              <div className="app-icon"><img src={tetrisIcon} alt="Tetris app icon" /></div>
              <div>
                <p className="app-label">TETRIS</p>
                <h3>Clear lines anywhere.</h3>
                <p>Stack falling blocks, clear lines, and chase a high score right on your badge.</p>
              </div>
            </article>
            <article className="app-card app-card-solana">
              <img className="solana-glyph" src={solanaLogo} alt="" width="101" height="88" />
              <div>
                <p className="app-label">SOLANA SCAVENGER HUNT</p>
                <h3>The venue is the game.</h3>
                <p>Open Solana Scavenger Hunt and find boxes with the Solana logo around the venue. Hold your badge close to a box to unlock pals for your My Badge screen.</p>
              </div>
            </article>
            <article className="app-card app-card-scanner">
              <div className="app-icon app-icon-sticker"><img src={scannerSticker} alt="Use your badge's Scanner app sticker" /></div>
              <div>
                <p className="app-label">SCANNER</p>
                <h3>Scan stickers for unlockables.</h3>
                <p>Use the Scanner app to scan stickers around the event and unlock new apps, games, and surprises.</p>
              </div>
            </article>
          </div>
          <div className="unlock-strip">
            <div className="unlock-copy"><p className="eyebrow"><span>UNLOCKABLES</span> FIND THEM ALL</p><h3>What will your badge discover?</h3></div>
            <div className="unlock-apps" aria-label="Example unlockable apps">
              <div><img src={eightBallIcon} alt="" /><span>8-Ball</span></div>
              <div><img src={tetrisIcon} alt="" /><span>Tetris</span></div>
              <div><img src={flappyIcon} alt="" /><span>Flappy</span></div>
              <div className="more-apps"><b>+?</b><span>And more</span></div>
            </div>
          </div>
        </section>

        <section className="partnership-section">
          <div className="partnership-mark">
            <img src={partnershipBadgePhoto} alt="The badge screen displaying Hack the North and Solana" />
          </div>
          <div className="partnership-copy">
            <div className="partnership-lockup">
              <p className="eyebrow"><span>HACK THE NORTH × SOLANA</span></p>
              <a className="partnership-logo" href="https://solana.com/" aria-label="Visit Solana">
                <img src={solanaWordmark} alt="Solana" width="646" height="96" loading="lazy" />
              </a>
            </div>
            <h2>Engineered by <span className="partnership-name">Hack the North.</span><br /><em>Made possible by Solana.</em></h2>
            <p>
              The Hacker Badge was designed and developed in-house by Hack the North, from PCB design and component selection to firmware and software development.
            </p>
            <p>
              This project was made possible by Solana, a high-performance blockchain and builder ecosystem powering fast, scalable experiences across payments, gaming, AI, and more.
            </p>
            <a className="text-link" href="https://solana.com/solutions" rel="noreferrer">Explore Solana <ArrowIcon /></a>
          </div>
        </section>

        <section id="jaza" className="battery-partner-section" aria-labelledby="battery-partner-heading">
          <div className="battery-partner-copy">
            <div className="battery-partner-lockup">
              <p className="eyebrow"><span>OUR BATTERY PARTNER</span></p>
              <a className="jaza-logo" href="https://www.jazaenergy.com/" aria-label="Visit Jaza">
                <img src={jazaLogo} alt="Jaza" width="158" height="29" loading="lazy" />
              </a>
            </div>
            <h2 id="battery-partner-heading">Batteries<br /><em>included.</em></h2>
            <p className="battery-partner-credit">
              Jaza is providing all the AA batteries for every Hacker Badge at Hack the North.
            </p>
            <p className="battery-partner-mission">
              Beyond the badge, Jaza brings affordable, reliable energy to communities across Africa through solar-powered battery hubs. Run by local women known as Jaza Stars, these hubs power homes, businesses, and new opportunities.
            </p>
            <a className="text-link" href="https://www.jazaenergy.com/" rel="noreferrer">Explore JAZA <ArrowIcon /></a>
          </div>
          <div className="battery-partner-art">
            <img
              src={jazaBatteriesPhoto}
              alt="A massive pile of AA batteries illuminated by blue light"
              width="1536"
              height="1024"
              loading="lazy"
              decoding="async"
            />
          </div>
        </section>

        <section className="help-section section-pad">
          <div className="faq-column">
            <h2>Frequently asked<br />questions.</h2>
            <div className="faq-list">
              {faqs.map((faq) => (
                <details key={faq.question}>
                  <summary>{faq.question}<span aria-hidden="true">+</span></summary>
                  <p>{faq.answer}</p>
                </details>
              ))}
            </div>
          </div>
          <aside className="manual-card">
            <p className="eyebrow"><span>NEED A HAND?</span></p>
            <div>
              <span className="manual-symbol" aria-hidden="true">↗</span>
              <h2>Instruction<br />manual</h2>
              <p>The full instruction manual is coming soon, with setup, controls, troubleshooting, and guides to building your own apps. For help during the event, visit the badge help desk.</p>
            </div>
            <a className="button button-primary" href="/manual">View the manual <ArrowIcon /></a>
          </aside>
        </section>
      </main>
    </Shell>
  );
}

function ManualPage() {
  if (shouldEmbedManual()) {
    return (
      <Shell page="manual">
        <main id="content" className="manual-page manual-page-embedded">
          <p className="manual-embed-note">
            If the manual does not load, <a href={MANUAL_URL} target="_blank" rel="noreferrer">open it directly in Notion</a>.
          </p>
          <iframe
            className="manual-embed"
            src={MANUAL_EMBED_URL}
            title="Hack the North Hacker Badge instruction manual"
            allowFullScreen
          />
        </main>
      </Shell>
    );
  }

  return (
    <Shell page="manual">
      <main id="content" className="manual-page">
        <section>
          <p className="eyebrow"><span>INSTRUCTION MANUAL</span> HACK THE NORTH 2026</p>
          <h1>Everything you need<br /><em>to make it yours.</em></h1>
          <div className="manual-link-block">
            <span>OPEN MANUAL</span>
            <p>For the best experience on this browser, open the instruction manual directly in Notion.</p>
            <a className="button button-primary" href={MANUAL_URL} target="_blank" rel="noreferrer">
              View instruction manual <ArrowIcon />
            </a>
          </div>
        </section>
      </main>
    </Shell>
  );
}

const glyphRows = [
  ["@", "#", "?", "&", "%", "+"],
  ["!", "?", "$", "@", "=", "*"],
  ["&", "~", "=", "#", "!", "^"],
  ["^", "+", "*", "%", "$", "~"],
];

const frequencyRows = [
  ["RED", "7", "3", "9", "1"],
  ["BLUE", "4", "8", "2", "6"],
  ["GREEN", "0", "5", "1", "9"],
  ["GOLD", "6", "2", "8", "4"],
];

const wordRows = [
  ["GOOSE", "314", "271"],
  ["MAPLE", "502", "846"],
  ["WATER", "731", "094"],
  ["NIGHT", "268", "513"],
  ["BUILD", "905", "427"],
  ["NORTH", "146", "680"],
  ["LASER", "893", "352"],
  ["CLOUD", "470", "925"],
];

function ManualTable({ headings, rows, label }) {
  return (
    <div className="ktnh-table-wrap" role="region" aria-label={label} tabIndex={0}>
      <table aria-label={label}>
        <thead><tr>{headings.map((heading) => <th key={heading}>{heading}</th>)}</tr></thead>
        <tbody>
          {rows.map((row) => (
            <tr key={row.join("-")}>{row.map((cell, index) => <td key={`${cell}-${index}`}>{cell}</td>)}</tr>
          ))}
        </tbody>
      </table>
    </div>
  );
}

function KtnhPage() {
  return (
    <Shell page="ktnh">
      <main id="content" className="ktnh-page">
        <section className="ktnh-hero">
          <p className="eyebrow"><span>EXPERT MANUAL</span> HACK THE NORTH 2026</p>
          <div className="ktnh-hero-grid">
            <div>
              <h1>Keep Talking<br /><em>and Nobody Honks.</em></h1>
              <p className="ktnh-lede">
                A group game for two or more people. One Operator holds the badge; everyone else uses this manual as the Experts. Describe and decode every puzzle before the timer runs out.
              </p>
            </div>
            <aside className="ktnh-warning">
              <span>EXPERTS ONLY</span>
              <strong>Operator: no peeking.</strong>
              <p>The fun comes from describing the badge out loud. Put this screen in an Expert&apos;s hands.</p>
            </aside>
          </div>
        </section>

        <section className="ktnh-start section-pad">
          <div>
            <p className="eyebrow"><span>HOW TO PLAY</span></p>
            <h2>One badge.<br /><em>Two roles.</em></h2>
          </div>
          <ol className="ktnh-steps">
            <li><b>01</b><div><strong>Split up</strong><p>The Operator holds the badge. Everyone else is an Expert with this page.</p></div></li>
            <li><b>02</b><div><strong>Share the unit code</strong><p>The two-digit code at the top of the badge changes how several rules work.</p></div></li>
            <li><b>03</b><div><strong>Describe and decode</strong><p>The badge does not name its modules. Use the example screens below to identify what the Operator describes. Experts never view the badge; the Operator never reads the tables.</p></div></li>
            <li><b>04</b><div><strong>Beat the clock</strong><p>Every wrong submission costs 10 seconds. Start opens the module selector; switching preserves each puzzle and partial answer while the clock keeps running. Clear all modules to defuse the honk.</p></div></li>
          </ol>
        </section>

        <section className="ktnh-controls">
          <div><span>DIFFICULTY</span><strong>1–6 unique modules</strong></div>
          <div><span>TIME</span><strong>1:30–4:00</strong></div>
          <div><span>MISTAKE</span><strong>−10 seconds</strong></div>
          <div><span>EXIT</span><strong>HOME, then HOME to confirm</strong></div>
        </section>

        <section className="ktnh-manual section-pad">
          <div className="ktnh-manual-heading">
            <p className="eyebrow"><span>DECODER</span> SIX MODULES</p>
            <h2>Say exactly<br /><em>what you see.</em></h2>
          </div>

          <article id="patch-cables" className="ktnh-module">
            <header><span>01</span><div><p>CUT ONE</p><h3>Patch Cables</h3></div><img className="ktnh-module-preview" src="/ktnh/patch-cables.png" alt="Badge puzzle with four numbered colored cables" width="320" height="240" loading="lazy" /></header>
            <div className="ktnh-module-body">
              <p>Ask for all four cable colours in numbered order, then apply the first rule that matches:</p>
              <ol className="ktnh-rule-stack">
                <li><b>1</b>If the unit code is even and there is a RED cable, cut the first RED cable.</li>
                <li><b>2</b>Otherwise, if there is a BLUE cable, cut the last BLUE cable.</li>
                <li><b>3</b>Otherwise, if there is exactly one GOLD cable, cut that cable.</li>
                <li><b>4</b>Otherwise, cut cable 4.</li>
              </ol>
            </div>
          </article>

          <article id="glyph-stack" className="ktnh-module">
            <header><span>02</span><div><p>PRESS FOUR</p><h3>Glyph Stack</h3></div><img className="ktnh-module-preview" src="/ktnh/glyph-stack.png" alt="Badge puzzle with four selectable symbols" width="320" height="240" loading="lazy" /></header>
            <div className="ktnh-module-body">
              <p>Symbols appear in more than one row, with different orders. Find the one row containing all four shown glyphs. Tell the Operator to press them in that row&apos;s left-to-right order, skipping glyphs that are not shown.</p>
              <div className="glyph-order" role="region" aria-label="Glyph ordering rows" tabIndex={0}>
                {glyphRows.map((row, index) => <div key={row.join("")}><b>{index + 1}</b>{row.map((glyph) => <code key={glyph}>{glyph}</code>)}</div>)}
              </div>
            </div>
          </article>

          <article id="frequency-dial" className="ktnh-module">
            <header><span>03</span><div><p>SET ONE DIGIT</p><h3>Frequency Dial</h3></div><img className="ktnh-module-preview" src="/ktnh/frequency-dial.png" alt="Badge puzzle with a signal color and a single-digit dial" width="320" height="240" loading="lazy" /></header>
            <div className="ktnh-module-body">
              <p>Use the signal colour as the row and the <strong>last digit</strong> of the unit code as the column. Set the dial to the result.</p>
              <ManualTable label="Frequency dial decoder" headings={["SIGNAL", "0–1", "2–4", "5–7", "8–9"]} rows={frequencyRows} />
            </div>
          </article>

          <article id="arrow-relay" className="ktnh-module">
            <header><span>04</span><div><p>ENTER THREE</p><h3>Arrow Relay</h3></div><img className="ktnh-module-preview" src="/ktnh/arrow-relay.png" alt="Badge puzzle with three directions and three answer slots" width="320" height="240" loading="lazy" /></header>
            <div className="ktnh-module-body">
              <p>Ask for the three directions in order. Use the <strong>tens digit</strong> of the unit code:</p>
              <ul className="ktnh-transform-list">
                <li><b>1–2</b><span>Rotate every direction clockwise.</span><code>U→R→D→L→U</code></li>
                <li><b>3–4</b><span>Replace every direction with its opposite.</span><code>U↔D &nbsp; R↔L</code></li>
                <li><b>5–6</b><span>Rotate every direction counter-clockwise.</span><code>U→L→D→R→U</code></li>
                <li><b>7–9</b><span>Reverse the order only. Do not change directions.</span><code>U R D → D R U</code></li>
              </ul>
            </div>
          </article>

          <article id="word-lock" className="ktnh-module">
            <header><span>05</span><div><p>ENTER THREE DIGITS</p><h3>Word Lock</h3></div><img className="ktnh-module-preview" src="/ktnh/word-lock.png" alt="Badge puzzle with a code word and a three-digit combination" width="320" height="240" loading="lazy" /></header>
            <div className="ktnh-module-body">
              <p>Find the displayed word. Choose the EVEN or ODD column using the whole unit code, then enter the three digits.</p>
              <ManualTable label="Word lock combinations" headings={["WORD", "EVEN UNIT", "ODD UNIT"]} rows={wordRows} />
            </div>
          </article>

          <article id="simon-honks" className="ktnh-module">
            <header><span>06</span><div><p>ENTER THREE</p><h3>Simon Honks</h3></div><img className="ktnh-module-preview" src="/ktnh/simon-honks.png" alt="Badge puzzle with a flashing color signal and three answer slots" width="320" height="240" loading="lazy" /></header>
            <div className="ktnh-module-body">
              <p>Ask for the three flashing colours in order. Use the <strong>last digit</strong> of the unit code, translate each colour, and read back the three D-pad directions.</p>
              <ManualTable
                label="Simon Honks colour translations"
                headings={["LAST DIGIT", "RED", "BLUE", "GREEN", "GOLD"]}
                rows={[
                  ["0–2", "UP", "RIGHT", "DOWN", "LEFT"],
                  ["3–5", "RIGHT", "DOWN", "LEFT", "UP"],
                  ["6–7", "DOWN", "LEFT", "UP", "RIGHT"],
                  ["8–9", "LEFT", "UP", "RIGHT", "DOWN"],
                ]}
              />
              <p className="ktnh-note">The Operator can press A to replay the loop from its first colour.</p>
            </div>
          </article>
        </section>

        <section className="ktnh-finale">
          <p className="eyebrow"><span>GOOD LUCK</span></p>
          <h2>Keep calm.<br /><em>Keep honking.</em></h2>
          <a className="button button-primary" href="#content">Back to top <ArrowIcon /></a>
        </section>
      </main>
    </Shell>
  );
}

const rules = [
  {
    title: "Experimental electronic badge",
    body: [
      "I understand that the Hack the North badge is an experimental electronic event badge provided for educational, interactive, and event participation purposes. It is not a toy, medical device, safety device, or certified consumer product.",
      "I understand that the badge contains electronic components and should be handled with reasonable care.",
    ],
  },
  {
    title: "Safe use",
    body: ["I agree to use the badge responsibly and follow all instructions provided by Hack the North, including instructions printed on the badge, shown on the badge screen, posted at badge.hackthenorth.com, or communicated by organizers."],
    bullets: [
      "Use only AA alkaline batteries.",
      "Insert batteries according to the marked + / – polarity.",
      "Keep the badge dry.",
      "Do not crush, bend, puncture, short, burn, or physically abuse the badge.",
      "Do not wear the badge while sleeping.",
      "Stop using it if it becomes hot, smells unusual, smokes, appears damaged, or behaves unexpectedly.",
      "Bring any badge that appears unsafe or damaged to an organizer.",
    ],
  },
  {
    title: "Air travel",
    body: [
      "I understand that airline and airport rules may apply to electronic devices and batteries. Before flying, I agree to remove the batteries from the badge and keep the badge and batteries in my carry-on baggage, unless instructed otherwise by airport security or my airline.",
      "I understand that I am responsible for following the rules of my airline, airport, and destination.",
    ],
  },
  {
    title: "Software and hardware modification",
    body: [
      "Hack the North may allow or encourage software-based interaction with the badge as part of the event experience.",
      "I understand that hardware modification, external power modification, soldering, physical alteration, or connecting unsupported accessories may damage the badge or create safety risks. Hardware modifications are not supported by Hack the North unless explicitly allowed in official badge instructions or supervised badge activities.",
    ],
  },
  {
    title: "Assumption of risk",
    body: [
      "I understand that using experimental electronics involves some inherent risks, including damage to the badge, irritation, electrical malfunction, or property damage if the badge is misused, damaged, modified, exposed to water, or used with incorrect batteries.",
      "I voluntarily accept the risks associated with using, carrying, and interacting with the badge.",
    ],
  },
  {
    title: "Limited replacements",
    body: [
      "I understand that badges are produced in limited quantities. Hack the North may not be able to replace badges that are lost, damaged, wet, modified, misused, or otherwise made unusable.",
      "Hack the North may, at its discretion, replace or disable a badge if needed for safety, event operations, or accessibility reasons.",
    ],
  },
  {
    title: "Organizer authority",
    body: ["I agree that Hack the North organizers may inspect, disable, collect, or refuse use of a badge if they reasonably believe it is unsafe, damaged, misused, interfering with event systems, or being used in a way that violates event rules."],
  },
  {
    title: "Release",
    body: ["To the fullest extent permitted by law, I release Hack the North, its organizers, volunteers, sponsors, partners, and venue hosts from claims arising from my use, misuse, modification, transport, or handling of the badge, except where such release is not permitted by law."],
  },
  {
    title: "Minors",
    body: ["If I am under the age of majority, my parent or legal guardian acknowledges and agrees to these terms on my behalf."],
  },
];

function RulesPage() {
  return (
    <Shell page="rules">
      <main id="content" className="rules-page">
        <section className="rules-hero">
          <h1>Hacker Badge Rules</h1>
          <p>
            You&apos;ve already agreed to these terms in your event waiver, and the same safety guidance is printed on the
            back of your badge. We&apos;re restating it here to make sure we&apos;re all on the same page.
          </p>
        </section>

        <aside className="safety-callout">
          <span className="warning-mark">!</span>
          <div>
            <strong>If your badge is damaged: stop using it immediately</strong>
            <p>
              The badge is an experimental electronic device. If yours becomes hot, smells unusual, smokes, appears
              damaged, or behaves unexpectedly, turn it off and bring it to the badge help desk.
            </p>
          </div>
        </aside>

        <div className="rules-layout">
          <aside className="rules-index" aria-label="Rules index">
            <span>CONTENTS</span>
            {rules.map((rule, index) => (
              <a key={rule.title} href={`#rule-${index + 1}`}>
                <b>{String(index + 1).padStart(2, "0")}</b>{rule.title}
              </a>
            ))}
          </aside>
          <div className="rules-list">
            {rules.map((rule, index) => (
              <section id={`rule-${index + 1}`} className="rule" key={rule.title}>
                <span className="rule-number">{String(index + 1).padStart(2, "0")}</span>
                <div>
                  <h2>{rule.title}</h2>
                  {rule.body.map((paragraph) => <p key={paragraph}>{paragraph}</p>)}
                  {rule.bullets && (
                    <ul>{rule.bullets.map((item) => <li key={item}>{item}</li>)}</ul>
                  )}
                </div>
              </section>
            ))}
          </div>
        </div>

        <section className="legal-note">
          <p className="eyebrow"><span>ACKNOWLEDGEMENT</span> EVENT DEVICES</p>
          <h2>Hacker Badge and Event Devices</h2>
          <p>
            You acknowledge that Hack the North may provide you with a hacker badge or other event-related electronic
            device (collectively, the “Badge”) for use during the Event. The Badge may contain electronic components,
            batteries, and/or other hardware. You agree to use the Badge only as instructed, including by following all
            directions, warnings, and rules printed on the Badge and posted at{" "}
            <a href="https://badge.hackthenorth.com">badge.hackthenorth.com</a>, as updated from time to time.
          </p>
          <p>
            You agree not to misuse, tamper with, disassemble, modify, puncture, short-circuit, expose to water or excessive
            heat, or otherwise use the Badge in a manner inconsistent with the provided instructions. You assume all risks
            associated with your possession, use, misuse, loss, theft, damage, or disposal of the Badge, including any risk
            of personal injury, property damage, device malfunction, data loss, or damage to your own or another person’s
            property. To the fullest extent permitted by law, you release and discharge the Releasees from any liability,
            claim, loss, damage, cost, or expense arising out of or relating to the Badge, including its provision,
            condition, use, misuse, malfunction, loss, theft, damage, or disposal, whether caused by negligence or otherwise.
          </p>
          <p>
            The Organizers may, in their discretion, inspect, disable, replace, restrict, or require return of any Badge,
            and may refuse replacement for any lost, stolen, damaged, modified, or misused Badge.
          </p>
        </section>

        <aside id="badge-choice" className="badge-choice-note" aria-label="Your badge choice">
          <p>
            <strong>Receiving and using a Hacker Badge is entirely voluntary.</strong>{" "}
            If you do not want a Hacker Badge, you can choose a non-digital badge instead.
          </p>
        </aside>
      </main>
    </Shell>
  );
}

const FLASH_PINS = [
  ["LCD MOSI", "10", "SPI2, 40 MHz, mode 0"],
  ["LCD CLK", "1", ""],
  ["LCD CS", "2", ""],
  ["LCD DC", "0", ""],
  ["LCD RST", "4", ""],
  ["I2C SDA", "5", "Shared: accel 0x19 + NFC 0x26, 400 kHz"],
  ["I2C SCL", "6", ""],
  ["HC165 DATA", "7", "Button shift register serial out"],
  ["HC165 LOAD", "20", "Pulse low to latch"],
  ["HC165 CLK", "21", ""],
  ["START button", "9", "Active-low pushbutton; strapping pin (download mode)"],
  ["LED DIN (WS2812)", "3", "RMT peripheral, GRB order"],
];

function FlashSection({ index, title, children }) {
  return (
    <section className="rule flash-rule">
      <span className="rule-number">{index}</span>
      <div>
        <h2>{title}</h2>
        {children}
      </div>
    </section>
  );
}

function CustomFlashPage() {
  return (
    <Shell page="custom-flash">
      <main id="content" className="rules-page">
        <section className="rules-hero">
          <p className="eyebrow"><span>EXPERIMENTAL</span> RE-FLASH AT YOUR OWN RISK</p>
          <h1>Custom Flash</h1>
          <p>
            Everything you need to wipe the badge and write your own firmware:
            the pin map, the flashing procedure, and bring-up notes for the
            buttons, screen, accelerometer, LEDs, NFC, and radio.
          </p>
        </section>

        <aside className="safety-callout">
          <span className="warning-mark">!</span>
          <div>
            <strong>Very experimental. All bets are off if you do this.</strong>
            <p>
              Re-flashing replaces the event firmware. If it goes wrong your
              badge may stop working entirely, and badges are produced in
              limited quantities — we may not be able to replace one that is
              bricked, damaged, or misused. Back up anything you care about
              first. If your badge becomes hot, smells unusual, smokes, or
              behaves unexpectedly, turn it off and bring it to the badge
              help desk.
            </p>
          </div>
        </aside>

        <div className="flash-download">
          <a className="button button-primary" href="/custom-firmware-hal.md" download>
            Download the HAL guide (.md) <ArrowIcon />
          </a>
          <p>Plain Markdown — feed it straight to your AI assistant as context.</p>
        </div>

        <div className="flash-guide">
          <FlashSection index="00" title="What you are working with">
            <ul>
              <li>MCU module: ESP32-C3-MINI-1-N4 (RISC-V, 4 MB flash, native USB-Serial-JTAG).</li>
              <li>Display: ST7789, 320×240, RGB565, SPI.</li>
              <li>Buttons: 8 inputs on a 74HC165 shift register + 1 dedicated Start button.</li>
              <li>Accelerometer: SC7A20HTR on shared I2C, address <code>0x19</code>.</li>
              <li>NFC reader: MFRC522 on the same I2C bus, address <code>0x26</code>.</li>
              <li>LEDs: 6× WS2812B-2020 around the board edge.</li>
              <li>Power: AA battery holder → MT3608 boost → XC6220 LDO; USB-C for flashing.</li>
              <li>Flash layout: app at <code>0x10000</code> (<code>0x2A0000</code>), storage after it (<code>0x140000</code>). Flash mode DIO, 80 MHz, 4 MB.</li>
            </ul>
          </FlashSection>

          <FlashSection index="01" title="Toolchain + flashing">
            <ol>
              <li>Install ESP-IDF <strong>v5.5.3</strong> and activate it (<code>idf.py</code> is only on PATH after sourcing the activation script).</li>
              <li>Create an <code>esp32c3</code> project and set the target: <code>idf.py set-target esp32c3</code>.</li>
              <li>Plug in USB-C. The console is <strong>USB-Serial-JTAG, not UART0</strong> — enable <code>CONFIG_ESP_CONSOLE_USB_SERIAL_JTAG</code>.</li>
              <li>Flash with <code>idf.py -p /dev/tty.usbmodemXXXX flash</code> (macOS) or <code>/dev/ttyACM0</code> (Linux).</li>
              <li>If esptool reports “No serial data received”, enter download mode: <strong>hold Start (GPIO9) while plugging in USB</strong>, then retry. A blank screen here is expected — that is download mode, not a brick.</li>
              <li><code>idf.py monitor</code> for serial output. Only one process may own the port at a time.</li>
            </ol>
          </FlashSection>

          <FlashSection index="02" title="Pin map">
            <div className="ktnh-table-wrap" role="region" aria-label="Badge pin map" tabIndex={0}>
              <table aria-label="Badge pin map">
                <thead><tr><th>Function</th><th>GPIO</th><th>Notes</th></tr></thead>
                <tbody>
                  {FLASH_PINS.map((row) => (
                    <tr key={row[0]}>{row.map((cell, i) => <td key={i}>{cell}</td>)}</tr>
                  ))}
                </tbody>
              </table>
            </div>
          </FlashSection>

          <FlashSection index="03" title="Buttons (74HC165 + Start)">
            <ul>
              <li>Shift order, active-low: <code>A, B, Home, Down, Left, Right, Up, Aux1</code> (A shifts out first).</li>
              <li>Read protocol: pull <code>LOAD</code> low then high to latch; repeat 8× — sample <code>DATA</code>, then pulse <code>CLK</code> high-low. A sampled <code>0</code> means pressed.</li>
              <li><code>Start</code> is separate: GPIO9 low means pressed. It is also the strapping pin, so holding it at reset enters download mode.</li>
              <li>Poll at ~10 ms and debounce. Aux1 is a maintained side switch, not momentary.</li>
            </ul>
          </FlashSection>

          <FlashSection index="04" title="Screen (ST7789, 320×240)">
            <ul>
              <li>SPI2: MOSI=10, CLK=1, CS=2, DC=0, RST=4. 40 MHz, mode 0, 16-bit RGB565.</li>
              <li>Init: reset, init, <code>invert_color(true)</code>, <code>swap_xy(true)</code>, <code>mirror(true, false)</code>. If the image looks rotated or mirrored, those three calls are the fix.</li>
              <li>Use the ESP-IDF <code>esp_lcd</code> ST7789 driver; two ~30-row DMA stripe buffers are plenty.</li>
            </ul>
          </FlashSection>

          <FlashSection index="05" title="Accelerometer (SC7A20, 0x19)">
            <ul>
              <li>SDA=5, SCL=6, 400 kHz. <code>WHO_AM_I (0x0F)</code> should read <code>0x11</code>.</li>
              <li><code>CTRL_REG1 (0x20) = 0x57</code> → 100 Hz, all axes on. <code>CTRL_REG4 (0x23) = 0x80</code> → ±2 g.</li>
              <li>Poll <code>STATUS (0x27)</code> for ZYXDA, read 6 bytes from <code>OUT_X_L (0x28)</code> with auto-increment. Each axis is 12-bit left-justified: 1 count = 1 mg.</li>
              <li>The NFC chip shares this bus: use bounded I2C timeouts and retry, never wait forever.</li>
            </ul>
          </FlashSection>

          <FlashSection index="06" title="LEDs (6× WS2812B, GPIO3)">
            <ul>
              <li>Single data line on GPIO3, GRB order, driven by the RMT peripheral.</li>
              <li>Front order: 0 UpperLeft, 1 UpperRight, 2 MiddleRight, 3 BottomRight, 4 BottomLeft, 5 MiddleLeft.</li>
              <li>Keep brightness modest — full white on all six can brown-out the board on AA power.</li>
            </ul>
          </FlashSection>

          <FlashSection index="07" title="NFC + radio (optional)">
            <ul>
              <li>NFC (MFRC522, <code>0x26</code>) is power-hungry: keep it off except while scanning, and don&apos;t init it at all if you don&apos;t need it.</li>
              <li>BLE via NimBLE works (extended advertising + passive scan), but stock buffer tuning OOMs the heap — trim to small connectionless sizes, 1M PHY only.</li>
            </ul>
          </FlashSection>

          <FlashSection index="08" title="Minimal HAL checklist">
            <ul className="flash-checklist">
              <li>GPIO init + Start-button read (GPIO9, active-low).</li>
              <li>HC165 bit-bang read in A,B,Home,Down,Left,Right,Up,Aux1 order.</li>
              <li>10 ms poll + debounce.</li>
              <li>SPI ST7789 init + fill-screen test.</li>
              <li>I2C init + accel WHO_AM_I check + mg read.</li>
              <li>WS2812 RMT init + chase pattern (dim!).</li>
              <li>NFC/BLE only if your project needs them.</li>
            </ul>
          </FlashSection>
        </div>
      </main>
    </Shell>
  );
}

function App() {
  const path = window.location.pathname.replace(/\/+$/, "") || "/";

  if (path === "/sync") {
    document.title = "Sync your badge | Hack the North";
    return <Shell page="sync"><SyncPage /></Shell>;
  }

  if (path === "/closeup") {
    document.title = "Ultra Closeup | Hack the North Badge";
    return <Shell page="closeup"><CloseupViewer /></Shell>;
  }

  if (path === "/rules") {
    document.title = "Badge Rules | Hack the North";
    return <RulesPage />;
  }

  if (path === "/manual") {
    document.title = "Instruction Manual | Hack the North Badge";
    return <ManualPage />;
  }

  if (path === "/ktnh") {
    document.title = "Keep Talking and Nobody Honks | Hacker Badge";
    return <KtnhPage />;
  }

  if (path === "/custom-flash") {
    document.title = "Custom Flash | Hacker Badge";
    return <CustomFlashPage />;
  }

  document.title = "Hacker Badge | Hack the North";
  return <HomePage />;
}

createRoot(document.getElementById("root")).render(
  <React.StrictMode>
    <App />
  </React.StrictMode>,
);
