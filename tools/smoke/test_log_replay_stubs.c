/* Link stubs for tools/smoke/test_log_replay.c (pass 156): the walker's REAL method processor (rsx_commands.c, which writes the
 * 0x1D70/0x1D74 label words the capture records as WORD tag 9) is linked into the harness; it declares these two as extern. */
#include <stdint.h>
extern uint8_t* vm_base;
void vm_write32(uint32_t a, uint32_t v)
{
    uint8_t* p = vm_base + a;
    p[0] = (uint8_t)(v >> 24); p[1] = (uint8_t)(v >> 16); p[2] = (uint8_t)(v >> 8); p[3] = (uint8_t)v;
}
void ps3_ms(const char* msg) { (void)msg; }
