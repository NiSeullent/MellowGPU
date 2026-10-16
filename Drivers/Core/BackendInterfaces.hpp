// SPDX-License-Identifier: MIT
#ifndef MELLOWGPU_BACKEND_INTERFACES_HPP
#define MELLOWGPU_BACKEND_INTERFACES_HPP
#include <stdint.h>

// Internal C++ contract v1.0; NOT the wire ABI, kernel ABI or hardware admission.
// These initial adapters bind metadata only. No GPU operation reports success.
// Single owner: bind before publication; externally serialize every call.
// One instance per (registry, epoch). Destroy it after its consumer lifetime;
// it cannot detect native hotplug/reset or attest the caller-provided identity.
namespace MellowGPUCore { namespace Backend {
enum class KernelArchitecture : uint8_t { Unknown, X86_64, Arm64, Arm64e };
enum class Operation : uint8_t {
    Describe, CreateResource, CreateQueue, Submit, ObserveFence, Recover, Present,
    VendorExtension
};
enum class Status : uint8_t {
    NotBound, WrongVendor, InvalidIdentity, UnknownArchitecture, IdentityBound,
    AlreadyBound, MetadataOnly, UnsupportedABI, WrongDevice, StaleEpoch,
    Unsupported, UnsupportedOperation, BindingConflict
};
struct Identity {
    uint16_t vendor {0}; // PCI GPU vendor, never CPU vendor.
    uint16_t device {0};
    uint32_t revision {0};
    uint64_t registry {0};
    uint64_t epoch {0};
    KernelArchitecture architecture {KernelArchitecture::Unknown};
};
struct Request {
    uint16_t major {0};
    uint16_t minor {0};
    Operation operation {Operation::Describe};
    uint64_t registry {0};
    uint64_t epoch {0};
};
struct Output {
    Identity identity {};
    uint64_t capabilities {0};
    uint64_t object {0};
    bool nativeInitialized {false};
};
template<uint16_t Vendor> class MetadataInterfaceV1 final {
    static_assert(Vendor == 0x8086 || Vendor == 0x1002 || Vendor == 0x10de,
                  "v1 metadata adapters cover these three PCI vendors only");
public:
    MetadataInterfaceV1() = default;
    MetadataInterfaceV1(const MetadataInterfaceV1 &) = delete;
    MetadataInterfaceV1 &operator=(const MetadataInterfaceV1 &) = delete;
    MetadataInterfaceV1(MetadataInterfaceV1 &&) = delete;
    MetadataInterfaceV1 &operator=(MetadataInterfaceV1 &&) = delete;
    Status bindIdentity(const Identity &identity) {
        if (identity.vendor != Vendor) return Status::WrongVendor;
        if (!identity.device || identity.device == 0xffff || !identity.registry || !identity.epoch)
            return Status::InvalidIdentity;
        switch (identity.architecture) {
            case KernelArchitecture::X86_64:
            case KernelArchitecture::Arm64:
            case KernelArchitecture::Arm64e: break;
            default: return Status::UnknownArchitecture;
        }
        if (bound_) {
            const bool same = identity.vendor == identity_.vendor && identity.device == identity_.device &&
                identity.revision == identity_.revision && identity.registry == identity_.registry &&
                identity.epoch == identity_.epoch && identity.architecture == identity_.architecture;
            return same ? Status::AlreadyBound : Status::BindingConflict;
        }
        identity_ = identity;
        bound_ = true;
        return Status::IdentityBound;
    }
    Status call(const Request &request, Output &out) const {
        out = Output {};
        if (!bound_) return Status::NotBound;
        if (request.major != 1 || request.minor != 0) return Status::UnsupportedABI;
        if (request.registry != identity_.registry) return Status::WrongDevice;
        if (request.epoch != identity_.epoch) return Status::StaleEpoch;
        switch (request.operation) {
            case Operation::Describe:
                out.identity = identity_;
                return Status::MetadataOnly;
            case Operation::CreateResource:
            case Operation::CreateQueue:
            case Operation::Submit:
            case Operation::ObserveFence:
            case Operation::Recover:
            case Operation::Present:
            case Operation::VendorExtension: return Status::Unsupported;
            default: return Status::UnsupportedOperation;
        }
    }
private:
    Identity identity_ {};
    bool bound_ {false};
};
using IntelInterfaceV1 = MetadataInterfaceV1<0x8086>;
using AmdInterfaceV1 = MetadataInterfaceV1<0x1002>;
using NvidiaInterfaceV1 = MetadataInterfaceV1<0x10de>;
}} // namespace MellowGPUCore::Backend
#endif
