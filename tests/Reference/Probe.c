/* Licensed under the MIT License. Standalone C oracle: no product C++ library. */
#include "COracle.h"
#include "mir-gen.h"
#include <errno.h>
#include <inttypes.h>
#include <stdlib.h>
#include <string.h>

static void observe(MIR_context_t ctx, const char *stage, MIR_item_t item, FILE *snapshot, void *data) {
  (void) data;
  printf("stage:%s function:%s\n", stage, MIR_item_name(ctx, item));
  if (snapshot != NULL) {
    char buffer[4096]; size_t count;
    rewind(snapshot);
    while ((count = fread(buffer, 1, sizeof(buffer), snapshot)) != 0) fwrite(buffer, 1, count, stdout);
  } else MIR_output_item(ctx, stdout, item);
  puts("end-stage");
}
static int64_t fixture_unused(void) { return 0; }
static void *fixture_resolver(const char *name) {
  return strcmp(name, "unused") ? NULL : (void *) fixture_unused;
}
struct label_identity { MIR_insn_t label; size_t number; };
struct operand_identities {
  MIR_reg_t *registers, next_register;
  size_t register_capacity, label_count;
  struct label_identity *labels;
};
static int compare_labels(const void *left, const void *right) {
  uintptr_t a = (uintptr_t) ((const struct label_identity *) left)->label;
  uintptr_t b = (uintptr_t) ((const struct label_identity *) right)->label;
  return (a > b) - (a < b);
}
static MIR_reg_t register_identity(struct operand_identities *identities, MIR_reg_t reg) {
  if (!reg) return 0;
  if (reg >= identities->register_capacity) {
    size_t capacity = (size_t) reg + 1, previous = identities->register_capacity;
    if (capacity < previous * 2) capacity = previous * 2;
    MIR_reg_t *registers = realloc(identities->registers, capacity * sizeof(*registers));
    if (!registers) exit(2);
    memset(registers + previous, 0, (capacity - previous) * sizeof(*registers));
    identities->registers = registers; identities->register_capacity = capacity;
  }
  if (!identities->registers[reg]) identities->registers[reg] = ++identities->next_register;
  return identities->registers[reg];
}
static void operand_snapshot(MIR_context_t ctx, MIR_func_t function, MIR_op_t op, struct operand_identities *identities) {
  switch (op.mode) {
  case MIR_OP_REG:
    printf("reg:%" PRIu32 ":%d:%s", register_identity(identities, op.u.reg), (int) MIR_reg_type(ctx, op.u.reg, function),
      MIR_reg_hard_reg_name(ctx, op.u.reg, function) ? MIR_reg_hard_reg_name(ctx, op.u.reg, function) : ""); break;
  case MIR_OP_INT: printf("int:%016" PRIx64, (uint64_t) op.u.i); break;
  case MIR_OP_UINT: printf("uint:%016" PRIx64, op.u.u); break;
  case MIR_OP_FLOAT: { uint32_t bits; memcpy(&bits, &op.u.f, 4); printf("float:%08" PRIx32, bits); break; }
  case MIR_OP_DOUBLE: { uint64_t bits; memcpy(&bits, &op.u.d, 8); printf("double:%016" PRIx64, bits); break; }
  case MIR_OP_LDOUBLE: {
#if defined(__x86_64__) && !defined(_WIN32)
    size_t count = 10;
#else
    size_t count = sizeof(long double);
#endif
    fputs("long-double:", stdout); for (size_t n = 0; n < count; ++n) printf("%02x", ((unsigned char *) &op.u.ld)[n]); break;
  }
  case MIR_OP_REF:
    printf("symbol:%s/%d:%s", op.u.ref->module->name, (int) op.u.ref->item_type, MIR_item_name(ctx, op.u.ref)); break;
  case MIR_OP_LABEL: {
    struct label_identity key = {op.u.label, 0};
    const struct label_identity *found = bsearch(&key, identities->labels, identities->label_count,
                                                sizeof(key), compare_labels);
    if (!found) exit(2);
    printf("label:%zu", found->number); break;
  }
  case MIR_OP_STR:
    fputs("string:", stdout); for (size_t n = 0; n < op.u.str.len; ++n) printf("%02x", (unsigned char) op.u.str.s[n]); break;
  case MIR_OP_MEM:
    {
    MIR_reg_t base = register_identity(identities, op.u.mem.base);
    MIR_reg_t index = register_identity(identities, op.u.mem.index);
    printf("memory:%d:%016" PRIx64 ":%" PRIu32 ":%" PRIu32 ":%u:%s:%s", (int) op.u.mem.type, (uint64_t) op.u.mem.disp,
      base, index, op.u.mem.index ? op.u.mem.scale : 1,
      op.u.mem.alias ? MIR_alias_name(ctx, op.u.mem.alias) : "", op.u.mem.nonalias ? MIR_alias_name(ctx, op.u.mem.nonalias) : ""); break;
    }
  default: printf("unexpected:%d", (int) op.mode); break;
  }
}

int main(int argc, char **argv) {
  if (argc < 4 || argc > 5) { fputs("usage: reference-probe INPUT LEVEL [binary-]jit|interpret|scan|encode [OUTPUT]\n", stderr); return 2; }
  const char *mode = argv[3]; int binary_input = !strncmp(mode, "binary-", 7); if (binary_input) mode += 7;
  char *end; errno = 0; unsigned long level = strtoul(argv[2], &end, 10);
  if (errno || *end || level > 3 || (strcmp(mode, "jit") && strcmp(mode, "interpret") && strcmp(mode, "scan") && strcmp(mode, "encode"))) return 2;
  MIR_context_t ctx = MIR_init();
  FILE *input = fopen(argv[1], "rb"); if (!input) return 2;
  if (binary_input) { MIR_read(ctx, input); fclose(input); }
  else {
  if (fseek(input, 0, SEEK_END)) { fclose(input); return 2; }
  long length = ftell(input);
  if (length < 0 || (uintmax_t) length >= SIZE_MAX) { fclose(input); return 2; }
  rewind(input); char *text = malloc((size_t) length + 1);
  if (!text) { fclose(input); return 2; }
  if (fread(text, 1, (size_t) length, input) != (size_t) length) { free(text); fclose(input); return 2; }
  fclose(input); text[length] = 0;
  MIR_scan_string(ctx, text); free(text);
  }
  if (!strcmp(mode, "encode")) {
    if (argc != 5) { MIR_finish(ctx); return 2; }
    FILE *output = fopen(argv[4], "wb"); if (!output) { MIR_finish(ctx); return 2; }
    MIR_write(ctx, output); int failed = fclose(output); MIR_finish(ctx); return failed ? 2 : 0;
  }
  if (!strcmp(mode, "scan")) {
    size_t opcodes[MIR_INSN_BOUND] = {0}, modules = 0;
    for (MIR_module_t module = DLIST_HEAD(MIR_module_t, *MIR_get_module_list(ctx)); module;
         module = DLIST_NEXT(MIR_module_t, module)) {
      ++modules;
      for (MIR_item_t item = DLIST_HEAD(MIR_item_t, module->items); item; item = DLIST_NEXT(MIR_item_t, item)) {
        if (item->item_type != MIR_func_item) continue;
        struct operand_identities identities = {0};
        for (MIR_insn_t instruction = DLIST_HEAD(MIR_insn_t, item->u.func->insns); instruction;
             instruction = DLIST_NEXT(MIR_insn_t, instruction)) if (instruction->code == MIR_LABEL) ++identities.label_count;
        if (identities.label_count) {
          identities.labels = malloc(identities.label_count * sizeof(*identities.labels)); if (!identities.labels) return 2;
          size_t number = 0;
          for (MIR_insn_t instruction = DLIST_HEAD(MIR_insn_t, item->u.func->insns); instruction;
               instruction = DLIST_NEXT(MIR_insn_t, instruction)) if (instruction->code == MIR_LABEL) {
            identities.labels[number] = (struct label_identity) {instruction, number}; ++number;
          }
          qsort(identities.labels, identities.label_count, sizeof(*identities.labels), compare_labels);
        }
        for (MIR_insn_t instruction = DLIST_HEAD(MIR_insn_t, item->u.func->insns); instruction;
             instruction = DLIST_NEXT(MIR_insn_t, instruction)) {
          ++opcodes[instruction->code];
          printf("instruction:%s/%s", item->u.func->name, MIR_insn_name(ctx, instruction->code));
          if (instruction->code == MIR_LABEL) {
            putchar(' '); operand_snapshot(ctx, item->u.func, MIR_new_label_op(ctx, instruction), &identities);
            putchar('\n'); continue;
          }
          for (size_t n = 0; n < instruction->nops; ++n) { putchar(' '); operand_snapshot(ctx, item->u.func, instruction->ops[n], &identities); }
          putchar('\n');
        }
        free(identities.registers); free(identities.labels);
      }
    }
    printf("modules:%zu\n", modules);
    printf("long-double-is-double:%d\n",
#if defined(_WIN32) || __SIZEOF_LONG_DOUBLE__ == 8
           1
#else
           0
#endif
    );
    for (unsigned n = 0; n < MIR_INSN_BOUND; ++n) if (opcodes[n]) printf("opcode:%s=%zu\n", MIR_insn_name(ctx, (MIR_insn_code_t) n), opcodes[n]);
    MIR_finish(ctx); return 0;
  }
  MIR_item_t entry = NULL;
  for (MIR_module_t module = DLIST_HEAD(MIR_module_t, *MIR_get_module_list(ctx)); module;
       module = DLIST_NEXT(MIR_module_t, module)) {
    MIR_load_module(ctx, module);
    for (MIR_item_t item = DLIST_HEAD(MIR_item_t, module->items); item; item = DLIST_NEXT(MIR_item_t, item))
      if (item->item_type == MIR_func_item && !strcmp(item->u.func->name, "entry")) entry = item;
  }
  if (!entry || entry->u.func->nargs || entry->u.func->nres != 1 || entry->u.func->res_types[0] != MIR_T_I64) {
    MIR_finish(ctx); return 2;
  }
  int64_t result;
  if (!strcmp(mode, "interpret")) {
    MIR_link(ctx, MIR_set_interp_interface, fixture_resolver);
    MIR_val_t value; MIR_interp_arr(ctx, entry, &value, 0, NULL); result = value.i;
  } else {
    MIR_gen_init(ctx); MIR_gen_set_optimize_level(ctx, (unsigned) level);
    MIR_opt_set_test_observer(ctx, observe, NULL);
    MIR_link(ctx, MIR_set_gen_interface, fixture_resolver);
    result = ((int64_t (*)(void)) MIR_gen(ctx, entry))();
    MIR_gen_finish(ctx);
  }
  printf("result:i64=%" PRId64 "\n", result); MIR_finish(ctx); return 0;
}
