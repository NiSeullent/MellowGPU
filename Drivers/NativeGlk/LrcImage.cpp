// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Mellow contributors.
// Register offset data and encoding semantics derived from Intel i915 files at
// Linux 0d9ff90a5422cc7509258aaaba1e7481df4d332a, copyright Intel Corporation.
// See LRC-IMPLEMENTATION-REPORT.md for exact files, blobs and contribution scope.
//
// Permission is hereby granted, free of charge, to any person obtaining a copy
// of this software and associated documentation files (the "Software"), to deal
// in the Software without restriction, including without limitation the rights
// to use, copy, modify, merge, publish, distribute, sublicense, and/or sell copies
// of the Software, and to permit persons to whom the Software is furnished to do
// so, subject to the following conditions:
// The above copyright notice and this permission notice shall be included in
// all copies or substantial portions of the Software.
// THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
// IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
// FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
// AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
// LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
// OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
// SOFTWARE.
#include "LrcImage.hpp"

namespace MellowNativeGlk::Lrc {
namespace {
constexpr uint32_t LrcaMask = 0xfffff000U;
constexpr uint32_t DescriptorFlags = 0x119U;
constexpr uint32_t ForceRestore = 4;
constexpr uint32_t PriorityMask = 0x600;
constexpr uint32_t ContextIdMask = 0x1fffff;
constexpr uint32_t ContextControl = 3, RingHead = 5, RingTail = 7,
                   RingStart = 9, RingControl = 11, Timestamp = 0x23,
                   MiMode = 0x55, BbOffset = 0x65, PerContext = 0x19,
                   IndirectContext = 0x1b, IndirectOffset = 0x1d;
constexpr uint32_t GroupStarts[] = {1, 0x21, 0x41, 0x51};
constexpr uint32_t GroupCounts[] = {14, 9, 1, 44};
// Expanded register offsets in the pinned gen9_{r,x}cs_offsets tables.
constexpr uint16_t Offsets0[] = {0x244,0x034,0x030,0x038,0x03c,0x168,0x140,
                                0x110,0x11c,0x114,0x118,0x1c0,0x1c4,0x1c8};
constexpr uint16_t Offsets1[] = {0x3a8,0x28c,0x288,0x284,0x280,0x27c,0x278,0x274,0x270};
constexpr uint16_t Offsets3[] = {0x028,0x09c,0x0c0,0x178,0x17c,0x358,0x170,
 0x150,0x154,0x158,0x41c,0x600,0x604,0x608,0x60c,0x610,0x614,0x618,
 0x61c,0x620,0x624,0x628,0x62c,0x630,0x634,0x638,0x63c,0x640,0x644,
 0x648,0x64c,0x650,0x654,0x658,0x65c,0x660,0x664,0x668,0x66c,0x670,
 0x674,0x678,0x67c,0x068};
static_assert(sizeof(Offsets0)/sizeof(*Offsets0) == 14 &&
              sizeof(Offsets1)/sizeof(*Offsets1) == 9 &&
              sizeof(Offsets3)/sizeof(*Offsets3) == 44, "pinned Gen9 table sizes");

uint32_t read32(const uint8_t *p) {
    return uint32_t(p[0]) | (uint32_t(p[1]) << 8) | (uint32_t(p[2]) << 16) | (uint32_t(p[3]) << 24);
}
void write32(uint8_t *p, uint32_t v) {
    for (unsigned i = 0; i < 4; ++i) p[i] = static_cast<uint8_t>(v >> (8*i));
}
uint32_t value(const uint8_t *image, uint32_t index) {
    return read32(image + StateOffset + 4*index);
}
void set(uint8_t *image, uint32_t index, uint32_t v) {
    write32(image + StateOffset + 4*index, v);
}
void zero(uint8_t *p, size_t bytes) { for (size_t i = 0; i < bytes; ++i) p[i] = 0; }
bool extent(uint64_t address, uint64_t bytes, uint64_t limit, uint64_t alignment) {
    return address && !(address & (alignment-1)) && bytes && bytes <= limit && address <= limit-bytes;
}
bool ringSize(uint32_t bytes) {
    return bytes >= PageBytes && bytes <= 0x200000 && !(bytes & (bytes-1));
}
bool offset(uint32_t bytes, uint32_t position) { return position < bytes && !(position & 7); }
bool forward(uint32_t bytes, uint32_t next, uint32_t previous) {
    const uint32_t delta = (next-previous) & (bytes-1);
    return delta && delta < bytes/2;
}
bool partialOverlap(const void *a, const void *b, size_t bytes) {
    if (a == b) return false;
    const uintptr_t x = reinterpret_cast<uintptr_t>(a), y = reinterpret_cast<uintptr_t>(b);
    return x < y ? y-x < bytes : x-y < bytes;
}
Status buffer(Engine engine, const uint8_t *image, size_t bytes, EngineInfo &info) {
    const Status status = describeEngine(engine, info);
    if (status != Status::Ok) return status;
    return image && bytes == info.contextBytes ? Status::Ok : Status::InvalidBuffer;
}
Status specValid(const ContextImageSpec &spec) {
    EngineInfo info;
    const Status status = describeEngine(spec.engine, info);
    if (status != Status::Ok) return status;
    if (!extent(spec.pml4Dma, PageBytes, DmaAddressLimit, PageBytes)) return Status::InvalidAddressSpace;
    if (!info.renderState) return spec.render ? Status::UnexpectedRenderPrerequisites : Status::Ok;
    if (!spec.render) return Status::MissingRenderPrerequisites;
    const auto &r = *spec.render;
    if (!r.indirectCtxBytes || r.indirectCtxBytes > 63*CachelineBytes ||
        (r.indirectCtxBytes & (CachelineBytes-1)) ||
        !extent(r.indirectCtxGgtt, r.indirectCtxBytes, 1ULL<<32, CachelineBytes))
        return Status::InvalidWorkaroundBatch;
    if (r.powerClockState & ~0x80078fffU) return Status::InvalidPowerClockState;
    return Status::Ok;
}
uint32_t regOffset(unsigned group, unsigned index, bool render) {
    if (group == 0) return Offsets0[index];
    if (group == 1) return Offsets1[index];
    if (group == 2) return render ? 0xc8 : 0x200;
    return Offsets3[index];
}
uint32_t header(unsigned group, bool render) {
    const bool posted = group != 2 || !render;
    return 0x11000000U | (2*GroupCounts[group]-1) | (posted ? 0x1000 : 0);
}
void setLayout(uint8_t *image, const EngineInfo &info, bool close) {
    for (unsigned group = 0; group < 4; ++group) {
        const uint32_t start = GroupStarts[group];
        set(image, start, header(group, info.renderState));
        for (unsigned n = 0; n < GroupCounts[group]; ++n)
            set(image, start+1+2*n, info.mmioBase + regOffset(group,n,info.renderState));
    }
    if (close) set(image, 0xaa, 0x05000000);
}
Status layout(const uint8_t *image, const EngineInfo &info) {
    for (unsigned group = 0; group < 4; ++group) {
        const uint32_t start = GroupStarts[group];
        if (value(image,start) != header(group,info.renderState)) return Status::LayoutMismatch;
        for (unsigned n = 0; n < GroupCounts[group]; ++n)
            if (value(image,start+1+2*n) != info.mmioBase+regOffset(group,n,info.renderState))
                return Status::LayoutMismatch;
    }
    return Status::Ok;
}
void initializeRegisters(const ContextImageSpec &spec, uint8_t *image, const EngineInfo &info, bool inhibit) {
    if (inhibit) zero(image+StateOffset,PageBytes);
    setLayout(image,info,inhibit);
    set(image,ContextControl,0x000f0008U | (inhibit ? 1 : 0));
    set(image,Timestamp,spec.contextTimestamp);
    set(image,BbOffset,0);
    set(image,0x31,static_cast<uint32_t>(spec.pml4Dma >> 32));
    set(image,0x33,static_cast<uint32_t>(spec.pml4Dma));
    if (info.renderState) {
        set(image,IndirectContext,spec.render->indirectCtxGgtt | (spec.render->indirectCtxBytes/CachelineBytes));
        set(image,IndirectOffset,0x26U<<6);
    }
    set(image,MiMode,(value(image,MiMode) & ~0x100U) | 0x01000000U);
}
Status persistent(SubmitState state) {
    if (!state.contextId || state.contextId > ContextIdMask) return Status::InvalidContextId;
    if (!(state.lrca & LrcaMask)) return Status::InvalidContextAddress;
    return (state.lrca & ~(LrcaMask|ForceRestore)) == DescriptorFlags ? Status::Ok : Status::InvalidDescriptor;
}
}

Status describeEngine(Engine engine, EngineInfo &out) {
    EngineInfo info;
    switch (engine) {
    case Engine::Rcs0: info = {0x2000,22*PageBytes,true}; break;
    case Engine::Vcs0: info = {0x12000,2*PageBytes,false}; break;
    case Engine::Bcs0: info = {0x22000,2*PageBytes,false}; break;
    case Engine::Vecs0: info = {0x1a000,2*PageBytes,false}; break;
    default: return Status::UnsupportedEngine;
    }
    out = info; return Status::Ok;
}
Status initContextImage(const ContextImageSpec &spec, const uint8_t *defaultState,
                       size_t defaultBytes, uint8_t *image, size_t bytes) {
    EngineInfo info;
    Status status = buffer(spec.engine,image,bytes,info);
    if (status != Status::Ok) return status;
    if ((!defaultState && defaultBytes) || (defaultState && defaultBytes != bytes) ||
        (defaultState && partialOverlap(defaultState,image,bytes))) return Status::InvalidBuffer;
    if ((status=specValid(spec)) != Status::Ok) return status;
    // Preserve typed inputs if their storage belongs to a caller-owned image.
    const RenderPrerequisites render = spec.render ? *spec.render : RenderPrerequisites{};
    ContextImageSpec stable = spec; if (stable.render) stable.render = &render;
    if (defaultState) {
        if (defaultState != image) for (size_t i=0;i<bytes;++i) image[i]=defaultState[i];
        zero(image,PageBytes);
    } else zero(image,bytes);
    initializeRegisters(stable,image,info,!defaultState);
    return Status::Ok;
}
Status scrubRegisterState(const ContextImageSpec &spec, uint8_t *image, size_t bytes) {
    EngineInfo info;
    Status status = buffer(spec.engine,image,bytes,info);
    if (status != Status::Ok || (status=specValid(spec)) != Status::Ok) return status;
    const RenderPrerequisites render = spec.render ? *spec.render : RenderPrerequisites{};
    ContextImageSpec stable=spec; if (stable.render) stable.render=&render;
    initializeRegisters(stable,image,info,true);
    return Status::Ok;
}
Status resetStopRing(Engine engine, uint8_t *image, size_t bytes) {
    EngineInfo info;
    Status status=buffer(engine,image,bytes,info);
    if (status != Status::Ok || (status=layout(image,info)) != Status::Ok) return status;
    set(image,MiMode,(value(image,MiMode)&~0x100U)|0x01000000U);
    return Status::Ok;
}
Status updateContextRegisters(const ContextImageSpec &spec, const RingSpec &ring,
                             uint32_t contextGgtt, uint8_t *image, size_t bytes, uint32_t &lrcaOut) {
    EngineInfo info;
    Status status=buffer(spec.engine,image,bytes,info);
    if (status != Status::Ok || (status=specValid(spec)) != Status::Ok) return status;
    if (!extent(contextGgtt,info.contextBytes,1ULL<<32,PageBytes)) return Status::InvalidContextAddress;
    if (!ringSize(ring.bytes) || !extent(ring.ggtt,ring.bytes,1ULL<<32,PageBytes)) return Status::InvalidRing;
    if (!offset(ring.bytes,ring.head) || !offset(ring.bytes,ring.tail)) return Status::InvalidRingOffset;
    if ((status=layout(image,info)) != Status::Ok) return status;
    set(image,RingStart,ring.ggtt); set(image,RingHead,ring.head); set(image,RingTail,ring.tail);
    set(image,RingControl,(ring.bytes-PageBytes)|1);
    if (info.renderState) set(image,0x43,spec.render->powerClockState);
    lrcaOut=contextGgtt|DescriptorFlags|ForceRestore;
    return Status::Ok;
}
Status decodeDescriptor(Engine engine, uint64_t descriptor, DescriptorFields &out) {
    EngineInfo info;
    Status status=describeEngine(engine,info); if (status!=Status::Ok) return status;
    const uint64_t allowed=(uint64_t(ContextIdMask)<<32)|LrcaMask|DescriptorFlags|ForceRestore|
                           (info.renderState ? PriorityMask : 0);
    if ((descriptor & ~allowed) || (descriptor & DescriptorFlags) != DescriptorFlags ||
        (!info.renderState && (descriptor & PriorityMask)) || (descriptor & PriorityMask)==PriorityMask)
        return Status::InvalidDescriptor;
    const uint32_t lrca=static_cast<uint32_t>(descriptor)&LrcaMask;
    const uint32_t id=static_cast<uint32_t>(descriptor>>32);
    if (!lrca) return Status::InvalidContextAddress;
    if (!id) return Status::InvalidContextId;
    DescriptorFields fields;
    fields.lrca=lrca; fields.contextId=id; fields.forceRestore=descriptor&ForceRestore;
    fields.hasPriority=info.renderState;
    fields.priority=(descriptor&0x400) ? Priority::High : (descriptor&0x200) ? Priority::Normal : Priority::Low;
    out=fields; return Status::Ok;
}
Status contextIdForTag(unsigned tag, uint32_t &ccid) {
    if (tag>=63) return Status::InvalidContextId;
    ccid=(1+tag)<<5; return Status::Ok;
}
Status prepareSubmission(Engine engine, uint32_t bytes, uint32_t head, uint32_t tail,
                        Priority priority, SubmitState &state, uint8_t *image,
                        size_t imageBytes, uint64_t &descriptorOut) {
    EngineInfo info;
    Status status=buffer(engine,image,imageBytes,info);
    if (status!=Status::Ok) return status;
    if (!ringSize(bytes)) return Status::InvalidRing;
    if (!offset(bytes,head) || !offset(bytes,tail)) return Status::InvalidRingOffset;
    if ((status=layout(image,info))!=Status::Ok) return status;
    const uint32_t previous=value(image,RingTail);
    if (!offset(bytes,previous)) return Status::InvalidRingOffset;
    if ((value(image,RingControl)&~0xc00U)!=((bytes-PageBytes)|1)) return Status::RingMismatch;
    if (!extent(value(image,RingStart),bytes,1ULL<<32,PageBytes)) return Status::InvalidRing;
    if ((value(image,MiMode)&0x01000100U)==0x01000100U) return Status::StopRingSet;
    if ((head/CachelineBytes)==(tail/CachelineBytes) && tail<head) return Status::TailInHeadCacheline;
    if ((status=persistent(state))!=Status::Ok) return status;
    if (priority!=Priority::Low && priority!=Priority::Normal && priority!=Priority::High) return Status::InvalidDescriptor;
    uint32_t low=state.lrca;
    if (!forward(bytes,tail,previous)) low|=ForceRestore;
    if (info.renderState) low|=priority==Priority::High ? 0x400 : priority==Priority::Normal ? 0x200 : 0;
    const uint64_t descriptor=(uint64_t(state.contextId)<<32)|low;
    set(image,RingTail,tail); state.lrca&=~ForceRestore; descriptorOut=descriptor;
    return Status::Ok;
}
Status markUnwind(uint32_t bytes, uint32_t requestTail, uint32_t ringTail, SubmitState &state) {
    if (!ringSize(bytes)) return Status::InvalidRing;
    if (!offset(bytes,requestTail) || !offset(bytes,ringTail)) return Status::InvalidRingOffset;
    const Status status=persistent(state); if (status!=Status::Ok) return status;
    if (forward(bytes,requestTail,ringTail+8)) state.lrca|=ForceRestore;
    return Status::Ok;
}
Status planElspSubmission(Engine engine, uint64_t port0, uint64_t port1, ElspPlan &out) {
    EngineInfo info;
    Status status=describeEngine(engine,info); if (status!=Status::Ok) return status;
    if (!port0) return Status::EmptyPort0;
    DescriptorFields a,b;
    if ((status=decodeDescriptor(engine,port0,a))!=Status::Ok) return status;
    if (port1) {
        if ((status=decodeDescriptor(engine,port1,b))!=Status::Ok) return status;
        if (a.lrca==b.lrca || a.contextId==b.contextId) return Status::DuplicateContext;
    }
    const uint32_t reg=info.mmioBase+0x230;
    ElspPlan plan;
    plan.writes[0]={reg,static_cast<uint32_t>(port1>>32)}; plan.writes[1]={reg,static_cast<uint32_t>(port1)};
    plan.writes[2]={reg,static_cast<uint32_t>(port0>>32)}; plan.writes[3]={reg,static_cast<uint32_t>(port0)};
    out=plan; return Status::Ok;
}
Status planCsbReset(Engine engine, CsbReset &out) {
    EngineInfo info;
    const Status status=describeEngine(engine,info); if (status!=Status::Ok) return status;
    CsbReset reset;
    reset.statusPointer={info.mmioBase+0x3a0,0xffff0505}; reset.head=5;
    reset.writePointerDword=5; reset.entryFill=~uint64_t(0); out=reset; return Status::Ok;
}
Status walkCsb(uint8_t head, uint32_t writePointer, CsbWalk &out) {
    const uint8_t tail=static_cast<uint8_t>(writePointer);
    if (head>=CsbEntries || tail>=CsbEntries) return Status::InvalidCsbPointer;
    CsbWalk walk;
    while (head!=tail) { head=static_cast<uint8_t>((head+1)%CsbEntries); walk.indices[walk.count++]=head; }
    walk.newHead=tail; out=walk; return Status::Ok;
}
Status decodeCsbEntry(uint64_t entry, CsbEvent &out) {
    if (entry==~uint64_t(0)) return Status::CsbEntryNotWritten;
    CsbEvent event;
    event.status=static_cast<uint32_t>(entry); event.contextId=static_cast<uint32_t>(entry>>32);
    event.idleToActive=event.status&1; event.preempted=event.status&2;
    event.elementSwitch=event.status&4; event.activeToIdle=event.status&8;
    event.complete=event.status&16; event.liteRestore=event.status&0x8000;
    event.promote=event.idleToActive||event.preempted;
    out=event; return Status::Ok;
}
Status checkCsbContext(const CsbEvent &event, uint32_t assigned) {
    if (!assigned || assigned>ContextIdMask || !event.contextId || event.contextId>ContextIdMask)
        return Status::InvalidContextId;
    return event.contextId==assigned ? Status::Ok : Status::CsbIdentityMismatch;
}
Status csbEntryMmioOffset(Engine engine, unsigned index, uint32_t &out) {
    EngineInfo info;
    const Status status=describeEngine(engine,info); if (status!=Status::Ok) return status;
    if (index>=CsbEntries) return Status::InvalidCsbPointer;
    out=info.mmioBase+0x370+8*index; return Status::Ok;
}
Status decodeRegisterState(Engine engine, const uint8_t *image, size_t bytes, RegisterState &out) {
    EngineInfo info;
    Status status=buffer(engine,image,bytes,info);
    if (status!=Status::Ok || (status=layout(image,info))!=Status::Ok) return status;
    RegisterState state;
    state.contextControl=value(image,ContextControl); state.ringHead=value(image,RingHead);
    state.ringTail=value(image,RingTail); state.ringStart=value(image,RingStart); state.ringCtl=value(image,RingControl);
    state.bbState=value(image,0x11); state.timestamp=value(image,Timestamp);
    for (unsigned n=0;n<4;++n) { state.pdpUdw[n]=value(image,0x31-4*n); state.pdpLdw[n]=value(image,0x33-4*n); }
    state.slot43=value(image,0x43); state.miMode=value(image,MiMode); state.bbOffset=value(image,BbOffset);
    state.bbPerCtxPtr=value(image,PerContext); state.indirectCtx=value(image,IndirectContext);
    state.indirectCtxOffset=value(image,IndirectOffset); state.listClosed=value(image,0xaa)==0x05000000;
    out=state; return Status::Ok;
}
Status checkRegisterState(const RegisterState &state, uint32_t ggtt, uint32_t bytes) {
    if (!ringSize(bytes) || !extent(ggtt,bytes,1ULL<<32,PageBytes)) return Status::InvalidRing;
    if (state.ringStart!=ggtt || (state.ringCtl&~0xc00U)!=((bytes-PageBytes)|1)) return Status::RingMismatch;
    return (state.miMode&0x01000100U)==0x01000100U ? Status::StopRingSet : Status::Ok;
}
} // namespace MellowNativeGlk::Lrc
