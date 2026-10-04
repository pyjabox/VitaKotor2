/* crash.c -- hardware CPU-fault capture (see crash.h)
 *
 * Until this was wired, a hardware fault inside the game (e.g. anywhere past
 * `>>> entering SDL_main`) left log.txt simply *stopping* with no PC/LR -- the
 * only evidence was a gzipped psp2core dump that needed offline parsing. This
 * registers a kubridge exception handler that writes the full CPU state to
 * log.txt at the moment of the fault, and -- crucially -- resolves the faulting
 * PC/LR against the base address of each so-loaded module so the crash can be
 * routed to addr2line without touching a coredump:
 *
 *   arm-vita-eabi-addr2line -f -C -e <the .so or build/KOTOR> <reported offset>
 *
 * The handler must NOT return (returning resumes at the faulting instruction ->
 * fault loop); it logs, then parks so the on-disk log survives for retrieval.
 */

#include <vitasdk.h>
#include <kubridge.h>
#include <string.h>

#include "crash.h"
#include "so_util.h"
#include "log.h"

// Loaded modules (defined in main.c) so we can turn an absolute fault address
// into "<module> + <offset>" for symbolication.
extern so_module kotor_mod;
extern so_module port_mod;

// Static scratch -- the exception handler runs on a constrained stack, and
// SceKernelModuleInfo alone is 440 bytes.
static SceUID s_modids[128];
static SceKernelModuleInfo s_modinfo;

// Resolve an address against the *system* module map (SceGxm, SceShaccCg,
// SceLibKernel, our own eboot...). A fault inside a Sony module reports a PC in
// the 0xE0000000 range that means nothing on its own; knowing which module owns
// it is usually the whole diagnosis. Returns 1 if it was resolved.
static int describe_in_system_modules(const char *label, uint32_t addr) {
  SceSize num = sizeof(s_modids) / sizeof(s_modids[0]);
  if (sceKernelGetModuleList(0xFF, s_modids, &num) < 0)
    return 0;
  for (SceSize i = 0; i < num; i++) {
    memset(&s_modinfo, 0, sizeof(s_modinfo));
    s_modinfo.size = sizeof(s_modinfo);
    if (sceKernelGetModuleInfo(s_modids[i], &s_modinfo) < 0)
      continue;
    for (int s = 0; s < 4; s++) {
      uint32_t base = (uint32_t)s_modinfo.segments[s].vaddr;
      uint32_t sz = (uint32_t)s_modinfo.segments[s].memsz;
      if (!base || !sz) continue;
      if (addr >= base && addr < base + sz) {
        log_printf("    %s = 0x%08x  [%s seg%d + 0x%x]", label, addr,
                   s_modinfo.module_name, s, (unsigned)(addr - base));
        return 1;
      }
    }
  }
  return 0;
}

// Full map, dumped once per fault: lets any address in the register dump be
// attributed offline without another hardware round-trip.
static void dump_module_map(void) {
  SceSize num = sizeof(s_modids) / sizeof(s_modids[0]);
  if (sceKernelGetModuleList(0xFF, s_modids, &num) < 0) {
    log_printf("  [modmap] sceKernelGetModuleList failed");
    return;
  }
  log_printf("  [modmap] %u modules loaded:", (unsigned)num);
  for (SceSize i = 0; i < num; i++) {
    memset(&s_modinfo, 0, sizeof(s_modinfo));
    s_modinfo.size = sizeof(s_modinfo);
    if (sceKernelGetModuleInfo(s_modids[i], &s_modinfo) < 0)
      continue;
    log_printf("    %-28s seg0=0x%08x+0x%-8x seg1=0x%08x+0x%x",
               s_modinfo.module_name,
               (unsigned)(uintptr_t)s_modinfo.segments[0].vaddr,
               (unsigned)s_modinfo.segments[0].memsz,
               (unsigned)(uintptr_t)s_modinfo.segments[1].vaddr,
               (unsigned)s_modinfo.segments[1].memsz);
  }
}

// Name + module-relative offset of an absolute address, for addr2line routing.
static void describe_addr(const char *label, uint32_t addr) {
  if (addr >= kotor_mod.text_base &&
      addr <  kotor_mod.text_base + kotor_mod.text_size) {
    log_printf("    %s = 0x%08x  [libkotor2.so + 0x%x]",
               label, addr, (unsigned)(addr - kotor_mod.text_base));
  } else if (addr >= port_mod.text_base &&
             addr <  port_mod.text_base + port_mod.text_size) {
    log_printf("    %s = 0x%08x  [libObbVfs.so + 0x%x]",
               label, addr, (unsigned)(addr - port_mod.text_base));
  } else if (!describe_in_system_modules(label, addr)) {
    // Our loader (build/KOTOR, text vaddr 0x81000000) or an unmapped address.
    log_printf("    %s = 0x%08x  [loader/system]", label, addr);
  }
}

static const char *type_name(uint32_t t) {
  switch (t) {
    case KU_KERNEL_EXCEPTION_TYPE_DATA_ABORT:            return "DATA_ABORT";
    case KU_KERNEL_EXCEPTION_TYPE_PREFETCH_ABORT:        return "PREFETCH_ABORT";
    case KU_KERNEL_EXCEPTION_TYPE_UNDEFINED_INSTRUCTION: return "UNDEFINED_INSTRUCTION";
    default:                                             return "UNKNOWN";
  }
}

static void crash_handler(KuKernelExceptionContext *c) {
  // Before anything else: drain the buffered log and switch to unbuffered,
  // lock-free writes. The faulting thread may itself hold the log lock, so
  // taking it here would park the dump forever instead of writing it.
  log_panic();
  log_printf("========================================");
  log_printf("[CRASH] CPU fault: %s (type=%u)", type_name(c->exceptionType),
             (unsigned)c->exceptionType);
  log_printf("  module bases: libkotor2.so=0x%08x (+0x%x)  libObbVfs.so=0x%08x (+0x%x)",
             (unsigned)kotor_mod.text_base, (unsigned)kotor_mod.text_size,
             (unsigned)port_mod.text_base, (unsigned)port_mod.text_size);
  describe_addr("PC", c->pc);
  describe_addr("LR", c->lr);
  log_printf("    SP = 0x%08x   FAR = 0x%08x   FSR = 0x%08x",
             (unsigned)c->sp, (unsigned)c->FAR, (unsigned)c->FSR);
  log_printf("  r0-r3   %08x %08x %08x %08x",
             (unsigned)c->r0, (unsigned)c->r1, (unsigned)c->r2, (unsigned)c->r3);
  log_printf("  r4-r7   %08x %08x %08x %08x",
             (unsigned)c->r4, (unsigned)c->r5, (unsigned)c->r6, (unsigned)c->r7);
  log_printf("  r8-r11  %08x %08x %08x %08x",
             (unsigned)c->r8, (unsigned)c->r9, (unsigned)c->r10, (unsigned)c->r11);
  log_printf("  r12     %08x", (unsigned)c->r12);
  dump_module_map();
  log_printf("[CRASH] parked; retrieve ux0:data/kotor2/log.txt");
  log_printf("========================================");
  log_flush();   // the log fd is kept open for speed -- make the dump durable

  // Do not resume (would re-fault). Park so the flushed log survives.
  while (1)
    sceKernelDelayThread(1000 * 1000);
}

void crash_init(void) {
  KuKernelExceptionHandlerOpt opt;
  opt.size = sizeof(opt);
  int a = kuKernelRegisterExceptionHandler(
      KU_KERNEL_EXCEPTION_TYPE_DATA_ABORT, crash_handler, NULL, &opt);
  int b = kuKernelRegisterExceptionHandler(
      KU_KERNEL_EXCEPTION_TYPE_PREFETCH_ABORT, crash_handler, NULL, &opt);
  int c = kuKernelRegisterExceptionHandler(
      KU_KERNEL_EXCEPTION_TYPE_UNDEFINED_INSTRUCTION, crash_handler, NULL, &opt);
  log_printf("[crash] exception handlers registered (data=0x%x prefetch=0x%x undef=0x%x)",
             a, b, c);
}
