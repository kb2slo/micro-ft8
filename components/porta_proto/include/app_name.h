// The app's name as both boards say it to the world: the sidekick's mDNS instance name and each
// board's boot line. One spelling everywhere, "micro-FT8": lowercase m, the mode in capitals, as the
// wordmark is (docs/README.md, The app's name). The web app's and the Worker's own constants match it.
//
// In porta_proto because it is the one component both idf.py projects compile; it is no part of the
// link's protocol.
#pragma once

#define MICROFT8_APP_NAME "micro-FT8"
