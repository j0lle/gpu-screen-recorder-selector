#ifndef GSR_DBUS_PROTOCOL_H
#define GSR_DBUS_PROTOCOL_H

#include <stdint.h>

#define GSR_DBUS_PROTOCOL_VERSION 1

typedef enum {
    GSR_DBUS_MESSAGE_REQ_CREATE_SESSION,
    GSR_DBUS_MESSAGE_REQ_SELECT_SOURCES,
    GSR_DBUS_MESSAGE_REQ_START,
    GSR_DBUS_MESSAGE_REQ_OPEN_PIPEWIRE_REMOTE
} gsr_dbus_message_req_type;

typedef struct {

} gsr_dbus_message_req_create_session;

typedef struct {
    char session_handle[128];
    uint32_t capture_type;
    uint32_t cursor_mode;
} gsr_dbus_message_req_select_sources;

typedef struct {
    char session_handle[128];
} gsr_dbus_message_req_start;

typedef struct {
    char session_handle[128];
} gsr_dbus_message_req_open_pipewire_remote;

typedef struct {
    uint8_t protocol_version;
    gsr_dbus_message_req_type type;
    union {
        gsr_dbus_message_req_create_session create_session;
        gsr_dbus_message_req_select_sources select_sources;
        gsr_dbus_message_req_start start;
        gsr_dbus_message_req_open_pipewire_remote open_pipewire_remote;
    };
} gsr_dbus_request_message;

typedef enum {
    GSR_DBUS_MESSAGE_RESP_ERROR,
    GSR_DBUS_MESSAGE_RESP_CREATE_SESSION,
    GSR_DBUS_MESSAGE_RESP_SELECT_SOURCES,
    GSR_DBUS_MESSAGE_RESP_START,
    GSR_DBUS_MESSAGE_RESP_OPEN_PIPEWIRE_REMOTE
} gsr_dbus_message_resp_type;

typedef struct {
    uint32_t error_code;
    char message[128];
} gsr_dbus_message_resp_error;

typedef struct {
    char session_handle[128];
} gsr_dbus_message_resp_create_session;

typedef struct {
    
} gsr_dbus_message_resp_select_sources;

typedef struct {
    char restore_token[128];
    uint32_t pipewire_node;
} gsr_dbus_message_resp_start;

typedef struct {
    
} gsr_dbus_message_resp_open_pipewire_remote;

typedef struct {
    uint8_t protocol_version;
    gsr_dbus_message_resp_type type;
    union {
        gsr_dbus_message_resp_error error;
        gsr_dbus_message_resp_create_session create_session;
        gsr_dbus_message_resp_select_sources select_sources;
        gsr_dbus_message_resp_start start;
        gsr_dbus_message_resp_open_pipewire_remote open_pipewire_remote;
    };
} gsr_dbus_response_message;

#endif /* GSR_DBUS_PROTOCOL_H */
