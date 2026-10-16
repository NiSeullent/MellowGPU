// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Mellow contributors.
// Hosted stand-in for <IOKit/pci/IOPCIDevice.h>, used only by
// tests/native_glk_topology_iokit_tests.cpp. It declares the two members the
// GLK topology adapter calls with the public SDK 15.5 signatures
// (configRead16(IOByteCount), configRead8(IOByteCount), which forward to
// extendedConfigRead16/8() there) over an injected configuration space that
// counts every access. It is not an IOKit implementation.
#pragma once
#include <stdint.h>

typedef uint8_t UInt8;
typedef uint16_t UInt16;
typedef unsigned long long IOByteCount; // IOByteCount64 on LP64

enum {
    kIOPCIConfigVendorID = 0x00,
    kIOPCIConfigDeviceID = 0x02,
    kIOPCIConfigRevisionID = 0x08,
    kIOPCIConfigSubSystemVendorID = 0x2c,
    kIOPCIConfigSubSystemID = 0x2e
};

class IOPCIDevice {
public:
    UInt16 configRead16(IOByteCount offset) {
        ++configReads;
        const UInt16 value = static_cast<UInt16>(config[offset] | config[offset + 1] << 8);
        if (afterConfigRead) afterConfigRead(configContext);
        return value;
    }
    UInt8 configRead8(IOByteCount offset) {
        ++configReads;
        const UInt8 value = config[offset];
        if (afterConfigRead) afterConfigRead(configContext);
        return value;
    }
    UInt8 config[64] {};
    unsigned configReads = 0;
    void *configContext = nullptr;
    void (*afterConfigRead)(void *) = nullptr;
};
