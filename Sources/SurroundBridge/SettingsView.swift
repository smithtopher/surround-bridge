// Settings window: General (output, codec), Upmix (the same controls as Surround Player), About.
import AppKit
import SwiftUI

struct SettingsView: View {
    var body: some View {
        TabView {
            GeneralSettings().tabItem { Label("General", systemImage: "gearshape") }
            UpmixSettingsView().tabItem { Label("Upmix", systemImage: "hifispeaker.2") }
            AboutView().tabItem { Label("About", systemImage: "info.circle") }
        }
        .frame(width: 520)
        .onAppear { NSApp.activate(ignoringOtherApps: true) }
    }
}

private func note(_ text: String) -> some View {
    Text(text).font(.caption).foregroundStyle(.secondary).fixedSize(horizontal: false, vertical: true)
}

struct GeneralSettings: View {
    @EnvironmentObject var model: BridgeModel

    var body: some View {
        Form {
            Section {
                Picker("Send to", selection: $model.outputUID) {
                    if model.outputs.isEmpty { Text("No HDMI or optical output connected").tag(model.outputUID) }
                    ForEach(model.outputs) { o in Text(o.name + (o.isHDMI ? " (HDMI)" : "")).tag(o.uid) }
                }
                note("The HDMI or optical output your TV, receiver or soundbar is connected to. Surround Bridge uses it exclusively while it's on.")
                Toggle("Switch the Mac's sound to Surround Bridge while it's on", isOn: $model.autoSwitchOutput)
                note("Everything that plays then goes through the bridge. Turning it off puts the previous output back.")
            } header: { Text("Output") }

            Section {
                Picker("Format", selection: $model.codec) {
                    Text("Dolby Digital (AC3)").tag(0)
                    Text("DTS").tag(1)
                }
                if model.codec == 0 {
                    Picker("Dolby Digital bitrate", selection: $model.ac3Bitrate) {
                        Text("384 kb/s").tag(384)
                        Text("448 kb/s (DVD)").tag(448)
                        Text("640 kb/s (best)").tag(640)
                    }
                } else {
                    Picker("DTS bitrate", selection: $model.dtsBitrate) {
                        Text("1509 kb/s (full rate, best)").tag(1509)
                        Text("754 kb/s (half rate)").tag(754)
                    }
                    note("Some TVs don't pass DTS on to an amp over optical. If the amp stays silent, use Dolby Digital.")
                }
            } header: { Text("Surround format") }

            Section {
                Toggle("Start Surround Bridge when you log in", isOn: $model.launchAtLogin)
                if let e = model.lastError { note(e) }
            }
        }
        .formStyle(.grouped)
    }
}

struct UpmixSettingsView: View {
    @EnvironmentObject var model: BridgeModel

    var body: some View {
        Form {
            Section {
                Picker("Upmix stereo to 5.1", selection: UpmixBindings(model: model).mode) {
                    ForEach(UpmixSettings.Mode.allCases) { Text($0.title).tag($0) }
                }
                note(model.upmix.mode.blurb)
                if model.upmix.enabled {
                    UpmixSliders(compact: false)
                }
                note("Changes apply while it plays. Sound that's already 5.1 (from apps that output it) isn't upmixed.")
            } header: { Text("Stereo Upmix") }
        }
        .formStyle(.grouped)
    }
}

/// Bindings onto model.upmix (copied from Surround Player's SettingsView): a preset sets the
/// sliders; moving a slider turns a preset into Custom; a dB slider's left end means off.
/// SwiftUI calls binding closures on the main thread.
@MainActor
struct UpmixBindings {
    let model: BridgeModel

    var mode: Binding<UpmixSettings.Mode> {
        let model = self.model
        return Binding(get: { MainActor.assumeIsolated { model.upmix.mode } }, set: { m in
            MainActor.assumeIsolated {
                var u = model.upmix
                u.mode = m
                if m != .custom { u.apply(m) }
                model.upmix = u
            }
        })
    }

    func up<T>(_ kp: WritableKeyPath<UpmixSettings, T>, custom: Bool = true) -> Binding<T> {
        let model = self.model
        return Binding(get: { MainActor.assumeIsolated { model.upmix[keyPath: kp] } }, set: { v in
            MainActor.assumeIsolated {
                var u = model.upmix
                u[keyPath: kp] = v
                if custom, u.mode != .off { u.mode = .custom }
                model.upmix = u
            }
        })
    }

    func dbOff(_ kp: WritableKeyPath<UpmixSettings, Double>) -> Binding<Double> {
        let b = up(kp)
        return Binding(get: { max(-20, b.wrappedValue) }, set: { b.wrappedValue = $0 <= -20 ? -40 : $0 })
    }
}

struct UpmixSliders: View {
    @EnvironmentObject var model: BridgeModel
    let compact: Bool

    var body: some View {
        let b = UpmixBindings(model: model)
        Group {
            slider("Surround", b.dbOff(\.surroundDb), -20...6, step: 0.5) { $0 <= -20 ? "Off" : String(format: "%+.1f dB", $0) }
            slider("Centre focus", b.up(\.center), 0...1, step: 0.05) {
                $0 < 0.05 ? "Phantom" : $0 > 0.95 ? "Locked" : String(format: "%.0f%%", $0 * 100)
            }
            slider("Front width", b.up(\.width), 0...1, step: 0.05) {
                abs($0 - 0.5) < 0.01 ? "As recorded" : String(format: "%.0f%%", $0 * 100)
            }
            slider("Subwoofer", b.dbOff(\.lfeDb), -20...6, step: 0.5) { $0 <= -20 ? "Off" : String(format: "%+.1f dB", $0) }
            if !compact {
                DisclosureGroup("Advanced") {
                    Picker("Bass crossover", selection: b.up(\.crossoverHz)) {
                        ForEach([60, 80, 100, 120, 150, 200, 250], id: \.self) { Text("\($0) Hz").tag($0) }
                    }
                    Picker("Bass to subwoofer", selection: b.up(\.lfeMove)) {
                        Text("Copy (main speakers keep it)").tag(false)
                        Text("Move (small main speakers)").tag(true)
                    }
                    slider("Bass boost", b.up(\.bassDb), 0...12, step: 0.5) { $0 < 0.25 ? "Off" : String(format: "+%.1f dB", $0) }
                    slider("Rear delay", b.up(\.rearDelayMs), 0...30, step: 1) { $0 < 0.5 ? "Off" : String(format: "%.0f ms", $0) }
                    Picker("Rear high-cut", selection: b.up(\.rearHighcutHz)) {
                        Text("Off").tag(0)
                        ForEach([4000, 6000, 7000, 8000, 10000, 12000, 16000], id: \.self) { Text("\($0 / 1000) kHz").tag($0) }
                    }
                    note("""
                    Crossover: bass below it goes to the subwoofer channel (changing it causes a short dropout). \
                    Most AV receivers already send the speakers' bass to the subwoofer, so a strong subwoofer level \
                    can double the bass. Rear delay keeps sounds anchored at the front; the high-cut makes the rears \
                    read as room sound rather than as speakers you can point at.
                    """)
                }
            }
        }
    }

    private func slider(_ title: String, _ value: Binding<Double>, _ range: ClosedRange<Double>, step: Double,
                        _ label: @escaping (Double) -> String) -> some View {
        HStack {
            if compact { Text(title).font(.caption).frame(width: 78, alignment: .leading) }
            Slider(value: value, in: range, step: step) { if !compact { Text(title) } }
                .controlSize(compact ? .small : .regular)
            Text(label(value.wrappedValue)).font(compact ? .caption : .body).monospacedDigit()
                .frame(width: compact ? 70 : 84, alignment: .trailing)
        }
    }
}

struct AboutView: View {
    @State private var confirmUninstall = false
    @State private var removeBlackHole = false

    var body: some View {
        Form {
            Section {
                LabeledContent("Version", value: Bundle.main.object(forInfoDictionaryKey: "CFBundleShortVersionString") as? String ?? "development build")
                note("Surround Bridge sends everything your Mac plays to your amp as Dolby Digital or DTS surround, upmixing stereo to 5.1 on the way. It's a sister app of Surround Player.")
            }
            Section {
                note("Apps play into BlackHole 2ch, a free virtual audio device by Existential Audio Inc., installed with its own official installer. Surround Bridge isn't affiliated with Existential Audio.")
                Link("BlackHole website", destination: URL(string: "https://existential.audio/blackhole/")!)
                note("Audio processing uses FFmpeg, under the LGPL. \"Dolby Digital\" and \"DTS\" are trademarks of their owners; Surround Bridge isn't affiliated with or licensed by them.")
            } header: { Text("Credits") }
            Section {
                Toggle("Also remove BlackHole (other apps may use it)", isOn: $removeBlackHole)
                Button("Uninstall Surround Bridge…") { confirmUninstall = true }
            } header: { Text("Uninstall") }
        }
        .formStyle(.grouped)
        .confirmationDialog("Uninstall Surround Bridge?", isPresented: $confirmUninstall) {
            Button("Uninstall", role: .destructive) { Uninstaller.run(removeBlackHole: removeBlackHole) }
        } message: {
            Text(removeBlackHole ? "Removes the app and BlackHole. macOS asks for your password." : "Removes the app. macOS asks for your password.")
        }
    }
}
