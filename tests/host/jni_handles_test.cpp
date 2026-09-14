// 32-bit guest handles: kind in the low 2 bits, table index above; 0 is null.
#include <cstdio>

#include "check.h"
#include "zb/jni_handles.h"

int main() {
    using zb::HandleKind;
    using Ref = std::optional<std::uint64_t>;

    CHECK(!zb::handle_kind(0));
    CHECK(zb::handle_kind(zb::make_handle(HandleKind::Global, 7)) == HandleKind::Global);
    CHECK(zb::handle_index(zb::make_handle(HandleKind::WeakGlobal, 7)) == 7);

    zb::LocalHandles locals;
    CHECK(locals.add(0) == 0);
    CHECK(locals.get(0) == Ref(0));
    const std::uint32_t a = locals.add(0x7F00000000A0ull);  // base frame created on demand
    CHECK(locals.frame_count() == 1);
    CHECK(a != 0 && zb::handle_kind(a) == HandleKind::Local);
    CHECK(locals.get(a) == Ref(0x7F00000000A0ull));

    locals.push_frame();
    const std::uint32_t b = locals.add(0x7F00000000B0ull);
    const std::uint32_t c = locals.add(0x7F00000000C0ull);
    CHECK(locals.remove(b) == Ref(0x7F00000000B0ull));
    CHECK(!locals.get(b));     // deleted
    CHECK(!locals.remove(b));  // double delete is invalid
    const std::vector<std::uint64_t> released = locals.pop_frame();
    CHECK(released.size() == 1 && released[0] == 0x7F00000000C0ull);
    CHECK(!locals.get(c));                           // gone with its frame
    CHECK(locals.get(a) == Ref(0x7F00000000A0ull));  // outer frame intact
    CHECK(!locals.get(zb::make_handle(HandleKind::Global, 0)));  // wrong kind
    CHECK(!locals.get(zb::make_handle(HandleKind::Local, 99)));  // out of range
    CHECK(locals.remove(0) == Ref(0));                           // DeleteLocalRef(NULL) is allowed
    CHECK(locals.pop_frame().size() == 1);
    CHECK(locals.pop_frame().empty() && locals.frame_count() == 0);

    zb::GlobalHandles globals(HandleKind::Global);
    const std::uint32_t g1 = globals.add(0x10);
    const std::uint32_t g2 = globals.add(0x20);
    CHECK(zb::handle_kind(g1) == HandleKind::Global && g1 != g2);
    CHECK(globals.get(g2) == Ref(0x20));
    CHECK(globals.remove(g1) == Ref(0x10));
    CHECK(!globals.get(g1) && !globals.remove(g1));
    CHECK(globals.add(0x30) == g1);  // freed slot reused
    CHECK(!globals.get(zb::make_handle(HandleKind::WeakGlobal, zb::handle_index(g2))));  // kind mismatch

    zb::IdTable ids;
    CHECK(ids.intern(0) == 0);
    const std::uint32_t m1 = ids.intern(0x7F0000001000ull);
    const std::uint32_t m2 = ids.intern(0x7F0000002000ull);
    CHECK(m1 == 1 && m2 == 2 && ids.intern(0x7F0000001000ull) == m1);
    CHECK(ids.get(m2) == Ref(0x7F0000002000ull));
    CHECK(!ids.get(3));

    std::printf("jni_handles_test ok\n");
    return 0;
}
