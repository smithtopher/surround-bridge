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

The Mac app's installer (`SurroundBridge-1.0.0.pkg`) is built on a Mac with `scripts/make_installer.sh`
and goes in `macOS/`.
