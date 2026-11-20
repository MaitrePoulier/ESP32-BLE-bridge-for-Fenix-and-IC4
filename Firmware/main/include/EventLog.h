#ifndef _EVENT_LOG_H_
#define _EVENT_LOG_H_

    #include <stdint.h>
    #include <time.h>

    #ifdef __cplusplus
        extern "C" {
    #endif

    // Log of the bridge events (connections, disconnections, first FTMS frames...), kept in NVS: it survives a
    // restart (opening the serial monitor resets the board). Read with the console "bridge log" or on the screen.
    #define EVENT_LOG_SIZE  48
    #define EVENT_TEXT_LEN  64

    typedef struct {
        uint16_t boot;               // Boot number (incremented at each start)
        uint16_t repeat;             // Same text logged again (merged): number of extra times
        uint32_t uptime_s;           // Uptime of the last occurrence
        int64_t epoch;               // Local time of the last occurrence (0: not synchronised)
        char text[EVENT_TEXT_LEN];
    } event_t;

    void EventLog_init(void);        // After nvs_flash_init: loads the log, logs "Boot", saves every 5 s if needed
    void EventLog_add(const char *format, ...) __attribute__((format(printf, 1, 2)));   // Any task
    int  EventLog_copy(event_t *out, int max);   // Oldest first, returns the number of events
    void EventLog_clear(void);
    uint16_t EventLog_bootNumber(void);

    #ifdef __cplusplus
    }
    #endif

#endif
