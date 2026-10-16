// GlkDeviceIdentity.swift — shared identity, gating and reporting primitives for the
// MellowGPU GLK device lab. Compiled into every device-lab executable; this file
// contains declarations only (no top-level statements) so each tool supplies its own
// entry point.
//
// SCOPE AND HONESTY BOUNDARIES (enforced in code below, not merely documented):
//   * This is a PUBLIC-API measurement harness. Public Metal/AppKit/IOKit success
//     cannot by itself attest native-driver fences, full third-party GPU execution,
//     WindowServer scanout, or game compatibility. Reports keep those flags false.
//   * The tools run only as a native (untranslated) x86_64 process on macOS 15.8.1
//     (24H32). Any other host fails at the host gate before any Metal/IOKit device call.
//   * The designated first physical device is the user's Lenovo 15IGL7 Intel UHD
//     (Gemini Lake, GLK) GPU: PCI 8086:3185 rev 0x06 subsystem 17AA:3E77 at 00:02.0.
//     A Metal device only counts when its MTLDevice.registryID resolves through real
//     IOService ancestors to an IOPCIDevice (exact class) under an IOPCIBridge, at
//     00:02.0, whose IOPCIFamily data properties carry exactly those IDs, with real PCI
//     resource properties (reg and assigned-addresses). IORegistryCompatibility /
//     synthetic metadata is explicitly NOT physical ownership and is rejected.
//   * No device, a translated (Rosetta) process, or a missing Metal 3 family is a
//     FAILING / UNAVAILABLE result — never a passing skip. There is no CPU fallback.
//
// Public references:
//   Metal          https://developer.apple.com/documentation/metal
//   IOKit registry https://developer.apple.com/documentation/iokit
//   proc_translated https://developer.apple.com/documentation/apple-silicon/about-the-rosetta-translation-environment

import Foundation
import Metal
import IOKit
import CryptoKit

// MARK: - Target identity (facts sourced from the passive preflight; see provenance.json)

enum GlkTarget {
    static let vendorID: UInt32 = 0x8086
    static let deviceID: UInt32 = 0x3185
    static let revisionID: UInt32 = 0x06
    static let subsystemVendorID: UInt32 = 0x17AA
    static let subsystemID: UInt32 = 0x3E77
    // GFX0@2 under AppleACPIPCI in the passive preflight: bus 0, device 2, function 0.
    static let pciBus: UInt32 = 0
    static let pciDevice: UInt32 = 2
    static let pciFunction: UInt32 = 0
    // IORegistryEntryID observed passively in target-passive-preflight.json. Registry
    // IDs are assigned per boot and are informational only; they are never used as a
    // match criterion.
    static let observedPciRegistryID: UInt64 = 4294967814 // 0x100000206
    static let osProductVersion = "15.8.1"
    static let osBuildVersion = "24H32"
    static let kernelArch = "x86_64"

    static func description() -> [String: Any] {
        return [
            "vendor_id": Int(vendorID), "device_id": Int(deviceID), "revision_id": Int(revisionID),
            "subsystem_vendor_id": Int(subsystemVendorID), "subsystem_id": Int(subsystemID),
            "pci_slot": "8086:3185 rev06 subsys 17aa:3e77", "pci_location": "00:02.0",
            "observed_pci_registry_id_hint": String(observedPciRegistryID),
            "expected_os": osProductVersion, "expected_build": osBuildVersion, "expected_arch": kernelArch,
        ]
    }
}

// MARK: - Structured, line-buffered JSONL output

var gToolName = "glk-device-lab"
var gSchemaVersion = 1
private let gEmitLock = NSLock()

private let gISO: ISO8601DateFormatter = {
    let f = ISO8601DateFormatter()
    f.formatOptions = [.withInternetDateTime, .withFractionalSeconds]
    return f
}()

/// Emit one JSON object per line with a single locked write, so that a hang, a watchdog
/// or a GPU timeout can never retroactively turn a partial stage into a successful
/// report and concurrent emitters never interleave a line. Payloads are validated first:
/// JSONSerialization raises an uncatchable exception on invalid objects.
func emit(_ stage: String, _ details: [String: Any] = [:]) {
    gEmitLock.lock()
    defer { gEmitLock.unlock() }
    var object = details
    object["tool"] = gToolName
    object["schema_version"] = gSchemaVersion
    object["stage"] = stage
    object["utc"] = gISO.string(from: Date())
    guard JSONSerialization.isValidJSONObject(object),
          var line = try? JSONSerialization.data(withJSONObject: object, options: [.sortedKeys]) else {
        FileHandle.standardError.write(Data("emit: non-serializable payload for stage \(stage)\n".utf8))
        return
    }
    line.append(0x0A)
    FileHandle.standardOutput.write(line)
}

private func failurePayload(_ code: Int32, _ message: String, _ extra: [String: Any]) -> [String: Any] {
    var payload = extra
    payload["code"] = Int(code)
    payload["error"] = message
    payload["public_metal_only"] = true
    payload["native_driver_fence_evidence"] = false
    payload["full_gpu_execution_attested"] = false
    payload["hardware_qualification"] = false
    return payload
}

/// Fatal exit. A failure is always an explicit non-zero status with evidence; this
/// is the only sanctioned way to end a tool that did not fully succeed.
func fail(_ code: Int32, _ message: String, _ extra: [String: Any] = [:]) -> Never {
    emit("failed", failurePayload(code, message, extra))
    exit(code)
}

/// Fatal exit from a watchdog thread: _exit() skips atexit handlers that could deadlock
/// against a main thread blocked inside Metal/AppKit.
func failImmediately(_ code: Int32, _ message: String, _ extra: [String: Any] = [:]) -> Never {
    emit("failed", failurePayload(code, message, extra))
    _exit(code)
}

/// Process-level hard watchdog. Unless the returned semaphore is signalled within
/// `seconds`, the process emits a fatal, explicitly unclean failure and exits: a hang can
/// never become a silent or successful result.
func armProcessWatchdog(seconds: Double, what: String) -> DispatchSemaphore {
    let done = DispatchSemaphore(value: 0)
    DispatchQueue.global(qos: .userInitiated).async {
        if done.wait(timeout: .now() + seconds) == .timedOut {
            failImmediately(7, "\(what) exceeded its \(seconds)s hard watchdog; terminating the process does NOT cancel in-flight GPU/WindowServer work",
                            ["timed_out": true, "clean_cleanup": false])
        }
    }
    return done
}

// MARK: - Hashing helpers

func sha256Hex(_ data: Data) -> String {
    SHA256.hash(data: data).map { String(format: "%02x", $0) }.joined()
}
func sha256Hex(_ pointer: UnsafeRawPointer, _ count: Int) -> String {
    sha256Hex(Data(bytes: pointer, count: count))
}
func sha256Hex(_ words: [UInt32]) -> String {
    words.withUnsafeBufferPointer { buffer in
        sha256Hex(buffer.baseAddress!, buffer.count * MemoryLayout<UInt32>.stride)
    }
}

/// FNV-1a 64 over the little-endian bytes of each word; matches tests/glk_oracle_tests.cpp.
func fnv1a64Hex(_ words: [UInt32]) -> String {
    var hash: UInt64 = 0xcbf29ce484222325
    for word in words {
        var w = word
        for _ in 0..<4 {
            hash ^= UInt64(w & 0xFF)
            hash = hash &* 0x100000001b3
            w >>= 8
        }
    }
    return String(format: "%016llx", hash)
}

// MARK: - Host / process environment

/// Returns 1 under Rosetta translation, 0 for native, -1 when the sysctl is absent.
/// Apple documents ENOENT as a native-process answer for sysctl.proc_translated.
func procTranslated() -> Int32 {
    var value: Int32 = 0
    var size = MemoryLayout<Int32>.size
    let rc = sysctlbyname("sysctl.proc_translated", &value, &size, nil, 0)
    if rc != 0 { return errno == ENOENT ? 0 : -1 }
    return value
}

func sysctlInt32(_ name: String) -> Int32? {
    var value: Int32 = 0
    var size = MemoryLayout<Int32>.size
    return sysctlbyname(name, &value, &size, nil, 0) == 0 ? value : nil
}

func sysctlString(_ name: String) -> String? {
    var size = 0
    if sysctlbyname(name, nil, &size, nil, 0) != 0 || size == 0 { return nil }
    var buffer = [CChar](repeating: 0, count: size)
    if sysctlbyname(name, &buffer, &size, nil, 0) != 0 { return nil }
    return String(cString: buffer)
}

func hostEnvironment() -> [String: Any] {
    let info = ProcessInfo.processInfo
    return [
        "os_version_string": info.operatingSystemVersionString,
        "kern_osproductversion": sysctlString("kern.osproductversion") ?? "unknown",
        "kern_osversion": sysctlString("kern.osversion") ?? "unknown",
        "kern_version": sysctlString("kern.version") ?? "unknown",
        "hw_model": sysctlString("hw.model") ?? "unknown",
        "machdep_cpu_brand": sysctlString("machdep.cpu.brand_string") ?? "unknown",
        "proc_translated": Int(procTranslated()),
        "hw_optional_arm64": sysctlInt32("hw.optional.arm64").map { Int($0) as Any } ?? NSNull(),
        "process_arch": currentArchName(),
        "pid": info.processIdentifier,
        "physical_memory": info.physicalMemory,
        "active_processor_count": info.activeProcessorCount,
    ]
}

func currentArchName() -> String {
    #if arch(x86_64)
    return "x86_64"
    #elseif arch(arm64)
    return "arm64"
    #else
    return "unknown"
    #endif
}

// MARK: - Host gate: native x86_64 macOS 15.8.1 (24H32) only

struct HostFacts {
    var translated: Int32
    var processArch: String
    var osBuild: String
    var osProduct: String
    var appleSiliconHost: Bool

    func dictionary() -> [String: Any] {
        return ["proc_translated": Int(translated), "process_arch": processArch, "kern_osversion": osBuild,
                "kern_osproductversion": osProduct, "apple_silicon_host": appleSiliconHost]
    }
}

struct GateViolation {
    var code: Int32
    var reason: String
}

func currentHostFacts() -> HostFacts {
    HostFacts(translated: procTranslated(), processArch: currentArchName(),
              osBuild: sysctlString("kern.osversion") ?? "unknown",
              osProduct: sysctlString("kern.osproductversion") ?? "unknown",
              appleSiliconHost: sysctlInt32("hw.optional.arm64") == 1)
}

/// Pure decision for the host gate (unit-tested by the self-checks).
func evaluateHostGate(_ h: HostFacts) -> [GateViolation] {
    var violations: [GateViolation] = []
    if h.translated != 0 {
        violations.append(GateViolation(code: 22, reason: "process is translated or its translation state is unknown (proc_translated=\(h.translated)); not native GPU evidence"))
    }
    if h.processArch != GlkTarget.kernelArch {
        violations.append(GateViolation(code: 26, reason: "process arch \(h.processArch) is not \(GlkTarget.kernelArch)"))
    }
    if h.appleSiliconHost {
        violations.append(GateViolation(code: 26, reason: "host is Apple silicon (hw.optional.arm64=1), not the physical x86_64 GLK laptop"))
    }
    if h.osBuild != GlkTarget.osBuildVersion {
        violations.append(GateViolation(code: 27, reason: "macOS build \(h.osBuild) is not \(GlkTarget.osBuildVersion)"))
    }
    if h.osProduct != GlkTarget.osProductVersion {
        violations.append(GateViolation(code: 27, reason: "macOS \(h.osProduct) is not \(GlkTarget.osProductVersion)"))
    }
    return violations
}

/// Must run before ANY Metal / IOKit device / WindowServer call.
func requireNativeTargetHost() {
    let facts = currentHostFacts()
    let violations = evaluateHostGate(facts)
    if let first = violations.first {
        fail(first.code, first.reason, ["host_gate": facts.dictionary(), "metal_called": false,
                                        "host_gate_violations": violations.map { $0.reason }])
    }
    emit("host_gate_passed", ["host_gate": facts.dictionary()])
}

// MARK: - PCI identity resolved through real IOService ancestors

struct PciIdentity {
    var vendorID: UInt32?
    var deviceID: UInt32?
    var revisionID: UInt32?
    var subsystemVendorID: UInt32?
    var subsystemID: UInt32?
    var regPhysHi: UInt32?
    var pciRegistryID: UInt64 = 0
    var providerClass: String = "unknown"
    var parentClass: String = "unknown"
    var parentIsPCIBridge = false
    var registryPath: String = "unknown"
    var ancestorClasses: [String] = []
    var assignedAddressesBytes: Int = 0
    var regBytes: Int = 0
    var compatible: [String] = []
    var syntheticIdentityKeys: [String] = []

    /// bus:device.function decoded from the first `reg` cell (phys.hi).
    var locationString: String {
        guard let hi = regPhysHi else { return "unresolved" }
        return String(format: "%02x:%02x.%x", (hi >> 16) & 0xFF, (hi >> 11) & 0x1F, (hi >> 8) & 0x7)
    }

    func dictionary() -> [String: Any] {
        func value(_ v: UInt32?) -> Any { v.map { Int($0) as Any } ?? NSNull() }
        return [
            "vendor_id": value(vendorID),
            "device_id": value(deviceID),
            "revision_id": value(revisionID),
            "subsystem_vendor_id": value(subsystemVendorID),
            "subsystem_id": value(subsystemID),
            "pci_location": locationString,
            "pci_registry_id": String(pciRegistryID),
            "provider_class": providerClass,
            "parent_class": parentClass,
            "parent_is_pci_bridge": parentIsPCIBridge,
            "registry_path": registryPath,
            "ancestor_classes": ancestorClasses,
            "assigned_addresses_bytes": assignedAddressesBytes,
            "reg_bytes": regBytes,
            "compatible": compatible,
            "synthetic_identity_keys": syntheticIdentityKeys,
        ]
    }
}

/// IOPCIFamily publishes config-space IDs as little-endian 32-bit OSData. Numbers or
/// strings are not config-space evidence and are rejected (nil).
func readUInt32Property(_ entry: io_registry_entry_t, _ key: String) -> UInt32? {
    guard let raw = IORegistryEntryCreateCFProperty(entry, key as CFString, kCFAllocatorDefault, 0)?
        .takeRetainedValue(), let data = raw as? Data, data.count >= 4 else { return nil }
    let b = Array(data.prefix(4))
    return UInt32(b[0]) | (UInt32(b[1]) << 8) | (UInt32(b[2]) << 16) | (UInt32(b[3]) << 24)
}

func readDataLength(_ entry: io_registry_entry_t, _ key: String) -> Int {
    guard let raw = IORegistryEntryCreateCFProperty(entry, key as CFString, kCFAllocatorDefault, 0)?
        .takeRetainedValue() else { return 0 }
    return (raw as? Data)?.count ?? 0
}

func readStringArrayFromData(_ entry: io_registry_entry_t, _ key: String) -> [String] {
    guard let raw = IORegistryEntryCreateCFProperty(entry, key as CFString, kCFAllocatorDefault, 0)?
        .takeRetainedValue() else { return [] }
    if let data = raw as? Data {
        // IOKit "compatible" is a NUL-separated packed C-string list.
        return data.split(separator: 0x00).compactMap { String(data: Data($0), encoding: .utf8) }
            .filter { !$0.isEmpty }
    }
    if let s = raw as? String { return [s] }
    return []
}

func objectClassName(_ entry: io_registry_entry_t) -> String {
    if let cf = IOObjectCopyClass(entry) { return cf.takeRetainedValue() as String }
    return "unknown"
}

func registryPath(_ entry: io_registry_entry_t) -> String {
    var buffer = [CChar](repeating: 0, count: 1024)
    if IORegistryEntryGetPath(entry, kIOServicePlane, &buffer) == KERN_SUCCESS {
        return String(cString: buffer)
    }
    return "unknown"
}

/// Walk the IOService ancestor chain from the node owning `registryID` until a real
/// IOPCIDevice is found, capturing the properties needed to prove (or refute)
/// physical ownership of the GLK GPU.
func pciIdentity(forRegistryID registryID: UInt64) -> PciIdentity? {
    var entry = IOServiceGetMatchingService(kIOMainPortDefault, IORegistryEntryIDMatching(registryID))
    guard entry != 0 else { return nil }
    var ancestry: [String] = []
    defer { if entry != 0 { IOObjectRelease(entry) } }
    for _ in 0..<32 {
        let cls = objectClassName(entry)
        ancestry.append(cls)
        if IOObjectConformsTo(entry, "IOPCIDevice") != 0 {
            var identity = PciIdentity()
            identity.vendorID = readUInt32Property(entry, "vendor-id")
            identity.deviceID = readUInt32Property(entry, "device-id")
            identity.revisionID = readUInt32Property(entry, "revision-id")
            identity.subsystemVendorID = readUInt32Property(entry, "subsystem-vendor-id")
            identity.subsystemID = readUInt32Property(entry, "subsystem-id")
            identity.regPhysHi = readUInt32Property(entry, "reg")
            var pciID: UInt64 = 0
            IORegistryEntryGetRegistryEntryID(entry, &pciID)
            identity.pciRegistryID = pciID
            identity.providerClass = cls
            identity.registryPath = registryPath(entry)
            identity.ancestorClasses = ancestry
            identity.assignedAddressesBytes = readDataLength(entry, "assigned-addresses")
            identity.regBytes = readDataLength(entry, "reg")
            identity.compatible = readStringArrayFromData(entry, "compatible")
            var parent: io_registry_entry_t = 0
            if IORegistryEntryGetParentEntry(entry, kIOServicePlane, &parent) == KERN_SUCCESS, parent != 0 {
                identity.parentClass = objectClassName(parent)
                identity.parentIsPCIBridge = IOObjectConformsTo(parent, "IOPCIBridge") != 0
                IOObjectRelease(parent)
            }
            // Synthetic/compatibility-shim markers. Presence of any of these means the
            // IDs are not proof of physical ownership.
            let suspectKeys = ["MellowPhysicalVendorID", "MellowPhysicalDeviceID", "MellowPhysicalBDF",
                               "MellowPhysicalIdentitySource", "IOKitRegistryCompatibility",
                               "IORegistryCompatible", "MellowSyntheticIdentity"]
            for key in suspectKeys where
                IORegistryEntryCreateCFProperty(entry, key as CFString, kCFAllocatorDefault, 0)?
                    .takeRetainedValue() != nil {
                identity.syntheticIdentityKeys.append(key)
            }
            return identity
        }
        var parent: io_registry_entry_t = 0
        let status = IORegistryEntryGetParentEntry(entry, kIOServicePlane, &parent)
        IOObjectRelease(entry)
        entry = 0
        if status != KERN_SUCCESS || parent == 0 { return nil }
        entry = parent
    }
    return nil
}

struct GlkMatch {
    var ownershipAttested: Bool
    var idsMatch: Bool
    var reasons: [String]
}

/// Decide whether a resolved PCI identity is the genuine GLK GPU. `ownershipAttested`
/// requires the exact IDs AND evidence of the real PCI node: exact IOPCIDevice class
/// under an IOPCIBridge, location 00:02.0, reg + assigned-addresses resources, and no
/// synthetic identity keys.
func evaluateGlkMatch(_ pci: PciIdentity) -> GlkMatch {
    var reasons: [String] = []
    func check(_ name: String, _ actual: UInt32?, _ expected: UInt32) {
        if actual != expected {
            reasons.append("\(name) mismatch: got \(actual.map { String(format: "0x%x", $0) } ?? "nil"), "
                + "expected \(String(format: "0x%x", expected))")
        }
    }
    check("vendor_id", pci.vendorID, GlkTarget.vendorID)
    check("device_id", pci.deviceID, GlkTarget.deviceID)
    check("revision_id", pci.revisionID, GlkTarget.revisionID)
    check("subsystem_vendor_id", pci.subsystemVendorID, GlkTarget.subsystemVendorID)
    check("subsystem_id", pci.subsystemID, GlkTarget.subsystemID)
    let idsMatch = reasons.isEmpty
    if !pci.syntheticIdentityKeys.isEmpty {
        reasons.append("synthetic identity keys present (not physical ownership): "
            + pci.syntheticIdentityKeys.joined(separator: ","))
    }
    if pci.regBytes < 20 {
        reasons.append("no PCI config-space reg entry (\(pci.regBytes) bytes)")
    }
    if pci.assignedAddressesBytes < 20 {
        reasons.append("no assigned-addresses PCI resources (\(pci.assignedAddressesBytes) bytes)")
    }
    let expectedLocation = String(format: "%02x:%02x.%x", GlkTarget.pciBus, GlkTarget.pciDevice, GlkTarget.pciFunction)
    if pci.locationString != expectedLocation {
        reasons.append("PCI location \(pci.locationString) is not \(expectedLocation)")
    }
    if pci.providerClass != "IOPCIDevice" {
        reasons.append("provider class \(pci.providerClass) is not exactly IOPCIDevice")
    }
    if !pci.parentIsPCIBridge {
        reasons.append("IOPCIDevice parent \(pci.parentClass) is not an IOPCIBridge")
    }
    return GlkMatch(ownershipAttested: reasons.isEmpty, idsMatch: idsMatch, reasons: reasons)
}

/// CPU-only known-answer tests of the ownership predicate and the host gate using
/// in-memory fixtures. The fixtures never take part in device selection.
func identityPredicateSelftest() -> (failures: Int, cases: [[String: Any]]) {
    var physical = PciIdentity()
    physical.vendorID = 0x8086; physical.deviceID = 0x3185; physical.revisionID = 0x06
    physical.subsystemVendorID = 0x17AA; physical.subsystemID = 0x3E77
    physical.regPhysHi = 0x0000_1000; physical.regBytes = 80; physical.assignedAddressesBytes = 60
    physical.providerClass = "IOPCIDevice"; physical.parentClass = "AppleACPIPCI"; physical.parentIsPCIBridge = true

    var pciCases: [(String, PciIdentity, Bool)] = [("exact_physical_glk", physical, true)]
    func variant(_ name: String, _ mutate: (inout PciIdentity) -> Void) {
        var p = physical; mutate(&p); pciCases.append((name, p, false))
    }
    variant("wrong_vendor") { $0.vendorID = 0x1002 }
    variant("wrong_device") { $0.deviceID = 0x3184 }
    variant("wrong_revision") { $0.revisionID = 0x03 }
    variant("wrong_subsystem_vendor") { $0.subsystemVendorID = 0x1028 }
    variant("wrong_subsystem") { $0.subsystemID = 0x0000 }
    variant("ids_not_data_properties") { $0.vendorID = nil }
    variant("synthetic_identity_key") { $0.syntheticIdentityKeys = ["MellowPhysicalVendorID"] }
    variant("no_assigned_addresses") { $0.assignedAddressesBytes = 0 }
    variant("no_reg") { $0.regBytes = 0; $0.regPhysHi = nil }
    variant("wrong_location_00_01_0") { $0.regPhysHi = 0x0000_0800 }
    variant("wrong_bus") { $0.regPhysHi = 0x0001_1000 }
    variant("parent_not_bridge") { $0.parentIsPCIBridge = false; $0.parentClass = "MellowSyntheticNub" }
    variant("shim_provider_class") { $0.providerClass = "IOKitRegistryCompatibility" }

    var failures = 0
    var cases: [[String: Any]] = []
    for (name, pci, expected) in pciCases {
        let got = evaluateGlkMatch(pci).ownershipAttested
        if got != expected { failures += 1 }
        cases.append(["case": name, "expected_attested": expected, "attested": got])
    }

    let target = HostFacts(translated: 0, processArch: "x86_64", osBuild: "24H32", osProduct: "15.8.1",
                           appleSiliconHost: false)
    var hostCases: [(String, HostFacts, Int32)] = [("target_host", target, 0)]
    func hostVariant(_ name: String, _ code: Int32, _ mutate: (inout HostFacts) -> Void) {
        var h = target; mutate(&h); hostCases.append((name, h, code))
    }
    hostVariant("rosetta_translated", 22) { $0.translated = 1 }
    hostVariant("translation_unknown", 22) { $0.translated = -1 }
    hostVariant("arm64_process", 26) { $0.processArch = "arm64" }
    hostVariant("apple_silicon_host", 26) { $0.appleSiliconHost = true }
    hostVariant("other_build", 27) { $0.osBuild = "24G90" }
    hostVariant("other_product", 27) { $0.osProduct = "15.7" }
    for (name, facts, expectedCode) in hostCases {
        let got = evaluateHostGate(facts).first?.code ?? 0
        if got != expectedCode { failures += 1 }
        cases.append(["case": name, "expected_gate_code": Int(expectedCode), "gate_code": Int(got)])
    }
    return (failures, cases)
}

// MARK: - Deterministic, CPU-verifiable compute reference

/// A fixed integer mixing kernel used as a GPU compute workload. It is overflow-exact
/// on both CPU and GPU (uint32 wraparound), so the CPU result is an independent oracle
/// for the GPU output. `rounds` scales arithmetic intensity for throughput sampling.
func computeReferenceWord(_ input: UInt32, nonce: UInt32, rounds: Int) -> UInt32 {
    var x = input ^ nonce
    var i = 0
    while i < rounds {
        x = x &* 1664525 &+ 1013904223
        x ^= x >> 15
        x = x &* 2246822519
        x ^= x >> 13
        i += 1
    }
    return x
}

func makeComputeInput(count: Int, nonce: UInt32) -> [UInt32] {
    var state = nonce ^ 0xC001D00D
    var input = [UInt32](repeating: 0, count: count)
    for i in 0..<count {
        state = state &* 1664525 &+ 1013904223
        input[i] = i < 4 ? UInt32(i + 1) : state ^ UInt32(truncatingIfNeeded: i)
    }
    return input
}

func makeComputeExpected(_ input: [UInt32], nonce: UInt32, rounds: Int) -> [UInt32] {
    input.map { computeReferenceWord($0, nonce: nonce, rounds: rounds) }
}

func makePoison(count: Int, nonce: UInt32) -> [UInt32] {
    var state = nonce ^ 0x51A7E5ED
    var poison = [UInt32](repeating: 0, count: count)
    for i in 0..<count {
        state = state &* 1664525 &+ 1013904223
        poison[i] = state ^ (UInt32(truncatingIfNeeded: i) &* 0x9E3779B9)
    }
    return poison
}

// MARK: - Simple statistics for pacing / timing samples

func summarize(_ samples: [Double]) -> [String: Any] {
    guard !samples.isEmpty else { return ["count": 0] }
    let sorted = samples.sorted()
    let n = sorted.count
    let sum = sorted.reduce(0, +)
    let mean = sum / Double(n)
    func pct(_ p: Double) -> Double {
        if n == 1 { return sorted[0] }
        let rank = p / 100.0 * Double(n - 1)
        let lo = Int(rank.rounded(.down)); let hi = Int(rank.rounded(.up))
        let frac = rank - Double(lo)
        return sorted[lo] + (sorted[hi] - sorted[lo]) * frac
    }
    let variance = sorted.reduce(0.0) { $0 + ($1 - mean) * ($1 - mean) } / Double(n)
    return [
        "count": n, "min": sorted.first!, "max": sorted.last!, "mean": mean,
        "median": pct(50), "p95": pct(95), "p99": pct(99), "stddev": variance.squareRoot(),
    ]
}
