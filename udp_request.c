#include "dnscrypt.h"

typedef struct SendtoWithRetryCtx_ {
    void (*cb)            (UDPRequest *udp_request);
    const void            *buffer;
    UDPRequest            *udp_request;
    const struct sockaddr *dest_addr;
    evutil_socket_t        handle;
    size_t                 length;
    ev_socklen_t           dest_len;
    int                    flags;
} SendtoWithRetryCtx;

/* Forward declarations. */
static int sendto_with_retry(SendtoWithRetryCtx *const ctx);

#ifndef UDP_BUFFER_SIZE
# define UDP_BUFFER_SIZE 2097152
#endif
#ifndef UDP_DELAY_BETWEEN_RETRIES
# define UDP_DELAY_BETWEEN_RETRIES 1
#endif

#ifndef SO_RCVBUFFORCE
# define SO_RCVBUFFORCE SO_RCVBUF
#endif
#ifndef SO_SNDBUFFORCE
# define SO_SNDBUFFORCE SO_SNDBUF
#endif

static void
udp_tune(evutil_socket_t const handle)
{
    if (handle == -1) {
        return;
    }
    setsockopt(handle, SOL_SOCKET, SO_RCVBUFFORCE,
               (void *) (int []) { UDP_BUFFER_SIZE }, sizeof (int));
    setsockopt(handle, SOL_SOCKET, SO_SNDBUFFORCE,
               (void *) (int []) { UDP_BUFFER_SIZE }, sizeof (int));
#if defined(IP_MTU_DISCOVER) && defined(IP_PMTUDISC_DONT)
    setsockopt(handle, IPPROTO_IP, IP_MTU_DISCOVER,
               (void *) (int []) { IP_PMTUDISC_DONT }, sizeof (int));
#elif defined(IP_DONTFRAG)
    setsockopt(handle, IPPROTO_IP, IP_DONTFRAG,
               (void *) (int []) { 0 }, sizeof (int));
#endif
}

static void
client_to_proxy_cb_sendto_cb(UDPRequest * const udp_request)
{
    (void)udp_request;
}

static void
udp_request_kill(UDPRequest * const udp_request)
{
    if (udp_request == NULL || udp_request->status.is_dying)
        return;
    
    udp_request->status.is_dying = 1;

    // free
    struct context *c;
    if (udp_request->sendto_retry_timer != NULL) {
        free(event_get_callback_arg(udp_request->sendto_retry_timer));
        event_free(udp_request->sendto_retry_timer);
        udp_request->sendto_retry_timer = NULL;
    }
    if (udp_request->timeout_timer != NULL) {
        event_free(udp_request->timeout_timer);
        udp_request->timeout_timer = NULL;
    }
    
    c = udp_request->context;
    if (udp_request->status.is_in_queue != 0) {
        assert(!TAILQ_EMPTY(&c->udp_request_queue));
        TAILQ_REMOVE(&c->udp_request_queue, udp_request, queue);
        assert(c->connections > 0);
        c->connections--;
    }

    udp_request->context = NULL;
    free(udp_request);
}

static int
udp_listener_kill_oldest_request(struct context *c)
{
    if (TAILQ_EMPTY(&c->udp_request_queue))
        return -1;
    
    udp_request_kill(TAILQ_FIRST(&c->udp_request_queue));

    return 0;
}

static void
sendto_with_retry_timer_cb(evutil_socket_t retry_timer_handle, short ev_flags,
                           void * const ctx_)
{       
    SendtoWithRetryCtx * const ctx = ctx_;
        
    (void) ev_flags;
    assert(retry_timer_handle ==
           event_get_fd(ctx->udp_request->sendto_retry_timer));
            
    sendto_with_retry(ctx);
}       

static int
sendto_with_retry(SendtoWithRetryCtx *const ctx)
{
    void              (*cb)(UDPRequest *udp_request);
    SendtoWithRetryCtx *ctx_cb;
    UDPRequest         *udp_request = ctx->udp_request;
    int                 err; 
    _Bool               retriable;
        
    if (sendto(ctx->handle, ctx->buffer, ctx->length, ctx->flags,
               ctx->dest_addr, ctx->dest_len) == (ssize_t) ctx->length) {
        cb = ctx->cb;
        if (udp_request->sendto_retry_timer != NULL) {
            assert(event_get_callback_arg(udp_request->sendto_retry_timer)
                   == ctx);
            free(ctx);
            event_free(udp_request->sendto_retry_timer);
            udp_request->sendto_retry_timer = NULL;
        }
        if (cb) {
            cb(udp_request);
        }
        return 0;
    }

    err = evutil_socket_geterror(udp_request->client_proxy_handle);
    logger(LOG_WARNING, "sendto: [%s]", evutil_socket_error_to_string(err));

    retriable = (err == ENOBUFS || err == ENOMEM ||
                 err == EAGAIN || err == EINTR);

    if (retriable == 0) {
        udp_request_kill(udp_request);
        return -1;
    }
    assert(DNS_QUERY_TIMEOUT < UCHAR_MAX);
    if (++(udp_request->retries) > DNS_QUERY_TIMEOUT) {
        udp_request_kill(udp_request);
        return -1;
    }
    if (udp_request->sendto_retry_timer != NULL) {
        ctx_cb = event_get_callback_arg(udp_request->sendto_retry_timer);
        assert(ctx_cb != NULL);
        assert(ctx_cb->udp_request == ctx->udp_request);
        assert(ctx_cb->buffer == ctx->buffer);
        assert(ctx_cb->cb == ctx->cb);
    } else {
        if ((ctx_cb = malloc(sizeof *ctx_cb)) == NULL) {
            udp_request_kill(udp_request);
            return -1;
        }
        if ((udp_request->sendto_retry_timer =
             evtimer_new(udp_request->context->event_loop,
                         sendto_with_retry_timer_cb, ctx_cb)) == NULL) {
            free(ctx_cb);
            udp_request_kill(udp_request);
            return -1;
        }
        assert(ctx_cb ==
               event_get_callback_arg(udp_request->sendto_retry_timer));
        *ctx_cb = *ctx;
    }
    const struct timeval tv = {
        .tv_sec = (time_t) UDP_DELAY_BETWEEN_RETRIES, .tv_usec = 0
    };
    evtimer_add(udp_request->sendto_retry_timer, &tv);
    return -1;

}

static void
proxy_client_send_truncated(UDPRequest * const udp_request,
                            uint8_t dns_reply[DNS_MAX_PACKET_SIZE_UDP],
                            size_t dns_reply_len)
{
    assert(dns_reply_len > DNS_OFFSET_FLAGS2);
    dns_reply[DNS_OFFSET_FLAGS] |= DNS_FLAGS_TC | DNS_FLAGS_QR;
    dns_reply[DNS_OFFSET_FLAGS2] |= DNS_FLAGS2_RA;
    sendto_with_retry(&(SendtoWithRetryCtx) {
            .udp_request = udp_request,
            .handle = udp_request->client_proxy_handle,
            .buffer = dns_reply,
            .length = dns_reply_len,
            .flags = 0,
            .dest_addr = (struct sockaddr *)&udp_request->client_sockaddr,
            .dest_len = udp_request->client_sockaddr_len,
            .cb = udp_request_kill
        });
}

static void
client_to_proxy_cb(evutil_socket_t client_proxy_handle, short ev_flags, void * const context)
{
    logger(LOG_INFO, "client to proxy cb");
    uint8_t dns_query[DNS_MAX_PACKET_SIZE_UDP];
    struct context *c = context;
    UDPRequest *udp_request;
    ssize_t curve_ret;
    ssize_t nread;
    size_t dns_query_len = 0;
    size_t max_query_size;
    size_t request_edns_payload_size;

    (void)ev_flags;
    assert(client_proxy_handle == c->udp_listener_handle);

    udp_request = calloc(1, sizeof(*udp_request));
    if (udp_request == NULL)
        return;

    udp_request->context = c;
    udp_request->sendto_retry_timer = NULL;
    udp_request->timeout_timer = NULL;
    udp_request->client_proxy_handle = client_proxy_handle;
    udp_request->client_sockaddr_len = sizeof(udp_request->client_sockaddr);
    nread = recvfrom(client_proxy_handle, 
                     (void *)dns_query, 
                     sizeof(dns_query), 
                     0, 
                     (struct sockaddr *)&udp_request->client_sockaddr,
                     &udp_request->client_sockaddr_len);
    if (nread < 0) {
        const int err = evutil_socket_geterror(client_proxy_handle);
        logger(LOG_WARNING, "recvfrom(client): [%s]", evutil_socket_error_to_string(err));
        udp_request_kill(udp_request);
        return;
    }

    if (nread < (ssize_t)DNS_HEADER_SIZE
            || nread > sizeof(dns_query)) {
        logger(LOG_WARNING, "Short query received");
        free(udp_request);
        return;
    }
    if (c->connections >= c->connections_max) {
        if (udp_listener_kill_oldest_request(c) != 0) {
            // tcp
        }
    }
    c->connections++;
    assert(c->connections <= c->connections_max);
    TAILQ_INSERT_TAIL(&c->udp_request_queue, udp_request, queue);
    memset(&udp_request->status, 0, sizeof(udp_request->status));
    udp_request->status.is_in_queue = 1;
    dns_query_len = (size_t)nread;
    assert(dns_query_len <= sizeof(dns_query));
    
    edns_add_section(c, dns_query, &dns_query_len, sizeof(dns_query), &request_edns_payload_size);

    if (request_edns_payload_size < DNS_MAX_PACKET_SIZE_UDP_SEND) {
        max_query_size = DNS_MAX_PACKET_SIZE_UDP_SEND;
    } else {
        max_query_size = request_edns_payload_size;
    }
    if (max_query_size > sizeof(dns_query)) {
        max_query_size = sizeof(dns_query);
    }

    assert(max_query_size <= sizeof(dns_query));
    if (udp_request->context->tcp_only != 0) {
        proxy_client_send_truncated(udp_request, dns_query, dns_query_len);
        return;
    }
    assert(SIZE_MAX - DNSCRYPT_MAX_PADDING - dnscrypt_query_header_size() > dns_query_len);

    size_t max_len = dns_query_len + DNSCRYPT_MAX_PADDING + dnscrypt_query_header_size(); 
    if (max_len > max_query_size) {
        max_len = max_query_size;
    }
    if (dns_query_len + dnscrypt_query_header_size() > max_len) {
        proxy_client_send_truncated(udp_request, dns_query, dns_query_len);
        return;
    }


    sendto_with_retry(& (SendtoWithRetryCtx) {
        .udp_request = udp_request,
        .handle = c->udp_resolver_handle,
        .buffer = dns_query,
        .length = dns_query_len,
        .flags = 0,
        .dest_addr = (struct sockaddr *) &c->resolver_sockaddr,
        .dest_len = c->resolver_sockaddr_len,
        .cb = client_to_proxy_cb_sendto_cb
    });
}

static void
resolver_to_proxy_cb(evutil_socket_t proxy_resolver_handle, short ev_flags, void * const context)
{
    logger(LOG_INFO, "resolver to proxy cb");
    uint8_t dns_reply[DNS_MAX_PACKET_SIZE_UDP];
    struct context *c = context;
    UDPRequest *scanned_udp_request;
    UDPRequest *udp_request = NULL;
    struct sockaddr_storage resolver_sockaddr;
    ev_socklen_t resolver_sockaddr_len = sizeof(struct sockaddr_storage);
    ssize_t nread;
    size_t dns_reply_len = (size_t)0U;
    size_t uncurved_len;

    (void)ev_flags;

    nread = recvfrom(proxy_resolver_handle, 
                     (void *)dns_reply, sizeof(dns_reply), 0,
                     (struct sockaddr *)&resolver_sockaddr,
                     &resolver_sockaddr_len);
    if (nread < 0) {
        const int err = evutil_socket_geterror(proxy_resolver_handle);
        logger(LOG_WARNING, "recvfrom(resolver): [%s]", evutil_socket_error_to_string(err));
        return;
    }
    if (evutil_sockaddr_cmp((const struct sockaddr *)&resolver_sockaddr,
                            (const struct sockaddr *)
                            &c->resolver_sockaddr, 1) != 0) {
        logger(LOG_WARNING, "Received a resolver reply from a different resolver");
        return;
    }


    TAILQ_FOREACH(scanned_udp_request, &c->udp_request_queue, queue) {
        udp_request = scanned_udp_request;
        break;
    }

    dns_reply_len = nread;
    sendto_with_retry(& (SendtoWithRetryCtx) {
            .udp_request = udp_request,
            .handle = udp_request->client_proxy_handle,
            .buffer = dns_reply,
            .length = dns_reply_len,
            .flags = 0,
            .dest_addr = (struct sockaddr *) &udp_request->client_sockaddr,
            .dest_len = udp_request->client_sockaddr_len,
            .cb = udp_request_kill
        });
}

int
udp_listern_bind(struct context *c)
{
    // listen socket & bind
    assert(c->udp_listener_handle == -1);

    if ((c->udp_listener_handle = socket(c->local_sockaddr.ss_family, SOCK_DGRAM, IPPROTO_UDP)) == -1) {
        logger(LOG_ERR, "Unable to create a socket (UDP)");
        return -1;
    }

    evutil_make_socket_closeonexec(c->udp_listener_handle);
    evutil_make_socket_nonblocking(c->udp_listener_handle);
    if (bind(c->udp_listener_handle, (struct sockaddr *)&c->local_sockaddr, c->local_sockaddr_len) != 0) {
        logger(LOG_ERR, "Unable to bind (UDP) [%s]", evutil_socket_error_to_string(evutil_socket_geterror(c->udp_listener_handle)));
        evutil_closesocket(c->udp_listener_handle);
        c->udp_listener_handle = -1;
        return -1;
    }

    udp_tune(c->udp_listener_handle);

    // resolver socket
    assert(c->udp_resolver_handle == -1);
    if ((c->udp_resolver_handle = socket(c->resolver_sockaddr.ss_family, SOCK_DGRAM, IPPROTO_UDP)) == -1) {
        logger(LOG_ERR, "Unable to create a socket to the resolver");
        evutil_closesocket(c->udp_resolver_handle);
        c->udp_listener_handle = -1;
    }
    evutil_make_socket_closeonexec(c->udp_resolver_handle);
    evutil_make_socket_nonblocking(c->udp_resolver_handle);
    udp_tune(c->udp_resolver_handle);

    TAILQ_INIT(&c->udp_request_queue);

    return 0;
}

void
udp_listener_stop(struct context *c)
{
    event_free(c->udp_resolver_event);
    c->udp_resolver_event = NULL;

    while (udp_listener_kill_oldest_request(c) == 0);
    logger(LOG_INFO, "UDP listener shut down");
}

int
udp_listener_start(struct context *c)
{
    assert(c->udp_listener_handle != -1);
    if ((c->udp_listener_event = event_new(c->event_loop, c->udp_listener_handle, EV_READ | EV_PERSIST, client_to_proxy_cb, c)) == NULL) {
        return -1;
    }
    if (event_add(c->udp_listener_event, NULL) != 0) {
        udp_listener_stop(c);
        return -1;
    }

    assert(c->udp_resolver_handle != -1);
    if ((c->udp_resolver_event = event_new(c->event_loop, c->udp_resolver_handle, EV_READ | EV_PERSIST, resolver_to_proxy_cb, c)) == NULL) {
        udp_listener_stop(c);
        return -1;
    }
    if (event_add(c->udp_resolver_event, NULL) != 0) {
        udp_listener_stop(c);
        return -1;
    }
    return 0;
}