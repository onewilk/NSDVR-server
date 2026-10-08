#pragma once
// Streaming loops used when a TCP client asked for the NSDVR extension (FeatureFlags bit 2).
// Clients that do not set the flag never reach this code: the official loop in TCPmode.c runs.

#include "next_ext.h"

#ifdef __cplusplus
extern "C" {
#endif

// WMM AC_VI (CS5)
#define NEXT_IP_TOS_VALUE 0xA0

// Both return when sending fails or streaming is stopped; the caller closes the socket.
void NextSession_Video(int sock, const NextExtConfig* ext);
void NextSession_Audio(int sock, const NextExtConfig* ext);

#ifdef __cplusplus
}
#endif
