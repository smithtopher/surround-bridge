// swift-tools-version:5.9
import Foundation
import PackageDescription

// FFmpeg (static, built with libavfilter), in order of preference:
//   1. $FFMPEG_PREFIX (scripts/build_app.sh sets this per architecture)
//   2. vendor/ffmpeg/<host arch> (scripts/build_ffmpeg.sh)
//   3. a sibling Surround Player checkout's vendored build (../music_player/vendor/ffmpeg/<arch>),
//      which uses the same configure options
let packageRoot = URL(fileURLWithPath: #filePath).deletingLastPathComponent().path
#if arch(arm64)
let hostArch = "arm64"
#else
let hostArch = "x86_64"
#endif
let candidates = ["\(packageRoot)/vendor/ffmpeg/\(hostArch)", "\(packageRoot)/../music_player/vendor/ffmpeg/\(hostArch)"]
let ffmpegPrefix = ProcessInfo.processInfo.environment["FFMPEG_PREFIX"]
    ?? candidates.first { FileManager.default.fileExists(atPath: "\($0)/lib/libavfilter.a") } ?? candidates[0]

let ffmpegLinks: [LinkerSetting] = [
    .unsafeFlags(["-L\(ffmpegPrefix)/lib"]),
    .linkedLibrary("avfilter"), .linkedLibrary("avformat"), .linkedLibrary("avcodec"), .linkedLibrary("avutil"),
    .linkedLibrary("swresample"), .linkedLibrary("swscale"),
    .linkedLibrary("z"), .linkedLibrary("bz2"),            // needed by a static FFmpeg
    .linkedFramework("CoreAudio"), .linkedFramework("AudioToolbox"), .linkedFramework("CoreFoundation"),
    .linkedFramework("CoreVideo"), .linkedFramework("VideoToolbox"), .linkedFramework("CoreMedia"),
    .linkedFramework("CoreServices"), .linkedFramework("Security"),
]

let package = Package(
    name: "SurroundBridge",
    platforms: [.macOS(.v14)],
    targets: [
        // the portable pipeline + macOS back-ends; public API in core/include (bridge.h, pc_types.h)
        .target(
            name: "BridgeCore",
            path: "core",
            cSettings: [.unsafeFlags(["-I\(ffmpegPrefix)/include", "-O2", "-Wall", "-Wno-deprecated-declarations"])],
            linkerSettings: ffmpegLinks
        ),
        // the menu-bar app
        .executableTarget(
            name: "SurroundBridge",
            dependencies: ["BridgeCore"],
            path: "Sources/SurroundBridge",
            linkerSettings: [.linkedFramework("AVFoundation"), .linkedFramework("ServiceManagement")]
        ),
        // the command-line tool (also built by scripts/build.sh)
        .executableTarget(
            name: "sbridge",
            dependencies: ["BridgeCore"],
            path: "tools",
            cSettings: [.unsafeFlags(["-I\(ffmpegPrefix)/include"])]
        ),
    ]
)
