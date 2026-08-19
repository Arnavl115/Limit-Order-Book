#include "core/order_arena.hpp"
#include "core/order_id_map.hpp"

#include "test_framework.hpp"

using namespace lob;

static OrderNode* fakeNode(OrderId id) {
    OrderNode* n = new OrderNode();
    n->order.id = id;
    return n;
}

TEST(order_id_map_insert_and_find) {
    OrderIdMap map;
    CHECK(map.empty());
    CHECK(map.size() == 0);
    CHECK(map.find(7) == nullptr);

    OrderNode* n7 = fakeNode(7);
    CHECK(map.insert(7, n7));
    CHECK(!map.empty());
    CHECK(map.size() == 1);
    CHECK(map.find(7) == n7);
    CHECK(map.find(8) == nullptr);
    delete n7;
}

TEST(order_id_map_duplicate_insert_rejected) {
    OrderIdMap map;
    OrderNode* n = fakeNode(1);
    CHECK(map.insert(1, n));
    CHECK(!map.insert(1, n));  // duplicate id
    CHECK(map.size() == 1);
    delete n;
}

TEST(order_id_map_erase) {
    OrderIdMap map;
    OrderNode* a = fakeNode(1);
    OrderNode* b = fakeNode(2);
    OrderNode* c = fakeNode(3);
    map.insert(1, a);
    map.insert(2, b);
    map.insert(3, c);

    CHECK(map.erase(2));
    CHECK(map.size() == 2);
    CHECK(map.find(2) == nullptr);
    CHECK(map.find(1) == a);
    CHECK(map.find(3) == c);

    CHECK(!map.erase(99));  // unknown
    CHECK(map.erase(1));
    CHECK(map.erase(3));
    CHECK(map.empty());
    CHECK(map.find(1) == nullptr);
    delete a;
    delete b;
    delete c;
}

TEST(order_id_map_grows_and_preserves_entries) {
    OrderIdMap map;
    const std::size_t n = 100'000;
    std::vector<OrderNode*> nodes(n);
    for (std::size_t i = 0; i < n; ++i) {
        nodes[i] = fakeNode(static_cast<OrderId>(i + 1));
    }
    for (std::size_t i = 0; i < n; ++i) {
        CHECK(map.insert(static_cast<OrderId>(i + 1), nodes[i]));
    }
    CHECK(map.size() == n);

    // Every entry survives the growth/rehash sequence.
    for (std::size_t i = 0; i < n; ++i) {
        CHECK(map.find(static_cast<OrderId>(i + 1)) == nodes[i]);
    }

    // Erase half, then verify the rest are still findable (tombstone paths).
    for (std::size_t i = 0; i < n; i += 2) {
        CHECK(map.erase(static_cast<OrderId>(i + 1)));
    }
    for (std::size_t i = 1; i < n; i += 2) {
        CHECK(map.find(static_cast<OrderId>(i + 1)) == nodes[i]);
    }
    for (OrderNode* p : nodes) {
        delete p;
    }
}

TEST(order_id_map_reserve_avoids_rehash) {
    OrderIdMap map;
    map.reserve(1'000'000);
    std::vector<OrderNode*> nodes(50'000);
    for (std::size_t i = 0; i < nodes.size(); ++i) {
        nodes[i] = fakeNode(static_cast<OrderId>(i + 1));
    }
    for (std::size_t i = 0; i < nodes.size(); ++i) {
        CHECK(map.insert(static_cast<OrderId>(i + 1), nodes[i]));
    }
    CHECK(map.size() == nodes.size());
    CHECK(map.find(static_cast<OrderId>(nodes.size())) == nodes.back());
    for (OrderNode* p : nodes) {
        delete p;
    }
}

TEST(order_id_map_high_churn_stays_correct) {
    OrderIdMap map;
    // Interleave adds and cancels so tombstones accumulate; correctness (not
    // just performance) must survive the compaction rehash.
    const std::size_t n = 100'000;
    for (std::size_t round = 0; round < 4; ++round) {
        std::vector<OrderNode*> nodes(n);
        for (std::size_t i = 0; i < n; ++i) {
            nodes[i] = fakeNode(static_cast<OrderId>(i));
        }
        for (std::size_t i = 0; i < n; ++i) {
            map.insert(static_cast<OrderId>(i), nodes[i]);
        }
        for (std::size_t i = 0; i < n; i += 2) {
            map.erase(static_cast<OrderId>(i));
        }
        for (std::size_t i = 1; i < n; i += 2) {
            CHECK(map.find(static_cast<OrderId>(i)) != nullptr);
        }
        for (std::size_t i = 1; i < n; i += 2) {
            map.erase(static_cast<OrderId>(i));
        }
        CHECK(map.empty());
        for (OrderNode* p : nodes) {
            delete p;
        }
    }
}