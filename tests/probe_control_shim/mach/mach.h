// SPDX-License-Identifier: MIT
// Test-only public Mach type boundary. No native task or kernel call is made.
#pragma once
#include <stdint.h>
using mach_port_t = uint32_t;
using task_port_t = mach_port_t;
using kern_return_t = int;
constexpr kern_return_t KERN_SUCCESS = 0;
task_port_t mach_task_self();
