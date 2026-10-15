#include "../Mellow/FullSystemQualification.hpp"
#include <assert.h>
#include <stdio.h>
#include <string.h>
int main() {
    using namespace MellowGPUQualification;
    unsigned checks = 0;
    const uint64_t inputs[] = {0, MellowRuntime::ConfigurationMask,
                              MellowRuntime::AllEvidenceMask, UINT64_MAX};
    for (uint64_t evidence : inputs) {
        const auto legacy = MellowRuntime::evaluate(evidence);
        const auto decision = evaluate(legacy);
        assert(!decision.mayAdvertiseMetal); ++checks;
        assert(!strcmp(decision.name, "unqualified")); ++checks;
        assert(decision.profileVersion == 1); ++checks;
        // Even a complete caller-supplied policy assessment cannot create native authority.
        MellowGPUCore::Conformance::Assessment synthetic {
            MellowGPUCore::Conformance::Status::ReadyForSupportReview,
            MellowGPUCore::Conformance::Gate::Count, true, true};
        assert(!evaluate(legacy, synthetic).mayAdvertiseMetal); ++checks;
    }
    assert(MellowRuntime::evaluate(MellowRuntime::AllEvidenceMask).mayAdvertiseMetal); ++checks;
    assert(!NativeAuthorityIntegrated && !RequiredCaseInventoryIntegrated); ++checks;
    printf("PASS: %u full-system publication checks; no GPU or native qualification\n", checks);
}
