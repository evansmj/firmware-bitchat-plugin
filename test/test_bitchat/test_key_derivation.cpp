#include <unity.h>
#include "modules/BitChatBridgeModule.h"
#include <Curve25519.h>
#include <Ed25519.h>
#include <cstring>

// ============================================================================
// BitChat key derivation tests (deriveBitChatSeed + Ed25519 / Noise X25519)
// ============================================================================

static bool allZero(const uint8_t* p, size_t n) {
    for (size_t i = 0; i < n; i++) {
        if (p[i] != 0) return false;
    }
    return true;
}

void test_derive_seed_deterministic(void) {
    uint8_t secret[32];
    for (int i = 0; i < 32; i++) secret[i] = (uint8_t)(i * 7 + 1);

    uint8_t a[32], b[32];
    BitChatBridgeModule::deriveBitChatSeed(secret, "bitchat-ed25519-v1", a);
    BitChatBridgeModule::deriveBitChatSeed(secret, "bitchat-ed25519-v1", b);

    TEST_ASSERT_EQUAL_MEMORY(a, b, 32); // same input -> same seed (stable across reboots)
    TEST_ASSERT_FALSE(allZero(a, 32));
}

void test_derive_seed_domain_separation(void) {
    uint8_t secret[32];
    for (int i = 0; i < 32; i++) secret[i] = (uint8_t)(0xA0 + i);

    uint8_t edSeed[32], noiseSeed[32];
    BitChatBridgeModule::deriveBitChatSeed(secret, "bitchat-ed25519-v1", edSeed);
    BitChatBridgeModule::deriveBitChatSeed(secret, "bitchat-noise-v1", noiseSeed);

    // Different domain tags must produce different keys, and neither equals the
    // Meshtastic secret they were derived from.
    TEST_ASSERT_NOT_EQUAL(0, memcmp(edSeed, noiseSeed, 32));
    TEST_ASSERT_NOT_EQUAL(0, memcmp(edSeed, secret, 32));
    TEST_ASSERT_NOT_EQUAL(0, memcmp(noiseSeed, secret, 32));
}

void test_derive_seed_changes_with_secret(void) {
    uint8_t secret1[32], secret2[32];
    memset(secret1, 0x11, 32);
    memset(secret2, 0x11, 32);
    secret2[0] = 0x12; // single-byte difference

    uint8_t out1[32], out2[32];
    BitChatBridgeModule::deriveBitChatSeed(secret1, "bitchat-noise-v1", out1);
    BitChatBridgeModule::deriveBitChatSeed(secret2, "bitchat-noise-v1", out2);

    TEST_ASSERT_NOT_EQUAL(0, memcmp(out1, out2, 32)); // avalanche - not derived from node id
}

void test_ed25519_public_key_valid(void) {
    uint8_t secret[32];
    for (int i = 0; i < 32; i++) secret[i] = (uint8_t)(i + 3);

    uint8_t priv[32], pub[32];
    BitChatBridgeModule::deriveBitChatSeed(secret, "bitchat-ed25519-v1", priv);
    Ed25519::derivePublicKey(pub, priv);

    TEST_ASSERT_FALSE(allZero(pub, 32)); // a real Ed25519 public key was produced
}

void test_noise_public_key_valid_x25519(void) {
    uint8_t secret[32];
    for (int i = 0; i < 32; i++) secret[i] = (uint8_t)(200 - i);

    // Reproduce the firmware's derivation: clamp the seed to a valid X25519 scalar and
    // eval against the base point. (dh1 must NOT be used here - it would overwrite the
    // private key with random bytes, giving a different key every boot.)
    uint8_t noisePriv[32], noisePub[32];
    BitChatBridgeModule::deriveBitChatSeed(secret, "bitchat-noise-v1", noisePriv);
    noisePriv[0] &= 0xF8;
    noisePriv[31] = (noisePriv[31] & 0x7F) | 0x40;
    Curve25519::eval(noisePub, noisePriv, nullptr);

    TEST_ASSERT_FALSE(allZero(noisePub, 32)); // real 32-byte X25519 public key, not fake

    // Deterministic: same secret -> same public key across reboots.
    uint8_t noisePriv2[32], noisePub2[32];
    BitChatBridgeModule::deriveBitChatSeed(secret, "bitchat-noise-v1", noisePriv2);
    noisePriv2[0] &= 0xF8;
    noisePriv2[31] = (noisePriv2[31] & 0x7F) | 0x40;
    Curve25519::eval(noisePub2, noisePriv2, nullptr);
    TEST_ASSERT_EQUAL_MEMORY(noisePub, noisePub2, 32);
}
