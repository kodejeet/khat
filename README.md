# Gmail Notifier for GNOME

A minimal native C++/Qt 6 notifier for new Gmail inbox messages on Ubuntu GNOME.
It polls the Gmail API with a low default frequency (3 hours), stores only the
state needed for deduplication, and delivers notifications through the standard
freedesktop notification service.

## Status

Scaffold only. Implementation is proceeding incrementally; see `PROGRESS.md`
for completed, tested, pending, and blocked work.

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

Run the scaffold:

```sh
QT_QPA_PLATFORM=offscreen ./build/gmail-notifier --version
```

## Security

- OAuth client configuration and refresh tokens are kept out of the repository.
- Only Gmail metadata and short previews are requested.
- No credentials, tokens, or email content are written to logs.
- Real Google authorization is not exercised by the automated test suite.

## License

Not yet specified.
