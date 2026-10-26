# Gmail Notifier for GNOME

A minimal native C++/Qt 6 notifier for new Gmail inbox messages on Ubuntu GNOME.
It polls the Gmail API with a low default frequency (3 hours), stores only the
state needed for deduplication, and delivers notifications through the standard
freedesktop notification service.

## Status

The background application, Gmail synchronization, native notifications,
single-instance command socket, suspend/resume handling, and minimal settings
window are implemented. OAuth authorization and secure token storage are not
implemented yet, so the notifier currently starts idle and never contacts Gmail.
See `PROGRESS.md` for tested, pending, and blocked work.

## Requirements

- Ubuntu 24.04 or newer with GNOME
- Qt 6 development packages (Core, Gui, Network, Sql, Widgets, DBus, Test)
- CMake 3.21+
- A C++20 compiler
- A Google Cloud project with the Gmail API enabled and a Desktop OAuth client
  (documented later; credentials are never stored in this repository)

## Build

```sh
cmake -S . -B build -DCMAKE_BUILD_TYPE=Debug
cmake --build build
ctest --test-dir build --output-on-failure
```

## Run

```sh
# Open the settings window (the window can be closed; polling continues)
./build/gmail-notifier --settings

# Run without a window, as used by the systemd user service
./build/gmail-notifier --background

# Ask a running instance for an immediate check
./build/gmail-notifier --check-now
```

Only one notifier instance runs at a time. A second launch talks to the existing
process through a private socket in the user runtime directory. Account
authorization is not available yet, so manual checks report that no authorized
accounts exist.

## Security

- OAuth client configuration and refresh tokens are kept out of the repository.
- Only Gmail metadata and short previews are requested.
- No credentials, tokens, or email content are written to logs.
- Real Google authorization is not exercised by the automated test suite.

## License

Not yet specified.
