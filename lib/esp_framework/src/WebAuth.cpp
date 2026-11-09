#include "WebAuth.h"
#include <Arduino.h>
#include <bearssl/bearssl.h>
#include <osapi.h>
#include <string.h>

namespace {
const uint32_t SESSION_TTL = 8UL * 60 * 60;

uint32_t uptime()
{
    // Seconds from the core's monotonic clock avoid millis() rollover after 49 days.
    return micros64() / 1000000;
}
const char HEX_DIGITS[] = "0123456789abcdef";

void encode(const uint8_t *bytes, size_t size, char *out)
{
    for (size_t i = 0; i < size; ++i)
    {
        out[2 * i] = HEX_DIGITS[bytes[i] >> 4];
        out[2 * i + 1] = HEX_DIGITS[bytes[i] & 15];
    }
    out[size * 2] = 0;
}
bool decode(const char *hex, uint8_t *out, size_t size)
{
    for (size_t i = 0; i < size * 2; ++i)
    {
        const char *digit = hex[i] ? strchr(HEX_DIGITS, hex[i]) : nullptr;
        if (!digit) return false;
        if ((i & 1) == 0) out[i / 2] = (digit - HEX_DIGITS) << 4;
        else out[i / 2] |= digit - HEX_DIGITS;
    }
    return true;
}
bool equal(const void *left, const void *right, size_t size)
{
    const uint8_t *a = static_cast<const uint8_t *>(left);
    const uint8_t *b = static_cast<const uint8_t *>(right);
    volatile uint8_t difference = 0;
    for (size_t i = 0; i < size; ++i) difference |= a[i] ^ b[i];
    return difference == 0;
}
void derive(const char *password, const uint8_t salt[16], uint8_t digest[32])
{
    br_sha256_context context;
    br_sha256_init(&context);
    br_sha256_update(&context, salt, 16);
    br_sha256_update(&context, password, strlen(password));
    br_sha256_out(&context, digest);
}
}

bool WebAuth::makePassword(const char *password, char record[PASSWORD_SIZE])
{
    size_t size = strlen(password);
    if (size < 8 || size > 64) return false;
    uint8_t salt[16], digest[32];
    if (os_get_random(salt, sizeof(salt)) != 0) return false;
    derive(password, salt, digest);
    record[0] = '2'; record[1] = '$';
    encode(salt, sizeof(salt), record + 2);
    record[34] = '$';
    encode(digest, sizeof(digest), record + 35);
    return true;
}

bool WebAuth::verifyPassword(const char *password, const char *record)
{
    size_t size = strlen(password), storedSize = strlen(record);
    if (size == 0 || size > 64 || storedSize == 0) return false;
    if (storedSize != PASSWORD_SIZE - 1 || record[0] != '2' || record[1] != '$'
        || record[34] != '$') return false;
    uint8_t salt[16], expected[32], actual[32];
    if (!decode(record + 2, salt, sizeof(salt)) || !decode(record + 35, expected, sizeof(expected)))
        return false;
    derive(password, salt, actual);
    return equal(expected, actual, sizeof(actual));
}

bool WebAuth::issue(char token[TOKEN_SIZE])
{
    uint8_t random[16];
    if (os_get_random(random, sizeof(random)) != 0) return false;
    encode(random, sizeof(random), token);
    uint32_t now = uptime();
    size_t slot = 0;
    for (size_t i = 0; i < 4; ++i)
    {
        if (sessions[i].token[0] == 0 || now - sessions[i].lastUsed >= SESSION_TTL)
        {
            slot = i;
            break;
        }
        if (now - sessions[i].lastUsed > now - sessions[slot].lastUsed) slot = i;
    }
    memcpy(sessions[slot].token, token, TOKEN_SIZE);
    sessions[slot].lastUsed = now;
    return true;
}
bool WebAuth::authorize(const char *token)
{
    if (strlen(token) != TOKEN_SIZE - 1) return false;
    uint32_t now = uptime();
    for (auto &session : sessions)
    {
        if (now - session.lastUsed >= SESSION_TTL) session.token[0] = 0;
        if (session.token[0] && equal(token, session.token, TOKEN_SIZE - 1))
        {
            session.lastUsed = now;
            return true;
        }
    }
    return false;
}
void WebAuth::revoke(const char *token)
{
    for (auto &session : sessions)
        if (strcmp(token, session.token) == 0) session.token[0] = 0;
}
void WebAuth::clear() { for (auto &session : sessions) session.token[0] = 0; }
