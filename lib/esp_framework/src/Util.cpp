#include "Util.h"

char *Util::strlowr(char *str)
{
    char *orign = str;
    for (; *str != '\0'; str++)
        *str = tolower(*str);
    return orign;
}

char *Util::dtostrfd(double number, unsigned char prec, char *s)
{
    if ((isnan(number)) || (isinf(number)))
    { // Fix for JSON output (https://stackoverflow.com/questions/1423081/json-left-out-infinity-and-nan-json-status-in-ecmascript)
        strcpy(s, PSTR("null"));
        return s;
    }
    else
    {
        return dtostrf(number, 1, prec, s);
    }
}

uint32_t Util::SqrtInt(uint32_t num)
{
    if (num <= 1)
    {
        return num;
    }

    uint32_t x = num / 2;
    uint32_t y;
    do
    {
        y = (x + num / x) / 2;
        if (y >= x)
        {
            return x;
        }
        x = y;
    } while (true);
}

uint32_t Util::RoundSqrtInt(uint32_t num)
{
    uint32_t s = SqrtInt(4 * num);
    if (s & 1)
    {
        s++;
    }
    return s / 2;
}
