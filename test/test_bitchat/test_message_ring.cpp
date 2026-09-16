#include <unity.h>
#include "modules/BitChatBridgeModule.h"
#include <cstring>

// ============================================================================
// BitChatMessageRing (lock-free SPSC queue) Tests
// ============================================================================

static QueuedMessage makeMsg(uint32_t sender, uint8_t type, bool fromBLE) {
    QueuedMessage q;
    q.msg.type = type;
    q.msg.setSenderId32(sender);
    q.fromBLE = fromBLE;
    return q;
}

void test_ring_empty_on_init(void) {
    BitChatMessageRing ring;
    QueuedMessage out;
    TEST_ASSERT_TRUE(ring.empty());
    TEST_ASSERT_FALSE(ring.full());
    TEST_ASSERT_FALSE(ring.pop(out)); // nothing to pop
}

void test_ring_fifo_order(void) {
    BitChatMessageRing ring;

    TEST_ASSERT_TRUE(ring.push(makeMsg(1, BITCHAT_MSG_MESSAGE, true)));
    TEST_ASSERT_TRUE(ring.push(makeMsg(2, BITCHAT_MSG_MESSAGE, false)));
    TEST_ASSERT_TRUE(ring.push(makeMsg(3, BITCHAT_MSG_MESSAGE, true)));

    QueuedMessage out;
    TEST_ASSERT_TRUE(ring.pop(out));
    TEST_ASSERT_EQUAL(1u, out.msg.getSenderId32());
    TEST_ASSERT_TRUE(out.fromBLE);
    TEST_ASSERT_TRUE(ring.pop(out));
    TEST_ASSERT_EQUAL(2u, out.msg.getSenderId32());
    TEST_ASSERT_FALSE(out.fromBLE);
    TEST_ASSERT_TRUE(ring.pop(out));
    TEST_ASSERT_EQUAL(3u, out.msg.getSenderId32());
    TEST_ASSERT_TRUE(ring.empty());
}

void test_ring_full_drops_new(void) {
    BitChatMessageRing ring;

    // Usable capacity is CAPACITY - 1
    const size_t usable = BitChatMessageRing::CAPACITY - 1;
    for (size_t i = 0; i < usable; i++) {
        TEST_ASSERT_TRUE(ring.push(makeMsg((uint32_t)(i + 1), BITCHAT_MSG_MESSAGE, true)));
    }
    TEST_ASSERT_TRUE(ring.full());

    // Next push must be rejected (new message dropped, ring unchanged)
    TEST_ASSERT_FALSE(ring.push(makeMsg(999, BITCHAT_MSG_MESSAGE, true)));

    // The first-pushed message is still at the head (oldest retained, not overwritten)
    QueuedMessage out;
    TEST_ASSERT_TRUE(ring.pop(out));
    TEST_ASSERT_EQUAL(1u, out.msg.getSenderId32());
}

void test_ring_wraparound(void) {
    BitChatMessageRing ring;
    QueuedMessage out;

    // Cycle many times through the ring to exercise index wraparound
    uint32_t nextPush = 1;
    uint32_t nextExpect = 1;
    for (int cycle = 0; cycle < 100; cycle++) {
        // push 3, pop 3, staying well under capacity
        for (int i = 0; i < 3; i++) {
            TEST_ASSERT_TRUE(ring.push(makeMsg(nextPush++, BITCHAT_MSG_MESSAGE, false)));
        }
        for (int i = 0; i < 3; i++) {
            TEST_ASSERT_TRUE(ring.pop(out));
            TEST_ASSERT_EQUAL(nextExpect++, out.msg.getSenderId32());
        }
        TEST_ASSERT_TRUE(ring.empty());
    }
}
