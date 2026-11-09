#pragma once
#include <stddef.h>
#include <stdint.h>

class WebAuth
{
public:
    static const size_t TOKEN_SIZE = 33; // 128 random bits, hex encoded.
    static const size_t PASSWORD_SIZE = 100; // 2$<16-byte salt>$<SHA-256(salt || password)>

private:
    struct Session
    {
        char token[TOKEN_SIZE] = {};
        uint32_t lastUsed = 0;
    } sessions[4];

public:
    static bool makePassword(const char *password, char record[PASSWORD_SIZE]);
    static bool verifyPassword(const char *password, const char *record);
    bool issue(char token[TOKEN_SIZE]);
    bool authorize(const char *token);
    void revoke(const char *token);
    void clear();
};
