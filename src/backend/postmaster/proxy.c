#include <unistd.h>
#include <errno.h>

#include "postgres.h"
#include "funcapi.h"
#include "miscadmin.h"
#include "pgstat.h"
#include "postmaster/postmaster.h"
#include "postmaster/proxy.h"
#include "postmaster/fork_process.h"
#include "access/htup_details.h"
#include "replication/walsender.h"
#include "storage/ipc.h"
#include "storage/latch.h"
#include "storage/proc.h"
#include "storage/procarray.h"
#include "utils/builtins.h"
#include "utils/memutils.h"
#include "libpq/libpq.h"
#include "libpq/libpq-be.h"
#include "libpq/pqsignal.h"
#include "tcop/tcopprot.h"
#include "utils/timeout.h"
#include "utils/ps_status.h"
#include "../interfaces/libpq/libpq-fe.h"
#include "../interfaces/libpq/libpq-int.h"

#define INIT_BUF_SIZE	   (64*1024)
#define MAX_READY_EVENTS   128
#define DB_HASH_SIZE	   101
#define PROXY_WAIT_TIMEOUT 1000 /* 1 second */

struct SessionPool;
struct Proxy;

typedef struct
{
	char database[NAMEDATALEN];
	char username[NAMEDATALEN];
}
SessionPoolKey;

/*
 * Channels represent both clients and backends
 */
typedef struct Channel
{
	char*	 buf;
	int		 rx_pos;
	int		 tx_pos;
	int		 tx_size;
	int		 buf_size;
	int		 event_pos;			 /* Position of wait event returned by AddWaitEventToSet */

	Port*	 client_port;		 /* Not null for client, null for server */

	pgsocket backend_socket;
	PGPROC*	 backend_proc;
	int		 backend_pid;
	bool	 backend_is_tainted; /* client changes session context */
	bool	 backend_is_ready;	 /* ready for query */
	bool	 is_interrupted;	 /* client interrupts query execution */
	bool	 is_disconnected;	 /* connection is lost */
	bool	 write_pending;		 /* write request is pending: emulate epoll EPOLLET (edge-triggered) flag */
	bool	 read_pending;		 /* read request is pending: emulate epoll EPOLLET (edge-triggered) flag */
	/* We need to save startup packet response to be able to send it to new connection */
	int		 handshake_response_size;
	char*	 handshake_response;

	struct Channel* peer;
	struct Channel* next;
	struct Proxy*	proxy;
	struct SessionPool* pool;
}
Channel;

/*
 * Control structure for connection proxies (several proxy workers can be launched and each has it sown proxy instance).
 * Proxy contains hash of session pools for reach role/dbname combination.
 */
typedef struct Proxy
{
	MemoryContext memctx;		 /* Memory context for this proxy (used only in single thread) */
	MemoryContext tmpctx;		 /* Temporary memory context used for parsing startup packet */
	WaitEventSet* wait_events;	 /* Set of socket descriptors of backends and clients socket descriptors */
	HTAB*	 pools;				 /* Session pool map with dbname/role used as a key */
	int		 n_accepted_connections; /* Number of accepted, but not yet established connections
									  * (startup packet is not received and db/role are not known) */
	int		 max_backends;		 /* Maximal number of backends per database */
	bool	 shutdown;			 /* Shutdown flag */
	Channel* hangout;			 /* List of disconnected backends */
	ConnectionProxyState* state; /* State of proxy */
} Proxy;

/*
 * Connection pool to particular role/dbname
 */
typedef struct SessionPool
{
	SessionPoolKey key;
	Channel* idle_backends;		  /* List of idle clients */
	Channel* pending_clients;	  /* List of clients waiting for free backend */
	Proxy*	 proxy;				  /* Owner of this pool */
	int		 n_launched_backends; /* Total number of launched backends */
	int		 n_idle_backends;	  /* Number of backends in idle state */
	int		 n_connected_clients; /* Total number of connected clients */
	int		 n_idle_clients;	  /* Number of clients in idle state */
	int		 n_pending_clients;	  /* Number of clients waiting for free backend */
}
SessionPool;

static void channel_remove(Channel* chan);
static Channel* backend_start(SessionPool* pool);
static bool channel_read(Channel* chan);
static bool channel_write(Channel* chan, bool synchronous);

/*
 * #define ELOG(severity, fmt,...) elog(severity, "PROXY: " fmt, ## __VA_ARGS__)
 */
//#define ELOG(severity,fmt,...)
#define ELOG(severity, fmt,...) elog(severity, "PROXY: " fmt, ## __VA_ARGS__)

static Proxy* proxy;
int MyProxyId;
pgsocket MyProxySocket;
ConnectionProxyState* ProxyState;

/**
 * Backend is ready for next command outside transaction block (idle state).
 * Now if backend is not tainted it is possible to schedule some other client to this backend.
 */
static bool
backend_reschedule(Channel* chan, bool is_new)
{
	chan->backend_is_ready = false;
	if (chan->backend_proc == NULL) /* Lazy resolving of PGPROC entry */
	{
		Assert(chan->backend_pid != 0);
		chan->backend_proc = BackendPidGetProc(chan->backend_pid);
		Assert(chan->backend_proc); /* If backend completes execution of some query, then it has definitely registered itself in procarray */
	}
	if (is_new || !chan->backend_proc->is_tainted) /* If backend is not storing some session context */
	{
		Channel* pending = chan->pool->pending_clients;
		Assert(!chan->backend_is_tainted);
		if (chan->peer)
			chan->peer->peer = NULL;
		chan->pool->n_idle_clients += 1;
		if (pending)
		{
			/* Has pending clients: serve one of them */
			ELOG(LOG, "Backed %d is reassigned to client %p", chan->backend_pid, pending);
			chan->pool->pending_clients = pending->next;
			Assert(chan != pending);
			chan->peer = pending;
			pending->peer = chan;
			chan->pool->n_pending_clients -= 1;
			if (pending->tx_size == 0) /* new client has sent startup packet and we now need to send handshake response */
			{
				Assert(chan->handshake_response != NULL); /* backend already sent handshake response */
				Assert(chan->handshake_response_size < chan->buf_size);
				memcpy(chan->buf, chan->handshake_response, chan->handshake_response_size);
				chan->rx_pos = chan->tx_size = chan->handshake_response_size;
				ELOG(LOG, "Simulate response for startup packet to client %p", pending);
				chan->backend_is_ready = true;
				return channel_write(pending, false);
			}
			else
			{
				ELOG(LOG, "Try to send pending request from client %p to backend %p (pid %d)", pending, chan, chan->backend_pid);
				Assert(pending->tx_pos == 0 && pending->rx_pos >= pending->tx_size);
				return channel_write(chan, false); /* Send pending request to backend */
			}
		}
		else /* return backend to the list of idle backends */
		{
			ELOG(LOG, "Backed %d is idle", chan->backend_pid);
			Assert(!chan->client_port);
			chan->next = chan->pool->idle_backends;
			chan->pool->idle_backends = chan;
			chan->pool->n_idle_backends += 1;
			chan->peer = NULL;
		}
	}
	else if (!chan->backend_is_tainted) /* if it was not marked as tainted before... */
	{
		ELOG(LOG, "Backed %d is tainted", chan->backend_pid);
		chan->backend_is_tainted = true;
		chan->proxy->state->n_dedicated_backends += 1;
	}
	return true;
}

/**
 * Parse client's startup packet and assign client to proper connection pool based on dbname/role
 */
static bool
client_connect(Channel* chan, int startup_packet_size)
{
	bool found;
	SessionPoolKey key;
	char* startup_packet = chan->buf;

	Assert(chan->client_port);

	/* parse startup packet in tmpctx memory context and reset it when it is not needed any more */
	MemoryContextReset(chan->proxy->tmpctx);
	MemoryContextSwitchTo(chan->proxy->tmpctx);

	/* Associate libpq with client's port */
	MyProcPort = chan->client_port;
	pq_init();

	if (ParseStartupPacket(chan->client_port, chan->proxy->tmpctx, startup_packet+4, startup_packet_size-4, false) != STATUS_OK) /* skip packet size */
	{
		MyProcPort = NULL;
		elog(WARNING, "Failed to parse startup packet for client %p", chan);
		return false;
	}
	MyProcPort = NULL;
	if (am_walsender)
	{
		elog(WARNING, "WAL sender should not be connected through proxy");
		return false;
	}

	chan->proxy->state->n_ssl_clients += chan->client_port->ssl_in_use;
	pg_set_noblock(chan->client_port->sock); /* SSL handshake may switch socket to blocking mode */
	memset(&key, 0, sizeof(key));
	strlcpy(key.database, chan->client_port->database_name, NAMEDATALEN);
	strlcpy(key.username, chan->client_port->user_name, NAMEDATALEN);

	ELOG(LOG, "Client %p connects to %s/%s", chan, key.database, key.username);

	chan->pool = (SessionPool*)hash_search(chan->proxy->pools, &key, HASH_ENTER, &found);
	if (!found)
	{
		/* First connection to this role/dbname */
		chan->proxy->state->n_pools += 1;
		memset((char*)chan->pool + sizeof(SessionPoolKey), 0, sizeof(SessionPool) - sizeof(SessionPoolKey));
	}
	chan->pool->proxy = chan->proxy;
	chan->pool->n_connected_clients += 1;
	chan->pool->n_idle_clients += 1;
	chan->proxy->n_accepted_connections -= 1;
	return true;
}

/*
 * Attach client to backend. Return true if new backend is attached, false otherwise.
 */
static bool
client_attach(Channel* chan)
{
	Channel* idle_backend = chan->pool->idle_backends;
	chan->pool->n_idle_clients -= 1;
	if (idle_backend)
	{
		/* has some idle backend */
		Assert(!idle_backend->backend_is_tainted && !idle_backend->client_port);
		Assert(chan != idle_backend);
		chan->peer = idle_backend;
		idle_backend->peer = chan;
		chan->pool->idle_backends = idle_backend->next;
		chan->pool->n_idle_backends -= 1;
		ELOG(LOG, "Attach client %p to backend %p (pid %d)", chan, idle_backend, idle_backend->backend_pid);
	}
	else /* all backends are busy */
	{
		if (chan->pool->n_launched_backends < chan->proxy->max_backends)
		{
			/* Try to start new backend */
			idle_backend = backend_start(chan->pool);
			if (idle_backend != NULL)
			{
				ELOG(LOG, "Start new backend %p (pid %d) for client %p",
					 idle_backend, idle_backend->backend_pid, chan);
				Assert(chan != idle_backend);
				chan->peer = idle_backend;
				idle_backend->peer = chan;
				return true;
			}
		}
		/* Postpone handshake until some backend is available */
		ELOG(LOG, "Client %p is waiting for available backends", chan);
		chan->next = chan->pool->pending_clients;
		chan->pool->pending_clients = chan;
		chan->pool->n_pending_clients += 1;
	}
	return false;
}

/*
 * Handle communication failure for this channel.
 * It is not possible to remove channel immediately because it can be triggered by other epoll events.
 * So link all channels in L1 list for pending delete.
 */
static void
channel_hangout(Channel* chan, char const* op)
{
	Channel** ipp;
	Channel* peer = chan->peer;
	if (chan->is_disconnected)
	   return;

	if (chan->client_port) {
		ELOG(LOG, "Hangout client %p due to %s error: %m", chan, op);
		for (ipp = &chan->pool->pending_clients; *ipp != NULL; ipp = &(*ipp)->next) {
			if (*ipp == chan) {
				*ipp = chan->next;
				chan->pool->n_pending_clients -= 1;
				break;
			}
		}
	} else {
		ELOG(LOG, "Hangout backend %p (pid %d) due to %s error: %m", chan, chan->backend_pid, op);
		for (ipp = &chan->pool->idle_backends; *ipp != NULL; ipp = &(*ipp)->next) {
			if (*ipp == chan) {
				*ipp = chan->next;
				chan->pool->n_idle_backends -= 1;
				break;
			}
		}
	}
	if (peer)
	{
		peer->peer = NULL;
		chan->peer = NULL;
	}
	chan->backend_is_ready = false;

	if (chan->client_port && peer) /* If it is client connected to backend. */
	{
		if (!chan->is_interrupted) /* Client didn't sent 'X' command, so do it for him. */
		{
			ELOG(LOG, "Send terminate command to backend %p (pid %d)", peer, peer->backend_pid);
			peer->is_interrupted = true; /* interrupted flags makes channel_write to send 'X' message */
			channel_write(peer, false);
			return;
		}
		else if (!peer->is_interrupted)
		{
			/* Client already sent 'X' command, so we can safely reschedule backend to some other client session */
			backend_reschedule(peer, false);
		}
	}
	chan->next = chan->proxy->hangout;
	chan->proxy->hangout = chan;
	chan->is_disconnected = true;
}

/*
 * Try to write data to the socket.
 */
static ssize_t
socket_write(Channel* chan, char const* buf, size_t size)
{
	ssize_t rc;
#ifdef USE_SSL
	int waitfor = 0;
	if (chan->client_port && chan->client_port->ssl_in_use)
		rc = be_tls_write(chan->client_port, (char*)buf, size, &waitfor);
	else
#endif
		rc = chan->client_port
			? secure_raw_write(chan->client_port, buf, size)
			: send(chan->backend_socket, buf, size, 0);
	if (rc == 0 || (rc < 0 && (errno != EAGAIN && errno != EWOULDBLOCK)))
	{
		channel_hangout(chan, "write");
	}
	else if (rc < 0)
	{
		/* do not accept more read events while write request is pending */
		ModifyWaitEvent(chan->proxy->wait_events, chan->event_pos, WL_SOCKET_WRITEABLE|WL_SOCKET_EDGE, NULL);
		chan->write_pending = true;
	}
	else if (chan->write_pending)
	{
		/* resume accepting read events */
		ModifyWaitEvent(chan->proxy->wait_events, chan->event_pos, WL_SOCKET_READABLE|WL_SOCKET_EDGE, NULL);
		chan->write_pending = false;
	}
	return rc;
}


/*
 * Try to send some data to the channel.
 * Data is located in the peer buffer. Because of using edge-triggered mode we have have to use non-blocking IO
 * and try to write all available data. Once write is completed we should try to read more data from source socket.
 * "synchronous" flag is used to avoid infinite recursion or reads-writers.
 * Returns true if there is nothing to do or operation is successfully completed, false in case of error
 * or socket buffer is full.
 */
static bool
channel_write(Channel* chan, bool synchronous)
{
	Channel* peer = chan->peer;
	if (!chan->client_port && chan->is_interrupted)
	{
		/* Send terminate command to the backend. */
		char const terminate[] = {'X', 0, 0, 0, 4};
		if (socket_write(chan, terminate, sizeof(terminate)) <= 0)
			return false;
		channel_hangout(chan, "terminate");
		return true;
	}
	if (peer == NULL)
		return false;

	while (peer->tx_pos < peer->tx_size) /* has something to write */
	{
		ssize_t rc = socket_write(chan, peer->buf + peer->tx_pos, peer->tx_size - peer->tx_pos);
		ELOG(LOG, "%p: write %d tx_pos=%d, tx_size=%d: %m", chan, (int)rc, peer->tx_pos, peer->tx_size);
		if (rc <= 0)
			return false;
		if (chan->client_port)
			chan->proxy->state->tx_bytes += rc;
		else
			chan->proxy->state->rx_bytes += rc;
		peer->tx_pos += rc;
	}
	if (peer->tx_size != 0)
	{
		/* Copy rest of received data to the beginning of the buffer */
		chan->backend_is_ready = false;
		Assert(peer->rx_pos >= peer->tx_size);
		memmove(peer->buf, peer->buf + peer->tx_size, peer->rx_pos - peer->tx_size);
		peer->rx_pos -= peer->tx_size;
		peer->tx_pos = peer->tx_size = 0;
		if (peer->backend_is_ready) {
			Assert(peer->rx_pos == 0);
			backend_reschedule(peer, false);
			return true;
		}
	}
	return synchronous || channel_read(peer); /* write is not invoked from read */
}

/*
 * Try to read more data from the channel and send it to the peer.
 */
static bool
channel_read(Channel* chan)
{
	int	 msg_start;
	while (chan->tx_size == 0) /* there is no pending write op */
	{
		ssize_t rc;
#ifdef USE_SSL
		int waitfor = 0;
		if (chan->client_port && chan->client_port->ssl_in_use)
			rc = be_tls_read(chan->client_port, chan->buf + chan->rx_pos, chan->buf_size - chan->rx_pos, &waitfor);
		else
#endif
			rc = chan->client_port
				? secure_raw_read(chan->client_port, chan->buf + chan->rx_pos, chan->buf_size - chan->rx_pos)
				: recv(chan->backend_socket, chan->buf + chan->rx_pos, chan->buf_size - chan->rx_pos, 0);

		ELOG(LOG, "%p: read %d: %m", chan, (int)rc);
		if (rc <= 0)
		{
			if (rc == 0 || (errno != EAGAIN && errno != EWOULDBLOCK))
				channel_hangout(chan, "read");
			else
			{
				/* do not accept more write events while read request is pending */
				ModifyWaitEvent(chan->proxy->wait_events, chan->event_pos, WL_SOCKET_READABLE|WL_SOCKET_EDGE, NULL);
				chan->read_pending = true;
			}
			return false; /* wait for more data */
		}
		else if (chan->read_pending)
		{
			/* resume accepting all events */
			ModifyWaitEvent(chan->proxy->wait_events, chan->event_pos, WL_SOCKET_READABLE|WL_SOCKET_WRITEABLE|WL_SOCKET_EDGE, NULL);
			chan->read_pending = false;
		}
		chan->rx_pos += rc;
		msg_start = 0;

		/* Loop through all received messages */
		while (chan->rx_pos - msg_start >= 5) /* has message code + length */
		{
			int msg_len;
			bool handshake = false;
			if (chan->pool == NULL) /* process startup packet */
			{
				Assert(msg_start == 0);
				memcpy(&msg_len, chan->buf + msg_start, sizeof(msg_len));
				msg_len = ntohl(msg_len);
				handshake = true;
			}
			else
			{
				ELOG(LOG, "%p receive message %c", chan, chan->buf[msg_start]);
				memcpy(&msg_len, chan->buf + msg_start + 1, sizeof(msg_len));
				msg_len = ntohl(msg_len) + 1;
			}
			if (msg_start + msg_len > chan->buf_size)
			{
				/* Reallocate buffer to fit complete message body */
				chan->buf_size = msg_start + msg_len;
				chan->buf = realloc(chan->buf, chan->buf_size);
			}
			if (chan->rx_pos - msg_start >= msg_len) /* Message is completely fetched */
			{
				int response_size = msg_start + msg_len;
				if (chan->pool == NULL) /* receive startup packet */
				{
					Assert(chan->client_port);
					if (!client_connect(chan, msg_len))
					{
						/* Some trouble with processing startup packet */
						chan->is_disconnected = true;
						channel_remove(chan);
						return false;
					}
				}
				else if (!chan->client_port /* Message from backend */
					&& chan->buf[msg_start] == 'Z'	/* Ready for query */
					&& chan->buf[msg_start+5] == 'I') /* Transaction block status is idle */
				{
					Assert(chan->rx_pos - msg_start == msg_len); /* Should be last message */
					chan->backend_is_ready = true; /* Backend is ready for query */
					chan->proxy->state->n_transactions += 1;
				}
				else if (chan->client_port /* Message from client */
						 && chan->buf[msg_start] == 'X')	/* Terminate message */
				{
					chan->is_interrupted = true;
					if (chan->peer == NULL || !chan->peer->backend_is_tainted)
					{
						/* Skip terminate message to idle and non-tainted backends */
						channel_hangout(chan, "terminate");
						return false;
					}
				}
				if (chan->peer == NULL)	 /* client is not yet connected to backend */
				{
					if (!chan->client_port)
					{
						/* We are not expecting messages from idle backend. Assume that it some error or shutdown. */
						channel_hangout(chan, "idle");
						return false;
					}
					client_attach(chan);
					if (handshake) /* Send handshake response to the client */
					{
						/* If we attach new client to the existed backend, then we need to send handshake response to the client */
						Channel* backend = chan->peer;
						Assert(chan->rx_pos == msg_len && msg_start == 0);
						chan->rx_pos = 0; /* Skip startup packet */
						if (backend != NULL) /* Backend was assigned */
						{
							Assert(backend->handshake_response != NULL); /* backend has already sent handshake responses */
							Assert(backend->handshake_response_size < backend->buf_size);
							memcpy(backend->buf, backend->handshake_response, backend->handshake_response_size);
							backend->rx_pos = backend->tx_size = backend->handshake_response_size;
							backend->backend_is_ready = true;
							return channel_write(chan, false);
						}
						else
						{
							/* Handshake response will be send to client later when backend is assigned */
							return false;
						}
					}
					else if (chan->peer == NULL) /* Backend was not assigned */
					{
						chan->tx_size = response_size; /* query will be send later once backend is assigned */
						return false;
					}
				}
				msg_start += msg_len;
			}
			else break; /* Incomplete message. */
		}
		if (msg_start != 0)
		{
			/* Has some complete messages to send to peer */
			Assert(chan->tx_pos == 0);
			Assert(chan->rx_pos >= msg_start);
			chan->tx_size = msg_start;
			if (!channel_write(chan->peer, true))
				return false;
		}
		/* If backend is out of transaction, then reschedule it */
		if (chan->backend_is_ready)
			return backend_reschedule(chan, false);

		/* Do not try to read more data if edge-triggered mode is not supported */
		if (!WaitEventUseEpoll)
			break;
	}
	return true;
}

/*
 * Create new channel.
 */
static Channel*
channel_create(Proxy* proxy)
{
	Channel* chan = (Channel*)calloc(1, sizeof(Channel));
	chan->proxy = proxy;
	chan->buf = malloc(INIT_BUF_SIZE);
	chan->buf_size = INIT_BUF_SIZE;
	chan->tx_pos = chan->rx_pos = chan->tx_size = 0;
	return chan;
}

/*
 * Register new channel in wait event set.
 */
static bool
channel_register(Proxy* proxy, Channel* chan)
{
	pgsocket sock = chan->client_port ? chan->client_port->sock : chan->backend_socket;
	/* Using edge epoll mode requires non-blocking sockets */
	pg_set_noblock(sock);
	chan->event_pos =
		AddWaitEventToSet(proxy->wait_events, WL_SOCKET_READABLE|WL_SOCKET_WRITEABLE|WL_SOCKET_EDGE,
						  sock, NULL, chan);
	if (chan->event_pos < 0)
	{
		elog(WARNING, "PROXY: Failed to add new client - too much sessions: %d clients, %d backends. "
					 "Try to increase 'max_sessions' configuration parameter.",
					 proxy->state->n_clients, proxy->state->n_backends);
		return false;
	}
	return true;
}

/*
 * Start new backend for particular pool associated with dbname/role combination.
 * Backend is forked using BackendStartup function.
 */
static Channel*
backend_start(SessionPool* pool)
{
	Channel* chan;
	char postmaster_port[8];
	char const* keywords[] = {"port","dbname","user","sslmode","application_name",NULL};
	char const* values[] = {postmaster_port,pool->key.database,pool->key.username,"disable","pool_worker",NULL};
	PGconn* conn;
	char* msg;
	int int32_buf;
	int msg_len;
	static bool libpqconn_loaded;

	if (!libpqconn_loaded)
	{
		/* We need libpq library to be able to establish connections to pool workers.
		* This library can not be linked statically, so load it on demand. */
		load_file("libpqconn", false);
		libpqconn_loaded = true;
	}
	pg_itoa(PostPortNumber, postmaster_port);
	conn = LibpqConnectdbParams(keywords, values);
	if (!conn)
		return NULL;

	chan = channel_create(pool->proxy);
	chan->pool = pool;
	chan->backend_socket = conn->sock;
	/* Using edge epoll mode requires non-blocking sockets */
	pg_set_noblock(conn->sock);

	/* Save handshake response */
	chan->handshake_response_size = conn->inEnd;
	chan->handshake_response = malloc(chan->handshake_response_size);
	memcpy(chan->handshake_response, conn->inBuffer, chan->handshake_response_size);

	/* Extract backend pid */
	msg = chan->handshake_response;
	while (*msg != 'K') /* Scan handshake response until we reach PID message */
	{
		memcpy(&int32_buf, ++msg, sizeof(int32_buf));
		msg_len = ntohl(int32_buf);
		msg += msg_len;
		Assert(msg < chan->handshake_response + chan->handshake_response_size);
	}
	memcpy(&int32_buf, msg+5, sizeof(int32_buf));
	chan->backend_pid = ntohl(int32_buf);

	if (channel_register(pool->proxy, chan))
	{
		pool->proxy->state->n_backends += 1;
		pool->n_launched_backends += 1;
	}
	else
	{
		/* Too much sessions, error report was already logged */
		closesocket(chan->backend_socket);
		free(chan->buf);
		free(chan);
		chan = NULL;
	}
	return chan;
}

/*
 * Add new client accepted by postmaster. This client will be assigned to concrete session pool
 * when it's startup packet is received.
 */
static void
proxy_add_client(Proxy* proxy, Port* port)
{
	Channel* chan = channel_create(proxy);
	chan->client_port = port;
	chan->backend_socket = PGINVALID_SOCKET;
	if (channel_register(proxy, chan))
	{
		ELOG(LOG, "Add new client %p", chan);
		proxy->n_accepted_connections += 1;
		proxy->state->n_clients += 1;
	}
	else
	{
		/* Too much sessions, error report was already logged */
		closesocket(port->sock);
#if defined(ENABLE_GSS) || defined(ENABLE_SSPI)
		free(port->gss);
#endif
		free(port);
		free(chan->buf);
		free(chan);
	}
}

/*
 * Perform delayed deletion of channel
 */
static void
channel_remove(Channel* chan)
{
	Assert(chan->is_disconnected); /* should be marked as disconnected by channel_hangout */
	DeleteWaitEventFromSet(chan->proxy->wait_events, chan->event_pos);
	if (chan->client_port)
	{
		if (chan->pool)
			chan->pool->n_connected_clients -= 1;
		else
			chan->proxy->n_accepted_connections -= 1;
		chan->proxy->state->n_clients -= 1;
		chan->proxy->state->n_ssl_clients -= chan->client_port->ssl_in_use;
		closesocket(chan->client_port->sock);
		free(chan->client_port);
	}
	else
	{
		chan->proxy->state->n_backends -= 1;
		chan->proxy->state->n_dedicated_backends -= chan->backend_is_tainted;
		chan->pool->n_launched_backends -= 1;
		closesocket(chan->backend_socket);
		free(chan->handshake_response);

		if (chan->pool->pending_clients)
		{
			/* Try to start new backend instead of terminated */
			Channel* new_backend = backend_start(chan->pool);
			if (new_backend != NULL)
			{
				ELOG(LOG, "Spawn new backend %p instead of terminated %p", new_backend, chan);
				backend_reschedule(new_backend, true);
			}
		}
	}
	free(chan->buf);
	free(chan);
}



/*
 * Create new proxy.
 */
static Proxy*
proxy_create(pgsocket postmaster_socket, ConnectionProxyState* state, int max_backends)
{
	HASHCTL ctl;
	Proxy*	proxy = calloc(1, sizeof(Proxy));
	proxy->memctx = AllocSetContextCreate(TopMemoryContext,
										  "Proxy",
										  ALLOCSET_DEFAULT_SIZES);
	proxy->tmpctx = AllocSetContextCreate(proxy->memctx,
										  "Startup packet parsing context",
										  ALLOCSET_DEFAULT_SIZES);
	MemSet(&ctl, 0, sizeof(ctl));
	ctl.keysize = sizeof(SessionPoolKey);
	ctl.entrysize = sizeof(SessionPool);
	ctl.hcxt = proxy->memctx;
	proxy->pools = hash_create("Pool by database and user", DB_HASH_SIZE,
							   &ctl, HASH_ELEM | HASH_BLOBS | HASH_CONTEXT);
	 /* We need events both for clients and backends so multiply MaxConnection by two */
	proxy->wait_events = CreateWaitEventSet(TopMemoryContext, MaxSessions*2);
	AddWaitEventToSet(proxy->wait_events, WL_SOCKET_READABLE,
					  postmaster_socket, NULL, NULL);
	proxy->max_backends = max_backends;
	proxy->state = state;
	return proxy;
}

/*
 * Main proxy loop
 */
static void
proxy_loop(Proxy* proxy)
{
	int i, n_ready;
	WaitEvent ready[MAX_READY_EVENTS];
	Channel *chan, *next;

	/* Main loop */
	while (!proxy->shutdown)
	{
		/* Use timeout to allow normal proxy shutdown */
		n_ready = WaitEventSetWait(proxy->wait_events, PROXY_WAIT_TIMEOUT, ready, MAX_READY_EVENTS, PG_WAIT_CLIENT);
		for (i = 0; i < n_ready; i++) {
			chan = (Channel*)ready[i].user_data;
			if (chan == NULL) /* new connection from postmaster */
			{
				Port* port = (Port*)calloc(1, sizeof(Port));
				port->sock = pg_recv_sock(ready[i].fd);
				if (port->sock == PGINVALID_SOCKET)
				{
					elog(WARNING, "Failed to receive session socket: %m");
					free(port);
				}
				else
				{
#if defined(ENABLE_GSS) || defined(ENABLE_SSPI)
					port->gss = (pg_gssinfo *) calloc(1, sizeof(pg_gssinfo));
					if (!port->gss)
						ereport(FATAL,
								(errcode(ERRCODE_OUT_OF_MEMORY),
								 errmsg("out of memory")));
#endif
					proxy_add_client(proxy, port);
				}
			}
			else
			{
				if (ready[i].events & WL_SOCKET_WRITEABLE) {
					ELOG(LOG, "Channel %p is writable", chan);
					/* At systems not supporttring epoll edge triggering (Win32, FreeBSD, MacOS), we need to disable writable event to avoid busy loop */
					ModifyWaitEvent(chan->proxy->wait_events, chan->event_pos, WL_SOCKET_READABLE | WL_SOCKET_EDGE, NULL);
					channel_write(chan, false);
				}
				if (ready[i].events & WL_SOCKET_READABLE) {
					ELOG(LOG, "Channel %p is readable", chan);
					channel_read(chan);
				}
			}
		}
		/*
		 * Delayed deallocation of disconnected channels.
		 * We can not delete channels immediately because of presence of peer events.
		 */
		for (chan = proxy->hangout; chan != NULL; chan = next)
		{
			next = chan->next;
			channel_remove(chan);
		}
		proxy->hangout = NULL;
	}
}

/*
 * Handle normal shutdown of Postgres instance
 */
static void
proxy_handle_sigterm(SIGNAL_ARGS)
{
	if (proxy)
		proxy->shutdown = true;
}

#ifdef EXEC_BACKEND
static pid_t
proxy_forkexec(void)
{
	char	   *av[10];
	int			ac = 0;

	av[ac++] = "postgres";
	av[ac++] = "--forkproxy";
	av[ac++] = NULL;			/* filled in by postmaster_forkexec */
	av[ac] = NULL;

	Assert(ac < lengthof(av));

	return postmaster_forkexec(ac, av);
}
#endif

NON_EXEC_STATIC void
ConnectionProxyMain(int argc, char *argv[])
{
	sigjmp_buf	local_sigjmp_buf;

	/* Identify myself via ps */
	init_ps_display("connection proxy", "", "", "");

	SetProcessingMode(InitProcessing);

	pqsignal(SIGTERM, proxy_handle_sigterm);
	pqsignal(SIGQUIT, quickdie);
	InitializeTimeouts();		/* establishes SIGALRM handler */

	/* Early initialization */
	BaseInit();

	/*
	 * Create a per-backend PGPROC struct in shared memory, except in the
	 * EXEC_BACKEND case where this was done in SubPostmasterMain. We must do
	 * this before we can use LWLocks (and in the EXEC_BACKEND case we already
	 * had to do some stuff with LWLocks).
	 */
#ifndef EXEC_BACKEND
	InitProcess();
#endif

	/*
	 * If an exception is encountered, processing resumes here.
	 *
	 * See notes in postgres.c about the design of this coding.
	 */
	if (sigsetjmp(local_sigjmp_buf, 1) != 0)
	{
		/* Prevents interrupts while cleaning up */
		HOLD_INTERRUPTS();

		/* Report the error to the server log */
		EmitErrorReport();

		/*
		 * We can now go away.	Note that because we called InitProcess, a
		 * callback was registered to do ProcKill, which will clean up
		 * necessary state.
		 */
		proc_exit(0);
	}
	/* We can now handle ereport(ERROR) */
	PG_exception_stack = &local_sigjmp_buf;

	PG_SETMASK(&UnBlockSig);

	proxy = proxy_create(MyProxySocket, &ProxyState[MyProxyId], SessionPoolSize);
	proxy_loop(proxy);

	proc_exit(0);
}

/*
 * Function for launching proxy by postmaster.
 * This "boilerplate" code is taken from another auxiliary workers.
 * In future it may be replaced with background worker.
 * The main problem with background worker is how to pass socket to it and obtains its PID.
 */
int
ConnectionProxyStart()
{
	pid_t		worker_pid;

#ifdef EXEC_BACKEND
	switch ((worker_pid = proxy_forkexec()))
#else
	switch ((worker_pid = fork_process()))
#endif
	{
		case -1:
			ereport(LOG,
					(errmsg("could not fork proxy worker process: %m")));
			return 0;

#ifndef EXEC_BACKEND
		case 0:
			/* in postmaster child ... */
			InitPostmasterChild();

			ConnectionProxyMain(0, NULL);
			break;
#endif
		default:
		  elog(LOG, "Start proxy process %d", (int) worker_pid);
		  return (int) worker_pid;
	}

	/* shouldn't get here */
	return 0;
}

/*
 * We need some place in shared memory to provide information about proxies state.
 */
int ConnectionProxyShmemSize(void)
{
	return ConnectionProxiesNumber*sizeof(ConnectionProxyState);
}

void ConnectionProxyShmemInit(void)
{
	bool found;
	ProxyState = (ConnectionProxyState*)ShmemInitStruct("connection proxy contexts",
														ConnectionProxyShmemSize(), &found);
	if (!found)
		memset(ProxyState, 0, ConnectionProxyShmemSize());
}

PG_FUNCTION_INFO_V1(pg_pooler_state);

typedef struct
{
	int proxy_id;
	TupleDesc ret_desc;
} PoolerStateContext;

/**
 * Return information about proxies state.
 * This set-returning functions returns the following columns:
 *
 * pid			  - proxy process identifier
 * n_clients	  - number of clients connected to proxy
 * n_ssl_clients  - number of clients using SSL protocol
 * n_pools		  - number of pools (role/dbname combinations) maintained by proxy
 * n_backends	  - total number of backends spawned by this proxy (including tainted)
 * n_dedicated_backends - number of tainted backend
 * tx_bytes		  - amount of data sent from backends to clients
 * rx_bytes		  - amount of data sent from client to backends
 * n_transactions - number of transaction proceeded by all backends of this proxy
 */
Datum pg_pooler_state(PG_FUNCTION_ARGS)
{
	FuncCallContext* srf_ctx;
	MemoryContext old_context;
	PoolerStateContext* ps_ctx;
	HeapTuple tuple;
	Datum values[9];
	bool  nulls[9];
	int id;
	int i;

	if (SRF_IS_FIRSTCALL())
	{
		srf_ctx = SRF_FIRSTCALL_INIT();
		old_context = MemoryContextSwitchTo(srf_ctx->multi_call_memory_ctx);
		ps_ctx = (PoolerStateContext*)palloc(sizeof(PoolerStateContext));
		get_call_result_type(fcinfo, NULL, &ps_ctx->ret_desc);
		ps_ctx->proxy_id = 0;
		srf_ctx->user_fctx = ps_ctx;
		MemoryContextSwitchTo(old_context);
	}
	srf_ctx = SRF_PERCALL_SETUP();
	ps_ctx = srf_ctx->user_fctx;
	id = ps_ctx->proxy_id;
	if (id == ConnectionProxiesNumber)
		SRF_RETURN_DONE(srf_ctx);

	values[0] = Int32GetDatum(ProxyState[id].pid);
	values[1] = Int32GetDatum(ProxyState[id].n_clients);
	values[2] = Int32GetDatum(ProxyState[id].n_ssl_clients);
	values[3] = Int32GetDatum(ProxyState[id].n_pools);
	values[4] = Int32GetDatum(ProxyState[id].n_backends);
	values[5] = Int32GetDatum(ProxyState[id].n_dedicated_backends);
	values[6] = Int64GetDatum(ProxyState[id].tx_bytes);
	values[7] = Int64GetDatum(ProxyState[id].rx_bytes);
	values[8] = Int64GetDatum(ProxyState[id].n_transactions);

	for (i = 0; i <= 8; i++)
		nulls[i] = false;

	ps_ctx->proxy_id += 1;
	tuple = heap_form_tuple(ps_ctx->ret_desc, values, nulls);
	SRF_RETURN_NEXT(srf_ctx, HeapTupleGetDatum(tuple));
}
