# Install, run, and remove khat

This guide covers a per-user installation on Ubuntu GNOME. It does not install a
system-wide daemon and does not require root after the build dependencies are
present. The application runs as one process in the user's graphical session and
uses a user-level systemd service for autostart.

The application, executable, desktop entry, systemd unit, icon, and local data
paths all use the name `khat`. Configuration and tokens created before the
rename are not imported; configure and authorize the account again as described
below. The only retained historical identifier is the immutable Google Cloud
project ID `your-project-id`.

Verified on Ubuntu 26.04.1 LTS, GNOME Shell 50.1, Qt 6.10.2, CMake 4.2.3, and
GCC 15.2. The build requires Qt 6.5 or newer and CMake 3.21 or newer; Ubuntu
24.10 or newer is expected to satisfy those requirements. Ubuntu 24.04's default
Qt 6.4 packages are too old.

## Current status

The build, OAuth account management, Secret Service token storage, Gmail
synchronization, native notifications, settings window, background process,
single-instance socket, and suspend/offline handling are implemented. Automated
tests use local fakes and a private-bus Secret Service fake. On 2026-10-27,
OAuth consent, token exchange, keyring storage, and live read-only Gmail
synchronization were verified with a locally supplied Desktop client and
explicit user consent. The application starts idle until you provide an OAuth
client and authorize an account. The OAuth client file described below cannot
authorize an account by itself.

## 1. Install build dependencies

```sh
sudo apt update
sudo apt install build-essential cmake qt6-base-dev desktop-file-utils
```

`qt6-base-dev` provides Qt Core, GUI, Widgets, Network, SQL, DBus, and Test.
`dbus-daemon` provides `dbus-run-session` for the D-Bus tests and is present on
a normal Ubuntu GNOME installation. The application uses QtDBus for Secret
Service support, so no `libsecret` development package is required.

Do not install the application with `sudo`. The commands below put it under
`~/.local`.

## 2. Build and test

From the repository root:

```sh
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release \
    -DCMAKE_INSTALL_PREFIX="$HOME/.local"
cmake --build build
ctest --test-dir build --output-on-failure
```

The default check interval is 180 minutes, and the test suite never contacts
Google. The Gmail tests use a loopback fake server. Notification tests use a
private or fake D-Bus service, and the full suite records the actual result in
`PROGRESS.md`.

If `dbus-run-session` is unavailable, the two D-Bus test executables are not
registered by CMake; all other tests still run. Install `dbus-daemon` first if
you want the complete suite.

## 3. Install for the current user

Keep the same install prefix used for configuration; the desktop file and
systemd unit contain the absolute installed binary path:

```sh
cmake --install build
```

The default per-user installation creates:

- `~/.local/bin/khat`
- `~/.local/share/applications/khat.desktop`
- `~/.local/share/systemd/user/khat.service`
- `~/.local/share/icons/hicolor/<size>x<size>/apps/khat.png` for
  16, 22, 24, 32, 48, 64, 128, 256, and 512 pixel icons

The desktop entry and notifications use the icon name `khat`, which
resolves through the hicolor theme. The source artwork and the Windows `.ico`
and macOS `.icns` containers are kept in `assets/icons`; the GNOME install rules
install only the PNG size ladder.

`~/.local/bin` is normally on `PATH` on Ubuntu. Run `hash -r` in an already open
shell after the first install. The systemd unit is installed but not enabled.
The unit lives under `~/.local/share/systemd/user`, which is in systemd's user
unit search path.

## 4. Prepare the OAuth client file

The application reads a local OAuth client file separate from account tokens:

```sh
install -d -m 700 "$HOME/.config/khat"
install -m 600 config/oauth-client.example.json \
    "$HOME/.config/khat/oauth-client.json"
${EDITOR:-vi} "$HOME/.config/khat/oauth-client.json"
```

Replace every placeholder with values from a Google Cloud **Desktop app** OAuth
client. Alternatively, save Google's downloaded Desktop client JSON as
`oauth-client.json`; the loader accepts its `{"installed": {...}}` wrapper and
supplies the default `revoke_uri` and read-only scope when they are absent. Keep
the scope exactly:

```
https://www.googleapis.com/auth/gmail.readonly
```

Never commit the filled-in file, access tokens, or refresh tokens. Desktop OAuth
client secrets are not confidential in the same way as server secrets, but they
are still kept in a mode-0600 file and must not be logged. See
[Google Cloud and OAuth setup](google-oauth-setup.md) for the complete procedure.

## 5. Run

Start the settings window:

```sh
khat --settings
```

Closing the window does not stop polling. A second launch of `--settings` asks
the existing process to show its window instead of starting another scheduler.

Other modes:

```sh
# Windowless process, used by the systemd user service
khat --background

# Request an immediate check from the running process
khat --check-now

khat --version
```

`--settings`, `--background`, and `--check-now` are mutually exclusive. The
application makes an immediate check on startup, but the first check for each
account establishes a baseline and does not notify about old mail. A successful
later check uses a 24-hour overlap plus message-ID deduplication.

## 6. Start automatically in the graphical session

The installed service is disabled by default. Enable it after the OAuth client
and account are configured:

```sh
systemctl --user daemon-reload
systemctl --user enable --now khat.service
systemctl --user status khat.service
```

The service runs `khat --background` after
`graphical-session.target`, is stopped with that target at logout, and restarts
on failure. It does not require root. The desktop menu entry can be used at the
same time; the second process forwards a request to the running service instead
of creating a duplicate notifier.

Useful commands:

```sh
systemctl --user is-enabled khat.service
systemctl --user is-active khat.service
systemctl --user restart khat.service
systemctl --user stop khat.service
journalctl --user -u khat.service -b --no-pager
```

If the service cannot find a graphical display, make sure the graphical session
is active and the user manager has the display variables:

```sh
systemctl --user show-environment | grep -E 'DISPLAY|WAYLAND_DISPLAY|DBUS'
systemctl --user import-environment DISPLAY WAYLAND_DISPLAY XAUTHORITY
```

The second command is only needed for unusual sessions that do not import the
display environment; it changes the current user manager environment, not the
system configuration. On the tested Ubuntu GNOME session these variables were
already present.

## 7. Settings and files

All paths are per-user:

| Purpose | Path |
| --- | --- |
| Application settings | `~/.config/khat/config.json` |
| OAuth client | `~/.config/khat/oauth-client.json` |
| SQLite state and dedupe | `~/.local/share/khat/state.db` |
| OAuth tokens | Secret Service, not a file in these directories |
| Runtime lock and socket | `$XDG_RUNTIME_DIR/khat/` |
| Desktop entry | `~/.local/share/applications/khat.desktop` |
| Application icons | `~/.local/share/icons/hicolor/*/apps/khat.png` |
| systemd user unit | `~/.local/share/systemd/user/khat.service` |

Settings are saved atomically. The polling interval accepts 5–1440 minutes and
defaults to 180. Updates can be included or excluded, previews can be disabled,
and notifications can be turned off without deleting synchronization state.
Only message IDs and notification bookkeeping are persisted; bodies and
attachments are never stored.

The state directory is mode-restricted by the application. The single-instance
socket lives in a mode-0700 runtime directory with a user-only socket. Do not
loosen these permissions to make commands work.

Refresh tokens are stored as Secret Service items belonging to
`khat`. Keep the login keyring unlocked while the app runs; if Secret
Service is missing or locked, token loading fails and the account is treated as
unauthorized until the keyring is available. Never copy token values into a
terminal, log, or bug report.

## 8. Update an existing installation

Stop the service before replacing the binary, then install again:

```sh
systemctl --user stop khat.service
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release \
    -DCMAKE_INSTALL_PREFIX="$HOME/.local"
cmake --build build
ctest --test-dir build --output-on-failure
cmake --install build
systemctl --user daemon-reload
systemctl --user start khat.service
```

Application settings, OAuth configuration, OAuth tokens, and SQLite state are
not removed by an upgrade. Back up the two configuration files and the state
database if the account history matters. Data from before the khat rename is
not detected or migrated; configure the OAuth client and authorize accounts
again under the khat paths.

## 9. Remove the application completely

Remove accounts through the settings window first. Removing an account deletes
its local synchronization state and its Secret Service token, and the
application attempts to revoke the Google grant before forgetting it. If
revocation cannot be completed, local cleanup still proceeds and the app reports
a warning.

Then stop and disable autostart:

```sh
systemctl --user disable --now khat.service
systemctl --user daemon-reload
systemctl --user reset-failed khat.service
```

Remove the installed program and launchers:

```sh
rm -f "$HOME/.local/bin/khat"
rm -f "$HOME/.local/share/applications/khat.desktop"
rm -f "$HOME/.local/share/icons/hicolor/"*/apps/khat.png
rm -f "$HOME/.local/share/systemd/user/khat.service"
rmdir --ignore-fail-on-non-empty "$HOME/.local/share/systemd/user" 2>/dev/null || true
```

Remove application data, including the SQLite database, its WAL/SHM files, the
OAuth client file, and any stale runtime socket:

```sh
rm -rf "$HOME/.config/khat"
rm -rf "$HOME/.local/share/khat"
rm -rf "${XDG_RUNTIME_DIR:-$HOME/.cache}/khat"
```

Do not use a broad `rm -rf` on a parent directory. The paths above are specific
to this application and contain no other application's data.

Finally, remove the Google grant and any remaining credential-manager items:

1. Visit <https://myaccount.google.com/permissions>, select the khat OAuth
   client, and remove access. This invalidates refresh tokens even if a local
   copy was missed.
2. If the app is already gone, open GNOME **Passwords and Keys** (Seahorse) and
   delete entries labelled for khat. Do not print token values to the terminal.
3. Optionally delete the Google Cloud project or OAuth client if no other
   application uses it. This does not affect local files.

The uninstall does not remove packages such as Qt or `libsecret`, because other
applications may use them. It also does not remove the `~/.local` prefix.

If the application was installed under `/usr/local` instead, the installed paths
are `/usr/local/bin/khat`,
`/usr/local/share/applications/khat.desktop`,
`/usr/local/share/icons/hicolor/*/apps/khat.png`, and
`/usr/local/share/systemd/user/khat.service`; removing those requires
the corresponding administrative permission. The recommended per-user install
never needs root.

## Related documentation

- [Google Cloud and OAuth setup](google-oauth-setup.md)
- [Resource use, API cost, and troubleshooting](operations.md)
- [Notifications and Do Not Disturb](notifications-and-dnd.md)
- [GNOME Online Accounts investigation](goa-investigation.md)
