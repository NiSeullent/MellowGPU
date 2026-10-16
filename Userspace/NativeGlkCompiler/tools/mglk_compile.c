/*
 * mglk-compile: offline GLK (Gen9 LP) SPIR-V compute compiler built on the
 * NativeGlkCompiler adapter (pinned upstream Mesa spirv_to_nir + brw).
 *
 * SPDX-License-Identifier: MIT
 *
 * usage: mglk-compile [--device 0x3185] [--entry main]
 *                     [--binding SET:BINDING:BTI:ssbo|ubo]... --out PREFIX input.spv
 *
 * Once the arguments parse, PREFIX.json is always written; a successful
 * compile also writes PREFIX.isa.bin, PREFIX.disasm.txt and PREFIX.log.txt.
 * Exit status: 0 compiled, 1 input rejected or compile failed (diagnostic on
 * stderr and in PREFIX.json), 2 usage or output I/O error.
 *
 * Host-only: the ISA is never executed, uploaded or submitted anywhere.
 */
#include "mellow_glk_compiler.h"

#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static FILE *json;

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

static int
usage(const char *argv0, const char *why)
{
   fprintf(stderr, "%s: %s\nusage: %s [--device 0x3185] [--entry main] "
           "[--binding SET:BINDING:BTI:ssbo|ubo]... --out PREFIX input.spv\n",
           argv0, why, argv0);
   return 2;
}

static bool
write_output(const char *prefix, const char *suffix, const void *data, size_t size)
{
   char path[4096];
   if ((size_t)snprintf(path, sizeof(path), "%s%s", prefix, suffix) >= sizeof(path))
      return false;
   FILE *fp = fopen(path, "wb");
   if (!fp)
      return false;
   const bool ok = fwrite(data, 1, size, fp) == size;
   return fclose(fp) == 0 && ok;
}

/* Reads the whole module; size checks mirror the adapter's own bounds. */
static mglk_status
read_spirv(const char *path, uint32_t **words, size_t *count, char *why, size_t why_size)
{
   const size_t cap = (size_t)MGLK_MAX_SPIRV_WORDS * 4u;
   *words = NULL;
   *count = 0;
   FILE *fp = fopen(path, "rb");
   if (!fp) {
      snprintf(why, why_size, "cannot open %s", path);
      return MGLK_E_INVALID_ARGUMENT;
   }
   uint8_t *bytes = malloc(cap + 1);
   size_t size = bytes ? fread(bytes, 1, cap + 1, fp) : 0;
   const bool read_error = !bytes || ferror(fp);
   fclose(fp);
   mglk_status status = MGLK_OK;
   if (read_error) {
      snprintf(why, why_size, "cannot read %s", path);
      status = MGLK_E_INVALID_ARGUMENT;
   } else if (size > cap) {
      snprintf(why, why_size, "%s exceeds %zu bytes (%u SPIR-V words)", path, cap,
               MGLK_MAX_SPIRV_WORDS);
      status = MGLK_E_LIMIT_EXCEEDED;
   } else if (size == 0 || size % 4u) {
      snprintf(why, why_size, "%s is %zu bytes; a SPIR-V module is a nonzero "
               "whole number of 32-bit words", path, size);
      status = MGLK_E_MALFORMED_INPUT;
   }
   if (status != MGLK_OK) {
      free(bytes);
      return status;
   }
   *words = (uint32_t *)bytes;
   *count = size / 4u;
   return MGLK_OK;
}

static void
print_device(const mglk_device_desc *d)
{
   fprintf(json, ",\n \"device\": {\"pci_device_id\": %u, \"ver\": %u, \"verx10\": %u, "
           "\"gt\": %u, \"num_slices\": %u, \"num_subslices_total\": %u, "
           "\"max_eus_per_subslice\": %u, \"num_thread_per_eu\": %u, \"max_cs_threads\": %u, "
           "\"max_cs_workgroup_threads\": %u, \"max_workgroup_invocations\": %u, "
           "\"runtime_fused_topology_known\": %u, \"debug_env_present\": %u, \"platform\": ",
           d->pci_device_id, d->ver, d->verx10, d->gt, d->num_slices,
           d->num_subslices_total, d->max_eus_per_subslice, d->num_thread_per_eu,
           d->max_cs_threads, d->max_cs_workgroup_threads, d->max_workgroup_invocations,
           d->runtime_fused_topology_known, d->debug_env_present);
   json_str(d->platform_name);
   fprintf(json, ", \"marketing_name\": ");
   json_str(d->marketing_name);
   fprintf(json, "}");
}

static void
print_kernel(const mglk_kernel *kernel, const char *prefix)
{
   mglk_kernel_info info;
   mglk_kernel_get_info(kernel, &info);
   char subgroup[16] = "null";
   if (info.subgroup_id_push_dword != MGLK_NO_PUSH_DWORD)
      snprintf(subgroup, sizeof(subgroup), "%u", info.subgroup_id_push_dword);
   fprintf(json, ",\n \"info\": {\"program_size\": %u, \"const_data_offset\": %u, "
           "\"const_data_size\": %u, \"simd_mask\": %u, \"prog_offset\": [%u, %u, %u], "
           "\"prog_spilled_mask\": %u, \"local_size\": [%u, %u, %u], "
           "\"dispatch_simd_size\": %u, \"dispatch_threads\": %u, "
           "\"dispatch_group_size\": %u, \"dispatch_right_mask\": %u, \"grf_used\": %u, "
           "\"dispatch_grf_start_reg\": %u, \"total_scratch\": %u, \"total_shared\": %u, "
           "\"push_cross_thread_dwords\": %u, \"push_per_thread_dwords\": %u, "
           "\"subgroup_id_push_dword\": %s, \"uses_barrier\": %u, \"uses_sampler\": %u, "
           "\"uses_dynamic_bti\": %u, \"uses_ubo_pull\": %u, \"decoded_instructions\": %u, "
           "\"compacted_instructions\": %u, \"validation_errors\": %u}",
           info.program_size, info.const_data_offset, info.const_data_size,
           info.simd_mask, info.prog_offset[0], info.prog_offset[1], info.prog_offset[2],
           info.prog_spilled_mask, info.local_size[0], info.local_size[1],
           info.local_size[2], info.dispatch_simd_size, info.dispatch_threads,
           info.dispatch_group_size, info.dispatch_right_mask, info.grf_used,
           info.dispatch_grf_start_reg, info.total_scratch, info.total_shared,
           info.push_cross_thread_dwords, info.push_per_thread_dwords, subgroup,
           info.uses_barrier, info.uses_sampler, info.uses_dynamic_bti, info.uses_ubo_pull,
           info.decoded_instructions, info.compacted_instructions, info.validation_errors);
   size_t n = 0;
   const mglk_resource_use *res = mglk_kernel_resources(kernel, &n);
   fprintf(json, ",\n \"resources\": [");
   for (size_t i = 0; i < n; i++)
      fprintf(json, "%s{\"bti\": %u, \"kind\": \"%s\", \"access\": %u}", i ? ", " : "",
              res[i].bti, res[i].kind == MGLK_RESOURCE_SSBO ? "ssbo" : "ubo", res[i].access);
   const mglk_reloc *rel = mglk_kernel_relocs(kernel, &n);
   fprintf(json, "],\n \"relocs\": [");
   for (size_t i = 0; i < n; i++)
      fprintf(json, "%s{\"id\": %u, \"offset\": %u, \"delta\": %u, \"type\": %u}",
              i ? ", " : "", rel[i].id, rel[i].offset, rel[i].delta, rel[i].type);
   fprintf(json, "],\n \"outputs\": {\"isa\": ");
   char path[4096];
   snprintf(path, sizeof(path), "%s.isa.bin", prefix);
   json_str(path);
   fprintf(json, ", \"disassembly\": ");
   snprintf(path, sizeof(path), "%s.disasm.txt", prefix);
   json_str(path);
   fprintf(json, ", \"log\": ");
   snprintf(path, sizeof(path), "%s.log.txt", prefix);
   json_str(path);
   fprintf(json, "}");
}

int
main(int argc, char **argv)
{
   const char *argv0 = argv[0], *entry = "main", *prefix = NULL, *input = NULL;
   unsigned long device = MGLK_PCI_GLK_2X6;
   mglk_binding bindings[MGLK_MAX_BINDINGS];
   size_t nbindings = 0;

   for (int i = 1; i < argc; i++) {
      const char *arg = argv[i];
      const bool has_value = i + 1 < argc;
      if (strcmp(arg, "--device") == 0 && has_value) {
         char *end = NULL;
         device = strtoul(argv[++i], &end, 0);
         if (!end || *end || device > 0xffffu)
            return usage(argv0, "--device takes a 16-bit PCI device id");
      } else if (strcmp(arg, "--entry") == 0 && has_value) {
         entry = argv[++i];
      } else if (strcmp(arg, "--out") == 0 && has_value) {
         prefix = argv[++i];
      } else if (strcmp(arg, "--binding") == 0 && has_value) {
         unsigned set, binding, bti;
         char kind[8], tail;
         if (nbindings == MGLK_MAX_BINDINGS)
            return usage(argv0, "too many --binding entries");
         if (sscanf(argv[++i], "%u:%u:%u:%7[a-z]%c", &set, &binding, &bti, kind, &tail) != 4 ||
             (strcmp(kind, "ssbo") != 0 && strcmp(kind, "ubo") != 0))
            return usage(argv0, "--binding takes SET:BINDING:BTI:ssbo|ubo");
         bindings[nbindings++] = (mglk_binding){
            .set = set, .binding = binding, .bti = bti,
            .kind = strcmp(kind, "ssbo") == 0 ? MGLK_RESOURCE_SSBO : MGLK_RESOURCE_UBO,
         };
      } else if (arg[0] == '-' || input) {
         return usage(argv0, "unexpected argument");
      } else {
         input = arg;
      }
   }
   if (!input || !prefix)
      return usage(argv0, "an input file and --out PREFIX are required");

   char path[4096];
   if ((size_t)snprintf(path, sizeof(path), "%s.json", prefix) >= sizeof(path))
      return usage(argv0, "--out prefix too long");
   json = fopen(path, "w");
   if (!json) {
      perror(path);
      return 2;
   }
   fprintf(json, "{\"tool\": \"mglk-compile\", \"abi_version\": %u, \"mesa_commit\": \"%s\", "
           "\"mesa_tree\": \"%s\", \"gpu_execution\": \"NOT_RUN\",\n \"input\": ",
           MGLK_ABI_VERSION, MGLK_MESA_COMMIT, MGLK_MESA_TREE);
   json_str(input);
   fprintf(json, ", \"entry_point\": ");
   json_str(entry);
   fprintf(json, ", \"device_request\": %lu,\n \"bindings\": [", device);
   for (size_t i = 0; i < nbindings; i++)
      fprintf(json, "%s{\"set\": %u, \"binding\": %u, \"bti\": %u, \"kind\": \"%s\"}",
              i ? ", " : "", bindings[i].set, bindings[i].binding, bindings[i].bti,
              bindings[i].kind == MGLK_RESOURCE_SSBO ? "ssbo" : "ubo");
   fprintf(json, "]");

   mglk_diagnostic diag = { .status = MGLK_OK, .message = "" };
   mglk_compiler *compiler = NULL;
   mglk_kernel *kernel = NULL;
   uint32_t *words = NULL;
   size_t count = 0;
   mglk_status status = read_spirv(input, &words, &count, diag.message, sizeof(diag.message));
   fprintf(json, ", \"input_words\": %zu", count);
   if (status == MGLK_OK)
      status = mglk_compiler_create((uint32_t)device, &compiler, &diag);
   if (status == MGLK_OK) {
      mglk_device_desc desc;
      mglk_compiler_describe(compiler, &desc);
      print_device(&desc);
      status = mglk_compile_compute_spirv(compiler, words, count, entry, bindings,
                                          nbindings, &kernel, &diag);
   }

   bool io_ok = true;
   if (status == MGLK_OK) {
      size_t size = 0;
      const uint8_t *program = mglk_kernel_program(kernel, &size);
      const char *dis = mglk_kernel_disassembly(kernel), *log = mglk_kernel_log(kernel);
      io_ok = write_output(prefix, ".isa.bin", program, size) &&
              write_output(prefix, ".disasm.txt", dis, strlen(dis)) &&
              write_output(prefix, ".log.txt", log, strlen(log));
      print_kernel(kernel, prefix);
   }
   fprintf(json, ",\n \"status\": \"%s\", \"message\": ", mglk_status_name(status));
   json_str(diag.message);
   fprintf(json, "}\n");
   io_ok = fclose(json) == 0 && io_ok;

   mglk_kernel_destroy(kernel);
   mglk_compiler_destroy(compiler);
   free(words);
   if (!io_ok) {
      fprintf(stderr, "%s: failed to write outputs for %s\n", argv0, prefix);
      return 2;
   }
   if (status != MGLK_OK) {
      fprintf(stderr, "%s: %s: %s\n", argv0, mglk_status_name(status), diag.message);
      return 1;
   }
   return 0;
}
