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

/*=======================================================================================
 * Includes
 *=======================================================================================*/

#include <string.h>
#include <stdbool.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <unistd.h>
#include <errno.h>
#include "cli_transport_telnet.h"

/*=======================================================================================
 * Private Defines
 *=======================================================================================*/

#define CLI_TELNET_OK (0)
#define CLI_TELNET_ERR (-1)

#define TELNET_IAC 255U
#define TELNET_DONT 254U
#define TELNET_DO 253U
#define TELNET_WONT 252U
#define TELNET_WILL 251U
#define TELNET_SB 250U
#define TELNET_SE 240U

#define TELNET_OPT_ECHO 1U
#define TELNET_OPT_SGA 3U
#define TELNET_OPT_NAWS 31U
#define TELNET_OPT_LINEMODE 34U

#define TELNET_STATE_NORMAL 0U
#define TELNET_STATE_IAC 1U
#define TELNET_STATE_VERB 2U
#define TELNET_STATE_SB 3U
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
/** @brief Initial IAC option bundle transmitted by @c cli_telnet_negotiate(). */
static const uint8_t s_telnet_negotiation[] = {
    TELNET_IAC,
    TELNET_WILL,
    TELNET_OPT_ECHO,
    TELNET_IAC,
    TELNET_WILL,
    TELNET_OPT_SGA,
    TELNET_IAC,
    TELNET_DONT,
    TELNET_OPT_LINEMODE,
    TELNET_IAC,
    TELNET_DO,
    TELNET_OPT_NAWS,
};

/*=======================================================================================
 * Private Function Prototypes
 *=======================================================================================*/

static int telnet_fd_from_ctx(cli_telnet_ctx_struct_t *ctx);
static void cli_telnet_respond(cli_telnet_ctx_struct_t *ctx, uint8_t verb, uint8_t opt);
static bool cli_telnet_process_byte(cli_telnet_ctx_struct_t *ctx, uint8_t byte, uint8_t *out);
static cli_transport_ret_t cli_telnet_transport_available(void *raw_ctx);
static cli_transport_ret_t cli_telnet_transport_read(void *raw_ctx);
static cli_transport_ret_t cli_telnet_transport_write(void *raw_ctx, const uint8_t *buf, cli_transport_buflen_t len);
static cli_transport_ret_t cli_telnet_transport_flush(void *raw_ctx);
static int cli_telnet_send_all(int fd, const uint8_t *buf, int len);

/*=======================================================================================
 * Public Functions
 *=======================================================================================*/

void cli_transport_telnet_init(cli_telnet_ctx_struct_t *ctx,
                               cli_transport_struct_t *transport,
                               int client_fd)
{
  if (!ctx || !transport)
    return;

  memset(ctx, 0, sizeof(*ctx));
  ctx->client_fd = client_fd;
  ctx->state = TELNET_STATE_NORMAL;

  /* Disable Nagle's algorithm to reduce lag for interactive CLI */
  if (client_fd >= 0)
  {
    int flag = 1;
    setsockopt(client_fd, IPPROTO_TCP, TCP_NODELAY, &flag, sizeof(flag));
  }

  transport->available = cli_telnet_transport_available;
  transport->read = cli_telnet_transport_read;
  transport->write = cli_telnet_transport_write;
  transport->flush = cli_telnet_transport_flush;
  transport->ctx = ctx;
  transport->kind = CLI_TRANSPORT_TELNET;
}

void cli_transport_telnet_set_client(cli_telnet_ctx_struct_t *ctx, int client_fd)
{
  if (ctx)
  {
    ctx->client_fd = client_fd;
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

void cli_transport_telnet_negotiate(cli_telnet_ctx_struct_t *ctx)
{
  int fd = telnet_fd_from_ctx(ctx);
  int8_t rc = CLI_TELNET_ERR;

  if (fd >= 0)
  {
    rc = cli_telnet_send_all(fd, s_telnet_negotiation,
                             (int)sizeof(s_telnet_negotiation));
  }

  if (rc >= 0)
  {
    ctx->negotiated = true;
    ctx->local_echo = true;
    ctx->local_sga = true;
    ctx->remote_linemode_rejected = true;
    ctx->remote_naws = true;
  }
}

bool cli_transport_telnet_client_connected(const cli_telnet_ctx_struct_t *ctx)
{
  bool connected = false;

  if (ctx && ctx->client_fd >= 0)
  {
    connected = true;
  }

  return connected;
}

int8_t cli_telnet_handler(cli_telnet_ctx_struct_t *ctx)
{
  int fd = telnet_fd_from_ctx(ctx);
  int8_t result = CLI_OK;

  if (!ctx)
  {
    result = CLI_ERR;
  }
  else if (ctx->rx_pos < ctx->rx_len)
  {
    result = CLI_OK;
  }
  else if (fd < 0)
  {
    result = CLI_ERR;
  }
  else
  {
    int n = recv(fd, ctx->rx_buf, (int)sizeof(ctx->rx_buf), MSG_DONTWAIT);
    if (n > 0)
    {
      uint16_t wp = 0;
      for (int i = 0; i < n && wp < sizeof(ctx->rx_buf); i++)
      {
        uint8_t out;
        if (cli_telnet_process_byte(ctx, ctx->rx_buf[i], &out))
        {
          ctx->rx_buf[wp++] = out;
        }
      }
      ctx->rx_len = wp;
      ctx->rx_pos = 0;
      if (ctx->rx_pos < ctx->rx_len)
      {
        result = CLI_OK;
      }
    }
    else if (n == 0)
    {
      result = CLI_ERR;
    }
    else
    {
      result = (errno == EWOULDBLOCK || errno == EAGAIN) ? CLI_OK : CLI_ERR;
    }

    if (result == CLI_OK && ctx->rx_pos >= ctx->rx_len)
    {
      struct sockaddr_in peer;
      socklen_t peerlen = sizeof(peer);
      if (getpeername(fd, (struct sockaddr *)&peer, &peerlen) < 0)
      {
        result = CLI_ERR;
      }
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
static int telnet_fd_from_ctx(cli_telnet_ctx_struct_t *ctx)
{
  int fd = -1;
  if (ctx && ctx->client_fd >= 0)
  {
    fd = ctx->client_fd;
  }
  return fd;
}

/**
 * @brief Block-transmit bytes over the network client.
 * @param[in] client Arduino network client.
 * @param[in] buf    Data buffer to transmit.
 * @param[in] len    Number of bytes to transmit.
 * @return Total bytes sent, or @c CLI_TELNET_ERR on failure.
 */
static int cli_telnet_send_all(int fd, const uint8_t *buf, int len)
{
  cli_transport_buflen_t sent = 0;
  cli_transport_ret_t result = len;

  while (sent < len && result >= 0)
  {
    int n = send(fd, buf + sent, (size_t)(len - sent), 0);
    if (n <= 0)
    {
      result = CLI_TELNET_ERR;
    }
    else
    {
      sent += n;
    }
  }

  if (result >= 0)
  {
    result = sent;
  }

  return result;
}

/**
 * @brief Respond to an incoming IAC option negotiation request.
 *
 * @param[in] ctx   Telnet context carrying the socket fd.
 * @param[in] verb  Telnet verb byte.
 * @param[in] opt   Telnet option byte.
 */
static void cli_telnet_respond(cli_telnet_ctx_struct_t *ctx, uint8_t verb, uint8_t opt)
{
  int fd = telnet_fd_from_ctx(ctx);
  uint8_t response[3];
  bool send_response = true;

  if (fd >= 0)
  {
    response[0] = TELNET_IAC;

    switch (verb)
    {
    case TELNET_DO:
      if (opt == TELNET_OPT_ECHO)
      {
        if (ctx->local_echo)
        {
          send_response = false;
        }
        else
        {
          ctx->local_echo = true;
          response[1] = TELNET_WILL;
        }
      }
      else if (opt == TELNET_OPT_SGA)
      {
        if (ctx->local_sga)
        {
          send_response = false;
        }
        else
        {
          ctx->local_sga = true;
          response[1] = TELNET_WILL;
        }
      }
      else
      {
        response[1] = TELNET_WONT;
      }
      break;

    case TELNET_DONT:
      if (opt == TELNET_OPT_ECHO && ctx->local_echo)
      {
        ctx->local_echo = false;
        response[1] = TELNET_WONT;
      }
      else if (opt == TELNET_OPT_SGA && ctx->local_sga)
      {
        ctx->local_sga = false;
        response[1] = TELNET_WONT;
      }
      else
      {
        send_response = false;
      }
      break;
    case TELNET_WILL:
      if (opt == TELNET_OPT_NAWS)
      {
        if (ctx->remote_naws)
        {
          send_response = false;
        }
        else
        {
          ctx->remote_naws = true;
          response[1] = TELNET_DO;
        }
      }
      else if (opt == TELNET_OPT_LINEMODE && ctx->remote_linemode_rejected)
      {
        send_response = false;
      }
      else
      {
        if (opt == TELNET_OPT_LINEMODE)
        {
          ctx->remote_linemode_rejected = true;
        }
        response[1] = TELNET_DONT;
      }
      break;

    case TELNET_WONT:
      if (opt == TELNET_OPT_NAWS && ctx->remote_naws)
      {
        ctx->remote_naws = false;
        response[1] = TELNET_DONT;
      }
      else
      {
        send_response = false;
      }
      break;

    default:
      send_response = false;
      break;
    }

    if (send_response)
    {
      response[2] = opt;
      (void)cli_telnet_send_all(fd, response, 3);
    }
  }
}

/**
 * @brief Process one byte through the IAC state machine.
 *
 * Feeds a single raw byte into the telnet state machine, which either
 * stores decoded user data into the rx buffer or triggers IAC negotiation
 * responses via @c cli_telnet_respond().
 *
 * @param[in,out] ctx  Telnet context (state machine state + rx buffer).
 * @param[in]     byte The next byte from the raw socket read.
 */
static bool cli_telnet_process_byte(cli_telnet_ctx_struct_t *ctx, uint8_t byte, uint8_t *out)
{
  bool stored = false;

  switch (ctx->state)
  {
  case TELNET_STATE_NORMAL:
    if (byte == TELNET_IAC)
    {
      ctx->state = TELNET_STATE_IAC;
    }
    else if (byte != '\0')
    {
      *out = byte;
      stored = true;
    }
    break;

  case TELNET_STATE_IAC:
    if (byte == TELNET_IAC)
    {
      *out = 0xFF;
      ctx->state = TELNET_STATE_NORMAL;
      stored = true;
    }
    else if (byte == TELNET_DO || byte == TELNET_DONT || byte == TELNET_WILL ||
             byte == TELNET_WONT)
    {
      ctx->verb = byte;
      ctx->state = TELNET_STATE_VERB;
    }
    else if (byte == TELNET_SB)
    {
      ctx->state = TELNET_STATE_SB;
    }
    else
    {
      ctx->state = TELNET_STATE_NORMAL;
    }
    break;

  case TELNET_STATE_VERB:
    cli_telnet_respond(ctx, ctx->verb, byte);
    ctx->state = TELNET_STATE_NORMAL;
    break;

  case TELNET_STATE_SB:
    if (byte == TELNET_IAC)
    {
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

/* Transport Callbacks ------------------------------------------------------------------*/

/**
 * @brief Check whether at least one decoded Telnet payload byte is ready.
 *
 * Returns 1 immediately when the internal rx buffer still has unconsumed
 * bytes.  When the buffer is empty, calls @c cli_telnet_handler() to pump
 * raw socket data through the IAC state machine.  If the handler refills the
 * buffer, returns 1; if the handler reports a disconnect or fatal socket
 * error, returns @c CLI_TELNET_ERR.
 *
 * This is the Stream-style readiness check the engine calls it first
 * before every @c cli_telnet_transport_read() to decide whether to loop.
 *
 * @param[in] raw_ctx  Telnet context pointer.
 *
 * @return 1 when at least one decoded byte is available,
 *         0 when no data is ready (nonâ€‘blocking),
 *         @c CLI_TELNET_ERR on disconnect or fatal error.
 */
static cli_transport_ret_t cli_telnet_transport_available(void *raw_ctx)
{
  cli_telnet_ctx_struct_t *ctx = (cli_telnet_ctx_struct_t *)raw_ctx;
  cli_transport_ret_t result = 0;

  if (ctx)
  {
    if (ctx->rx_pos < ctx->rx_len)
    {
      result = 1;
    }
    else if (cli_telnet_handler(ctx) == CLI_OK)
    {
      result = (ctx->rx_pos < ctx->rx_len) ? 1 : 0;
    }
    else
    {
      result = CLI_TELNET_ERR;
    }
  }
  else
  {
    result = CLI_TELNET_ERR;
  }

  return result;
}

/**
 * @brief Read one decoded Telnet payload byte.
 *
 * Must only be called when @c cli_telnet_transport_available() has returned a
 * positive value.  Consumes one byte from the internal rx buffer and advances
 * the read cursor.  When the last byte is consumed the buffer is reset
 * (rx_pos = rx_len = 0) so the next @c available() call will refill it.
 *
 * @param[in] raw_ctx  Telnet context pointer.
 *
 * @return The next decoded user byte (0..255), or @c CLI_TELNET_ERR if the
 *         buffer is unexpectedly empty.
 */
static cli_transport_ret_t cli_telnet_transport_read(void *raw_ctx)
{
  cli_telnet_ctx_struct_t *ctx = (cli_telnet_ctx_struct_t *)raw_ctx;
  cli_transport_ret_t result = CLI_TELNET_ERR;

  if (ctx)
  {
    if (ctx->rx_pos < ctx->rx_len)
    {
      result = (cli_transport_ret_t)ctx->rx_buf[ctx->rx_pos++];
      if (ctx->rx_pos >= ctx->rx_len)
      {
        ctx->rx_pos = 0;
        ctx->rx_len = 0;
      }
    }
  }

  return result;
}

/**
 * @brief Send bytes over Telnet, escaping literal @c 0xFF values.
 *
 * @param[in] raw_ctx  Telnet context pointer.
 * @param[in] buf      User payload to transmit.
 * @param[in] len      Number of bytes to transmit.
 *
 * @return Logical payload bytes sent, or @c -1 on error.
 */
static cli_transport_ret_t cli_telnet_transport_write(void *raw_ctx,
                                                      const uint8_t *buf, cli_transport_buflen_t len)
{
  cli_telnet_ctx_struct_t *ctx = (cli_telnet_ctx_struct_t *)raw_ctx;
  int fd = telnet_fd_from_ctx(ctx);
  cli_transport_ret_t result = (cli_transport_ret_t)len;
  cli_transport_buflen_t start = 0;

  if (fd < 0)
  {
    result = CLI_TELNET_ERR;
  }
  else
  {
    for (cli_transport_buflen_t i = 0; i < len && result >= 0; i++)
    {
      if (buf[i] == 0xFF)
      {
        if (i > start)
        {
          cli_transport_buflen_t span = i - start;
          if (cli_telnet_send_all(fd, buf + start, (int)span) < 0)
          {
            result = CLI_TELNET_ERR;
          }
        }
        if (result >= 0)
        {
          static const uint8_t esc[2] = {0xFF, 0xFF};
          if (cli_telnet_send_all(fd, esc, 2) < 0)
          {
            result = CLI_TELNET_ERR;
          }
        }

        start = i + 1;
      }
    }
    if (result >= 0 && start < len)
    {
      cli_transport_buflen_t tail = len - start;
      if (cli_telnet_send_all(fd, buf + start, (int)tail) < 0)
      {
        result = CLI_TELNET_ERR;
      }
    }
  }

  return result;
}

/**
 * @brief Flush callback for Telnet transport.
 *
 * @param[in] raw_ctx  Unused.
 *
 * @return Always @c 0 because @c TCP_NODELAY already minimizes buffering.
 */
static cli_transport_ret_t cli_telnet_transport_flush(void *raw_ctx)
{
  (void)raw_ctx;
  return CLI_OK;
}

/*################################### END OF FILE ######################################*/