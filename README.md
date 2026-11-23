# Surround Bridge releases

Downloads only; the source is on [`main`](https://github.com/smithtopher/surround-bridge/tree/main).

## Windows

| File | Version | Built from | SHA-256 |
|---|---|---|---|
| [Windows/SurroundBridge-Setup-1.0.0-windows-x64.exe](Windows/SurroundBridge-Setup-1.0.0-windows-x64.exe) | 1.0.0 | main 4086eef | `1c92c37c6d3d726c2fd34a6ca01e8bdf1d11ce5dfcc4acc3b5bf1d940016fcd5` |

Windows 10/11, 64-bit, ~70 MB, nothing else to install. Open it and click **Install** (per user,
no administrator rights). It isn't code-signed yet: if SmartScreen warns, **More info** → **Run anyway**.
See the [README](https://github.com/smithtopher/surround-bridge/blob/main/README.md#install-windows-1011-64-bit)
and the [changelog](https://github.com/smithtopher/surround-bridge/blob/main/CHANGELOG.md).

## macOS

| File | Version | Built from | SHA-256 |
|---|---|---|---|
| [macOS/SurroundBridge-1.1.0.pkg](macOS/SurroundBridge-1.1.0.pkg) | 1.1.0 | main 6f8c490 | `b7395128c62fe6b9c7dae63019a68c54d74a486fbf77f04d093fe8204f52d9d2` |

macOS 14+, Apple Silicon and Intel (universal), 16 MB. It installs the app in Applications and the
**Surround Bridge audio device** (built from BlackHole 0.7.1's GPL-3 source, see
[driver/](https://github.com/smithtopher/surround-bridge/tree/main/driver)), so there's nothing else to
install. It isn't signed by a registered developer yet: the first time, right-click it in Finder →
**Open** → **Open**. See the
[README](https://github.com/smithtopher/surround-bridge/blob/main/README.md#install-macos-14) and the
[changelog](https://github.com/smithtopher/surround-bridge/blob/main/CHANGELOG.md).
