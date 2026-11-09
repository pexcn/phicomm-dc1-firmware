#include "WebAuth.h"
#include <cassert>
#include <cstring>
#include <cstdio>

static uint64_t now;
static uint8_t randomByte;
static bool failRandom;
uint64_t micros64() { return now * 1000; }
int os_get_random(unsigned char *out, size_t size)
{
    if (failRandom) return -1;
    for (size_t i = 0; i < size; ++i) out[i] = randomByte++;
    return 0;
}

int main()
{
    // Cross-check the actual BearSSL hash against Python SHA-256.
    char record[WebAuth::PASSWORD_SIZE];
    assert(WebAuth::makePassword("test-password", record));
    assert(strcmp(record, "2$000102030405060708090a0b0c0d0e0f$6bb6535cc4adff6d2138573f125b9fea7135129df687ca37139a6720c5c8b8e0") == 0);
    assert(WebAuth::verifyPassword("test-password", record));
    assert(!WebAuth::verifyPassword("wrong-password", record));
    record[0] = '1';
    assert(!WebAuth::verifyPassword("test-password", record)); // Unsupported format.
    record[0] = '2';
    record[40] = 'z';
    assert(!WebAuth::verifyPassword("test-password", record));
    assert(!WebAuth::verifyPassword("secret", "secret"));
    assert(!WebAuth::verifyPassword("", ""));
    assert(!WebAuth::makePassword("short", record));
    // Already-migrated short passwords remain usable, but cannot be newly set.
    assert(WebAuth::verifyPassword("secret", "2$000102030405060708090a0b0c0d0e0f$67557c0d86179c45199f096d85b7aeb2b8b34bf423c403ba517f72bcc5913c4c"));
    failRandom = true;
    assert(!WebAuth::makePassword("test-password", record));
    WebAuth auth;
    char first[WebAuth::TOKEN_SIZE], second[WebAuth::TOKEN_SIZE], token[WebAuth::TOKEN_SIZE];
    assert(!auth.issue(token));
    failRandom = false;
    assert(auth.issue(first) && auth.issue(second));
    assert(strlen(first) == 32);
    assert(strcmp(first, second) != 0 && auth.authorize(first) && auth.authorize(second));
    auth.revoke(first);
    assert(!auth.authorize(first) && auth.authorize(second));
    auth.clear();
    assert(!auth.authorize(second) && !auth.authorize(""));
    // Request-only expiry must not revive an idle session after millis() wraps.
    now = UINT32_MAX - 1000;
    assert(auth.issue(first));
    now += 10000;
    assert(auth.authorize(first));
    now += 8UL * 60 * 60 * 1000;
    assert(!auth.authorize(first));
    assert(auth.issue(first));
    now += (1ULL << 32) + 1000;
    assert(!auth.authorize(first));
    // Four concurrent sessions, oldest evicted only when the table is full.
    auth.clear();
    assert(auth.issue(first));
    for (int i = 0; i < 4; ++i) { ++now; assert(auth.issue(token)); }
    assert(!auth.authorize(first) && auth.authorize(token));
    puts("PASS: salted SHA-256, supported record format, sessions, logout and request-only expiry");
}
