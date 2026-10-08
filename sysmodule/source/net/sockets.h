#pragma once
#include <switch.h>
#include <arpa/inet.h> // htons
#include <switch/services/bsd.h>

#define SOCKET_INVALID -1

void SocketInit();

void SocketDeinit();

int SocketUdp();

int SocketTcpListen(short port);

typedef enum {
	// Accept succeeded, guaranteed to have a valid socket in outAccepted
	SocketAcceptError_OK,
	// Accept failed, the console was put into sleep mode and the listener needs to be reset
	SocketAcceptError_NetDown,
	// Accept failed, either no pending connections or an error occured, try again
	SocketAcceptError_Fail,
} SocketAcceptResult;

SocketAcceptResult SocketTcpAccept(int listenerHandle, int* outAccepted, struct sockaddr* addr, socklen_t* addrlen);

// Coses a socket, also sets it to SOCKET_INVALID
void SocketClose(int* socket);

// Returns true on success
bool SocketSendAll(int socket, const void* buffer, u32 size);

// Like SocketSendAll; allowIncoming = true does not treat pending incoming data as a disconnection
bool SocketSendAllEx(int socket, const void* buffer, u32 size, bool allowIncoming);

// Non-blocking receive: > 0 bytes read, 0 nothing pending, -1 closed or failed
s32 SocketRecvNonBlocking(int socket, void* buffer, u32 size);

// setsockopt(IPPROTO_IP, IP_TOS); returns true on success
bool SocketSetTos(int socket, int tos);

// Returns true on success
bool SocketUDPSendTo(int socket, const void* data, u32 size, struct sockaddr* addr, socklen_t addrlen);

// Returns true on success
bool SocketSetBroadcast(int socket, bool allow);

// Accounts for the network mask
s32 SocketGetBroadcastAddress();

// Returns 0 on timeout, -1 on error, otherwise the number of bytes received
s32 SocketRecv(int socket, void* buffer, u32 size);

bool SocketRecevExact(int socket, void* buffer, u32 size);

bool SocketMakeNonBlocking(int socket);

int SocketNativeErrno();