// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Mellow contributors.
// Register offsets and read order from Intel i915 (Linux
// 0d9ff90a5422cc7509258aaaba1e7481df4d332a); see
// Drivers/NativeGlk/RenderWorkarounds.provenance.json.
#pragma once
#include "../Drivers/NativeGlk/RenderWorkarounds.hpp"
#include <IOKit/pci/IOPCIDevice.h>

namespace MellowNative {

// Native input path of RenderWa::decodeTopology() for the bound GLK device:
// the PCI identity from configuration space, then GEN8_FUSE2 and, when slice
// 0 is enabled, GEN9_EU_DISABLE(0), in gen9_sseu_info_init() order, read
// through the owner's GTTMMADR mapping inside a FORCEWAKE_GT window.
//
// The binding proof, the MMIO mapping and the forcewake lease are owned by
// other units (the native device owner and the independently developed GLK
// forcewake unit) and are injected here; no production implementation of
// them is wired yet. A call without all six operations, or whose binding
// check fails, refuses before any configuration or register access. The
// identity is checked before forcewake is requested or MMIO is touched.
enum class GlkTopologyCleanup : uint8_t { Complete, ForcewakeUnknown };

struct GlkTopologyReadOps {
    void *context {};
    // Atomically grant one bounded native-operation hold: retain the owner,
    // device, context and BAR mapping, keep the owning PM epoch awake, and
    // defer detach/reset/PM completion until endAccess. A true result grants
    // exactly one hold; false grants none. Caller must retain device/context
    // until this grant is attempted. These are native-owner operations, not
    // user assertions; no implementation is wired by this adapter.
    bool (*beginAccess)(void *context, IOPCIDevice *device) {};
    // Complete: drop the operation hold after known forcewake cleanup.
    // ForcewakeUnknown: transfer this EXISTING hold to native owner quarantine;
    // retain PM/device/context/BAR and unknown wake obligations until an
    // authoritative native recovery resolves them. Never perform normal free
    // or PM completion on Unknown. The owner must retain its quarantine record
    // independently of the client; this function returns no recovery authority.
    void (*endAccess)(void *context, IOPCIDevice *device, GlkTopologyCleanup) {};
    // True only for the held, current owner/generation with exclusive binding,
    // usable PM epoch and BAR mapping. Revocation must make this false before
    // draining the hold. This is checked between configuration/MMIO accesses.
    bool (*bound)(void *context, IOPCIDevice *device) {};
    // intel_uncore_forcewake_get/put(uncore, FORCEWAKE_GT) semantics: the
    // domain is awake and acknowledged between a true return and release.
    bool (*acquireGtForcewake)(void *context) {};
    // True only for known complete release. False means an unknown native
    // outcome: endAccess receives ForcewakeUnknown and must retain quarantine.
    bool (*releaseGtForcewake)(void *context) {};
    // One 32-bit read at a GTTMMADR offset through the owner's mapping.
    bool (*readRegister)(void *context, uint32_t offset, uint32_t &value) {};
};

enum class GlkTopologyReadStatus : uint8_t {
    Ok,
    MissingDevice,
    MissingOps,
    NotBound,             // before any access, or binding lost by the end
    IdentityRefused,      // decode holds the RenderWa status
    ForcewakeUnavailable,
    RegisterReadFailed,
    TopologyRefused,      // decode holds the RenderWa status
    AccessUnavailable,   // native owner refused an operation/PM/lifetime hold
    ForcewakeReleaseUnknown
};

struct GlkTopologyReadResult {
    GlkTopologyReadStatus status {};
    MellowNativeGlk::RenderWa::Status decode {};
};

// out is written only on Ok, except ReleaseUnknown clears it completely to
// prevent stale topology from escaping as success. beginAccess is paired with
// endAccess; successful forcewake acquire is released first, including failed
// reads/revocation. Externally serialized, immutable callback table; output is
// historical fuse metadata, never a live native-device authority/lease.
GlkTopologyReadResult glkReadTopology(IOPCIDevice *device, const GlkTopologyReadOps &ops,
                                      MellowNativeGlk::RenderWa::Topology &out);

} // namespace MellowNative
