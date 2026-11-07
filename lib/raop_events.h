/*
 * raop_events: the AirPlay "event channel", receiver to sender.
 *
 * Apple's receivers return an eventPort in the first SETUP response; the
 * client connects to it, and the receiver sends RTSP requests over it. One of
 * them, "forceKeyFrame" (POST /command), asks the client for an IDR frame:
 * CarPlay receivers use it. Mirroring clients send a single IDR per session
 * otherwise, which some decoders and players can't live with.
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU Lesser General Public License as published by
 * the Free Software Foundation; either version 2.1 of the License, or (at
 * your option) any later version.
 */
#ifndef RAOP_EVENTS_H
#define RAOP_EVENTS_H

#include "logger.h"

typedef struct raop_events_s raop_events_t;

/* Listens on a new TCP port (returned in *port) for the client's connection. */
raop_events_t *raop_events_init(logger_t *logger, int use_ipv6, unsigned short *port);
void raop_events_destroy(raop_events_t *events);

/* Sends "forceKeyFrame". Returns 0 if sent, -1 if the client hasn't connected. */
int raop_events_force_keyframe(raop_events_t *events);

#endif
