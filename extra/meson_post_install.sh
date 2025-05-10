#!/bin/sh

# Needed to remove password prompt when recording a monitor (without desktop portal option) on amd/intel or nvidia wayland
/usr/sbin/setcap cap_sys_admin+ep ${MESON_INSTALL_DESTDIR_PREFIX}/bin/gsr-kms-server \
    || echo "\n!!! Please re-run install as root\n"

# This is needed (for EGL_CONTEXT_PRIORITY_HIGH_IMG) to allow gpu screen recorder to run faster than the heaviest application on AMD.
# For example when trying to record a game at 60 fps and the game drops to 45 fps in some place that would also make gpu screen recorder
# drop to 45 fps unless this setcap is used. Recording would also drop to below 60 fps in some games even though they run above 60 fps.
/usr/sbin/setcap cap_sys_nice+ep ${MESON_INSTALL_DESTDIR_PREFIX}/bin/gpu-screen-recorder
