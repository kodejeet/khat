# Application icon assets

`icon.png` is the 2000x2000 source artwork. Generated files:

- `khat-16.png` through `khat-1024.png` — PNG size ladder
  used by the hicolor theme installation
- `khat.ico` — multi-resolution Windows icon (16, 24, 32, 48, 64, 128,
  and 256 pixels)
- `khat.icns` — macOS icon container with the standard resolution set
  up to 1024 pixels

Regenerate every variant after changing `icon.png` (the icon generator script
is part of the maintainer's local tooling, not this repository; the committed
PNG files and the build do not require it):
