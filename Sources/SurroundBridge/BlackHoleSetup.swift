// The virtual audio device. Normally it's our own "Surround Bridge" device (SurroundBridge.driver,
// built from BlackHole's GPL-3 source, see driver/README.md), which the .pkg installs. If it's
// missing (e.g. the app was copied without the installer), the fallback is real BlackHole 2ch:
// Surround Bridge doesn't redistribute that (its official binaries and installers are "All rights
// reserved" by Existential Audio Inc.), so the app downloads the official installer from
// existential.audio, checks that it's signed by Existential Audio's Developer ID, and opens it.
import AppKit
import CryptoKit
import Foundation

@MainActor
final class BlackHoleSetup: ObservableObject {
    static let shared = BlackHoleSetup()

    enum Phase: Equatable {
        case idle
        case downloading
        case installerOpen            // BlackHole's installer is running; waiting for the driver to appear
        case needsAudioRestart        // the driver is installed but macOS audio hasn't loaded it yet
        case failed(String)
    }
    @Published private(set) var phase = Phase.idle

    static let downloadURL = URL(string: "https://existential.audio/downloads/BlackHole2ch-0.7.1.pkg")!
    static let pageURL = URL(string: "https://existential.audio/blackhole/")!
    /// 0.7.1 as published (the same file Homebrew's cask pins). A newer file is accepted when it's
    /// signed by Existential Audio's Developer ID instead.
    nonisolated static let knownSHA256 = "57b540f27a3e29c37e310e01bee0fdfab76733087e47f997ef9dccf851400dcf"
    nonisolated static let signer = "Developer ID Installer: Existential Audio Inc. (Q5C99V536K)"
    static let driverPath = "/Library/Audio/Plug-Ins/HAL/BlackHole2ch.driver"
    static let ownDriverPath = "/Library/Audio/Plug-Ins/HAL/SurroundBridge.driver"
    static let ownDeviceUID = "SurroundBridge_UID"

    static var driverInstalled: Bool { FileManager.default.fileExists(atPath: driverPath) }
    /// Our driver is on disk (but macOS audio may not have loaded it yet).
    static var ownDriverInstalled: Bool { FileManager.default.fileExists(atPath: ownDriverPath) }

    private var poll: Timer?

    func install() {
        phase = .downloading
        let task = URLSession.shared.downloadTask(with: Self.downloadURL) { tmp, response, error in
            let result: Result<URL, SetupError>
            if let tmp, (response as? HTTPURLResponse)?.statusCode == 200 {
                let dest = FileManager.default.temporaryDirectory.appendingPathComponent("BlackHole2ch.pkg")
                try? FileManager.default.removeItem(at: dest)
                do {
                    try FileManager.default.moveItem(at: tmp, to: dest)
                    result = Self.verify(dest) ? .success(dest)
                        : .failure(SetupError(message: "The downloaded installer isn't signed by Existential Audio, so it wasn't opened."))
                } catch {
                    result = .failure(SetupError(message: error.localizedDescription))
                }
            } else {
                result = .failure(SetupError(message: error?.localizedDescription ?? "The download didn't work."))
            }
            DispatchQueue.main.async { MainActor.assumeIsolated { BlackHoleSetup.shared.downloaded(result) } }
        }
        task.resume()
    }

    private func downloaded(_ result: Result<URL, SetupError>) {
        switch result {
        case .failure(let e):
            phase = .failed(e.message)
        case .success(let pkg):
            phase = .installerOpen
            NSWorkspace.shared.open(pkg)   // Installer.app: BlackHole's own welcome, licence, install
            poll?.invalidate()
            poll = Timer.scheduledTimer(withTimeInterval: 2, repeats: true) { _ in
                MainActor.assumeIsolated { BlackHoleSetup.shared.checkInstalled() }
            }
        }
    }

    private func checkInstalled() {
        guard Self.driverInstalled || Self.ownDriverInstalled else { return }
        poll?.invalidate()
        poll = nil
        // the driver is on disk; macOS audio picks it up after it restarts (BlackHole's installer
        // asks for a full restart; restarting coreaudiod is enough)
        phase = BridgeModel.shared.virtualDeviceUID == nil ? .needsAudioRestart : .idle
    }

    /// Restarts macOS's audio service (standard password prompt) so the new device appears.
    func restartAudio() {
        var error: NSDictionary?
        NSAppleScript(source: "do shell script \"launchctl kickstart -k system/com.apple.audio.coreaudiod\" with administrator privileges")?
            .executeAndReturnError(&error)
        phase = error == nil ? .idle : .needsAudioRestart
    }

    func openWebsite() { NSWorkspace.shared.open(Self.pageURL) }

    /// The known 0.7.1 file, or any installer signed with Existential Audio's Developer ID.
    nonisolated static func verify(_ pkg: URL) -> Bool {
        if let data = try? Data(contentsOf: pkg),
           SHA256.hash(data: data).map({ String(format: "%02x", $0) }).joined() == knownSHA256 { return true }
        let p = Process()
        p.executableURL = URL(fileURLWithPath: "/usr/sbin/pkgutil")
        p.arguments = ["--check-signature", pkg.path]
        let out = Pipe()
        p.standardOutput = out
        p.standardError = Pipe()
        guard (try? p.run()) != nil else { return false }
        p.waitUntilExit()
        let text = String(data: out.fileHandleForReading.readDataToEndOfFile(), encoding: .utf8) ?? ""
        return p.terminationStatus == 0 && text.contains(signer) && text.contains("Notarization: trusted")
    }
}

struct SetupError: Error { let message: String }
