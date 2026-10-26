# khat

A minimal native C++/Qt 6 notifier for new Gmail inbox messages on Ubuntu GNOME.
It polls the Gmail API with a low default frequency (3 hours), stores only the
state needed for deduplication, and delivers notifications through the standard
freedesktop notification service.

## Naming

The application is named **khat** everywhere: the executable, desktop entry,
systemd unit, icon, configuration/state/runtime paths, notification IDs, and
Secret Service namespace. The only retained historical identifier is the
immutable Google Cloud project ID `your-project-id`. The OAuth client ID,
API configuration, and `gmail.readonly` scope are unchanged.

Data and tokens created before the rename are not imported automatically.
Configure the OAuth client under `~/.config/khat` and authorize the account
again.

## Status

The background application, OAuth account flow, Secret Service token storage,
Gmail synchronization, native notifications, single-instance command socket,
suspend/resume handling, and minimal settings window are implemented. Automated
tests use local OAuth/Gmail fakes and a private-bus Secret Service fake. On
2026-10-27, OAuth consent, token exchange, Secret Service storage, and live
read-only Gmail synchronization were verified with a locally supplied Desktop
client and explicit user consent. You must supply your own Desktop OAuth client
and authorize each account; until then the notifier starts idle and never
contacts Gmail. See `PROGRESS.md` for tested, verified, and remaining work.

## Requirements

- Ubuntu 24.10 or newer with GNOME (verified on Ubuntu 26.04.1 with GNOME
  Shell 50.1; Ubuntu 24.04's default Qt 6.4 packages are too old)
- Qt 6.5+ development packages (Core, Gui, Network, Sql, Widgets, DBus, Test)
- CMake 3.21+
- A C++20 compiler
- A Google Cloud project with the Gmail API enabled and a Desktop OAuth client
  (credentials are never stored in this repository)

## Build

```sh
cmake -S . -B build -DCMAKE_BUILD_TYPE=Debug
cmake --build build
ctest --test-dir build --output-on-failure
```

## Run

```sh
# Open the settings window (the window can be closed; polling continues)
./build/khat --settings

# Run without a window, as used by the systemd user service
./build/khat --background

# Ask a running instance for an immediate check
./build/khat --check-now
```

Only one notifier instance runs at a time. A second launch talks to the existing
process through a private socket in the user runtime directory. To connect an
account, prepare the OAuth client file described in the setup guide, open
**Settings → Accounts**, choose **Add Account...**, and complete Google's consent
screen. The refresh token is stored in Secret Service. Manual checks report that
no authorized accounts exist until at least one account is connected.

## Documentation

- [Install, run, and remove](docs/installation.md) — packages, build, per-user
  install, systemd autostart, update, and complete uninstall
- [Google Cloud and OAuth setup](docs/google-oauth-setup.md) — Cloud project,
  Gmail API, Desktop OAuth client, consent screen, testing/verification limits
- [Resource use, API cost, and troubleshooting](docs/operations.md) — request
  and quota estimates, memory/disk use, diagnostics, suspend/logout/DND notes
- [Notifications and Do Not Disturb](docs/notifications-and-dnd.md) — delivery,
  click behavior, and the GNOME 50.1 DND limitation
- [GNOME Online Accounts investigation](docs/goa-investigation.md) — why GOA
  tokens are not used
- [Application icon assets](assets/icons/README.md) — source artwork, installed
  PNG size ladder, and Windows/macOS icon containers

## Security

- OAuth client configuration stays in a mode-0600 local file; refresh tokens
  live in Secret Service and are never written to the repository or `config.json`.
- Only Gmail metadata and short previews are requested.
- No credentials, tokens, email bodies, or email subjects are written to logs.
- Real Google authorization and real keyring writes are not exercised by the
  automated test suite.

## License

Not yet specified.
