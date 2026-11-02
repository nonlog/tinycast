# Windows adaptation status

Upstream baseline: `abue-ammar/tinycast`, commit `a5b11bed`.
The macOS SwiftUI application remains in `Tinycast/`. Windows is an independent native executable
under `windows/`, with explicit behavioral adaptations, not a binary-compatible SwiftUI build.

## Feature matrix

| Area | Windows preview | Remaining parity work |
| --- | --- | --- |
| Palette | Win32 Unicode Edit + owner-drawn ListBox, light/dark, Chinese/English, tray, global shortcut, single instance | Broader accessibility audit, complete settings editor, richer actions UI |
| App launcher | Start Menu + packaged apps, shared fuzzy tiers, favorites and bounded usage boost | Modern upstream DP alignment, script romanization/pinyin, running-app toggle, per-app shortcuts |
| Calculator | Safe arithmetic parser, common functions, length/mass/time/data/temperature units, session history | Full natural-language/date/time grammar, live currency/crypto, locale-aware input |
| Clipboard | Opt-in text history, RAM-only bounded storage, exclusion markers, copy/paste | Images/files, OCR, persistent encrypted history, pinning, per-app exclusion editor |
| Files | Cancellable, bounded configured-folder name search | Persistent index, live updates, advanced scopes and metadata search |
| Quicklinks | HTTP(S) keyword searches, UTF-8 encoded input | File/deeplink forms, parameter UI, clipboard placeholders |
| Snippets | Named text templates, single-pass substitution, explicit paste | Markdown UI, nested references, automatic keyword expansion |
| Commands | Separate executable/arguments with per-invocation confirmation | Sandboxed scripting, argument forms, command-specific hotkeys |
| Window management | 18 geometry actions plus original-placement restore, work-area and DWM-frame handling | Monitor moves, custom layouts, Rooms, cycling, virtual desktop integration |
| Raycast extensions | Not ported | JS runtime, native component renderer, API/permissions/OAuth compatibility |
| AI/MCP and dictation | Not ported | Windows service/provider design and credential storage before enabling |
| Calendar/notes/dictionary | Not ported | Native Windows implementations and integration choices |
| Updating | Portable CI build and checksum-matched Scoop release manifest; no autonomous updater | Ongoing bucket version maintenance and signing |

## Adapted source behavior

- `src/core.cpp` fuzzy matching follows `Tinycast/Features/Launcher/Model/SearchRelevance.swift`'s
  exact/prefix/word-start/substring/subsequence tiers. It does not claim parity with the newer
  `LauncherMatch.swift` dynamic-programming alignment or `ScriptRomanization.swift`.
- Window placement follows `Tinycast/Features/WindowManagement/Model/WindowPlacementEngine.swift`:
  rounded shared edges, outer/interior gap rules, halves/quarters/thirds, center and resizing.
- The arithmetic parser is a new bounded C++ implementation of a subset, not a translation of every
  `Calculator/Model/` file. It never evaluates code or fetches exchange rates.
- Unicode matching supports ASCII case-folding, common Latin diacritics, fullwidth ASCII and direct
  CJK text. It does not implement full Unicode canonical composition or every locale's case rules.

## Acceptance boundaries

Automated CI covers deterministic core behavior, JSON validation/preservation, isolated file search,
invalid-window rejection, native startup, Unicode control round-trips and synthetic composition guards.
It cannot alone establish real IME candidate behavior, physical multi-monitor mixed-DPI behavior,
assistive-technology usability, elevated-app interaction or long-session performance.

Release packages must retain the upstream AGPL license and notices, the exact source commit and a
checksum. No unsigned binary should be presented as a signed release, and no preview should be
presented as complete upstream feature parity.
