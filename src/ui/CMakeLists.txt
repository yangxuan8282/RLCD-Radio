SET(SOURCES ui_Monitor.c
    ui_connect.c
    ui.c
    ui_comp_hook.c
    ui_helpers.c
    fonts/ui_font_Font1.c)

add_library(ui ${SOURCES})
