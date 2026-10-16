// SPDX-License-Identifier: MIT
// Original test-only classes; no Metal driver instance is created.
#define MELLOW_METADATA_NO_MAIN 1
#include "../Tools/metal-runtime-metadata.mm"
#include <assert.h>

static unsigned fixtureCalls;

@interface MGMetadataBase : NSObject
- (id)initWithAcceleratorPort:(uint32_t)port;
- (uint64_t)registryID;
+ (void *)deviceRef;
@end
@implementation MGMetadataBase
+ (void)initialize { ++fixtureCalls; }
- (id)initWithAcceleratorPort:(uint32_t)port { (void)port; ++fixtureCalls; return self; }
- (uint64_t)registryID { ++fixtureCalls; return 0; }
+ (void *)deviceRef { ++fixtureCalls; return nullptr; }
@end

@interface MGMetadataChild : MGMetadataBase
- (void *)sharedRef;
@end
@implementation MGMetadataChild
- (void *)sharedRef { ++fixtureCalls; return nullptr; }
@end

int main() { @autoreleasepool {
    Class child = objc_lookUpClass("MGMetadataChild");
    NSDictionary *inherited = describeMethod(child, "registryID", false);
    assert([inherited[@"found"] boolValue]);
    assert([inherited[@"declaring_owner"] isEqualToString:@"MGMetadataBase"]);
    assert(![inherited[@"declaring_owner_is_metaclass"] boolValue]);
    assert([inherited[@"inherited"] boolValue]);
    Method method = class_getInstanceMethod(objc_lookUpClass("MGMetadataBase"), sel_registerName("registryID"));
    assert([inherited[@"encoding"] isEqualToString:text(method_getTypeEncoding(method))]);
    assert([inherited[@"argument_types_including_self_and_cmd"] count] == method_getNumberOfArguments(method));
    NSDictionary *own = describeMethod(child, "sharedRef", false);
    assert([own[@"declaring_owner"] isEqualToString:@"MGMetadataChild"]);
    assert(![own[@"inherited"] boolValue]);
    NSDictionary *classMethod = describeMethod(child, "deviceRef", true);
    assert([classMethod[@"found"] boolValue]);
    assert([classMethod[@"declaring_owner"] isEqualToString:@"MGMetadataBase"]);
    assert([classMethod[@"declaring_owner_is_metaclass"] boolValue]);
    assert(![describeMethod(child, "deviceRef", false)[@"found"] boolValue]);
    assert(![describeClass("MGMetadataMissing")[@"found"] boolValue]);
    assert(fixtureCalls == 0);
    puts("PASS 7 original runtime metadata cases; no fixture initialization or method invocation");
    return 0;
}}
