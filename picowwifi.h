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
