<h1 align="center">khat</h1>

<p align="center">
  <img src="assets/icons/icon.png" alt="khat icon" width="140" />
</p>

<p align="center"><b>Know your inbox without opening a browser.</b> khat sits
quietly in the GNOME session and shows a native desktop notification for new
Gmail mail — instantly on demand, because checks read Google's history feed
instead of waiting for the search index to catch up.</p>

<p align="center">
  <a href="#how-it-works">How it works</a> ·
  <a href="#install">Installation</a> ·
  <a href="#connect-your-gmail-account-one-time-api-setup">Connect your account</a> ·
  <a href="#usage">Usage</a> ·
  <a href="#security-and-privacy">Security &amp; Privacy</a> ·
  <a href="#documentation">Documentation</a> ·
  <a href="#contributing">Contributing</a>
</p>

<p align="center">
  <a href="#license"><img src="https://img.shields.io/badge/license-GPL--3.0--or--later-blue.svg" alt="License: GPL-3.0-or-later"></a>
  <img src="https://img.shields.io/badge/platform-Linux%20GNOME-blue.svg" alt="Platform: Linux GNOME">
  <img src="https://img.shields.io/badge/Qt-6.5%2B-41CD52.svg" alt="Qt 6.5 or newer">
  <img src="https://img.shields.io/badge/Ubuntu-24.10%2B-E95420.svg" alt="Ubuntu 24.10 or newer">
</p>

- **Small and native** — one C++/Qt 6 binary; no browser processes, no Electron,
  roughly 34 MiB idle RSS
- **Low-frequency, but instant on demand** — background checks default to every
  3 hours (configurable 5 min – 24 h), and a manual check is immediate because
  khat reads Gmail's change feed instead of waiting for search indexing
- **Privacy-first** — read-only access (`gmail.readonly`), tokens in the GNOME
  Keyring, no telemetry; the only network traffic goes to Google's own
  OAuth/Gmail endpoints
- **Multi-account** — authorize several Gmail accounts, notification shows
  sender, subject, and a short preview; click opens that exact message in Gmail
- **System-aware** — single-instance socket interface, suspend/resume handling,
  systemd user service autostart, and a small settings window

## How it works

khat runs as a small background application. On the first launch you complete
Google's OAuth consent screen in your browser (PKCE, localhost redirect); the
refresh token is stored in the GNOME Keyring. On every check — scheduled or
manual "Check Now" — khat asks Gmail's incremental **history feed** for inbox
messages that arrived since the previous check, downloads only sender /
subject / snippet metadata for unseen messages, and shows a native desktop
notification whose click action opens the message at `mail.google.com`. An
idle check is one small API request; full message bodies and attachments are
never downloaded.

Details of the internals: [docs/architecture.md](docs/architecture.md).

## Install

Per-user install, no root required — builds from source and starts the
systemd user service:

```sh
curl -fsSL https://github.com/kodejeet/khat/raw/main/install.sh | bash
```

<details>
<summary>Requirements and alternatives</summary>

- Ubuntu 24.10 or newer (Qt 6.5+). The script installs to `~/.local`
  and needs `build-essential`, `cmake`, and `qt6-base-dev`; it points out the
  package install if anything is missing.
- Git checkout alternative:

  ```sh
  git clone https://github.com/kodejeet/khat.git khat
  cd khat
  ./install.sh
  ```

- Remove again anytime:

  ```sh
  curl -fsSL https://github.com/kodejeet/khat/raw/main/uninstall.sh | bash
  ```

  (your configured accounts survive a plain uninstall; add `--purge` to erase
  configuration and state as well)
</details>

## Connect your Gmail account (one-time API setup)

khat talks to the Gmail API, so you need **one** credential of your own: an
OAuth client from your Google Cloud project. Setting it up takes roughly five
minutes:

1. Open <https://console.cloud.google.com/> and create a project (free).
2. Enable the **Gmail API**:
   <https://console.cloud.google.com/apis/library/gmail.googleapis.com>
3. Configure the OAuth consent screen (External is fine for private use) and
   add your own account as a test user.
4. Create credentials → **OAuth client ID → Desktop app**, download the JSON,
   and save it as `~/.config/khat/oauth-client.json`:

   ```sh
   mkdir -p ~/.config/khat
   mv ~/Downloads/client_secret_....json ~/.config/khat/oauth-client.json
   chmod 600 ~/.config/khat/oauth-client.json
   ```

5. Start khat and authorize: **Settings → Accounts → Add Account...**

```sh
~/.local/bin/khat --settings
```

While the Cloud project stays in **Testing** mode, Google expires the consent
after **7 days** — reauthorize weekly or publish the app for production. More
detail: [docs/google-oauth-setup.md](docs/google-oauth-setup.md).

## Usage

```sh
khat --settings     # open the settings window (safe to close; polling continues)
khat --background   # run as the background notifier (what the systemd unit does)
khat --check-now    # tell a running instance to check and notify immediately
```

Only one instance runs at a time; a second launch forwards to the first.

## Documentation

- [Architecture and internals](docs/architecture.md) — modules, sync/auth
  pipeline, storage schema, and the test strategy
- [Install, run, and remove](docs/installation.md) — manual build, per-user
  install, systemd autostart, update, complete uninstall
- [Google Cloud and OAuth setup](docs/google-oauth-setup.md) — Cloud project,
  Gmail API, desktop OAuth client, consent screen, testing-mode limits
- [Resource use, API cost, and troubleshooting](docs/operations.md) — request
  and quota estimates, memory/disk use, diagnostics, suspend/logout/DND notes
- [Notifications and Do Not Disturb](docs/notifications-and-dnd.md) — delivery,
  click behavior, and the GNOME DND limitation

## Security and Privacy

- **Read-only**: khat uses the `gmail.readonly` scope and requests message
  metadata and a short snippet only — full bodies and attachments are never
  fetched or stored.
- **No telemetry**: the only outbound requests go to Google's own endpoints
  (`accounts.google.com`, `oauth2.googleapis.com`, `gmail.googleapis.com`) over
  HTTPS; everything else stays on your device.
- **Secrets in the keyring**: OAuth tokens live in the GNOME Keyring and are
  never written to files or logs; state files have owner-only permissions, and
  removing an account revokes the grant.

## Contributing

Issues and pull requests are welcome — see
[CONTRIBUTING.md](CONTRIBUTING.md) and the
[Code of Conduct](CODE_OF_CONDUCT.md).

## License

khat is licensed under the **GNU GPL v3** (SPDX: `GPL-3.0-or-later`) — see
[LICENSE](LICENSE).
