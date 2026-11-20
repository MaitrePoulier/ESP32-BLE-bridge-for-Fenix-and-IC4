// Serial console (UART0, CP2102N): esp_console + linenoise
// Port of esp32-mezz-jauges/Firmware/main/Console.cpp, adapted to the ESP32-WROOM-32E-N8R2 base board
#include "Console.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include <string.h>
#include <strings.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdarg.h>
#include <math.h>
#include <time.h>
#include <string>
#include "Config.h"

#include "esp_console.h"
#include "linenoise/linenoise.h"
#include "esp_chip_info.h"
#include "esp_mac.h"
#include "esp_app_desc.h"
#include "esp_idf_version.h"
#include "esp_heap_caps.h"
#include "esp_timer.h"
#include "ESP32.h"
#include "MyWifi.h"
#include "Batt.h"
#include "Lvgl_code.h"
#include "lvgl.h"

// ============================================================================
// COMMON DISPLAY: banner, sections and aligned "label  value" rows
// ============================================================================

// Coloured stars of the banner
static void print_colored_stars(int count) {
    for (int i = 0; i < count; i++) {
        int color_index = (abs(console_random()) % 216) + 16;
        printf("\033[38;5;%dm*", color_index);
    }
    printf(AINSIrst);
}

// Shown at the end of the initialisation (console_start). The terminal is not cleared, so that the boot messages
// stay visible (WiFi, time...).
void console_displayIntro(void) {
    print_colored_stars(57); printf("\n");
    printf("   Use Tera Term in ANSI mode to support up/down arrows \n");
    printf("           Look nice with Consolas Font in 14           \n");
    printf("        Type 'help' to display all the commands         \n");
    printf("                 ESP32-WROOM-32E-N8R2                   \n");
    print_colored_stars(57); printf("\n");
}

static constexpr int SECTION_WIDTH = 56;   // Section title underlined up to this column
static constexpr int LABEL_WIDTH = 21;     // Column of the values

// Number of characters shown for a UTF-8 text (continuation bytes 10xxxxxx do not count)
static int utf8_columns(const char *s) {
    int n = 0;
    for (; *s; s++) n += ((*s & 0xC0) != 0x80);
    return n;
}

void console_section(const char *title) {
    printf("\n" BCYN "── %s ", title);
    for (int i = utf8_columns(title) + 4; i < SECTION_WIDTH; i++) printf("─");
    printf(AINSIrst "\n");
}

void console_row(const char *label, const char *format, ...) {
    printf("  %s", label);
    for (int i = utf8_columns(label); i < LABEL_WIDTH; i++) putchar(' ');
    va_list args;
    va_start(args, format);
    vprintf(format, args);
    va_end(args);
    putchar('\n');
}

char *console_safe_text(char *dst, size_t size, const char *src) {
    if (size == 0) return dst;
    size_t i = 0;
    for (; src[i] != '\0' && i + 1 < size; i++) {
        const unsigned char c = (unsigned char)src[i];
        dst[i] = (c < 0x20 || c == 0x7F) ? '?' : (char)c;
    }
    dst[i] = '\0';
    return dst;
}

// ============================================================================
// COMMANDS AND SUB-COMMANDS: one table per command. The help ('help'), the hint, the usage and the argument-count
// check all come from these tables (dispatch, register_command).
// Rules: without an argument, the default sub-command (a status, never a long or destructive action) or the usage
// if the command has none; unknown sub-command or wrong arguments: usage and error code.
// ============================================================================

static constexpr int USAGE_ERROR = -1;                 // Returned by a sub-command: invalid arguments

struct subcmd_t {
    const char *name;
    const char *args;                  // Notation: [optional] <value> a|b ("": no argument)
    uint8_t min_args, max_args;        // Number of arguments after the sub-command name
    int (*func)(int argc, char **argv);   // argv[0] = sub-command name
    const char *help;                  // One sentence
};

struct command_t {
    const char *name;
    const char *summary;               // One sentence
    const subcmd_t *subs;              // nullptr: command without argument (func)
    uint8_t count;
    int8_t default_sub;                // Index run without an argument; -1: sub-command required
    int (*func)(int argc, char **argv);
};

// "[a|b|c]" (optional sub-command) or "<a|b|c>" (required)
static void build_hint(const command_t *c, char *buf, size_t size) {
    size_t len = snprintf(buf, size, "%c", c->default_sub >= 0 ? '[' : '<');
    for (int i = 0; i < c->count && len < size; i++) {
        len += snprintf(buf + len, size - len, "%s%s", i ? "|" : "", c->subs[i].name);
    }
    if (len < size) snprintf(buf + len, size - len, "%c", c->default_sub >= 0 ? ']' : '>');
}

static void print_usage(const command_t *c, const subcmd_t *s) {
    if (s != nullptr) {
        printf("Usage: %s %s%s%s\n", c->name, s->name, s->args[0] ? " " : "", s->args);
    } else if (c->subs != nullptr) {
        char hint[96];
        build_hint(c, hint, sizeof(hint));
        printf("Usage: %s %s\n", c->name, hint);
    } else {
        printf("Usage: %s   (no argument)\n", c->name);
    }
    printf("       'help %s' for details\n", c->name);
}

static int dispatch(void *context, int argc, char **argv) {
    const command_t *c = (const command_t *)context;
    if (c->subs == nullptr) {
        if (argc > 1) { print_usage(c, nullptr); return 1; }
        return c->func(argc, argv);
    }
    const subcmd_t *s = nullptr;
    if (argc < 2) {
        if (c->default_sub < 0) { print_usage(c, nullptr); return 1; }
        s = &c->subs[c->default_sub];
    } else {
        for (int i = 0; i < c->count && s == nullptr; i++) {
            if (strcasecmp(argv[1], c->subs[i].name) == 0) s = &c->subs[i];
        }
        if (s == nullptr) {
            printf("Unknown subcommand '%s'\n", argv[1]);
            print_usage(c, nullptr);
            return 1;
        }
    }
    const int nargs = (argc >= 2) ? argc - 2 : 0;
    if (nargs < s->min_args || nargs > s->max_args) { print_usage(c, s); return 1; }

    char *default_argv[] = {(char *)s->name, nullptr};
    const int ret = (argc >= 2) ? s->func(argc - 1, argv + 1) : s->func(1, default_argv);
    if (ret == USAGE_ERROR) { print_usage(c, s); return 1; }
    return ret;
}

// Help text wrapped here into lines of at most HELP_LINE bytes. 'help' passes it to arg_print_formatted (argtable3),
// which wraps again at 77 bytes: when its break falls on a '\n', it prints a blank line. With short lines that all
// start with a non-blank character followed by a space ("- " or no-break space + spaces), the break always falls
// after the '\n'.
static constexpr size_t HELP_LINE = 72;
static constexpr const char *HELP_ITEM = "- ";                  // Start of a sub-command line
static constexpr const char *HELP_NEXT = "\xC2\xA0   ";          // Continuation: no-break space (U+00A0) + 3 spaces

static void help_append(std::string &out, const char *prefix, const std::string &text) {
    std::string line = prefix;
    size_t pos = 0;
    while (pos < text.size()) {
        size_t end = text.find(' ', pos);
        if (end == std::string::npos) end = text.size();
        const std::string word = text.substr(pos, end - pos);
        if (line.size() > strlen(prefix) && line.size() + 1 + word.size() > HELP_LINE) {
            out += line + "\n";
            line = HELP_NEXT;
        } else if (line.size() > strlen(prefix)) {
            line += ' ';
        }
        line += word;
        pos = end + 1;
    }
    out += line;
}

// Registers a command: help = summary + one line per sub-command (kept forever by esp_console, in PSRAM if possible:
// internal SRAM is needed by the WiFi), hint copied by esp_console
static void register_command(const command_t *c) {
    std::string text;
    help_append(text, "", c->summary);
    for (int i = 0; i < c->count; i++) {
        const subcmd_t &s = c->subs[i];
        text += "\n";
        help_append(text, HELP_ITEM, std::string(s.name) + (s.args[0] ? " " : "") + s.args +
                                     (i == c->default_sub ? " (default)" : "") + ": " + s.help);
    }
    char *help = (char *)heap_caps_malloc_prefer(text.size() + 1, 2, MALLOC_CAP_SPIRAM, MALLOC_CAP_DEFAULT);
    assert(help != nullptr);
    memcpy(help, text.c_str(), text.size() + 1);
    char hint[96];
    if (c->subs != nullptr) build_hint(c, hint, sizeof(hint));
    const esp_console_cmd_t cmd = {
        .command = c->name, .help = help, .hint = c->subs != nullptr ? hint : nullptr, .func = nullptr,
        .argtable = nullptr, .func_w_context = &dispatch, .context = (void *)c
    };
    ESP_ERROR_CHECK(esp_console_cmd_register(&cmd));
}

#define COUNT_OF(a) ((uint8_t)(sizeof(a) / sizeof((a)[0])))

// ============================================================================
// restart, detail
// ============================================================================

static int cmd_restart(int argc, char **argv) {
    esp_restart();
    return 0;
}

// Whole system status (former commands detail, crash, heap, temp, version and lvglVersion)
static int cmd_detail(int argc, char **argv) {
    // Measurements first (~100 ms for the temperature)
    const float esp_temp = ESP32_ReadTemp();
    const float vbatt = battRead();

    esp_chip_info_t chip;
    esp_chip_info(&chip);
    uint8_t mac[6] = {};
    esp_read_mac(mac, ESP_MAC_WIFI_STA);
    const uint32_t up = (uint32_t)(esp_timer_get_time() / 1000000);

    console_section(ESP32_model_str());
    console_row("Revision", "v%d.%d, %d cores, %d MHz", chip.revision / 100, chip.revision % 100, chip.cores,
                CONFIG_ESP_DEFAULT_CPU_FREQ_MHZ);
    console_row("WiFi MAC", "%02X:%02X:%02X:%02X:%02X:%02X", mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
    console_row("Last reset", "%s", ESP32_reset_reason_str());
    console_row("Uptime", "%lu:%02lu:%02lu", (unsigned long)(up / 3600), (unsigned long)(up / 60 % 60),
                (unsigned long)(up % 60));
    const time_t now = time(NULL);
    if (now > 1700000000) {   // After 2023: the time was synchronised (SNTP)
        struct tm local;
        localtime_r(&now, &local);
        char text[32];
        strftime(text, sizeof(text), "%Y-%m-%d %H:%M:%S", &local);
        console_row("Local time", "%s", text);
    } else {
        console_row("Local time", "not synchronised ('wifi connect <ssid> [password]')");
    }

    console_section("Memory");
    console_row("Internal SRAM", "%.1f KB free, largest block %.1f KB",
                heap_caps_get_free_size(MALLOC_CAP_INTERNAL) / 1024.0,
                heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL) / 1024.0);
    console_row("PSRAM", "%.2f MB free, largest block %.2f MB",
                heap_caps_get_free_size(MALLOC_CAP_SPIRAM) / 1048576.0,
                heap_caps_get_largest_free_block(MALLOC_CAP_SPIRAM) / 1048576.0);

    console_section("Software");
    char elf_sha[9];
    esp_app_get_elf_sha256(elf_sha, sizeof(elf_sha));
    console_row("Firmware", "%s (ELF %s)", esp_app_get_description()->version, elf_sha);
    console_row("ESP-IDF", "v%d.%d.%d (%s)", ESP_IDF_VERSION_MAJOR, ESP_IDF_VERSION_MINOR, ESP_IDF_VERSION_PATCH,
                esp_get_idf_version());
    console_row("LVGL", "%d.%d.%d", lv_version_major(), lv_version_minor(), lv_version_patch());

    console_section("Battery (ADC, GPIO39)");
    console_row("Voltage", "%.2f V", vbatt);
    console_row("Charge", "%.0f %% (LiFePO4 table)", getBatteryPercentage(vbatt));

    console_section("Temperatures");
    if (isnan(esp_temp)) console_row(ESP32_model_str(), "read error");
    else                 console_row(ESP32_model_str(), "%.1f °C (indicative: uncalibrated sensor)", esp_temp);
    printf("\n");
    return 0;
}

static const command_t restart_cmd = {"restart", "Restart the ESP32-WROOM-32E-N8R2.", nullptr, 0, -1, &cmd_restart};
static const command_t detail_cmd = {
    "detail", "System status: ESP32 (revision, MAC, last reset, uptime, time), memory, software versions, battery "
              "(ADC) and ESP32 temperature (indicative).",
    nullptr, 0, -1, &cmd_detail};

// ============================================================================
// wifi
// ============================================================================

static int wifi_list(int argc, char **argv) { MyWifiList(); return 0; }
static int wifi_scan(int argc, char **argv) { MyWifiScan(); return 0; }

static int wifi_connect(int argc, char **argv) {
    const esp_err_t err = MyWifiConnect(argv[1], argc > 2 ? argv[2] : "");
    char ssid[33];
    printf("WiFi \"%s\" saved, time sync: %s\n", console_safe_text(ssid, sizeof(ssid), argv[1]), esp_err_to_name(err));
    return err == ESP_OK ? 0 : 1;
}

static int wifi_forget(int argc, char **argv) {
    const esp_err_t err = MyWifiForget(argv[1]);
    if (err != ESP_OK) {
        char ssid[33];
        printf("WiFi \"%s\": %s\n", console_safe_text(ssid, sizeof(ssid), argv[1]), esp_err_to_name(err));
    }
    return err == ESP_OK ? 0 : 1;
}

static const subcmd_t wifi_subs[] = {
    {"list", "", 0, 0, &wifi_list, "Saved networks (max 10, passwords not shown)."},
    {"scan", "", 0, 0, &wifi_scan, "Networks in range with their signal strength."},
    {"connect", "<ssid> [password]", 1, 2, &wifi_connect,
     "Add or update a saved network, then connect and sync the time (quote an SSID with spaces)."},
    {"forget", "<ssid|number>", 1, 1, &wifi_forget, "Delete a saved network (number shown by 'wifi list')."},
};
static const command_t wifi_cmd = {"wifi", "WiFi networks used for the time sync (SNTP).", wifi_subs,
                                   COUNT_OF(wifi_subs), 0, nullptr};

// ============================================================================
// ui
// ============================================================================

static int ui_perf(int argc, char **argv) {
    const bool off = (argc >= 2);
    if (off && strcasecmp(argv[1], "off") != 0) return USAGE_ERROR;
    Gui_requestUiPerf(!off);
    printf(off ? "ui perf stopped\n" : "ui perf: one line per second ('ui perf off' to stop)\n");
    return 0;
}

static int ui_benchmark(int argc, char **argv) {
    Gui_requestBenchmark();
    printf("Results on this console at the end ('restart' to go back to the user interface).\n");
    return 0;
}

static const subcmd_t ui_subs[] = {
    {"perf", "[off]", 0, 1, &ui_perf,
     "Print LVGL FPS / CPU / render / flush of the displayed screen every second; off stops it."},
    {"benchmark", "", 0, 0, &ui_benchmark,
     "Run the LVGL benchmark on the display and print its results ('restart' to exit)."},
};
static const command_t ui_cmd = {"ui", "User interface tests over the serial port.", ui_subs, COUNT_OF(ui_subs), -1,
                                 nullptr};

// ============================================================================
// touch
// ============================================================================

static int touch_cal(int argc, char **argv) { Gui_requestTouchCalibrate(); return 0; }
static int touch_test(int argc, char **argv) { Gui_requestTouchTest(); return 0; }

static const subcmd_t touch_subs[] = {
    {"cal", "", 0, 0, &touch_cal, "Calibrate the touch screen (touch the 4 corners)."},
    {"test", "", 0, 0, &touch_test, "Print the touched points for 10 s to check the precision."},
};
static const command_t touch_cmd = {"touch", "Touch screen calibration and test.", touch_subs, COUNT_OF(touch_subs),
                                    -1, nullptr};

// ============================================================================
// COMMAND REGISTRATION AND INTERACTIVE CONSOLE (REPL)
// ============================================================================

void register_custom_commands(void) {
    register_command(&detail_cmd);
    register_command(&restart_cmd);
    register_command(&wifi_cmd);
    register_command(&ui_cmd);
    register_command(&touch_cmd);
}

static esp_console_repl_t *s_repl = NULL;

// Prepares the console (commands registered) without opening it: see console_start
void console_init(void) {
    esp_console_repl_config_t repl_config = ESP_CONSOLE_REPL_CONFIG_DEFAULT();

    repl_config.prompt = BGRN CONFIG_IDF_TARGET ">" AINSIrst;
    repl_config.max_cmdline_length = CONFIG_CONSOLE_MAX_COMMAND_LINE_LENGTH;
    repl_config.task_stack_size = 6144;   // wifi connect / scan run in the console task
    repl_config.task_core_id = 0;         // Core 1: LVGL loop (app_main)

    esp_console_register_help_command();
    register_custom_commands();

    // UART0 (CP2102N), 115200 bauds: installs the UART driver and its VFS
    esp_console_dev_uart_config_t hw_config = ESP_CONSOLE_DEV_UART_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_console_new_repl_uart(&hw_config, &repl_config, &s_repl));
    // esp_console_new_repl_uart probes the terminal (escape sequence) and, without an answer, switches linenoise to
    // "dumb mode" for the whole session: no history (up/down arrows) and no line editing. At boot the terminal is
    // often not open yet (reset by a flash): the normal mode is kept, the console is used with an ANSI terminal
    // (Tera Term, see the banner).
    linenoiseSetDumbMode(0);
}

// End of the initialisation (WiFi stopped after the time sync): banner, then prompt
void console_start(void) {
    if (s_repl == NULL) return;
    printf("\n");
    console_displayIntro();
    ESP_ERROR_CHECK(esp_console_start_repl(s_repl));
}
