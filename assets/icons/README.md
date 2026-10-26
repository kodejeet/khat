# Application icon assets

`icon.png` is the 2000x2000 source artwork.

Generated files:

- `gmail-notifier-16.png` through `gmail-notifier-1024.png` — PNG size ladder
  used by the hicolor theme installation
- `gmail-notifier.ico` — multi-resolution Windows icon (16, 24, 32, 48, 64, 128,
  and 256 pixels)
- `gmail-notifier.icns` — macOS icon container with the standard resolution set
  up to 1024 pixels

Regenerate every variant after changing `icon.png`:

```sh
python3 tools/generate-icons.py
```

The generator requires Python 3 and Pillow. The build itself uses the committed
PNG files and does not require Python or Pillow.
