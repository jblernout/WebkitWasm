// The browser identity the engine presents: the host transport's User-Agent
// when it provides one (bib_host_string("useragent"), so navigator.userAgent
// and the wire fingerprint agree), else WebKit's standard UA.
#pragma once
#include "bib_host.h"
#include "UserAgent.h"
#include <stdlib.h>
#include <wtf/text/WTFString.h>

static inline String bibUserAgent()
{
    if (char* ua = bib_host_string("useragent")) {
        String s = String::fromUTF8(ua);
        free(ua);
        if (!s.isEmpty())
            return s;
    }
    return WebCore::standardUserAgent();
}
