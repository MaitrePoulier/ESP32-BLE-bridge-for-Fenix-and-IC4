// Event log of the bridge, kept in NVS (see EventLog.h)
// Ring buffer in RAM; a FreeRTOS timer writes it to NVS at most every 5 s, only when it changed (flash wear).
#include "EventLog.h"

#include <stdio.h>
#include <stdarg.h>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/timers.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_system.h"
#include "nvs.h"

static const char *TAG = "EventLog";
static const char *NVS_NAMESPACE = "eventlog";

#define SAVE_PERIOD_MS 5000

typedef struct {
    uint16_t version;
    uint16_t boot;
    uint16_t head;      // Next slot
    uint16_t count;
    event_t events[EVENT_LOG_SIZE];
} event_store_t;

static event_store_t s_log;              // Protected by s_mux
static event_store_t s_save_copy;        // Copy written to NVS (timer task only)
static bool s_dirty = false;
static portMUX_TYPE s_mux = portMUX_INITIALIZER_UNLOCKED;
static const uint16_t STORE_VERSION = 1;

static void save_cb(TimerHandle_t timer)
{
    taskENTER_CRITICAL(&s_mux);
    const bool dirty = s_dirty;
    if (dirty) {
        memcpy(&s_save_copy, &s_log, sizeof(s_log));
        s_dirty = false;
    }
    taskEXIT_CRITICAL(&s_mux);
    if (!dirty) return;

    nvs_handle_t nvs;
    esp_err_t err = nvs_open(NVS_NAMESPACE, NVS_READWRITE, &nvs);
    if (err == ESP_OK) {
        err = nvs_set_blob(nvs, "log", &s_save_copy, sizeof(s_save_copy));
        if (err == ESP_OK) err = nvs_commit(nvs);
        nvs_close(nvs);
    }
    if (err != ESP_OK) ESP_LOGW(TAG, "Saving the event log: %s", esp_err_to_name(err));
}

static const char *reset_reason(void)
{
    switch (esp_reset_reason()) {
        case ESP_RST_POWERON:  return "power-on";
        case ESP_RST_EXT:      return "external reset";
        case ESP_RST_SW:       return "software reset";
        case ESP_RST_PANIC:    return "PANIC";
        case ESP_RST_INT_WDT:  return "INTERRUPT WATCHDOG";
        case ESP_RST_TASK_WDT: return "TASK WATCHDOG";
        case ESP_RST_WDT:      return "WATCHDOG";
        case ESP_RST_BROWNOUT: return "BROWNOUT";
        default:               return "other reset";
    }
}

void EventLog_init(void)
{
    nvs_handle_t nvs;
    size_t size = sizeof(s_log);
    bool loaded = false;
    if (nvs_open(NVS_NAMESPACE, NVS_READONLY, &nvs) == ESP_OK) {
        loaded = nvs_get_blob(nvs, "log", &s_log, &size) == ESP_OK && size == sizeof(s_log) &&
                 s_log.version == STORE_VERSION && s_log.head < EVENT_LOG_SIZE && s_log.count <= EVENT_LOG_SIZE;
        nvs_close(nvs);
    }
    if (!loaded) {
        memset(&s_log, 0, sizeof(s_log));
        s_log.version = STORE_VERSION;
    }
    s_log.boot++;
    EventLog_add("Boot %u (%s)", s_log.boot, reset_reason());

    TimerHandle_t timer = xTimerCreate("eventlog", pdMS_TO_TICKS(SAVE_PERIOD_MS), pdTRUE, NULL, save_cb);
    if (timer != NULL) xTimerStart(timer, 0);
}

void EventLog_add(const char *format, ...)
{
    char text[EVENT_TEXT_LEN];
    va_list args;
    va_start(args, format);
    vsnprintf(text, sizeof(text), format, args);
    va_end(args);

    const uint32_t uptime = (uint32_t)(esp_timer_get_time() / 1000000);
    const time_t now = time(NULL);
    const int64_t epoch = (now > 1700000000) ? (int64_t)now : 0;   // After 2023: synchronised

    taskENTER_CRITICAL(&s_mux);
    event_t *last = (s_log.count > 0) ? &s_log.events[(s_log.head + EVENT_LOG_SIZE - 1) % EVENT_LOG_SIZE] : NULL;
    if (last != NULL && last->boot == s_log.boot && strcmp(last->text, text) == 0) {
        // Same event again (scan without result...): merged
        if (last->repeat < UINT16_MAX) last->repeat++;
        last->uptime_s = uptime;
        last->epoch = epoch;
    } else {
        event_t *e = &s_log.events[s_log.head];
        e->boot = s_log.boot;
        e->repeat = 0;
        e->uptime_s = uptime;
        e->epoch = epoch;
        memcpy(e->text, text, sizeof(e->text));
        s_log.head = (s_log.head + 1) % EVENT_LOG_SIZE;
        if (s_log.count < EVENT_LOG_SIZE) s_log.count++;
    }
    s_dirty = true;
    taskEXIT_CRITICAL(&s_mux);
}

int EventLog_copy(event_t *out, int max)
{
    taskENTER_CRITICAL(&s_mux);
    const int n = (s_log.count < max) ? s_log.count : max;
    const int first = (s_log.head + EVENT_LOG_SIZE - s_log.count) % EVENT_LOG_SIZE;
    const int skip = s_log.count - n;   // Oldest events dropped if max is smaller
    for (int i = 0; i < n; i++) out[i] = s_log.events[(first + skip + i) % EVENT_LOG_SIZE];
    taskEXIT_CRITICAL(&s_mux);
    return n;
}

void EventLog_clear(void)
{
    taskENTER_CRITICAL(&s_mux);
    s_log.head = 0;
    s_log.count = 0;
    s_dirty = true;
    taskEXIT_CRITICAL(&s_mux);
}

uint16_t EventLog_bootNumber(void)
{
    return s_log.boot;
}
