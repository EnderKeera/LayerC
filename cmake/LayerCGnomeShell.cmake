# GNOME Shell extension lane (docs/Wayland.md, "GNOME Shell extension"):
# plain ESM JavaScript with no build step or dependency. It is installed for
# the user to enable with gnome-extensions; installing never enables it.
include(GNUInstallDirs)
option(LAYERC_INSTALL_GNOME_EXTENSION
    "Install the LayerC GNOME Shell extension under the data directory" ON)
set(LAYERC_GNOME_EXTENSION_UUID layerc@enderkeera.github.io)
if(LAYERC_INSTALL_GNOME_EXTENSION)
    install(DIRECTORY
        "${PROJECT_SOURCE_DIR}/packaging/gnome-shell/${LAYERC_GNOME_EXTENSION_UUID}"
        DESTINATION "${CMAKE_INSTALL_DATADIR}/gnome-shell/extensions"
        COMPONENT GnomeShellExtension)
endif()
