#include <string.h>
#include <stdbool.h>
#include "lwip/tcp.h"
#include "pico/cyw43_arch.h"
#include "pico/time.h"
#include "cli_transport_telnet.h"

/*=======================================================================================
 * Private Defines
 *=======================================================================================*/

#define CLI_TELNET_OK  (0)
#define CLI_TELNET_ERR (-1)

#define TELNET_IAC  255U
#define TELNET_DONT 254U
#define TELNET_DO   253U
#define TELNET_WONT 252U
#define TELNET_WILL 251U
#define TELNET_SB   250U
#define TELNET_SE   240U

#define TELNET_OPT_ECHO     1U
#define TELNET_OPT_SGA      3U
#define TELNET_OPT_NAWS     31U
#define TELNET_OPT_LINEMODE 34U

#define TELNET_STATE_NORMAL 0U
#define TELNET_STATE_IAC    1U
#define TELNET_STATE_VERB   2U
#define TELNET_STATE_SB     3U
#define TELNET_STATE_SB_IAC 4U

#ifndef CLI_TELNET_MAX_SB_LEN
/* Longest sub-negotiation payload accepted; a longer block (no IAC SE from a broken client)
 * is abandoned so it cannot swallow the input that follows. */
#define CLI_TELNET_MAX_SB_LEN 64U
#endif

#ifndef CLI_TELNET_SEND_TIMEOUT_MS
/* How long output may wait for a client that does not read it (send buffer full): the
 * session then ends instead of the device waiting forever. */
#define CLI_TELNET_SEND_TIMEOUT_MS 5000U
#endif

/*=======================================================================================
 * Private Variables
 *=======================================================================================*/

static const uint8_t s_telnet_negotiation[] = {
    TELNET_IAC, TELNET_WILL, TELNET_OPT_ECHO,
    TELNET_IAC, TELNET_WILL, TELNET_OPT_SGA,
    TELNET_IAC, TELNET_DONT, TELNET_OPT_LINEMODE,
    TELNET_IAC, TELNET_DO,   TELNET_OPT_NAWS,
};

/*=======================================================================================
 * Private Function Prototypes
 *=======================================================================================*/

static struct tcp_pcb *telnet_pcb_from_ctx(cli_telnet_ctx_t *ctx);
static int telnet_send_all(cli_telnet_ctx_t *ctx, const uint8_t *buf, int len);
static err_t telnet_output(cli_telnet_ctx_t *ctx);
static void telnet_respond(cli_telnet_ctx_t *ctx, uint8_t verb, uint8_t opt);
static bool telnet_process_byte(cli_telnet_ctx_t *ctx, uint8_t byte, uint8_t *out);
static cli_transport_ret_t cli_telnet_transport_available(void *raw_ctx);
static cli_transport_ret_t cli_telnet_transport_read(void *raw_ctx);
static cli_transport_ret_t cli_telnet_transport_write(void *raw_ctx, const uint8_t *buf, cli_transport_buflen_t len);
static cli_transport_ret_t cli_telnet_transport_flush(void *raw_ctx);

static err_t tcp_recv_cb(void *arg, struct tcp_pcb *tpcb,
                         struct pbuf *p, err_t err);
static void tcp_err_cb(void *arg, err_t err);

/*=======================================================================================
 * Public Functions
 *=======================================================================================*/

void cli_transport_telnet_init(cli_telnet_ctx_t *ctx,
                               cli_transport_struct_t *transport,
                               struct tcp_pcb *client_pcb)
{
  if (ctx && transport) {
    memset(ctx, 0, sizeof(*ctx));
    ctx->client_pcb = client_pcb;
    ctx->state = TELNET_STATE_NORMAL;

    if (client_pcb) {
      cyw43_arch_lwip_begin();
      tcp_arg(client_pcb, ctx);
      tcp_recv(client_pcb, tcp_recv_cb);
      tcp_err(client_pcb, tcp_err_cb);
      tcp_nagle_disable(client_pcb);
      cyw43_arch_lwip_end();
    }

    transport->available = cli_telnet_transport_available;
    transport->read = cli_telnet_transport_read;
    transport->write = cli_telnet_transport_write;
    transport->flush = cli_telnet_transport_flush;
    transport->ctx = ctx;
    transport->kind = CLI_TRANSPORT_TELNET;
  }
}

void cli_transport_telnet_set_client(cli_telnet_ctx_t *ctx,
                                     struct tcp_pcb *client_pcb)
{
  if (ctx) {
    cli_transport_telnet_close(ctx); /* the previous client, and its queued input */
    ctx->client_pcb = client_pcb;
    ctx->state = TELNET_STATE_NORMAL;
    ctx->verb = 0;
    ctx->sb_len = 0;
    ctx->negotiated = false;
    ctx->local_echo = false;
    ctx->local_sga = false;
    ctx->remote_naws = false;
    ctx->remote_linemode_rejected = false;
    ctx->rx_len = 0;
    ctx->rx_pos = 0;
    ctx->started = false;
    ctx->input_started = false;

    if (client_pcb) {
      cyw43_arch_lwip_begin();
      tcp_arg(client_pcb, ctx);
      tcp_recv(client_pcb, tcp_recv_cb);
      tcp_err(client_pcb, tcp_err_cb);
      tcp_nagle_disable(client_pcb);
      cyw43_arch_lwip_end();
    }
  }
}

void cli_transport_telnet_close(cli_telnet_ctx_t *ctx)
{
  if (ctx) {
    cyw43_arch_lwip_begin();
    if (ctx->client_pcb != NULL) {
      struct tcp_pcb *pcb = ctx->client_pcb;
      /* Nothing may call back into this context once the connection is closed. */
      tcp_arg(pcb, NULL);
      tcp_recv(pcb, NULL);
      tcp_err(pcb, NULL);
      if (tcp_close(pcb) != ERR_OK) {
        tcp_abort(pcb); /* out of memory for the FIN: drop the connection instead */
      }
      ctx->client_pcb = NULL;
    }
    if (ctx->rx_queue != NULL) {
      pbuf_free(ctx->rx_queue);
      ctx->rx_queue = NULL;
    }
    ctx->peer_closed = false;
    cyw43_arch_lwip_end();
  }
}

void cli_transport_telnet_negotiate(cli_telnet_ctx_t *ctx)
{
  int rc = CLI_TELNET_ERR;

  if (ctx) {
    rc = telnet_send_all(ctx, s_telnet_negotiation, (int)sizeof(s_telnet_negotiation));
    if (rc >= 0) {
      (void)telnet_output(ctx);
    }
  }

  if (rc >= 0) {
    ctx->negotiated = true;
    ctx->local_echo = true;
    ctx->local_sga = true;
    ctx->remote_linemode_rejected = true;
    ctx->remote_naws = true;
  }
}

bool cli_transport_telnet_client_connected(const cli_telnet_ctx_t *ctx)
{
  bool connected = false;

  if (ctx && ctx->client_pcb != NULL) {
    connected = true;
  }

  return connected;
}

int8_t cli_telnet_handler(cli_telnet_ctx_t *ctx)
{
  int8_t result = CLI_OK;
  u16_t raw_len = 0U;
  bool connected = false;
  bool closed = false;

  if (!ctx) {
    result = CLI_ERR;
  } else if (ctx->rx_pos < ctx->rx_len) {
    /* Decoded input is still waiting to be read. */
  } else {
    /* The receive callback runs in the background: the queue is shared under the lwIP lock.
     * Only the bytes taken are acknowledged, so while the CLI is busy TCP holds the rest
     * back (the window closes) instead of anything being dropped. */
    cyw43_arch_lwip_begin();
    connected = (ctx->client_pcb != NULL);
    if (connected && ctx->rx_queue != NULL) {
      raw_len = pbuf_copy_partial(ctx->rx_queue, ctx->raw_buf, (u16_t)sizeof(ctx->raw_buf), 0U);
      ctx->rx_queue = pbuf_free_header(ctx->rx_queue, raw_len);
      tcp_recved(ctx->client_pcb, raw_len);
    }
    closed = ctx->peer_closed && ctx->rx_queue == NULL;
    cyw43_arch_lwip_end();

    if (!connected) {
      result = CLI_ERR; /* reset or failed (tcp_err_cb) */
    } else if (raw_len > 0U) {
      bool accept_input = ctx->started;

      ctx->rx_len = 0;
      ctx->rx_pos = 0;
      for (u16_t i = 0U; i < raw_len; i++) {
        uint8_t out;
        /* One decoded byte at most per raw byte: rx_buf (as large as raw_buf) cannot fill. */
        if (telnet_process_byte(ctx, ctx->raw_buf[i], &out) && accept_input &&
            (ctx->input_started || out > 0x20U)) {
          ctx->input_started = true;
          ctx->rx_buf[ctx->rx_len++] = out;
        }
      }
      if (!ctx->started && !accept_input) {
        ctx->started = true;
        ctx->input_started = false;
      }
    } else if (closed) {
      result = CLI_ERR; /* the client closed and everything it sent has been decoded */
    } else if (!ctx->started) {
      ctx->started = true;
      ctx->input_started = false;
    } else {
      /* Nothing received yet. */
    }
  }

  return result;
}

/*=======================================================================================
 * Private Functions
 *=======================================================================================*/

static struct tcp_pcb *telnet_pcb_from_ctx(cli_telnet_ctx_t *ctx)
{
  struct tcp_pcb *pcb = NULL;
  if (ctx) {
    pcb = ctx->client_pcb;
  }
  return pcb;
}

/**
 * @brief Queue @p len bytes for sending, waiting while lwIP's send buffer is full.
 *
 * Takes the lwIP lock for each attempt and releases it while waiting: the acknowledgements
 * that free buffer space are processed in the background under that lock. A client that
 * reads nothing for @c CLI_TELNET_SEND_TIMEOUT_MS ends its session; a reset (tcp_err_cb)
 * ends the wait at once. Must not be called from an lwIP callback (it may sleep).
 *
 * @return @p len, or @c CLI_TELNET_ERR.
 */
static int telnet_send_all(cli_telnet_ctx_t *ctx, const uint8_t *buf, int len)
{
  int sent = 0;
  int result = len;
  absolute_time_t deadline = make_timeout_time_ms(CLI_TELNET_SEND_TIMEOUT_MS);

  while (sent < len && result >= 0) {
    err_t err = ERR_MEM;
    u16_t chunk = 0U;

    cyw43_arch_lwip_begin();
    if (ctx->client_pcb == NULL) {
      result = CLI_TELNET_ERR; /* reset or closed */
    } else {
      u16_t avail = tcp_sndbuf(ctx->client_pcb);
      chunk = (len - sent > (int)avail) ? avail : (u16_t)(len - sent);
      if (chunk > 0U) {
        err = tcp_write(ctx->client_pcb, buf + sent, chunk, TCP_WRITE_FLAG_COPY);
      }
      if (err != ERR_OK) {
        /* Buffer or segment queue full: push out what is queued so it can be acknowledged. */
        (void)tcp_output(ctx->client_pcb);
      }
    }
    cyw43_arch_lwip_end();

    if (result < 0) {
      /* Connection gone. */
    } else if (err == ERR_OK) {
      sent += chunk;
      deadline = make_timeout_time_ms(CLI_TELNET_SEND_TIMEOUT_MS); /* progress */
    } else if (err == ERR_MEM && !time_reached(deadline)) {
      sleep_ms(1); /* wait for the client to acknowledge (lock released) */
    } else {
      result = CLI_TELNET_ERR; /* the client stopped reading, or another error */
    }
  }

  return result;
}

/** @brief Hand queued output to lwIP for sending now. */
static err_t telnet_output(cli_telnet_ctx_t *ctx)
{
  err_t out_err = ERR_VAL;

  cyw43_arch_lwip_begin();
  if (ctx->client_pcb != NULL) {
    out_err = tcp_output(ctx->client_pcb);
  }
  cyw43_arch_lwip_end();

  return out_err;
}

static void telnet_respond(cli_telnet_ctx_t *ctx, uint8_t verb, uint8_t opt)
{
  struct tcp_pcb *pcb = telnet_pcb_from_ctx(ctx);
  uint8_t response[3];
  bool send_response = true;

  if (pcb) {
    response[0] = TELNET_IAC;

    switch (verb) {
    case TELNET_DO:
      if (opt == TELNET_OPT_ECHO) {
        if (ctx->local_echo) {
          send_response = false;
        } else {
          ctx->local_echo = true;
          response[1] = TELNET_WILL;
        }
      } else if (opt == TELNET_OPT_SGA) {
        if (ctx->local_sga) {
          send_response = false;
        } else {
          ctx->local_sga = true;
          response[1] = TELNET_WILL;
        }
      } else {
        response[1] = TELNET_WONT;
      }
      break;

    case TELNET_DONT:
      if (opt == TELNET_OPT_ECHO && ctx->local_echo) {
        ctx->local_echo = false;
        response[1] = TELNET_WONT;
      } else if (opt == TELNET_OPT_SGA && ctx->local_sga) {
        ctx->local_sga = false;
        response[1] = TELNET_WONT;
      } else {
        send_response = false;
      }
      break;

    case TELNET_WILL:
      if (opt == TELNET_OPT_NAWS) {
        if (ctx->remote_naws) {
          send_response = false;
        } else {
          ctx->remote_naws = true;
          response[1] = TELNET_DO;
        }
      } else if (opt == TELNET_OPT_LINEMODE && ctx->remote_linemode_rejected) {
        send_response = false;
      } else {
        if (opt == TELNET_OPT_LINEMODE) {
          ctx->remote_linemode_rejected = true;
        }
        response[1] = TELNET_DONT;
      }
      break;

    case TELNET_WONT:
      if (opt == TELNET_OPT_NAWS && ctx->remote_naws) {
        ctx->remote_naws = false;
        response[1] = TELNET_DONT;
      } else {
        send_response = false;
      }
      break;

    default:
      send_response = false;
      break;
    }

    if (send_response) {
      response[2] = opt;
      if (telnet_send_all(ctx, response, 3) >= 0) {
        (void)telnet_output(ctx);
      }
    }
  }
}

static bool telnet_process_byte(cli_telnet_ctx_t *ctx, uint8_t byte, uint8_t *out)
{
  bool stored = false;

  switch (ctx->state) {
  case TELNET_STATE_NORMAL:
    if (byte == TELNET_IAC) {
      ctx->state = TELNET_STATE_IAC;
    } else if (byte != '\0') {
      *out = byte;
      stored = true;
    }
    break;

  case TELNET_STATE_IAC:
    if (byte == TELNET_IAC) {
      *out = 0xFF;
      ctx->state = TELNET_STATE_NORMAL;
      stored = true;
    } else if (byte == TELNET_DO || byte == TELNET_DONT ||
               byte == TELNET_WILL || byte == TELNET_WONT) {
      ctx->verb = byte;
      ctx->state = TELNET_STATE_VERB;
    } else if (byte == TELNET_SB) {
      ctx->state = TELNET_STATE_SB;
      ctx->sb_len = 0;
    } else {
      ctx->state = TELNET_STATE_NORMAL;
    }
    break;

  case TELNET_STATE_VERB:
    telnet_respond(ctx, ctx->verb, byte);
    ctx->state = TELNET_STATE_NORMAL;
    break;

  case TELNET_STATE_SB:
    if (byte == TELNET_IAC) {
      ctx->state = TELNET_STATE_SB_IAC;
    } else if (++ctx->sb_len > CLI_TELNET_MAX_SB_LEN) {
      /* No IAC SE in sight (broken client): give up on the block so input is not
       * swallowed forever. The remaining bytes are treated as normal data. */
      ctx->state = TELNET_STATE_NORMAL;
    } else {
      /* Sub-negotiation payload (e.g. NAWS size) is not used. */
    }
    break;

  case TELNET_STATE_SB_IAC:
    if (byte == TELNET_SE) {
      ctx->state = TELNET_STATE_NORMAL;
    } else if (byte == TELNET_IAC) {
      /* An escaped 0xFF is one payload byte: it counts towards the limit like any other. */
      if (++ctx->sb_len > CLI_TELNET_MAX_SB_LEN) {
        ctx->state = TELNET_STATE_NORMAL;
      } else {
        ctx->state = TELNET_STATE_SB;
      }
    } else {
      /* IAC <command> inside SB: the block was never terminated; resume normal parsing. */
      ctx->state = TELNET_STATE_IAC;
      stored = telnet_process_byte(ctx, byte, out);
    }
    break;

  default:
    ctx->state = TELNET_STATE_NORMAL;
    break;
  }

  return stored;
}

static err_t tcp_recv_cb(void *arg, struct tcp_pcb *tpcb,
                         struct pbuf *p, err_t err)
{
  /* Runs in lwIP's context with its lock held. The segments are queued as they are and
   * acknowledged once cli_telnet_handler() has taken them. */
  cli_telnet_ctx_t *ctx = (cli_telnet_ctx_t *)arg;

  (void)err;
  if (ctx == NULL) {
    /* Closed while data was in flight (callbacks are detached on close): discard it. */
    if (p != NULL) {
      tcp_recved(tpcb, p->tot_len);
      pbuf_free(p);
    }
  } else if (p == NULL) {
    /* The client closed its side: reported once the queued input has been decoded; the
     * connection itself is closed by cli_transport_telnet_close(). */
    ctx->peer_closed = true;
  } else if (ctx->rx_queue == NULL) {
    ctx->rx_queue = p;
  } else {
    pbuf_cat(ctx->rx_queue, p);
  }

  return ERR_OK;
}

static void tcp_err_cb(void *arg, err_t err)
{
  /* The connection was reset or failed and lwIP has already freed it. */
  cli_telnet_ctx_t *ctx = (cli_telnet_ctx_t *)arg;

  (void)err;
  if (ctx) {
    ctx->client_pcb = NULL;
    if (ctx->rx_queue != NULL) {
      pbuf_free(ctx->rx_queue);
      ctx->rx_queue = NULL;
    }
  }
}

static cli_transport_ret_t cli_telnet_transport_available(void *raw_ctx)
{
  cli_telnet_ctx_t *ctx = (cli_telnet_ctx_t *)raw_ctx;
  cli_transport_ret_t result = 0;

  if (ctx) {
    if (ctx->rx_pos < ctx->rx_len) {
      result = 1;
    } else if (cli_telnet_handler(ctx) == CLI_OK) {
      result = (ctx->rx_pos < ctx->rx_len) ? 1 : 0;
    } else {
      result = CLI_TELNET_ERR;
    }
  } else {
    result = CLI_TELNET_ERR;
  }

  return result;
}

static cli_transport_ret_t cli_telnet_transport_read(void *raw_ctx)
{
  cli_telnet_ctx_t *ctx = (cli_telnet_ctx_t *)raw_ctx;
  cli_transport_ret_t result = CLI_TELNET_ERR;

  if (ctx) {
    if (ctx->rx_pos < ctx->rx_len) {
      result = (cli_transport_ret_t)ctx->rx_buf[ctx->rx_pos++];
      if (ctx->rx_pos >= ctx->rx_len) {
        ctx->rx_pos = 0;
        ctx->rx_len = 0;
      }
    }
  }

  return result;
}

static cli_transport_ret_t cli_telnet_transport_write(void *raw_ctx,
    const uint8_t *buf, cli_transport_buflen_t len)
{
  cli_telnet_ctx_t *ctx = (cli_telnet_ctx_t *)raw_ctx;
  cli_transport_ret_t result = (cli_transport_ret_t)len;
  cli_transport_buflen_t start = 0;

  if (!ctx) {
    result = CLI_TELNET_ERR;
  } else {
    /* telnet_send_all() takes the lwIP lock itself (and releases it while waiting). */
    for (cli_transport_buflen_t i = 0; i < len && result >= 0; i++) {
      if (buf[i] == 0xFF) {
        if (i > start) {
          cli_transport_buflen_t span = i - start;
          if (telnet_send_all(ctx, buf + start, (int)span) < 0) {
            result = CLI_TELNET_ERR;
          }
        }
        if (result >= 0) {
          static const uint8_t esc[2] = {0xFF, 0xFF};
          if (telnet_send_all(ctx, esc, 2) < 0) {
            result = CLI_TELNET_ERR;
          }
        }
        start = i + 1;
      }
    }
    if (result >= 0 && start < len) {
      cli_transport_buflen_t tail = len - start;
      if (telnet_send_all(ctx, buf + start, (int)tail) < 0) {
        result = CLI_TELNET_ERR;
      }
    }
  }

  return result;
}

static cli_transport_ret_t cli_telnet_transport_flush(void *raw_ctx)
{
  cli_telnet_ctx_t *ctx = (cli_telnet_ctx_t *)raw_ctx;
  cli_transport_ret_t result = CLI_TELNET_ERR;

  if (ctx && telnet_output(ctx) == ERR_OK) {
    result = CLI_OK;
  }

  return result;
}
