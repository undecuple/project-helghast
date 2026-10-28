/* Symbols a lifted game normally supplies (ppu_recomp.c), empty for the ps3recomp D3D12 host smoke
 * test (tools/smoke/build-host.sh). Layout matches func_entry in the generated ppu_recomp.h. */
#include <stdint.h>
#include <stddef.h>
struct kz3_func_entry { uint64_t addr; void* func; const char* name; };
const struct kz3_func_entry function_table[1] = { { 0, NULL, NULL } };
const uint64_t function_table_count = 0;

/* Unbuffered stdout/stderr so the host's progress lines survive an abnormal exit under Proton. */
#include <stdio.h>
__attribute__((constructor)) static void kz3_unbuffered_stdio(void)
{
    setvbuf(stdout, NULL, _IONBF, 0);
    setvbuf(stderr, NULL, _IONBF, 0);
}

/* Patch 1128 (SPU pass 107): sys_ppu_thread.c frees the exiting thread's guest callback-stack slot in ppu_loader.cpp,
 * which the host smoke build does not link; the host build runs no guest callbacks, so there is no slot to free. */
void ppu_guest_call_stack_release(void) {}
