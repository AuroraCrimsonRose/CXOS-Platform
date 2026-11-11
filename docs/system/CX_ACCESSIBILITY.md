# Accessibility
### CX Design Spec: Aurora Tejeda / CATX Systems

> **Status: DESIGN 2026-11-11, nothing built.** Accessibility is part of the
> toolkit, the compositor and the trusted UI from the first day, tested in
> the verification boots like everything else, and its own privilege.

---

## 1. What it covers

- **Vision:** a screen reader; a magnifier following the focus and the text
  caret; text scaling (the toolkit's logical units and scalable fonts,
  roadmap 2.8a and 2.8a′); high-contrast themes; colour filters for colour
  blindness; large and high-contrast cursors; Braille displays.
- **Hearing:** captions; alert sounds also shown as a screen flash; mono
  downmix and balance (`CX_AUDIO.md` §2a item 9).
- **Motor:** full keyboard access to everything; sticky, slow, bounce and
  mouse keys; dwell click; switch access by scanning; voice control later;
  key repeat adjustable; no time limit that cannot be lengthened (the
  consent screen's 500 ms guard is a minimum, never a deadline).
- **Cognitive:** reduced motion (the compositor's springs off) and reduced
  transparency; Atkinson Hyperlegible Next and Mono as one setting
  (`CX_FONTS.md` §3 item 5); consistent layouts from the design system.

## 2. Where CXOS aims to be better (Aurora, 2026-11-11)

1. **The accessibility tree is the toolkit's widget tree.** Every widget has
   a role, a name and a state (roadmap 2.8a, goal 4). Elsewhere it is a
   bridge the app must feed (AT-SPI, UI Automation) and it falls out of
   step; here an app built on the toolkit has it with no work.
2. **Tested like the rest.** UI automation (2.8a goal 9) goes through the
   same tree, so a widget a test cannot reach is one a screen reader cannot
   reach. An interactive widget with no name, or one keyboard focus cannot
   reach, fails the verification boot; the check is sabotaged.
3. **Assistive technology is a privilege of its own.** Reading other
   programs' trees and sending them input needs its own grant: held by
   platform-signed tools, or given through the consent screen, with an
   indicator for as long as it is held. (Android's accessibility services
   are a main route for banking malware to read screens and press buttons.)
   Synthetic input never answers a consent screen or the Ctrl+Alt+Del screen;
   both are fully readable by the screen reader.
4. **There from the first second:** the boot menu, the login screen, the
   installer, recovery and the trusted screens. One shortcut, held anywhere,
   turns the screen reader on.
5. **The compositor's features reach every program:** zoom, colour filters
   and large cursors work for programs that draw every pixel themselves
   (games), since the compositor owns the pixels (roadmap 2.8e).
6. **Seizure protection:** the compositor sees every frame, so it detects
   flashing above three times a second (WCAG 2.3.1's threshold) and damps it,
   system-wide, as a setting.
7. **One place for the settings,** applied at once to every program and
   carried with the user.

## 3. Open

- **The speech engine: decided (Aurora, 2026-11-11).** The userland is X,
  so no engine's code can be vendored; what is taken is data, whose licence
  is read when it is vendored. Two voices, both our own engine:
  1. **The system voice:** our own formant synthesiser after Klatt's
     published 1980 design, with the CMU Pronouncing Dictionary (BSD-style)
     and published letter-to-sound rules for words it lacks. Small and fast:
     it runs in the boot menu, recovery and the installer, and stays clear
     at the very high rates experienced screen-reader users choose.
  2. **A natural voice, optional, later:** our own small neural inference
     with Kokoro's weights (Apache-2.0), as a package; SVOX Pico's voice data
     (Apache-2.0) is a lighter middle choice.
  Not used: eSpeak NG (GPL-3), Piper (its pronunciation is eSpeak NG's),
  Mimic 3 (AGPL), Coqui XTTS (non-commercial), MaryTTS and RHVoice (LGPL).
- **Braille:** translation tables and display drivers are our own (the
  common libraries are LGPL or GPL).
- **Captions** need a caption track in the media pipeline; live captions
  need speech recognition, much later.

## 4. Order

1. 2.8a: the tree, keyboard access everywhere, scaling, high contrast,
   reduced motion, the automation checks of §2 item 2.
2. displayd's input path: sticky, slow, bounce and mouse keys, repeat rate.
3. 2.8e: zoom, colour filters, large cursors, flash damping.
4. Phase B, with audio: the screen reader, the speech engine, the grant of
   §2 item 3, captions from a caption track.
5. Later: Braille, switch access, dwell click, voice control.
