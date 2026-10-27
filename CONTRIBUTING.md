# Contributing to khat

Thanks for your interest in making khat better! This document explains how to
report problems and contribute code.

## Reporting issues

Open a GitHub issue and include:

- Ubuntu version, GNOME Shell version, and Qt version (`qt6 --version` or the
  package list)
- How you installed khat (`install.sh`, manual build, ...)
- What you expected, what happened instead, and rough reproduction steps

For logs, use `journalctl --user -u khat.service -b --no-pager` (the app also
logs to stdout when run manually). **Never attach private data**: access or
refresh tokens, OAuth client secrets, email addresses, subjects, snippets, or
bodies. The app is written so its logs should not contain these — please keep
it that way also in your bug report.

## Building and testing locally

```sh
sudo apt install build-essential cmake qt6-base-dev   # Ubuntu 24.10+
cmake -S . -B build -DCMAKE_BUILD_TYPE=Debug
cmake --build build -j4
ctest --test-dir build --output-on-failure
```

Full setup of a Google OAuth client is documented in
[docs/google-oauth-setup.md](docs/google-oauth-setup.md); you need your own
Desktop OAuth client file at `~/.config/khat/oauth-client.json` before the app
contacts Gmail.

## Pull requests

- Keep one logical change per pull request; describe *why*, not just *what*.
- Every behavior change should come with tests. Behavior lives in small,
  asynchronously wired Qt classes; tests use QtTest and inject doubles
  (see `tests/support/` and `docs/architecture.md` — Testing strategy).
- Never contact real Google endpoints from tests; extend
  `tests/support/FakeGmailServer.h` instead.
- Follow the surrounding style: C++20, Qt idioms (objects connect Qt signals
  or lambdas, `QStringLiteral`, early-out guards), no worker threads without
  a very good reason, no secrets in logs.
- Update documentation when behavior changes: README, `docs/*.md`.

Commit messages use a small prefix convention, e.g.
`sync: use the history feed for regular checks`, `docs: ...`, `app: ...`,
`auth: ...`, `tests: ...`.

## Code of Conduct

By participating in this project you agree to the
[Code of Conduct](CODE_OF_CONDUCT.md).

## Licensing

khat is GPL-3.0-or-later (see [LICENSE](LICENSE)). By contributing you agree
your contributions are licensed under the same terms.

## What lives in the repository (and what does not)

The public repository ships everything users and contributors need: sources,
tests, `install.sh`/`uninstall.sh`, docs, licensing. The maintainer keeps
private working notes (`PROGRESS.md`, `SPEC.md`, `docs/goa-investigation.md`)
out of the repository; if you wonder about the rationale for a decision, ask
in an issue — decisions with user impact are documented in `docs/`.
