#include <string.h>
#include <stdbool.h>
#include "lwip/tcp.h"
#include "pico/cyw43_arch.h"
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
static int telnet_send_all(struct tcp_pcb *pcb, const uint8_t *buf, int len);
static err_t telnet_output(struct tcp_pcb *pcb);
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
  if (!ctx || !transport) return;

  memset(ctx, 0, sizeof(*ctx));
  ctx->client_pcb = client_pcb;
  ctx->state = TELNET_STATE_NORMAL;

  if (client_pcb) {
    tcp_arg(client_pcb, ctx);
    tcp_recv(client_pcb, tcp_recv_cb);
    tcp_err(client_pcb, tcp_err_cb);
    tcp_nagle_disable(client_pcb);
  }

  transport->available = cli_telnet_transport_available;
  transport->read = cli_telnet_transport_read;
  transport->write = cli_telnet_transport_write;
  transport->flush = cli_telnet_transport_flush;
  transport->ctx = ctx;
  transport->kind = CLI_TRANSPORT_TELNET;
}

void cli_transport_telnet_set_client(cli_telnet_ctx_t *ctx,
                                     struct tcp_pcb *client_pcb)
{
  if (ctx) {
    ctx->client_pcb = client_pcb;
    ctx->state = TELNET_STATE_NORMAL;
    ctx->verb = 0;
    ctx->negotiated = false;
    ctx->local_echo = false;
    ctx->local_sga = false;
    ctx->remote_naws = false;
    ctx->remote_linemode_rejected = false;
    ctx->rx_len = 0;
    ctx->rx_pos = 0;
    ctx->raw_len = 0;
    ctx->raw_pos = 0;
    ctx->started = false;
    ctx->input_started = false;

    if (client_pcb) {
      tcp_arg(client_pcb, ctx);
      tcp_recv(client_pcb, tcp_recv_cb);
      tcp_err(client_pcb, tcp_err_cb);
      tcp_nagle_disable(client_pcb);
    }
  }
}

void cli_transport_telnet_negotiate(cli_telnet_ctx_t *ctx)
{
  struct tcp_pcb *pcb = telnet_pcb_from_ctx(ctx);
  int8_t rc = CLI_TELNET_ERR;

  if (pcb) {
    cyw43_arch_lwip_begin();
    rc = telnet_send_all(pcb, s_telnet_negotiation,
                         (int)sizeof(s_telnet_negotiation));
    if (rc >= 0) {
      (void)telnet_output(pcb);
    }
    cyw43_arch_lwip_end();
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
  bool accept_input = false;

  if (!ctx) {
    result = CLI_ERR;
  } else if (ctx->rx_pos < ctx->rx_len) {
    result = CLI_OK;
  } else if (!ctx->client_pcb) {
    result = CLI_ERR;
  } else if (ctx->raw_pos < ctx->raw_len) {
    accept_input = ctx->started;

    ctx->rx_len = 0;
    ctx->rx_pos = 0;
    while (ctx->raw_pos < ctx->raw_len && ctx->rx_len < sizeof(ctx->rx_buf)) {
      uint8_t out;
      if (telnet_process_byte(ctx, ctx->raw_buf[ctx->raw_pos], &out)) {
        if (accept_input) {
          if (ctx->input_started || out > 0x20U) {
            ctx->input_started = true;
            ctx->rx_buf[ctx->rx_len++] = out;
          }
        }
      }
      ctx->raw_pos++;
    }
    ctx->raw_pos = 0;
    ctx->raw_len = 0;
    if (!ctx->started && !accept_input) {
      ctx->started = true;
      ctx->input_started = false;
    }
    if (ctx->rx_pos < ctx->rx_len) {
      result = CLI_OK;
    }
  } else if (!ctx->started) {
    ctx->started = true;
    ctx->input_started = false;
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

static int telnet_send_all(struct tcp_pcb *pcb, const uint8_t *buf, int len)
{
  int sent = 0;
  int result = CLI_TELNET_ERR;

  if (pcb) {
    result = len;
    while (sent < len && result >= 0) {
      u16_t avail = tcp_sndbuf(pcb);
      u16_t chunk = (u16_t)(len - sent);
      err_t err;

      if (avail == 0U) {
        result = CLI_TELNET_ERR;
      } else {
        if (chunk > avail) {
          chunk = avail;
        }
        err = tcp_write(pcb, buf + sent, chunk, TCP_WRITE_FLAG_COPY);
        if (err != ERR_OK) {
          result = CLI_TELNET_ERR;
        } else {
          sent += chunk;
        }
      }
    }
    result = (result >= 0) ? sent : result;
  }

  return result;
}

static err_t telnet_output(struct tcp_pcb *pcb)
{
  err_t out_err = ERR_VAL;

  if (pcb != NULL) {
    out_err = tcp_output(pcb);
  }

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
      cyw43_arch_lwip_begin();
      if (telnet_send_all(pcb, response, 3) >= 0) {
        (void)telnet_output(pcb);
      }
      cyw43_arch_lwip_end();
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
    } else {
      ctx->state = TELNET_STATE_NORMAL;
    }
    break;

  case TELNET_STATE_VERB:
    telnet_respond(ctx, ctx->verb, byte);
    ctx->state = TELNET_STATE_NORMAL;
    break;

  case TELNET_STATE_SB:
    ctx->state = (byte == TELNET_IAC) ? TELNET_STATE_SB_IAC : TELNET_STATE_SB;
    break;

  case TELNET_STATE_SB_IAC:
    ctx->state = (byte == TELNET_SE) ? TELNET_STATE_NORMAL : TELNET_STATE_SB;
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
  (void)err;
  cli_telnet_ctx_t *ctx = (cli_telnet_ctx_t *)arg;
  err_t result = ERR_OK;

  if (!ctx) { result = ERR_VAL; goto done; }
  if (p == NULL) {
    ctx->client_pcb = NULL;
    goto done;
  }

  cyw43_arch_lwip_begin();
  for (struct pbuf *q = p; q != NULL; q = q->next) {
    uint8_t *data = (uint8_t *)q->payload;
    for (u16_t i = 0; i < q->len && ctx->raw_len < sizeof(ctx->raw_buf); i++) {
      ctx->raw_buf[ctx->raw_len++] = data[i];
    }
  }
  tcp_recved(tpcb, p->tot_len);
  pbuf_free(p);
  cyw43_arch_lwip_end();

done:
  return result;
}

static void tcp_err_cb(void *arg, err_t err)
{
  (void)err;
  cli_telnet_ctx_t *ctx = (cli_telnet_ctx_t *)arg;
  if (ctx) ctx->client_pcb = NULL;
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
  struct tcp_pcb *pcb = telnet_pcb_from_ctx(ctx);
  cli_transport_ret_t result = (cli_transport_ret_t)len;
  cli_transport_buflen_t start = 0;

  if (!pcb) {
    result = CLI_TELNET_ERR;
  } else {
    cyw43_arch_lwip_begin();
    for (cli_transport_buflen_t i = 0; i < len && result >= 0; i++) {
      if (buf[i] == 0xFF) {
        if (i > start) {
          cli_transport_buflen_t span = i - start;
          if (telnet_send_all(pcb, buf + start, (int)span) < 0) {
            result = CLI_TELNET_ERR;
          }
        }
        if (result >= 0) {
          static const uint8_t esc[2] = {0xFF, 0xFF};
          if (telnet_send_all(pcb, esc, 2) < 0) {
            result = CLI_TELNET_ERR;
          }
        }
        start = i + 1;
      }
    }
    if (result >= 0 && start < len) {
      cli_transport_buflen_t tail = len - start;
      if (telnet_send_all(pcb, buf + start, (int)tail) < 0) {
        result = CLI_TELNET_ERR;
      }
    }
    cyw43_arch_lwip_end();
  }

  return result;
}

static cli_transport_ret_t cli_telnet_transport_flush(void *raw_ctx)
{
  cli_telnet_ctx_t *ctx = (cli_telnet_ctx_t *)raw_ctx;
  struct tcp_pcb *pcb = telnet_pcb_from_ctx(ctx);
  cli_transport_ret_t result = CLI_OK;

  if (!pcb) {
    result = CLI_TELNET_ERR;
  } else {
    cyw43_arch_lwip_begin();
    err_t err = telnet_output(pcb);
    cyw43_arch_lwip_end();
    result = (err == ERR_OK) ? CLI_OK : CLI_TELNET_ERR;
  }

  return result;
}
