/**
 * @file cli_transport_telnet.cpp
 * @brief Arduino Telnet transport adapter implementation for open-libcli.
 *
 * Implements Telnet IAC negotiation, option responses, protocol-byte
 * stripping on receive, and IAC escaping on transmit for Arduino network
 * clients.
 *
 *
 * @copyright Copyright (c) 2026 Muhammad Hassaan Shah
 */

#if defined(ARDUINO)

/*=======================================================================================
 * Includes
 *=======================================================================================*/

#include <Arduino.h>
#include <WiFi.h>
#include <Client.h>
#include <string.h>

#include "cli_transport_telnet.hpp"

/*=======================================================================================
 * Private Defines
 *=======================================================================================*/

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
 * Private Macros
 *=======================================================================================*/

/* No private function-like macros. */

/*=======================================================================================
 * Private Types
 *=======================================================================================*/

/* No private types. */

/*=======================================================================================
 * External Data Variables
 *=======================================================================================*/

/* No externally visible data variables. */

/*=======================================================================================
 * Private Variables
 *=======================================================================================*/

static const uint8_t s_telnet_negotiation[] = {
    TELNET_IAC, TELNET_WILL, TELNET_OPT_ECHO,     TELNET_IAC, TELNET_WILL, TELNET_OPT_SGA,
    TELNET_IAC, TELNET_DONT, TELNET_OPT_LINEMODE, TELNET_IAC, TELNET_DO,   TELNET_OPT_NAWS,
};

/*=======================================================================================
 * Private Function Prototypes
 *=======================================================================================*/

static Client *telnet_client_from_ctx(cli_telnet_arduino_ctx_t *ctx);
static cli_transport_ret_t telnet_send_all(Client *client, const uint8_t *buf, cli_transport_buflen_t len);
static void telnet_respond(cli_telnet_arduino_ctx_t *ctx, uint8_t verb, uint8_t opt);
static bool telnet_process_byte(cli_telnet_arduino_ctx_t *ctx, uint8_t byte, uint8_t *out);
static cli_transport_ret_t cli_telnet_transport_available(void *raw_ctx);
static cli_transport_ret_t cli_telnet_transport_read(void *raw_ctx);
static cli_transport_ret_t cli_telnet_transport_write(void *raw_ctx, const uint8_t *buf, cli_transport_buflen_t len);
static cli_transport_ret_t cli_telnet_transport_flush(void *raw_ctx);

/*=======================================================================================
 * Public Functions
 *=======================================================================================*/

void cli_transport_telnet_init(cli_telnet_arduino_ctx_t *ctx, cli_transport_struct_t *transport, void *client_handle) {
  if (!ctx || !transport) {
    return;
  }

  memset(ctx, 0, sizeof(*ctx));
  ctx->client_handle = client_handle;

  // Disable Nagle's algorithm to reduce lag for interactive CLI
  if (client_handle) {
    ((WiFiClient *)client_handle)->setNoDelay(true);
  }

  ctx->state = TELNET_STATE_NORMAL;

  transport->available = cli_telnet_transport_available;
  transport->read = cli_telnet_transport_read;
  transport->write = cli_telnet_transport_write;
  transport->flush = cli_telnet_transport_flush;
  transport->ctx = ctx;
  transport->kind = CLI_TRANSPORT_TELNET;
}

void cli_transport_telnet_set_client(cli_telnet_arduino_ctx_t *ctx, void *client_handle) {
  if (ctx) {
    ctx->client_handle = client_handle;
    ctx->state = TELNET_STATE_NORMAL;
    ctx->verb = 0;
    ctx->negotiated = false;
    ctx->local_echo = false;
    ctx->local_sga = false;
    ctx->remote_naws = false;
    ctx->remote_linemode_rejected = false;
    ctx->rx_len = 0;
    ctx->rx_pos = 0;
  }
}

void cli_transport_telnet_negotiate(cli_telnet_arduino_ctx_t *ctx) {
  Client *client = telnet_client_from_ctx(ctx);
  if (client) {
    if (telnet_send_all(client, s_telnet_negotiation, (cli_transport_buflen_t)sizeof(s_telnet_negotiation)) >= 0) {
      ctx->negotiated = true;
      ctx->local_echo = true;
      ctx->local_sga = true;
      ctx->remote_linemode_rejected = true;
      ctx->remote_naws = true;
    }
  }
}

bool cli_transport_telnet_client_connected(const cli_telnet_arduino_ctx_t *ctx) {
  bool connected = false;

  if (ctx && ctx->client_handle) {
    connected = ((Client *)ctx->client_handle)->connected();
  }

  return connected;
}

int8_t cli_telnet_handler(cli_telnet_arduino_ctx_t *ctx) {
  Client *client = telnet_client_from_ctx(ctx);
  int8_t result = CLI_OK;

  if (ctx && ctx->rx_pos < ctx->rx_len) {
    return CLI_OK;
  }

  if (!ctx || !client) {
    result = CLI_ERR;
  } else {
    while (client->available() > 0 && ctx->rx_len < sizeof(ctx->rx_buf)) {
      int raw = client->read();
      if (raw >= 0) {
        uint8_t out;
        if (telnet_process_byte(ctx, (uint8_t)raw, &out)) {
          ctx->rx_buf[ctx->rx_len++] = out;
        }
      } else {
        result = CLI_ERR;
        break;
      }
    }

    if (result == CLI_OK && !client->connected() && ctx->rx_pos >= ctx->rx_len) {
      result = CLI_ERR;
    }
  }

  return result;
}

/*=======================================================================================
 * Private Functions
 *=======================================================================================*/
 
/**
 * @brief Helper to cast the opaque handle back to an Arduino Client.
 * @param[in] ctx Telnet context.
 * @return Pointer to the Client object, or @c NULL if invalid.
 */
static Client *telnet_client_from_ctx(cli_telnet_arduino_ctx_t *ctx) {
  Client *client = NULL;

  if (ctx && ctx->client_handle) {
    client = (Client *)ctx->client_handle;
  }

  return client;
}

/**
 * @brief Block-transmit bytes over the network client.
 * @param[in] client Arduino network client.
 * @param[in] buf    Data buffer to transmit.
 * @param[in] len    Number of bytes to transmit.
 * @return Total bytes sent, or @c CLI_TELNET_ERR on failure.
 */
static cli_transport_ret_t telnet_send_all(Client *client, const uint8_t *buf, cli_transport_buflen_t len) {
  cli_transport_buflen_t sent = 0;
  cli_transport_ret_t rc = len;

  while (sent < len && rc >= 0) {
    size_t written = client->write(buf + sent, (size_t)(len - sent));
    if (written == 0U) {
      rc = CLI_TELNET_ERR;
    } else {
      sent = (cli_transport_buflen_t)(sent + written);
    }
  }

  if (rc >= 0) {
    rc = sent;
  }

  return rc;
}

/**
 * @brief Respond to an incoming IAC option negotiation request.
 * @param[in] ctx  Telnet context.
 * @param[in] verb Telnet verb byte.
 * @param[in] opt  Telnet option byte.
 */
static void telnet_respond(cli_telnet_arduino_ctx_t *ctx, uint8_t verb, uint8_t opt) {
  Client *client = telnet_client_from_ctx(ctx);
  uint8_t response[3];
  bool send_response = true;

  if (client) {
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
      (void)telnet_send_all(client, response, 3);
    }
  }
}

/**
 * @brief Process one byte through the IAC state machine.
 *
 * Feeds a single raw byte into the telnet state machine, which either
 * stores decoded user data into the rx buffer or triggers IAC negotiation
 * responses via @c telnet_respond().
 *
 * @param[in,out] ctx  Telnet context (state machine state + rx buffer).
 * @param[in]     byte The next byte from the raw socket read.
 */
static bool telnet_process_byte(cli_telnet_arduino_ctx_t *ctx, uint8_t byte, uint8_t *out) {
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
    } else if (byte == TELNET_DO || byte == TELNET_DONT || byte == TELNET_WILL ||
               byte == TELNET_WONT) {
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
    if (byte == TELNET_IAC) {
      ctx->state = TELNET_STATE_SB_IAC;
    }
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

/**
 * @brief Check if one decoded Telnet payload byte is available.
 * @param[in] raw_ctx  Telnet context pointer.
 * @return Positive when data is ready, @c 0 when no payload, or @c CLI_TELNET_ERR on error.
 */
static cli_transport_ret_t cli_telnet_transport_available(void *raw_ctx) {
  cli_telnet_arduino_ctx_t *ctx = (cli_telnet_arduino_ctx_t *)raw_ctx;
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

/**
 * @brief Read one decoded Telnet payload byte.
 * @param[in] raw_ctx  Telnet context pointer.
 * @return Byte value 0..255, or @c CLI_TELNET_ERR on error/no data.
 */
static cli_transport_ret_t cli_telnet_transport_read(void *raw_ctx) {
  cli_telnet_arduino_ctx_t *ctx = (cli_telnet_arduino_ctx_t *)raw_ctx;
  cli_transport_ret_t ret = CLI_TELNET_ERR;

  if (ctx) {
    if (ctx->rx_pos < ctx->rx_len) {
      ret = ctx->rx_buf[ctx->rx_pos++];
      if (ctx->rx_pos >= ctx->rx_len) {
        ctx->rx_pos = 0;
        ctx->rx_len = 0;
      }
    }
  }

  return ret;
}

/**
 * @brief Send bytes over Telnet, escaping literal @c 0xFF values.
 * @param[in] raw_ctx  Telnet context pointer.
 * @param[in] buf      User payload to transmit.
 * @param[in] len      Number of bytes to transmit.
 * @return Logical payload bytes sent, or @c -1 on error.
 */
static cli_transport_ret_t cli_telnet_transport_write(void *raw_ctx, const uint8_t *buf, cli_transport_buflen_t len) {
  cli_telnet_arduino_ctx_t *ctx = (cli_telnet_arduino_ctx_t *)raw_ctx;
  Client *client = telnet_client_from_ctx(ctx);
  cli_transport_ret_t rc = len;
  cli_transport_buflen_t start = 0;

  if (!client) {
    rc = CLI_TELNET_ERR;
  } else {
    for (cli_transport_buflen_t i = 0; i < len && rc >= 0; i++) {
      if (buf[i] == 0xFF) {
        if (i > start) {
          cli_transport_buflen_t span_len = i - start;
          if (telnet_send_all(client, buf + start, span_len) < 0) {
            rc = CLI_TELNET_ERR;
          }
        }

        if (rc >= 0) {
          static const uint8_t iac_escaped[2] = {0xFF, 0xFF};
          if (telnet_send_all(client, iac_escaped, 2) < 0) {
            rc = CLI_TELNET_ERR;
          }
        }

        start = i + 1;
      }
    }

    if (rc >= 0 && start < len) {
      cli_transport_buflen_t tail_len = len - start;
      if (telnet_send_all(client, buf + start, tail_len) < 0) {
        rc = CLI_TELNET_ERR;
      }
    }
  }

  return rc;
}

/**
 * @brief Flush the underlying network client.
 * @param[in] raw_ctx  Telnet context pointer.
 * @return Always @c 0.
 */
static cli_transport_ret_t cli_telnet_transport_flush(void *raw_ctx) {
  cli_telnet_arduino_ctx_t *ctx = (cli_telnet_arduino_ctx_t *)raw_ctx;
  Client *client = telnet_client_from_ctx(ctx);

  if (client) {
    client->flush();
  }
  return 0;
}

#endif

/*################################### END OF FILE ######################################*/
