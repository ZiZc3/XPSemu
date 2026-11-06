/*
 * XOG: the Xbox's 4 KiB pages on the PS5's 16 KiB ones. Included by core/vm.c.
 * Our own code. GPL-3.0-or-later.
 *
 * Physical memory is in two parts:
 *   [0, guest_size)  the title's 64 MiB: kernel and contiguous memory, exact to 4 KiB,
 *                    reached through the windows, whose addresses are 1:1 with it.
 *   [guest_size, ram_size)  backing for virtual memory, in whole 16 KiB blocks.
 *
 * A guest VA block (16 KiB) holds one physical block, VA and PA congruent, for as long
 * as any of its 4 KiB pages is committed. The parts not committed cost nothing the title
 * can see: it is charged page by page, up to guest_size in all, as on the console.
 * Keeping virtual memory out of the low part also means contiguous blocks never have
 * to move virtual pages out of their way.
 *
 * Protection works on host pages, so it follows the block: a block is closed if a page
 * in it has no access, read-only if a page in it is tracked, otherwise open. A store
 * anywhere in a read-only block is reported for every tracked page in it (dirty.c),
 * which can only ever over-report: no write goes unseen. Pages that hold data stay
 * without execute while read-only: read+execute is execute-only on the console, and a
 * read of that ends the title with no signal.
 */
#define RX_BLK 16384u
#define RX_BLK_PAGES (RX_BLK / NC_PAGE)
#define RX_P_NONE 0
#define RX_P_RO 1
#define RX_P_RW 3

static uint32_t *g_blk_of_va;   /* per VA block: its physical block + 1, or 0 */
static uint8_t *g_va_refs;      /* per VA block: 4 KiB pages mapped there */
static uint8_t *g_pblk_refs;    /* per physical block: VA blocks bound to it */
static uint8_t *g_pblk_prot;    /* per physical block: what the host has now */
static uint32_t g_next_blk;     /* where the search for a free backing block goes on */
static volatile int g_rx_lock;

static NcVm *g_soft_vm;
/* Per VA block: 0 ordinary, 1 soft (nothing mapped, each access emulated), 2 shadowed. */
uint8_t *g_va_soft;
#define RX_SHADOWS 4096u
typedef struct RxShadow {
    uint32_t vb, pblk, pa[RX_BLK_PAGES];
    uint64_t hash[RX_BLK_PAGES];    /* each physical page as the shadow last left it */
    bool code, used;
} RxShadow;
static RxShadow g_shadow[RX_SHADOWS];
static uint16_t *g_shadow_of;   /* per VA block: its shadow + 1 */
static uint16_t *g_soft_hits;   /* per VA block: accesses since g_soft_since */
static uint32_t *g_soft_since;  /* ... in ms */
static uint8_t *g_pa_table;     /* per page: a page table of the title's */
static uint32_t g_soft_blocks, g_shadow_n, g_shadow_code, g_shadow_refused;
uint32_t g_soft_emulated, g_soft_failed;
#define RX_SOFT_HOT 64u         /* accesses in 100 ms: a block worth a shadow */

static void rx_lock(void)
{
    while (__atomic_exchange_n(&g_rx_lock, 1, __ATOMIC_ACQUIRE)) {
        sched_yield();
    }
}

static void rx_unlock(void)
{
    __atomic_store_n(&g_rx_lock, 0, __ATOMIC_RELEASE);
}

static bool rx_blocks_init(NcVm *vm)
{
    const size_t vblocks = (1ull << 32) / RX_BLK, pblocks = vm->ram_size / RX_BLK;
    g_blk_of_va = calloc(vblocks, sizeof(uint32_t));
    g_va_refs = calloc(vblocks, 1);
    g_pblk_refs = calloc(pblocks, 1);
    g_pblk_prot = malloc(pblocks);
    if (!g_blk_of_va || !g_va_refs || !g_pblk_refs || !g_pblk_prot) {
        return false;
    }
    memset(g_pblk_prot, RX_P_RW, pblocks);
    g_next_blk = vm->guest_size / RX_BLK;
    g_va_soft = calloc(vblocks, 1);
    g_shadow_of = calloc(vblocks, sizeof(uint16_t));
    g_soft_hits = calloc(vblocks, sizeof(uint16_t));
    g_soft_since = calloc(vblocks, sizeof(uint32_t));
    g_pa_table = calloc(vm->ram_size / NC_PAGE, 1);
    if (!g_va_soft || !g_shadow_of || !g_soft_hits || !g_soft_since || !g_pa_table) {
        return false;
    }
    return true;
}

/* What physical block b should be, from its pages' flags. */
static int rx_block_wants(const NcVm *vm, uint32_t b)
{
    int want = RX_P_RW;
    const uint32_t first = b * RX_BLK_PAGES;
    for (uint32_t i = 0; i < RX_BLK_PAGES; i++) {
        if (g_na && __atomic_load_n(&g_na[first + i], __ATOMIC_ACQUIRE)) {
            return RX_P_NONE;
        }
        if (__atomic_load_n(&vm->wp[first + i], __ATOMIC_ACQUIRE) == NC_WP_CLEAN) {
            want = RX_P_RO;
        }
    }
    return want;
}

/* VA blocks bound to physical block b, through the reverse map's pages. */
static void rx_protect_aliases(const NcVm *vm, uint32_t b, int prot)
{
    uint32_t done[RX_BLK_PAGES];
    unsigned nd = 0;
    const uint32_t pa = b * RX_BLK;
    for (uint32_t i = 0; i < RX_BLK_PAGES; i++) {
        const uint32_t va = vm->rmap[b * RX_BLK_PAGES + i];
        if (!va) {
            continue;
        }
        const uint32_t vb = va / RX_BLK;
        if (g_blk_of_va[vb] != pa + 1) {
            continue;
        }
        bool seen = false;
        for (unsigned k = 0; k < nd; k++) {
            seen = seen || done[k] == vb;
        }
        if (!seen) {
            done[nd++] = vb;
            rx_protect((void *)(uintptr_t)(vb * RX_BLK), RX_BLK, prot);
        }
    }
}

/* The host's protection brought up to the flags for physical blocks [b0, b1), with one
 * call per window for each run of blocks that change the same way. */
static void rx_apply(NcVm *vm, uint32_t b0, uint32_t b1)
{
    rx_lock();
    for (uint32_t b = b0; b < b1;) {
        const int want = rx_block_wants(vm, b);
        if (g_pblk_prot[b] == want) {
            b++;
            continue;
        }
        uint32_t e = b + 1;
        while (e < b1 && g_pblk_prot[e] != rx_block_wants(vm, e) && rx_block_wants(vm, e) == want) {
            e++;
        }
        const uint32_t pa = b * RX_BLK;
        const size_t len = (size_t)(e - b) * RX_BLK;
        const int r1 = rx_protect((void *)(uintptr_t)(NC_CONTIG_WINDOW + pa), len, want);
        const int r2 = rx_protect((void *)(uintptr_t)(NC_GPU_WINDOW + pa), len, want);
        if (r1 || r2) {
            /* The console refuses once its memory map is too split up. Unseen, the title's
             * texture updates would never be noticed: these pages count as always changed. */
            static unsigned told;
            if (told++ < 16) {
                nc_log("xog-vm", "protect %08x+%zx refused: %08x %08x; those pages count as changed", pa, len,
                       (unsigned)r1, (unsigned)r2);
            }
            for (uint32_t i = b * RX_BLK_PAGES; i < e * RX_BLK_PAGES; i++) {
                __atomic_store_n(&vm->wp[i], 0, __ATOMIC_RELEASE);
            }
            rx_protect((void *)(uintptr_t)(NC_CONTIG_WINDOW + pa), len, RX_P_RW);
            rx_protect((void *)(uintptr_t)(NC_GPU_WINDOW + pa), len, RX_P_RW);
            b = e;
            continue;
        }
        for (uint32_t i = b; i < e; i++) {
            rx_protect_aliases(vm, i, want);
            g_pblk_prot[i] = (uint8_t)want;
        }
        b = e;
    }
    rx_unlock();
}

static void rx_protect_pages(NcVm *vm, uint32_t pa, uint32_t n, bool ro)
{
    if (!n) {
        return;
    }
    const uint32_t first = pa / NC_PAGE, last = first + n;
    const uint32_t b0 = pa / RX_BLK, b1 = (pa + n * NC_PAGE - 1) / RX_BLK + 1;
    /* All of a block or none of it. A block with a page nobody asks to watch stays open and
     * its pages count as written: the HLE compares the bytes at a span's ends (pbdraw.c), so
     * sharing a block with data the game writes costs neither a fault per frame nor a false
     * upload. Marked before it closes, cleared before it opens: a store that faults meanwhile
     * on another thread is put right by the fault handler, which applies the flags. */
    for (uint32_t b = b0; b < b1; b++) {
        const uint32_t s = b * RX_BLK_PAGES;
        if (ro) {
            bool whole = true;
            for (uint32_t i = s; i < s + RX_BLK_PAGES; i++) {
                if ((i < first || i >= last) && __atomic_load_n(&vm->wp[i], __ATOMIC_ACQUIRE) != NC_WP_CLEAN) {
                    whole = false;
                }
            }
            if (!whole) {
                continue;
            }
        }
        for (uint32_t i = s; i < s + RX_BLK_PAGES; i++) {
            __atomic_store_n(&vm->wp[i], ro ? NC_WP_CLEAN : 0, __ATOMIC_RELEASE);
        }
    }
    rx_apply(vm, b0, b1);
}

static void rx_noaccess(NcVm *vm, uint32_t pa, uint32_t n, bool on)
{
    const uint32_t page = pa / NC_PAGE;
    for (uint32_t i = 0; i < n; i++) {
        __atomic_store_n(&g_na[page + i], on ? 1 : 0, __ATOMIC_RELEASE);
    }
    rx_apply(vm, pa / RX_BLK, (pa + n * NC_PAGE - 1) / RX_BLK + 1);
}

/* dirty.c's fault: bring block b's flags and the host together. */
void rx_vm_block_apply(NcVm *vm, uint32_t pa)
{
    rx_apply(vm, pa / RX_BLK, pa / RX_BLK + 1);
}

/* VA block vb onto physical block pblk. */
static bool rx_bind(NcVm *vm, uint32_t vb, uint32_t pblk)
{
    const uint32_t b = pblk / RX_BLK;
    if (!rx_map(vb * RX_BLK, pblk, RX_BLK)) {
        return false;
    }
    rx_lock();
    g_blk_of_va[vb] = pblk + 1;
    g_pblk_refs[b]++;
    if (g_pblk_prot[b] != RX_P_RW) {
        rx_protect((void *)(uintptr_t)(vb * RX_BLK), RX_BLK, g_pblk_prot[b]);
    }
    rx_unlock();
    return true;
}

static uint32_t rx_free_block(const NcVm *vm);

static uint64_t rx_page_hash(const uint8_t *p)
{
    uint64_t h = 0xCBF29CE484222325ull;
    for (unsigned o = 0; o < NC_PAGE; o += 8) {
        uint64_t w;
        memcpy(&w, p + o, 8);
        h = (h ^ w) * 0x100000001B3ull;
    }
    return h;
}

static uint32_t g_wb_kept, g_wb_tables;
#define RX_LAST 16u
static uint32_t g_last_wb[RX_LAST], g_last_wb_at;   /* physical pages written back last */

/* One shadowed page into its physical page, unless that page moved on. */
static bool g_wb_tell;
extern void hle_host_mem_set_dirty(uint32_t pa, uint32_t len);

static void rx_shadow_put(NcVm *vm, RxShadow *s, unsigned n)
{
    const uint32_t pa = s->pa[n];
    uint8_t *to = vm->phys + pa;
    const uint8_t *from = vm->phys + s->pblk + n * NC_PAGE;
    if (g_pa_table[pa / NC_PAGE]) {
        g_wb_tables++;
        return;
    }
    if (rx_page_hash(to) != s->hash[n]) {
        static unsigned told;
        if (told++ < 32) {
            nc_log("xog-soft", "pa %08x changed behind block %08x's shadow: it keeps what it has", pa, s->vb * RX_BLK);
        }
        g_wb_kept++;
        return;
    }
    if (memcmp(to, from, NC_PAGE)) {
        memcpy(to, from, NC_PAGE);
        s->hash[n] = rx_page_hash(to);
        if (g_wb_tell) {
            hle_host_mem_set_dirty(pa, NC_PAGE);    /* a save: the GPU's writeback must see it */
        }
        g_last_wb[g_last_wb_at++ % RX_LAST] = pa;
        nc_vm_protect_page(vm, pa, false);
    }
}

/* The shadow's contents back in the physical pages, and the host's page gone. */
static void rx_shadow_drop(NcVm *vm, uint32_t vb)
{
    const unsigned i = g_shadow_of[vb];
    if (!i) {
        return;
    }
    RxShadow *s = &g_shadow[i - 1];
    for (unsigned n = 0; n < RX_BLK_PAGES; n++) {
        rx_shadow_put(vm, s, n);
    }
    rx_unmap(vb * RX_BLK, RX_BLK);
    g_pblk_refs[s->pblk / RX_BLK]--;
    g_shadow_of[vb] = 0;
    g_shadow_n--;
    g_shadow_code -= s->code;
    s->used = false;
    g_va_soft[vb] = 1;
}

/* The physical address behind a VA in a block bound whole, its page mapped or not; or UINT32_MAX. */
uint32_t rx_va_bound_pa(uint32_t va)
{
    const uint32_t bound = g_blk_of_va[va / RX_BLK];
    return bound ? bound - 1 + (va & (RX_BLK - 1)) : UINT32_MAX;
}

bool rx_vm_is_table(uint32_t pa)
{
    return g_pa_table && g_soft_vm && pa < g_soft_vm->ram_size && g_pa_table[pa / NC_PAGE];
}

/* A block of its own for a soft block. False: it can't have one, and stays soft. */
bool rx_shadow_make(NcVm *vm, uint32_t va, bool code)
{
    const uint32_t vb = va / RX_BLK;
    static int off = -1;
    if (off < 0) {
        off = access("/data/xpsemu/noshadow", F_OK) == 0;
        nc_log("xog-soft", "shadows %s", off ? "off (/data/xpsemu/noshadow)" : "on");
    }
    if (off) {
        return false;
    }
    if (g_va_soft[vb] != 1) {
        return g_va_soft[vb] == 2;
    }
    uint32_t pa[RX_BLK_PAGES];
    for (unsigned n = 0; n < RX_BLK_PAGES; n++) {
        const uint32_t pv = vb * RX_BLK + n * NC_PAGE, p = nc_vm_phys(vm, pv);
        if (p == UINT32_MAX) {
            return false;   /* a hole must fault: it may yet fill */
        }
        if (g_pa_table[p / NC_PAGE] || vm->rmap[p / NC_PAGE] != pv) {
            g_shadow_refused++;
            return false;   /* a table must stay readable, an alias coherent */
        }
        pa[n] = p;
    }
    unsigned i = 0;
    while (i < RX_SHADOWS && g_shadow[i].used) {
        i++;
    }
    const uint32_t pblk = i < RX_SHADOWS ? rx_free_block(vm) : UINT32_MAX;
    if (pblk == UINT32_MAX) {
        g_shadow_refused++;
        return false;
    }
    for (unsigned n = 0; n < RX_BLK_PAGES; n++) {
        memcpy(vm->phys + pblk + n * NC_PAGE, vm->phys + pa[n], NC_PAGE);
    }
    if (!rx_map(vb * RX_BLK, pblk, RX_BLK)) {
        g_shadow_refused++;
        return false;
    }
    RxShadow *s = &g_shadow[i];
    s->vb = vb;
    s->pblk = pblk;
    memcpy(s->pa, pa, sizeof(pa));
    s->code = code;
    s->used = true;
    for (unsigned n = 0; n < RX_BLK_PAGES; n++) {
        s->hash[n] = rx_page_hash(vm->phys + pa[n]);
    }
    g_pblk_refs[pblk / RX_BLK]++;
    g_shadow_of[vb] = (uint16_t)(i + 1);
    g_shadow_n++;
    g_shadow_code += code;
    g_va_soft[vb] = 2;
    static unsigned told;
    if (told++ < 64) {
        nc_log("xog-soft", "shadow for block %08x: %s, pages at %08x %08x %08x %08x", vb * RX_BLK, code ? "code" : "data", pa[0], pa[1],
               pa[2], pa[3]);
    }
    return true;
}

/* ---- one instruction, for real, in a scratch block ---- */
#define RX_STEPS 4u             /* blocks one instruction can touch: code, two operands, a stack */
static struct {
    uint32_t vb, pblk;
    bool used;
} g_step[RX_STEPS];
volatile uint32_t g_steps_n;
uint32_t g_steps_done;

bool rx_step_active(uint32_t va)
{
    for (unsigned i = 0; g_steps_n && i < RX_STEPS; i++) {
        if (g_step[i].used && g_step[i].vb == va / RX_BLK) {
            return true;
        }
    }
    return false;
}

bool rx_step_begin(NcVm *vm, uint32_t va)
{
    const uint32_t vb = va / RX_BLK;
    unsigned i = 0;
    while (i < RX_STEPS && g_step[i].used) {
        if (g_step[i].vb == vb) {
            return false;   /* mapped and it still faulted: this can't help */
        }
        i++;
    }
    if (i == RX_STEPS) {
        return false;
    }
    if (!g_step[i].pblk) {
        const uint32_t pblk = rx_free_block(vm);
        if (pblk == UINT32_MAX) {
            return false;
        }
        g_step[i].pblk = pblk;
        g_pblk_refs[pblk / RX_BLK]++;
    }
    for (unsigned n = 0; n < RX_BLK_PAGES; n++) {
        const uint32_t p = nc_vm_phys(vm, vb * RX_BLK + n * NC_PAGE);
        uint8_t *to = vm->phys + g_step[i].pblk + n * NC_PAGE;
        if (p == UINT32_MAX) {
            memset(to, 0, NC_PAGE);
        } else {
            memcpy(to, vm->phys + p, NC_PAGE);
        }
    }
    if (!rx_map(vb * RX_BLK, g_step[i].pblk, RX_BLK)) {
        return false;
    }
    g_step[i].vb = vb;
    g_step[i].used = true;
    g_steps_n++;
    g_steps_done++;
    return true;
}

/* What the instruction changed goes to the pages, seen as a store; the scratch goes. */
void rx_steps_end(NcVm *vm)
{
    for (unsigned i = 0; g_steps_n && i < RX_STEPS; i++) {
        if (!g_step[i].used) {
            continue;
        }
        const uint32_t vb = g_step[i].vb;
        for (unsigned n = 0; n < RX_BLK_PAGES; n++) {
            const uint32_t p = nc_vm_phys(vm, vb * RX_BLK + n * NC_PAGE);
            const uint8_t *from = vm->phys + g_step[i].pblk + n * NC_PAGE;
            if (p != UINT32_MAX && memcmp(vm->phys + p, from, NC_PAGE)) {
                memcpy(vm->phys + p, from, NC_PAGE);
                nc_vm_protect_page(vm, p, false);
            }
            if (p == UINT32_MAX) {
                /* a store into a page the title hasn't mapped: lost, and it shouldn't have run */
                for (unsigned o = 0; o < NC_PAGE; o += 8) {
                    uint64_t w;
                    memcpy(&w, from + o, 8);
                    if (w) {
                        static unsigned told;
                        if (told++ < 16) {
                            nc_log("xog-soft", "a stepped instruction wrote into hole %08x", vb * RX_BLK + n * NC_PAGE + o);
                        }
                        break;
                    }
                }
            }
        }
        rx_unmap(vb * RX_BLK, RX_BLK);
        g_step[i].used = false;
        g_steps_n--;
    }
}

/* An access to a soft block. True: it comes often enough for a shadow. */
bool rx_soft_hot(uint32_t va, uint32_t now_ms)
{
    const uint32_t vb = va / RX_BLK;
    if (now_ms - g_soft_since[vb] > 100) {
        g_soft_since[vb] = now_ms;
        g_soft_hits[vb] = 0;
    }
    return ++g_soft_hits[vb] > RX_SOFT_HOT;
}

/* Data shadows into their physical pages, some at each flip, in turn. What changed counts as
 * written. Scattered pages aren't memory the GPU can use whole, so this is for what little
 * reads them by physical address, and can't cost a heap's worth of comparing a frame. */
/* A save: every shadow's bytes in the physical pages, which is what a state holds. */
void rx_shadow_flush_all(void)
{
    NcVm *vm = g_soft_vm;
    if (!vm) {
        return;
    }
    g_wb_tell = true;
    for (unsigned i = 0; i < RX_SHADOWS; i++) {
        if (g_shadow[i].used) {
            for (unsigned n = 0; n < RX_BLK_PAGES; n++) {
                rx_shadow_put(vm, &g_shadow[i], n);
            }
        }
    }
    g_wb_tell = false;
}

uint8_t *rx_pa_table_map(void)
{
    return g_pa_table;
}

void rx_shadow_sync(void)
{
    if (!g_shadow_n || g_shadow_n == g_shadow_code) {
        return;
    }
    NcVm *vm = g_soft_vm;
    static unsigned at;
    unsigned done = 0;
    for (unsigned looked = 0; looked < RX_SHADOWS && done < 32; looked++) {
        const RxShadow *s = &g_shadow[at];
        at = (at + 1) % RX_SHADOWS;
        if (!s->used || s->code) {
            continue;
        }
        done++;
        for (unsigned n = 0; n < RX_BLK_PAGES; n++) {
            rx_shadow_put(vm, &g_shadow[(unsigned)(s - g_shadow)], n);
        }
    }
}

/* At a stop: what a physical page has to do with the shadows. */
void rx_soft_dump_pa(uint32_t pa)
{
    NcVm *vm = g_soft_vm;
    if (!vm || pa >= vm->ram_size) {
        return;
    }
    nc_log("xog-stop", "pa %08x: table %u, mapped at %08x; write-backs kept off %u, refused for tables %u", pa, g_pa_table[pa / NC_PAGE],
           vm->rmap[pa / NC_PAGE], g_wb_kept, g_wb_tables);
    for (unsigned i = 0; i < RX_SHADOWS; i++) {
        for (unsigned n = 0; g_shadow[i].used && n < RX_BLK_PAGES; n++) {
            if (g_shadow[i].pa[n] == (pa & ~(NC_PAGE - 1))) {
                nc_log("xog-stop", "pa %08x is in the live shadow of block %08x", pa, g_shadow[i].vb * RX_BLK);
            }
        }
    }
    for (unsigned i = 0; i < RX_LAST; i++) {
        if (g_last_wb[i] == (pa & ~(NC_PAGE - 1))) {
            nc_log("xog-stop", "pa %08x was among the last pages written back", pa);
        }
    }
}

void rx_vm_mark_table(uint32_t pa)
{
    if (g_pa_table && g_soft_vm && pa < g_soft_vm->ram_size) {
        g_pa_table[pa / NC_PAGE] = 1;
        const uint32_t va = g_soft_vm->rmap[pa / NC_PAGE];
        if (va && g_va_soft[va / RX_BLK] == 2) {
            rx_shadow_drop(g_soft_vm, va / RX_BLK);
        }
    }
}

void nc_soft_report(void)
{
    if (g_soft_blocks || g_soft_emulated || g_soft_failed) {
        extern uint32_t g_soft_filled;
        nc_log("xog-soft", "%u soft blocks; shadows %u, %u of them code, %u refused; accesses emulated %u, stepped %u, failed %u; pages filled %u", g_soft_blocks,
               g_shadow_n, g_shadow_code, g_shadow_refused, g_soft_emulated, g_steps_done, g_soft_failed, g_soft_filled);
        g_soft_emulated = g_steps_done = g_shadow_refused = g_soft_filled = 0;
    }
}

/* [va, va + len) onto [pa, ...): the block each page lies in, bound if it isn't yet. */
static bool rx_map_pages(NcVm *vm, uint32_t va, uint32_t pa, uint32_t len)
{
    g_soft_vm = vm;
    for (uint32_t off = 0; off < len; off += NC_PAGE) {
        const uint32_t v = va + off, p = pa + off, vb = v / RX_BLK;
        const uint32_t pblk = p & ~(RX_BLK - 1);
        if (g_va_soft[vb] || p < vm->guest_size || ((v ^ p) & (RX_BLK - 1)) ||
            (g_blk_of_va[vb] && g_blk_of_va[vb] != pblk + 1)) {
            /* The console can't say this one: the block goes soft, all of it. */
            if (g_va_soft[vb] == 2) {
                rx_shadow_drop(vm, vb);
            }
            const uint32_t bound = g_blk_of_va[vb];
            if (bound) {
                rx_lock();
                g_blk_of_va[vb] = 0;
                g_pblk_refs[(bound - 1) / RX_BLK]--;
                rx_unlock();
                rx_unmap(vb * RX_BLK, RX_BLK);
            }
            if (!g_va_soft[vb]) {
                g_va_soft[vb] = 1;
                g_soft_blocks++;
                static unsigned told;
                if (told++ < 32) {
                    nc_log("xog-soft", "map va %08x pa %08x: block goes soft", v, p);
                }
            }
            if (!(vm->pte[v / NC_PAGE] & NC_PTE_VALID)) {
                g_va_refs[vb]++;
            }
            continue;
        }
        if (!g_blk_of_va[vb] && !rx_bind(vm, vb, pblk)) {
            return false;
        }
        if (!(vm->pte[v / NC_PAGE] & NC_PTE_VALID)) {
            g_va_refs[vb]++;
        }
    }
    return true;
}

/* A page left the guest's address space; the last in its block takes the host page. */
static void rx_page_unmapped(uint32_t va)
{
    const uint32_t vb = va / RX_BLK;
    if (g_va_soft[vb] == 2) {
        rx_shadow_drop(g_soft_vm, vb);  /* a hole now: it must fault */
    }
    if (!g_va_refs[vb] || --g_va_refs[vb]) {
        return;
    }
    if (g_va_soft[vb]) {
        g_va_soft[vb] = 0;
        g_soft_blocks--;
    }
    const uint32_t bound = g_blk_of_va[vb];
    if (bound) {
        rx_lock();
        g_blk_of_va[vb] = 0;
        g_pblk_refs[(bound - 1) / RX_BLK]--;
        rx_unlock();
        rx_unmap(vb * RX_BLK, RX_BLK);
    }
}

/* A backing block nothing is bound to. */
static uint32_t rx_free_block(const NcVm *vm)
{
    const uint32_t lo = vm->guest_size / RX_BLK, hi = vm->ram_size / RX_BLK;
    for (uint32_t n = 0; n < hi - lo; n++) {
        const uint32_t b = g_next_blk >= hi ? lo : g_next_blk;
        g_next_blk = b + 1;
        if (!g_pblk_refs[b]) {
            bool clear = true;
            for (uint32_t i = 0; i < RX_BLK_PAGES && clear; i++) {
                clear = !vm->page_used[b * RX_BLK_PAGES + i];
            }
            if (clear) {
                return b * RX_BLK;
            }
        }
    }
    return UINT32_MAX;
}

static uint32_t rx_free_pages(const NcVm *vm)
{
    uint32_t used = 0;
    for (uint32_t p = 0; p < vm->ram_size / NC_PAGE; p++) {
        used += vm->page_used[p] != 0;
    }
    const uint32_t all = vm->guest_size / NC_PAGE;
    return used < all ? all - used : 0;
}

static bool rx_alloc_at(NcVm *vm, uint32_t va, uint32_t len, uint32_t *pa_out)
{
    len = nc_align_up(len, NC_PAGE);
    if (rx_free_pages(vm) < len / NC_PAGE) {
        nc_log("hakuX-native", "vm: the title's %u MiB are used up: %u KiB asked at %08x, %u KiB free",
               vm->guest_size >> 20, len >> 10, va, rx_free_pages(vm) * (NC_PAGE >> 10));
        return false;
    }
    for (uint32_t off = 0; off < len; off += NC_PAGE) {
        const uint32_t v = va + off, vb = v / RX_BLK;
        uint32_t pblk = g_blk_of_va[vb] ? g_blk_of_va[vb] - 1 : rx_free_block(vm);
        uint32_t pa = pblk == UINT32_MAX ? UINT32_MAX : pblk + (v & (RX_BLK - 1));
        if (pa != UINT32_MAX && vm->page_used[pa / NC_PAGE]) {
            pa = UINT32_MAX;
        }
        if (pa == UINT32_MAX || !nc_vm_map(vm, v, pa, NC_PAGE)) {
            nc_log("hakuX-native", "vm: no backing page for %08x", v);
            for (uint32_t back = 0; back < off; back += NC_PAGE) {
                const uint32_t got = nc_vm_phys(vm, va + back);
                if (got != UINT32_MAX) {
                    nc_vm_unmap(vm, va + back, NC_PAGE);
                    nc_vm_free_phys(vm, got & ~(NC_PAGE - 1), NC_PAGE);
                }
            }
            return false;
        }
        vm->page_used[pa / NC_PAGE] = 1;
        memset(vm->phys + pa, 0, NC_PAGE);
        if (pa_out && off == 0) {
            *pa_out = pa;
        }
    }
    return true;
}
