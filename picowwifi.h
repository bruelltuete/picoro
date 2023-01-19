#pragma once
#include "coroutine.h"


// will yield internally
// ssid and pw need to outlive the returned Waitable!
extern Waitable* connect_wifi(const char* ssid, const char* pw, bool* success);
// disconnects and stops all wifi stuff. driver will exit.
extern Waitable* disconnect_wifi();

// does a http head request and returns the value of the date header (as an unparsed string).
// FIXME: incorrect function name...
extern Waitable* httpreq_head(const char* host, const char* url, int port, char* responsebuffer, int* bufferlength);

// sends the byte contents of buffer to host:port using one(!) udp packet.
// this is not meant for continous stream but rather for small amounts of one-off data.
extern Waitable* send_udp(const char* host, int port, const char* buffer, int bufferlength, bool* maybesuccess);

// sends the byte contents of buffer to host:port using tcp.
// does not wait for a response. should be used for small amounts of one-off data.
extern Waitable* send_tcp(const char* host, int port, const char* buffer, int bufferlength, bool* success);


// sntp only, see https://www.rfc-editor.org/rfc/rfc4330
// returns milliseconds since 1970 (unix epoch) that correspond to a local microsecond watchdog time.
extern Waitable* get_ntp(const char* host, uint64_t* ms_since_1970, absolute_time_t* localts);
