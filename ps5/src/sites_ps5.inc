/*
 * XOG: hot trapping instructions stop trapping. Included by cpu_ps5.c. Our own code.
 *
 * A signal costs 5 to 11 us on the console, the gate 0.06 (probe 2). An instruction in
 * the game's code that keeps trapping becomes a jump to a stub in the kernel area:
 *
 *   +0   far call to the gate
 *   +7   the instructions moved out of the jump's way, then a jump back
 *
 * The gate sees a return address in the stubs, does the instruction's work with the
 * guest's registers, and the guest goes on at +7. Only an instruction that has actually
 * trapped is ever touched, so data can't be mistaken for code; beyond device accesses
 * this takes rdtsc, cli, sti, pushfd and popfd as well.
 *
 * A jump takes 5 bytes. A shorter instruction moves what follows it into the stub, only
 * where nothing can land inside the moved bytes: no direct branch within 128 KiB, no
 * jump table entry nearby, no guest thread stopped there. Anything else stays a trap.
 */
#define RX_SITE_STUB 96u
#define RX_SITE_MAX ((NC_KERNEL_SIZE - 0x32000u - 0x1000u) / RX_SITE_STUB)
#define RX_SITE_VA(i) (NC_KERNEL_VA + 0x32000u + (i) * RX_SITE_STUB)
#define RX_SITE_HITS 8u
#define RX_HITS 8192u

enum { RX_S_MMIO = 1, RX_S_RDTSC, RX_S_CLI, RX_S_STI, RX_S_PUSHFD, RX_S_POPFD, RX_S_IN, RX_S_OUT, RX_S_SOFT };

typedef struct RxStub {
    uint32_t eip;
    uint8_t kind, len, total, dead;
    uint8_t orig[16];   /* the bytes the jump replaced */
    uint8_t insn[15];   /* the instruction itself, for device accesses */
    uint32_t copy_va;   /* a copy in the stub, for the kernel to decode: port I/O */
    uint16_t plain;     /* a soft site: operands in ordinary memory, in a row */
} RxStub;

typedef struct RxHit {
    uint32_t eip;
    uint16_t n;     /* 0xFFFF: it can't be rewritten, don't look again */
    uint16_t kills; /* a soft site given back this many times */
} RxHit;

static RxStub g_stubs[RX_SITE_MAX];
static unsigned g_nstubs;
static RxHit *g_hits;
static uint64_t g_gate_n, g_site_n;

extern bool rx_mmio_emulate_at(NcFrame *f, const uint8_t *code, uint32_t addr);
extern bool rx_mmio_ea(const NcFrame *f, const uint8_t *code, uint32_t *addr);

static bool in_image(const NcCpu *cpu, uint32_t a)
{
    return a >= cpu->image_lo && a < cpu->image_hi;
}

/* Any word in [lo, hi) on a stack, from its pointer to its top. Through the host's view,
 * which can't fault. */
static bool stack_holds(NcVm *vm, uint32_t esp, uint32_t top, uint32_t lo, uint32_t hi)
{
    const uint32_t end = (top > esp && top - esp <= 0x100000u) ? top : esp + 0x2000u;
    for (uint32_t a = esp & ~3u; a < end;) {
        const uint32_t n = NC_PAGE - (a & (NC_PAGE - 1));
        const uint32_t pa = nc_vm_phys(vm, a);
        if (pa != UINT32_MAX) {
            const uint8_t *p = vm->phys + pa;
            for (uint32_t o = 0; o + 4 <= n; o += 4) {
                const uint32_t w = nc_rd32(p + o);
                if (w >= lo && w < hi) {
                    return true;
                }
            }
        }
        if (a + n < a) {
            break;
        }
        a += n;
    }
    return false;
}

static const char *g_refuse;     /* why site_make said no */

static bool in_stubs(uint32_t a)
{
    return a >= RX_SITE_VA(0) && a < RX_SITE_VA(RX_SITE_MAX);
}

static bool code_mapped(uint32_t a)
{
    NcVm *vm = &g_core_for_faults->vm;
    return nc_vm_phys(vm, a) != UINT32_MAX && nc_vm_phys(vm, a + 15) != UINT32_MAX && g_va_soft[a >> 14] != 1 && g_va_soft[(a + 15) >> 14] != 1;
}

/* A decode from `at` to the known boundary `stop`. 1: it stopped on `stop` and passed over
 * `a`'s start. 0: on `stop`, but `a` was no boundary. -1: it can't tell. -2: it went past `stop`. */
static int code_walk(uint32_t at, uint32_t a, uint32_t stop)
{
    bool hit = false;
    for (int steps = 0; steps < 4096; steps++) {
        if (at > stop) {
            return -2;
        }
        hit = hit || at == a;
        if (at == stop) {
            return hit ? 1 : 0;
        }
        if (!code_mapped(at)) {
            return -1;
        }
        const unsigned l = insn_len((const uint8_t *)(uintptr_t)at);
        if (!l || l > 15) {
            return -1;
        }
        at += l;
    }
    return -1;
}

/*
 * Is a two-byte short branch found at `a` by the byte scan a real instruction? `eip` and
 * `end` are known boundaries: the rewritten range. It is false only when decoding from known
 * code passes over `a`; anything the decode can't vouch for counts as real, since a refused
 * site only costs a trap and a wrong rewrite costs a crash.
 */
static bool short_branch_real(uint32_t a, uint32_t eip, uint32_t end)
{
    if (a >= end) {
        return code_walk(end, a, a) != -2;
    }
    if (a >= eip) {
        return true;    /* inside the range itself */
    }
    /* From padding back of it, where functions start. A walk that reaches the site in step
     * is trustworthy; every one of them has to say no. */
    unsigned no = 0;
    for (uint32_t p = a; p > 2 && a - p < 0x1000u && no < 4; p--) {
        if (nc_vm_phys(&g_core_for_faults->vm, p - 2) == UINT32_MAX || g_va_soft[(p - 2) >> 14] == 1) {
            break;
        }
        const uint8_t x = *(const uint8_t *)(uintptr_t)(p - 2), y = *(const uint8_t *)(uintptr_t)(p - 1);
        if ((x == 0xCC || x == 0x90 || x == 0xC3) && (y == 0xCC || y == 0x90)) {
            if (code_walk(p, a, eip) != 0) {
                return true;
            }
            no++;
        }
    }
    return no == 0;
}

/* `bounds`: where the moved instructions start. Those are the only addresses in the range an
 * instruction could branch to; nothing can land in the middle of an instruction. The current
 * thread's own resume is the caller's to steer (site_note). */
static bool nothing_lands_inside(NcCpu *cpu, uint32_t eip, uint32_t total, const uint32_t *bounds, unsigned nb)
{
    if (!nb) {
        return true;
    }
    NcVm *vm = &g_core_for_faults->vm;
#define RX_IS_BOUND(v, out)                 \
    do {                                    \
        out = false;                        \
        for (unsigned bi = 0; bi < nb; bi++) { \
            out = out || (v) == bounds[bi]; \
        }                                   \
    } while (0)
    if (!in_stubs(eip)) {
        const uint32_t lo = eip > cpu->image_lo + 0x20000u ? eip - 0x20000u : cpu->image_lo;
        const uint32_t hi = eip + 0x20000u < cpu->image_hi ? eip + 0x20000u : cpu->image_hi;
        for (uint32_t a = lo; a + 6 < hi; a++) {
            /* a title unloads sections and pages out its own: nothing here can be taken as there.
             * From the scan's first byte, which needn't start a page, and up to the read's last. */
            if (a == lo || (a & (NC_PAGE - 1)) == 0) {
                if (nc_vm_phys(vm, a) == UINT32_MAX || g_va_soft[a >> 14] == 1) {
                    a |= NC_PAGE - 1;
                    continue;
                }
            }
            if ((a & (NC_PAGE - 1)) > NC_PAGE - 8 && (nc_vm_phys(vm, a + 7) == UINT32_MAX || g_va_soft[(a + 7) >> 14] == 1)) {
                continue;
            }
            const uint8_t *q = (const uint8_t *)(uintptr_t)a;
            bool hit;
            uint32_t t = 0;
            if (q[0] == 0xE8 || q[0] == 0xE9) {
                t = a + 5 + nc_rd32(q + 1);
            } else if (q[0] == 0x0F && q[1] >= 0x80 && q[1] <= 0x8F) {
                t = a + 6 + nc_rd32(q + 2);
            }
            RX_IS_BOUND(t, hit);
            if (hit) {
                g_refuse = "a branch lands inside";
                return false;
            }
            if (q[0] == 0xEB || (q[0] >= 0x70 && q[0] <= 0x7F)) {
                t = a + 2 + (uint32_t)(int32_t)(int8_t)q[1];
                RX_IS_BOUND(t, hit);
                if (hit && short_branch_real(a, eip, eip + total)) {
                    g_refuse = "a short branch lands inside";
                    return false;
                }
            }
            /* a jump table's entry */
            const uint32_t w = nc_rd32(q);
            RX_IS_BOUND(w, hit);
            if (hit) {
                g_refuse = "a table entry points inside";
                return false;
            }
        }
    }
#undef RX_IS_BOUND
    const NcKernel *k = cpu->cfg.opaque;
    for (unsigned n = 0; n < NC_MAX_THREADS; n++) {
        const NcThread *t = &k->threads[n];
        if (t->state != NC_T_FREE && t != k->current) {
            const uint32_t e = nc_context_eip(&t->ctx);
            if (e > eip && e < eip + total) {
                g_refuse = "a stopped thread is inside";
                return false;
            }
            if (stack_holds(vm, nc_context_gpr(&t->ctx, NC_ESP), t->stack_base, eip + 1, eip + total)) {
                g_refuse = "a thread's stack points inside";
                return false;
            }
        }
    }
    if (k->current && stack_holds(vm, (uint32_t)rx_frame.regs[NC_ESP], k->current->stack_base, eip + 1, eip + total)) {
        g_refuse = "the stack points inside";
        return false;
    }
    return true;
}

static bool site_make(NcCpu *cpu, uint32_t eip, int kind, unsigned len, const uint8_t *insn)
{
    if (g_nstubs >= RX_SITE_MAX) {
        g_refuse = "no stubs left";
        return false;
    }
    if (!in_image(cpu, eip) && !in_stubs(eip)) {
        g_refuse = "outside the image";
        return false;
    }
    uint32_t bounds[8];
    unsigned nb = 0;
    const uint32_t stub_va = RX_SITE_VA(g_nstubs);
    uint8_t *stub = (uint8_t *)(uintptr_t)stub_va;
    unsigned n = rx_gate_call(stub);
    uint32_t at = eip + len;
    unsigned total = len;
    bool terminal = false;
    /* moved instructions, whose targets stay where they were */
    while (total < 5 && !terminal) {
        if (!code_mapped(at)) {
            g_refuse = "what follows isn't mapped";
            return false;
        }
        const uint8_t *q = (const uint8_t *)(uintptr_t)at;
        const unsigned l = insn_len(q);
        if (!l || l > 15 || n + l + 10 > RX_SITE_STUB || nb >= 8 || (!in_image(cpu, at + l) && !in_stubs(at + l))) {
            g_refuse = "next instruction undecodable or too big";
            return false;
        }
        bounds[nb++] = at;
        if (q[0] == 0xE8 || q[0] == 0xCC || q[0] == 0xCD || q[0] == 0xF4 || q[0] == 0xCF || q[0] == 0x9A ||
            (q[0] == 0x0F && q[1] == 0x0B) || (q[0] == 0xFF && ((q[1] >> 3) & 7) == 2)) {
            g_refuse = "a call or trap follows";
            return false;   /* a call, a trap: its return or its report would name the stub */
        }
        if (q[0] == 0x9C || q[0] == 0x9D || q[0] == 0xFA || q[0] == 0xFB) {
            g_refuse = "a flag instruction follows";
            return false;   /* the flags: the kernel's to see, and no sweep looks in a stub */
        }
        if (q[0] >= 0xE0 && q[0] <= 0xE3) {
            g_refuse = "loop or jecxz follows";
            return false;   /* loop, jecxz: a short reach with no long form to rewrite it to */
        }
        for (unsigned i = 0; i < g_nstubs; i++) {
            if (!g_stubs[i].dead && at < g_stubs[i].eip + g_stubs[i].total && at + l > g_stubs[i].eip) {
                g_refuse = "another site follows";
                return false;
            }
        }
        if (q[0] == 0xE9 || q[0] == 0xEB) {
            const uint32_t t = at + l + (q[0] == 0xE9 ? nc_rd32(q + 1) : (uint32_t)(int32_t)(int8_t)q[1]);
            stub[n] = 0xE9;
            nc_wr32(stub + n + 1, t - (stub_va + n + 5));
            n += 5;
            terminal = true;
        } else if (q[0] >= 0x70 && q[0] <= 0x7F) {
            const uint32_t t = at + 2 + (uint32_t)(int32_t)(int8_t)q[1];
            stub[n] = 0x0F;
            stub[n + 1] = (uint8_t)(0x80 | (q[0] & 0xF));
            nc_wr32(stub + n + 2, t - (stub_va + n + 6));
            n += 6;
        } else if (q[0] == 0x0F && q[1] >= 0x80 && q[1] <= 0x8F) {
            const uint32_t t = at + 6 + nc_rd32(q + 2);
            stub[n] = 0x0F;
            stub[n + 1] = q[1];
            nc_wr32(stub + n + 2, t - (stub_va + n + 6));
            n += 6;
        } else {
            memcpy(stub + n, q, l);
            n += l;
            terminal = q[0] == 0xC3 || q[0] == 0xC2;
        }
        total += l;
        at += l;
    }
    /* After a ret or a jump, only padding may be overwritten. */
    while (total < 5) {
        const uint8_t b = *(const uint8_t *)(uintptr_t)at;
        if (b != 0xCC && b != 0x90) {
            g_refuse = "code right after a ret or jump";
            return false;
        }
        total++;
        at++;
    }
    if (total > sizeof(g_stubs[0].orig)) {
        g_refuse = "too long";
        return false;
    }
    if (!nothing_lands_inside(cpu, eip, total, bounds, nb)) {
        return false;
    }
    const unsigned code_end = n + (terminal ? 0 : 5);
    const bool wants_copy = kind == RX_S_IN || kind == RX_S_OUT;
    if (wants_copy && code_end + len + 5 > RX_SITE_STUB) {
        g_refuse = "no room for the copy";
        return false;
    }
    if (!terminal) {
        stub[n] = 0xE9;
        nc_wr32(stub + n + 1, (eip + total) - (stub_va + n + 5));
    }
    RxStub *s = &g_stubs[g_nstubs];
    memset(s, 0, sizeof(*s));
    if (wants_copy) {
        memcpy(stub + code_end, (const void *)(uintptr_t)eip, len);
        s->copy_va = stub_va + code_end;
        /* the kernel leaves the EIP just past the copy: from there, on through the stub */
        stub[code_end + len] = 0xE9;
        nc_wr32(stub + code_end + len + 1, (stub_va + 7) - (stub_va + code_end + len + 5));
    }
    s->eip = eip;
    s->kind = (uint8_t)kind;
    s->len = (uint8_t)len;
    s->total = (uint8_t)total;
    if (insn) {
        memcpy(s->insn, insn, sizeof(s->insn));
    }
    for (unsigned i = 0; i < total; i++) {
        uint8_t *w = code_byte(eip + i);
        if (!w) {
            return false;
        }
        s->orig[i] = *w;
    }
    uint8_t jump[16];
    memset(jump, 0x90, sizeof(jump));
    jump[0] = 0xE9;
    nc_wr32(jump + 1, stub_va - (eip + 5));
    for (unsigned i = 0; i < total; i++) {
        *code_byte(eip + i) = jump[i];
    }
    g_nstubs++;
    {
        char hex[64];
        int o = 0;
        for (unsigned b = 0; b < total; b++) {
            o += snprintf(hex + o, sizeof(hex) - (size_t)o, "%02x ", s->orig[b]);
        }
        nc_log("xog-site", "#%u %08x kind %d len %u total %u stub %08x back %08x | was %s| now %02x %02x %02x %02x %02x", g_nstubs - 1,
               eip, kind, len, total, stub_va, terminal ? 0 : eip + total, hex, *code_byte(eip), *code_byte(eip + 1),
               *code_byte(eip + 2), *code_byte(eip + 3), *code_byte(eip + 4));
    }
    return true;
}

/* The length of the live site that starts at this address, or 0. */
static unsigned site_skip(uint32_t at)
{
    for (unsigned i = 0; i < g_nstubs; i++) {
        if (g_stubs[i].eip == at && !g_stubs[i].dead) {
            return g_stubs[i].total;
        }
    }
    return 0;
}

/* An instruction trapped once more. `insn` points at its bytes for a device access.
 *
 * Nonzero: it was rewritten just now, and the bytes after it may have moved, so the guest
 * must not resume at eip + len. The value is where its neighbour went: right for a caller
 * whose work is done. A caller whose work isn't done resumes at eip instead, and the jump
 * brings it to the gate, which does it. */
static uint32_t site_note(NcCpu *cpu, uint32_t eip, int kind, unsigned len, const uint8_t *insn)
{
    if (!in_image(cpu, eip) && !in_stubs(eip)) {
        return 0;
    }
    if (!g_hits && !(g_hits = calloc(RX_HITS, sizeof(RxHit)))) {
        return 0;
    }
    unsigned h = (eip * 2654435761u) % RX_HITS;
    for (unsigned n = 0; n < RX_HITS; n++, h = (h + 1) % RX_HITS) {
        if (g_hits[h].eip == eip || !g_hits[h].eip) {
            break;
        }
    }
    RxHit *e = &g_hits[h];
    if (e->eip != eip) {
        if (e->eip) {
            return 0;
        }
        e->eip = eip;
    }
    if (e->n == 0xFFFF || ++e->n < (kind == RX_S_SOFT ? 512u : RX_SITE_HITS)) {
        return 0;
    }
    if (!site_make(cpu, eip, kind, len, insn)) {
        static unsigned told;
        if (told++ < 128) {
            nc_log("xog-cpu", "site %08x (kind %d) stays a trap: %s", eip, kind, g_refuse ? g_refuse : "?");
        }
        e->n = 0xFFFF;
        return 0;
    }
    e->n = 0;
    return RX_SITE_VA(g_nstubs - 1) + 7;    /* past the far call: see rx_gate_c */
}

/* The bytes put back: the code at [va, va + len) was replaced, or a site reached ordinary memory. */
static void site_kill(RxStub *s, bool restore)
{
    static unsigned told;
    if (told++ < 64) {
        nc_log("xog-site", "%08x off (%s)", s->eip, restore ? "ordinary memory" : "code replaced");
    }
    if (restore) {
        for (unsigned i = 0; i < s->total; i++) {
            uint8_t *w = code_byte(s->eip + i);
            if (w) {
                *w = s->orig[i];
            }
        }
    }
    s->dead = 1;
}

static void sites_replaced(uint32_t va, uint32_t len)
{
    /* other code there now: a site whose patch is gone is forgotten, the next sweep finds what's new. Only
     * those: a fixup or a mod in the section leaves the others patched, and they must stay known. The entry
     * stays with kind 0, so others behind it in the table are still found. */
    for (unsigned i = 0; g_sites && i < RX_SITES; i++) {
        const uint32_t at = g_sites[i].eip;
        if (at >= va && at - va < len && g_sites[i].kind) {
            NcVm *vm = &g_core_for_faults->vm;
            const uint8_t *q = (const uint8_t *)(uintptr_t)at;
            const bool mapped = nc_vm_phys(vm, at) != UINT32_MAX && nc_vm_phys(vm, at + 1) != UINT32_MAX;
            const bool patched = mapped && (g_sites[i].kind == RX_K_RDTSC ? q[0] == 0x0F && q[1] == 0x0B : q[0] == 0xCC);
            if (!patched) {
                g_sites[i].kind = 0;
                g_nsites--;
            }
        }
    }
    for (unsigned i = 0; i < g_nstubs; i++) {
        if (!g_stubs[i].dead && g_stubs[i].eip >= va && g_stubs[i].eip - va < len) {
            site_kill(&g_stubs[i], false);
        }
    }
}

/* A soft site's instruction back on the CPU; it may earn a site again, twice. */
static void site_give_back(RxStub *s)
{
    site_kill(s, true);
    if (!g_hits) {
        return;
    }
    unsigned h = (s->eip * 2654435761u) % RX_HITS;
    for (unsigned n = 0; n < RX_HITS; n++, h = (h + 1) % RX_HITS) {
        if (g_hits[h].eip == s->eip) {
            g_hits[h].n = ++g_hits[h].kills >= 3 ? 0xFFFF : 0;
            return;
        }
        if (!g_hits[h].eip) {
            return;
        }
    }
}

extern uint32_t rx_last_eip, rx_last_len;
extern uint8_t rx_last_code[15];
extern bool rx_ram_run(NcVm *vm, NcFrame *f, const uint8_t *code, uint32_t addr, void (*stored)(uint32_t pa, uint32_t n));
extern bool rx_ram_is_string(const uint8_t *code);
extern bool rx_ram_hole_set, rx_ram_hole_store;
extern uint32_t rx_ram_hole;
extern void rx_guest_stored(uint32_t pa, uint32_t n);
extern bool nc_vm_noaccess_has(NcVm *vm, uint32_t pa);
static bool rx_frame_page_fault(NcFrame *f, uint32_t va, bool store);
#define RX_SOFT_PLAIN 4096u

/* The last access emulated on a soft block came by a signal: often enough, a site. */
void rx_soft_site_note(void)
{
    if (!rx_last_len || rx_last_len > 15) {
        return;
    }
    const uint32_t r = site_note(g_cpu, rx_last_eip, RX_S_SOFT, rx_last_len, rx_last_code);
    if (r && (uint32_t)rx_frame.rip == rx_last_eip + rx_last_len) {
        rx_frame.rip = r;   /* the access is done; its neighbour may be in the stub now */
    }
}

/* The guest's flag word on its stack, as pushfd would write it. */
static void site_run(NcCpu *cpu, RxStub *s, uint32_t stub_after)
{
    NcFrame *f = &rx_frame;
    g_site_n++;
    f->rip = s->eip;
    switch (s->kind) {
    case RX_S_MMIO: {
        uint32_t addr = 0;
        if (!rx_mmio_ea(f, s->insn, &addr) || !rx_mmio_emulate_at(f, s->insn, addr)) {
            site_kill(s, true);     /* ordinary memory this time: it runs itself from now on */
            return;
        }
        if (f->rip != (uint64_t)s->eip + s->len) {
            return;                 /* the device wants it again: back through the jump */
        }
        break;
    }
    case RX_S_RDTSC: {
        const uint64_t t = (uint64_t)(((unsigned __int128)(rx_rdtsc() - cpu->cycle_offset) * g_tsc_ratio) >> 32);
        f->regs[NC_EAX] = (uint32_t)t;
        f->regs[NC_EDX] = (uint32_t)(t >> 32);
        break;
    }
    case RX_S_CLI:
        f->vif = 0;
        break;
    case RX_S_STI:
        f->vif = 1;
        break;
    case RX_S_PUSHFD: {
        const uint32_t esp = (uint32_t)f->regs[NC_ESP] - 4;
        nc_wr32((void *)(uintptr_t)esp, nc_frame_eflags(f));
        f->regs[NC_ESP] = esp;
        break;
    }
    case RX_S_POPFD: {
        const uint32_t esp = (uint32_t)f->regs[NC_ESP];
        nc_frame_set_eflags(f, nc_rd32((const void *)(uintptr_t)esp));
        f->regs[NC_ESP] = esp + 4;
        break;
    }
    case RX_S_SOFT: {
        NcVm *vm = &g_core_for_faults->vm;
        const bool strings = rx_ram_is_string(s->insn);
        uint32_t addr = 0;
        if (!strings) {
            if (!rx_mmio_ea(f, s->insn, &addr) || addr >= 0xFD000000u) {
                site_give_back(s);
                return;
            }
            const uint32_t pa = nc_vm_phys(vm, addr);
            if (pa != UINT32_MAX && pa < vm->ram_size && nc_vm_noaccess_has(vm, pa)) {
                /* a store to a report page goes on from here (hle_mem_report_store) */
                extern bool hle_mem_report_store(uint32_t pa);
                unsigned pre = 0;
                while (pre < 4 && (s->insn[pre] == 0x66 || s->insn[pre] == 0x2E || s->insn[pre] == 0x3E || s->insn[pre] == 0x26 ||
                                   s->insn[pre] == 0x36)) {
                    pre++;
                }
                const uint8_t op = s->insn[pre];
                const bool store = op == 0x88 || op == 0x89 || op == 0xC6 || op == 0xC7;
                if (!store || !hle_mem_report_store(pa)) {
                    site_give_back(s);  /* a guard's: the fault has work to do */
                    return;
                }
            }
            if (g_va_soft[addr >> 14] == 1) {
                s->plain = 0;
            } else if (pa != UINT32_MAX) {
                bool closed = false;
                for (uint32_t o = 0; o < 0x4000u && !closed; o += NC_PAGE) {
                    closed = nc_vm_noaccess_has(vm, (pa & ~0x3FFFu) + o);
                }
                if (closed) {
                    s->plain = 0;
                } else if (++s->plain > RX_SOFT_PLAIN) {
                    site_give_back(s);
                    return;
                }
            }
        }
        if (!rx_ram_run(vm, f, s->insn, addr, rx_guest_stored)) {
            if (rx_ram_hole_set) {
                if (!rx_frame_page_fault(f, rx_ram_hole, rx_ram_hole_store)) {
                    site_give_back(s);  /* nobody takes it: the CPU's own fault reports it */
                }
                return;
            }
            site_give_back(s);
            return;
        }
        if ((uint32_t)f->rip == s->eip) {
            return;     /* a rep not done: back through the jump */
        }
        break;
    }
    case RX_S_IN:
    case RX_S_OUT:
        f->rip = s->copy_va;
        if (!cpu->cfg.sysop(cpu->cfg.opaque, f, s->kind == RX_S_IN ? NC_SYSOP_IN : NC_SYSOP_OUT, s->len)) {
            char b[96];
            const int n = snprintf(b, sizeof(b), "xog: port I/O failed at %08x\n", s->eip);
            rx_log_raw(b, (size_t)n);
            _exit(3);
        }
        return;     /* the op may have switched threads: the frame's EIP is theirs now */
    default:
        break;
    }
    f->rip = stub_after;
}

/* ---- the gate's C side ---- */

volatile uint32_t rx_entry_tf;
volatile uint32_t rx_in_gate;   /* the gate's C code runs: the way back looks at the flag */

/* After the work: a bypass, pending interrupts. 1: off the guest. 2: straight back
 * (one step pending: the exit's look must not spin on it). 0: back, after a look. */
static int gate_after(NcCpu *cpu)
{
    bool step = false;
    if (finish_common(cpu, &step)) {
        __atomic_store_n(&rx_in_gate, 0, __ATOMIC_SEQ_CST);
        return 1;
    }
    rx_entry_tf = step ? RX_TF : 0;
    /* A request from here on sends a kick, and a kick on the way back rewinds to the look.
     * One made before this store is seen by that look, or by the step's trap. */
    __atomic_store_n(&rx_in_gate, 0, __ATOMIC_SEQ_CST);
    return step ? 2 : 0;
}

int rx_gate_c(void)
{
    NcCpu *cpu = g_cpu;
    NcFrame *f = &rx_frame;
    const uint32_t eip = (uint32_t)f->rip;
    __atomic_store_n(&rx_in_gate, 1, __ATOMIC_SEQ_CST);
    g_gate_n++;
    if (eip >= RX_SITE_VA(0) && eip < RX_SITE_VA(RX_SITE_MAX) && (eip - RX_SITE_VA(0)) % RX_SITE_STUB == 7) {
        RxStub *s = &g_stubs[(eip - RX_SITE_VA(0)) / RX_SITE_STUB];
        if (s->dead) {
            /* replaced code under a stale jump can't happen: its bytes went with it */
            f->rip = s->eip;
        } else {
            site_run(cpu, s, eip);
        }
    } else if (!cpu->cfg.sysop(cpu->cfg.opaque, f, NC_SYSOP_INT, 2)) {
        char b[96];
        const int n = snprintf(b, sizeof(b), "xog: a kernel call failed at %08x\n", eip);
        rx_log_raw(b, (size_t)n);
        _exit(3);
    }
    return gate_after(cpu);
}

int rx_gate_again(void)
{
    __atomic_store_n(&rx_in_gate, 1, __ATOMIC_SEQ_CST);
    return gate_after(g_cpu);
}
