add_subdirectory(core)
add_subdirectory(daemon)
add_subdirectory(cli)

# Tether's own front ends. Landline is driven by its GNOME Shell extension and
# ships none of them; the sources are kept as received.
if(LANDLINE_UPSTREAM_FRONTENDS)
    add_subdirectory(ui_common)
    add_subdirectory(gtk)
    if(TETHER_BUILD_TUI)
        add_subdirectory(tui)
    endif()
    add_subdirectory(dialog)
endif()
