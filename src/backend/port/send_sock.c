/*-------------------------------------------------------------------------
 *
 * send_sock.c
 *	  Send socket descriptor to another process
 *
 * Portions Copyright (c) 1996-2018, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * src/backend/port/send_sock.c
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#ifdef WIN32
typedef struct
{
	SOCKET origsocket;
	WSAPROTOCOL_INFO wsainfo;
} InheritableSocket;
#endif

/*
 * Send socket descriptor "sock" to backend process through Unix socket "chan"
 */
int
pg_send_sock(pgsocket chan, pgsocket sock, pid_t pid)
{
#ifdef WIN32
	InheritableSocket dst;
	size_t rc;
	dst.origsocket = sock;
	if (WSADuplicateSocket(sock, pid, &dst.wsainfo) != 0)
	{
		ereport(FATAL,
				(errmsg("could not duplicate socket %d for use in backend: error code %d",
						(int)sock, WSAGetLastError())));
		return -1;
	}
	rc = send(chan, &dst, sizeof(dst), 0);
	if (rc != sizeof(dst))
	{
		ereport(FATAL,
				(errmsg("Failed to send inheritable socket: rc=%d, error code %d",
						(int)rc, WSAGetLastError())));
		return -1;
	}
	return 0;
#else
	struct msghdr msg = { 0 };
	struct iovec io;
	struct cmsghdr * cmsg;
    char buf[CMSG_SPACE(sizeof(sock))];
    memset(buf, '\0', sizeof(buf));

    /* On Mac OS X, the struct iovec is needed, even if it points to minimal data */
    io.iov_base = "";
	io.iov_len = 1;

    msg.msg_iov = &io;
    msg.msg_iovlen = 1;
    msg.msg_control = buf;
    msg.msg_controllen = sizeof(buf);

    cmsg = CMSG_FIRSTHDR(&msg);
	if (!cmsg)
		return PGINVALID_SOCKET;

    cmsg->cmsg_level = SOL_SOCKET;
    cmsg->cmsg_type = SCM_RIGHTS;
    cmsg->cmsg_len = CMSG_LEN(sizeof(sock));

    memcpy(CMSG_DATA(cmsg), &sock, sizeof(sock));
    msg.msg_controllen = cmsg->cmsg_len;

    while (sendmsg(chan, &msg, 0) < 0)
	{
		if (errno != EINTR)
			return PGINVALID_SOCKET;
	}

	return 0;
#endif
}


/*
 * Receive socket descriptor from postmaster process through Unix socket "chan"
 */
pgsocket
pg_recv_sock(pgsocket chan)
{
#ifdef WIN32
	InheritableSocket src;
	SOCKET s;
	size_t rc = recv(chan, &src, sizeof(src), 0);
	if (rc != sizeof(src))
	{
		ereport(FATAL,
				(errmsg("Failed to receive inheritable socket: rc=%d, error code %d",
						(int)rc, WSAGetLastError())));
	}
	s = WSASocket(FROM_PROTOCOL_INFO,
				  FROM_PROTOCOL_INFO,
				  FROM_PROTOCOL_INFO,
				  &src.wsainfo,
				  0,
				  0);
	if (s == INVALID_SOCKET)
	{
		ereport(FATAL,
				(errmsg("could not create inherited socket: error code %d\n",
						WSAGetLastError())));
	}

	/*
	 * To make sure we don't get two references to the same socket, close
	 * the original one. (This would happen when inheritance actually
	 * works..
	 */
	closesocket(src.origsocket);
	return s;
#else
	struct msghdr msg = {0};
    char c_buffer[256];
    char m_buffer[256];
    struct iovec io;
	struct cmsghdr * cmsg;
	pgsocket sock;
	int rc;

	while (true)
	{
		io.iov_base = m_buffer;
		io.iov_len = sizeof(m_buffer);
		msg.msg_iov = &io;
		msg.msg_iovlen = 1;

		msg.msg_control = c_buffer;
		msg.msg_controllen = sizeof(c_buffer);

		rc = recvmsg(chan, &msg, 0);
		if (rc < 0 && errno == EINTR)
			continue;

		if (rc > 0)
			break;

		if (rc == 0)
			elog(WARNING, "Empty datagram is received");
		else
			elog(WARNING, "Failed to receive socket: %m");
		return PGINVALID_SOCKET;
	}

    cmsg = CMSG_FIRSTHDR(&msg);
	if (!cmsg)
	{
		elog(WARNING, "Invalid send socket message");
		return PGINVALID_SOCKET;
	}

    memcpy(&sock, CMSG_DATA(cmsg), sizeof(sock));

	pg_set_noblock(sock);

    return sock;
#endif
}
