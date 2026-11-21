// The menu-bar popover: on/off, what's happening, levels, the upmix mode and main sliders.
import AppKit
import SwiftUI

struct MenuView: View {
    @EnvironmentObject var model: BridgeModel

    var body: some View {
        VStack(alignment: .leading, spacing: 12) {
            HStack(spacing: 10) {
                Image(systemName: "hifispeaker.2.fill").font(.title2).foregroundStyle(model.isRunning ? Color.accentColor : .secondary)
                VStack(alignment: .leading, spacing: 1) {
                    Text("Surround Bridge").font(.headline)
                    HStack(spacing: 5) {
                        Circle().fill(dotColor).frame(width: 7, height: 7)
                        Text(model.statusLine).font(.caption).foregroundStyle(.secondary).lineLimit(2)
                    }
                }
                Spacer()
                Toggle("", isOn: $model.enabled).toggleStyle(.switch).labelsHidden()
                    .help(model.enabled ? "Turn off: the Mac's sound goes back to where it was" : "Turn on: everything plays through your amp as surround")
            }

            problem

            if model.isRunning {
                VStack(spacing: 4) {
                    MeterRow(label: "L", value: model.meter(0))
                    MeterRow(label: "R", value: model.meter(1))
                }
            }

            Divider()
            UpmixQuickControls()

            Divider()
            HStack {
                SettingsLink { Text("Settings…") }
                    .simultaneousGesture(TapGesture().onEnded { NSApp.activate(ignoringOtherApps: true) })
                Spacer()
                Button("Quit") { NSApp.terminate(nil) }
            }
            .buttonStyle(.borderless)
        }
        .padding(14)
        .frame(width: 340)
    }

    private var dotColor: Color {
        switch model.state {
        case .running: return .green
        case .starting: return .yellow
        case .off: return .secondary
        default: return .orange
        }
    }

    /// Plain-language help for whatever is stopping it.
    @ViewBuilder private var problem: some View {
        switch model.state {
        case .needsPermission:
            Notice(text: "Surround Bridge listens to the virtual audio device that apps play into, which macOS treats like a microphone. Allow it in System Settings ▸ Privacy & Security ▸ Microphone.",
                   button: "Open Privacy Settings") {
                NSWorkspace.shared.open(URL(string: "x-apple.systempreferences:com.apple.preference.security?Privacy_Microphone")!)
            }
        case .noVirtualDevice:
            VirtualDeviceSetup()
        case .noOutput:
            Notice(text: "Connect your TV, receiver or soundbar (HDMI or optical) and switch it on; Surround Bridge starts by itself when it appears.",
                   button: nil, action: {})
        case .failed:
            Notice(text: "It couldn't start. Check that nothing else is using the HDMI/optical output exclusively, then turn it off and on again.",
                   button: nil, action: {})
        default:
            EmptyView()
        }
    }
}

/// One-time setup of the virtual audio device apps play into: BlackHole 2ch, from its official
/// installer (see BlackHoleSetup).
struct VirtualDeviceSetup: View {
    @ObservedObject private var setup = BlackHoleSetup.shared

    var body: some View {
        switch setup.phase {
        case .idle:
            Notice(text: "One more thing: Surround Bridge needs BlackHole 2ch, a free virtual audio device that your apps play into. Its official installer will open; it takes a minute.",
                   button: "Install BlackHole…") { setup.install() }
        case .downloading:
            HStack(spacing: 8) { ProgressView().controlSize(.small); Text("Downloading BlackHole's installer…").font(.caption) }
        case .installerOpen:
            Notice(text: "Finish BlackHole's installer (it asks for your password). Surround Bridge notices when it's done.",
                   button: nil, action: {})
        case .needsAudioRestart:
            Notice(text: "BlackHole is installed. macOS needs to restart its audio once to see it (sound stops for a second).",
                   button: "Restart macOS Audio") { setup.restartAudio() }
        case .failed(let why):
            Notice(text: "\(why) You can also download BlackHole 2ch from its website.", button: "Open BlackHole Website") { setup.openWebsite() }
        }
    }
}

struct Notice: View {
    let text: String
    let button: String?
    let action: () -> Void
    var body: some View {
        VStack(alignment: .leading, spacing: 6) {
            Text(text).font(.caption).fixedSize(horizontal: false, vertical: true)
            if let button { Button(button, action: action).controlSize(.small) }
        }
        .padding(8)
        .frame(maxWidth: .infinity, alignment: .leading)
        .background(Color.orange.opacity(0.12), in: RoundedRectangle(cornerRadius: 6))
    }
}

struct MeterRow: View {
    let label: String
    let value: Double
    var body: some View {
        HStack(spacing: 6) {
            Text(label).font(.caption2.monospaced()).foregroundStyle(.secondary).frame(width: 10)
            GeometryReader { g in
                ZStack(alignment: .leading) {
                    Capsule().fill(Color.secondary.opacity(0.15))
                    Capsule().fill(value > 0.95 ? Color.red : value > 0.8 ? Color.yellow : Color.green)
                        .frame(width: g.size.width * value)
                        .animation(.linear(duration: 0.15), value: value)
                }
            }
            .frame(height: 6)
        }
    }
}

/// Mode + the four main sliders (the full set, with Advanced, is in Settings ▸ Upmix).
struct UpmixQuickControls: View {
    @EnvironmentObject var model: BridgeModel

    var body: some View {
        VStack(alignment: .leading, spacing: 8) {
            Picker("Stereo upmix", selection: UpmixBindings(model: model).mode) {
                ForEach(UpmixSettings.Mode.allCases) { Text($0.title).tag($0) }
            }
            Text(model.upmix.mode.blurb).font(.caption).foregroundStyle(.secondary).fixedSize(horizontal: false, vertical: true)
            if model.upmix.enabled {
                UpmixSliders(compact: true)
            }
        }
    }
}
