// GlkWorkloads.metal — Metal Shading Language workloads for the GLK device-lab
// benchmark. This file is the reviewable source of truth. GlkMetalBench.swift embeds a
// byte-identical copy and compiles it at runtime with MTLDevice.makeLibrary(source:),
// so that MSL is genuinely compiled by the target's Metal toolchain on the real GPU.
// A build-time .metallib may also be produced from this file for provenance.
//
// The compute kernel is integer-only and uint32-overflow-exact, so a CPU evaluation of
// the identical recurrence is an independent oracle for the GPU output (no CPU fallback
// for producing results — the CPU only predicts what the GPU must independently compute).

#include <metal_stdlib>
using namespace metal;

struct MixParams {
    uint count;
    uint nonce;
    uint rounds;
};

// Deterministic integer mixing. Must stay identical to computeReferenceWord() in
// GlkDeviceIdentity.swift.
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
    // A reduction witness lets the host confirm the whole grid executed, independent of
    // the per-element compare.
    atomic_fetch_add_explicit(checksum, value, memory_order_relaxed);
}

// Full-target triangle (-1,-1)/(3,-1)/(-1,3) covers the entire render target with three
// vertices, so a constant-color fragment yields a deterministic, fully-known image.
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
