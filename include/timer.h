#include <Arduino.h>
String millisToTimeString(unsigned long eventMillis) {
    static bool ntpEverSynced = false;
    static unsigned long ntpSyncMillis = 0;
    static time_t ntpSyncTime = 0;

    if (!ntpEverSynced) {
        time_t now;
        time(&now);
        if (now > 1700000000UL) {               // якщо час адекватний (після 2023 року)
            ntpEverSynced = true;
            ntpSyncMillis = millis();
            ntpSyncTime = now;
        }
    }

    if (!ntpEverSynced) {
        return "00-00-00";
    }

    long deltaSeconds = (long)(eventMillis - ntpSyncMillis) / 1000L;
    time_t eventTime = ntpSyncTime + deltaSeconds;

    struct tm timeinfo;
    localtime_r(&eventTime, &timeinfo);
    char buf[9];
    strftime(buf, sizeof(buf), "%H-%M-%S", &timeinfo);
    return String(buf);
}

String replaceMillisWithTime(const String& original) {
    int colon1 = original.indexOf(':');
    if (colon1 == -1) return original;
    int colon2 = original.indexOf(':', colon1 + 1);
    if (colon2 == -1) return original;
    int colon3 = original.indexOf(':', colon2 + 1);
    if (colon3 == -1) return original;

    String millisStr = original.substring(colon2 + 1, colon3);
    unsigned long eventMillis = millisStr.toInt();

    String realTime = millisToTimeString(eventMillis);
    return original.substring(0, colon2 + 1) + realTime + original.substring(colon3);
}