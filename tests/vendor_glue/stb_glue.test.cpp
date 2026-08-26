// stb_glue.h - proves stb_image decodes through pool_vendor and stb_sprintf formats correctly.
// Spec: docs/MEMORY.md §8.6, docs/PLATFORM.md §9.5. Rubric: docs/TESTING.md §7.
#include "runner/tl_test.h"
#include "vendor_glue/stb_glue.h"
#include "pool_vendor_test_api.h"

#include <string.h>

TL_TEST(stb_glue_snprintf_matches_expected_bytes, "vendor_glue,stb,smoke") {
    char buf[32];
    i32 needed = vendor_glue_stbsp_snprintf(buf, sizeof buf, "%d/%d=%s", 22, 7, "pi-ish");
    TL_EXPECT_TRUE(needed == (i32)strlen("22/7=pi-ish"));
    TL_EXPECT_TRUE(strcmp(buf, "22/7=pi-ish") == 0);

    // Truncation: buf_size too small still NUL-terminates and reports the FULL length needed.
    char small[4];
    i32 full_len = vendor_glue_stbsp_snprintf(small, sizeof small, "%s", "truncated");
    TL_EXPECT_TRUE(full_len == (i32)strlen("truncated"));
    TL_EXPECT_TRUE(strlen(small) == 3);
}

TL_TEST(stb_glue_decodes_a_1x1_bmp_through_pool_vendor, "vendor_glue,stb,smoke") {
    vendor_glue_test_ensure_pool_vendor(t);

    // A hand-built 1x1 24bpp uncompressed BMP: one pure-red pixel (BGR 00 00 FF), row padded to
    // a 4-byte boundary. stb_image supports BMP without any extra format library.
    static const u8 bmp[] = {
        'B', 'M',                          // magic
        58, 0, 0, 0,                       // file size
        0, 0, 0, 0,                        // reserved
        54, 0, 0, 0,                       // pixel data offset
        40, 0, 0, 0,                       // DIB header size (BITMAPINFOHEADER)
        1, 0, 0, 0,                        // width = 1
        1, 0, 0, 0,                        // height = 1
        1, 0,                              // planes
        24, 0,                             // bits per pixel
        0, 0, 0, 0,                        // compression = BI_RGB
        4, 0, 0, 0,                        // image size
        0, 0, 0, 0,                        // x ppm
        0, 0, 0, 0,                        // y ppm
        0, 0, 0, 0,                        // colours used
        0, 0, 0, 0,                        // important colours
        0x00, 0x00, 0xFF, 0x00,            // B, G, R, pad
    };
    static_assert(sizeof bmp == 58, "");

    u64 baseline = pool_stats(pool_vendor())->live_bytes;
    i32 w = 0, h = 0;
    u8* pixels = vendor_glue_stbi_load_from_memory(bmp, (i32)sizeof bmp, &w, &h);
    TL_ASSERT_TRUE(pixels != nullptr);
    // The decoded buffer must show up in pool_vendor's own accounting, not merely decode - the
    // default STBI_MALLOC would satisfy every assertion above this line identically
    // (docs/TESTING.md §7 "measure, don't assert").
    TL_EXPECT_TRUE(pool_stats(pool_vendor())->live_bytes > baseline);
    TL_EXPECT_TRUE(w == 1 && h == 1);
    TL_EXPECT_TRUE(pixels[0] == 0xFF && pixels[1] == 0x00 && pixels[2] == 0x00 && pixels[3] == 0xFF);
    vendor_glue_stbi_free(pixels);
    TL_EXPECT_EQ(pool_stats(pool_vendor())->live_bytes, baseline);
}
