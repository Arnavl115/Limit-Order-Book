#include "core/order_arena.hpp"

#include "test_framework.hpp"

using namespace lob;

TEST(order_arena_allocates_and_recycles_nodes) {
    OrderArena arena(8);  // tiny chunk: forces growth in later tests
    OrderNode* a = arena.allocate();
    OrderNode* b = arena.allocate();
    CHECK(a != nullptr);
    CHECK(b != nullptr);
    CHECK(a != b);
    CHECK(a->prev == nullptr);
    CHECK(a->next == nullptr);
    CHECK(a->level == nullptr);
    CHECK(arena.chunkCount() == 1);

    // Nodes hold real Order data and survive.
    a->order.id = 42;
    a->order.qty = 10;
    a->order.remaining = 10;
    CHECK(a->order.id == 42);

    // Deallocate puts the node back on the free list...
    arena.deallocate(a);
    arena.deallocate(b);

    // ...and the next allocate reuses it (no new chunk, no heap traffic).
    OrderNode* a2 = arena.allocate();
    CHECK(a2 == a || a2 == b);  // LIFO free list: b was freed last
    CHECK(a2->prev == nullptr);
    CHECK(a2->next == nullptr);
    CHECK(arena.chunkCount() == 1);
    CHECK(arena.capacity() == 8);
}

TEST(order_arena_grows_chunks_on_demand) {
    OrderArena arena(4);  // tiny initial chunk
    OrderNode* first = arena.allocate();
    CHECK(arena.capacity() == 4);

    // Exhaust the first chunk; the second allocate must grow the arena.
    arena.allocate();
    arena.allocate();
    arena.allocate();  // 4 nodes now used
    CHECK(arena.chunkCount() == 1);

    OrderNode* fifth = arena.allocate();
    CHECK(fifth != nullptr);
    CHECK(arena.chunkCount() == 2);
    CHECK(arena.capacity() == 12);  // 4 + 8 (geometric doubling)

    // Every allocated node has clean pointers.
    CHECK(fifth->prev == nullptr && fifth->next == nullptr && fifth->level == nullptr);

    // Pointers into the first chunk remain valid after growth (vector per
    // chunk is never reallocated).
    first->order.id = 99;
    CHECK(first->order.id == 99);
}

TEST(order_arena_capacity_is_accurate) {
    OrderArena arena(2);
    CHECK(arena.capacity() == 0);
    arena.allocate();
    CHECK(arena.capacity() == 2);
    arena.allocate();
    arena.allocate();
    CHECK(arena.capacity() == 2 + 4);  // second chunk doubled
}

TEST(order_arena_default_chunk_is_large) {
    OrderArena arena;
    CHECK(arena.chunkCount() == 0);
    arena.allocate();
    CHECK(arena.chunkCount() == 1);
    CHECK(arena.capacity() == OrderArena::kDefaultChunk);
}

TEST(order_arena_deallocate_never_reuses_an_inuse_node) {
    OrderArena arena(16);
    OrderNode* keep = arena.allocate();
    OrderNode* free1 = arena.allocate();
    OrderNode* free2 = arena.allocate();
    arena.deallocate(free1);
    arena.deallocate(free2);
    OrderNode* re1 = arena.allocate();
    OrderNode* re2 = arena.allocate();
    CHECK(re1 == free2);  // LIFO
    CHECK(re2 == free1);
    CHECK(re1 != keep);
    CHECK(re2 != keep);
    CHECK(keep->order.id == 0);  // untouched
}
