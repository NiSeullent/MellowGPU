/*
 * MellowGPU NativeGlkCompiler adapter: GLK (Gen9 LP) compute kernels through
 * the pinned upstream Mesa NIR/SPIR-V frontend and brw backend.
 *
 * SPDX-License-Identifier: MIT
 *
 * Every GPU instruction this file returns is produced by upstream
 * brw_compile() (dispatching to brw_compile_cs) and checked again with the
 * upstream gen_decode/gen_validate/gen_print ISA layer.  The adapter only
 * performs input bounding, frontend lowering in the order iris/anv use,
 * binding-table mapping and metadata extraction.
 */
#include "mellow_glk_compiler.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "brw_compiler.h"
#include "brw_nir.h"
#include "compiler/glsl_types.h"
#include "compiler/nir/nir.h"
#include "compiler/nir/nir_builder.h"
#include "compiler/spirv/nir_spirv.h"
#include "compiler/spirv/spirv_info.h"
#include "dev/intel_debug.h"
#include "dev/intel_device_info.h"
#include "gen.h"
#include "util/ralloc.h"
#include "util/u_math.h"

#define MGLK_LOG_CAP (64u * 1024u)
#define MGLK_SPIRV_MAGIC 0x07230203u
#define MGLK_SPIRV_OP_ENTRY_POINT 15u
#define MGLK_SPIRV_OP_VARIABLE 59u
#define MGLK_SPIRV_MODEL_GLCOMPUTE 5u

struct mglk_log {
   char *text;
   size_t len;
   bool truncated;
   void *mem_ctx;
};

struct mglk_compiler {
   void *mem_ctx;
   struct intel_device_info devinfo;
   struct brw_compiler *brw;
   bool debug_env_present;
};

struct mglk_kernel {
   void *mem_ctx;
   mglk_kernel_info info;
   uint8_t *program;
   mglk_resource_use *resources;
   mglk_reloc *relocs;
   char *disassembly;
   struct mglk_log log;
};

static const char *const status_names[] = {
   [MGLK_OK] = "MGLK_OK",
   [MGLK_E_INVALID_ARGUMENT] = "MGLK_E_INVALID_ARGUMENT",
   [MGLK_E_UNSUPPORTED_DEVICE] = "MGLK_E_UNSUPPORTED_DEVICE",
   [MGLK_E_UNSUPPORTED_INPUT] = "MGLK_E_UNSUPPORTED_INPUT",
   [MGLK_E_MALFORMED_INPUT] = "MGLK_E_MALFORMED_INPUT",
   [MGLK_E_FRONTEND_REJECTED] = "MGLK_E_FRONTEND_REJECTED",
   [MGLK_E_COMPILE_FAILED] = "MGLK_E_COMPILE_FAILED",
   [MGLK_E_ISA_VALIDATION_FAILED] = "MGLK_E_ISA_VALIDATION_FAILED",
   [MGLK_E_LIMIT_EXCEEDED] = "MGLK_E_LIMIT_EXCEEDED",
   [MGLK_E_OUT_OF_MEMORY] = "MGLK_E_OUT_OF_MEMORY",
};

const char *
mglk_status_name(mglk_status status)
{
   if ((unsigned)status < ARRAY_SIZE(status_names) && status_names[status])
      return status_names[status];
   return "MGLK_E_UNKNOWN";
}

PRINTFLIKE(3, 4) static mglk_status
diag_set(mglk_diagnostic *diag, mglk_status status, const char *fmt, ...)
{
   if (diag) {
      va_list ap;
      va_start(ap, fmt);
      diag->status = status;
      vsnprintf(diag->message, sizeof(diag->message), fmt, ap);
      va_end(ap);
   }
   return status;
}

static void
diag_ok(mglk_diagnostic *diag)
{
   if (diag) {
      diag->status = MGLK_OK;
      diag->message[0] = '\0';
   }
}

static void
log_vappend(struct mglk_log *log, const char *fmt, va_list ap)
{
   if (!log || log->truncated)
      return;
   char line[1024];
   int n = vsnprintf(line, sizeof(line), fmt, ap);
   if (n < 0)
      return;
   size_t add = MIN2((size_t)n, sizeof(line) - 1);
   if (log->len + add + 2 > MGLK_LOG_CAP) {
      log->truncated = true;
      return;
   }
   char *grown = reralloc_size(log->mem_ctx, log->text, log->len + add + 2);
   if (!grown) {
      log->truncated = true;
      return;
   }
   log->text = grown;
   memcpy(log->text + log->len, line, add);
   log->len += add;
   if (add == 0 || line[add - 1] != '\n')
      log->text[log->len++] = '\n';
   log->text[log->len] = '\0';
}

PRINTFLIKE(3, 4) static void
mglk_shader_log(void *data, unsigned *id, const char *fmt, ...)
{
   (void)id;
   va_list ap;
   va_start(ap, fmt);
   log_vappend(data, fmt, ap);
   va_end(ap);
}

static void
mglk_log_printf(struct mglk_log *log, const char *fmt, ...)
{
   va_list ap;
   va_start(ap, fmt);
   log_vappend(log, fmt, ap);
   va_end(ap);
}

mglk_status
mglk_compiler_create(uint32_t pci_device_id, mglk_compiler **out,
                     mglk_diagnostic *diag)
{
   if (!out)
      return diag_set(diag, MGLK_E_INVALID_ARGUMENT, "out must not be NULL");
   *out = NULL;
   if (pci_device_id != MGLK_PCI_GLK_3X6 && pci_device_id != MGLK_PCI_GLK_2X6)
      return diag_set(diag, MGLK_E_UNSUPPORTED_DEVICE,
                      "PCI 8086:%04x is not a Gemini Lake (Gen9 LP) device; "
                      "this adapter only targets GLK 8086:3184/8086:3185",
                      pci_device_id);

   void *mem_ctx = ralloc_context(NULL);
   if (!mem_ctx)
      return diag_set(diag, MGLK_E_OUT_OF_MEMORY, "ralloc_context failed");
   mglk_compiler *compiler = rzalloc(mem_ctx, mglk_compiler);
   if (!compiler) {
      ralloc_free(mem_ctx);
      return diag_set(diag, MGLK_E_OUT_OF_MEMORY, "compiler allocation failed");
   }
   compiler->mem_ctx = mem_ctx;

   if (!intel_get_device_info_from_pci_id(pci_device_id, &compiler->devinfo)) {
      ralloc_free(mem_ctx);
      return diag_set(diag, MGLK_E_UNSUPPORTED_DEVICE,
                      "pinned Mesa %s has no device info for 8086:%04x",
                      MGLK_MESA_COMMIT, pci_device_id);
   }
   const struct intel_device_info *devinfo = &compiler->devinfo;
   if (devinfo->ver != 9 || devinfo->verx10 != 90 ||
       devinfo->platform != INTEL_PLATFORM_GLK) {
      ralloc_free(mem_ctx);
      return diag_set(diag, MGLK_E_UNSUPPORTED_DEVICE,
                      "8086:%04x resolved to verx10=%d platform=%d, not Gen9 GLK",
                      pci_device_id, devinfo->verx10, (int)devinfo->platform);
   }

   /* iris/anv parse INTEL_DEBUG/INTEL_SIMD_DEBUG before creating brw; the
    * parse also sets the default SIMD8/16/32 enable mask brw selects from.
    * Callers wanting reproducible output must leave both unset. */
   process_intel_debug_variable();
   compiler->debug_env_present =
      getenv("INTEL_DEBUG") != NULL || getenv("INTEL_SIMD_DEBUG") != NULL;
   glsl_type_singleton_init_or_ref();
   compiler->brw = brw_compiler_create(mem_ctx, devinfo);
   if (!compiler->brw) {
      glsl_type_singleton_decref();
      ralloc_free(mem_ctx);
      return diag_set(diag, MGLK_E_OUT_OF_MEMORY, "brw_compiler_create failed");
   }
   compiler->brw->shader_debug_log = mglk_shader_log;
   compiler->brw->shader_perf_log = mglk_shader_log;

   *out = compiler;
   diag_ok(diag);
   return MGLK_OK;
}

void
mglk_compiler_destroy(mglk_compiler *compiler)
{
   if (!compiler)
      return;
   glsl_type_singleton_decref();
   ralloc_free(compiler->mem_ctx);
}

mglk_status
mglk_compiler_describe(const mglk_compiler *compiler, mglk_device_desc *out)
{
   if (!compiler || !out)
      return MGLK_E_INVALID_ARGUMENT;
   const struct intel_device_info *d = &compiler->devinfo;
   memset(out, 0, sizeof(*out));
   out->abi_version = MGLK_ABI_VERSION;
   out->pci_device_id = (uint32_t)d->pci_device_id;
   out->ver = (uint32_t)d->ver;
   out->verx10 = (uint32_t)d->verx10;
   out->gt = (uint32_t)d->gt;
   out->is_gen9_lp = d->platform == INTEL_PLATFORM_GLK;
   out->num_slices = d->num_slices;
   out->num_subslices_total = d->subslice_total;
   out->max_eus_per_subslice = d->max_eus_per_subslice;
   out->num_thread_per_eu = d->num_thread_per_eu;
   out->max_cs_threads = d->max_cs_threads;
   out->max_cs_workgroup_threads = d->max_cs_workgroup_threads;
   out->max_workgroup_invocations =
      MIN2(MGLK_MAX_WORKGROUP_DIM, d->max_cs_workgroup_threads * 32u);
   out->grf_registers = 128;
   out->timestamp_frequency = d->timestamp_frequency;
   out->topology_source = MGLK_TOPOLOGY_STATIC_PCI_TABLE_FUSING_UNKNOWN;
   out->runtime_fused_topology_known = 0;
   out->debug_env_present = compiler->debug_env_present;
   snprintf(out->platform_name, sizeof(out->platform_name), "GLK");
   snprintf(out->marketing_name, sizeof(out->marketing_name), "%s", d->name);
   snprintf(out->mesa_commit, sizeof(out->mesa_commit), "%s", MGLK_MESA_COMMIT);
   return MGLK_OK;
}

const nir_shader_compiler_options *
mglk_compiler_nir_options(const mglk_compiler *compiler)
{
   return compiler ? &compiler->brw->nir_options[MESA_SHADER_COMPUTE] : NULL;
}

static mglk_kernel *
kernel_alloc(void)
{
   void *mem_ctx = ralloc_context(NULL);
   if (!mem_ctx)
      return NULL;
   mglk_kernel *kernel = rzalloc(mem_ctx, mglk_kernel);
   if (!kernel) {
      ralloc_free(mem_ctx);
      return NULL;
   }
   kernel->mem_ctx = mem_ctx;
   kernel->log.mem_ctx = mem_ctx;
   kernel->log.text = ralloc_strdup(mem_ctx, "");
   return kernel;
}

void
mglk_kernel_destroy(mglk_kernel *kernel)
{
   if (kernel)
      ralloc_free(kernel->mem_ctx);
}

static mglk_status
check_compute_nir(const mglk_compiler *compiler, const nir_shader *nir,
                  mglk_diagnostic *diag)
{
   if (nir->info.stage != MESA_SHADER_COMPUTE)
      return diag_set(diag, MGLK_E_UNSUPPORTED_INPUT,
                      "stage %s is not supported; only compute kernels are",
                      mesa_shader_stage_name(nir->info.stage));
   if (nir->options != &compiler->brw->nir_options[MESA_SHADER_COMPUTE])
      return diag_set(diag, MGLK_E_INVALID_ARGUMENT,
                      "nir_shader must be created with mglk_compiler_nir_options()");
   if (nir->info.workgroup_size_variable)
      return diag_set(diag, MGLK_E_UNSUPPORTED_INPUT,
                      "variable workgroup size is not supported");
   const unsigned limit =
      MIN2(MGLK_MAX_WORKGROUP_DIM, compiler->devinfo.max_cs_workgroup_threads * 32u);
   uint64_t invocations = 1;
   for (unsigned i = 0; i < 3; i++) {
      const unsigned dim = nir->info.workgroup_size[i];
      if (dim == 0 || dim > MGLK_MAX_WORKGROUP_DIM)
         return diag_set(diag, MGLK_E_LIMIT_EXCEEDED,
                         "workgroup_size[%u]=%u outside 1..%u", i, dim,
                         MGLK_MAX_WORKGROUP_DIM);
      invocations *= dim;
   }
   if (invocations > limit)
      return diag_set(diag, MGLK_E_LIMIT_EXCEEDED,
                      "workgroup has %llu invocations; GLK limit is %u",
                      (unsigned long long)invocations, limit);
   if (!nir_shader_get_entrypoint((nir_shader *)nir))
      return diag_set(diag, MGLK_E_MALFORMED_INPUT, "nir_shader has no entrypoint");
   return MGLK_OK;
}

struct resource_scan {
   uint32_t access[MGLK_MAX_BTI + 1];
   uint8_t kind[MGLK_MAX_BTI + 1];
   bool dynamic;
   bool out_of_range;
   uint32_t bad_bti;
};

static void
note_resource(struct resource_scan *scan, nir_src *index, mglk_resource_kind kind,
              uint32_t access)
{
   if (!nir_src_is_const(*index)) {
      scan->dynamic = true;
      return;
   }
   const uint64_t bti = nir_src_as_uint(*index);
   if (bti > MGLK_MAX_BTI) {
      scan->out_of_range = true;
      scan->bad_bti = (uint32_t)MIN2(bti, UINT32_MAX);
      return;
   }
   scan->access[bti] |= access;
   scan->kind[bti] = (uint8_t)kind;
}

static void
scan_resources(nir_shader *nir, struct resource_scan *scan)
{
   memset(scan, 0, sizeof(*scan));
   nir_foreach_function_impl(impl, nir) {
      nir_foreach_block(block, impl) {
         nir_foreach_instr(instr, block) {
            if (instr->type != nir_instr_type_intrinsic)
               continue;
            nir_intrinsic_instr *intrin = nir_instr_as_intrinsic(instr);
            switch (intrin->intrinsic) {
            case nir_intrinsic_load_ssbo:
            case nir_intrinsic_get_ssbo_size:
               note_resource(scan, &intrin->src[0], MGLK_RESOURCE_SSBO, MGLK_ACCESS_READ);
               break;
            case nir_intrinsic_store_ssbo:
               note_resource(scan, &intrin->src[1], MGLK_RESOURCE_SSBO, MGLK_ACCESS_WRITE);
               break;
            case nir_intrinsic_ssbo_atomic:
            case nir_intrinsic_ssbo_atomic_swap:
               note_resource(scan, &intrin->src[0], MGLK_RESOURCE_SSBO,
                             MGLK_ACCESS_ATOMIC | MGLK_ACCESS_READ | MGLK_ACCESS_WRITE);
               break;
            case nir_intrinsic_load_ubo:
               note_resource(scan, &intrin->src[0], MGLK_RESOURCE_UBO, MGLK_ACCESS_READ);
               break;
            default:
               break;
            }
         }
      }
   }
}

static mglk_status
finish_isa_checks(const mglk_compiler *compiler, mglk_kernel *kernel,
                  const struct brw_cs_prog_data *prog_data, mglk_diagnostic *diag)
{
   const struct intel_device_info *devinfo = &compiler->devinfo;
   const unsigned code_size = prog_data->base.const_data_size ?
      prog_data->base.const_data_offset : prog_data->base.program_size;
   void *tmp = ralloc_context(kernel->mem_ctx);
   if (!tmp)
      return diag_set(diag, MGLK_E_OUT_OF_MEMORY, "ISA check context");

   /* Each SIMD variant is a complete program; check them independently. */
   unsigned starts[3], count = 0;
   for (unsigned simd = 0; simd < 3; simd++) {
      if (prog_data->prog_mask & (1u << simd))
         starts[count++] = prog_data->prog_offset[simd];
   }
   mglk_status status = MGLK_OK;
   char *dis = NULL;
   size_t dis_len = 0;
   FILE *fp = open_memstream(&dis, &dis_len);
   if (!fp) {
      ralloc_free(tmp);
      return diag_set(diag, MGLK_E_OUT_OF_MEMORY, "open_memstream failed");
   }
   for (unsigned v = 0; v < count && status == MGLK_OK; v++) {
      unsigned begin = starts[v], end = code_size;
      for (unsigned w = 0; w < count; w++) {
         if (starts[w] > begin && starts[w] < end)
            end = starts[w];
      }
      if (begin >= end || end > code_size) {
         status = diag_set(diag, MGLK_E_ISA_VALIDATION_FAILED,
                           "SIMD variant range [%u,%u) outside code size %u",
                           begin, end, code_size);
         break;
      }
      const uint8_t *bytes = kernel->program + begin;
      const int size = (int)(end - begin);

      gen_scan_raw_layout_params scan = {
         .raw_bytes = bytes,
         .raw_bytes_size = size,
         .layouts = ralloc_array(tmp, gen_inst_layout, size / 8 + 1),
         .num_insts = size / 8 + 1,
      };
      if (!scan.layouts || !gen_scan_raw_layout(&scan)) {
         status = diag_set(diag, MGLK_E_ISA_VALIDATION_FAILED,
                           "gen_scan_raw_layout rejected variant at offset %u", begin);
         break;
      }
      for (int i = 0; i < scan.num_insts; i++)
         kernel->info.compacted_instructions += scan.layouts[i].was_compacted;

      gen_decode_params dec = {
         .devinfo = devinfo,
         .raw_bytes = bytes,
         .raw_bytes_size = size,
         .mem_ctx = tmp,
      };
      if (!gen_decode(&dec) || dec.num_errors) {
         status = diag_set(diag, MGLK_E_ISA_VALIDATION_FAILED,
                           "gen_decode failed at offset %u: %s", begin,
                           dec.num_errors ? dec.errors[0].msg : "unknown");
         break;
      }
      kernel->info.decoded_instructions += (uint32_t)dec.num_insts;

      gen_validate_params val = {
         .devinfo = devinfo,
         .insts = dec.insts,
         .num_insts = dec.num_insts,
         .mem_ctx = tmp,
      };
      if (!gen_validate(&val) || val.num_errors) {
         kernel->info.validation_errors += (uint32_t)MAX2(val.num_errors, 1);
         status = diag_set(diag, MGLK_E_ISA_VALIDATION_FAILED,
                           "gen_validate: %d error(s), first at inst %u: %s",
                           val.num_errors,
                           val.num_errors ? val.errors[0].index : 0,
                           val.num_errors ? val.errors[0].msg : "unknown");
      }

      fprintf(fp, "# SIMD variant @0x%x (%d bytes)\n", begin, size);
      gen_print_params print = {
         .devinfo = devinfo,
         .fp = fp,
         .flags = GEN_PRINT_BYTE_OFFSETS | GEN_PRINT_HEX | GEN_PRINT_IGNORE_ENV,
         .insts = dec.insts,
         .num_insts = dec.num_insts,
         .errors = val.errors,
         .num_errors = val.num_errors,
         .raw_bytes = bytes,
         .raw_bytes_size = size,
      };
      gen_print(&print);
   }
   fclose(fp);
   kernel->disassembly = ralloc_strndup(kernel->mem_ctx, dis ? dis : "", dis_len);
   free(dis);
   ralloc_free(tmp);
   return status;
}

/* Shared tail for both inputs: the iris compute sequence around brw_compile. */
static mglk_status
compile_compute(mglk_compiler *compiler, mglk_kernel *kernel, nir_shader *nir,
                mglk_diagnostic *diag)
{
   const struct intel_device_info *devinfo = &compiler->devinfo;
   bool progress = false;

   /* Dispatch base is not part of this contract: with explicit options
    * (NULL leaves load_base_workgroup_id, which brw cannot emit) upstream
    * folds base workgroup/global ids to zero, as anv does for its kernels. */
   const nir_lower_compute_system_values_options csv_options = {
      .has_base_global_invocation_id = false,
      .has_base_workgroup_id = false,
   };
   NIR_PASS(progress, nir, nir_lower_compute_system_values, &csv_options);
   const struct brw_nir_compiler_opts opts = {0};
   brw_preprocess_nir(compiler->brw, nir, &opts);
   NIR_PASS(progress, nir, brw_nir_lower_cs_intrinsics, devinfo, NULL);
   (void)progress;

   struct resource_scan *scan = rzalloc(kernel->mem_ctx, struct resource_scan);
   if (!scan)
      return diag_set(diag, MGLK_E_OUT_OF_MEMORY, "resource scan allocation");
   scan_resources(nir, scan);
   if (scan->out_of_range)
      return diag_set(diag, MGLK_E_UNSUPPORTED_INPUT,
                      "buffer binding table index %u exceeds Gen9 limit %u",
                      scan->bad_bti, MGLK_MAX_BTI);

   struct brw_cs_prog_data *prog_data = rzalloc(kernel->mem_ctx, struct brw_cs_prog_data);
   if (!prog_data)
      return diag_set(diag, MGLK_E_OUT_OF_MEMORY, "prog_data allocation");
   bool subgroup_id_lowered = false;
   NIR_PASS(subgroup_id_lowered, nir, brw_nir_lower_cs_subgroup_id, devinfo, 0);
   if (subgroup_id_lowered) {
      prog_data->base.push_sizes[0] = 4;
      brw_cs_fill_push_const_info(devinfo, prog_data, 0);
      prog_data->base.push_sizes[0] = align(4, REG_SIZE);
   } else {
      brw_cs_fill_push_const_info(devinfo, prog_data, -1);
   }
   /* source_blake3 is a uint8_t array; copy instead of type-punning it. */
   memcpy(&prog_data->base.source_hash, nir->info.source_blake3,
          sizeof(prog_data->base.source_hash));

   struct brw_cs_prog_key key;
   memset(&key, 0, sizeof(key));
   key.base.vue_layout = INTEL_VUE_LAYOUT_SEPARATE;

   struct brw_compile_cs_params params = {
      .base = {
         .mem_ctx = kernel->mem_ctx,
         .nir = nir,
         .key = &key.base,
         .prog_data = &prog_data->base,
         .log_data = &kernel->log,
      },
   };
   const unsigned *assembly = brw_compile(compiler->brw, &params.base);
   if (!assembly)
      return diag_set(diag, MGLK_E_COMPILE_FAILED, "brw_compile: %s",
                      params.base.error_str ? params.base.error_str : "no error string");
   if (prog_data->base.program_size == 0 || prog_data->prog_mask == 0)
      return diag_set(diag, MGLK_E_COMPILE_FAILED,
                      "brw_compile returned an empty program (size=%u mask=%u)",
                      prog_data->base.program_size, prog_data->prog_mask);

   kernel->program = ralloc_size(kernel->mem_ctx, prog_data->base.program_size);
   if (!kernel->program)
      return diag_set(diag, MGLK_E_OUT_OF_MEMORY, "program copy");
   memcpy(kernel->program, assembly, prog_data->base.program_size);

   mglk_kernel_info *info = &kernel->info;
   info->abi_version = MGLK_ABI_VERSION;
   info->pci_device_id = (uint32_t)devinfo->pci_device_id;
   info->verx10 = (uint32_t)devinfo->verx10;
   info->program_size = prog_data->base.program_size;
   info->const_data_offset = prog_data->base.const_data_offset;
   info->const_data_size = prog_data->base.const_data_size;
   info->simd_mask = prog_data->prog_mask;
   for (unsigned i = 0; i < 3; i++) {
      info->prog_offset[i] = prog_data->prog_offset[i];
      info->local_size[i] = prog_data->local_size[i];
   }
   info->prog_spilled_mask = prog_data->prog_spilled;
   const struct intel_cs_dispatch_info dispatch =
      brw_cs_get_dispatch_info(devinfo, prog_data, NULL);
   info->dispatch_simd_size = dispatch.simd_size;
   info->dispatch_threads = dispatch.threads;
   info->dispatch_group_size = dispatch.group_size;
   info->dispatch_right_mask = dispatch.right_mask;
   info->grf_used = prog_data->base.grf_used;
   info->dispatch_grf_start_reg = prog_data->base.dispatch_grf_start_reg;
   info->total_scratch = prog_data->base.total_scratch;
   info->total_shared = prog_data->base.total_shared;
   info->push_cross_thread_dwords = prog_data->push.cross_thread.dwords;
   info->push_per_thread_dwords = prog_data->push.per_thread.dwords;
   /* brw_nir_lower_cs_subgroup_id was given push offset 0 above. */
   info->subgroup_id_push_dword = subgroup_id_lowered ? 0 : MGLK_NO_PUSH_DWORD;
   info->uses_barrier = prog_data->uses_barrier;
   info->uses_sampler = prog_data->uses_sampler;
   info->uses_dynamic_bti = scan->dynamic;
   info->uses_ubo_pull = prog_data->base.has_ubo_pull;

   unsigned used = 0;
   for (unsigned bti = 0; bti <= MGLK_MAX_BTI; bti++)
      used += scan->access[bti] != 0;
   kernel->resources = rzalloc_array(kernel->mem_ctx, mglk_resource_use, MAX2(used, 1));
   if (!kernel->resources)
      return diag_set(diag, MGLK_E_OUT_OF_MEMORY, "resource table");
   for (unsigned bti = 0, n = 0; bti <= MGLK_MAX_BTI; bti++) {
      if (scan->access[bti]) {
         kernel->resources[n++] = (mglk_resource_use){
            .bti = bti,
            .kind = (mglk_resource_kind)scan->kind[bti],
            .access = scan->access[bti],
         };
      }
   }
   info->num_resources = used;

   info->num_relocs = prog_data->base.num_relocs;
   kernel->relocs = rzalloc_array(kernel->mem_ctx, mglk_reloc, MAX2(info->num_relocs, 1));
   if (!kernel->relocs)
      return diag_set(diag, MGLK_E_OUT_OF_MEMORY, "reloc table");
   for (unsigned i = 0; i < info->num_relocs; i++) {
      const struct intel_shader_reloc *r = &prog_data->base.relocs[i];
      kernel->relocs[i] = (mglk_reloc){
         .id = r->id, .offset = r->offset, .delta = r->delta, .type = (uint32_t)r->type,
      };
   }

   mglk_log_printf(&kernel->log, "mglk: brw_compile ok: %u bytes, simd_mask=0x%x, "
                   "mesa=%s, debug_env=%d", info->program_size, info->simd_mask,
                   MGLK_MESA_COMMIT, compiler->debug_env_present);
   return finish_isa_checks(compiler, kernel, prog_data, diag);
}

mglk_status
mglk_compile_compute_nir(mglk_compiler *compiler, nir_shader *nir,
                         mglk_kernel **out, mglk_diagnostic *diag)
{
   if (out)
      *out = NULL;
   if (!compiler || !nir || !out) {
      if (nir)
         ralloc_free(nir);
      return diag_set(diag, MGLK_E_INVALID_ARGUMENT,
                      "compiler, nir and out must not be NULL");
   }
   mglk_kernel *kernel = kernel_alloc();
   if (!kernel) {
      ralloc_free(nir);
      return diag_set(diag, MGLK_E_OUT_OF_MEMORY, "kernel allocation");
   }
   ralloc_steal(kernel->mem_ctx, nir);

   mglk_status status = check_compute_nir(compiler, nir, diag);
   if (status == MGLK_OK)
      status = compile_compute(compiler, kernel, nir, diag);
   if (status != MGLK_OK) {
      mglk_kernel_destroy(kernel);
      return status;
   }
   *out = kernel;
   diag_ok(diag);
   return MGLK_OK;
}

struct spirv_scan_result {
   bool found_compute;
   bool found_other_model;
   uint32_t other_model;
};

/* Storage classes the binding/lowering path below maps: Input, Uniform,
 * Workgroup, Private, Function, StorageBuffer.  Anything else (push
 * constants, images/samplers, physical buffers) would reach brw unlowered. */
static bool
spirv_storage_supported(uint32_t storage)
{
   return storage == 1u || storage == 2u || storage == 4u || storage == 6u ||
          storage == 7u || storage == 12u;
}

/* Bounded structural walk; anything malformed is rejected before vtn. */
static mglk_status
scan_spirv(const uint32_t *words, size_t count, const char *entry,
           struct spirv_scan_result *res, mglk_diagnostic *diag)
{
   memset(res, 0, sizeof(*res));
   if (count < 5)
      return diag_set(diag, MGLK_E_MALFORMED_INPUT,
                      "SPIR-V module has %zu words; header needs 5", count);
   if (words[0] != MGLK_SPIRV_MAGIC)
      return diag_set(diag, MGLK_E_MALFORMED_INPUT,
                      "bad SPIR-V magic 0x%08x%s", words[0],
                      words[0] == 0x03022307u ? " (byte-swapped modules unsupported)" : "");
   const uint32_t version = words[1];
   if ((version & 0xff0000ffu) != 0 || version < 0x00010000u || version > 0x00010600u)
      return diag_set(diag, MGLK_E_MALFORMED_INPUT,
                      "unsupported SPIR-V version word 0x%08x", version);
   if (words[3] == 0 || words[3] > MGLK_MAX_SPIRV_WORDS)
      return diag_set(diag, MGLK_E_MALFORMED_INPUT, "invalid SPIR-V id bound %u", words[3]);
   for (size_t i = 5; i < count;) {
      const uint32_t wc = words[i] >> 16, op = words[i] & 0xffffu;
      if (wc == 0 || wc > count - i)
         return diag_set(diag, MGLK_E_MALFORMED_INPUT,
                         "SPIR-V instruction opcode %u at word %zu has word count %u "
                         "past module end (%zu words)", op, i, wc, count);
      if (op == MGLK_SPIRV_OP_ENTRY_POINT) {
         if (wc < 4)
            return diag_set(diag, MGLK_E_MALFORMED_INPUT,
                            "OpEntryPoint at word %zu too short", i);
         const char *name = (const char *)&words[i + 3];
         const size_t max_len = (size_t)(wc - 3) * 4;
         if (strnlen(name, max_len) == max_len)
            return diag_set(diag, MGLK_E_MALFORMED_INPUT,
                            "OpEntryPoint name at word %zu is not NUL terminated", i);
         if (strcmp(name, entry) == 0) {
            if (words[i + 1] == MGLK_SPIRV_MODEL_GLCOMPUTE) {
               res->found_compute = true;
            } else {
               res->found_other_model = true;
               res->other_model = words[i + 1];
            }
         }
      }
      if (op == MGLK_SPIRV_OP_VARIABLE) {
         if (wc < 4)
            return diag_set(diag, MGLK_E_MALFORMED_INPUT,
                            "OpVariable at word %zu too short", i);
         if (!spirv_storage_supported(words[i + 3]))
            return diag_set(diag, MGLK_E_UNSUPPORTED_INPUT,
                            "OpVariable %%%u uses storage class %u; only Input, Uniform, "
                            "Workgroup, Private, Function and StorageBuffer are supported",
                            words[i + 2], words[i + 3]);
      }
      i += wc;
   }
   if (!res->found_compute) {
      if (res->found_other_model)
         return diag_set(diag, MGLK_E_UNSUPPORTED_INPUT,
                         "entry point '%s' has execution model %u, not GLCompute",
                         entry, res->other_model);
      return diag_set(diag, MGLK_E_UNSUPPORTED_INPUT,
                      "no GLCompute entry point named '%s'", entry);
   }
   return MGLK_OK;
}

static void
mglk_spirv_debug(void *data, enum nir_spirv_debug_level level, size_t offset,
                 const char *message)
{
   static const char *const names[] = { "info", "warning", "error" };
   const char *lvl = (level >= 0 && (unsigned)level < ARRAY_SIZE(names)) ?
      names[level] : "invalid";
   mglk_log_printf(data, "spirv_to_nir %s @%zu: %s", lvl, offset, message);
}

struct binding_lower_state {
   const mglk_binding *bindings;
   size_t count;
   char *error;
   void *mem_ctx;
};

static bool
lower_vulkan_resource_index(nir_builder *b, nir_intrinsic_instr *intrin, void *data)
{
   struct binding_lower_state *state = data;
   if (state->error)
      return false;
   switch (intrin->intrinsic) {
   case nir_intrinsic_vulkan_resource_index: {
      const uint32_t set = nir_intrinsic_desc_set(intrin);
      const uint32_t binding = nir_intrinsic_binding(intrin);
      const uint32_t desc_type = nir_intrinsic_desc_type(intrin);
      const mglk_binding *match = NULL;
      for (size_t i = 0; i < state->count; i++) {
         if (state->bindings[i].set == set && state->bindings[i].binding == binding)
            match = &state->bindings[i];
      }
      if (!match) {
         state->error = ralloc_asprintf(state->mem_ctx,
                                        "SPIR-V references unmapped resource "
                                        "(set=%u, binding=%u)", set, binding);
         return false;
      }
      /* vtn tags the index with nir_descriptor_type at this pin. */
      const uint32_t want = match->kind == MGLK_RESOURCE_SSBO ?
         nir_descriptor_type_storage_buffer : nir_descriptor_type_uniform_buffer;
      if (desc_type != want) {
         state->error = ralloc_asprintf(state->mem_ctx,
                                        "(set=%u, binding=%u) is descriptor type %u "
                                        "but mapped as %s", set, binding, desc_type,
                                        match->kind == MGLK_RESOURCE_SSBO ? "SSBO" : "UBO");
         return false;
      }
      if (!nir_src_is_const(intrin->src[0]) || nir_src_as_uint(intrin->src[0]) != 0) {
         state->error = ralloc_asprintf(state->mem_ctx,
                                        "descriptor arrays are not supported "
                                        "(set=%u, binding=%u)", set, binding);
         return false;
      }
      b->cursor = nir_before_instr(&intrin->instr);
      nir_def *index = nir_vec2(b, nir_imm_int(b, (int)match->bti), nir_imm_int(b, 0));
      nir_def_replace(&intrin->def, index);
      return true;
   }
   case nir_intrinsic_vulkan_resource_reindex:
      state->error = ralloc_strdup(state->mem_ctx,
                                   "descriptor array reindexing is not supported");
      return false;
   case nir_intrinsic_load_vulkan_descriptor:
      nir_def_replace(&intrin->def, intrin->src[0].ssa);
      return true;
   default:
      return false;
   }
}

mglk_status
mglk_compile_compute_spirv(mglk_compiler *compiler, const uint32_t *words,
                           size_t word_count, const char *entry_point,
                           const mglk_binding *bindings, size_t binding_count,
                           mglk_kernel **out, mglk_diagnostic *diag)
{
   if (out)
      *out = NULL;
   if (!compiler || !out || !words || !entry_point)
      return diag_set(diag, MGLK_E_INVALID_ARGUMENT,
                      "compiler, words, entry_point and out must not be NULL");
   if (word_count > MGLK_MAX_SPIRV_WORDS)
      return diag_set(diag, MGLK_E_LIMIT_EXCEEDED, "SPIR-V has %zu words; limit %u",
                      word_count, MGLK_MAX_SPIRV_WORDS);
   const size_t entry_len = strnlen(entry_point, MGLK_MAX_ENTRY_NAME + 1);
   if (entry_len == 0 || entry_len > MGLK_MAX_ENTRY_NAME)
      return diag_set(diag, MGLK_E_INVALID_ARGUMENT,
                      "entry point name length must be 1..%u", MGLK_MAX_ENTRY_NAME);
   if (binding_count && !bindings)
      return diag_set(diag, MGLK_E_INVALID_ARGUMENT,
                      "bindings must not be NULL when binding_count is %zu", binding_count);
   if (binding_count > MGLK_MAX_BINDINGS)
      return diag_set(diag, MGLK_E_LIMIT_EXCEEDED,
                      "binding map has %zu entries; limit %u", binding_count,
                      MGLK_MAX_BINDINGS);
   for (size_t i = 0; i < binding_count; i++) {
      const mglk_binding *bi = &bindings[i];
      if (bi->bti > MGLK_MAX_BTI)
         return diag_set(diag, MGLK_E_LIMIT_EXCEEDED, "binding %zu: bti %u > %u",
                         i, bi->bti, MGLK_MAX_BTI);
      if (bi->kind != MGLK_RESOURCE_SSBO && bi->kind != MGLK_RESOURCE_UBO)
         return diag_set(diag, MGLK_E_INVALID_ARGUMENT, "binding %zu: invalid kind %d",
                         i, (int)bi->kind);
      for (size_t j = 0; j < i; j++) {
         if ((bindings[j].set == bi->set && bindings[j].binding == bi->binding) ||
             bindings[j].bti == bi->bti)
            return diag_set(diag, MGLK_E_INVALID_ARGUMENT,
                            "binding %zu duplicates entry %zu (set/binding or bti)", i, j);
      }
   }

   struct spirv_scan_result scan;
   mglk_status status = scan_spirv(words, word_count, entry_point, &scan, diag);
   if (status != MGLK_OK)
      return status;

   mglk_kernel *kernel = kernel_alloc();
   if (!kernel)
      return diag_set(diag, MGLK_E_OUT_OF_MEMORY, "kernel allocation");

   const struct spirv_capabilities caps = {
      .Shader = true,
   };
   const struct spirv_to_nir_options spirv_options = {
      .environment = NIR_SPIRV_VULKAN,
      .capabilities = &caps,
      .ubo_addr_format = nir_address_format_32bit_index_offset,
      .ssbo_addr_format = nir_address_format_32bit_index_offset,
      .phys_ssbo_addr_format = nir_address_format_64bit_global,
      .push_const_addr_format = nir_address_format_logical,
      .shared_addr_format = nir_address_format_32bit_offset,
      .global_addr_format = nir_address_format_64bit_global,
      .temp_addr_format = nir_address_format_32bit_offset,
      .constant_addr_format = nir_address_format_64bit_global,
      .min_ubo_alignment = 64,
      .min_ssbo_alignment = 4,
      .debug = { .func = mglk_spirv_debug, .private_data = &kernel->log },
      /* vtn_fail must longjmp back to us, never trap, in assert builds. */
      .skip_os_break_in_debug_build = true,
   };
   nir_shader *nir = spirv_to_nir(words, word_count, NULL, MESA_SHADER_COMPUTE,
                                  entry_point, &spirv_options,
                                  &compiler->brw->nir_options[MESA_SHADER_COMPUTE]);
   if (!nir) {
      status = diag_set(diag, MGLK_E_FRONTEND_REJECTED,
                        "upstream spirv_to_nir rejected the module: %s",
                        kernel->log.len ? kernel->log.text : "(no message)");
      mglk_kernel_destroy(kernel);
      return status;
   }
   ralloc_steal(kernel->mem_ctx, nir);

   bool progress = false;
   NIR_PASS(progress, nir, nir_lower_variable_initializers, nir_var_function_temp);
   NIR_PASS(progress, nir, nir_lower_returns);
   NIR_PASS(progress, nir, nir_inline_functions);
   NIR_PASS(progress, nir, nir_opt_copy_prop);
   NIR_PASS(progress, nir, nir_opt_deref);
   nir_remove_non_entrypoints(nir);
   NIR_PASS(progress, nir, nir_lower_variable_initializers, ~0);
   NIR_PASS(progress, nir, nir_split_var_copies);
   NIR_PASS(progress, nir, nir_split_per_member_structs);
   NIR_PASS(progress, nir, nir_lower_global_vars_to_local);
   NIR_PASS(progress, nir, nir_lower_vars_to_ssa);
   NIR_PASS(progress, nir, nir_lower_system_values);
   NIR_PASS(progress, nir, nir_lower_vars_to_explicit_types, nir_var_mem_shared,
            glsl_get_natural_size_align_bytes);
   NIR_PASS(progress, nir, nir_lower_explicit_io, nir_var_mem_shared,
            nir_address_format_32bit_offset);

   struct binding_lower_state lower = {
      .bindings = bindings, .count = binding_count, .mem_ctx = kernel->mem_ctx,
   };
   nir_shader_intrinsics_pass(nir, lower_vulkan_resource_index,
                              nir_metadata_control_flow, &lower);
   if (lower.error) {
      status = diag_set(diag, MGLK_E_UNSUPPORTED_INPUT, "%s", lower.error);
      mglk_kernel_destroy(kernel);
      return status;
   }
   NIR_PASS(progress, nir, nir_lower_explicit_io, nir_var_mem_ubo | nir_var_mem_ssbo,
            nir_address_format_32bit_index_offset);
   NIR_PASS(progress, nir, nir_opt_copy_prop);
   NIR_PASS(progress, nir, nir_opt_constant_folding);
   NIR_PASS(progress, nir, nir_opt_dce);
   (void)progress;

   status = check_compute_nir(compiler, nir, diag);
   if (status == MGLK_OK)
      status = compile_compute(compiler, kernel, nir, diag);
   if (status != MGLK_OK) {
      mglk_kernel_destroy(kernel);
      return status;
   }
   *out = kernel;
   diag_ok(diag);
   return MGLK_OK;
}

mglk_status
mglk_kernel_get_info(const mglk_kernel *kernel, mglk_kernel_info *out)
{
   if (!kernel || !out)
      return MGLK_E_INVALID_ARGUMENT;
   *out = kernel->info;
   return MGLK_OK;
}

const uint8_t *
mglk_kernel_program(const mglk_kernel *kernel, size_t *size)
{
   if (size)
      *size = kernel ? kernel->info.program_size : 0;
   return kernel ? kernel->program : NULL;
}

const mglk_resource_use *
mglk_kernel_resources(const mglk_kernel *kernel, size_t *count)
{
   if (count)
      *count = kernel ? kernel->info.num_resources : 0;
   return kernel ? kernel->resources : NULL;
}

const mglk_reloc *
mglk_kernel_relocs(const mglk_kernel *kernel, size_t *count)
{
   if (count)
      *count = kernel ? kernel->info.num_relocs : 0;
   return kernel ? kernel->relocs : NULL;
}

const char *
mglk_kernel_disassembly(const mglk_kernel *kernel)
{
   return kernel && kernel->disassembly ? kernel->disassembly : "";
}

const char *
mglk_kernel_log(const mglk_kernel *kernel)
{
   return kernel && kernel->log.text ? kernel->log.text : "";
}
