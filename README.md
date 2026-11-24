# Surround Bridge releases

Downloads only; the source is on [`main`](https://github.com/smithtopher/surround-bridge/tree/main).

## Windows

| File | Version | Built from | SHA-256 |
|---|---|---|---|
| [Windows/SurroundBridge-Setup-1.3.2-windows-x64.exe](Windows/SurroundBridge-Setup-1.3.2-windows-x64.exe) | 1.3.2 | main 4869729 | `680843cd862979a51a712959696ac8b57c9075975a7ee74aefa7e349e96efcf7` |

Windows 10/11, 64-bit, ~70 MB, nothing else to install. Open it and click **Install** (per user,
no administrator rights). It isn't code-signed yet: if SmartScreen warns, **More info** → **Run anyway**.
See the [README](https://github.com/smithtopher/surround-bridge/blob/main/README.md#install-windows-1011-64-bit)
and the [changelog](https://github.com/smithtopher/surround-bridge/blob/main/CHANGELOG.md).

## macOS

| File | Version | Built from | SHA-256 |
|---|---|---|---|
| [macOS/SurroundBridge-1.2.0.pkg](macOS/SurroundBridge-1.2.0.pkg) | 1.2.0 | main eee1ef9 | `bf5eccdb08255c7b91025fa8c08b8690cce2af6958d436b45fe1c87b2f72310d` |

macOS 14+, Apple Silicon and Intel (universal), 16 MB. It installs the app in Applications and the
**Surround Bridge audio device** (built from BlackHole 0.7.1's GPL-3 source, see
[driver/](https://github.com/smithtopher/surround-bridge/tree/main/driver)), so there's nothing else to
install. It isn't signed by a registered developer yet: the first time, right-click it in Finder →
**Open** → **Open**. See the
[README](https://github.com/smithtopher/surround-bridge/blob/main/README.md#install-macos-14) and the
[changelog](https://github.com/smithtopher/surround-bridge/blob/main/CHANGELOG.md).
