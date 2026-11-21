import AppKit
import SwiftUI

@main
struct SurroundBridgeApp: App {
    @NSApplicationDelegateAdaptor(AppDelegate.self) var appDelegate
    @ObservedObject private var model = BridgeModel.shared

    var body: some Scene {
        MenuBarExtra {
            MenuView().environmentObject(model)
        } label: {
            Image(systemName: model.isRunning ? "hifispeaker.2.fill" : "hifispeaker.2")
        }
        .menuBarExtraStyle(.window)

        Settings {
            SettingsView().environmentObject(model)
        }
    }
}

final class AppDelegate: NSObject, NSApplicationDelegate {
    private var signalSources: [DispatchSourceSignal] = []

    func applicationDidFinishLaunching(_ notification: Notification) {
        // Ctrl-C / kill: give the HDMI device back and restore the Mac's sound output first
        for sig in [SIGINT, SIGTERM] {
            signal(sig, SIG_IGN)
            let src = DispatchSource.makeSignalSource(signal: sig, queue: .main)
            src.setEventHandler {
                MainActor.assumeIsolated { BridgeModel.shared.shutdown() }
                exit(0)
            }
            src.resume()
            signalSources.append(src)
        }
        _ = MainActor.assumeIsolated { BridgeModel.shared }   // start now (it may turn on at launch)
    }

    func applicationWillTerminate(_ notification: Notification) {
        MainActor.assumeIsolated { BridgeModel.shared.shutdown() }
    }
}

/// Removes the app (and optionally BlackHole) with the standard administrator prompt, then quits.
enum Uninstaller {
    @MainActor static func run(removeBlackHole: Bool) {
        BridgeModel.shared.enabled = false
        BridgeModel.shared.shutdown()
        try? SMAppServiceHelper.unregister()
        let app = Bundle.main.bundlePath
        var cmd = "rm -rf '\(app.replacingOccurrences(of: "'", with: "'\\''"))'"
        if removeBlackHole {
            cmd += " && rm -rf /Library/Audio/Plug-Ins/HAL/BlackHole2ch.driver && launchctl kickstart -k system/com.apple.audio.coreaudiod"
        }
        let script = "do shell script \"\(cmd.replacingOccurrences(of: "\\", with: "\\\\").replacingOccurrences(of: "\"", with: "\\\""))\" with administrator privileges"
        var error: NSDictionary?
        NSAppleScript(source: script)?.executeAndReturnError(&error)
        if error == nil {
            BridgeModel.defaults.removePersistentDomain(forName: Bundle.main.bundleIdentifier ?? "local.surroundbridge")
            NSApp.terminate(nil)
        }
    }
}

import ServiceManagement
enum SMAppServiceHelper {
    static func unregister() throws { if SMAppService.mainApp.status == .enabled { try SMAppService.mainApp.unregister() } }
}
