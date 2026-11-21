// The app's model: settings, on/off, the Mac's output switching, device watching, status.
// The only place that calls the C bridge (BridgeCore: sb_*), always on `queue`, so the UI never
// waits for a device to switch formats.
import AppKit
import AVFoundation
import BridgeCore
import CoreAudio
import ServiceManagement
import SwiftUI

struct OutputDevice: Identifiable, Hashable {
    let uid: String
    let name: String
    let isHDMI: Bool
    var id: String { uid }
}

func cString<T>(_ tuple: T) -> String {
    withUnsafeBytes(of: tuple) { raw in String(cString: raw.bindMemory(to: CChar.self).baseAddress!) }
}

@MainActor
final class BridgeModel: ObservableObject {
    static let shared = BridgeModel()

    enum State: Equatable {
        case off, starting, running
        case needsPermission          // microphone access denied
        case noVirtualDevice          // BlackHole not installed
        case noOutput                 // the chosen HDMI/optical device isn't connected
        case failed(String)
    }

    // settings (persisted). A separate suite can be named for test runs (never the user's own).
    static let defaults: UserDefaults = {
        if let suite = ProcessInfo.processInfo.environment["SURROUND_BRIDGE_DEFAULTS"], let d = UserDefaults(suiteName: suite) { return d }
        return .standard
    }()
    private var d: UserDefaults { Self.defaults }

    @Published var enabled: Bool = defaults.bool(forKey: "enabled") {
        didSet { d.set(enabled, forKey: "enabled"); if enabled != oldValue { apply() } }
    }
    @Published var outputUID: String = defaults.string(forKey: "outputUID") ?? "" {
        didSet {
            d.set(outputUID, forKey: "outputUID")
            if let o = outputs.first(where: { $0.uid == outputUID }) { d.set(o.name, forKey: "outputName") }
            if outputUID != oldValue { restartIfOn() }
        }
    }
    /// 0 Dolby Digital, 1 DTS
    @Published var codec: Int = defaults.object(forKey: "codec") as? Int ?? 0 {
        didSet { d.set(codec, forKey: "codec"); if codec != oldValue { restartIfOn() } }
    }
    @Published var ac3Bitrate: Int = defaults.object(forKey: "ac3Bitrate") as? Int ?? 640 {
        didSet { d.set(ac3Bitrate, forKey: "ac3Bitrate"); if ac3Bitrate != oldValue { restartIfOn() } }
    }
    @Published var dtsBitrate: Int = defaults.object(forKey: "dtsBitrate") as? Int ?? 1509 {
        didSet { d.set(dtsBitrate, forKey: "dtsBitrate"); if dtsBitrate != oldValue { restartIfOn() } }
    }
    /// Point the Mac's sound output at the bridge while it's on (and back afterwards).
    @Published var autoSwitchOutput: Bool = defaults.object(forKey: "autoSwitchOutput") as? Bool ?? true {
        didSet { d.set(autoSwitchOutput, forKey: "autoSwitchOutput") }
    }
    @Published var launchAtLogin: Bool = SMAppService.mainApp.status == .enabled {
        didSet {
            guard launchAtLogin != oldValue else { return }
            do { if launchAtLogin { try SMAppService.mainApp.register() } else { try SMAppService.mainApp.unregister() } }
            catch { lastError = "Launch at login: \(error.localizedDescription)" }
        }
    }
    /// Sliders apply live; turning the upmix on or off changes the encoder layout: restart.
    @Published var upmix = UpmixSettings.load(defaults, defaultMode: .music) {
        didSet {
            guard upmix != oldValue else { return }
            upmix.save(d)
            pushUpmix()
            if upmix.enabled != oldValue.enabled { restartIfOn() }
        }
    }

    // status
    @Published private(set) var state = State.off {
        didSet { if Self.debug && state != oldValue { FileHandle.standardError.write("[bridge] state: \(state) — \(statusLine)\n".data(using: .utf8)!) } }
    }
    @Published private(set) var outputs: [OutputDevice] = []
    @Published private(set) var virtualDeviceUID: String?
    @Published private(set) var status = SBStatus()
    @Published private(set) var outputDescription = ""
    @Published var lastError: String?

    private let bridge = sb_bridge_create()
    private let queue = DispatchQueue(label: "surround-bridge")
    private var listeners: [AudioObjectPropertyListenerBlock] = []
    private var timer: Timer?
    private var generation = 0                // invalidates a start that finished after a stop
    private static let debug = ProcessInfo.processInfo.environment["SURROUND_BRIDGE_DEBUG"] != nil

    private init() {
        pushUpmix()
        refreshDevices()
        restoreOutputAfterCrash()
        listeners = AudioSystem.watch { [weak self] in MainActor.assumeIsolated { self?.devicesChanged() } }
        NSWorkspace.shared.notificationCenter.addObserver(forName: NSWorkspace.didWakeNotification, object: nil, queue: .main) { [weak self] _ in
            MainActor.assumeIsolated { self?.restartIfOn() }
        }
        timer = Timer.scheduledTimer(withTimeInterval: 1, repeats: true) { [weak self] _ in
            MainActor.assumeIsolated { self?.tick() }
        }
        if ProcessInfo.processInfo.environment["SURROUND_BRIDGE_AUTOSTART"] != nil { enabled = true }
        if enabled { apply() }
    }

    var isRunning: Bool { state == .running }

    /// Short line for the menu: what's happening now.
    var statusLine: String {
        switch state {
        case .off: return "Off"
        case .starting: return "Starting…"
        case .running:
            let codecName = codec == 1 ? "DTS" : "Dolby Digital"
            let lay = upmix.enabled ? "5.1" : (status.channels >= 6 ? "5.1" : "stereo")
            return "\(codecName) \(lay) → \(selectedOutput?.name ?? "output") · ~\(Int(status.latency_ms.rounded())) ms"
        case .needsPermission: return "Needs microphone access (see below)"
        case .noVirtualDevice: return "The Surround Bridge audio device isn't installed"
        case .noOutput: return "\(rememberedOutputName ?? "The HDMI/optical output") isn't connected"
        case .failed(let why): return why
        }
    }

    var selectedOutput: OutputDevice? {
        outputs.first { $0.uid == outputUID } ?? rememberedOutputName.flatMap { n in outputs.first { $0.name == n } }
    }
    private var rememberedOutputName: String? { d.string(forKey: "outputName") }

    // MARK: devices

    private func refreshDevices() {
        var raw = [PCDevice](repeating: PCDevice(), count: 64)
        let n = Int(sb_list_outputs(&raw, 64))
        outputs = raw.prefix(n).filter { ($0.is_hdmi != 0 || $0.has_compressed_formats != 0) }
            .map { OutputDevice(uid: cString($0.uid), name: cString($0.name), isHDMI: $0.is_hdmi != 0) }
        var ins = [PCDevice](repeating: PCDevice(), count: 64)
        let ni = Int(sb_list_inputs(&ins, 64))
        virtualDeviceUID = ins.prefix(ni).first { cString($0.name).localizedCaseInsensitiveContains("BlackHole") }.map { cString($0.uid) }
        // first run: pick the only (or first) digital output
        if outputUID.isEmpty, let first = outputs.first { outputUID = first.uid }
    }

    /// The TV changes its device UID when it re-sends its EDID (input/mode change), so a missing
    /// output is looked up by name; the bridge restarts when it comes back.
    private func devicesChanged() {
        let had = selectedOutput
        refreshDevices()
        if let o = selectedOutput, o.uid != outputUID { outputUID = o.uid }   // same TV, new UID
        guard enabled else { return }
        switch state {
        case .running where selectedOutput == nil || had?.uid != selectedOutput?.uid:
            restartIfOn()
        case .noOutput, .noVirtualDevice, .failed:
            if selectedOutput != nil, virtualDeviceUID != nil { apply() }
        default:
            break
        }
    }

    // MARK: on / off

    private func restartIfOn() {
        guard enabled else { return }
        stopBridge { [weak self] in self?.apply() }
    }

    private func apply() {
        if enabled { start() } else { stopBridge(nil) }
    }

    private func start() {
        lastError = nil
        guard let input = virtualDeviceUID else { state = .noVirtualDevice; return }
        guard let output = selectedOutput else { state = .noOutput; return }
        // capturing the virtual device counts as recording: without permission CoreAudio would block
        switch AVCaptureDevice.authorizationStatus(for: .audio) {
        case .authorized: break
        case .notDetermined:
            state = .starting
            AVCaptureDevice.requestAccess(for: .audio) { ok in
                DispatchQueue.main.async { MainActor.assumeIsolated {
                    let m = BridgeModel.shared
                    if ok { m.start() } else { m.state = .needsPermission }
                } }
            }
            return
        default:
            state = .needsPermission
            return
        }
        state = .starting
        generation += 1
        let gen = generation
        let codec = Int32(self.codec == 1 ? SB_CODEC_DTS : SB_CODEC_AC3)
        let ac3 = Int32(ac3Bitrate), dts = Int32(dtsBitrate)
        let bridge = self.bridge
        queue.async {
            var cfg = SBConfig()
            cfg.codec = codec
            cfg.ac3_bitrate_kbps = ac3
            cfg.dts_bitrate_kbps = dts
            var err = [CChar](repeating: 0, count: 512)
            let rc = input.withCString { inp in
                output.uid.withCString { out -> Int32 in
                    cfg.input_uid = inp
                    cfg.output_uid = out
                    return sb_bridge_start(bridge, &cfg, &err, 512)
                }
            }
            var st = SBStatus()
            sb_bridge_status(bridge, &st)
            let message = String(cString: err)
            DispatchQueue.main.async { MainActor.assumeIsolated {
                let m = BridgeModel.shared
                guard gen == m.generation, m.enabled else { return }
                if rc == 0 {
                    m.status = st
                    m.outputDescription = cString(st.output)
                    m.state = .running
                    m.switchOutputToBridge()
                } else {
                    m.state = .failed(message.isEmpty ? "Could not start" : message)
                }
            } }
        }
    }

    private func stopBridge(_ then: (() -> Void)?) {
        generation += 1
        restoreOutput()
        let bridge = self.bridge
        queue.async {
            sb_bridge_stop(bridge)   // gives the HDMI device back (format, exclusive access)
            DispatchQueue.main.async { MainActor.assumeIsolated {
                let m = BridgeModel.shared
                if !m.enabled { m.state = .off }
                then?()
            } }
        }
    }

    /// Quit / SIGTERM: synchronous, so the device and the Mac's output are restored before exit.
    func shutdown() {
        restoreOutput()
        queue.sync { sb_bridge_stop(bridge) }
    }

    private func pushUpmix() {
        var u = upmix.engine
        let bridge = self.bridge
        queue.async { sb_bridge_set_upmix(bridge, &u) }
    }

    // MARK: the Mac's sound output

    /// While on, apps must play into the virtual device (the HDMI device is ours, exclusively).
    /// What was selected before is remembered, also on disk in case the app is killed.
    private func switchOutputToBridge() {
        guard autoSwitchOutput, let vuid = virtualDeviceUID, let vid = AudioSystem.device(uid: vuid) else { return }
        if let cur = AudioSystem.defaultOutput, cur != vid, let cuid = AudioSystem.uid(of: cur) {
            d.set(cuid, forKey: "restoreOutputUID")
            if let s = AudioSystem.systemOutput, let suid = AudioSystem.uid(of: s) { d.set(suid, forKey: "restoreSystemOutputUID") }
        }
        _ = AudioSystem.setDefaultOutput(vid)
        _ = AudioSystem.setSystemOutput(vid)
    }

    private func restoreOutput() {
        guard let vuid = virtualDeviceUID, let vid = AudioSystem.device(uid: vuid) else { return }
        // only if it's still pointing at us (the user may have picked something else meanwhile)
        if AudioSystem.defaultOutput == vid, let uid = d.string(forKey: "restoreOutputUID"), let id = AudioSystem.device(uid: uid) {
            _ = AudioSystem.setDefaultOutput(id)
        }
        if AudioSystem.systemOutput == vid, let uid = d.string(forKey: "restoreSystemOutputUID"), let id = AudioSystem.device(uid: uid) {
            _ = AudioSystem.setSystemOutput(id)
        }
        d.removeObject(forKey: "restoreOutputUID")
        d.removeObject(forKey: "restoreSystemOutputUID")
    }

    /// A previous run was killed while on: put the Mac's output back unless we're turning on again.
    private func restoreOutputAfterCrash() {
        if !enabled, d.string(forKey: "restoreOutputUID") != nil { restoreOutput() }
    }

    // MARK: status

    private func tick() {
        guard state == .running else { return }
        var st = SBStatus()
        let bridge = self.bridge
        queue.async {
            sb_bridge_status(bridge, &st)
            DispatchQueue.main.async { MainActor.assumeIsolated {
                let m = BridgeModel.shared
                guard m.state == .running else { return }
                m.status = st
                if st.running == 0 { m.restartIfOn() }   // the pipeline stopped by itself (device gone)
                if Self.debug {
                    FileHandle.standardError.write(("[bridge] \(m.statusLine) | buffer \(String(format: "%.1f", st.buffer_ms)) ms, drift "
                        + "\(String(format: "%+.0f", st.drift_ppm)) ppm, overruns \(st.overruns), jumps \(st.jumps), upmix \(st.upmixing) | "
                        + "default output \(AudioSystem.defaultOutput.map(AudioSystem.name) ?? "-")\n").data(using: .utf8)!)
                }
            } }
        }
    }

    /// Input level per channel, 0...1 for the meters (-60 dB .. 0 dB).
    func meter(_ ch: Int) -> Double {
        guard state == .running, ch < Int(status.channels) else { return 0 }
        let db = withUnsafeBytes(of: status.peak_db) { $0.bindMemory(to: Float.self)[ch] }
        return max(0, min(1, (Double(db) + 60) / 60))
    }
}
