#pragma once
// Minimal WinSock2 stand-in.  The player does not use networking; this exists
// only so that Bedrock's includes resolve.
#include "Windows.h"
typedef int socklen_t_compat;
struct sockaddr { unsigned short sa_family; char sa_data[14]; };
struct fd_set_compat { unsigned int fd_count; SOCKET fd_array[64]; };
#define AF_INET 2
#define SOCK_STREAM 1
#define SOCK_DGRAM 2
#define INADDR_ANY 0
#define WSAGetLastError() 0
#define WSAStartup(a, b) 0
#define WSACleanup() 0
