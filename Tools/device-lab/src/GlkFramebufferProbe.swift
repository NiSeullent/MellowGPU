// GlkFramebufferProbe.swift — real GUI framebuffer / WindowServer presentation probe
// for the designated GLK GPU.
//
// Opens an on-screen AppKit window backed by a CAMetalLayer on the Metal device bound to
// the physical GLK GPU, acquires real drawables, renders a known per-frame color, calls
// commandBuffer.present(drawable), and records presentation timestamps (frame pacing) plus
// a GPU readback of each presented drawable validated against the known color.
//
// Honesty boundaries enforced below:
//   * No WindowServer session, no GLK Metal device, a translated process, or a missing
//     drawable is a FAILING result — never a passing skip, never a CPU fallback.
//   * Reading back the drawable TEXTURE proves the GPU produced the presented pixels. It
//     does NOT prove scanout/photons: true screen-capture verification needs Screen
//     Recording (TCC) permission, which this tool neither requests nor changes. That
//     check is reported explicitly as NOT_RUN.
//   * Public presentation success does not attest native-driver fences or compositor
//     ownership; those flags stay false.

import Foundation
import Metal
import QuartzCore
import IOKit
import AppKit

struct ProbeOptions {
    var registryID: UInt64?
    var frames = 120
    var width = 512
    var height = 512
    var presentTimeout: Double = 5
    var maxTotalSeconds: Double = 90
    var selfcheck = false
    var headlessCheckOnly = false
}

func frameColor(_ frame: Int) -> (r: UInt8, g: UInt8, b: UInt8, a: UInt8) {
    (UInt8((frame &* 7 &+ 10) & 0xFF), UInt8((frame &* 13 &+ 20) & 0xFF),
     UInt8((frame &* 29 &+ 40) & 0xFF), 255)
}

func parseProbeOptions() -> ProbeOptions {
    var options = ProbeOptions()
    var iterator = CommandLine.arguments.dropFirst().makeIterator()
    func nextValue(_ flag: String) -> String {
        guard let value = iterator.next() else { fail(2, "missing value for \(flag)") }
        return value
    }
    func parseInt(_ s: String, _ flag: String, min: Int, max: Int) -> Int {
        guard let v = Int(s), v >= min, v <= max else { fail(2, "invalid \(flag): \(s)") }
        return v
    }
    while let arg = iterator.next() {
        switch arg {
        case "--registry-id":
            let t = nextValue(arg).lowercased()
            let parsed = t.hasPrefix("0x") ? UInt64(t.dropFirst(2), radix: 16) : UInt64(t)
            guard let v = parsed else { fail(2, "invalid --registry-id") }
            options.registryID = v
        case "--frames": options.frames = parseInt(nextValue(arg), arg, min: 1, max: 100_000)
        case "--width": options.width = parseInt(nextValue(arg), arg, min: 32, max: 8192)
        case "--height": options.height = parseInt(nextValue(arg), arg, min: 32, max: 8192)
        case "--present-timeout":
            guard let v = Double(nextValue(arg)), v >= 1, v <= 60 else { fail(2, "invalid --present-timeout") }
            options.presentTimeout = v
        case "--max-total-seconds":
            guard let v = Double(nextValue(arg)), v >= 5, v <= 1200 else { fail(2, "invalid --max-total-seconds") }
            options.maxTotalSeconds = v
        case "--selfcheck": options.selfcheck = true
        case "--headless-check": options.headlessCheckOnly = true
        case "--help", "-h": FileHandle.standardError.write(Data("See file header for usage.\n".utf8)); exit(0)
        default: fail(2, "unknown argument: \(arg)")
        }
    }
    return options
}

func runProbeSelfcheck() -> Never {
    emit("selfcheck_started", ["host": hostEnvironment()])
    var failures = 0
    let c0 = frameColor(0), c1 = frameColor(1)
    if c0.r != 10 || c0.g != 20 || c0.b != 40 || c0.a != 255 { failures += 1 }
    if c1.r != 17 || c1.g != 33 || c1.b != 69 || c1.a != 255 { failures += 1 }
    if frameColor(256).r != frameColor(0).r { failures += 1 } // wraps mod 256
    let stats = summarize([10, 20, 30])
    if (stats["mean"] as? Double) != 20 { failures += 1 }
    let identity = identityPredicateSelftest()
    failures += identity.failures
    emit("selfcheck_completed", ["failures": failures, "passed": failures == 0,
                                 "identity_cases": identity.cases, "identity_failures": identity.failures,
                                 "metal_called": false, "window_opened": false])
    exit(failures == 0 ? 0 : 1)
}

func pickGlkDevice(_ options: ProbeOptions) -> (MTLDevice, PciIdentity) {
    let devices = MTLCopyAllDevices()
    emit("device_enumeration", ["device_count": devices.count])
    if devices.isEmpty {
        fail(3, "no Metal devices present — target exposes no IOAccelerator/Metal GPU (expected while native driver absent)",
             ["metal_device_count": 0, "metal_tested": false])
    }
    var candidates: [(MTLDevice, PciIdentity)] = []
    for device in devices {
        let pci = pciIdentity(forRegistryID: device.registryID)
        let match = pci.map(evaluateGlkMatch)
        emit("device", [
            "name": device.name, "registry_id": String(device.registryID),
            "supports_metal3": device.supportsFamily(.metal3),
            "pci": pci?.dictionary() ?? ["resolved": false],
            "glk_ownership_attested": match?.ownershipAttested ?? false,
            "glk_reasons": match?.reasons ?? ["pci identity not resolved"],
        ])
        if let pci = pci, let match = match, match.ownershipAttested { candidates.append((device, pci)) }
    }
    if let requested = options.registryID {
        guard let hit = devices.first(where: { $0.registryID == requested }),
              let pci = pciIdentity(forRegistryID: hit.registryID) else {
            fail(3, "requested --registry-id not found or without PCI provider")
        }
        let match = evaluateGlkMatch(pci)
        guard match.ownershipAttested else {
            fail(11, "requested device is not the physical GLK GPU: \(match.reasons.joined(separator: "; "))")
        }
        return (hit, pci)
    }
    guard candidates.count == 1 else {
        fail(candidates.isEmpty ? 11 : 12,
             "expected exactly one Metal device bound to physical GLK; found \(candidates.count)")
    }
    return candidates[0]
}

// Thread-safe collector for presentation timestamps delivered on Metal's callback thread.
final class PacingCollector {
    private let lock = NSLock()
    private(set) var presentedTimes: [Double] = []
    func record(_ time: Double) { lock.lock(); presentedTimes.append(time); lock.unlock() }
    func snapshot() -> [Double] { lock.lock(); defer { lock.unlock() }; return presentedTimes }
}

// MARK: - Entry point

@main
struct GlkFramebufferProbeMain {
static func main() {
gToolName = "glk-framebuffer-probe"
gSchemaVersion = 1
let options = parseProbeOptions()
if options.selfcheck { runProbeSelfcheck() }

emit("started", [
    "host": hostEnvironment(), "target": GlkTarget.description(),
    "options": ["frames": options.frames, "width": options.width, "height": options.height,
                "present_timeout": options.presentTimeout, "max_total_seconds": options.maxTotalSeconds],
])

// Gate 1: native, untranslated x86_64 process on macOS 15.8.1 (24H32), checked before any
// WindowServer/Metal/IOKit device call.
requireNativeTargetHost()

// Hard watchdog: AppKit/WindowServer calls can block. A background timer guarantees a
// fatal, explicit _exit rather than a silent hang. Timeout is never a clean success.
let completed = armProcessWatchdog(seconds: options.maxTotalSeconds, what: "glk-framebuffer-probe")

// Gate 2: a real WindowServer/GUI session must exist.
let screens = NSScreen.screens
let mainDisplay = CGMainDisplayID()
emit("windowserver_probe", [
    "screen_count": screens.count, "has_main_screen": NSScreen.main != nil,
    "cg_main_display_id": Int(mainDisplay),
    "screen_sizes": screens.map { ["w": Int($0.frame.width), "h": Int($0.frame.height)] },
])
if screens.isEmpty || NSScreen.main == nil || mainDisplay == 0 {
    fail(24, "no WindowServer session / no attached display; GUI presentation cannot run (not a skip)",
         ["windowserver_session": false])
}
if options.headlessCheckOnly {
    emit("headless_check_completed", ["windowserver_session": true, "metal_tested": false])
    completed.signal(); exit(0)
}

// Gate 3: bind the GLK Metal device and require the Metal 3 family.
let (device, pci) = pickGlkDevice(options)
let registryID = device.registryID
if !device.supportsFamily(.metal3) {
    fail(23, "selected GLK device does not support Metal 3 family", ["supports_metal3": false])
}
emit("device_selected", ["name": device.name, "registry_id": String(registryID), "pci": pci.dictionary()])

guard let queue = device.makeCommandQueue() else { fail(6, "command queue creation failed") }

// Build the on-screen CAMetalLayer-backed window on the main thread.
let app = NSApplication.shared
app.setActivationPolicy(.regular)
app.finishLaunching()

let metalLayer = CAMetalLayer()
metalLayer.device = device
metalLayer.pixelFormat = .bgra8Unorm
metalLayer.framebufferOnly = false // allow GPU readback of the presented drawable
metalLayer.drawableSize = CGSize(width: options.width, height: options.height)
metalLayer.contentsScale = 1.0

let contentRect = NSRect(x: 0, y: 0, width: options.width, height: options.height)
let view = NSView(frame: contentRect)
view.layer = metalLayer          // layer-hosting view
view.wantsLayer = true
let window = NSWindow(contentRect: contentRect, styleMask: [.titled, .closable],
                      backing: .buffered, defer: false)
window.title = "GLK Device Lab — Framebuffer Probe"
window.contentView = view
window.makeKeyAndOrderFront(nil)
window.center()
app.activate(ignoringOtherApps: true)

func pumpEvents() {
    while let event = app.nextEvent(matching: .any, until: nil, inMode: .default, dequeue: true) {
        app.sendEvent(event)
    }
}
pumpEvents()
RunLoop.current.run(until: Date().addingTimeInterval(0.1)) // let the window map

let pacing = PacingCollector()
let bytesPerRow = ((options.width * 4) + 255) & ~255
let readbackBytes = bytesPerRow * options.height
guard let readback = device.makeBuffer(length: readbackBytes, options: .storageModeShared) else {
    fail(12, "readback buffer allocation failed")
}

var presented = 0
var presentCallbackTimeouts = 0
var droppedPresents = 0
var nilDrawables = 0
var readbackValidated = 0
var cpuSubmitTimes: [Double] = []

for frame in 0..<options.frames {
    autoreleasepool {
        let color = frameColor(frame)
        // Acquire a real drawable, retrying briefly (nextDrawable can transiently return nil).
        var drawable: CAMetalDrawable?
        let drawableDeadline = Date().addingTimeInterval(options.presentTimeout)
        while drawable == nil && Date() < drawableDeadline {
            drawable = metalLayer.nextDrawable()
            if drawable == nil { pumpEvents(); RunLoop.current.run(until: Date().addingTimeInterval(0.004)) }
        }
        guard let activeDrawable = drawable else {
            nilDrawables += 1
            fail(25, "nextDrawable() returned nil for \(options.presentTimeout)s at frame \(frame)",
                 ["frame": frame, "presented_before_failure": presented])
        }
        guard let command = queue.makeCommandBuffer() else { fail(6, "command buffer creation failed") }
        let pass = MTLRenderPassDescriptor()
        pass.colorAttachments[0].texture = activeDrawable.texture
        pass.colorAttachments[0].loadAction = .clear
        pass.colorAttachments[0].storeAction = .store
        pass.colorAttachments[0].clearColor = MTLClearColor(
            red: Double(color.r) / 255.0, green: Double(color.g) / 255.0,
            blue: Double(color.b) / 255.0, alpha: Double(color.a) / 255.0)
        guard let encoder = command.makeRenderCommandEncoder(descriptor: pass) else {
            fail(13, "render encoder creation failed at frame \(frame)")
        }
        encoder.endEncoding() // clear-only pass yields a fully-known image
        memset(readback.contents(), 0xA5, readbackBytes)
        guard let blit = command.makeBlitCommandEncoder() else { fail(15, "readback blit creation failed") }
        blit.copy(from: activeDrawable.texture, sourceSlice: 0, sourceLevel: 0,
                  sourceOrigin: MTLOrigin(x: 0, y: 0, z: 0),
                  sourceSize: MTLSize(width: options.width, height: options.height, depth: 1),
                  to: readback, destinationOffset: 0,
                  destinationBytesPerRow: bytesPerRow, destinationBytesPerImage: readbackBytes)
        blit.endEncoding()

        let presentedSignal = DispatchSemaphore(value: 0)
        activeDrawable.addPresentedHandler { drawable in
            let t = drawable.presentedTime
            if t > 0 { pacing.record(t) }
            presentedSignal.signal()
        }
        let completedSignal = DispatchSemaphore(value: 0)
        command.addCompletedHandler { _ in completedSignal.signal() }
        let recordedBefore = pacing.snapshot().count
        let submitTime = CACurrentMediaTime()
        cpuSubmitTimes.append(submitTime)
        command.present(activeDrawable)
        command.commit()

        // Bounded wait for GPU completion, then validate the presented drawable's pixels.
        guard completedSignal.wait(timeout: .now() + options.presentTimeout) == .success else {
            fail(7, "frame \(frame) GPU work did not complete within \(options.presentTimeout)s",
                 ["frame": frame, "timed_out": true, "clean_cleanup": false])
        }
        if command.status != .completed {
            fail(8, "frame \(frame) command buffer error: \(String(describing: command.error))",
                 ["frame": frame, "status": command.status.rawValue])
        }
        let pixels = readback.contents().bindMemory(to: UInt8.self, capacity: readbackBytes)
        // BGRA8Unorm storage order: byte0=B, byte1=G, byte2=R, byte3=A.
        var mismatches = 0
        let sampleStride = max(1, options.height / 64) // bounded sampling across rows
        var row = 0
        while row < options.height {
            for column in stride(from: 0, to: options.width, by: max(1, options.width / 64)) {
                let offset = row * bytesPerRow + column * 4
                if pixels[offset] != color.b || pixels[offset + 1] != color.g
                    || pixels[offset + 2] != color.r || pixels[offset + 3] != color.a {
                    mismatches += 1
                }
            }
            row += sampleStride
        }
        if mismatches != 0 {
            fail(14, "presented drawable readback mismatch at frame \(frame): \(mismatches) sampled pixels wrong",
                 ["frame": frame])
        }
        readbackValidated += 1

        // Bounded wait for the presentation callback (pacing evidence). Only a callback
        // carrying a real presentedTime counts as a presented frame.
        if presentedSignal.wait(timeout: .now() + options.presentTimeout) == .timedOut {
            presentCallbackTimeouts += 1
        } else if pacing.snapshot().count > recordedBefore {
            presented += 1
        } else {
            droppedPresents += 1
        }
        pumpEvents()
        if frame % 30 == 0 {
            emit("frame_progress", ["frame": frame, "presented": presented,
                                    "readback_validated": readbackValidated])
        }
    }
}

// Frame pacing from real presentation timestamps; CPU submit intervals as a secondary view.
func intervalsMs(_ times: [Double]) -> [Double] {
    guard times.count >= 2 else { return [] }
    return zip(times.dropFirst(), times).map { ($0 - $1) * 1000.0 }
}
let presentedTimes = pacing.snapshot()
let presentIntervals = intervalsMs(presentedTimes)
let submitIntervals = intervalsMs(cpuSubmitTimes)
if presented != options.frames {
    fail(28, "only \(presented) of \(options.frames) frames were confirmed presented with a real presentedTime",
         ["frames_presented": presented, "present_callback_timeouts": presentCallbackTimeouts,
          "dropped_presents": droppedPresents, "drawable_readback_validated_frames": readbackValidated,
          "present_interval_ms": summarize(presentIntervals)])
}

emit("completed", [
    "registry_id": String(registryID), "device_name": device.name, "pci": pci.dictionary(),
    "glk_ownership_attested": true,
    "host": hostEnvironment(), "target": GlkTarget.description(),
    "frames_requested": options.frames, "frames_presented": presented,
    "drawable_readback_validated_frames": readbackValidated, "nil_drawables": nilDrawables,
    "present_callback_timeouts": presentCallbackTimeouts, "dropped_presents": droppedPresents,
    "width": options.width, "height": options.height,
    "present_interval_ms": summarize(presentIntervals),
    "submit_interval_ms": summarize(submitIntervals),
    "presentation_timestamps_captured": presentedTimes.count,
    // Honesty flags.
    "metal_tested": true, "metal3_family": true, "windowserver_session": true,
    "window_presented": presented > 0, "drawable_readback_validated": readbackValidated == options.frames,
    "cpu_fallback_used": false, "public_metal_only": true,
    "native_driver_fence_evidence": false, "compositor_ownership_attested": false,
    "scanout_screencapture_verified": false,
    "scanout_note": "drawable-texture readback proves GPU-produced presented pixels; photon/scanout verification requires Screen Recording (TCC) permission and is NOT_RUN by design",
    "game_compatibility_attested": false, "hardware_qualification": false,
    "evidence_scope": "public CAMetalLayer drawable acquisition + presentation + GPU readback + pacing on the physical GLK GPU",
])
completed.signal()
exit(0)
} // static func main
} // struct GlkFramebufferProbeMain
