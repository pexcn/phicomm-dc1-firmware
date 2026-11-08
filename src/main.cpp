#include <Arduino.h>
#include "Framework.h"
#include "DC1.h"

void setup()
{
    Framework::one(115200);
    module = new DC1();
    Framework::setup();
}

void loop()
{
    Framework::loop();
}
