# Windows port

This directory is the native Windows adaptation of Tinycast. The macOS sources remain
unchanged. Preserve AGPL-3.0 attribution and identify every adapted algorithm's upstream file.

- C++20, Win32, native controls. No WebView, Electron, Node runtime, or continuous render loop.
- `core.hpp` is platform-independent. The executable and tests compile the same core sources.
- `App` owns all long-lived UI state, threads, settings, clipboard history, and window restore state.
- Every worker has cancellation and joins on shutdown; stale results never replace a newer query.
- Clipboard recording is opt-in, bounded, in memory only, and must respect exclusion formats.
- Never log clipboard text, credentials, full user filesystem results, or command contents.
- Test/build/release exclusively in GitHub Actions. Do not compile on this workstation or VPS.
- All commits use Codex <codex@openai.com> as both author and committer.
- This is a preview until parity and manual acceptance are documented. Unsupported features must
  not appear as working controls. Preserve a feature matrix and actual verification results.
