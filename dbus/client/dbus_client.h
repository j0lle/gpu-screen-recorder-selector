#ifndef GSR_DBUS_CLIENT_H
#define GSR_DBUS_CLIENT_H

/*
    Using a client-server architecture is needed for dbus because cap_sys_nice doesn't work with desktop portal.
    The main binary has cap_sys_nice and we launch a new child-process without it which uses uses desktop portal.
*/

#include "../portal.h"
#include <stdbool.h>
#include <stdint.h>
#include <signal.h>

typedef struct {
    int socket_pair[2];
    char *screencast_restore_token;
    pid_t pid;
} gsr_dbus_client;

/* Blocking. TODO: Make non-blocking */
bool gsr_dbus_client_init(gsr_dbus_client *self, const char *screencast_restore_token);
void gsr_dbus_client_deinit(gsr_dbus_client *self);

/* The follow functions should be called in order to setup ScreenCast properly */
/* These functions that return an int return the response status code */
int gsr_dbus_client_screencast_create_session(gsr_dbus_client *self, char *session_handle, size_t session_handle_size);
/*
    |capture_type| is a bitmask of gsr_portal_capture_type values. gsr_portal_capture_type values that are not supported by the desktop portal will be ignored.
    |gsr_portal_cursor_mode| is a bitmask of gsr_portal_cursor_mode values. gsr_portal_cursor_mode values that are not supported will be ignored.
*/
int gsr_dbus_client_screencast_select_sources(gsr_dbus_client *self, const char *session_handle, uint32_t capture_type, uint32_t cursor_mode);
int gsr_dbus_client_screencast_start(gsr_dbus_client *self, const char *session_handle, uint32_t *pipewire_node);
bool gsr_dbus_client_screencast_open_pipewire_remote(gsr_dbus_client *self, const char *session_handle, int *pipewire_fd);
const char* gsr_dbus_client_screencast_get_restore_token(gsr_dbus_client *self);

#endif /* GSR_DBUS_CLIENT_H */
