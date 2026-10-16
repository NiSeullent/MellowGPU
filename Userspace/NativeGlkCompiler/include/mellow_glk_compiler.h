/*
 * MellowGPU NativeGlkCompiler -- offline Intel Gen9 (Gemini Lake) compute
 * shader compiler adapter over the pinned upstream Mesa `brw` backend.
 *
 * SPDX-License-Identifier: MIT
 *
 * Scope: host-only translation of typed NIR or SPIR-V compute kernels into
 * Intel EU ISA plus the program/resource metadata a future RCS submission
 * owner needs.  Nothing in this interface touches a GPU, uploads a kernel,
 * programs a ring or claims hardware execution.  A successful compile is
 * NOT GPU or Metal qualification.
 *
 * Ownership rules (all objects are single-threaded per handle):
 *   - mglk_compiler: created by mglk_compiler_create, released by
 *     mglk_compiler_destroy.  It must outlive every kernel it produced and
 *     every nir_shader created from mglk_compiler_nir_options().
 *   - mglk_kernel: created by a successful compile call, released by
 *     mglk_kernel_destroy.  Every pointer returned by a kernel accessor is
 *     owned by the kernel and valid until mglk_kernel_destroy.
 *   - mglk_compile_compute_nir() always takes ownership of the nir_shader
 *     passed in (ralloc tree), on success and on failure.
 *   - Failures never return a partially built kernel; *out is set to NULL.
 */
#ifndef MELLOW_GLK_COMPILER_H
#define MELLOW_GLK_COMPILER_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define MGLK_ABI_VERSION 2u

/* Exact upstream compiler identity this adapter was written and built for. */
#define MGLK_MESA_REMOTE "https://gitlab.freedesktop.org/mesa/mesa"
#define MGLK_MESA_COMMIT "f5cb8ee032adabef599ae892ec4e42d796b84da9"
#define MGLK_MESA_TREE "ce032e8ca81cc2ee6e56a73c2e6707cea78dce32"
#define MGLK_MESA_VERSION "26.3.0-devel"

/* Gemini Lake PCI device IDs listed by the pinned include/pci_ids. */
#define MGLK_PCI_GLK_3X6 0x3184u
#define MGLK_PCI_GLK_2X6 0x3185u

/* Hard input bounds enforced before any upstream code sees the input. */
#define MGLK_MAX_SPIRV_WORDS (1u << 22)
#define MGLK_MAX_ENTRY_NAME 255u
#define MGLK_MAX_BINDINGS 64u
#define MGLK_MAX_BTI 239u /* Gen9 binding tables hold 240 surface entries. */
#define MGLK_MAX_WORKGROUP_DIM 1024u
#define MGLK_DIAG_MESSAGE_CAP 2048u
#define MGLK_NO_PUSH_DWORD 0xffffffffu

typedef enum mglk_status {
   MGLK_OK = 0,
   MGLK_E_INVALID_ARGUMENT = 1,
   MGLK_E_UNSUPPORTED_DEVICE = 2,
   MGLK_E_UNSUPPORTED_INPUT = 3,
   MGLK_E_MALFORMED_INPUT = 4,
   MGLK_E_FRONTEND_REJECTED = 5, /* upstream spirv_to_nir refused the module */
   MGLK_E_COMPILE_FAILED = 6,    /* upstream brw_compile returned NULL */
   MGLK_E_ISA_VALIDATION_FAILED = 7,
   MGLK_E_LIMIT_EXCEEDED = 8,
   MGLK_E_OUT_OF_MEMORY = 9,
} mglk_status;

typedef struct mglk_diagnostic {
   mglk_status status;
   /* NUL-terminated, truncated to the buffer; upstream text is preserved. */
   char message[MGLK_DIAG_MESSAGE_CAP];
} mglk_diagnostic;

typedef enum mglk_topology_source {
   /* Slice/subslice/EU counts come from the pinned static PCI table only.
    * Runtime fusing (kernel topology query) is UNKNOWN on this host. */
   MGLK_TOPOLOGY_STATIC_PCI_TABLE_FUSING_UNKNOWN = 1,
} mglk_topology_source;

typedef struct mglk_device_desc {
   uint32_t abi_version;
   uint32_t pci_device_id;
   uint32_t ver;    /* graphics IP major version (9 for GLK) */
   uint32_t verx10; /* 90 for GLK */
   uint32_t gt;
   uint32_t is_gen9_lp;
   uint32_t num_slices;
   uint32_t num_subslices_total;
   uint32_t max_eus_per_subslice;
   uint32_t num_thread_per_eu;
   uint32_t max_cs_threads;
   uint32_t max_cs_workgroup_threads;
   uint32_t max_workgroup_invocations;
   uint32_t grf_registers;
   uint64_t timestamp_frequency;
   mglk_topology_source topology_source;
   uint32_t runtime_fused_topology_known; /* always 0 here */
   /* INTEL_DEBUG or INTEL_SIMD_DEBUG was set when the compiler was created;
    * upstream brw then changes SIMD selection/output. */
   uint32_t debug_env_present;
   char platform_name[32];
   char marketing_name[96];
   char mesa_commit[48];
} mglk_device_desc;

typedef enum mglk_resource_kind {
   MGLK_RESOURCE_SSBO = 1,
   MGLK_RESOURCE_UBO = 2,
} mglk_resource_kind;

/* SPIR-V (DescriptorSet, Binding) -> Gen9 binding table index. */
typedef struct mglk_binding {
   uint32_t set;
   uint32_t binding;
   uint32_t bti;
   mglk_resource_kind kind;
} mglk_binding;

typedef enum mglk_access {
   MGLK_ACCESS_READ = 1u << 0,
   MGLK_ACCESS_WRITE = 1u << 1,
   MGLK_ACCESS_ATOMIC = 1u << 2,
} mglk_access;

/* Scanned from the NIR handed to brw_compile, so it is a superset of the
 * surfaces the final ISA can address (later upstream DCE may drop uses). */
typedef struct mglk_resource_use {
   uint32_t bti;
   mglk_resource_kind kind;
   uint32_t access; /* mglk_access bits observed in the lowered NIR */
} mglk_resource_use;

typedef struct mglk_reloc {
   uint32_t id;     /* upstream intel_shader_reloc_id */
   uint32_t offset; /* byte offset in the program blob */
   uint32_t delta;
   uint32_t type;   /* upstream intel_shader_reloc_type */
} mglk_reloc;

typedef struct mglk_kernel_info {
   uint32_t abi_version;
   uint32_t pci_device_id;
   uint32_t verx10;
   /* Program blob = EU instructions followed by constant data. */
   uint32_t program_size;
   uint32_t const_data_offset;
   uint32_t const_data_size;
   uint32_t simd_mask;          /* bit0=SIMD8, bit1=SIMD16, bit2=SIMD32 */
   uint32_t prog_offset[3];     /* per SIMD variant, valid where simd_mask */
   uint32_t prog_spilled_mask;
   uint32_t local_size[3];
   uint32_t dispatch_simd_size;
   uint32_t dispatch_threads;
   uint32_t dispatch_group_size;
   uint32_t dispatch_right_mask;
   uint32_t grf_used;
   uint32_t dispatch_grf_start_reg;
   uint32_t total_scratch;
   uint32_t total_shared;
   uint32_t push_cross_thread_dwords;
   uint32_t push_per_thread_dwords;
   /* Per-thread push dword the dispatcher must fill with each HW thread's
    * subgroup id (upstream brw_nir_lower_cs_subgroup_id), or
    * MGLK_NO_PUSH_DWORD when the kernel reads no subgroup id. */
   uint32_t subgroup_id_push_dword;
   uint32_t uses_barrier;
   uint32_t uses_sampler;
   uint32_t uses_dynamic_bti;
   uint32_t uses_ubo_pull; /* upstream has_ubo_pull: UBO read via dataport */
   uint32_t num_resources;
   uint32_t num_relocs;
   /* Upstream gen_decode/gen_validate results over the program bytes. */
   uint32_t decoded_instructions;
   uint32_t compacted_instructions;
   uint32_t validation_errors;
} mglk_kernel_info;

typedef struct mglk_compiler mglk_compiler;
typedef struct mglk_kernel mglk_kernel;
typedef struct nir_shader nir_shader;
typedef struct nir_shader_compiler_options nir_shader_compiler_options;

/* Accepts only the GLK PCI IDs above; anything else (including other Gen9
 * parts and Xe/GuC parts) is MGLK_E_UNSUPPORTED_DEVICE. */
mglk_status mglk_compiler_create(uint32_t pci_device_id, mglk_compiler **out,
                                 mglk_diagnostic *diag);
void mglk_compiler_destroy(mglk_compiler *compiler);

mglk_status mglk_compiler_describe(const mglk_compiler *compiler,
                                   mglk_device_desc *out);

/* NIR options a frontend must use for compute shaders given to
 * mglk_compile_compute_nir().  Owned by the compiler. */
const nir_shader_compiler_options *
mglk_compiler_nir_options(const mglk_compiler *compiler);

/* Typed NIR input.  SSBO/UBO block indices must already be Gen9 binding
 * table indices (as iris/anv produce after their binding-table pass). */
mglk_status mglk_compile_compute_nir(mglk_compiler *compiler, nir_shader *nir,
                                     mglk_kernel **out, mglk_diagnostic *diag);

/* SPIR-V input, translated by the upstream spirv_to_nir; every
 * (DescriptorSet, Binding) the module references must appear in bindings. */
mglk_status mglk_compile_compute_spirv(mglk_compiler *compiler,
                                       const uint32_t *words, size_t word_count,
                                       const char *entry_point,
                                       const mglk_binding *bindings,
                                       size_t binding_count, mglk_kernel **out,
                                       mglk_diagnostic *diag);

void mglk_kernel_destroy(mglk_kernel *kernel);

mglk_status mglk_kernel_get_info(const mglk_kernel *kernel,
                                 mglk_kernel_info *out);
const uint8_t *mglk_kernel_program(const mglk_kernel *kernel, size_t *size);
const mglk_resource_use *mglk_kernel_resources(const mglk_kernel *kernel,
                                               size_t *count);
const mglk_reloc *mglk_kernel_relocs(const mglk_kernel *kernel, size_t *count);
/* Upstream gen_print disassembly (with inline validation) of the program. */
const char *mglk_kernel_disassembly(const mglk_kernel *kernel);
/* Upstream compiler log lines (shader_debug_log/perf_log), may be empty. */
const char *mglk_kernel_log(const mglk_kernel *kernel);

const char *mglk_status_name(mglk_status status);

#ifdef __cplusplus
}
#endif

#endif
