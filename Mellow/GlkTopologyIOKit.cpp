// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Mellow contributors.
#include "GlkTopologyIOKit.hpp"

namespace MellowNative {
namespace RenderWa = MellowNativeGlk::RenderWa;
namespace {
// PCI type 0 header offsets; checked against the SDK header at compile time.
static_assert(kIOPCIConfigVendorID == 0x00 && kIOPCIConfigDeviceID == 0x02 &&
              kIOPCIConfigRevisionID == 0x08 && kIOPCIConfigSubSystemVendorID == 0x2c &&
              kIOPCIConfigSubSystemID == 0x2e, "PCI configuration header offsets");

// GEN8_F2_S_ENA_MASK is REG_GENMASK(27, 25); bit 25 enables slice 0.
constexpr uint32_t Fuse2SliceZero = 1U << 25;

GlkTopologyReadResult result(GlkTopologyReadStatus status,
                             RenderWa::Status decode = RenderWa::Status::Ok) {
    GlkTopologyReadResult r;
    r.status = status;
    r.decode = decode;
    return r;
}

// The public KPI overloads take IOByteCount and forward to
// extendedConfigRead16/8(); the UInt8 virtuals are __IOPCIDEVICE_INTERNAL__.
UInt16 config16(IOPCIDevice *device, unsigned offset) {
    return device->configRead16(static_cast<IOByteCount>(offset));
}
UInt8 config8(IOPCIDevice *device, unsigned offset) {
    return device->configRead8(static_cast<IOByteCount>(offset));
}

class AccessScope {
public:
    AccessScope(IOPCIDevice *device, const GlkTopologyReadOps &ops) : device_(device), ops_(ops) {}
    ~AccessScope() { if (active_) (void)finish(); }
    GlkTopologyCleanup releaseForcewake() {
        if (forcewake_) {
            forcewake_ = false;
            if (!ops_.releaseGtForcewake(ops_.context)) cleanup_ = GlkTopologyCleanup::ForcewakeUnknown;
        }
        return cleanup_;
    }
    GlkTopologyCleanup finish() {
        const auto cleanup = releaseForcewake();
        active_ = false;
        ops_.endAccess(ops_.context, device_, cleanup);
        return cleanup;
    }
    void acquiredForcewake() { forcewake_ = true; }
    AccessScope(const AccessScope &) = delete;
    AccessScope &operator=(const AccessScope &) = delete;
private:
    IOPCIDevice *device_;
    const GlkTopologyReadOps ops_; // callback cleanup cannot be changed mid-call
    bool forcewake_ = false;
    bool active_ = true;
    GlkTopologyCleanup cleanup_ = GlkTopologyCleanup::Complete;
};

GlkTopologyReadResult readBorrowed(IOPCIDevice *device, const GlkTopologyReadOps &ops,
                                    AccessScope &scope, RenderWa::Topology &out) {
    if (!ops.bound(ops.context, device)) return result(GlkTopologyReadStatus::NotBound);

    RenderWa::PciIdentity id;
    id.vendor = config16(device, kIOPCIConfigVendorID);
    if (!ops.bound(ops.context, device)) return result(GlkTopologyReadStatus::NotBound);
    id.device = config16(device, kIOPCIConfigDeviceID);
    if (!ops.bound(ops.context, device)) return result(GlkTopologyReadStatus::NotBound);
    id.revision = config8(device, kIOPCIConfigRevisionID);
    if (!ops.bound(ops.context, device)) return result(GlkTopologyReadStatus::NotBound);
    id.subsystemVendor = config16(device, kIOPCIConfigSubSystemVendorID);
    if (!ops.bound(ops.context, device)) return result(GlkTopologyReadStatus::NotBound);
    id.subsystem = config16(device, kIOPCIConfigSubSystemID);
    if (!ops.bound(ops.context, device)) return result(GlkTopologyReadStatus::NotBound);
    id.present = true;
    RenderWa::Platform platform;
    RenderWa::Status status = RenderWa::identifyPlatform(id, platform);
    if (status != RenderWa::Status::Ok) return result(GlkTopologyReadStatus::IdentityRefused, status);

    if (!ops.acquireGtForcewake(ops.context)) return result(GlkTopologyReadStatus::ForcewakeUnavailable);
    scope.acquiredForcewake();
    if (!ops.bound(ops.context, device)) return result(GlkTopologyReadStatus::NotBound);
    RenderWa::FuseReadings fuses;
    bool read = ops.readRegister(ops.context, RenderWa::Fuse2Offset, fuses.fuse2);
    if (!ops.bound(ops.context, device)) return result(GlkTopologyReadStatus::NotBound);
    // gen9_sseu_info_init() reads GEN9_EU_DISABLE(s) only for enabled slices.
    if (read && (fuses.fuse2 & Fuse2SliceZero))
        read = ops.readRegister(ops.context, RenderWa::EuDisable0Offset, fuses.euDisable0);
    if (!ops.bound(ops.context, device)) return result(GlkTopologyReadStatus::NotBound);
    if (!read) return result(GlkTopologyReadStatus::RegisterReadFailed);
    // The values are only attributable to this device if it stayed bound.
    if (!ops.bound(ops.context, device)) return result(GlkTopologyReadStatus::NotBound);
    fuses.present = true;

    RenderWa::Topology topology;
    status = RenderWa::decodeTopology(id, fuses, topology);
    if (status != RenderWa::Status::Ok) return result(GlkTopologyReadStatus::TopologyRefused, status);
    out = topology;
    return result(GlkTopologyReadStatus::Ok);
}
} // namespace

GlkTopologyReadResult glkReadTopology(IOPCIDevice *device, const GlkTopologyReadOps &providedOps,
                                      RenderWa::Topology &out) {
    const GlkTopologyReadOps ops = providedOps;
    if (!device) return result(GlkTopologyReadStatus::MissingDevice);
    if (!ops.beginAccess || !ops.endAccess || !ops.bound || !ops.acquireGtForcewake ||
        !ops.releaseGtForcewake || !ops.readRegister)
        return result(GlkTopologyReadStatus::MissingOps);
    if (!ops.beginAccess(ops.context, device)) return result(GlkTopologyReadStatus::AccessUnavailable);
    AccessScope scope(device, ops);
    RenderWa::Topology candidate;
    auto r = readBorrowed(device, ops, scope, candidate);
    // No topology publication precedes forcewake cleanup. A failed release
    // cannot be overwritten by an earlier successful fuse decode/read result.
    if (scope.releaseForcewake() == GlkTopologyCleanup::ForcewakeUnknown) {
        auto *bytes = reinterpret_cast<uint8_t *>(&out);
        for (size_t i = 0; i < sizeof out; ++i) bytes[i] = 0;
        (void)scope.finish(); // transfer the existing access hold to quarantine
        return result(GlkTopologyReadStatus::ForcewakeReleaseUnknown);
    }
    if (r.status == GlkTopologyReadStatus::Ok && !ops.bound(ops.context, device))
        r = result(GlkTopologyReadStatus::NotBound);
    if (r.status == GlkTopologyReadStatus::Ok) out = candidate;
    (void)scope.finish(); // output handling also completed under the held lease
    return r;
}

} // namespace MellowNative
