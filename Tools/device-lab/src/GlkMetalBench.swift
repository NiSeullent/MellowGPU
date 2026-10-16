// GlkMetalBench.swift — headless Metal 3 benchmark for the designated GLK GPU.
//
// Really compiles MSL on the target's Metal toolchain, submits bounded compute / render
// / blit command buffers to the Metal device whose registryID resolves through real
// IOService ancestors to PCI 8086:3185 rev06 subsys 17AA:3E77, validates every result
// against an independent CPU oracle, and records GPU timestamps, command-buffer errors
// and workload sizes across repeated iterations.
//
// No Metal device, a translated process, a missing Metal 3 family, or a device that is
// not the physical GLK GPU is a FAILING result (non-zero exit) — never a passing skip,
// and never a CPU fallback. Public Metal success alone does not attest native-driver
// fences, full GPU execution, WindowServer scanout, or game compatibility; those flags
// stay false in every report.
//
// Usage:
//   glk-metal-bench [--registry-id 0xID] [--iterations N] [--compute-elements N]
//                   [--compute-rounds N] [--render-size N] [--blit-bytes N]
//                   [--command-timeout SEC] [--max-total-seconds SEC] [--enumerate-only]
//   glk-metal-bench --selfcheck                          (CPU-only; no Metal)
//   glk-metal-bench --oracle-digest NONCE ROUNDS COUNT   (CPU-only; no Metal)

import Foundation
import Metal
import IOKit

// Embedded copy of shaders/GlkWorkloads.metal (identical code lines; comments omitted),
// compiled at runtime by the target GPU's Metal toolchain. build-glk-device-lab.py fails
// the build if any code line drifts between the two.
let kEmbeddedMSL = """
#include <metal_stdlib>
using namespace metal;

struct MixParams {
    uint count;
    uint nonce;
    uint rounds;
};

inline uint glk_mix(uint input, uint nonce, uint rounds) {
    uint x = input ^ nonce;
    for (uint i = 0u; i < rounds; ++i) {
        x = x * 1664525u + 1013904223u;
        x ^= x >> 15;
        x = x * 2246822519u;
        x ^= x >> 13;
    }
    return x;
}

kernel void glk_compute_mix(device uint *out            [[buffer(0)]],
                            const device uint *input    [[buffer(1)]],
                            constant MixParams &params   [[buffer(2)]],
                            device atomic_uint *checksum [[buffer(3)]],
                            uint gid                     [[thread_position_in_grid]]) {
    if (gid >= params.count) {
        return;
    }
    uint value = glk_mix(input[gid], params.nonce, params.rounds);
    out[gid] = value;
    atomic_fetch_add_explicit(checksum, value, memory_order_relaxed);
}

vertex float4 glk_fullscreen_vertex(uint vid [[vertex_id]]) {
    const float2 positions[3] = { float2(-1.0, -1.0), float2(3.0, -1.0), float2(-1.0, 3.0) };
    return float4(positions[vid], 0.0, 1.0);
}

struct ColorParams {
    float4 color;
};

fragment float4 glk_constant_fragment(constant ColorParams &params [[buffer(0)]]) {
    return params.color;
}
"""

struct BenchOptions {
    var registryID: UInt64?
    var iterations = 5
    var computeElements = 1 << 20
    var computeRounds = 64
    var renderSize = 256
    var blitBytes = 16 << 20
    var commandTimeout: Double = 10
    var maxTotalSeconds: Double = 100
    var enumerateOnly = false
    var selfcheck = false
    var oracleDigest: (nonce: UInt32, rounds: Int, count: Int)?
}

func parseOptions() -> BenchOptions {
    var options = BenchOptions()
    var iterator = CommandLine.arguments.dropFirst().makeIterator()
    func nextValue(_ flag: String) -> String {
        guard let value = iterator.next() else { fail(2, "missing value for \(flag)") }
        return value
    }
    func parseUInt64(_ s: String, _ flag: String) -> UInt64 {
        let t = s.lowercased()
        let parsed = t.hasPrefix("0x") ? UInt64(t.dropFirst(2), radix: 16) : UInt64(t)
        guard let value = parsed else { fail(2, "invalid integer for \(flag): \(s)") }
        return value
    }
    func parseInt(_ s: String, _ flag: String, min: Int, max: Int) -> Int {
        guard let value = Int(s), value >= min, value <= max else {
            fail(2, "invalid \(flag): \(s) (expected \(min)...\(max))")
        }
        return value
    }
    while let arg = iterator.next() {
        switch arg {
        case "--registry-id": options.registryID = parseUInt64(nextValue(arg), arg)
        case "--iterations": options.iterations = parseInt(nextValue(arg), arg, min: 1, max: 1000)
        case "--compute-elements": options.computeElements = parseInt(nextValue(arg), arg, min: 1024, max: 1 << 26)
        case "--compute-rounds": options.computeRounds = parseInt(nextValue(arg), arg, min: 1, max: 1 << 16)
        case "--render-size": options.renderSize = parseInt(nextValue(arg), arg, min: 16, max: 8192)
        case "--blit-bytes": options.blitBytes = parseInt(nextValue(arg), arg, min: 4096, max: 1 << 28)
        case "--command-timeout":
            guard let v = Double(nextValue(arg)), v >= 1, v <= 120 else { fail(2, "invalid --command-timeout") }
            options.commandTimeout = v
        case "--max-total-seconds":
            guard let v = Double(nextValue(arg)), v >= 10, v <= 3600 else { fail(2, "invalid --max-total-seconds") }
            options.maxTotalSeconds = v
        case "--enumerate-only": options.enumerateOnly = true
        case "--selfcheck": options.selfcheck = true
        case "--oracle-digest":
            let nonce = parseUInt64(nextValue(arg), arg)
            guard nonce <= UInt64(UInt32.max) else { fail(2, "--oracle-digest nonce exceeds 32 bits") }
            let rounds = parseInt(nextValue(arg), arg, min: 1, max: 1 << 16)
            let count = parseInt(nextValue(arg), arg, min: 1, max: 1 << 26)
            options.oracleDigest = (UInt32(nonce), rounds, count)
        case "--help", "-h":
            FileHandle.standardError.write(Data("See file header for usage.\n".utf8)); exit(0)
        default: fail(2, "unknown argument: \(arg)")
        }
    }
    return options
}

// MARK: - CPU-only self-check (no Metal calls; used for build verification off-target)

func runSelfcheck() -> Never {
    emit("selfcheck_started", ["host": hostEnvironment()])
    var failures = 0
    // Known-answer fixed vector for the mixing recurrence.
    let nonce: UInt32 = 0x12345678
    let input = makeComputeInput(count: 8, nonce: nonce)
    let expected = makeComputeExpected(input, nonce: nonce, rounds: 3)
    // Recompute independently to catch accidental edits to the recurrence.
    func referenceInline(_ v: UInt32) -> UInt32 {
        var x = v ^ nonce
        for _ in 0..<3 {
            x = x &* 1664525 &+ 1013904223; x ^= x >> 15
            x = x &* 2246822519; x ^= x >> 13
        }
        return x
    }
    for i in 0..<input.count where expected[i] != referenceInline(input[i]) {
        failures += 1
    }
    let checksum = expected.reduce(UInt32(0)) { $0 &+ $1 }
    // Statistics sanity.
    let stats = summarize([1, 2, 3, 4])
    if (stats["median"] as? Double) != 2.5 { failures += 1 }
    if (stats["min"] as? Double) != 1 || (stats["max"] as? Double) != 4 { failures += 1 }
    if makePoison(count: 8, nonce: nonce) == expected { failures += 1 }
    // Known-answer tests of the physical-ownership predicate and the host gate.
    let identity = identityPredicateSelftest()
    failures += identity.failures
    emit("selfcheck_completed", [
        "failures": failures, "input_first": Array(input.prefix(4)),
        "expected_first": Array(expected.prefix(4)), "checksum": checksum,
        "fnv1a64": fnv1a64Hex(expected), "identity_cases": identity.cases,
        "identity_failures": identity.failures,
        "passed": failures == 0, "metal_called": false, "gpu_touched": false,
    ])
    exit(failures == 0 ? 0 : 1)
}

/// CPU-only oracle digest used to cross-check this Swift reference against the C++
/// oracle over arbitrary vectors (tests/test_device_lab_host.py). Never calls Metal.
func runOracleDigest(nonce: UInt32, rounds: Int, count: Int) -> Never {
    let input = makeComputeInput(count: count, nonce: nonce)
    let expected = makeComputeExpected(input, nonce: nonce, rounds: rounds)
    emit("oracle_digest_completed", [
        "nonce": nonce, "rounds": rounds, "count": count,
        "expected_first": Array(expected.prefix(4)),
        "checksum": expected.reduce(UInt32(0)) { $0 &+ $1 }, "fnv1a64": fnv1a64Hex(expected),
        "metal_called": false, "gpu_touched": false,
    ])
    exit(0)
}

// MARK: - Command submission with a bounded wait

struct CommandResult {
    var gpuStart: Double
    var gpuEnd: Double
    var wallSeconds: Double
}

func submitAndWait(_ command: MTLCommandBuffer, label: String, timeout: Double, registryID: UInt64) -> CommandResult {
    guard command.device.registryID == registryID else {
        fail(17, "command buffer belongs to another Metal device (\(command.device.registryID) != \(registryID))")
    }
    command.label = label
    let done = DispatchSemaphore(value: 0)
    command.addCompletedHandler { _ in done.signal() }
    let wall0 = DispatchTime.now()
    command.commit()
    guard done.wait(timeout: .now() + timeout) == .success else {
        fail(7, "command '\(label)' exceeded \(timeout)s; terminating the host does NOT cancel the GPU command — preserve logs",
             ["timed_out": true, "clean_cleanup": false])
    }
    let wallSeconds = Double(DispatchTime.now().uptimeNanoseconds - wall0.uptimeNanoseconds) / 1e9
    if command.status != .completed {
        let err = command.error
        fail(8, "command '\(label)' did not complete: status=\(command.status.rawValue) error=\(String(describing: err))",
             ["command_buffer_error": err.map { String(describing: $0) } ?? "none", "status": command.status.rawValue])
    }
    guard command.gpuStartTime > 0, command.gpuEndTime > command.gpuStartTime else {
        fail(19, "command '\(label)' completed without valid GPU timestamps",
             ["gpu_start": command.gpuStartTime, "gpu_end": command.gpuEndTime])
    }
    return CommandResult(gpuStart: command.gpuStartTime, gpuEnd: command.gpuEndTime, wallSeconds: wallSeconds)
}

// MARK: - Device selection bound to the physical GLK GPU

func selectGlkDevice(_ options: BenchOptions) -> (MTLDevice, PciIdentity) {
    let devices = MTLCopyAllDevices()
    emit("device_enumeration", ["device_count": devices.count])
    if devices.isEmpty {
        fail(3, "no Metal devices present — the target currently exposes no IOAccelerator/Metal GPU (expected while the native driver is absent)",
             ["metal_device_count": 0, "metal_tested": false])
    }
    var candidates: [(MTLDevice, PciIdentity)] = []
    for device in devices {
        let pci = pciIdentity(forRegistryID: device.registryID)
        let match = pci.map(evaluateGlkMatch)
        emit("device", [
            "name": device.name, "registry_id": String(device.registryID),
            "is_low_power": device.isLowPower, "is_removable": device.isRemovable,
            "is_headless": device.isHeadless, "supports_metal3": device.supportsFamily(.metal3),
            "location": Int(device.location.rawValue), "location_number": device.locationNumber,
            "has_unified_memory": device.hasUnifiedMemory,
            "pci": pci?.dictionary() ?? ["resolved": false],
            "glk_ids_match": match?.idsMatch ?? false,
            "glk_ownership_attested": match?.ownershipAttested ?? false,
            "glk_reasons": match?.reasons ?? ["pci identity not resolved"],
        ])
        if let pci = pci, let match = match, match.ownershipAttested {
            candidates.append((device, pci))
        }
    }
    if options.enumerateOnly {
        emit("enumerate_only_completed", [
            "glk_candidates": candidates.count, "metal_tested": false,
            "note": "enumeration only; no workloads submitted",
        ])
        exit(candidates.isEmpty ? 3 : 0)
    }
    if let requested = options.registryID {
        guard let hit = devices.first(where: { $0.registryID == requested }) else {
            fail(3, "requested --registry-id \(String(format: "0x%llx", requested)) is not among enumerated Metal devices")
        }
        guard let pci = pciIdentity(forRegistryID: hit.registryID) else {
            fail(11, "requested device has no resolvable PCI provider")
        }
        let match = evaluateGlkMatch(pci)
        guard match.ownershipAttested else {
            fail(11, "requested device is not the physical GLK GPU: \(match.reasons.joined(separator: "; "))",
                 ["pci": pci.dictionary()])
        }
        return (hit, pci)
    }
    guard candidates.count == 1 else {
        fail(candidates.isEmpty ? 11 : 12,
             "expected exactly one Metal device bound to physical GLK 8086:3185; found \(candidates.count)",
             ["glk_candidate_count": candidates.count])
    }
    return candidates[0]
}

// MARK: - Workloads

func runComputeBenchmark(_ device: MTLDevice, _ library: MTLLibrary, _ queue: MTLCommandQueue,
                         registryID: UInt64, options: BenchOptions) -> [String: Any] {
    guard let function = library.makeFunction(name: "glk_compute_mix") else {
        fail(5, "compute function glk_compute_mix missing from compiled library")
    }
    let pipeline: MTLComputePipelineState
    do { pipeline = try device.makeComputePipelineState(function: function) }
    catch { fail(5, "compute pipeline creation failed: \(error)") }

    let count = options.computeElements
    let byteCount = count * MemoryLayout<UInt32>.stride
    guard let outputBuf = device.makeBuffer(length: byteCount, options: .storageModeShared),
          let inputBuf = device.makeBuffer(length: byteCount, options: .storageModeShared),
          let checksumBuf = device.makeBuffer(length: MemoryLayout<UInt32>.stride, options: .storageModeShared) else {
        fail(4, "compute buffer allocation failed for \(byteCount) bytes")
    }
    var gpuTimes: [Double] = []
    var wallTimes: [Double] = []
    var throughputElems: [Double] = []
    let tgWidth = max(1, min(256, pipeline.maxTotalThreadsPerThreadgroup))

    for iteration in 0..<options.iterations {
        let nonce = UInt32.random(in: 1...UInt32.max)
        let input = makeComputeInput(count: count, nonce: nonce)
        let expected = makeComputeExpected(input, nonce: nonce, rounds: options.computeRounds)
        let poison = makePoison(count: count, nonce: nonce)
        let inPtr = inputBuf.contents().bindMemory(to: UInt32.self, capacity: count)
        let outPtr = outputBuf.contents().bindMemory(to: UInt32.self, capacity: count)
        for i in 0..<count { inPtr[i] = input[i]; outPtr[i] = poison[i] }
        checksumBuf.contents().bindMemory(to: UInt32.self, capacity: 1)[0] = 0

        guard let command = queue.makeCommandBuffer(), let encoder = command.makeComputeCommandEncoder() else {
            fail(6, "compute command/encoder creation failed")
        }
        encoder.setComputePipelineState(pipeline)
        encoder.setBuffer(outputBuf, offset: 0, index: 0)
        encoder.setBuffer(inputBuf, offset: 0, index: 1)
        var params = (UInt32(count), nonce, UInt32(options.computeRounds))
        encoder.setBytes(&params, length: MemoryLayout<UInt32>.stride * 3, index: 2)
        encoder.setBuffer(checksumBuf, offset: 0, index: 3)
        let groups = (count + tgWidth - 1) / tgWidth
        encoder.dispatchThreadgroups(MTLSize(width: groups, height: 1, depth: 1),
                                     threadsPerThreadgroup: MTLSize(width: tgWidth, height: 1, depth: 1))
        encoder.endEncoding()
        let result = submitAndWait(command, label: "glk-compute-\(iteration)-\(String(nonce, radix: 16))",
                                   timeout: options.commandTimeout, registryID: registryID)
        // Independent validation of GPU output against the CPU oracle.
        let words = outputBuf.contents().bindMemory(to: UInt32.self, capacity: count)
        var mismatches = 0
        var firstMismatch = -1
        for i in 0..<count where words[i] != expected[i] {
            mismatches += 1; if firstMismatch < 0 { firstMismatch = i }
        }
        let gpuChecksum = checksumBuf.contents().bindMemory(to: UInt32.self, capacity: 1)[0]
        let cpuChecksum = expected.reduce(UInt32(0)) { $0 &+ $1 }
        if mismatches != 0 {
            fail(9, "compute mismatch: \(mismatches) elements differ (first at \(firstMismatch))",
                 ["iteration": iteration, "first_mismatch_index": firstMismatch])
        }
        if gpuChecksum != cpuChecksum {
            fail(9, "compute checksum mismatch: gpu=\(gpuChecksum) cpu=\(cpuChecksum)", ["iteration": iteration])
        }
        let gpuTime = result.gpuEnd - result.gpuStart
        gpuTimes.append(gpuTime); wallTimes.append(result.wallSeconds)
        throughputElems.append(gpuTime > 0 ? Double(count) / gpuTime : 0)
        emit("compute_iteration", [
            "iteration": iteration, "nonce": nonce, "elements": count, "rounds": options.computeRounds,
            "bytes_read": byteCount, "bytes_written": byteCount, "mismatches": 0,
            "gpu_seconds": gpuTime, "wall_seconds": result.wallSeconds,
            "elements_per_second": gpuTime > 0 ? Double(count) / gpuTime : 0,
            "gpu_checksum": gpuChecksum, "output_sha256": sha256Hex(outputBuf.contents(), byteCount),
            "registry_id": String(registryID),
        ])
    }
    return [
        "elements": count, "rounds": options.computeRounds, "iterations": options.iterations,
        "thread_execution_width": pipeline.threadExecutionWidth,
        "max_threads_per_threadgroup": pipeline.maxTotalThreadsPerThreadgroup,
        "gpu_seconds": summarize(gpuTimes), "wall_seconds": summarize(wallTimes),
        "elements_per_second": summarize(throughputElems), "passed": true,
    ]
}

func runRenderBenchmark(_ device: MTLDevice, _ library: MTLLibrary, _ queue: MTLCommandQueue,
                        registryID: UInt64, options: BenchOptions) -> [String: Any] {
    let descriptor = MTLRenderPipelineDescriptor()
    descriptor.vertexFunction = library.makeFunction(name: "glk_fullscreen_vertex")
    descriptor.fragmentFunction = library.makeFunction(name: "glk_constant_fragment")
    descriptor.colorAttachments[0].pixelFormat = .rgba8Unorm
    let pipeline: MTLRenderPipelineState
    do { pipeline = try device.makeRenderPipelineState(descriptor: descriptor) }
    catch { fail(13, "render pipeline creation failed: \(error)") }

    let size = options.renderSize
    let rowStride = ((size * 4) + 255) & ~255 // 256-byte aligned rows
    let readbackBytes = rowStride * size
    let texDescriptor = MTLTextureDescriptor.texture2DDescriptor(pixelFormat: .rgba8Unorm,
                                                                 width: size, height: size, mipmapped: false)
    texDescriptor.storageMode = .private
    texDescriptor.usage = [.renderTarget]
    guard let texture = device.makeTexture(descriptor: texDescriptor),
          let readback = device.makeBuffer(length: readbackBytes, options: .storageModeShared) else {
        fail(12, "render resource creation failed")
    }
    var gpuTimes: [Double] = []
    var wallTimes: [Double] = []
    var mpixPerSecond: [Double] = []

    for iteration in 0..<options.iterations {
        // Deterministic, exactly-representable rgba8 color derived from the iteration.
        let r = UInt8((iteration &* 53 &+ 11) & 0xFF)
        let g = UInt8((iteration &* 97 &+ 29) & 0xFF)
        let b = UInt8((iteration &* 151 &+ 67) & 0xFF)
        let a: UInt8 = 255
        memset(readback.contents(), 0xA5, readbackBytes)
        guard let command = queue.makeCommandBuffer() else { fail(12, "render command creation failed") }
        let pass = MTLRenderPassDescriptor()
        pass.colorAttachments[0].texture = texture
        pass.colorAttachments[0].loadAction = .clear
        pass.colorAttachments[0].storeAction = .store
        pass.colorAttachments[0].clearColor = MTLClearColor(red: 0, green: 0, blue: 0, alpha: 0)
        guard let renderEncoder = command.makeRenderCommandEncoder(descriptor: pass) else {
            fail(13, "render encoder creation failed")
        }
        renderEncoder.setRenderPipelineState(pipeline)
        var color = (Float(r) / 255.0, Float(g) / 255.0, Float(b) / 255.0, Float(a) / 255.0)
        renderEncoder.setFragmentBytes(&color, length: MemoryLayout<Float>.stride * 4, index: 0)
        renderEncoder.drawPrimitives(type: .triangle, vertexStart: 0, vertexCount: 3)
        renderEncoder.endEncoding()
        guard let blit = command.makeBlitCommandEncoder() else { fail(15, "render readback blit creation failed") }
        blit.copy(from: texture, sourceSlice: 0, sourceLevel: 0,
                  sourceOrigin: MTLOrigin(x: 0, y: 0, z: 0),
                  sourceSize: MTLSize(width: size, height: size, depth: 1),
                  to: readback, destinationOffset: 0,
                  destinationBytesPerRow: rowStride, destinationBytesPerImage: readbackBytes)
        blit.endEncoding()
        let result = submitAndWait(command, label: "glk-render-\(iteration)", timeout: options.commandTimeout,
                                   registryID: registryID)
        // Validate every pixel against the known constant color; build the expected
        // padded readback image and compare hashes too.
        let pixels = readback.contents().bindMemory(to: UInt8.self, capacity: readbackBytes)
        var expectedImage = Data(repeating: 0xA5, count: readbackBytes)
        var mismatches = 0
        for row in 0..<size {
            for column in 0..<size {
                let offset = row * rowStride + column * 4
                if pixels[offset] != r || pixels[offset + 1] != g || pixels[offset + 2] != b || pixels[offset + 3] != a {
                    mismatches += 1
                }
                expectedImage[offset] = r; expectedImage[offset + 1] = g
                expectedImage[offset + 2] = b; expectedImage[offset + 3] = a
            }
        }
        if mismatches != 0 {
            fail(14, "render pixel mismatch: \(mismatches) of \(size * size) pixels", ["iteration": iteration])
        }
        let actualHash = sha256Hex(readback.contents(), readbackBytes)
        let expectedHash = sha256Hex(expectedImage)
        if actualHash != expectedHash {
            fail(14, "render readback hash mismatch", ["iteration": iteration])
        }
        let gpuTime = result.gpuEnd - result.gpuStart
        gpuTimes.append(gpuTime); wallTimes.append(result.wallSeconds)
        mpixPerSecond.append(gpuTime > 0 ? Double(size * size) / gpuTime / 1e6 : 0)
        emit("render_iteration", [
            "iteration": iteration, "width": size, "height": size, "pixels": size * size,
            "expected_rgba": [Int(r), Int(g), Int(b), Int(a)], "mismatches": 0,
            "gpu_seconds": gpuTime, "wall_seconds": result.wallSeconds,
            "readback_sha256": actualHash, "registry_id": String(registryID),
        ])
    }
    return [
        "width": size, "height": size, "iterations": options.iterations,
        "gpu_seconds": summarize(gpuTimes), "wall_seconds": summarize(wallTimes),
        "megapixels_per_second": summarize(mpixPerSecond), "passed": true,
    ]
}

func runBlitBenchmark(_ device: MTLDevice, _ queue: MTLCommandQueue,
                      registryID: UInt64, options: BenchOptions) -> [String: Any] {
    let byteCount = options.blitBytes & ~0xFF
    guard let source = device.makeBuffer(length: byteCount, options: .storageModeShared),
          let destination = device.makeBuffer(length: byteCount, options: .storageModeShared) else {
        fail(12, "blit buffer allocation failed for \(byteCount) bytes")
    }
    var gpuTimes: [Double] = []
    var wallTimes: [Double] = []
    var gbPerSecond: [Double] = []
    for iteration in 0..<options.iterations {
        let seed = UInt8((iteration &* 31 &+ 7) & 0xFF)
        let srcPtr = source.contents().bindMemory(to: UInt8.self, capacity: byteCount)
        for i in 0..<byteCount { srcPtr[i] = UInt8((Int(seed) &+ i) & 0xFF) }
        memset(destination.contents(), 0x5A, byteCount)
        guard let command = queue.makeCommandBuffer(), let blit = command.makeBlitCommandEncoder() else {
            fail(12, "blit command/encoder creation failed")
        }
        blit.copy(from: source, sourceOffset: 0, to: destination, destinationOffset: 0, size: byteCount)
        blit.endEncoding()
        let result = submitAndWait(command, label: "glk-blit-\(iteration)", timeout: options.commandTimeout,
                                   registryID: registryID)
        let srcHash = sha256Hex(source.contents(), byteCount)
        let dstHash = sha256Hex(destination.contents(), byteCount)
        if srcHash != dstHash {
            fail(14, "blit copy mismatch: destination != source", ["iteration": iteration])
        }
        let gpuTime = result.gpuEnd - result.gpuStart
        gpuTimes.append(gpuTime); wallTimes.append(result.wallSeconds)
        gbPerSecond.append(gpuTime > 0 ? Double(byteCount) / gpuTime / 1e9 : 0)
        emit("blit_iteration", [
            "iteration": iteration, "bytes": byteCount, "gpu_seconds": gpuTime,
            "wall_seconds": result.wallSeconds, "source_sha256": srcHash, "dest_sha256": dstHash,
            "registry_id": String(registryID),
        ])
    }
    return [
        "bytes": byteCount, "iterations": options.iterations,
        "gpu_seconds": summarize(gpuTimes), "wall_seconds": summarize(wallTimes),
        "gigabytes_per_second": summarize(gbPerSecond), "passed": true,
    ]
}

// MARK: - Entry point

@main
struct GlkMetalBenchMain {
    static func main() {
        gToolName = "glk-metal-bench"
        gSchemaVersion = 1
        let options = parseOptions()

        if options.selfcheck { runSelfcheck() }
        if let digest = options.oracleDigest {
            runOracleDigest(nonce: digest.nonce, rounds: digest.rounds, count: digest.count)
        }

        emit("started", [
            "host": hostEnvironment(), "target": GlkTarget.description(),
            "options": [
                "iterations": options.iterations, "compute_elements": options.computeElements,
                "compute_rounds": options.computeRounds, "render_size": options.renderSize,
                "blit_bytes": options.blitBytes, "command_timeout": options.commandTimeout,
                "max_total_seconds": options.maxTotalSeconds,
                "registry_id": options.registryID.map { String(format: "0x%llx", $0) } ?? "auto",
            ],
            "embedded_msl_sha256": sha256Hex(Data(kEmbeddedMSL.utf8)),
        ])

        // Gate 1: native, untranslated x86_64 process on macOS 15.8.1 (24H32), checked
        // before ANY Metal/IOKit device call. Then arm the whole-process watchdog.
        requireNativeTargetHost()
        let watchdog = armProcessWatchdog(seconds: options.maxTotalSeconds, what: "glk-metal-bench")

        // Gate 2: select the Metal device bound to the physical GLK GPU.
        let (device, pci) = selectGlkDevice(options)
        let registryID = device.registryID
        emit("device_selected", [
            "name": device.name, "registry_id": String(registryID), "pci": pci.dictionary(),
            "supports_metal3": device.supportsFamily(.metal3),
        ])

        // Gate 3: the applicable Metal 3 family is mandatory.
        if !device.supportsFamily(.metal3) {
            fail(23, "selected GLK device does not support the Metal 3 family", ["supports_metal3": false])
        }

        // Gate 4: really compile the embedded MSL on the target GPU's Metal toolchain.
        emit("msl_compile_started", ["source_sha256": sha256Hex(Data(kEmbeddedMSL.utf8))])
        let library: MTLLibrary
        do { library = try device.makeLibrary(source: kEmbeddedMSL, options: nil) }
        catch { fail(5, "runtime MSL compilation failed on device: \(error)") }
        emit("msl_compile_completed", ["function_names": library.functionNames])

        guard let queue = device.makeCommandQueue() else { fail(6, "command queue creation failed") }
        guard queue.device.registryID == registryID else {
            fail(17, "command queue belongs to another Metal device")
        }

        let compute = runComputeBenchmark(device, library, queue, registryID: registryID, options: options)
        let render = runRenderBenchmark(device, library, queue, registryID: registryID, options: options)
        let blit = runBlitBenchmark(device, queue, registryID: registryID, options: options)

        emit("completed", [
            "registry_id": String(registryID), "device_name": device.name,
            "pci": pci.dictionary(), "glk_ownership_attested": true,
            "host": hostEnvironment(), "target": GlkTarget.description(),
            "compute": compute, "render": render, "blit": blit,
            // Honesty flags: what this PUBLIC-API benchmark does and does not prove.
            "metal_tested": true, "metal3_family": true, "cpu_fallback_used": false,
            "public_metal_only": true, "native_driver_fence_evidence": false,
            "full_gpu_execution_attested": false, "windowserver_presentation_verified": false,
            "game_compatibility_attested": false, "hardware_qualification": false,
            "evidence_scope": "public Metal 3 compute/render/blit on the physical GLK GPU, validated against a CPU oracle",
        ])
        watchdog.signal()
        exit(0)
    }
}
