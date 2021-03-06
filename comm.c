
#include <stdlib.h>
#include <errno.h>
#include <string.h>
#include <pthread.h>
#include "comm.h"
#include	"tuple.h"

#define SOCKET_ERROR    -1
#define QUEUE_SIZE      5

static int		sequenceNumber = 1;

static int		numMsgTypes = 0;
static char		**msgtypes;

Msg			*replyQueue;
Client			*clients;
static CommStats	*commStats;
static pthread_mutex_t	statsMut = PTHREAD_MUTEX_INITIALIZER;
pthread_mutex_t			replyLogserverMut = PTHREAD_MUTEX_INITIALIZER;
pthread_cond_t			replyLogserverCond = PTHREAD_COND_INITIALIZER;
//int serialized_msg_hdr_len = -1; /* The comm libary needs to know the size of the serialized msg header
//				    as it is no longer the same size as Msg, we compute this once */
int encryption = 0;
char session_key[16];
static int whole_read(int sock, char *buf, long len)
{
  char*curr = buf, *bend = buf + len;

  while ((bend - curr) > 0) {
    int res = recv(sock, curr, bend - curr, 0);
    if (res <= 0) return res;
    curr += res;
    dfs_out("Got %d (%d) of %d bytes on read\n", res, curr - buf, len);
  };
  return len;
}

int compute_serialized_msg_hdr_len(int igenc) {
  Msg in_msg;
  char *serialized;
  size_t serialized_sz;
  int serialized_msg_hdr_len;
  in_msg.seq = 0;
  in_msg.type = 0;
  in_msg.res = 0;
  in_msg.len = 0;
  assert(!tuple_serialize_msg(&serialized, &serialized_sz, &in_msg));
  if (encryption && igenc) {
      char *dummy;
      dfs_out("Checking header len\n");
      cry_sym_init(session_key);
      cry_sym_encrypt(&dummy, &serialized_msg_hdr_len, serialized, serialized_sz);
      free(dummy);
  } else {
      serialized_msg_hdr_len = serialized_sz;
  }
  free(serialized);
  dfs_out("Serialized msg size[%d]\n", serialized_msg_hdr_len);
  return serialized_msg_hdr_len;
}


static void comm_stats()
{
    int		i;

    if (!commStats) return;
    for (i = 1; i < numMsgTypes; i++) {
	printf("%30s: %ld\n", msgtypes[i], commStats->msgs[i]);
    }
    printf("Data sent: %ld, Received: %ld\n", commStats->dataSent, commStats->dataReceived);
}


void comm_register_msgtypes(int num, char **types) 
{
    numMsgTypes = num;
    msgtypes = types;

    commStats = calloc(sizeof(CommStats) + num * sizeof(commStats->msgs[0]), 1);

    atexit(comm_stats);
}


// not re-entrant....
char *messageStr(int type)
{
    static char	s[64];

    if ((type >= 0) && (type < numMsgTypes)) {
	return msgtypes[type];
    }
    sprintf(s, "%d\n", type);
    return s;
}


int comm_server_socket(int port)
{
    int 		lsock, sock;
    struct sockaddr_in 	addr; 
    int 		addr_size = sizeof(struct sockaddr_in);

    if ((sock = socket(AF_INET,SOCK_STREAM,0)) == SOCKET_ERROR) {
	dfs_out("no create socket\n");
        return 0;
    }

    /* fill address struct */
    addr.sin_addr.s_addr = INADDR_ANY;
    addr.sin_port = htons(port);
    addr.sin_family = AF_INET;

    dfs_out("Binding to port %d\n", port);

    if (bind(sock,(struct sockaddr*)&addr, sizeof(addr)) == SOCKET_ERROR)
	dfs_die("Could not bind to port %d, errno %d\n", port, errno);
    fprintf(stderr, "bound\n");
    fflush(stderr);

    getsockname( sock, (struct sockaddr *) &addr, (socklen_t *)&addr_size);
    dfs_out("opened socket as fd (%d) on port (%d) for stream i/o\n", sock, ntohs(addr.sin_port) );

    if (listen(sock, QUEUE_SIZE) == SOCKET_ERROR) {
        dfs_out("Could not listen\n");
        return 0;
    }

    lsock = accept(sock, (struct sockaddr*)&addr,(socklen_t *)&addr_size);
    dfs_out("Socket accepted... %d\n", lsock);

    return lsock;
}


void comm_server_socket_mt(int port, void *(* listener)(void *))
{
    int 		sock;
    long		asock;
    struct sockaddr_in 	addr; 
    int 		addr_size = sizeof(struct sockaddr_in);

    if ((sock = socket(AF_INET,SOCK_STREAM,0)) == SOCKET_ERROR) {
	dfs_die("no create socket\n");
    }

    /* fill address struct */
    addr.sin_addr.s_addr = INADDR_ANY;
    addr.sin_port = htons(port);
    addr.sin_family = AF_INET;

    dfs_out("Binding to port %d....", port);

    if (bind(sock,(struct sockaddr*)&addr, sizeof(addr)) == SOCKET_ERROR)
	dfs_die("could not bind to port %d\n", port);
    fprintf(stderr, "bound\n");
    fflush(stderr);

    getsockname( sock, (struct sockaddr *) &addr, (socklen_t *)&addr_size);

    if (listen(sock, QUEUE_SIZE) == SOCKET_ERROR)
        dfs_die("Could not listen\n");

    dfs_out("opened socket as fd (%d) on port (%d) for stream i/o....READY\n", sock, ntohs(addr.sin_port) );

    while (asock = accept(sock, (struct sockaddr*)&addr,(socklen_t *)&addr_size)) {
	static int	id = 1;

	Client	*c = calloc(sizeof(Client), 1);
	c->id = id++;
	c->fd = asock;
	c->next = clients;
	clients = c;
	pthread_create(&c->tid, NULL, listener, c);
	dfs_out("Socket accepted... %d (new thread id: %d/%d)\n", asock, id, c->tid);
    }
}


int comm_client_socket(char *hostname, int port) 
{
    int			sock;
    struct hostent* 	hostInfo;
    struct sockaddr_in 	addr;
    long 		nHostAddr;

    dfs_out("Making a socket\n");

    if ((sock = socket(AF_INET,SOCK_STREAM,IPPROTO_TCP))  == SOCKET_ERROR) {
        dfs_out("Could not make a socket\n");
        return 0;
    }

    hostInfo = gethostbyname(hostname);
    memcpy(&nHostAddr, hostInfo->h_addr_list[0], hostInfo->h_length);

    addr.sin_addr.s_addr = nHostAddr;
    addr.sin_port = htons(port);
    addr.sin_family = AF_INET;

    dfs_out("Connecting to %s on port %d\n", hostname, port);

    if(connect(sock, (struct sockaddr*)&addr, sizeof(addr)) == SOCKET_ERROR) {
        dfs_out("Could not connect to host\n");
        return 0;
    }

    dfs_out("Created client sock %d connected to %s on port %d\n", sock, hostname, port);
    return sock;
}


//=============================================================================

    
Msg *comm_read(int igenc, int sock) {
    Msg		hdr, *m;
    int		res;
    char	*p, *pend;
    dfs_out("Top of comm_read\n");
    int serialized_msg_hdr_len = compute_serialized_msg_hdr_len(igenc);
    char *serialized = malloc(serialized_msg_hdr_len);
    //dfs_out("About to BLOCK on READ\n");
    if (0 > (res = whole_read(sock, (char *)serialized, serialized_msg_hdr_len)))
	dfs_die("recv error in read_msg\n");
    if (!res) {
	dfs_out("SOCKET CLOSED AT OTHER END\n");
	close(sock);
	return NULL;
    }
    dfs_out("Before enc0, msg header len[%d]\n", serialized_msg_hdr_len);
    int out_sz = serialized_msg_hdr_len;
    if (encryption && igenc) {
	dfs_out("Decrypting header\n");
        dfs_out("Read enc0\n");
	char *out;
	cry_sym_init(session_key);
	cry_sym_decrypt(&out, &out_sz, serialized, serialized_msg_hdr_len);
	free(serialized);
	serialized = out;
    }
    dfs_out("tplhdr[%s]\n", serialized);
    tuple_unserialize_msg(serialized, out_sz, &hdr);
    free(serialized);
    dfs_out("PEEK %d\n", hdr.len);
    // TODO Here we need to decrypt, as the message header length will be wrong
    m = (Msg *)malloc(sizeof(Msg) + hdr.len);
    assert(m);
    memcpy(m, &hdr, sizeof(Msg));
    p = ((char *)m) + sizeof(Msg);
    pend = p + hdr.len;
    while (p < pend) {
      if (0 > (res = whole_read(sock, p, hdr.len)))
	    dfs_die("readfrom error in SAGR");
	dfs_out("read %d\n", res);
	p += res;
    }
    dfs_out("Read enc1\n");
    if (encryption && hdr.len && igenc) {
	char *out;
	int out_sz;
	dfs_out("Decrypting value\n");
	cry_sym_init(session_key);
	cry_sym_decrypt(&out, &out_sz, m + 1, res);
	memcpy(m + 1, out, out_sz);
	m->len = out_sz;
	free(out);
    }
    if (commStats) {
	pthread_mutex_lock(&statsMut);
	commStats->dataReceived += sizeof(Msg) + hdr.len;
	pthread_mutex_unlock(&statsMut);
    }

    dfs_out("Read msg type '%s', len %d, res %d, seq %d\n", messageStr(m->type), sizeof(Msg) + hdr.len, 
	    m->res, m->seq);
    return m;
}


// Varargs are char *, long, char *, long, 0   # MUST BE NULL-TERMINATED!
int comm_send_prim(int igenc, int sock, int type, int result, int seq, va_list ap) {
    va_list		ap2;
    int			i = 0, num = 1, totallen = 0;
    struct iovec	*vecs;
    char		*data;
    int			res;
    Msg			m;
    
    // 'ap' started and ended outside this routine
    va_copy(ap2, ap);

    while (data = va_arg(ap, char *)) {
	totallen += va_arg(ap, int);
	num++;
    }
    m.type = type;
    m.len = totallen;
    m.seq = seq;
    m.res = result;

    vecs = (struct iovec *)malloc(sizeof(struct iovec) * num);
    i = 1;
    int total_sz = 0;
    while (data = va_arg(ap2, char *)) {
	vecs[i++].iov_len = va_arg(ap2, int);
	if (encryption && igenc) {
	    dfs_out("Encrypting a field\n");
	    char *out;
	    int out_sz;
	    cry_sym_init(session_key);
	    cry_sym_encrypt(&out, &out_sz, data, vecs[i - 1].iov_len);
	    data = out;
	    vecs[i - 1].iov_len = out_sz;
	}
	total_sz += vecs[i - 1].iov_len;
	vecs[i - 1].iov_base = data;
    }
    va_end(ap2);
    char* serialized;
    size_t serialized_sz;
    m.len = total_sz;
    if (encryption && igenc) {
	dfs_out("Encrypting header\n");
	char *out;
	int out_sz;
	tuple_serialize_msg(&serialized, &serialized_sz, &m);
	dfs_out("tplhdr[%s]\n", serialized);
	cry_sym_init(session_key);
	cry_sym_encrypt(&out, &out_sz, serialized, serialized_sz);
	vecs[0].iov_base = out;
	vecs[0].iov_len = out_sz;
    } else {
	dfs_out("Making unencrypted header\n");
	tuple_serialize_msg(&serialized, &serialized_sz, &m);
	vecs[0].iov_base = serialized;
	vecs[0].iov_len = serialized_sz;
    }
    struct msghdr	mhdr;
    memset((char *)&mhdr, 0, sizeof(mhdr));
    mhdr.msg_iov = vecs;
    mhdr.msg_iovlen = num;
    {
	int cnt;
	for (cnt = 0; cnt < num; cnt++) {
	    char *tmp_out = malloc(vecs[cnt].iov_len + 1);
	    memcpy(tmp_out, vecs[cnt].iov_base, vecs[cnt].iov_len);
	    tmp_out[vecs[cnt].iov_len] = '\0';
	    dfs_out("COMM_PRIM[%d][%d][%s]\n", cnt, vecs[cnt].iov_len, tmp_out);
	    free(tmp_out);
	}
    }

    if (0 > (res = sendmsg(sock, &mhdr, 0))) {
	dfs_out("No send msg type %s on sock %d\n", messageStr(type), sock);
	free(vecs);
	return -1;
    }
    int j;
    if (encryption && igenc) {
	for (j = 0; j < i; ++j) {
	    free(vecs[j].iov_base);
	}
    }
    free(vecs);
    free(serialized);
    dfs_out("Sent msg type '%s' (res %d), %d bytes on %d, seq %d\n", messageStr(type), result, res, sock, m.seq);
    return res;
}


// Varargs are char *, int, char *, int, 0   # MUST BE NULL-TERMINATED!
int comm_send(int igenc, int sock, int type, ...) {
    va_list		ap;

    va_start(ap, type);
    int	res = comm_send_prim(igenc, sock, type, 0, sequenceNumber++, ap);
    if (commStats) {
	pthread_mutex_lock(&statsMut);
	assert(type < numMsgTypes);
	commStats->msgs[type]++;
	commStats->dataSent += res;
	pthread_mutex_unlock(&statsMut);
    }
    va_end(ap);
    return res;
}


// The incoming vector must have 
int comm_sendmsg(int igenc, int sock, int type, struct msghdr *min)
{
    int			i = 0;
    int			num = min->msg_iovlen + 1;
    int			totallen = 0;
    struct iovec	*vecs = (struct iovec *)malloc(sizeof(struct iovec) * num);
    int			res;
    Msg			m;
    

    for (i = 0; i < min->msg_iovlen; i++) {
	vecs[i + 1] = min->msg_iov[i];
	totallen += min->msg_iov[i].iov_len;
    }
    char* serialized;
    size_t serialized_sz;
    tuple_serialize_msg(&serialized, &serialized_sz, &m);
    vecs[0].iov_base = serialized;
    vecs[0].iov_len = serialized_sz;
    m.type = type;
    m.len = totallen;
    m.seq = sequenceNumber++;
	
    struct msghdr	mhdr;
    memset((char *)&mhdr, 0, sizeof(mhdr));
    mhdr.msg_iov = vecs;
    mhdr.msg_iovlen = num;

    if (0 > (res = sendmsg(sock, &mhdr, 0))) 
	dfs_die("No send msg type %d\n", type);
    free(vecs);
    free(serialized);

    if (commStats) {
	pthread_mutex_lock(&statsMut);
	assert(type < numMsgTypes);
	commStats->msgs[type]++;
	commStats->dataSent += res;
	pthread_mutex_unlock(&statsMut);
    }

    dfs_out("Sent msg type '%s', %d bytes on %d, seq %d\n", messageStr(type), res, sock, m.seq);
    return res;
}


// Varargs are char *, int, char *, int, 0   # MUST BE NULL-TERMINATED!
int comm_reply(int igenc, int sock, Msg *m, int result, ...) {
    va_list		ap;

    va_start(ap, result);
    int	res = comm_send_prim(igenc, sock, MSG_REPLY, result, m->seq, ap);
    va_end(ap);

    if (commStats) {
	pthread_mutex_lock(&statsMut);
	commStats->msgs[MSG_REPLY]++;
	commStats->dataSent += res;
	pthread_mutex_unlock(&statsMut);
    }

    return res;
}


// Varargs are char *, int, char *, int, 0   # MUST BE NULL-TERMINATED!
// Call with socket, msg type, and then an arbitrary number of data description pairs, followed by a NULL.
//
// For example 'comm_send_and_reply(fd, MSG_GET_EXTENT, &sig, sizeof(sig), NULL)' sends a GET_EXTENT message 
// with a single data segment as a payload, the signature for which an extent is needed. The signature is 
// described by a data pointer and length. The following single NULL says that there is no more data.
//
Msg *comm_send_and_reply(int igenc, int sock, int type, ...) 
{
    va_list		ap;

    va_start(ap, type);
    int	res = comm_send_prim(igenc, sock, type, 0, sequenceNumber++, ap);
    va_end(ap);

    if (res < 0) return NULL;

    if (commStats) {
	pthread_mutex_lock(&statsMut);
	assert(type < numMsgTypes);
	commStats->msgs[type]++;
	commStats->dataSent += res;
	pthread_mutex_unlock(&statsMut);
    }
    return comm_read(igenc, sock);
}

Msg *comm_send_and_reply_mutex(int igenc, pthread_mutex_t *mut, pthread_cond_t *cond, int sock, int type, ...)
{
    //willcompile

    // Same as above, except that the reply will be read by the
    // listener thread and dumped on 'replyQueue', where you will grab
    // it. Synchornization between listener thread and  this routine
    // is through the mutex and cond variable parameters.
    va_list		ap;
    Msg *reply;
    pthread_mutex_lock(&replyLogserverMut);
    va_start(ap, type);
    int	res = comm_send_prim(igenc, sock, type, 0, sequenceNumber++, ap);
    va_end(ap);

    if (res < 0) {
	pthread_mutex_unlock(&replyLogserverMut);
	return NULL;
    }
    pthread_cond_wait(&replyLogserverCond, &replyLogserverMut);
    reply = replyQueue;
    replyQueue = NULL;
    pthread_mutex_unlock(&replyLogserverMut);
    pthread_cond_signal(&replyLogserverCond);

    return reply;
}

