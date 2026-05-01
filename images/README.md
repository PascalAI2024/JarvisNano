# Visual identity

Brand assets for JarvisNano. Palette matches [Ingenious Digital](https://ingeniousdigital.com): matte charcoal (#0d1117 / #1a1a1a) + vivid orange neon (#FF5722).

## Canonical assets (use these)

| File | Use | Notes |
| --- | --- | --- |
| [`logo.png`](logo.png) | Primary brand mark | Arc-reactor lens with neon J. Reads at favicon size when cropped tight. |
| [`logo-secondary.png`](logo-secondary.png) | Secondary stamp | Hex-frame JR monogram. Use sparingly — page footers, hardware etch, document watermark. |
| [`wordmark.png`](wordmark.png) | README banner / lockup | Two-line "JARVIS / NANO" with mark on the left, mirrors Ingenious Digital header structure. |
| [`mascot.png`](mascot.png) | Lineup add / hero portrait | Full-body chibi on a glowing orange ring — drops into the IGD 5-mascot lineup as #6 (*The J.A.R.V.I.S. — Voice Companion*). |
| [`mascot-bust.png`](mascot-bust.png) | Avatar / social card | Bust crop with neon J on chest plate. Ideal as GitHub social preview or Discord/Telegram avatar. |
| [`hero.png`](hero.png) | README hero | Phase-3 product render — Monolith with round AMOLED on, orange waveform, glowing under-ring. |
| [`hero-phase2.png`](hero-phase2.png) | Roadmap Phase 2 illustration | Modern smoked-oak cabinet, orange backlit grille, round screen. Anchors the speaker phase. |
| [`exploded-view.png`](exploded-view.png) | Hardware docs | Architectural exploded isometric of the Monolith. |
| [`cross-section.png`](cross-section.png) | Hardware docs | Side-view cutaway showing internal stack. |

## Subdirs

- [`igd-rebrand/`](igd-rebrand/) — full source set of the brand-aligned generation pass (mascot variants, all 3 logo concepts, all 3 product hero variants). Keep as masters; the canonical `logo.png` / `wordmark.png` etc. above are copies.
- [`enclosure/`](enclosure/) — 8 enclosure concept renders (4 exploded views + 1 cross-section + 3 Phase-3 hero shots) covering Monolith / Cube / Egg / Radio.
- [`early-concepts/`](early-concepts/) — first-pass non-IGD-aligned renders. Kept for provenance only; don't use in marketing.

## Provenance

- Logos & mascot: Flux Kontext Max via Kie.ai (`imagine.sh --max --square` / `--portrait`).
- Hero / housing photoreal renders: Flux Kontext Max + Z-Image (`--wide --max` / `--wide --z`).
- Generated 2026-05-01.

## Brand rules

- **Don't** recolor the orange — it must be the IGD signature `#FF5722`.
- **Don't** drop the underlying glow ring on product renders — it's the family signature shared across the IGD mascot lineup.
- **Don't** use the early-concepts/ assets in any public-facing place.
- **Do** pair the wordmark with the hero render in marketing; use `logo.png` solo only when space is tight (favicons, tab icons, etched marks).
