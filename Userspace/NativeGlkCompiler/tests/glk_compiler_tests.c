/*
 * Host regression for the NativeGlkCompiler adapter.  Compiles real NIR and
 * SPIR-V compute kernels for GLK (8086:3185) through the pinned upstream brw
 * backend, plus malformed/unsupported inputs, and records ISA + metadata.
 * Never touches a GPU: the ISA produced here has not executed anywhere.
 *
 * SPDX-License-Identifier: MIT
 *
 * usage: glk_compiler_tests <output-dir>   (exit status 0 = all expectations met)
 */
#include "mellow_glk_compiler.h"

#include <errno.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#include "compiler/nir/nir.h"
#include "compiler/nir/nir_builder.h"

static const char *out_dir;
static FILE *json;
static int failures;
static int cases;

static void
expect(bool cond, const char *name, const char *what)
{
   if (!cond) {
      fprintf(stderr, "FAIL %s: %s\n", name, what);
      failures++;
   }
}

static void
json_str(const char *s)
{
   fputc('"', json);
   for (; s && *s; s++) {
      const unsigned char c = (unsigned char)*s;
      if (c == '"' || c == '\\')
         fprintf(json, "\\%c", c);
      else if (c < 0x20)
         fprintf(json, "\\u%04x", c);
      else
         fputc(c, json);
   }
   fputc('"', json);
}

static bool
write_file(const char *name, const char *suffix, const void *data, size_t size)
{
   char path[1024];
   snprintf(path, sizeof(path), "%s/cases/%s%s", out_dir, name, suffix);
   FILE *fp = fopen(path, "wb");
   if (!fp)
      return false;
   const bool ok = fwrite(data, 1, size, fp) == size;
   return fclose(fp) == 0 && ok;
}

/* How a SPIR-V case was invoked, so the runner can replay it through the
 * mglk-compile executable from the recorded .spv file. */
struct spirv_input {
   const char *entry;
   const mglk_binding *map;
   size_t nmap;
   bool spv_written;
};

/* Records one compile outcome; returns the kernel for further comparison. */
static mglk_kernel *
record(const char *name, const char *input_kind, mglk_status expected,
       mglk_status status, mglk_kernel *kernel, const mglk_diagnostic *diag,
       const struct spirv_input *in)
{
   fprintf(json, "%s\n  {\"name\": ", cases++ ? "," : "");
   json_str(name);
   fprintf(json, ", \"input\": ");
   json_str(input_kind);
   fprintf(json, ", \"expected\": \"%s\", \"status\": \"%s\", \"message\": ",
           mglk_status_name(expected), mglk_status_name(status));
   json_str(diag->message);
   if (in) {
      fprintf(json, ",\n   \"entry\": ");
      json_str(in->entry);
      fprintf(json, ", \"spv\": %s, \"bindings\": [", in->spv_written ? "true" : "false");
      for (size_t i = 0; in->map && i < in->nmap; i++)
         fprintf(json, "%s{\"set\": %u, \"binding\": %u, \"bti\": %u, \"kind\": \"%s\"}",
                 i ? ", " : "", in->map[i].set, in->map[i].binding, in->map[i].bti,
                 in->map[i].kind == MGLK_RESOURCE_SSBO ? "ssbo" : "ubo");
      fprintf(json, "]");
   }
   expect(status == expected, name, "status mismatch");
   if (strcmp(input_kind, "device") != 0)
      expect((status == MGLK_OK) == (kernel != NULL), name, "kernel/status ownership mismatch");
   if (!kernel) {
      fprintf(json, "}");
      return NULL;
   }

   mglk_kernel_info info;
   mglk_kernel_get_info(kernel, &info);
   size_t size = 0, nres = 0, nrel = 0;
   const uint8_t *program = mglk_kernel_program(kernel, &size);
   const mglk_resource_use *res = mglk_kernel_resources(kernel, &nres);
   const mglk_reloc *rel = mglk_kernel_relocs(kernel, &nrel);
   const char *dis = mglk_kernel_disassembly(kernel);
   const char *log = mglk_kernel_log(kernel);

   expect(program && size > 0 && size == info.program_size, name, "empty program");
   expect(info.simd_mask != 0, name, "no SIMD variant");
   expect(info.decoded_instructions > 0, name, "gen_decode found no instructions");
   expect(info.validation_errors == 0, name, "gen_validate errors");
   expect(info.verx10 == 90 && info.pci_device_id == MGLK_PCI_GLK_2X6, name, "not GLK");
   expect(strstr(dis, "send") != NULL, name, "disassembly has no send");
   /* Gen9 brw_cs_fill_push_const_info: per-thread push iff subgroup id. */
   expect((info.push_per_thread_dwords > 0) ==
          (info.subgroup_id_push_dword != MGLK_NO_PUSH_DWORD), name, "subgroup id push");
   char subgroup[16] = "null";
   if (info.subgroup_id_push_dword != MGLK_NO_PUSH_DWORD)
      snprintf(subgroup, sizeof(subgroup), "%u", info.subgroup_id_push_dword);
   expect(write_file(name, ".isa.bin", program, size), name, "write isa");
   expect(write_file(name, ".disasm.txt", dis, strlen(dis)), name, "write disasm");
   expect(write_file(name, ".log.txt", log, strlen(log)), name, "write log");

   fprintf(json, ",\n   \"info\": {\"program_size\": %u, \"const_data_offset\": %u, "
           "\"const_data_size\": %u, \"simd_mask\": %u, \"prog_offset\": [%u, %u, %u], "
           "\"prog_spilled_mask\": %u, \"local_size\": [%u, %u, %u], "
           "\"dispatch_simd_size\": %u, \"dispatch_threads\": %u, "
           "\"dispatch_group_size\": %u, \"dispatch_right_mask\": %u, \"grf_used\": %u, "
           "\"dispatch_grf_start_reg\": %u, \"total_scratch\": %u, \"total_shared\": %u, "
           "\"push_cross_thread_dwords\": %u, \"push_per_thread_dwords\": %u, "
           "\"subgroup_id_push_dword\": %s, "
           "\"uses_barrier\": %u, \"uses_sampler\": %u, \"uses_dynamic_bti\": %u, "
           "\"uses_ubo_pull\": %u, "
           "\"decoded_instructions\": %u, \"compacted_instructions\": %u, "
           "\"validation_errors\": %u},\n   \"resources\": [",
           info.program_size, info.const_data_offset, info.const_data_size,
           info.simd_mask, info.prog_offset[0], info.prog_offset[1], info.prog_offset[2],
           info.prog_spilled_mask, info.local_size[0], info.local_size[1],
           info.local_size[2], info.dispatch_simd_size, info.dispatch_threads,
           info.dispatch_group_size, info.dispatch_right_mask, info.grf_used,
           info.dispatch_grf_start_reg, info.total_scratch, info.total_shared,
           info.push_cross_thread_dwords, info.push_per_thread_dwords, subgroup,
           info.uses_barrier, info.uses_sampler, info.uses_dynamic_bti,
           info.uses_ubo_pull,
           info.decoded_instructions, info.compacted_instructions,
           info.validation_errors);
   for (size_t i = 0; i < nres; i++)
      fprintf(json, "%s{\"bti\": %u, \"kind\": \"%s\", \"access\": %u}", i ? ", " : "",
              res[i].bti, res[i].kind == MGLK_RESOURCE_SSBO ? "ssbo" : "ubo",
              res[i].access);
   fprintf(json, "],\n   \"relocs\": [");
   for (size_t i = 0; i < nrel; i++)
      fprintf(json, "%s{\"id\": %u, \"offset\": %u, \"delta\": %u, \"type\": %u}",
              i ? ", " : "", rel[i].id, rel[i].offset, rel[i].delta, rel[i].type);
   fprintf(json, "],\n   \"isa\": ");
   json_str(name);
   fprintf(json, "}");
   return kernel;
}

static bool
same_program(const mglk_kernel *a, const mglk_kernel *b)
{
   size_t sa = 0, sb = 0;
   const uint8_t *pa = mglk_kernel_program(a, &sa);
   const uint8_t *pb = mglk_kernel_program(b, &sb);
   return pa && pb && sa == sb && memcmp(pa, pb, sa) == 0;
}

static const mglk_resource_use *
find_resource(const mglk_kernel *kernel, uint32_t bti)
{
   size_t n = 0;
   const mglk_resource_use *res = mglk_kernel_resources(kernel, &n);
   for (size_t i = 0; i < n; i++) {
      if (res[i].bti == bti)
         return &res[i];
   }
   return NULL;
}

/* ---- typed NIR kernels built with the upstream nir_builder ------------- */

static nir_shader *
nir_ssbo_write(const mglk_compiler *c, const nir_shader_compiler_options *opts,
               unsigned wg, uint32_t mul, uint32_t add, uint32_t bti)
{
   nir_builder b = nir_builder_init_simple_shader(
      MESA_SHADER_COMPUTE, opts ? opts : mglk_compiler_nir_options(c),
      "mglk_ssbo_write_m%u_a%u_wg%u", mul, add, wg);
   b.shader->info.workgroup_size[0] = (uint16_t)wg;
   b.shader->info.workgroup_size[1] = 1;
   b.shader->info.workgroup_size[2] = 1;
   b.shader->info.num_ssbos = 1;
   nir_def *x = nir_channel(&b, nir_load_global_invocation_id(&b, 32), 0);
   nir_def *value = nir_iadd_imm(&b, nir_imul_imm(&b, x, mul), add);
   nir_store_ssbo(&b, value, nir_imm_int(&b, (int)bti), nir_ishl_imm(&b, x, 2),
                  .write_mask = 0x1, .align_mul = 4);
   return b.shader;
}

static nir_shader *
nir_ssbo_copy_scale(const mglk_compiler *c)
{
   nir_builder b = nir_builder_init_simple_shader(
      MESA_SHADER_COMPUTE, mglk_compiler_nir_options(c), "mglk_ssbo_copy_scale");
   b.shader->info.workgroup_size[0] = 32;
   b.shader->info.workgroup_size[1] = 1;
   b.shader->info.workgroup_size[2] = 1;
   b.shader->info.num_ssbos = 2;
   nir_def *x = nir_channel(&b, nir_load_global_invocation_id(&b, 32), 0);
   nir_def *offset = nir_ishl_imm(&b, x, 2);
   nir_def *in = nir_load_ssbo(&b, 1, 32, nir_imm_int(&b, 1), offset, .align_mul = 4);
   nir_store_ssbo(&b, nir_imul_imm(&b, in, 9), nir_imm_int(&b, 0), offset,
                  .write_mask = 0x1, .align_mul = 4);
   return b.shader;
}

static nir_shader *
nir_shared_barrier(const mglk_compiler *c, unsigned wg)
{
   nir_builder b = nir_builder_init_simple_shader(
      MESA_SHADER_COMPUTE, mglk_compiler_nir_options(c), "mglk_shared_swap_wg%u", wg);
   b.shader->info.workgroup_size[0] = (uint16_t)wg;
   b.shader->info.workgroup_size[1] = 1;
   b.shader->info.workgroup_size[2] = 1;
   b.shader->info.shared_size = wg * 4;
   b.shader->info.num_ssbos = 1;
   nir_def *lid = nir_channel(&b, nir_load_local_invocation_id(&b), 0);
   nir_def *gid = nir_channel(&b, nir_load_global_invocation_id(&b, 32), 0);
   nir_store_shared(&b, gid, nir_ishl_imm(&b, lid, 2), .base = 0, .align_mul = 4,
                    .write_mask = 0x1);
   nir_barrier(&b, .execution_scope = SCOPE_WORKGROUP, .memory_scope = SCOPE_WORKGROUP,
               .memory_semantics = NIR_MEMORY_ACQ_REL, .memory_modes = nir_var_mem_shared);
   nir_def *peer = nir_load_shared(&b, 1, 32,
                                   nir_ishl_imm(&b, nir_ixor(&b, lid, nir_imm_int(&b, 1)), 2),
                                   .base = 0, .align_mul = 4);
   nir_store_ssbo(&b, peer, nir_imm_int(&b, 0), nir_ishl_imm(&b, gid, 2),
                  .write_mask = 0x1, .align_mul = 4);
   return b.shader;
}

/* ---- minimal SPIR-V 1.3 module emitter (typed input, not GPU code) ----- */

#define SPV_CAP 512u

struct spv {
   uint32_t w[SPV_CAP];
   size_t n;
};

static void
spv_op(struct spv *s, uint32_t opcode, unsigned count, ...)
{
   va_list ap;
   va_start(ap, count);
   s->w[s->n++] = ((count + 1u) << 16) | opcode;
   for (unsigned i = 0; i < count; i++)
      s->w[s->n++] = va_arg(ap, uint32_t);
   va_end(ap);
}

enum spv_case { SPV_GOOD, SPV_VERTEX_MODEL, SPV_UNDEFINED_ID, SPV_UBO_MUL };

/* layout(local_size_x = wg) in;
 * layout(set = S, binding = B, std430) buffer Out { uint data[]; };
 * void main() { uint i = gl_GlobalInvocationID.x; data[i] = i * mul + add; }
 * SPV_UBO_MUL instead reads the multiplier from
 * layout(set = S, binding = B + 1, std140) uniform Params { uint mul; }; */
static size_t
spirv_ssbo_write(struct spv *s, unsigned wg, uint32_t mul, uint32_t add,
                 uint32_t set, uint32_t binding, enum spv_case variant)
{
   const bool ubo = variant == SPV_UBO_MUL;
   s->n = 0;
   s->w[s->n++] = 0x07230203u;
   s->w[s->n++] = 0x00010300u;
   s->w[s->n++] = 0;  /* generator: none registered */
   s->w[s->n++] = ubo ? 29 : 23; /* id bound */
   s->w[s->n++] = 0;
   spv_op(s, 17, 1, 1u);                                   /* OpCapability Shader */
   spv_op(s, 14, 2, 0u, 1u);                               /* OpMemoryModel Logical GLSL450 */
   const uint32_t main_name = 'm' | 'a' << 8 | 'i' << 16 | (uint32_t)'n' << 24;
   spv_op(s, 15, 5, variant == SPV_VERTEX_MODEL ? 0u : 5u, 16u, main_name, 0u, 6u);
   spv_op(s, 16, 5, 16u, 17u, wg, 1u, 1u);                 /* LocalSize */
   spv_op(s, 71, 3, 6u, 11u, 28u);                         /* gid BuiltIn GlobalInvocationId */
   spv_op(s, 71, 3, 7u, 6u, 4u);                           /* rta ArrayStride 4 */
   spv_op(s, 72, 4, 8u, 0u, 35u, 0u);                      /* Out[0] Offset 0 */
   spv_op(s, 71, 2, 8u, 2u);                               /* Out Block */
   spv_op(s, 71, 3, 10u, 34u, set);                        /* DescriptorSet */
   spv_op(s, 71, 3, 10u, 33u, binding);                    /* Binding */
   if (ubo) {
      spv_op(s, 71, 2, 23u, 2u);                           /* Params Block */
      spv_op(s, 72, 4, 23u, 0u, 35u, 0u);                  /* Params[0] Offset 0 */
      spv_op(s, 71, 3, 25u, 34u, set);                     /* DescriptorSet */
      spv_op(s, 71, 3, 25u, 33u, binding + 1);             /* Binding */
   }
   spv_op(s, 19, 1, 1u);                                   /* %1 void */
   spv_op(s, 33, 2, 2u, 1u);                               /* %2 fn void() */
   spv_op(s, 21, 3, 3u, 32u, 0u);                          /* %3 uint */
   spv_op(s, 23, 3, 4u, 3u, 3u);                           /* %4 uvec3 */
   spv_op(s, 32, 3, 5u, 1u, 4u);                           /* %5 Input uvec3* */
   spv_op(s, 59, 3, 5u, 6u, 1u);                           /* %6 gid */
   spv_op(s, 29, 2, 7u, 3u);                               /* %7 uint[] */
   spv_op(s, 30, 2, 8u, 7u);                               /* %8 struct Out */
   spv_op(s, 32, 3, 9u, 12u, 8u);                          /* %9 StorageBuffer Out* */
   spv_op(s, 59, 3, 9u, 10u, 12u);                         /* %10 o */
   spv_op(s, 32, 3, 11u, 12u, 3u);                         /* %11 StorageBuffer uint* */
   spv_op(s, 21, 3, 12u, 32u, 1u);                         /* %12 int */
   spv_op(s, 43, 3, 12u, 13u, 0u);                         /* %13 int 0 */
   spv_op(s, 43, 3, 3u, 14u, mul);                         /* %14 */
   spv_op(s, 43, 3, 3u, 15u, add);                         /* %15 */
   if (ubo) {
      spv_op(s, 30, 2, 23u, 3u);                           /* %23 struct Params */
      spv_op(s, 32, 3, 24u, 2u, 23u);                      /* %24 Uniform Params* */
      spv_op(s, 59, 3, 24u, 25u, 2u);                      /* %25 params */
      spv_op(s, 32, 3, 26u, 2u, 3u);                       /* %26 Uniform uint* */
   }
   spv_op(s, 54, 4, 1u, 16u, 0u, 2u);                      /* %16 main */
   spv_op(s, 248, 1, 17u);                                 /* %17 label */
   spv_op(s, 61, 3, 4u, 18u, 6u);                          /* %18 load gid */
   spv_op(s, 81, 4, 3u, 19u, 18u, 0u);                     /* %19 gid.x */
   if (ubo) {
      spv_op(s, 65, 4, 26u, 27u, 25u, 13u);                /* %27 &params.mul */
      spv_op(s, 61, 3, 3u, 28u, 27u);                      /* %28 load mul */
   }
   spv_op(s, 132, 4, 3u, 20u, 19u, ubo ? 28u : 14u);       /* %20 imul */
   spv_op(s, 128, 4, 3u, 21u, 20u,
          variant == SPV_UNDEFINED_ID ? 99u : 15u);        /* %21 iadd */
   spv_op(s, 65, 5, 11u, 22u, 10u, 13u, 19u);              /* %22 access chain */
   spv_op(s, 62, 2, 22u, 21u);                             /* store */
   spv_op(s, 253, 0);                                      /* return */
   spv_op(s, 56, 0);                                       /* function end */
   return s->n;
}

/* Rewrites the storage class of the OpVariable defining result_id. */
static void
spv_patch_storage(uint32_t *w, size_t n, uint32_t result_id, uint32_t storage)
{
   for (size_t i = 5; i < n && (w[i] >> 16); i += w[i] >> 16) {
      if ((w[i] & 0xffffu) == 59u && w[i + 2] == result_id)
         w[i + 3] = storage;
   }
}

static mglk_kernel *
run_nir(mglk_compiler *c, const char *name, mglk_status expected, nir_shader *nir)
{
   mglk_diagnostic diag;
   mglk_kernel *kernel = NULL;
   mglk_status status = mglk_compile_compute_nir(c, nir, &kernel, &diag);
   return record(name, "nir", expected, status, kernel, &diag, NULL);
}

static mglk_kernel *
run_spirv(mglk_compiler *c, const char *name, mglk_status expected,
          const uint32_t *words, size_t count, const char *entry,
          const mglk_binding *bindings, size_t nbindings)
{
   /* Only modules that really occupy count words can be written out. */
   struct spirv_input in = { .entry = entry, .map = bindings, .nmap = nbindings };
   if (words && count <= SPV_CAP) {
      in.spv_written = write_file(name, ".spv", words, count * sizeof(uint32_t));
      expect(in.spv_written, name, "write spv");
   }
   mglk_diagnostic diag;
   mglk_kernel *kernel = NULL;
   mglk_status status = mglk_compile_compute_spirv(c, words, count, entry, bindings,
                                                   nbindings, &kernel, &diag);
   return record(name, "spirv", expected, status, kernel, &diag, &in);
}

static void
device_case(uint32_t pci, mglk_status expected)
{
   char name[64];
   snprintf(name, sizeof(name), "device_8086_%04x", pci);
   mglk_diagnostic diag;
   mglk_compiler *c = NULL;
   mglk_status status = mglk_compiler_create(pci, &c, &diag);
   record(name, "device", expected, status, NULL, &diag, NULL);
   expect((status == MGLK_OK) == (c != NULL), name, "compiler/status mismatch");
   if (c) {
      mglk_device_desc d;
      expect(mglk_compiler_describe(c, &d) == MGLK_OK, name, "describe");
      expect(d.ver == 9 && d.verx10 == 90 && d.is_gen9_lp, name, "not Gen9 LP");
      expect(d.runtime_fused_topology_known == 0, name, "fusing must be unknown");
      expect(d.debug_env_present == 0, name, "INTEL_DEBUG/INTEL_SIMD_DEBUG must be unset");
      fprintf(json, ",\n  {\"name\": \"%s_desc\", \"input\": \"device-desc\", "
              "\"expected\": \"MGLK_OK\", \"status\": \"MGLK_OK\", \"message\": \"\", "
              "\"device\": {\"pci_device_id\": %u, \"ver\": %u, \"verx10\": %u, \"gt\": %u, "
              "\"num_slices\": %u, \"num_subslices_total\": %u, \"max_eus_per_subslice\": %u, "
              "\"num_thread_per_eu\": %u, \"max_cs_threads\": %u, "
              "\"max_cs_workgroup_threads\": %u, \"max_workgroup_invocations\": %u, "
              "\"timestamp_frequency\": %llu, \"topology_source\": "
              "\"static-pci-table-runtime-fusing-unknown\", "
              "\"runtime_fused_topology_known\": %u, \"platform\": ",
              name, d.pci_device_id, d.ver, d.verx10, d.gt, d.num_slices,
              d.num_subslices_total, d.max_eus_per_subslice, d.num_thread_per_eu,
              d.max_cs_threads, d.max_cs_workgroup_threads, d.max_workgroup_invocations,
              (unsigned long long)d.timestamp_frequency, d.runtime_fused_topology_known);
      json_str(d.platform_name);
      fprintf(json, ", \"marketing_name\": ");
      json_str(d.marketing_name);
      fprintf(json, ", \"mesa_commit\": ");
      json_str(d.mesa_commit);
      fprintf(json, "}}");
      cases++;
      mglk_compiler_destroy(c);
   }
}

int
main(int argc, char **argv)
{
   if (argc != 2) {
      fprintf(stderr, "usage: %s <output-dir>\n", argv[0]);
      return 2;
   }
   out_dir = argv[1];
   char path[1024];
   snprintf(path, sizeof(path), "%s/cases", out_dir);
   if (mkdir(path, 0755) != 0 && errno != EEXIST) {
      perror(path);
      return 2;
   }
   snprintf(path, sizeof(path), "%s/cases.json", out_dir);
   json = fopen(path, "w");
   if (!json) {
      perror(path);
      return 2;
   }
   fprintf(json, "{\"mesa_commit\": \"%s\", \"abi_version\": %u, \"cases\": [",
           MGLK_MESA_COMMIT, MGLK_ABI_VERSION);

   device_case(MGLK_PCI_GLK_2X6, MGLK_OK);
   device_case(MGLK_PCI_GLK_3X6, MGLK_OK);
   device_case(0x5a85, MGLK_E_UNSUPPORTED_DEVICE); /* Apollo Lake: Gen9 LP, not GLK */
   device_case(0x1912, MGLK_E_UNSUPPORTED_DEVICE); /* Skylake GT2 */
   device_case(0x9a49, MGLK_E_UNSUPPORTED_DEVICE); /* Tiger Lake (Xe) */
   device_case(0x0000, MGLK_E_UNSUPPORTED_DEVICE);

   mglk_diagnostic diag;
   mglk_compiler *c = NULL;
   if (mglk_compiler_create(MGLK_PCI_GLK_2X6, &c, &diag) != MGLK_OK) {
      fprintf(stderr, "cannot create GLK compiler: %s\n", diag.message);
      return 1;
   }

   /* Typed NIR inputs. */
   mglk_kernel *w37 = run_nir(c, "nir_ssbo_write_m3_a7_wg64", MGLK_OK,
                              nir_ssbo_write(c, NULL, 64, 3, 7, 0));
   mglk_kernel *w37b = run_nir(c, "nir_ssbo_write_m3_a7_wg64_repeat", MGLK_OK,
                               nir_ssbo_write(c, NULL, 64, 3, 7, 0));
   mglk_kernel *w511 = run_nir(c, "nir_ssbo_write_m5_a11_wg64", MGLK_OK,
                               nir_ssbo_write(c, NULL, 64, 5, 11, 0));
   mglk_kernel *w37s = run_nir(c, "nir_ssbo_write_m3_a7_wg8", MGLK_OK,
                               nir_ssbo_write(c, NULL, 8, 3, 7, 0));
   mglk_kernel *w37t = run_nir(c, "nir_ssbo_write_m3_a7_wg64_bti5", MGLK_OK,
                               nir_ssbo_write(c, NULL, 64, 3, 7, 5));
   mglk_kernel *copy = run_nir(c, "nir_ssbo_copy_scale", MGLK_OK, nir_ssbo_copy_scale(c));
   mglk_kernel *shared = run_nir(c, "nir_shared_barrier_wg64", MGLK_OK,
                                 nir_shared_barrier(c, 64));

   if (w37 && w37b && w511 && w37s && w37t) {
      expect(same_program(w37, w37b), "determinism", "identical input differs");
      expect(!same_program(w37, w511), "constants", "different constants same ISA");
      expect(!same_program(w37, w37t), "bti", "different BTI same ISA");
      mglk_kernel_info a, b;
      mglk_kernel_get_info(w37, &a);
      mglk_kernel_get_info(w37s, &b);
      expect(a.local_size[0] == 64 && b.local_size[0] == 8, "local_size", "metadata");
      const mglk_resource_use *r = find_resource(w37t, 5);
      expect(r && r->access == MGLK_ACCESS_WRITE, "bti5", "resource table");
   }
   if (copy) {
      const mglk_resource_use *r0 = find_resource(copy, 0), *r1 = find_resource(copy, 1);
      expect(r0 && r0->access == MGLK_ACCESS_WRITE, "copy", "bti0 write");
      expect(r1 && r1->access == MGLK_ACCESS_READ, "copy", "bti1 read");
   }
   if (shared) {
      mglk_kernel_info info;
      mglk_kernel_get_info(shared, &info);
      expect(info.uses_barrier && info.total_shared >= 256, "shared", "barrier/shared");
   }

   /* Unsupported / invalid NIR. */
   static nir_shader_compiler_options foreign_options;
   run_nir(c, "nir_foreign_options", MGLK_E_INVALID_ARGUMENT,
           nir_ssbo_write(c, &foreign_options, 64, 3, 7, 0));
   nir_builder vs = nir_builder_init_simple_shader(MESA_SHADER_VERTEX,
                                                   mglk_compiler_nir_options(c), "vs");
   run_nir(c, "nir_vertex_stage", MGLK_E_UNSUPPORTED_INPUT, vs.shader);
   nir_shader *zero = nir_ssbo_write(c, NULL, 64, 3, 7, 0);
   zero->info.workgroup_size[1] = 0;
   run_nir(c, "nir_workgroup_dim_zero", MGLK_E_LIMIT_EXCEEDED, zero);
   nir_shader *huge = nir_ssbo_write(c, NULL, 1024, 3, 7, 0);
   huge->info.workgroup_size[1] = 2;
   run_nir(c, "nir_workgroup_2048", MGLK_E_LIMIT_EXCEEDED, huge);
   run_nir(c, "nir_bti_250", MGLK_E_UNSUPPORTED_INPUT, nir_ssbo_write(c, NULL, 64, 3, 7, 250));
   run_nir(c, "nir_null", MGLK_E_INVALID_ARGUMENT, NULL);

   /* SPIR-V inputs through upstream spirv_to_nir. */
   const mglk_binding map0[] = { { .set = 0, .binding = 0, .bti = 3, .kind = MGLK_RESOURCE_SSBO } };
   struct spv s;
   size_t n = spirv_ssbo_write(&s, 64, 3, 7, 0, 0, SPV_GOOD);
   mglk_kernel *s37 = run_spirv(c, "spirv_ssbo_write_m3_a7_wg64", MGLK_OK, s.w, n, "main", map0, 1);
   n = spirv_ssbo_write(&s, 64, 5, 11, 0, 0, SPV_GOOD);
   mglk_kernel *s511 = run_spirv(c, "spirv_ssbo_write_m5_a11_wg64", MGLK_OK, s.w, n, "main", map0, 1);
   n = spirv_ssbo_write(&s, 16, 3, 7, 0, 0, SPV_GOOD);
   mglk_kernel *s16 = run_spirv(c, "spirv_ssbo_write_m3_a7_wg16", MGLK_OK, s.w, n, "main", map0, 1);
   const mglk_binding ubo_mul_map[] = {
      { .set = 0, .binding = 0, .bti = 3, .kind = MGLK_RESOURCE_SSBO },
      { .set = 0, .binding = 1, .bti = 4, .kind = MGLK_RESOURCE_UBO },
   };
   n = spirv_ssbo_write(&s, 64, 0, 7, 0, 0, SPV_UBO_MUL);
   mglk_kernel *subo = run_spirv(c, "spirv_ubo_mul_a7_wg64", MGLK_OK, s.w, n, "main",
                                 ubo_mul_map, 2);
   if (s37 && s511 && s16) {
      expect(!same_program(s37, s511), "spirv_constants", "different constants same ISA");
      const mglk_resource_use *r = find_resource(s37, 3);
      expect(r && r->kind == MGLK_RESOURCE_SSBO && (r->access & MGLK_ACCESS_WRITE),
             "spirv_binding", "(0,0) must map to BTI 3");
      mglk_kernel_info info;
      mglk_kernel_get_info(s16, &info);
      expect(info.local_size[0] == 16, "spirv_wg16", "local size");
   }
   if (subo) {
      const mglk_resource_use *w = find_resource(subo, 3), *u = find_resource(subo, 4);
      expect(w && w->kind == MGLK_RESOURCE_SSBO && w->access == MGLK_ACCESS_WRITE,
             "spirv_ubo", "(0,0) must be an SSBO write at BTI 3");
      expect(u && u->kind == MGLK_RESOURCE_UBO && u->access == MGLK_ACCESS_READ,
             "spirv_ubo", "(0,1) must be a UBO read at BTI 4");
   }

   /* Malformed / unsupported SPIR-V. */
   n = spirv_ssbo_write(&s, 64, 3, 7, 0, 0, SPV_GOOD);
   run_spirv(c, "spirv_null_words", MGLK_E_INVALID_ARGUMENT, NULL, 0, "main", map0, 1);
   run_spirv(c, "spirv_header_only_3_words", MGLK_E_MALFORMED_INPUT, s.w, 3, "main", map0, 1);
   uint32_t bad[512];
   memcpy(bad, s.w, n * sizeof(uint32_t));
   bad[0] = 0x03022307u;
   run_spirv(c, "spirv_byteswapped_magic", MGLK_E_MALFORMED_INPUT, bad, n, "main", map0, 1);
   /* Cut inside OpStore: its header still claims 3 words. */
   run_spirv(c, "spirv_truncated", MGLK_E_MALFORMED_INPUT, s.w, n - 3, "main", map0, 1);
   memcpy(bad, s.w, n * sizeof(uint32_t));
   bad[5] = 0x00001111u; /* zero word count */
   run_spirv(c, "spirv_zero_word_count", MGLK_E_MALFORMED_INPUT, bad, n, "main", map0, 1);
   run_spirv(c, "spirv_missing_entry", MGLK_E_UNSUPPORTED_INPUT, s.w, n, "kernel_main", map0, 1);
   size_t nv = spirv_ssbo_write(&s, 64, 3, 7, 0, 0, SPV_VERTEX_MODEL);
   run_spirv(c, "spirv_vertex_model", MGLK_E_UNSUPPORTED_INPUT, s.w, nv, "main", map0, 1);
   size_t nu = spirv_ssbo_write(&s, 64, 3, 7, 0, 0, SPV_UNDEFINED_ID);
   run_spirv(c, "spirv_out_of_bounds_id", MGLK_E_FRONTEND_REJECTED, s.w, nu, "main", map0, 1);
   size_t nb = spirv_ssbo_write(&s, 64, 3, 7, 0, 3, SPV_GOOD);
   run_spirv(c, "spirv_unmapped_binding", MGLK_E_UNSUPPORTED_INPUT, s.w, nb, "main", map0, 1);
   n = spirv_ssbo_write(&s, 64, 3, 7, 0, 0, SPV_GOOD);
   const mglk_binding ubo_map[] = { { .set = 0, .binding = 0, .bti = 3, .kind = MGLK_RESOURCE_UBO } };
   run_spirv(c, "spirv_kind_mismatch", MGLK_E_UNSUPPORTED_INPUT, s.w, n, "main", ubo_map, 1);
   const mglk_binding dup_map[] = {
      { .set = 0, .binding = 0, .bti = 3, .kind = MGLK_RESOURCE_SSBO },
      { .set = 0, .binding = 1, .bti = 3, .kind = MGLK_RESOURCE_SSBO },
   };
   run_spirv(c, "spirv_duplicate_bti", MGLK_E_INVALID_ARGUMENT, s.w, n, "main", dup_map, 2);
   const mglk_binding high_map[] = { { .set = 0, .binding = 0, .bti = 240, .kind = MGLK_RESOURCE_SSBO } };
   run_spirv(c, "spirv_bti_240", MGLK_E_LIMIT_EXCEEDED, s.w, n, "main", high_map, 1);
   run_spirv(c, "spirv_word_limit", MGLK_E_LIMIT_EXCEEDED, s.w, (size_t)MGLK_MAX_SPIRV_WORDS + 1,
             "main", map0, 1);
   run_spirv(c, "spirv_null_bindings", MGLK_E_INVALID_ARGUMENT, s.w, n, "main", NULL, 1);
   memcpy(bad, s.w, n * sizeof(uint32_t));
   spv_patch_storage(bad, n, 10u, 9u); /* Out buffer as PushConstant */
   run_spirv(c, "spirv_push_constant_storage", MGLK_E_UNSUPPORTED_INPUT, bad, n, "main", map0, 1);
   memcpy(bad, s.w, n * sizeof(uint32_t));
   spv_patch_storage(bad, n, 10u, 0u); /* UniformConstant: images/samplers */
   run_spirv(c, "spirv_uniform_constant_storage", MGLK_E_UNSUPPORTED_INPUT, bad, n, "main",
             map0, 1);

   mglk_kernel *all[] = { w37, w37b, w511, w37s, w37t, copy, shared, s37, s511, s16, subo };
   for (size_t i = 0; i < sizeof(all) / sizeof(all[0]); i++)
      mglk_kernel_destroy(all[i]);
   mglk_compiler_destroy(c);

   fprintf(json, "\n], \"case_count\": %d, \"failures\": %d}\n", cases, failures);
   fclose(json);
   fprintf(stderr, "%d case(s), %d failure(s)\n", cases, failures);
   return failures ? 1 : 0;
}
