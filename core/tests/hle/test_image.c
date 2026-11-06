#include "hw/xbox/hle/core/image.h"
#include "hle_test.h"

/* Library ranges come from section headers: clamp them, reject garbage. */
static void test_section_end(void)
{
    uint32_t end = 0;
    HT_CHECK(hle_img_section_end(0x10000, 0x20000, 0x11000, 0x1000, &end));
    HT_EQ_U32(end, 0x12000);
    HT_CHECK(hle_img_section_end(0x10000, 0x20000, 0x2F000, 0x5000, &end));
    HT_EQ_U32(end, 0x30000);                                /* clamped to image */
    HT_CHECK(hle_img_section_end(0x10000, 0x20000, 0x11000, 0, &end));
    HT_EQ_U32(end, 0x11000);                                /* empty */
    HT_CHECK(!hle_img_section_end(0x10000, 0x20000, 0xF000, 0x100, &end));
    HT_CHECK(!hle_img_section_end(0x10000, 0x20000, 0x30000, 0x100, &end));
    /* va + vsize wraps: the old lib_hi = va + vsize became a tiny value */
    HT_CHECK(!hle_img_section_end(0x10000, 0x20000, 0x11000, 0xFFFFF000, &end));
    /* image reaching the top of the address space */
    HT_CHECK(hle_img_section_end(0xFFFF0000, 0x10000, 0xFFFF8000, 0x1000, &end));
    HT_EQ_U32(end, 0xFFFF9000);
    HT_CHECK(!hle_img_section_end(0xFFFF0000, 0x10000, 0xFFFF8000, 0x8000, &end));
}

int main(void)
{
    HT_RUN(test_section_end);
    HT_DONE();
}
