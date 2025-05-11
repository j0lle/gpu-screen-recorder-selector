#include "dbus_client.h"
#include "../protocol.h"

#include <sys/socket.h>
#include <sys/wait.h>
#include <sys/prctl.h>
#include <unistd.h>
#include <poll.h>

#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <errno.h>

// TODO: Error checking for write/read

static bool gsr_dbus_client_wait_for_startup(gsr_dbus_client *self) {
    struct pollfd poll_fd = {
        .fd = self->socket_pair[0],
        .events = POLLIN,
        .revents = 0
    };
    for(;;) {
        int poll_res = poll(&poll_fd, 1, 100);
        if(poll_res > 0 && (poll_fd.revents & POLLIN)) {
            char msg;
            read(self->socket_pair[0], &msg, 1);
            return true;
        } else {
            int status = 0;
            int wait_result = waitpid(self->pid, &status, WNOHANG);
            if(wait_result != 0) {
                int exit_code = -1;
                if(WIFEXITED(status))
                    exit_code = WEXITSTATUS(status);
                fprintf(stderr, "gsr error: gsr_dbus_client_init: server side or never started, exit code: %d\n", exit_code);
                self->pid = 0;
                return false;
            }
        }
    }
}

bool gsr_dbus_client_init(gsr_dbus_client *self, const char *screencast_restore_token) {
    memset(self, 0, sizeof(*self));

    if(socketpair(AF_UNIX, SOCK_STREAM, 0, self->socket_pair) == -1) {
        fprintf(stderr, "gsr error: gsr_dbus_client_init: socketpair failed, error: %s\n", strerror(errno));
        return false;
    }

    if(screencast_restore_token) {
        self->screencast_restore_token = strdup(screencast_restore_token);
        if(!self->screencast_restore_token) {
            fprintf(stderr, "gsr error: gsr_dbus_client_init: failed to clone restore token\n");
            gsr_dbus_client_deinit(self);
            return false;
        }
    }

    self->pid = fork();
    if(self->pid == -1) {
        fprintf(stderr, "gsr error: gsr_dbus_client_init: failed to fork process\n");
        gsr_dbus_client_deinit(self);
        return false;
    } else if(self->pid == 0) { /* child */
        char socket_pair_server_str[32];
        snprintf(socket_pair_server_str, sizeof(socket_pair_server_str), "%d", self->socket_pair[1]);

        /* Needed for NixOS for example, to make sure gsr-dbus-server doesn't inherit cap_sys_nice */
        prctl(PR_CAP_AMBIENT, PR_CAP_AMBIENT_CLEAR_ALL, 0, 0, 0);

        const char *args[] = { "gsr-dbus-server", socket_pair_server_str, self->screencast_restore_token ? self->screencast_restore_token : "", NULL };
        execvp(args[0], (char *const*)args);

        fprintf(stderr, "gsr error: gsr_dbus_client_init: execvp failed, error: %s\n", strerror(errno));
        _exit(127);
    } else { /* parent */
        if(!gsr_dbus_client_wait_for_startup(self)) {
            gsr_dbus_client_deinit(self);
            return false;
        }
    }

    return true;
}

void gsr_dbus_client_deinit(gsr_dbus_client *self) {
    for(int i = 0; i < 2; ++i) {
        if(self->socket_pair[i] > 0) {
            close(self->socket_pair[i]);
            self->socket_pair[i] = -1;
        }
    }

    if(self->screencast_restore_token) {
        free(self->screencast_restore_token);
        self->screencast_restore_token = NULL;
    }

    if(self->pid > 0) {
        kill(self->pid, SIGKILL);
        int status = 0;
        waitpid(self->pid, &status, 0);
        self->pid = 0;
    }
}

int gsr_dbus_client_screencast_create_session(gsr_dbus_client *self, char *session_handle, size_t session_handle_size) {
    const gsr_dbus_request_message request = {
        .protocol_version = GSR_DBUS_PROTOCOL_VERSION,
        .type = GSR_DBUS_MESSAGE_REQ_CREATE_SESSION,
        .create_session = (gsr_dbus_message_req_create_session) {}
    };
    write(self->socket_pair[0], &request, sizeof(request));

    gsr_dbus_response_message response = {0};
    read(self->socket_pair[0], &response, sizeof(response));

    if(response.protocol_version != GSR_DBUS_PROTOCOL_VERSION) {
        fprintf(stderr, "gsr error: gsr_dbus_client_screencast_create_session: server uses protocol version %d while the client is using protocol version %d", response.protocol_version, GSR_DBUS_PROTOCOL_VERSION);
        return -1;
    }

    if(response.type == GSR_DBUS_MESSAGE_RESP_ERROR) {
        fprintf(stderr, "gsr error: gsr_dbus_client_screencast_create_session: server return error: %s (%d)\n", response.error.message, (int)response.error.error_code);
        return response.error.error_code;
    }

    if(response.type != GSR_DBUS_MESSAGE_RESP_CREATE_SESSION) {
        fprintf(stderr, "gsr error: gsr_dbus_client_screencast_create_session: received incorrect response type. Expected %d got %d\n", GSR_DBUS_MESSAGE_RESP_CREATE_SESSION, response.type);
        return -1;
    }

    snprintf(session_handle, session_handle_size, "%s", response.create_session.session_handle);
    return 0;
}

int gsr_dbus_client_screencast_select_sources(gsr_dbus_client *self, const char *session_handle, uint32_t capture_type, uint32_t cursor_mode) {
    gsr_dbus_request_message request = {
        .protocol_version = GSR_DBUS_PROTOCOL_VERSION,
        .type = GSR_DBUS_MESSAGE_REQ_SELECT_SOURCES,
        .select_sources = (gsr_dbus_message_req_select_sources) {
            .capture_type = capture_type,
            .cursor_mode = cursor_mode
        }
    };
    snprintf(request.select_sources.session_handle, sizeof(request.select_sources.session_handle), "%s", session_handle);
    write(self->socket_pair[0], &request, sizeof(request));

    gsr_dbus_response_message response = {0};
    read(self->socket_pair[0], &response, sizeof(response));

    if(response.protocol_version != GSR_DBUS_PROTOCOL_VERSION) {
        fprintf(stderr, "gsr error: gsr_dbus_client_screencast_select_sources: server uses protocol version %d while the client is using protocol version %d", response.protocol_version, GSR_DBUS_PROTOCOL_VERSION);
        return -1;
    }

    if(response.type == GSR_DBUS_MESSAGE_RESP_ERROR) {
        fprintf(stderr, "gsr error: gsr_dbus_client_screencast_select_sources: server return error: %s (%d)\n", response.error.message, (int)response.error.error_code);
        return response.error.error_code;
    }

    if(response.type != GSR_DBUS_MESSAGE_RESP_SELECT_SOURCES) {
        fprintf(stderr, "gsr error: gsr_dbus_client_screencast_select_sources: received incorrect response type. Expected %d got %d\n", GSR_DBUS_MESSAGE_RESP_SELECT_SOURCES, response.type);
        return -1;
    }

    return 0;
}

int gsr_dbus_client_screencast_start(gsr_dbus_client *self, const char *session_handle, uint32_t *pipewire_node) {
    *pipewire_node = 0;

    gsr_dbus_request_message request = {
        .protocol_version = GSR_DBUS_PROTOCOL_VERSION,
        .type = GSR_DBUS_MESSAGE_REQ_START,
        .start = (gsr_dbus_message_req_start) {}
    };
    snprintf(request.start.session_handle, sizeof(request.start.session_handle), "%s", session_handle);
    write(self->socket_pair[0], &request, sizeof(request));

    gsr_dbus_response_message response = {0};
    read(self->socket_pair[0], &response, sizeof(response));

    if(response.protocol_version != GSR_DBUS_PROTOCOL_VERSION) {
        fprintf(stderr, "gsr error: gsr_dbus_client_screencast_start: server uses protocol version %d while the client is using protocol version %d", response.protocol_version, GSR_DBUS_PROTOCOL_VERSION);
        return -1;
    }

    if(response.type == GSR_DBUS_MESSAGE_RESP_ERROR) {
        fprintf(stderr, "gsr error: gsr_dbus_client_screencast_start: server return error: %s (%d)\n", response.error.message, (int)response.error.error_code);
        return response.error.error_code;
    }

    if(response.type != GSR_DBUS_MESSAGE_RESP_START) {
        fprintf(stderr, "gsr error: gsr_dbus_client_screencast_start: received incorrect response type. Expected %d got %d\n", GSR_DBUS_MESSAGE_RESP_START, response.type);
        return -1;
    }

    if(self->screencast_restore_token) {
        free(self->screencast_restore_token);
        if(response.start.restore_token[0] == '\0')
            self->screencast_restore_token = NULL;
        else
            self->screencast_restore_token = strdup(response.start.restore_token);
    }

    *pipewire_node = response.start.pipewire_node;
    return 0;
}

bool gsr_dbus_client_screencast_open_pipewire_remote(gsr_dbus_client *self, const char *session_handle, int *pipewire_fd) {
    *pipewire_fd = 0;

    gsr_dbus_request_message request = {
        .protocol_version = GSR_DBUS_PROTOCOL_VERSION,
        .type = GSR_DBUS_MESSAGE_REQ_OPEN_PIPEWIRE_REMOTE,
        .open_pipewire_remote = (gsr_dbus_message_req_open_pipewire_remote) {}
    };
    snprintf(request.open_pipewire_remote.session_handle, sizeof(request.open_pipewire_remote.session_handle), "%s", session_handle);
    write(self->socket_pair[0], &request, sizeof(request));

    gsr_dbus_response_message response = {0};
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

    const int bla = recvmsg(self->socket_pair[0], &message, MSG_WAITALL);
    (void)bla;

    if(response.protocol_version != GSR_DBUS_PROTOCOL_VERSION) {
        fprintf(stderr, "gsr error: gsr_dbus_client_screencast_open_pipewire_remote: server uses protocol version %d while the client is using protocol version %d", response.protocol_version, GSR_DBUS_PROTOCOL_VERSION);
        return false;
    }

    if(response.type == GSR_DBUS_MESSAGE_RESP_ERROR) {
        fprintf(stderr, "gsr error: gsr_dbus_client_screencast_open_pipewire_remote: server return error: %s (%d)\n", response.error.message, (int)response.error.error_code);
        return false;
    }

    if(response.type != GSR_DBUS_MESSAGE_RESP_OPEN_PIPEWIRE_REMOTE) {
        fprintf(stderr, "gsr error: gsr_dbus_client_screencast_open_pipewire_remote: received incorrect response type. Expected %d got %d\n", GSR_DBUS_MESSAGE_RESP_OPEN_PIPEWIRE_REMOTE, response.type);
        return false;
    }

    struct cmsghdr *cmsg = CMSG_FIRSTHDR(&message);
    if(!cmsg || cmsg->cmsg_type != SCM_RIGHTS) {
        fprintf(stderr, "gsr error: gsr_dbus_client_screencast_open_pipewire_remote: returned message data is missing file descriptor\n");
        return false;
    }

    memcpy(pipewire_fd, CMSG_DATA(cmsg), sizeof(*pipewire_fd));
    return true;
}

const char* gsr_dbus_client_screencast_get_restore_token(gsr_dbus_client *self) {
    return self->screencast_restore_token;
}
