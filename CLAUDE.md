# PS4SX2 performance fork: rules for Claude Code

Read `HANDOFF.md` first. It has the full state, build history, and next steps.

- Never commit or push secrets (API keys, GitHub tokens, `GH_TOKEN`, private keys). Scan every diff before pushing. This is the user's explicit instruction.
- Performance work only. No firmware or kernel vulnerability research, and no clock or power-table changes.
- Every new feature is opt-in through a marker in `/data/PS4SX2/` and forced off in PS1 mode (`ps4/vm/main.cpp`).
- For each test build:
  - Make a new branch `perf-test-N` from the previous one.
  - Bump the `+perf-testN` build tag in `ps4/vm/main.cpp`.
  - Add a log or heartbeat line so the change can be measured.
- Builds run only in CI: `gh workflow run build.yml --ref perf-test-N -R hubwallpaper858-tech/ps4sx2`. The PKG artifact is named `PS4SX2-PKG-<run_number>`.
- Mark new code with `// (AI-assisted) perf-testN:` comments.
- Give the user, who is not a developer, plain-language results and exact steps: which artifact, which marker folders, which route to play, which logs to send.
