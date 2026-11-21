// CoreAudio helpers for the app: the Mac's default output (what apps and alerts play to), UIDs,
// and change notifications. The bridge itself (capture, encode, the HDMI device) is in BridgeCore.
import CoreAudio
import Foundation

enum AudioSystem {
    private static func addr(_ sel: AudioObjectPropertySelector) -> AudioObjectPropertyAddress {
        AudioObjectPropertyAddress(mSelector: sel, mScope: kAudioObjectPropertyScopeGlobal, mElement: kAudioObjectPropertyElementMain)
    }

    private static func getDevice(_ sel: AudioObjectPropertySelector) -> AudioDeviceID? {
        var a = addr(sel)
        var id = AudioDeviceID(0)
        var size = UInt32(MemoryLayout<AudioDeviceID>.size)
        guard AudioObjectGetPropertyData(AudioObjectID(kAudioObjectSystemObject), &a, 0, nil, &size, &id) == noErr, id != 0 else { return nil }
        return id
    }

    @discardableResult
    private static func setDevice(_ sel: AudioObjectPropertySelector, _ id: AudioDeviceID) -> Bool {
        var a = addr(sel)
        var v = id
        return AudioObjectSetPropertyData(AudioObjectID(kAudioObjectSystemObject), &a, 0, nil,
                                          UInt32(MemoryLayout<AudioDeviceID>.size), &v) == noErr
    }

    /// The output apps play to.
    static var defaultOutput: AudioDeviceID? { getDevice(kAudioHardwarePropertyDefaultOutputDevice) }
    /// The output alerts and UI sounds play to (separate setting in Sound settings).
    static var systemOutput: AudioDeviceID? { getDevice(kAudioHardwarePropertyDefaultSystemOutputDevice) }

    static func setDefaultOutput(_ id: AudioDeviceID) -> Bool { setDevice(kAudioHardwarePropertyDefaultOutputDevice, id) }
    static func setSystemOutput(_ id: AudioDeviceID) -> Bool { setDevice(kAudioHardwarePropertyDefaultSystemOutputDevice, id) }

    static func device(uid: String) -> AudioDeviceID? {
        var a = addr(kAudioHardwarePropertyTranslateUIDToDevice)
        var cf = uid as CFString
        var id = AudioDeviceID(0)
        var size = UInt32(MemoryLayout<AudioDeviceID>.size)
        let r = withUnsafeMutablePointer(to: &cf) { p in
            AudioObjectGetPropertyData(AudioObjectID(kAudioObjectSystemObject), &a, UInt32(MemoryLayout<CFString>.size), p, &size, &id)
        }
        return r == noErr && id != kAudioObjectUnknown ? id : nil
    }

    static func uid(of id: AudioDeviceID) -> String? {
        var a = addr(kAudioDevicePropertyDeviceUID)
        var cf: Unmanaged<CFString>?
        var size = UInt32(MemoryLayout<Unmanaged<CFString>?>.size)
        guard AudioObjectGetPropertyData(id, &a, 0, nil, &size, &cf) == noErr, let s = cf?.takeRetainedValue() else { return nil }
        return s as String
    }

    static func name(of id: AudioDeviceID) -> String {
        var a = addr(kAudioObjectPropertyName)
        var cf: Unmanaged<CFString>?
        var size = UInt32(MemoryLayout<Unmanaged<CFString>?>.size)
        guard AudioObjectGetPropertyData(id, &a, 0, nil, &size, &cf) == noErr, let s = cf?.takeRetainedValue() else { return "?" }
        return s as String
    }

    /// Calls `onChange` on the main queue when devices come or go or the default output changes.
    static func watch(_ onChange: @escaping () -> Void) -> [AudioObjectPropertyListenerBlock] {
        var blocks: [AudioObjectPropertyListenerBlock] = []
        for sel in [kAudioHardwarePropertyDevices, kAudioHardwarePropertyDefaultOutputDevice] {
            var a = addr(sel)
            let block: AudioObjectPropertyListenerBlock = { _, _ in onChange() }
            AudioObjectAddPropertyListenerBlock(AudioObjectID(kAudioObjectSystemObject), &a, DispatchQueue.main, block)
            blocks.append(block)
        }
        return blocks
    }
}
