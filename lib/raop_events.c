/*
 * raop_events: the AirPlay "event channel", receiver to client (see raop_events.h).
 *
 * Copyright (c) 2026 Parker Wahle
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 */

#include "raop_events.h"

#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "compat.h"
#include "sockets.h"
#include "netutils.h"
#include "threads.h"
#include "plist/plist.h"

struct raop_events_s {
    logger_t *logger;
    int listen_sock;
    int sock; /* the client's connection, or -1 */
    unsigned int cseq;
    bool running;
    thread_handle_t thread;
    mutex_handle_t lock;
};

/* Logs what the client sends on the channel and answers its RTSP requests.
 * Anything that isn't readable RTSP means the channel is encrypted. */
static void handle_incoming(raop_events_t *events, const char *buf, int len) {
    char line[256];
    int n = len < (int) sizeof line - 1 ? len : (int) sizeof line - 1;
    bool text = len >= 4 && (!strncmp(buf, "RTSP", 4) || !strncmp(buf, "POST", 4) || !strncmp(buf, "GET ", 4));
    if (!text) {
        logger_log(events->logger, LOGGER_INFO, "raop_events: %d bytes not readable as RTSP (encrypted?)", len);
        return;
    }
    memcpy(line, buf, n);
    line[n] = '\0';
    for (char *p = line; *p; p++) {
        if (*p == '\r' || *p == '\n') *p = ' ';
    }
    logger_log(events->logger, LOGGER_INFO, "raop_events: received: %s", line);
    if (strncmp(buf, "RTSP", 4) != 0) {
        /* a request from the client: acknowledge it */
        const char *cseq = strstr(buf, "CSeq:");
        char reply[96];
        int cseq_value = cseq ? atoi(cseq + 5) : 0;
        int rlen = snprintf(reply, sizeof reply, "RTSP/1.0 200 OK\r\nCSeq: %d\r\n\r\n", cseq_value);
        MUTEX_LOCK(events->lock);
        if (events->sock >= 0) send(events->sock, reply, rlen, 0);
        MUTEX_UNLOCK(events->lock);
    }
}

static THREAD_RETVAL events_thread(void *arg) {
    raop_events_t *events = arg;
    while (events->running) {
        fd_set fds;
        struct timeval tv = {0, 500000};
        int fd = events->sock >= 0 ? events->sock : events->listen_sock;
        FD_ZERO(&fds);
        FD_SET(fd, &fds);
        if (select(fd + 1, &fds, NULL, NULL, &tv) <= 0) continue;
        if (fd == events->listen_sock) {
            int s = accept(events->listen_sock, NULL, NULL);
            if (s >= 0) {
                MUTEX_LOCK(events->lock);
                events->sock = s;
                MUTEX_UNLOCK(events->lock);
                logger_log(events->logger, LOGGER_INFO, "raop_events: client connected to the event channel");
            }
        } else {
            char buf[2048];
            int len = (int) recv(fd, buf, sizeof buf, 0);
            if (len <= 0) {
                logger_log(events->logger, LOGGER_INFO, "raop_events: client closed the event channel");
                MUTEX_LOCK(events->lock);
                CLOSESOCKET(events->sock);
                events->sock = -1;
                MUTEX_UNLOCK(events->lock);
                continue;
            }
            handle_incoming(events, buf, len);
        }
    }
    return 0;
}

raop_events_t *raop_events_init(logger_t *logger, int use_ipv6, unsigned short *port) {
    raop_events_t *events = calloc(1, sizeof *events);
    unsigned short p = 0;
    if (!events) return NULL;
    events->logger = logger;
    events->sock = -1;
    events->listen_sock = netutils_init_socket(&p, use_ipv6, 0);
    if (events->listen_sock < 0 || listen(events->listen_sock, 1) < 0) {
        if (events->listen_sock >= 0) CLOSESOCKET(events->listen_sock);
        free(events);
        return NULL;
    }
    MUTEX_CREATE(events->lock);
    events->running = true;
    THREAD_CREATE(events->thread, events_thread, events);
    *port = p;
    logger_log(logger, LOGGER_INFO, "raop_events: event channel listening on TCP port %u", p);
    return events;
}

void raop_events_destroy(raop_events_t *events) {
    if (!events) return;
    events->running = false;
    THREAD_JOIN(events->thread);
    if (events->sock >= 0) CLOSESOCKET(events->sock);
    CLOSESOCKET(events->listen_sock);
    MUTEX_DESTROY(events->lock);
    free(events);
}

int raop_events_force_keyframe(raop_events_t *events) {
    char header[256];
    char *body = NULL;
    uint32_t body_len = 0;
    int header_len, ret = -1;
    plist_t root;
    if (!events) return -1;
    root = plist_new_dict();
    plist_dict_set_item(root, "type", plist_new_string("forceKeyFrame"));
    plist_to_bin(root, &body, &body_len);
    plist_free(root);
    MUTEX_LOCK(events->lock);
    if (events->sock >= 0 && body) {
        header_len = snprintf(header, sizeof header,
                              "POST /command RTSP/1.0\r\nCSeq: %u\r\n"
                              "Content-Type: application/x-apple-binary-plist\r\nContent-Length: %u\r\n\r\n",
                              ++events->cseq, body_len);
        if (send(events->sock, header, header_len, 0) == header_len &&
            send(events->sock, body, body_len, 0) == (int) body_len) {
            ret = 0;
        }
    }
    MUTEX_UNLOCK(events->lock);
    logger_log(events->logger, LOGGER_INFO, "raop_events: forceKeyFrame %s", ret == 0 ? "sent" : "not sent (no client)");
    plist_mem_free(body);
    return ret;
}
