// SPDX-License-Identifier: MIT
// Test-only declarations for the public calls used by the actual userspace
// probe. Implementations in probe_control_tests.cpp inject all OS responses.
#pragma once
#include <stddef.h>
#include <stdint.h>
#include <mach/mach.h>
using IOReturn = kern_return_t;
using boolean_t = bool;
using io_object_t = mach_port_t;
using io_service_t = io_object_t;
using io_iterator_t = io_object_t;
using io_registry_entry_t = io_object_t;
using io_connect_t = io_object_t;
using CFDictionaryRef = const void *;
using CFMutableDictionaryRef = void *;
using io_name_t = char[128];
constexpr io_object_t IO_OBJECT_NULL = 0;
constexpr mach_port_t kIOMainPortDefault = 0;
constexpr IOReturn kIOReturnSuccess = KERN_SUCCESS;
inline constexpr const char *kIOServicePlane = "IOService";
CFMutableDictionaryRef IOServiceMatching(const char *);
IOReturn IOServiceGetMatchingServices(mach_port_t, CFDictionaryRef, io_iterator_t *);
io_object_t IOIteratorNext(io_iterator_t);
IOReturn IORegistryEntryGetParentEntry(io_registry_entry_t, const io_name_t,
                                      io_registry_entry_t *);
boolean_t IOObjectConformsTo(io_object_t, const io_name_t);
IOReturn IORegistryEntryGetRegistryEntryID(io_registry_entry_t, uint64_t *);
IOReturn IOObjectRelease(io_object_t);
IOReturn IOServiceOpen(io_service_t, task_port_t, uint32_t, io_connect_t *);
IOReturn IOServiceClose(io_connect_t);
kern_return_t IOConnectCallStructMethod(mach_port_t, uint32_t, const void *, size_t,
                                       void *, size_t *);
