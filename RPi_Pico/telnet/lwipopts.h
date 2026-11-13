#ifndef _LWIPOPTS_H
#define _LWIPOPTS_H

// Generally you would define your own explicit list of lwIP options
// (see https://www.nongnu.org/lwip/2_1_x/group__lwip__opts.html)
//
// The TCP sizes come from the common settings (send buffer and window of 8 * TCP_MSS).
// A send queue that fills up anyway makes the raw TCP API return ERR_MEM; the Telnet
// transport then waits for the client to acknowledge what was sent.

// This example uses a common include to avoid repetition
#include "lwipopts_examples_common.h"

#endif
