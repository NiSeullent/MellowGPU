// glk_oracle_tests.cpp — host, CPU-only, sanitizer-instrumented tests for the integer
// oracle that GlkMetalBench uses to independently validate GPU compute output.
//
// The recurrence here MUST stay equivalent to computeReferenceWord() in
// src/GlkDeviceIdentity.swift and glk_mix() in shaders/GlkWorkloads.metal.
// build-glk-device-lab.py runs this binary plain and under fatal ASan/UBSan
// (-fno-sanitize-recover=all, halt_on_error) and runs the sanitizer canaries, which must
// abort; tests/test_device_lab_host.py cross-checks the FNV-1a digests of arbitrary
// vectors against `glk-metal-bench --oracle-digest`, so any drift is caught.
//
// No Metal, GPU, MMIO, DMA, kext or hardware access of any kind occurs here.

#include <climits>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

namespace glk {

// uint32 overflow-exact mixing recurrence (matches Swift and MSL).
static uint32_t referenceWord(uint32_t input, uint32_t nonce, uint32_t rounds) {
    uint32_t x = input ^ nonce;
    for (uint32_t i = 0; i < rounds; ++i) {
        x = x * 1664525u + 1013904223u;
        x ^= x >> 15;
        x = x * 2246822519u;
        x ^= x >> 13;
    }
    return x;
}

static std::vector<uint32_t> makeInput(size_t count, uint32_t nonce) {
    std::vector<uint32_t> input(count);
    uint32_t state = nonce ^ 0xC001D00Du;
    for (size_t i = 0; i < count; ++i) {
        state = state * 1664525u + 1013904223u;
        input[i] = i < 4 ? static_cast<uint32_t>(i + 1)
                         : state ^ static_cast<uint32_t>(i);
    }
    return input;
}

static uint32_t checksum(const std::vector<uint32_t>& values) {
    uint32_t sum = 0;
    for (uint32_t v : values) sum += v; // wraparound intended
    return sum;
}

// FNV-1a 64 over the little-endian bytes of each word (matches fnv1a64Hex in Swift).
static uint64_t fnv1a64(const std::vector<uint32_t>& words) {
    uint64_t hash = 0xcbf29ce484222325ull;
    for (uint32_t word : words) {
        for (int shift = 0; shift < 32; shift += 8) {
            hash ^= static_cast<uint64_t>((word >> shift) & 0xFFu);
            hash *= 0x100000001b3ull;
        }
    }
    return hash;
}

#if defined(GLK_SANITIZED)
// Sanitizer canaries: deliberate defects that a correctly configured, fatal ASan/UBSan
// build must abort on. Compiled only into sanitizer builds, so the plain build never
// executes undefined behaviour.
static int canaryHeapOverflow(int seed) {
    uint32_t* words = new uint32_t[4];
    for (int i = 0; i < 4; ++i) words[i] = static_cast<uint32_t>(seed + i);
    volatile int index = 4; // one past the end
    uint32_t value = words[index];
    delete[] words;
    return static_cast<int>(value);
}

static int canarySignedOverflow(int seed) {
    volatile int base = INT_MAX;
    return base + seed; // seed >= 1 overflows
}
#endif

} // namespace glk

int main(int argc, char** argv) {
    uint32_t nonce = 0x12345678u;
    uint32_t rounds = 3;
    size_t count = 8;
    bool emitOnly = false;
    const char* canary = nullptr;
    for (int i = 1; i < argc; ++i) {
        std::string arg = argv[i];
        auto next = [&](const char* flag) -> const char* {
            if (i + 1 >= argc) { std::fprintf(stderr, "missing value for %s\n", flag); std::exit(2); }
            return argv[++i];
        };
        if (arg == "--nonce") nonce = static_cast<uint32_t>(strtoul(next("--nonce"), nullptr, 0));
        else if (arg == "--rounds") rounds = static_cast<uint32_t>(strtoul(next("--rounds"), nullptr, 0));
        else if (arg == "--count") count = static_cast<size_t>(strtoull(next("--count"), nullptr, 0));
        else if (arg == "--emit-only") emitOnly = true;
        else if (arg == "--canary") canary = next("--canary");
        else { std::fprintf(stderr, "unknown argument: %s\n", arg.c_str()); std::exit(2); }
    }
    if (canary != nullptr) {
#if defined(GLK_SANITIZED)
        const std::string which = canary;
        const int seed = argc > 0 ? 1 : 0; // runtime value the optimizer cannot fold
        int value = 0;
        if (which == "asan-heap-overflow") value = glk::canaryHeapOverflow(seed);
        else if (which == "ubsan-signed-overflow") value = glk::canarySignedOverflow(seed);
        else { std::fprintf(stderr, "unknown canary: %s\n", canary); return 2; }
        // Reaching this line means the sanitizer did NOT stop the defect: a failure.
        std::printf("{\"status\":\"CANARY_NOT_CAUGHT\",\"canary\":\"%s\",\"value\":%d}\n", canary, value);
        return 4;
#else
        std::fprintf(stderr, "canary %s requires a sanitizer build (GLK_SANITIZED)\n", canary);
        return 3;
#endif
    }
    if (count < 1 || count > (1u << 26)) { std::fprintf(stderr, "count out of range\n"); return 2; }

    std::vector<uint32_t> input = glk::makeInput(count, nonce);
    std::vector<uint32_t> expected(count);
    for (size_t i = 0; i < count; ++i) expected[i] = glk::referenceWord(input[i], nonce, rounds);
    uint32_t sum = glk::checksum(expected);

    int failures = 0;
    if (!emitOnly) {
        // Known-answer vector for nonce=0x12345678, rounds=3, count>=8 (matches Swift --selfcheck).
        if (nonce == 0x12345678u && rounds == 3 && count >= 8) {
            const uint32_t knownInputFirst[4] = {1u, 2u, 3u, 4u};
            const uint32_t knownExpectedFirst[4] = {1839021190u, 1196368173u, 352203248u, 3492132663u};
            for (int i = 0; i < 4; ++i) {
                if (input[i] != knownInputFirst[i]) { std::fprintf(stderr, "input[%d] drift\n", i); ++failures; }
                if (expected[i] != knownExpectedFirst[i]) { std::fprintf(stderr, "expected[%d] drift\n", i); ++failures; }
            }
        }
        // Determinism: two evaluations agree.
        for (size_t i = 0; i < count; ++i) {
            if (glk::referenceWord(input[i], nonce, rounds) != expected[i]) { ++failures; break; }
        }
        // More rounds must change at least one element (rounds is not ignored).
        bool allEqual = true;
        for (size_t i = 0; i < count && allEqual; ++i) {
            if (glk::referenceWord(input[i], nonce, rounds) != glk::referenceWord(input[i], nonce, rounds + 1)) allEqual = false;
        }
        if (allEqual) { std::fprintf(stderr, "rounds has no effect\n"); ++failures; }
    }

    // Final machine-readable line (last line is the authoritative result).
    std::printf("{\"status\":\"%s\",\"nonce\":%u,\"rounds\":%u,\"count\":%zu,"
                "\"input_first\":[%u,%u,%u,%u],\"expected_first\":[%u,%u,%u,%u],"
                "\"checksum\":%u,\"fnv1a64\":\"%016llx\",\"failures\":%d,"
                "\"hardware_execution\":false,\"gpu\":false}\n",
                failures == 0 ? "PASS_GLK_ORACLE" : "FAIL_GLK_ORACLE",
                nonce, rounds, count,
                input[0], count > 1 ? input[1] : 0u, count > 2 ? input[2] : 0u, count > 3 ? input[3] : 0u,
                expected[0], count > 1 ? expected[1] : 0u, count > 2 ? expected[2] : 0u, count > 3 ? expected[3] : 0u,
                sum, static_cast<unsigned long long>(glk::fnv1a64(expected)), failures);
    return failures == 0 ? 0 : 1;
}
