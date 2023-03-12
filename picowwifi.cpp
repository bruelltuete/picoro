#include "picowwifi.h"
#include "profiler.h"
#include "pico/cyw43_arch.h"
#include "lwip/tcp.h"
#include "lwip/dns.h"
#include <algorithm>
#define RINGBUFFER_SIZE 4
#include "ringbuffer.h"


#if PICORO_WIFIFUNC_IN_RAM
#define WIFIFUNC(f)    __no_inline_not_in_flash_func(f)
#else
#define WIFIFUNC(f)    f
#endif


enum WifiCmd
{
    CONNECT,
    DISCONNECT,

    SENDUDP,
    SENDTCP,
    GETNTP,
    HTTPHEAD,

    NONE = 0xdeadbeef
};

struct send_udp_or_tcp_s
{
    const char*     host;
    int             port;
    const char*     buffer;
    int             bufferlength;
    bool*           success;
    char*           responsebuffer;
    int*            responselength;
} send_udp_or_tcp;

struct CmdRingbufferEntry
{
    WifiCmd             cmd;
    Waitable            waitable;


    union
    {
        struct
        {
            const char*     ssid;
            const char*     pw;
            bool*           success;
        } connect;

        struct send_udp_or_tcp_s   send_udp_or_tcp;

        struct
        {
            const char*         host;
            uint64_t*           ms_since_1970;
            absolute_time_t*    localts;
        } getntp;

        struct
        {
            const char*     host;
            const char*     url;
            char*           responsebuffer;
            int*            bufferlength;
            int             port;
        } httphead;
    };
};

static Coroutine<512>       wifiblock;
static RingBuffer           cmdindices;
static CmdRingbufferEntry   cmdringbuffer[RINGBUFFER_SIZE];
static Waitable             newcmdswaitable;


static void WIFIFUNC(handle_disconnect)()
{
    PROFILE_THIS_FUNC;

    // FIXME: prob need to invalidate all outstanding requests/waitables

    int ec = cyw43_wifi_leave(&cyw43_state, CYW43_ITF_STA);
    // ignore if failed
    assert(ec == 0);
    // one last round of timeouts or other housekeeping.
    cyw43_arch_poll();
    // switch off the wifi chip.
    cyw43_arch_deinit();
}

static void WIFIFUNC(handle_connect)(const char* ssid, const char* pw, bool* success)
{
    PROFILE_THIS_FUNC;

    // FIXME: hook the gpio 24 pin irq so we wake up when there is something coming from the wifi chip.
    cyw43_arch_init();
    cyw43_arch_enable_sta_mode();

    int ec = cyw43_arch_wifi_connect_async(ssid, pw, CYW43_AUTH_WPA2_AES_PSK);
    if (ec != PICO_OK)
    {
        // this should only happen if the chip is wonky... maybe some power glitch or whatever.
        if (success != NULL)
            *success = false;
        return;
    }

    // try for at most 30 sec to connect.
    for (int i = 0; i < (30000 / 10); ++i)
    {
        cyw43_arch_poll();

        int linkstatus = cyw43_tcpip_link_status(&cyw43_state, CYW43_ITF_STA);
        // ok?
        if (linkstatus == CYW43_LINK_UP)
        {
            if (success != NULL)
                *success = true;
            return;
        }
        // error?
        if (linkstatus < 0)
        {
            if (success != NULL)
                *success = false;
            return;
        }

        yield_and_wait4time(make_timeout_time_ms(10));
    }

    // timed out
    if (success != NULL)
        *success = false;
    return;
}


// FIXME: this is practically screaming for some c++ class stuff...
struct CommonState
{
    ip_addr_t   remote_addr;
    int         seq;

    char*       responsebuffer;
    int         responsebufleft;    // how many bytes left in responsebuffer
};

static void WIFIFUNC(common_domainfound_callback)(const char* name, const ip_addr_t* ipaddr, void* callback_arg)
{
    PROFILE_THIS_FUNC;

    CommonState* state = (CommonState*) callback_arg;
    if (ipaddr == NULL)
    {
        // failed to resolve, for whatever reason.
        state->seq = -1;
        return;
    }

    state->remote_addr = *ipaddr;
    state->seq++;
}

static const char   HTTP_REQUEST0[] =
    "HEAD ";
// url here
static const char   HTTP_REQUEST1[] =
    " HTTP/1.1\r\n"
    "Host: ";
// host here
static const char   HTTP_REQUEST2[] =
    "\r\n"
    "Connection: close\r\n"
    "User-Agent: picow/1\r\n"               // probably something else...
    "\r\n";

enum
{
    HTTPHEAD_SEQ_RESOLVE = __LINE__,      // give a distinct value, to catch mixing up enums by mistake
    HTTPHEAD_SEQ_RESOLVE_WAITING,
    HTTPHEAD_SEQ_CONNECT,
    HTTPHEAD_SEQ_CONNECT_WAITING,
    HTTPHEAD_SEQ_SEND,
    HTTPHEAD_SEQ_SEND_WAITING,
    HTTPHEAD_SEQ_RECV,
    HTTPHEAD_SEQ_CLOSE,
    HTTPHEAD_SEQ_ERROR = -1,
};

struct HttpHeadState : CommonState
{
    // need to drag these around into tcpclient_recv_callback
    char*       responsebuffer;
    int*        bufferlength;
};

static err_t WIFIFUNC(tcpclient_connected_callback)(void* arg, struct tcp_pcb* tpcb, err_t err)
{
    CommonState* state = (CommonState*) arg;

    // docs say parameter err is always ERR_OK, should prob have a defensive check here somewhere.

    // debug?
    cyw43_arch_lwip_check();

    state->seq++;
    return ERR_OK;
}

static err_t WIFIFUNC(tcpclient_sent_callback)(void* arg, struct tcp_pcb* tpcb, u16_t len)
{
    CommonState* state = (CommonState*) arg;

    // debug?
    cyw43_arch_lwip_check();

    // FIXME: check how much len is of the user supplied buffer!
    //        have we transmitted all of it?
    //        maybe put a "outstanding_data" counter into CommonState.
    //        and/or introduce a sendacked state...
    state->seq++;
    return ERR_OK;
}

static err_t WIFIFUNC(tcpclient_recv_callback)(void* arg, struct tcp_pcb* tpcb, struct pbuf* p, err_t err)
{
    CommonState* state = (CommonState*) arg;

    // debug?
    cyw43_arch_lwip_check();

    // connection closed.
    if (p == NULL)
    {
        // if we have buffer space left then the remote end hung up first...
        if (state->responsebufleft > 0)
            // ...that means we need to transition to the next state now.
            state->seq++;
        // if it was us that hung up (eg because buffer is full) then we have already transitioned state.
        return ERR_OK;
    }

    // there might be dangling pbufs left.
    if (state->responsebufleft <= 0)
    {
        pbuf_free(p);
        return ERR_OK;
    }

    int bytes2copy = std::min((int)p->len, state->responsebufleft);
    memcpy(state->responsebuffer, p->payload, bytes2copy);
    state->responsebufleft -= bytes2copy;
    state->responsebuffer  += bytes2copy;

    // filled up our response buffer? we don't care about the rest that might be in flight.
    assert(state->responsebufleft >= 0);
    if (state->responsebufleft == 0)
    {
        // transition to next state (probably a close).
        // if server sent us a lot of data then we'll discard this at the top of this function.
        // we do not want tcp_shutdown()! receiving data with the rx-side shut triggers errors. we dont want an error, we want to ignore.
        state->seq++;
    }
    else
    {
        // this is a weird function... this is not an ack
        tcp_recved(tpcb, p->tot_len);
    }

    pbuf_free(p);
    return ERR_OK;
}

static err_t WIFIFUNC(httpclient_recv_callback)(void* arg, struct tcp_pcb* tpcb, struct pbuf* p, err_t err)
{
    HttpHeadState* state = (HttpHeadState*) arg;

    // debug?
    cyw43_arch_lwip_check();

    if (p == NULL)
    {
        state->seq = HTTPHEAD_SEQ_CLOSE;
        return ERR_OK;
    }

#define DATE_TOKEN      "\r\nDate:"

    // copy out the bit of data that we care for...
    // we are looking for the "Date: " header, everything else is irrelevant.
    const char* httpresp = (const char*) p->payload;
    // FIXME: near-zero-copy and fragmentation might make it hard to get the header... could be split between packets...
    for (int i = 0; i < (int)p->len - (int)sizeof(DATE_TOKEN) - 1; ++i)
    {
        // look for newlines... first thing we get is the status line, followed by headers.
        if (strncmp(&httpresp[i], DATE_TOKEN, sizeof(DATE_TOKEN) - 1) == 0)
        {
            // i points to the start of Date.
            // lets find out where the value is.
            int vi = i + sizeof(DATE_TOKEN) - 1;
            // skip any whitespace
            for (; (httpresp[vi] == ' ') && (vi < p->len); ++vi)
                ;
            // find end of line
            // (side note: the date value should have a max length: "Day, 31 Dec 9999 23:59:59 GMT", 29 characters)
            int ve = vi;
            for (; ve < p->len; ++ve)
            {
                if ((httpresp[ve] == '\r') || (httpresp[ve] == '\n'))
                    break;
            }
            assert(ve - vi == 29);

            memcpy(state->responsebuffer, &httpresp[vi], ve - vi);
            state->responsebuffer[ve - vi] = 0;
            *state->bufferlength = ve - vi + 1;     // include the terminator

            break;
        }
    }

#undef DATE_TOKEN

    // FIXME: this is a weird function... this is not an ack
    tcp_recved(tpcb, p->tot_len);

    pbuf_free(p);
    return ERR_OK;
}

static void WIFIFUNC(tcpclient_err_callback)(void* arg, err_t err)
{
    CommonState* state = (CommonState*) arg;
    // beware: docs say tcp_pcb is gone at this point. but that does not seem to be true?!

    // debug?
    cyw43_arch_lwip_check();

    // all "error" states have -1 as value.
    state->seq = HTTPHEAD_SEQ_ERROR;
}

// if there's only a single pbuf for the tx side, then lwip will copy the data into that single pbuf.
// in that case, no need for us to have a buffer to piece the string together. just let lwip do that in its pbuf.
#if !LWIP_NETIF_TX_SINGLE_PBUF
char        txbuffer[1400];     // FIXME: find out from lwip? slightly less than mtu
#endif

static void WIFIFUNC(handle_httphead)(const char* host, const char* url, int port, char* responsebuffer, int* bufferlength)
{
    err_t err = ERR_OK;

    // FIXME: figure out what we want to carry to the callbacks
    HttpHeadState    state;
    state.seq = HTTPHEAD_SEQ_RESOLVE;
    state.responsebuffer = responsebuffer;
    state.bufferlength = bufferlength;

    struct tcp_pcb* tcp_pcb = NULL;

    while (true)
    {
        cyw43_arch_poll();
        switch (state.seq)
        {
            case HTTPHEAD_SEQ_RESOLVE:
                err = dns_gethostbyname(host, &state.remote_addr, common_domainfound_callback, &state);
                if (err == ERR_OK)
                    state.seq = HTTPHEAD_SEQ_CONNECT;
                else
                if (err == ERR_INPROGRESS)
                    state.seq = HTTPHEAD_SEQ_RESOLVE_WAITING;
                else
                {
                    // malformed hostname
                    *bufferlength = 0;
                    return;
                }
                break;

            case HTTPHEAD_SEQ_CONNECT:
                // time to connect

                tcp_pcb = tcp_new_ip_type(IP_GET_TYPE(&state.remote_addr));
                assert(tcp_pcb != NULL);

                // attach our own state object to the pcb, so we get it during callbacks.
                tcp_arg(tcp_pcb, &state);
                // regular poll callback from lwip. might need this if we have to retry an operation.
                //tcp_poll(tcp_pcb, tcpclient_poll_callback, 5 * 2);
                // callback for when data "has been sent" and ack'd by the remote end.
                tcp_sent(tcp_pcb, tcpclient_sent_callback);
                // callback for when we've received data from the remote end.
                tcp_recv(tcp_pcb, httpclient_recv_callback);
                // callback for "fatal" errors, like out of mem? or "connection refused".
                tcp_err(tcp_pcb, tcpclient_err_callback);

                // FIXME: wonder why connect needs the mutex and the rest does not...
                cyw43_arch_lwip_begin();
                // starts connecting (but does not wait), when done callback is invoked.
                err = tcp_connect(tcp_pcb, &state.remote_addr, port, tcpclient_connected_callback);
                cyw43_arch_lwip_end();

                state.seq = HTTPHEAD_SEQ_CONNECT_WAITING;
                break;

            case HTTPHEAD_SEQ_SEND:
            {
                // time to send request
                cyw43_arch_lwip_begin();
#if LWIP_NETIF_TX_SINGLE_PBUF
                // in the single-pbuf case, this will copy our to-be-transmitted data into an lwip internal buffer.
                tcp_write(tcp_pcb, &HTTP_REQUEST0[0], sizeof(HTTP_REQUEST0) - 1, TCP_WRITE_FLAG_MORE);
                tcp_write(tcp_pcb, url, strlen(url), TCP_WRITE_FLAG_MORE);
                tcp_write(tcp_pcb, &HTTP_REQUEST1[0], sizeof(HTTP_REQUEST1) - 1, TCP_WRITE_FLAG_MORE);
                tcp_write(tcp_pcb, host, strlen(host), TCP_WRITE_FLAG_MORE);
                tcp_write(tcp_pcb, &HTTP_REQUEST2[0], sizeof(HTTP_REQUEST2) - 1, 0);
#else
                // FIXME: this looks like a mess... is saving a few cycles worth this?
                static_assert(sizeof(txbuffer) >= sizeof(HTTP_REQUEST0) + sizeof(HTTP_REQUEST1) + sizeof(HTTP_REQUEST2));
                const int maxurllength = sizeof(txbuffer) - sizeof(HTTP_REQUEST0) - sizeof(HTTP_REQUEST1) - sizeof(HTTP_REQUEST2);
                memcpy(&txbuffer[0], &HTTP_REQUEST0[0], sizeof(HTTP_REQUEST0) - 1);
                char* endofurlptr = (char*) memccpy(&txbuffer[sizeof(HTTP_REQUEST0) - 1], url, 0, maxurllength);
                assert(endofurlptr != NULL);    // url not null-terminated? overflowed txbuffer.
                memcpy(endofurlptr - 1, &HTTP_REQUEST1[0], sizeof(HTTP_REQUEST1) - 1);
                const int maxhostlength = sizeof(txbuffer) - (endofurlptr + sizeof(HTTP_REQUEST1) - 1 - &txbuffer[0]) - sizeof(HTTP_REQUEST2);
                char* endofhostptr = (char*) memccpy(endofurlptr - 1 + sizeof(HTTP_REQUEST1) - 1, host, 0, maxhostlength);
                assert(endofhostptr != NULL);    // host not null-terminated? overflowed txbuffer.
                memcpy(endofhostptr - 1, &HTTP_REQUEST2[0], sizeof(HTTP_REQUEST2) - 1);
                const int txlen = endofhostptr - 1 + sizeof(HTTP_REQUEST2) - 1 - &txbuffer[0];

                tcp_write(tcp_pcb, &txbuffer[0], txlen, 0);
#endif
                tcp_output(tcp_pcb);
                cyw43_arch_lwip_end();

                state.seq = HTTPHEAD_SEQ_SEND_WAITING;
                break;
            }

            case HTTPHEAD_SEQ_RECV:
                // FIXME: in the general case, we could get a response from the server before we have sent off our request!

                // nothing to do here, all recv code is in the callback.
                // fallthrough is fine.

            case HTTPHEAD_SEQ_RESOLVE_WAITING:
            case HTTPHEAD_SEQ_CONNECT_WAITING:
            case HTTPHEAD_SEQ_SEND_WAITING:
                // keep polling until callback says we are done.
                yield_and_wait4time(make_timeout_time_ms(10));
                break;

            case HTTPHEAD_SEQ_CLOSE:
                cyw43_arch_lwip_begin();
                err = tcp_close(tcp_pcb);
                cyw43_arch_lwip_end();
                if (err != ERR_OK)
                {
                    // try again to close, after some time.
                    yield_and_wait4time(make_timeout_time_ms(10));
                    break;
                }
                tcp_pcb = NULL;
                return;

            case HTTPHEAD_SEQ_ERROR:
                *bufferlength = 0;
                if (tcp_pcb != NULL)
                {
                    cyw43_arch_lwip_begin();
                    tcp_close(tcp_pcb);
                    cyw43_arch_lwip_end();
                }
                return;

            default:
                assert(false);
        }
    } // while true
}

enum
{
    SENDUDPTCP_SEQ_RESOLVE = __LINE__,      // give a distinct value, to catch mixing up enums by mistake
    SENDUDPTCP_SEQ_RESOLVE_WAITING,
    SENDUDPTCP_SEQ_CONNECT,
    SENDTCP_SEQ_CONNECT_WAITING,            // tcp only
    SENDUDPTCP_SEQ_SEND,
    SENDTCP_SEQ_SEND_WAITING,               // tcp only
    SENDUDPTCP_SEQ_RECV_WAITING,
    SENDUDPTCP_SEQ_CLOSE,
    SENDUDPTCP_SEQ_ERROR = -1,
};

struct SendUdpState : CommonState
{
    union
    {
        struct udp_pcb* udpsock;
        struct tcp_pcb* tcpsock;
    };
};

template <bool is_tcp>
static void WIFIFUNC(handle_sendudptcp)(struct send_udp_or_tcp_s* cmd)
{
    PROFILE_THIS_FUNC;

    err_t err = ERR_OK;
    SendUdpState    state;
    state.seq = SENDUDPTCP_SEQ_RESOLVE;
    state.udpsock = NULL;
    state.responsebuffer = cmd->responsebuffer;
    state.responsebufleft = cmd->responselength ? *cmd->responselength : 0;

    *cmd->success = false;

    for (int r = 0; ; ++r)      // count how many rounds through this loop we do, as a timeout mechanism
    {
        cyw43_arch_poll();
        switch (state.seq)
        {
            case SENDUDPTCP_SEQ_RESOLVE:
                // should take at most 4 sec, see DNS_TMR_INTERVAL and DNS_MAX_RETRIES.
                err = dns_gethostbyname(cmd->host, &state.remote_addr, common_domainfound_callback, &state);
                if (err == ERR_OK)
                    state.seq = SENDUDPTCP_SEQ_CONNECT;
                else
                if (err == ERR_INPROGRESS)
                    state.seq = SENDUDPTCP_SEQ_RESOLVE_WAITING;
                else
                {
                    // malformed hostname
                    return;
                }
                break;

            case SENDUDPTCP_SEQ_CONNECT:
                if (is_tcp)
                {
                    state.tcpsock = tcp_new_ip_type(IP_GET_TYPE(&state.remote_addr));
                    assert(state.tcpsock != NULL);

                    // attach our own state object to the pcb, so we get it during callbacks.
                    tcp_arg(state.tcpsock, &state);
                    // callback for "fatal" errors, like out of mem? or "connection refused".
                    tcp_err(state.tcpsock, tcpclient_err_callback);

                    // set state before tcp_connect() on the odd chance that the callback is
                    // invoked synchronously inside.
                    state.seq = SENDTCP_SEQ_CONNECT_WAITING;

                    // starts connecting (but does not wait), when done callback is invoked.
                    cyw43_arch_lwip_begin();
                    err = tcp_connect(state.tcpsock, &state.remote_addr, cmd->port, tcpclient_connected_callback);
                    cyw43_arch_lwip_end();
                    if (err != ERR_OK)
                        state.seq = SENDUDPTCP_SEQ_ERROR;
                }
                else
                {
                    state.udpsock = udp_new();
                    assert(state.udpsock != NULL);

                    cyw43_arch_lwip_begin();
                    err = udp_connect(state.udpsock, &state.remote_addr, cmd->port);
                    assert(err == ERR_OK);
                    cyw43_arch_lwip_end();
                    state.seq = SENDUDPTCP_SEQ_SEND;
                    // we could just fall-through here but instead we'll do once around the merrygoround for the benefit of cyw43_arch_poll() above.
                }
                break;
                
            case SENDUDPTCP_SEQ_SEND:
                cyw43_arch_lwip_begin();
                if (is_tcp)
                {
                    state.seq = SENDTCP_SEQ_SEND_WAITING;
                    // callback for when data "has been sent" and ack'd by the remote end.
                    tcp_sent(state.tcpsock, tcpclient_sent_callback);
                    // callback for when we've received data from the remote end.
                    if (cmd->responsebuffer != NULL)
                        tcp_recv(state.tcpsock, tcpclient_recv_callback);
                    tcp_write(state.tcpsock, cmd->buffer, cmd->bufferlength, 0);
                    tcp_output(state.tcpsock);
                    // note to self: send-callback incs send-waiting to recv-waiting.
                }
                else
                {
                    struct pbuf* p = pbuf_alloc_reference((void*) cmd->buffer, cmd->bufferlength, PBUF_REF);
                    // FIXME: receive callback!!
                    udp_send(state.udpsock, p);
                    // ignore return value of send(), udp is unreliable.
                    pbuf_free(p);
                    state.seq = SENDUDPTCP_SEQ_RECV_WAITING;
                }
                cyw43_arch_lwip_end();
                break;

            case SENDUDPTCP_SEQ_RECV_WAITING:
                // FIXME: in the general case, we could get a response from the server before we have sent off our request!
                if (cmd->responsebuffer == NULL)
                {
                    state.seq = SENDUDPTCP_SEQ_CLOSE;
                    break;
                }
                // nothing to do here, all recv code is in the callback.
                // fallthrough is fine.

            case SENDUDPTCP_SEQ_RESOLVE_WAITING:
            case SENDTCP_SEQ_CONNECT_WAITING:
            case SENDTCP_SEQ_SEND_WAITING:
                // have we spent too much time here?
                // (approx 60 sec, not every iteration takes 10ms but it's close enough)
                if (r > 60000/10)
                {
                    state.seq = SENDUDPTCP_SEQ_ERROR;
                }
                else
                {
                    // keep polling until callback says we are done.
                    yield_and_wait4time(make_timeout_time_ms(10));
                }
                break;

            case SENDUDPTCP_SEQ_CLOSE:
                if (is_tcp)
                {
                    cyw43_arch_lwip_begin();
                    // beware: tcp_close() is likely to put the socket into a timer queue for later cleanup.
                    // BUT: there is no later! when we return here then that's it, over and out.
                    // so instead do an ungraceful abort which will free the socket and not do any weird timer stuff.
                    // (side note: tcp_abort() will call our error-callback, but we dont want that nor care.)
                    tcp_err(state.tcpsock, NULL);
                    tcp_abort(state.tcpsock);
                    cyw43_arch_lwip_end();
                    state.tcpsock = NULL;
                    // success only after we've closed the socket.
                    *cmd->success = true;
                }
                else
                {
                    cyw43_arch_lwip_begin();
                    udp_disconnect(state.udpsock);
                    udp_remove(state.udpsock);
                    cyw43_arch_lwip_end();
                    *cmd->success = true;
                }
                if (cmd->responselength != NULL)
                    *cmd->responselength -= state.responsebufleft;
                return;

            case SENDUDPTCP_SEQ_ERROR:
                *cmd->success = false;
                if (is_tcp)
                {
                    if (state.tcpsock != NULL)
                    {
                        cyw43_arch_lwip_begin();
                        // see SENDUDPTCP_SEQ_CLOSE above, for an explanation.
                        tcp_err(state.tcpsock, NULL);
                        tcp_abort(state.tcpsock);
                        cyw43_arch_lwip_end();
                    }
                }
                else
                {
                    if (state.udpsock != NULL)
                    {
                        cyw43_arch_lwip_begin();
                        udp_disconnect(state.udpsock);
                        udp_remove(state.udpsock);
                        cyw43_arch_lwip_end();
                    }
                }
                return;

            default:
                assert(false);
        }
    } // for (ever)
}

struct SntpPacket
{
    uint8_t     mode:3;
    uint8_t     version:3;
    uint8_t     leap_indicator:2;       // not used by us

    uint8_t     stratum;
    uint8_t     poll;                   // not used by us
    int8_t      precision;              // not used by us

    uint32_t    root_delay;             // not used by us
    uint32_t    root_dispersion;        // not used by us
    char        reference_identifier_fourcc[4]; // not used by us
    uint64_t    reference_timestamp;    // not used by us
    uint64_t    originate_timestamp;
    uint64_t    receive_timestamp;
    uint64_t    transmit_timestamp;
};
static_assert(sizeof(SntpPacket) == 48);

enum
{
    GETNTP_SEQ_RESOLVE = __LINE__,      // give a distinct value, to catch mixing up enums by mistake
    GETNTP_SEQ_RESOLVE_WAITING,
    GETNTP_SEQ_CONNECT,
    GETNTP_SEQ_SEND,
    GETNTP_SEQ_RECV,
    GETNTP_SEQ_CLOSE,
    GETNTP_SEQ_ERROR = -1,
};

struct SntpState : CommonState
{
    SntpPacket          packet      __attribute__((aligned(8)));
    absolute_time_t     reqsend_us  __attribute__((aligned(8)));
    struct udp_pcb*     sock;

    uint64_t*           ms_since_1970;  // result
    absolute_time_t*    localts;        // result
};
static SntpState    sntpstate;      // keep in global mem instead of on stack... stack is in short supply.

static void WIFIFUNC(getntp_recv)(void* arg, struct udp_pcb* pcb, struct pbuf* p, const ip_addr_t* addr, u16_t port)
{
    assert(arg == &sntpstate);
    assert(pcb == sntpstate.sock);

    absolute_time_t now = get_absolute_time();

    // is from the server we reached out to?
    if (ip4_addr_eq(addr, &sntpstate.remote_addr) && (port == 123))
    {
        // packet big enough for a valid response?
        if (p->tot_len >= sizeof(SntpPacket))
        {
            // beware: pbuf_get_contiguous() is no use! it'd give us unaligned memory.
            pbuf_copy_partial(p, &sntpstate.packet, sizeof(sntpstate.packet), 0);
            // version the same we used? mode is server?
            if ((sntpstate.packet.version == 4) && (sntpstate.packet.mode == 4))
            {
                if (sntpstate.packet.stratum >= 1)
                {
                    // check that server relayed our request timestamp back to us.
                    if (sntpstate.packet.originate_timestamp == to_us_since_boot(sntpstate.reqsend_us))
                    {
                        // no longer bigendian...
                        sntpstate.packet.receive_timestamp   = __builtin_bswap64(sntpstate.packet.receive_timestamp);
                        sntpstate.packet.transmit_timestamp  = __builtin_bswap64(sntpstate.packet.transmit_timestamp);

                        // the ntp timestamp has 32 bit integer seconds, and 32 bit fractional seconds.
                        // so the smallest unit is 1/2^32 th second. we want microseconds.
                        // BUT BUT BUT: don't try to do direct conversion of this absolute timestamp to microseconds!
                        // that would loose far too much precision and the time/date you'd get would easily be 10 days off!
                        // instead we'll do mid point calc with an 11 bit fraction, which is close to millisecond resolution.
                        sntpstate.packet.receive_timestamp  >>= 32 - 11;
                        sntpstate.packet.transmit_timestamp >>= 32 - 11;
                        // lets assume that sending and receiving udp packets takes the same amount of time, ie is symmetric.
                        // so we can align the middle of server's recv-to-send time frame, with ours.
                        uint64_t servermiddle = (sntpstate.packet.transmit_timestamp + sntpstate.packet.receive_timestamp) / 2;
                        uint64_t ourmiddle_us = (to_us_since_boot(sntpstate.reqsend_us) + to_us_since_boot(now)) / 2;
                        
                        // ntp starts counting from 1st jan 1900. but unix epoch from 1970. that big offset here is millisec between 1900 and 1970, as defined per spec.
                        // (looks like that value is 70 years plus 17 leap days)
                        uint64_t t = servermiddle - ((2208988800ull << 32) >> (32 - 11));

                        // add up the fraction that came from the server.
                        uint32_t f = 0;
                        uint32_t temp = t & ((1 << 11) - 1);
                        if (temp & (1 << 10))   // is the 0.5s bit set?
                            f += 500;
                        if (temp & (1 << 9))
                            f += 250;
                        if (temp & (1 << 8))
                            f += 125;
                        if (temp & (1 << 7))
                            f += 63;
                        if (temp & (1 << 6))
                            f += 31;
                        // how many do we need? want?
                        if (temp & (1 << 5))
                            f += 16;
                        if (temp & (1 << 4))
                            f += 8;
                        if (temp & (1 << 3))
                            f += 4;
                        // i think at this point we have like 2ms rounding error already.

                        // slice off the remaining fraction, gets us an accurate seconds-since-1900.
                        *sntpstate.ms_since_1970 = (t >> 11) * 1000 + f;
                        update_us_since_boot(sntpstate.localts, ourmiddle_us);
                    }
                }
                else
                {
                    // kiss-of-death, server says fuck off.
                    // reference_identifier_fourcc will tell us why.
                    // but there's actually not much to do... we just drop the packet, fail the whole request, and retry with a different server next time around.
                }
            }
        }
    }

    pbuf_free(p);
    // after receiving a(ny) packet we are done.
    sntpstate.seq = GETNTP_SEQ_CLOSE;
}

static void WIFIFUNC(handle_getntp)(const char* host, uint64_t* ms_since_1970, absolute_time_t* localts)
{
    PROFILE_THIS_FUNC;

    sntpstate.seq = GETNTP_SEQ_RESOLVE;
    sntpstate.ms_since_1970 = ms_since_1970;
    *sntpstate.ms_since_1970 = 0;
    sntpstate.localts = localts;
    *sntpstate.localts = nil_time;
    err_t       err = ERR_OK;

    while (true)
    {
        cyw43_arch_poll();
        switch (sntpstate.seq)
        {
            case GETNTP_SEQ_RESOLVE:
                cyw43_arch_lwip_begin();
                err = dns_gethostbyname(host, &sntpstate.remote_addr, common_domainfound_callback, &sntpstate);
                cyw43_arch_lwip_end();
                if (err == ERR_OK)
                    sntpstate.seq = GETNTP_SEQ_CONNECT;
                else
                if (err == ERR_INPROGRESS)
                    sntpstate.seq = GETNTP_SEQ_RESOLVE_WAITING;
                else
                {
                    // malformed hostname
                    return;
                }
                break;

            case GETNTP_SEQ_CONNECT:
                sntpstate.sock = udp_new();
                assert(sntpstate.sock != NULL);

                cyw43_arch_lwip_begin();
                err = udp_connect(sntpstate.sock, &sntpstate.remote_addr, 123);
                assert(err == ERR_OK);
                cyw43_arch_lwip_end();
                sntpstate.seq = GETNTP_SEQ_SEND;
                break;
                // we could just fall-through here but instead we'll do once around the merrygoround for the benefit of cyw43_arch_poll() above.

            case GETNTP_SEQ_SEND:
            {
                sntpstate.reqsend_us = get_absolute_time();
                // prep packet to send to server
                sntpstate.packet.leap_indicator = 0;
                sntpstate.packet.version = 4;
                sntpstate.packet.mode = 3;  // meaning client
                sntpstate.packet.stratum = 0;
                sntpstate.packet.poll = 0;
                sntpstate.packet.precision = 0;
                sntpstate.packet.root_delay = 0;
                sntpstate.packet.root_dispersion = 0;
                *((uint32_t*) &sntpstate.packet.reference_identifier_fourcc[0]) = 0;
                sntpstate.packet.reference_timestamp = 0;
                sntpstate.packet.originate_timestamp = 0;
                sntpstate.packet.receive_timestamp = 0;
                // transmit timestamp is arbitrary, server just needs to relay it back to us (as a sanity check).
                sntpstate.packet.transmit_timestamp = to_us_since_boot(sntpstate.reqsend_us);

                cyw43_arch_lwip_begin();
                struct pbuf* p = pbuf_alloc_reference((void*) &sntpstate.packet, sizeof(sntpstate.packet), PBUF_REF);
                udp_recv(sntpstate.sock, getntp_recv, &sntpstate);
                udp_send(sntpstate.sock, p);
                pbuf_free(p);
                cyw43_arch_lwip_end();
                sntpstate.seq = GETNTP_SEQ_RECV;
                break;
            }

            case GETNTP_SEQ_RECV:
                // all handling is done in getntp_recv() callback.
                // we'll wait at most 1 sec for a response, any *time* stamp later than that is not much use.
                // rather than wait longer, we better fail early and try again.
                if (absolute_time_diff_us(sntpstate.reqsend_us, get_absolute_time()) > 1000000)
                {
                    sntpstate.seq = GETNTP_SEQ_ERROR;
                    break;
                }
                // fallthrough is fine.

            case GETNTP_SEQ_RESOLVE_WAITING:
                // keep polling until callback says we are done.
                yield_and_wait4time(make_timeout_time_ms(10));
                break;

            case GETNTP_SEQ_CLOSE:
                // fallthrough ok
            case GETNTP_SEQ_ERROR:     // FIXME: not sure we need both...
                if (sntpstate.sock != NULL)
                {
                    cyw43_arch_lwip_begin();
                    udp_disconnect(sntpstate.sock);
                    udp_remove(sntpstate.sock);
                    cyw43_arch_lwip_end();
                    sntpstate.sock = NULL;
                }
                return;

            default:
                assert(false);
        }
    } // while true
}

static uint32_t WIFIFUNC(wififunc)(uint32_t param)
{
    PROFILE_THIS_FUNC;

    bool keepspinning = true;

    while (keepspinning)
    {
        if (!rb_is_empty(&cmdindices))
        {
            // not empty, so do the thing.
            CmdRingbufferEntry&  c = cmdringbuffer[rb_peek_front(&cmdindices)];

            switch (c.cmd)
            {
                case CONNECT:
                    handle_connect(c.connect.ssid, c.connect.pw, c.connect.success);
                    break;
                    
                case DISCONNECT:
                    handle_disconnect();
                    keepspinning = false;
                    break;
                
                case SENDUDP:
                    handle_sendudptcp<false>(&c.send_udp_or_tcp);
                    break;
                case SENDTCP:
                    handle_sendudptcp<true>(&c.send_udp_or_tcp);
                    break;

                case GETNTP:
                    handle_getntp(c.getntp.host, c.getntp.ms_since_1970, c.getntp.localts);
                    break;

                case HTTPHEAD:
                    handle_httphead(c.httphead.host, c.httphead.url, c.httphead.port, c.httphead.responsebuffer, c.httphead.bufferlength);
                    break;

                default:
                    assert(false);
                    break;
            }

#ifndef NDEBUG
            c.cmd = NONE;
#endif
            signal(&c.waitable);

            rb_pop_front(&cmdindices);
        }

        if (!keepspinning)
            break;

        yield_and_wait4signal(&newcmdswaitable);
    }

    return 0;
}

Waitable* WIFIFUNC(disconnect_wifi)()
{
    PROFILE_THIS_FUNC;

    // safe to "start" multiple times.
    // FIXME: but yielding too often is unnecessary
    yield_and_start(wififunc, 0, &wifiblock);

    while (rb_is_full(&cmdindices))
    {
        // FIXME: does not work with countable semaphores!
        __breakpoint();
        yield_and_wait4signal(&cmdringbuffer[rb_peek_front(&cmdindices)].waitable);
    }

    CmdRingbufferEntry&  c = cmdringbuffer[rb_push_back(&cmdindices)];
    c.cmd = DISCONNECT;

    // tell driver that there are new cmds waiting.
    signal(&newcmdswaitable);

    return &c.waitable;
}

Waitable* WIFIFUNC(connect_wifi)(const char* ssid, const char* pw, bool* success)
{
    PROFILE_THIS_FUNC;

    // safe to "start" multiple times.
    // FIXME: but yielding too often is unnecessary
    yield_and_start(wififunc, 0, &wifiblock);


    while (rb_is_full(&cmdindices))
    {
        // FIXME: does not work with countable semaphores!
        __breakpoint();
        yield_and_wait4signal(&cmdringbuffer[rb_peek_front(&cmdindices)].waitable);
    }

    CmdRingbufferEntry&  c = cmdringbuffer[rb_push_back(&cmdindices)];
    c.cmd = CONNECT;
    c.connect.success = success;
    c.connect.ssid = ssid;
    c.connect.pw = pw;

    // tell driver that there are new cmds waiting.
    signal(&newcmdswaitable);

    return &c.waitable;
}

Waitable* WIFIFUNC(get_ntp)(const char* host, uint64_t* ms_since_1970, absolute_time_t* localts)
{
    PROFILE_THIS_FUNC;

    assert(ms_since_1970 != NULL);
    assert(localts != NULL);

    // safe to "start" multiple times.
    // FIXME: but yielding too often is unnecessary
    yield_and_start(wififunc, 0, &wifiblock);

    while (rb_is_full(&cmdindices))
    {
        // FIXME: does not work with countable semaphores!
        __breakpoint();
        yield_and_wait4signal(&cmdringbuffer[rb_peek_front(&cmdindices)].waitable);
    }

    CmdRingbufferEntry&  c = cmdringbuffer[rb_push_back(&cmdindices)];
    c.cmd = GETNTP;
    c.getntp.host = host;
    c.getntp.ms_since_1970 = ms_since_1970;
    c.getntp.localts = localts;

    // tell driver that there are new cmds waiting.
    signal(&newcmdswaitable);

    return &c.waitable;
}

Waitable* WIFIFUNC(send_tcp)(const char* host, int port, const char* buffer, int bufferlength, bool* success, char* responsebuffer, int* responsebufferlength)
{
    PROFILE_THIS_FUNC;

    // safe to "start" multiple times.
    // FIXME: but yielding too often is unnecessary
    yield_and_start(wififunc, 0, &wifiblock);

    while (rb_is_full(&cmdindices))
    {
        // FIXME: does not work with countable semaphores!
        __breakpoint();
        yield_and_wait4signal(&cmdringbuffer[rb_peek_front(&cmdindices)].waitable);
    }

    CmdRingbufferEntry&  c = cmdringbuffer[rb_push_back(&cmdindices)];
    c.cmd = SENDTCP;
    c.send_udp_or_tcp.host = host;
    c.send_udp_or_tcp.port = port;
    c.send_udp_or_tcp.buffer = buffer;
    c.send_udp_or_tcp.bufferlength = bufferlength;
    c.send_udp_or_tcp.success = success;
    c.send_udp_or_tcp.responsebuffer = responsebuffer;
    c.send_udp_or_tcp.responselength = responsebufferlength;

    // tell driver that there are new cmds waiting.
    signal(&newcmdswaitable);

    return &c.waitable;
}

Waitable* WIFIFUNC(send_udp)(const char* host, int port, const char* buffer, int bufferlength, bool* maybesuccess, char* responsebuffer, int* responsebufferlength)
{
    PROFILE_THIS_FUNC;

    // safe to "start" multiple times.
    // FIXME: but yielding too often is unnecessary
    yield_and_start(wififunc, 0, &wifiblock);

    while (rb_is_full(&cmdindices))
    {
        // FIXME: does not work with countable semaphores!
        __breakpoint();
        yield_and_wait4signal(&cmdringbuffer[rb_peek_front(&cmdindices)].waitable);
    }

    CmdRingbufferEntry&  c = cmdringbuffer[rb_push_back(&cmdindices)];
    c.cmd = SENDUDP;
    c.send_udp_or_tcp.host = host;
    c.send_udp_or_tcp.port = port;
    c.send_udp_or_tcp.buffer = buffer;
    c.send_udp_or_tcp.bufferlength = bufferlength;
    c.send_udp_or_tcp.success = maybesuccess;
    c.send_udp_or_tcp.responsebuffer = responsebuffer;
    c.send_udp_or_tcp.responselength = responsebufferlength;

    // tell driver that there are new cmds waiting.
    signal(&newcmdswaitable);

    return &c.waitable;
}

Waitable* WIFIFUNC(httpreq_head)(const char* host, const char* url, int port, char* responsebuffer, int* bufferlength)
{
    if (responsebuffer != NULL)
        assert(bufferlength != NULL);

    // safe to "start" multiple times.
    // FIXME: but yielding too often is unnecessary
    yield_and_start(wififunc, 0, &wifiblock);


    while (rb_is_full(&cmdindices))
    {
        // FIXME: does not work with countable semaphores!
        __breakpoint();
        yield_and_wait4signal(&cmdringbuffer[rb_peek_front(&cmdindices)].waitable);
    }

    CmdRingbufferEntry&  c = cmdringbuffer[rb_push_back(&cmdindices)];
    c.cmd = HTTPHEAD;
    c.httphead.host = host;
    c.httphead.url = url;
    c.httphead.port = port;
    c.httphead.responsebuffer = responsebuffer;
    c.httphead.bufferlength = bufferlength;

    // tell driver that there are new cmds waiting.
    signal(&newcmdswaitable);

    return &c.waitable;
}
