/*
 * XOG: a guest instruction that reached a device register. With code running
 * natively there is no translator turning it into a plain load or store, so any
 * instruction with a memory operand can land here: test, and, or, inc, rep movs...
 * The fault gives the operand's address; this does the instruction against the
 * device, flags included. Our own code. GPL-3.0-or-later.
 */
#include "native/core/mmio.h"
#include "native/core/vm.h"
#include "native/cpu/cpu.h"

#include <stdio.h>
#include <string.h>

extern NcCpu *g_cpu_for_ram(void);

#define RX_ARITH 0x8D5u /* OF SF ZF AF PF CF */
#define RX_CF 0x1u
#define RX_DF 0x400u

enum { A_ADD, A_OR, A_ADC, A_SBB, A_AND, A_SUB, A_XOR, A_CMP, A_TEST };

/* The operation done by the CPU itself, for its exact flags. */
#define RX_ALU(ins, suf, R)                                                                                  \
    __asm__ volatile("leaq -128(%%rsp), %%rsp\n pushq %[f]\n popfq\n " ins suf " %" R "[b], %" R "[a]\n"     \
                     " pushfq\n popq %[f]\n leaq 128(%%rsp), %%rsp"                                          \
                     : [a] "+r"(a), [f] "+r"(fl)                                                             \
                     : [b] "r"(b)                                                                            \
                     : "cc")

#define RX_ALU3(ins)                                                                                         \
    do {                                                                                                     \
        if (bytes == 1) {                                                                                    \
            RX_ALU(ins, "b", "b");                                                                           \
        } else if (bytes == 2) {                                                                             \
            RX_ALU(ins, "w", "w");                                                                           \
        } else {                                                                                             \
            RX_ALU(ins, "l", "k");                                                                           \
        }                                                                                                    \
    } while (0)

static uint32_t alu(unsigned op, unsigned bytes, uint32_t a, uint32_t b, uint32_t *eflags)
{
    uint64_t fl = (*eflags & RX_ARITH) | 2u;
    switch (op) {
    case A_ADD: RX_ALU3("add"); break;
    case A_OR: RX_ALU3("or"); break;
    case A_ADC: RX_ALU3("adc"); break;
    case A_SBB: RX_ALU3("sbb"); break;
    case A_AND: RX_ALU3("and"); break;
    case A_SUB: RX_ALU3("sub"); break;
    case A_XOR: RX_ALU3("xor"); break;
    case A_CMP: RX_ALU3("cmp"); break;
    default: RX_ALU3("test"); break;
    }
    *eflags = (*eflags & ~RX_ARITH) | ((uint32_t)fl & RX_ARITH);
    return a;
}

static uint32_t get_reg(NcFrame *f, unsigned r, unsigned bytes)
{
    if (bytes == 1) {
        return r < 4 ? nc_frame_gpr(f, (int)r) & 0xFF : (nc_frame_gpr(f, (int)r - 4) >> 8) & 0xFF;
    }
    const uint32_t v = nc_frame_gpr(f, (int)r);
    return bytes == 2 ? v & 0xFFFF : v;
}

static void set_reg(NcFrame *f, unsigned r, unsigned bytes, uint32_t v)
{
    if (bytes == 1) {
        const int w = r < 4 ? (int)r : (int)r - 4;
        const uint32_t o = nc_frame_gpr(f, w);
        nc_frame_set_gpr(f, w, r < 4 ? (o & ~0xFFu) | (v & 0xFF) : (o & ~0xFF00u) | ((v & 0xFF) << 8));
    } else if (bytes == 2) {
        nc_frame_set_gpr(f, (int)r, (nc_frame_gpr(f, (int)r) & ~0xFFFFu) | (v & 0xFFFF));
    } else {
        nc_frame_set_gpr(f, (int)r, v);
    }
}

static unsigned modrm_len(const uint8_t *p)
{
    const unsigned mod = p[0] >> 6, rm = p[0] & 7;
    unsigned n = 1;
    if (mod == 3) {
        return n;
    }
    if (rm == 4) {
        n++;
        if (mod == 0 && (p[1] & 7) == 5) {
            n += 4;
        }
    } else if (mod == 0 && rm == 5) {
        n += 4;
    }
    return n + (mod == 1 ? 1 : mod == 2 ? 4 : 0);
}

static uint32_t imm_at(const uint8_t *p, unsigned bytes)
{
    uint32_t v = 0;
    memcpy(&v, p, bytes);
    return v;
}

static uint32_t sext8(uint8_t v, unsigned bytes)
{
    const uint32_t s = (uint32_t)(int32_t)(int8_t)v;
    return bytes == 1 ? s & 0xFF : bytes == 2 ? s & 0xFFFF : s;
}

/* A device read or write; ordinary memory on the other side of a string move
 * goes straight through. */
/* The operand that stopped an access against RAM: not mapped. */
bool rx_ram_hole_set, rx_ram_hole_store;
uint32_t rx_ram_hole;

static void ram_hole(uint32_t a, bool store)
{
    rx_ram_hole_set = true;
    rx_ram_hole = a;
    rx_ram_hole_store = store;
}

static NcVm *g_ram_vm;     /* set: reads and writes are RAM's, through the host's view */
static void (*g_ram_stored)(uint32_t pa, uint32_t n);

extern uint8_t *g_va_soft;
extern uint32_t rx_va_bound_pa(uint32_t va);

/* Where a guest byte lives now: a shadowed block's own copy, or else its physical page. */
static uint8_t *ram_byte(NcVm *vm, uint32_t a)
{
    if (g_va_soft[a >> 14] == 2) {
        return (uint8_t *)(uintptr_t)a;
    }
    const uint32_t pa = nc_vm_phys(vm, a);
    return vm->phys + (pa != UINT32_MAX ? pa : rx_va_bound_pa(a));
}

static bool ram_mapped(uint32_t a, unsigned n)
{
    for (unsigned i = 0; i < n; i++) {
        /* a hole on a soft block; on a bound one the CPU would simply reach it */
        if (nc_vm_phys(g_ram_vm, a + i) == UINT32_MAX && rx_va_bound_pa(a + i) == UINT32_MAX) {
            return false;
        }
    }
    return true;
}

static bool rd(uint32_t a, unsigned n, uint32_t *v)
{
    uint64_t x = 0;
    if (g_ram_vm) {
        if (!ram_mapped(a, n)) {
            ram_hole(a, false);
            return false;
        }
        uint32_t r = 0;
        for (unsigned i = 0; i < n; i++) {
            r |= (uint32_t)*ram_byte(g_ram_vm, a + i) << (8 * i);
        }
        *v = r;
        return true;
    }
    if (!nc_mmio_access(a, n, false, &x)) {
        return false;
    }
    *v = (uint32_t)x;
    return true;
}

static bool wr(uint32_t a, unsigned n, uint32_t v)
{
    uint64_t x = v;
    if (g_ram_vm) {
        if (!ram_mapped(a, n)) {
            ram_hole(a, true);
            return false;
        }
        for (unsigned i = 0; i < n; i++) {
            const uint32_t pa = nc_vm_phys(g_ram_vm, a + i);
            const bool shadow = g_va_soft[(a + i) >> 14] == 2;  /* its page hears at the write-back */
            if (!shadow && pa != UINT32_MAX && (i == 0 || (pa & 4095u) == 0)) {
                g_ram_stored(pa, n - i);    /* before the store, each page once */
            }
            *ram_byte(g_ram_vm, a + i) = (uint8_t)(v >> (8 * i));
        }
        return true;
    }
    return nc_mmio_access(a, n, true, &x);
}

static bool string_op(NcFrame *f, uint8_t op, bool rep, unsigned bytes, unsigned len)
{
    uint32_t eflags = nc_frame_eflags(f);
    const int step = (eflags & RX_DF) ? -(int)bytes : (int)bytes;
    if (rep && nc_frame_gpr(f, NC_ECX) == 0) {
        nc_frame_set_eip(f, nc_frame_eip(f) + len);
        return true;
    }
    /* Against RAM a rep goes on here while its memory is mapped, rather than a fault an element. */
    for (unsigned count = 0;; count++) {
        const uint32_t esi = nc_frame_gpr(f, NC_ESI), edi = nc_frame_gpr(f, NC_EDI);
        uint32_t v = 0;
        if (g_ram_vm) {
            const bool load = op == 0xAC || op == 0xAD || op == 0xA4 || op == 0xA5;
            const bool store = op != 0xAC && op != 0xAD;
            if ((load && !ram_mapped(esi, bytes)) || (store && !ram_mapped(edi, bytes))) {
                if (!count) {
                    const bool src = load && !ram_mapped(esi, bytes);
                    ram_hole(src ? esi : edi, !src);
                }
                return count != 0;  /* done so far stands; the next element faults by itself */
            }
        }
        if (op == 0xAA || op == 0xAB) {         /* stos */
            v = get_reg(f, NC_EAX, bytes);
            if (!wr(edi, bytes, v)) {
                memcpy((void *)(uintptr_t)edi, &v, bytes);
            }
            nc_frame_set_gpr(f, NC_EDI, edi + (uint32_t)step);
        } else if (op == 0xAC || op == 0xAD) {  /* lods */
            if (!rd(esi, bytes, &v)) {
                memcpy(&v, (const void *)(uintptr_t)esi, bytes);
            }
            set_reg(f, NC_EAX, bytes, v);
            nc_frame_set_gpr(f, NC_ESI, esi + (uint32_t)step);
        } else {                                /* movs */
            if (!rd(esi, bytes, &v)) {
                memcpy(&v, (const void *)(uintptr_t)esi, bytes);
            }
            if (!wr(edi, bytes, v)) {
                memcpy((void *)(uintptr_t)edi, &v, bytes);
            }
            nc_frame_set_gpr(f, NC_ESI, esi + (uint32_t)step);
            nc_frame_set_gpr(f, NC_EDI, edi + (uint32_t)step);
        }
        if (rep) {
            const uint32_t ecx = nc_frame_gpr(f, NC_ECX) - 1;
            nc_frame_set_gpr(f, NC_ECX, ecx);
            if (ecx != 0) {
                if (g_ram_vm && count < 0x100000u) {
                    continue;
                }
                return true;    /* EIP stays: the next element comes here again */
            }
        }
        break;
    }
    nc_frame_set_eip(f, nc_frame_eip(f) + len);
    return true;
}

extern volatile uint32_t rx_last_mmio;

/* The memory operand's address: for an instruction that no longer faults its way here. */
bool rx_mmio_ea(const NcFrame *f, const uint8_t *code, uint32_t *addr)
{
    unsigned pre = 0;
    for (; pre < 14; pre++) {
        const uint8_t b = code[pre];
        if (!(b == 0x66 || b == 0xF3 || b == 0xF2 || b == 0x26 || b == 0x2E || b == 0x36 || b == 0x3E || b == 0x64 ||
              b == 0x65 || b == 0xF0)) {
            break;
        }
    }
    const uint8_t *q = code + pre;
    if (q[0] >= 0xA0 && q[0] <= 0xA3) {
        memcpy(addr, q + 1, 4);
        return true;
    }
    if (q[0] == 0x0F) {
        q++;
    }
    const uint8_t mrm = q[1];
    const unsigned mod = mrm >> 6, rm = mrm & 7;
    if (mod == 3) {
        return false;
    }
    const uint8_t *p = q + 2;
    uint32_t a = 0, d = 0;
    if (rm == 4) {
        const uint8_t sib = *p++;
        const unsigned scale = sib >> 6, index = (sib >> 3) & 7, base = sib & 7;
        if (index != 4) {
            a += nc_frame_gpr(f, (int)index) << scale;
        }
        if (base == 5 && mod == 0) {
            memcpy(&d, p, 4);
            a += d;
            p += 4;
        } else {
            a += nc_frame_gpr(f, (int)base);
        }
    } else if (rm == 5 && mod == 0) {
        memcpy(&a, p, 4);
        p += 4;
    } else {
        a = nc_frame_gpr(f, (int)rm);
    }
    if (mod == 1) {
        a += (uint32_t)(int32_t)(int8_t)*p;
    } else if (mod == 2) {
        memcpy(&d, p, 4);
        a += d;
    }
    *addr = a;
    return true;
}

bool rx_mmio_emulate_at(NcFrame *f, const uint8_t *code, uint32_t addr);

bool rx_mmio_emulate(NcFrame *f, uint32_t addr)
{
    return rx_mmio_emulate_at(f, (const uint8_t *)(uintptr_t)nc_frame_eip(f), addr);
}

bool rx_mmio_emulate_at(NcFrame *f, const uint8_t *code, uint32_t addr)
{
    rx_last_mmio = addr;
    const uint32_t eip = nc_frame_eip(f);
    unsigned pre = 0, bytes = 4;
    bool rep = false;
    for (; pre < 14; pre++) {
        const uint8_t b = code[pre];
        if (b == 0x66) {
            bytes = 2;
        } else if (b == 0xF3 || b == 0xF2) {
            rep = true;
        } else if (!(b == 0x26 || b == 0x2E || b == 0x36 || b == 0x3E || b == 0x64 || b == 0x65 || b == 0x67 ||
                     b == 0xF0)) {
            break;
        }
    }
    const uint8_t *q = code + pre;
    const uint8_t op = q[0];
    uint32_t eflags = nc_frame_eflags(f);
    uint32_t mem = 0, len = 0;
    const unsigned w = op & 1 ? bytes : 1;     /* the usual width bit */

    /* add or adc sbb and sub xor cmp: 00-3B */
    if (op < 0x40 && (op & 7) < 4) {
        const unsigned kind = op >> 3, r = (q[1] >> 3) & 7;
        const bool to_mem = !(op & 2);
        if (!rd(addr, w, &mem)) {
            return false;
        }
        len = pre + 1 + modrm_len(q + 1);
        if (to_mem) {
            const uint32_t out = alu(kind, w, mem, get_reg(f, r, w), &eflags);
            if (kind != A_CMP) {
                wr(addr, w, out);
                if (nc_mmio_take_retry()) {
                    return true;
                }
            }
        } else {
            const uint32_t out = alu(kind, w, get_reg(f, r, w), mem, &eflags);
            if (kind != A_CMP) {
                set_reg(f, r, w, out);
            }
        }
        nc_frame_set_eflags(f, eflags);
        nc_frame_set_eip(f, eip + len);
        return true;
    }

    switch (op) {
    case 0x80: case 0x81: case 0x83: {      /* op r/m, imm */
        const unsigned kind = (q[1] >> 3) & 7, ml = modrm_len(q + 1);
        const unsigned width = op == 0x80 ? 1 : bytes;
        const unsigned il = op == 0x81 ? bytes : 1;
        const uint32_t imm = op == 0x81 ? imm_at(q + 1 + ml, il) : op == 0x83 ? sext8(q[1 + ml], width) : q[1 + ml];
        if (!rd(addr, width, &mem)) {
            return false;
        }
        const uint32_t out = alu(kind, width, mem, imm, &eflags);
        if (kind != A_CMP) {
            wr(addr, width, out);
            if (nc_mmio_take_retry()) {
                return true;
            }
        }
        len = pre + 1 + ml + il;
        nc_frame_set_eflags(f, eflags);
        break;
    }
    case 0x84: case 0x85:                   /* test r/m, r */
        if (!rd(addr, w, &mem)) {
            return false;
        }
        alu(A_TEST, w, mem, get_reg(f, (q[1] >> 3) & 7, w), &eflags);
        nc_frame_set_eflags(f, eflags);
        len = pre + 1 + modrm_len(q + 1);
        break;
    case 0x86: case 0x87: {                 /* xchg */
        const unsigned r = (q[1] >> 3) & 7;
        if (!rd(addr, w, &mem)) {
            return false;
        }
        wr(addr, w, get_reg(f, r, w));
        if (nc_mmio_take_retry()) {
            return true;
        }
        set_reg(f, r, w, mem);
        len = pre + 1 + modrm_len(q + 1);
        break;
    }
    case 0x88: case 0x89:                   /* mov r/m, r */
        if (!wr(addr, w, get_reg(f, (q[1] >> 3) & 7, w))) {
            return false;
        }
        if (nc_mmio_take_retry()) {
            return true;
        }
        len = pre + 1 + modrm_len(q + 1);
        break;
    case 0x8A: case 0x8B:                   /* mov r, r/m */
        if (!rd(addr, w, &mem)) {
            return false;
        }
        set_reg(f, (q[1] >> 3) & 7, w, mem);
        len = pre + 1 + modrm_len(q + 1);
        break;
    case 0xA0: case 0xA1:                   /* mov eax, moffs */
        if (!rd(addr, w, &mem)) {
            return false;
        }
        set_reg(f, NC_EAX, w, mem);
        len = pre + 5;
        break;
    case 0xA2: case 0xA3:                   /* mov moffs, eax */
        if (!wr(addr, w, get_reg(f, NC_EAX, w))) {
            return false;
        }
        if (nc_mmio_take_retry()) {
            return true;
        }
        len = pre + 5;
        break;
    case 0xA4: case 0xA5: case 0xAA: case 0xAB: case 0xAC: case 0xAD:
        return string_op(f, op, rep, w, pre + 1);
    case 0xC6: case 0xC7: {                 /* mov r/m, imm */
        const unsigned ml = modrm_len(q + 1), il = op == 0xC7 ? bytes : 1;
        if (!wr(addr, w, imm_at(q + 1 + ml, il))) {
            return false;
        }
        if (nc_mmio_take_retry()) {
            return true;
        }
        len = pre + 1 + ml + il;
        break;
    }
    case 0xF6: case 0xF7: {                 /* test imm, not, neg */
        const unsigned sub = (q[1] >> 3) & 7, ml = modrm_len(q + 1);
        if (!rd(addr, w, &mem)) {
            return false;
        }
        if (sub == 0 || sub == 1) {
            const unsigned il = op == 0xF7 ? bytes : 1;
            alu(A_TEST, w, mem, imm_at(q + 1 + ml, il), &eflags);
            nc_frame_set_eflags(f, eflags);
            len = pre + 1 + ml + il;
        } else if (sub == 2) {
            wr(addr, w, ~mem);
            len = pre + 1 + ml;
        } else if (sub == 3) {
            const uint32_t out = alu(A_SUB, w, 0, mem, &eflags);
            wr(addr, w, out);
            nc_frame_set_eflags(f, eflags);
            len = pre + 1 + ml;
        } else {
            goto unknown;
        }
        break;
    }
    case 0xFE: case 0xFF: {                 /* inc, dec, push */
        const unsigned sub = (q[1] >> 3) & 7, ml = modrm_len(q + 1);
        if (sub > 1 && !(op == 0xFF && sub == 6)) {
            goto unknown;
        }
        if (!rd(addr, w, &mem)) {
            return false;
        }
        if (sub == 6) {
            const uint32_t esp = nc_frame_gpr(f, NC_ESP) - 4;
            if (g_ram_vm) {
                if (!wr(esp, 4, mem)) {
                    return false;
                }
            } else {
                memcpy((void *)(uintptr_t)esp, &mem, 4);
            }
            nc_frame_set_gpr(f, NC_ESP, esp);
        } else {
            const uint32_t cf = eflags & RX_CF;     /* inc and dec keep it */
            const uint32_t out = alu(sub == 0 ? A_ADD : A_SUB, w, mem, 1, &eflags);
            eflags = (eflags & ~RX_CF) | cf;
            wr(addr, w, out);
            nc_frame_set_eflags(f, eflags);
        }
        len = pre + 1 + ml;
        break;
    }
    case 0x8F: {                            /* pop r/m */
        if (((q[1] >> 3) & 7) != 0) {
            goto unknown;
        }
        const uint32_t esp = nc_frame_gpr(f, NC_ESP);
        if (!wr(addr, 4, nc_rd32((const void *)(uintptr_t)esp))) {
            return false;
        }
        if (nc_mmio_take_retry()) {
            return true;
        }
        nc_frame_set_gpr(f, NC_ESP, esp + 4);
        len = pre + 1 + modrm_len(q + 1);
        break;
    }
    case 0x0F: {
        const uint8_t op2 = q[1];
        const unsigned ml = modrm_len(q + 2);
        if (op2 == 0xB6 || op2 == 0xB7 || op2 == 0xBE || op2 == 0xBF) {     /* movzx, movsx */
            const unsigned sw = (op2 & 1) ? 2 : 1;
            if (!rd(addr, sw, &mem)) {
                return false;
            }
            if (op2 == 0xBE) {
                mem = (uint32_t)(int32_t)(int8_t)mem;
            } else if (op2 == 0xBF) {
                mem = (uint32_t)(int32_t)(int16_t)mem;
            }
            set_reg(f, (q[2] >> 3) & 7, bytes, mem);
            len = pre + 2 + ml;
        } else if (op2 == 0xA3 || (op2 == 0xBA && ((q[2] >> 3) & 7) == 4)) { /* bt */
            const unsigned bit = op2 == 0xA3 ? get_reg(f, (q[2] >> 3) & 7, bytes) & (bytes * 8 - 1)
                                             : q[2 + ml] & (bytes * 8 - 1);
            if (!rd(addr, bytes, &mem)) {
                return false;
            }
            eflags = (eflags & ~RX_CF) | ((mem >> bit) & 1);
            nc_frame_set_eflags(f, eflags);
            len = pre + 2 + ml + (op2 == 0xBA ? 1 : 0);
        } else {
            /* SSE and MMX moves: a fast copy that meets device memory */
            bool p66 = false, f3 = false, f2 = false;
            for (unsigned i = 0; i < pre; i++) {
                p66 = p66 || code[i] == 0x66;
                f3 = f3 || code[i] == 0xF3;
                f2 = f2 || code[i] == 0xF2;
            }
            unsigned n = 0, off = 0;
            bool load = false, mmx = false, clear = false;
            switch (op2) {
            case 0x10: case 0x11: n = f3 ? 4 : f2 ? 8 : 16; load = !(op2 & 1); clear = load; break;     /* movups, movss, movsd */
            case 0x12: case 0x13: n = 8; load = !(op2 & 1); break;                                      /* movlps */
            case 0x16: case 0x17: n = 8; off = 8; load = !(op2 & 1); break;                             /* movhps */
            case 0x28: case 0x29: n = 16; load = op2 == 0x28; break;                                    /* movaps */
            case 0x2B: n = 16; break;                                                                   /* movntps */
            case 0x6E: n = 4; load = true; clear = true; mmx = !p66; break;                             /* movd */
            case 0x7E:
                if (f3) {                                                                               /* movq */
                    n = 8; load = true; clear = true;
                } else {                                                                                /* movd */
                    n = 4; mmx = !p66;
                }
                break;
            case 0x6F: case 0x7F:                                                                       /* movq, movdqa, movdqu */
                load = op2 == 0x6F;
                if (p66 || f3) {
                    n = 16;
                } else {
                    n = 8; mmx = true;
                }
                break;
            case 0xD6:                                                                                  /* movq */
                if (!p66) {
                    goto unknown;
                }
                n = 8;
                break;
            case 0xE7:                                                                                  /* movntq, movntdq */
                if (p66) {
                    n = 16;
                } else {
                    n = 8; mmx = true;
                }
                break;
            default:
                goto unknown;
            }
            extern uint8_t *rx_frame_fx(NcFrame *f);
            const unsigned r = (q[2] >> 3) & 7;
            uint8_t *reg = rx_frame_fx(f) + (mmx ? 32 : 160) + 16 * r;
            if (load) {
                uint8_t buf[16];
                for (unsigned i = 0; i < n; i += 4) {
                    uint32_t v;
                    if (!rd(addr + i, 4, &v)) {
                        return false;
                    }
                    memcpy(buf + i, &v, 4);
                }
                if (clear) {
                    memset(reg, 0, mmx ? 8 : 16);
                }
                memcpy(reg + off, buf, n);
            } else {
                for (unsigned i = 0; i < n; i += 4) {
                    uint32_t v;
                    memcpy(&v, reg + off + i, 4);
                    if (!wr(addr + i, 4, v)) {
                        return false;
                    }
                    if (nc_mmio_take_retry()) {
                        return true;
                    }
                }
            }
            len = pre + 2 + ml;
        }
        break;
    }
    default:
    unknown: {
        static unsigned told;
        if (told++ < 32) {
            nc_log("xog-mmio", "not emulated: eip %08x addr %08x bytes %02x %02x %02x %02x %02x %02x", eip, addr,
                   code[0], code[1], code[2], code[3], code[4], code[5]);
        }
        return false;
    }
    }
    nc_frame_set_eip(f, eip + len);
    return true;
}

/* The access against RAM, for the signal path and the gate alike. */
bool rx_ram_run(NcVm *vm, NcFrame *f, const uint8_t *code, uint32_t addr, void (*stored)(uint32_t pa, uint32_t n))
{
    rx_ram_hole_set = false;
    g_ram_vm = vm;
    g_ram_stored = stored;
    const bool ok = rx_mmio_emulate_at(f, code, addr);
    g_ram_vm = NULL;
    return ok;
}

bool rx_ram_is_string(const uint8_t *code)
{
    unsigned pre = 0;
    for (; pre < 14; pre++) {
        const uint8_t b = code[pre];
        if (!(b == 0x66 || b == 0xF3 || b == 0xF2 || b == 0x26 || b == 0x2E || b == 0x36 || b == 0x3E || b == 0xF0 || b == 0x64 || b == 0x65)) {
            break;
        }
    }
    return code[pre] >= 0xA4 && code[pre] <= 0xAF;
}

uint32_t rx_last_eip, rx_last_len;
uint8_t rx_last_code[15];

/* A guest access to its own RAM done here, the block it is in staying closed: the guest's
 * bytes beside a target. False: nothing done (no instruction this knows, or memory that
 * isn't mapped); the caller opens the block. */
bool rx_ram_emulate(NcVm *vm, void *ucontext, uint32_t addr, void (*stored)(uint32_t pa, uint32_t n))
{
    NcFrame *f = nc_cpu_fault_frame(g_cpu_for_ram(), ucontext);
    if (!f) {
        return false;
    }
    const uint32_t eip = nc_frame_eip(f);
    uint8_t code[15];
    for (unsigned i = 0; i < sizeof(code); i++) {
        const uint32_t pa = nc_vm_phys(vm, eip + i);
        if (pa == UINT32_MAX) {
            return false;
        }
        code[i] = *ram_byte(vm, eip + i);
    }
    /* The fault's address is where the access faulted, which for one crossing into a page isn't
     * where it starts. */
    {
        unsigned pre = 0;
        bool seg = false;
        for (; pre < 14; pre++) {
            const uint8_t b = code[pre];
            if (b == 0x64 || b == 0x65) {
                seg = true;
            } else if (!(b == 0x66 || b == 0xF3 || b == 0xF2 || b == 0x26 || b == 0x2E || b == 0x36 || b == 0x3E || b == 0xF0)) {
                break;
            }
        }
        const uint8_t op = code[pre], op2 = code[pre + 1];
        const bool strings = op >= 0xA4 && op <= 0xAF;
        const bool bit_op = op == 0x0F && (op2 == 0xA3 || op2 == 0xAB || op2 == 0xB3 || op2 == 0xBB);
        uint32_t ea = 0;
        if (!seg && !strings && !bit_op && rx_mmio_ea(f, code, &ea) && ea != addr) {
            static unsigned told;
            if (told++ < 16) {
                nc_log("xog-soft", "access at eip %08x starts at %08x, faulted at %08x", eip, ea, addr);
            }
            addr = ea;
        }
    }
    const bool ok = rx_ram_run(vm, f, code, addr, stored);
    if (ok) {
        /* what a site would need of it (rx_soft_site_note) */
        unsigned pre = 0;
        bool seg = false;
        for (; pre < 14; pre++) {
            const uint8_t b = code[pre];
            if (b == 0x64 || b == 0x65) {
                seg = true;
            } else if (!(b == 0x66 || b == 0xF3 || b == 0xF2 || b == 0x26 || b == 0x2E || b == 0x36 || b == 0x3E || b == 0xF0)) {
                break;
            }
        }
        const uint8_t op = code[pre], op2 = code[pre + 1];
        const bool strings = rx_ram_is_string(code);
        const bool bit_op = op == 0x0F && (op2 == 0xA3 || op2 == 0xAB || op2 == 0xB3 || op2 == 0xBB);
        rx_last_eip = eip;
        rx_last_len = seg || bit_op ? 0 : strings ? pre + 1 : (uint32_t)nc_frame_eip(f) - eip;
        memcpy(rx_last_code, code, sizeof(rx_last_code));
        nc_cpu_fault_resume(g_cpu_for_ram(), ucontext);
    }
    return ok;
}
