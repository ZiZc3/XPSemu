#include "hw/xbox/hle/mem/pageuse.h"
#include "hle_test.h"

static void test_pageuse(void)
{
    HlePageUse p;
    HT_CHECK(hle_pageuse_init(&p, 1u << 20));       /* 256 pages */
    hle_pageuse_mark(&p, 0x1800, 0x2000, 7);        /* pages 1..3 */
    hle_pageuse_mark(&p, 0x3000, 0x10, 4);          /* page 3 keeps max 7 */
    HT_CHECK(hle_pageuse_get(&p, 0x0000, 0x1000) == 0);
    HT_CHECK(hle_pageuse_get(&p, 0x3FFC, 4) == 7);
    HT_CHECK(hle_pageuse_get(&p, 0x0000, 0x100000) == 7);
    HT_CHECK(hle_pageuse_get(&p, 0xFFFFF000, 0x1000) == 0);   /* outside */
    HT_CHECK(p.max == 7);
    hle_pageuse_fini(&p);
}

int main(void)
{
    HT_RUN(test_pageuse);
    HT_DONE();
}
