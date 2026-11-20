#ifndef _CONSOLE_H_
    #define _CONSOLE_H_

    #include <stdint.h>
    #include <stddef.h>
    #include <stdio.h>
    #include "ainsi-color-codes.h"

    // Serial console: esp_console + linenoise (line editing, history with the arrows, Tab completion, help)
    // Same structure as esp32-mezz-jauges/Firmware/main/Console.cpp
    void console_init(void);       // Registers the commands (console not opened yet)
    void console_start(void);      // End of the initialisation: banner, then prompt
    void console_displayIntro(void);
    void register_custom_commands(void);

    // Common display of the commands: coloured section title, then aligned "label  value" rows
    void console_section(const char *title);
    void console_row(const char *label, const char *format, ...) __attribute__((format(printf, 2, 3)));

    // Copies text that comes from outside (SSID received over the air) with the control characters
    // (ESC included: terminal escape sequences) replaced by '?'. Returns dst.
    char *console_safe_text(char *dst, size_t size, const char *src);

#endif
