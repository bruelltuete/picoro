#include "picowwifi.h"
#include "pico/cyw43_arch.h"
#include "lwip/tcp.h"
#include "lwip/dns.h"
#define RINGBUFFER_SIZE 4
#include "ringbuffer.h"

enum WifiCmd
{
    CONNECT,
    DISCONNECT,

    HTTPHEAD,

    NONE = 0xdeadbeef
};

struct CmdRingbufferEntry
{
    WifiCmd             cmd;
    Waitable            waitable;

    bool*               success;

    union
    {
        struct
        {
            const char*     ssid;
            const char*     pw;
        } connect;

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


static void handle_disconnect()
{
    // FIXME: prob need to invalidate all outstanding requests/waitables

    int ec = cyw43_wifi_leave(&cyw43_state, CYW43_ITF_STA);
    // ignore if failed
    assert(ec == 0);
    // one last round of timeouts or other housekeeping.
    cyw43_arch_poll();
    // switch off the wifi chip.
    cyw43_arch_deinit();
}

static void handle_connect(const char* ssid, const char* pw, bool* success)
{
    // FIXME: hook the gpio 24 pin irq so we wake up when there is something coming from the wifi chip.
    cyw43_arch_init();
    cyw43_arch_enable_sta_mode();

    int ec = cyw43_arch_wifi_connect_async(ssid, pw, CYW43_AUTH_WPA2_AES_PSK);
    assert(ec == PICO_OK);

    while (true)
    {
        cyw43_arch_poll();

        int linkstatus = cyw43_tcpip_link_status(&cyw43_state, CYW43_ITF_STA);
        // ok?
        if (linkstatus == CYW43_LINK_UP)
        {
            if (success != NULL)
                *success = true;
            break;
        }
        // error?
        if (linkstatus < 0)
        {
            if (success != NULL)
                *success = false;
            break;
        }

        yield_and_wait4time(make_timeout_time_ms(10));
    }
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
    HTTPHEAD_SEQ_RESOLVE,
    HTTPHEAD_SEQ_RESOLVE_WAITING,
    HTTPHEAD_SEQ_CONNECT,
    HTTPHEAD_SEQ_CONNECT_WAITING,
    HTTPHEAD_SEQ_SEND,
    HTTPHEAD_SEQ_SEND_WAITING,
    HTTPHEAD_SEQ_RECV,
    HTTPHEAD_SEQ_CLOSE,
    HTTPHEAD_SEQ_ERROR,
};

struct HttpHeadState
{
    ip_addr_t   remote_addr;
    int         seq;

    // need to drag these around into tcpclient_recv_callback
    char*       responsebuffer;
    int*        bufferlength;
};

static void tcpclient_domainfound_callback(const char* name, const ip_addr_t* ipaddr, void* callback_arg)
{
    HttpHeadState* state = (HttpHeadState*) callback_arg;
    if (ipaddr == NULL)
    {
        // failed to resolve, for whatever reason.
        state->seq = HTTPHEAD_SEQ_ERROR;
        return;
    }

    state->remote_addr = *ipaddr;
    state->seq++;
}

static err_t tcpclient_connected_callback(void* arg, struct tcp_pcb* tpcb, err_t err)
{
    HttpHeadState* state = (HttpHeadState*) arg;

    // docs say parameter err is always ERR_OK, should prob have a defensive check here somewhere.

    // debug?
    cyw43_arch_lwip_check();

    state->seq++;
    return ERR_OK;
}

static err_t tcpclient_sent_callback(void* arg, struct tcp_pcb* tpcb, u16_t len)
{
    HttpHeadState* state = (HttpHeadState*) arg;

    // debug?
    cyw43_arch_lwip_check();

    // queue/send more data, or free up whatever buffer is now no longer required.

    state->seq++;
    return ERR_OK;
}

static err_t tcpclient_recv_callback(void* arg, struct tcp_pcb* tpcb, struct pbuf* p, err_t err)
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
    for (int i = 0; i < p->len - sizeof(DATE_TOKEN) - 1; ++i)
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

static void tcpclient_err_callback(void* arg, err_t err)
{
    HttpHeadState* state = (HttpHeadState*) arg;
    // beware: docs say tcp_pcb is gone at this point. but that does not seem to be true?!

    // debug?
    cyw43_arch_lwip_check();

    state->seq = HTTPHEAD_SEQ_ERROR;
}

// if there's only a single pbuf for the tx side, then lwip will copy the data into that single pbuf.
// in that case, no need for us to have a buffer to piece the string together. just let lwip do that in its pbuf.
#if !LWIP_NETIF_TX_SINGLE_PBUF
char        txbuffer[1400];     // FIXME: find out from lwip? slightly less than mtu
#endif

static void handle_httphead(const char* host, const char* url, int port, char* responsebuffer, int* bufferlength)
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
                err = dns_gethostbyname(host, &state.remote_addr, tcpclient_domainfound_callback, &state);
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
                tcp_recv(tcp_pcb, tcpclient_recv_callback);
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
        }
    } // while true
}

static uint32_t wififunc(uint32_t param)
{
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
                    handle_connect(c.connect.ssid, c.connect.pw, c.success);
                    break;
                    
                case DISCONNECT:
                    handle_disconnect();
                    keepspinning = false;
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

Waitable* disconnect_wifi()
{
    // safe to "start" multiple times.
    // FIXME: but yielding too often is unnecessary
    yield_and_start(wififunc, 0, &wifiblock);

    while (rb_is_full(&cmdindices))
    {
        // FIXME: does not work with countable semaphores!
        yield_and_wait4signal(&cmdringbuffer[rb_peek_front(&cmdindices)].waitable);
    }

    CmdRingbufferEntry&  c = cmdringbuffer[rb_push_back(&cmdindices)];
    c.cmd = DISCONNECT;

    // tell driver that there are new cmds waiting.
    signal(&newcmdswaitable);

    return &c.waitable;
}

Waitable* connect_wifi(const char* ssid, const char* pw, bool* success)
{
    // safe to "start" multiple times.
    // FIXME: but yielding too often is unnecessary
    yield_and_start(wififunc, 0, &wifiblock);


    while (rb_is_full(&cmdindices))
    {
        // FIXME: does not work with countable semaphores!
        yield_and_wait4signal(&cmdringbuffer[rb_peek_front(&cmdindices)].waitable);
    }

    CmdRingbufferEntry&  c = cmdringbuffer[rb_push_back(&cmdindices)];
    c.cmd = CONNECT;
    c.success = success;
    c.connect.ssid = ssid;
    c.connect.pw = pw;

    // tell driver that there are new cmds waiting.
    signal(&newcmdswaitable);

    return &c.waitable;
}

Waitable* httpreq_head(const char* host, const char* url, int port, char* responsebuffer, int* bufferlength)
{
    if (responsebuffer != NULL)
        assert(bufferlength != NULL);

    // safe to "start" multiple times.
    // FIXME: but yielding too often is unnecessary
    yield_and_start(wififunc, 0, &wifiblock);


    while (rb_is_full(&cmdindices))
    {
        // FIXME: does not work with countable semaphores!
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
