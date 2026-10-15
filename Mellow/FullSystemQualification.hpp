#pragma once

#include "RuntimeReadiness.hpp"
#include "../Drivers/Core/Metal3Profile.hpp"

namespace MellowGPUQualification {

constexpr uint32_t ProfileVersion = 1;
// Neither boot arguments nor legacy bring-up evidence can supply these native
// components. Implement and authenticate them before changing this build gate.
constexpr bool NativeAuthorityIntegrated = false;
constexpr bool RequiredCaseInventoryIntegrated = false;

struct Decision {
    const char *name {"unqualified"};
    uint32_t profileVersion {ProfileVersion};
    bool mayAdvertiseMetal {};
};

inline Decision evaluate(const MellowRuntime::Decision &legacy,
                         const MellowGPUCore::Conformance::Assessment &assessment) {
    const bool qualified = NativeAuthorityIntegrated &&
        RequiredCaseInventoryIntegrated && legacy.mayAdvertiseMetal &&
        assessment.completeProfile && assessment.hardwareAttested &&
        assessment.status == MellowGPUCore::Conformance::Status::ReadyForSupportReview;
    return {qualified ? "qualified" : "unqualified", ProfileVersion, qualified};
}

inline Decision evaluate(const MellowRuntime::Decision &legacy) {
    // There is no trusted native receipt source in this build. Empty records
    // honestly describe that state, rather than converting readiness bits into
    // a Metal 3/WindowServer qualification record.
    return evaluate(legacy, MellowGPUCore::Conformance::assess({}, {}));
}

}
