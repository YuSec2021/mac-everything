# MacEverything branding

The mark combines a magnifying glass with three index lines, suggesting an E for
Everything. The app icon uses a teal-to-blue gradient; the menu bar uses a
monochrome template that adapts to the system appearance.

- `icon.svg`: editable vector icon.
- `logo.svg`: vector wordmark, with light/dark appearance support.
- `logo.png` / `logo-dark.png`: transparent wordmarks for README and sharing.
- `previews/about-light.png` / `previews/about-dark.png`: verified About window captures.
- `generate.swift`: reproducible AppKit renderer for all seven macOS icon sizes,
  the About icon, menu bar template, and wordmarks. No external dependencies.

Regenerate from the repository root:

```sh
swift assets/branding/generate.swift
```

The About window is available from **MacEverything → About MacEverything** or the
menu bar icon's right-click menu. It reads the version and build from the app's
Info.plist and links to the project and full changelog on GitHub.
