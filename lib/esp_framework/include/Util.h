#ifndef _Util_h
#define _Util_h

#include "Arduino.h"

class Util
{
public:
    static char *strlowr(char *str);
    static char *dtostrfd(double number, unsigned char prec, char *s);
    static uint32_t SqrtInt(uint32_t num);
    static uint32_t RoundSqrtInt(uint32_t num);
};
#endif
