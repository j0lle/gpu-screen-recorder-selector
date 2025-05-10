#include "../dbus_impl.h"
#include "../protocol.h"

#include <stdio.h>
#include <stdlib.h>

#include <unistd.h>
#include <sys/socket.h>

/* TODO: Error check write/read */

static int handle_create_session(gsr_dbus *dbus, int rpc_fd, const gsr_dbus_message_req_create_session *create_session) {
    (void)create_session;
    char *session_handle = NULL;
    const int status = gsr_dbus_screencast_create_session(dbus, &session_handle);
    if(status == 0) {
        gsr_dbus_response_message response = {
            .protocol_version = GSR_DBUS_PROTOCOL_VERSION,
            .type = GSR_DBUS_MESSAGE_RESP_CREATE_SESSION,
            .create_session = (gsr_dbus_message_resp_create_session) {}
        };
        snprintf(response.create_session.session_handle, sizeof(response.create_session.session_handle), "%s", session_handle);
        free(session_handle);
        write(rpc_fd, &response, sizeof(response));
    }
    return status;
}

static int handle_select_sources(gsr_dbus *dbus, int rpc_fd, const gsr_dbus_message_req_select_sources *select_sources) {
    const int status = gsr_dbus_screencast_select_sources(dbus, select_sources->session_handle, select_sources->capture_type, select_sources->cursor_mode);
    if(status == 0) {
        gsr_dbus_response_message response = {
            .protocol_version = GSR_DBUS_PROTOCOL_VERSION,
            .type = GSR_DBUS_MESSAGE_RESP_SELECT_SOURCES,
            .select_sources = (gsr_dbus_message_resp_select_sources) {}
        };
        write(rpc_fd, &response, sizeof(response));
    }
    return status;
}

static int handle_start(gsr_dbus *dbus, int rpc_fd, const gsr_dbus_message_req_start *start) {
    uint32_t pipewire_node = 0;
    const int status = gsr_dbus_screencast_start(dbus, start->session_handle, &pipewire_node);
    if(status == 0) {
        const char *screencast_restore_token = gsr_dbus_screencast_get_restore_token(dbus);
        gsr_dbus_response_message response = {
            .protocol_version = GSR_DBUS_PROTOCOL_VERSION,
            .type = GSR_DBUS_MESSAGE_RESP_START,
            .start = (gsr_dbus_message_resp_start) {
                .pipewire_node = pipewire_node
            }
        };
        snprintf(response.start.restore_token, sizeof(response.start.restore_token), "%s", screencast_restore_token ? screencast_restore_token : "");
        write(rpc_fd, &response, sizeof(response));
    }
    return status;
}

static bool handle_open_pipewire_remote(gsr_dbus *dbus, int rpc_fd, const gsr_dbus_message_req_open_pipewire_remote *open_pipewire_remote) {
    int pipewire_fd = 0;
    const bool success = gsr_dbus_screencast_open_pipewire_remote(dbus, open_pipewire_remote->session_handle, &pipewire_fd);
    if(success) {
        gsr_dbus_response_message response = {
            .protocol_version = GSR_DBUS_PROTOCOL_VERSION,
            .type = GSR_DBUS_MESSAGE_RESP_OPEN_PIPEWIRE_REMOTE,
            .open_pipewire_remote = (gsr_dbus_message_resp_open_pipewire_remote) {}
        };

        struct iovec iov = {
            .iov_base = &response,
            .iov_len = sizeof(response)
        };

        char msg_control[CMSG_SPACE(sizeof(int))];

        struct msghdr message = {
            .msg_iov = &iov,
            .msg_iovlen = 1,
            .msg_control = msg_control,
            .msg_controllen = sizeof(msg_control)
        };

        struct cmsghdr *cmsg = CMSG_FIRSTHDR(&message);
        cmsg->cmsg_level = SOL_SOCKET;
        cmsg->cmsg_type = SCM_RIGHTS;
        cmsg->cmsg_len = CMSG_LEN(sizeof(int));
        int *fds = (int*)CMSG_DATA(cmsg);
        fds[0] = pipewire_fd;
        message.msg_controllen = cmsg->cmsg_len;
        sendmsg(rpc_fd, &message, 0);
    }
    return success;
}

int main(int argc, char **argv) {
    if(argc != 3) {
        fprintf(stderr, "usage: gsr-dbus-server <rpc-fd> <screencast-restore-token>\n");
        return 1;
    }

    const char *rpc_fd_str = argv[1];
    const char *screencast_restore_token = argv[2];

    int rpc_fd = -1;
    if(sscanf(rpc_fd_str, "%d", &rpc_fd) != 1) {
        fprintf(stderr, "gsr-dbus-server error: rpc-fd is not a number: %s\n", rpc_fd_str);
        return 1;
    }

    if(screencast_restore_token[0] == '\0')
        screencast_restore_token = NULL;

    gsr_dbus dbus;
    if(!gsr_dbus_init(&dbus, screencast_restore_token))
        return 1;

    /* Tell client we have started up */
    write(rpc_fd, "S", 1);

    gsr_dbus_request_message request;
    for(;;) {
        read(rpc_fd, &request, sizeof(request));

        if(request.protocol_version != GSR_DBUS_PROTOCOL_VERSION) {
            gsr_dbus_response_message response = {
                .protocol_version = GSR_DBUS_PROTOCOL_VERSION,
                .type = GSR_DBUS_MESSAGE_RESP_ERROR,
                .error = (gsr_dbus_message_resp_error) {
                    .error_code = 1
                }
            };
            snprintf(response.error.message, sizeof(response.error.message), "Client uses protocol version %d while the server is using protocol version %d", request.protocol_version, GSR_DBUS_PROTOCOL_VERSION);
            fprintf(stderr, "gsr-dbus-server error: %s\n", response.error.message);
            write(rpc_fd, &response, sizeof(response));
            continue;
        }

        int status = 0;
        switch(request.type) {
            case GSR_DBUS_MESSAGE_REQ_CREATE_SESSION: {
                status = handle_create_session(&dbus, rpc_fd, &request.create_session);
                break;
            }
            case GSR_DBUS_MESSAGE_REQ_SELECT_SOURCES: {
                status = handle_select_sources(&dbus, rpc_fd, &request.select_sources);
                break;
            }
            case GSR_DBUS_MESSAGE_REQ_START: {
                status = handle_start(&dbus, rpc_fd, &request.start);
                break;
            }
            case GSR_DBUS_MESSAGE_REQ_OPEN_PIPEWIRE_REMOTE: {
                if(!handle_open_pipewire_remote(&dbus, rpc_fd, &request.open_pipewire_remote))
                    status = -1;
                break;
            }
        }

        if(status != 0) {
            gsr_dbus_response_message response = {
                .protocol_version = GSR_DBUS_PROTOCOL_VERSION,
                .type = GSR_DBUS_MESSAGE_RESP_ERROR,
                .error = (gsr_dbus_message_resp_error) {
                    .error_code = status
                }
            };
            snprintf(response.error.message, sizeof(response.error.message), "%s", "Failed to handle request");
            write(rpc_fd, &response, sizeof(response));
        }
    }

    gsr_dbus_deinit(&dbus);
    return 0;
}