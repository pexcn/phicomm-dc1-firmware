#pragma once
struct LogClock { unsigned hour = 0, minute = 0, second = 0; bool valid = false; };
class Rtc {
public:
    static LogClock rtcTime;
};
