#ifndef _LWIPOPTS_H
#define _LWIPOPTS_H

// Generally you would define your own explicit list of lwIP options
// (see https://www.nongnu.org/lwip/2_1_x/group__lwip__opts.html)
//
// Telnet writes the banner and login prompt back-to-back. The raw TCP API
// returns ERR_MEM instead of blocking when the send queue is too small.
#ifndef TCP_MSS
#define TCP_MSS 1460
#endif
#ifndef TCP_SND_BUF
#define TCP_SND_BUF (4 * TCP_MSS)
#endif
#ifndef TCP_WND
#define TCP_WND (4 * TCP_MSS)
#endif
#ifndef TCP_SND_QUEUELEN
#define TCP_SND_QUEUELEN ((4 * (TCP_SND_BUF) + (TCP_MSS - 1)) / TCP_MSS)
#endif
#ifndef MEMP_NUM_TCP_SEG
#define MEMP_NUM_TCP_SEG TCP_SND_QUEUELEN
#endif

// This example uses a common include to avoid repetition
#include "lwipopts_examples_common.h"

#endif
