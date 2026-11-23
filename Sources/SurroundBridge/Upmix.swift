// Stereo upmix settings. Copied from Surround Player (Sources/SurroundPlayer/Upmix.swift, see
// SHARED.md): same presets, ranges and "upmix*" UserDefaults keys. The engine does the work
// (core/pc_upmix.c). Differences: load() takes the default mode (the bridge starts with Music on);
// tone (bass and treble boost, `upmixBassDb` / `upmixTrebleDb`) is a listening preference kept
// apart from the presets, so a low-volume loudness curve survives switching modes.
import Foundation
import BridgeCore

struct UpmixSettings: Equatable {
    enum Mode: Int, CaseIterable, Identifiable {
        case off = 0, music, movies, wide, custom
        var id: Int { rawValue }
        var title: String {
            switch self {
            case .off: return "Off"
            case .music: return "Music"
            case .movies: return "Movies & TV"
            case .wide: return "Wide"
            case .custom: return "Custom"
            }
        }
        var blurb: String {
            switch self {
            case .off: return "Stereo stays stereo."
            case .music: return "Gentle room sound in the rears; vocals stay between the front speakers."
            case .movies: return "Dialogue locked to the centre speaker, ambience behind you."
            case .wide: return "Bigger, more enveloping sound with stronger rears and a wider front."
            case .custom: return "Your own settings."
            }
        }
    }

    var mode = Mode.off
    var includeVideo = false
    var surroundDb = -3.0       // rear level
    var center = 0.4            // 0 phantom centre .. 1 locked to the centre speaker
    var width = 0.55            // 0.5 = as recorded
    var lfeDb = -6.0            // -40 = no LFE
    var crossoverHz = 100
    var lfeMove = false         // false: copy bass to the sub; true: move it (small speakers)
    var bassDb = 0.0            // tone: not part of the presets
    var trebleDb = 0.0
    var rearDelayMs = 10.0
    var rearHighcutHz = 7000    // 0 = off

    var enabled: Bool { mode != .off }

    /// The sliders' values for a preset (mode and includeVideo are left alone).
    mutating func apply(_ preset: Mode) {
        switch preset {
        case .music:
            (surroundDb, center, width, lfeDb, crossoverHz, lfeMove, rearDelayMs, rearHighcutHz) =
                (-3, 0.4, 0.55, -6, 100, false, 10, 7000)
        case .movies:
            (surroundDb, center, width, lfeDb, crossoverHz, lfeMove, rearDelayMs, rearHighcutHz) =
                (-4, 0.9, 0.5, -3, 120, false, 15, 7000)
        case .wide:
            (surroundDb, center, width, lfeDb, crossoverHz, lfeMove, rearDelayMs, rearHighcutHz) =
                (0, 0.5, 0.8, -3, 100, false, 5, 12000)
        case .off, .custom:
            break
        }
    }

    var engine: PCUpmix {
        PCUpmix(enabled: enabled ? 1 : 0, include_video: includeVideo ? 1 : 0, surround_db: Float(surroundDb),
                center: Float(center), width: Float(width), lfe_db: Float(lfeDb), crossover_hz: Int32(crossoverHz),
                lfe_move: lfeMove ? 1 : 0, bass_db: Float(bassDb), rear_delay_ms: Float(rearDelayMs),
                rear_highcut_hz: Int32(rearHighcutHz), treble_db: Float(trebleDb))
    }

    static func load(_ d: UserDefaults, defaultMode: Mode = .off) -> UpmixSettings {
        var s = UpmixSettings()
        s.mode = Mode(rawValue: d.object(forKey: "upmixMode") as? Int ?? defaultMode.rawValue) ?? defaultMode
        s.includeVideo = d.object(forKey: "upmixVideo") as? Bool ?? false
        s.surroundDb = d.object(forKey: "upmixSurroundDb") as? Double ?? s.surroundDb
        s.center = d.object(forKey: "upmixCenter") as? Double ?? s.center
        s.width = d.object(forKey: "upmixWidth") as? Double ?? s.width
        s.lfeDb = d.object(forKey: "upmixLfeDb") as? Double ?? s.lfeDb
        s.crossoverHz = d.object(forKey: "upmixCrossoverHz") as? Int ?? s.crossoverHz
        s.lfeMove = d.object(forKey: "upmixLfeMove") as? Bool ?? s.lfeMove
        s.bassDb = d.object(forKey: "upmixBassDb") as? Double ?? s.bassDb
        s.trebleDb = d.object(forKey: "upmixTrebleDb") as? Double ?? s.trebleDb
        s.rearDelayMs = d.object(forKey: "upmixRearDelayMs") as? Double ?? s.rearDelayMs
        s.rearHighcutHz = d.object(forKey: "upmixRearHighcutHz") as? Int ?? s.rearHighcutHz
        return s
    }

    func save(_ d: UserDefaults) {
        d.set(mode.rawValue, forKey: "upmixMode")
        d.set(includeVideo, forKey: "upmixVideo")
        d.set(surroundDb, forKey: "upmixSurroundDb")
        d.set(center, forKey: "upmixCenter")
        d.set(width, forKey: "upmixWidth")
        d.set(lfeDb, forKey: "upmixLfeDb")
        d.set(crossoverHz, forKey: "upmixCrossoverHz")
        d.set(lfeMove, forKey: "upmixLfeMove")
        d.set(bassDb, forKey: "upmixBassDb")
        d.set(trebleDb, forKey: "upmixTrebleDb")
        d.set(rearDelayMs, forKey: "upmixRearDelayMs")
        d.set(rearHighcutHz, forKey: "upmixRearHighcutHz")
    }
}
