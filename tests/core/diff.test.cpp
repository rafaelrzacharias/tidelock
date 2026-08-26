// diff.cpp - the desync field diff: names the differing field and element, raw bits on both
// sides, honest overflow count. Spec: docs/ECS.md §6 consumer 3; docs/DETERMINISM.md §7.
#include "runner/tl_test.h"
#include "core/reflect.h"

#define TL_FIELDS_DiffRow(X, XA, XH) \
    X(i32, hp) X(u32, tag) XA(u8, mask, 4) XH(Entity, who)
TL_COMPONENT(DiffRow)

TL_TEST(diff_names_fields_elements_and_bits, "core,ecs,reflect,fast") {
    DiffRow a;
    DiffRow b;
    memset(&a, 0, sizeof(a));
    memset(&b, 0, sizeof(b));
    a.hp = -5;
    b.hp = 7;
    a.tag = b.tag = 0x99u;
    a.mask[1] = 0x40u;
    b.mask[1] = 0x41u;
    b.mask[3] = 0xFFu;
    a.who = handle_make<Entity>(3u, 1u);
    b.who = handle_make<Entity>(3u, 2u);

    DiffLine lines[8];
    const u32 n = reflect_diff_rows(&DiffRow_info, &a, &b, lines, 8u);
    TL_ASSERT_EQ(n, 4u);   // hp, mask[1], mask[3], who
    TL_EXPECT_EQ(lines[0].field->name_hash, "hp"_id);
    TL_EXPECT_EQ(lines[0].a_bits, (u64)(u32)-5);
    TL_EXPECT_EQ(lines[0].b_bits, 7u);
    TL_EXPECT_EQ(lines[1].field->name_hash, "mask"_id);
    TL_EXPECT_EQ(lines[1].element, 1u);
    TL_EXPECT_EQ(lines[1].a_bits, 0x40u);
    TL_EXPECT_EQ(lines[1].b_bits, 0x41u);
    TL_EXPECT_EQ(lines[2].element, 3u);
    TL_EXPECT_EQ(lines[3].field->name_hash, "who"_id);
    TL_EXPECT_EQ(lines[3].a_bits, (u64)handle_make<Entity>(3u, 1u).bits);

    // Identical rows: zero lines; a capped buffer still reports the true count.
    TL_EXPECT_EQ(reflect_diff_rows(&DiffRow_info, &a, &a, lines, 8u), 0u);
    TL_EXPECT_EQ(reflect_diff_rows(&DiffRow_info, &a, &b, lines, 2u), 4u);
}
