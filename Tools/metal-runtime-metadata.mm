// SPDX-License-Identifier: MIT
// Original metadata measurement only. No private initializer or GPU command is invoked.
#import <Foundation/Foundation.h>
#import <Metal/Metal.h>
#import <objc/runtime.h>
#include <dlfcn.h>
#include <mach-o/dyld.h>
#include <mach-o/loader.h>
#include <sys/sysctl.h>
#include <sys/utsname.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <initializer_list>

NSString *text(const char *value) {
    return value ? [NSString stringWithUTF8String:value] ?: @"<non-UTF8>" : @"";
}

id imageUUID(const struct mach_header *header) {
    if (!header || (header->magic != MH_MAGIC_64 && header->magic != MH_MAGIC)) return NSNull.null;
    const size_t headerBytes = header->magic == MH_MAGIC_64 ? sizeof(struct mach_header_64) : sizeof(struct mach_header);
    const auto *commands = reinterpret_cast<const unsigned char *>(header) + headerBytes;
    size_t offset = 0;
    for (uint32_t i = 0; i < header->ncmds; ++i) {
        if (offset > header->sizeofcmds || header->sizeofcmds - offset < sizeof(struct load_command)) return NSNull.null;
        const auto *command = reinterpret_cast<const struct load_command *>(commands + offset);
        if (command->cmdsize < sizeof(struct load_command) || command->cmdsize > header->sizeofcmds - offset) return NSNull.null;
        if (command->cmd == LC_UUID && command->cmdsize >= sizeof(struct uuid_command)) {
            const auto *uuid = reinterpret_cast<const struct uuid_command *>(command)->uuid;
            char value[37];
            snprintf(value, sizeof(value), "%02X%02X%02X%02X-%02X%02X-%02X%02X-%02X%02X-%02X%02X%02X%02X%02X%02X",
                uuid[0],uuid[1],uuid[2],uuid[3],uuid[4],uuid[5],uuid[6],uuid[7],uuid[8],uuid[9],uuid[10],uuid[11],uuid[12],uuid[13],uuid[14],uuid[15]);
            return text(value);
        }
        offset += command->cmdsize;
    }
    return NSNull.null;
}

NSDictionary *imageRecord(const char *path) {
    if (!path) return @{@"path":NSNull.null,@"loaded_image_found":@NO,@"uuid":NSNull.null};
    for (uint32_t i = 0; i < _dyld_image_count(); ++i) {
        const char *loaded = _dyld_get_image_name(i);
        if (loaded && strcmp(path, loaded) == 0)
            return @{@"path":text(loaded),@"loaded_image_found":@YES,@"uuid":imageUUID(_dyld_get_image_header(i))};
    }
    return @{@"path":text(path),@"loaded_image_found":@NO,@"uuid":NSNull.null};
}

NSArray *ancestry(Class value) {
    NSMutableArray *result = [NSMutableArray array];
    for (Class current = value; current && result.count < 128; current = class_getSuperclass(current))
        [result addObject:text(class_getName(current))];
    return result;
}

NSDictionary *describeMethod(Class value, const char *selectorName, bool classMethod) {
    NSMutableDictionary *result = [@{@"selector":text(selectorName),@"method_kind":classMethod?@"class":@"instance",@"found":@NO} mutableCopy];
    if (!value) return result;
    const SEL selector = sel_registerName(selectorName);
    Class current = classMethod ? object_getClass(value) : value;
    for (unsigned depth = 0; current && depth < 128; current = class_getSuperclass(current), ++depth) {
        unsigned count = 0;
        Method *methods = class_copyMethodList(current, &count);
        Method found = nullptr;
        for (unsigned i = 0; i < count; ++i)
            if (sel_isEqual(method_getName(methods[i]), selector)) { found = methods[i]; break; }
        free(methods);
        if (!found) continue;
        char *returnType = method_copyReturnType(found);
        NSMutableArray *arguments = [NSMutableArray array];
        for (unsigned i = 0; i < method_getNumberOfArguments(found); ++i) {
            char *type = method_copyArgumentType(found, i);
            [arguments addObject:text(type)]; free(type);
        }
        result[@"found"] = @YES;
        result[@"declaring_owner"] = text(class_getName(current));
        result[@"declaring_owner_is_metaclass"] = @(class_isMetaClass(current));
        result[@"declaring_owner_image"] = imageRecord(class_getImageName(current));
        result[@"encoding"] = text(method_getTypeEncoding(found));
        result[@"return_type"] = text(returnType); free(returnType);
        result[@"argument_types_including_self_and_cmd"] = arguments;
        result[@"inherited"] = @(depth != 0);
        Dl_info info = {};
        const bool located = dladdr(reinterpret_cast<const void *>(method_getImplementation(found)), &info) != 0;
        result[@"implementation_image"] = located ? imageRecord(info.dli_fname) : imageRecord(nullptr);
        result[@"implementation_invoked"] = @NO;
        return result;
    }
    return result;
}

NSDictionary *describeClass(const char *name) {
    Class value = objc_lookUpClass(name);
    if (!value) return @{@"requested_class":text(name),@"found":@NO};
    NSMutableArray *methods = [NSMutableArray array];
    for (const char *selector : {"initWithAcceleratorPort:","registryID","sharedRef","deviceRef"}) {
        [methods addObject:describeMethod(value, selector, false)];
        [methods addObject:describeMethod(value, selector, true)];
    }
    return @{@"requested_class":text(name),@"found":@YES,@"runtime_class":text(class_getName(value)),
             @"ancestry":ancestry(value),@"image":imageRecord(class_getImageName(value)),@"methods":methods};
}

NSString *sysctlText(const char *key) {
    size_t length = 0;
    if (sysctlbyname(key,nullptr,&length,nullptr,0) || !length || length > 4096) return @"unavailable";
    char *bytes = static_cast<char *>(calloc(length + 1,1));
    if (!bytes) return @"unavailable";
    NSString *result = sysctlbyname(key,bytes,&length,nullptr,0) ? @"unavailable" : text(bytes);
    free(bytes); return result;
}

#ifndef MELLOW_METADATA_NO_MAIN
int main(int argc, const char **argv) { @autoreleasepool {
    const bool enumerate = argc == 2 && strcmp(argv[1],"--enumerate-public-devices") == 0;
    if (argc != 1 && !enumerate) { fputs("Only --enumerate-public-devices is accepted\n",stderr); return 2; }
    NSMutableArray *before = [NSMutableArray array], *after = [NSMutableArray array], *devices = [NSMutableArray array];
    for (const char *name : {"MTLIOAccelDevice","_MTLDevice","AGXMetalDevice"}) [before addObject:describeClass(name)];
    if (enumerate) {
        NSArray<id<MTLDevice>> *existing = MTLCopyAllDevices(); // Public enumeration only; no command queue or command buffer.
        for (id<MTLDevice> device in existing) {
            Class actual = object_getClass(device);
            [devices addObject:@{@"runtime_class":text(class_getName(actual)),@"ancestry":ancestry(actual),@"metadata":describeClass(class_getName(actual))}];
        }
        for (const char *name : {"MTLIOAccelDevice","_MTLDevice","AGXMetalDevice"}) [after addObject:describeClass(name)];
    }
    NSMutableArray *images = [NSMutableArray array];
    for (uint32_t i = 0; i < _dyld_image_count(); ++i) {
        const char *name = _dyld_get_image_name(i);
        if (name && (strstr(name,"/Metal.framework/") || strstr(name,"AGXMetal") || strstr(name,"/IOGPU.framework/") || strstr(name,"/IOAccelerator.framework/")))
            [images addObject:imageRecord(name)];
    }
    struct utsname system = {}; uname(&system);
#if defined(__aarch64__) || defined(__arm64__)
    NSString *processArchitecture = @"arm64";
#elif defined(__x86_64__)
    NSString *processArchitecture = @"x86_64";
#else
    NSString *processArchitecture = @"other";
#endif
    NSDictionary *result = @{@"schema":@"mellowgpu.metal-runtime-metadata/1",@"process_architecture":processArchitecture,
        @"host_machine":text(system.machine),@"os_product_version":sysctlText("kern.osproductversion"),@"os_build":sysctlText("kern.osversion"),
        @"kernel_release":text(system.release),@"public_device_enumeration_called":@(enumerate),
        @"classes_before_public_enumeration":before,@"classes_after_public_enumeration":after,@"public_devices":devices,@"relevant_loaded_images":images,
        @"private_initializers_invoked_directly":@NO,@"private_driver_subclasses_instantiated_directly":@NO,@"command_buffers_created":@NO,@"gpu_commands_submitted":@NO,
        @"device_property_values_read":@NO,@"darwin24_x86_64_target_abi_verified":@NO,
        @"scope":@"Public Objective-C runtime metadata and optional public enumeration on this exact host only; no direct private initializer invocation, GPU submission, or x86/Darwin24 ABI qualification. Public enumeration may initialize Apple's existing devices internally."};
    NSError *error = nil;
    NSData *encoded = [NSJSONSerialization dataWithJSONObject:result options:NSJSONWritingPrettyPrinted|NSJSONWritingSortedKeys error:&error];
    if (!encoded) { fputs("JSON serialization failed\n",stderr); return 3; }
    fwrite(encoded.bytes,1,encoded.length,stdout); fputc('\n',stdout);
    return 0;
}}
#endif
