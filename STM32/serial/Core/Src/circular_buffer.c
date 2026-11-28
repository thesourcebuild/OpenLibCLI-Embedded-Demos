/**
 * @file circular_buffer.c
 * @brief Byte circular buffer (FIFO) for one producer and one consumer.
 *
 * 100% static allocation: the bytes live in storage the caller provides.
 *
 * The producer stores the byte before it moves @c head past it, and the consumer reads
 * the byte before it moves @c tail past it. All of these are volatile accesses, which the
 * compiler keeps in program order, so an interrupt on the same core always sees a slot
 * either before or after the update, never in between.
 *
 * @copyright Copyright (c) 2026 thesourcebuild and Muhammad Hassaan Shah
 *
 * SPDX-License-Identifier: MIT
 */

/*=======================================================================================
 * Includes
 *=======================================================================================*/

#include "circular_buffer.h"

/*=======================================================================================
 * Private Defines
 *=======================================================================================*/

/* No private constants. */

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

/* No private variables. */

/*=======================================================================================
 * Private Function Prototypes
 *=======================================================================================*/

static size_t circular_buffer_next(const circular_buffer_struct_t *cb, size_t index);

/*=======================================================================================
 * Public Functions
 *=======================================================================================*/

bool circular_buffer_init(circular_buffer_struct_t *cb, uint8_t *storage, size_t size) {
  bool ok = false;

  if (cb != NULL && storage != NULL && size >= 2U) {
    cb->data = storage;
    cb->size = size;
    cb->head = 0U;
    cb->tail = 0U;
    ok = true;
  }

  return ok;
}

bool circular_buffer_put(circular_buffer_struct_t *cb, uint8_t byte) {
  bool stored = false;

  if (cb != NULL && cb->data != NULL) {
    size_t head = cb->head;
    size_t next = circular_buffer_next(cb, head);

    if (next != cb->tail) {
      cb->data[head] = byte;
      cb->head = next; /* published once the byte is in place */
      stored = true;
    }
  }

  return stored;
}

bool circular_buffer_get(circular_buffer_struct_t *cb, uint8_t *byte) {
  bool taken = false;

  if (cb != NULL && cb->data != NULL && byte != NULL) {
    size_t tail = cb->tail;

    if (tail != cb->head) {
      *byte = cb->data[tail];
      cb->tail = circular_buffer_next(cb, tail); /* the slot is free once it was read */
      taken = true;
    }
  }

  return taken;
}

size_t circular_buffer_count(const circular_buffer_struct_t *cb) {
  size_t count = 0U;

  if (cb != NULL) {
    /* Each index is read once: the other side may move its own meanwhile. */
    size_t head = cb->head;
    size_t tail = cb->tail;

    if (head >= tail) {
      count = head - tail;
    } else {
      count = cb->size - tail + head;
    }
  }

  return count;
}

/*=======================================================================================
 * Private Functions
 *=======================================================================================*/

/**
 * @brief Slot after @p index, wrapping to the start of the storage.
 *
 * @param[in] cb     Buffer.
 * @param[in] index  Slot index (below @c cb->size).
 *
 * @return The next slot index.
 */
static size_t circular_buffer_next(const circular_buffer_struct_t *cb, size_t index) {
  size_t next = index + 1U;

  if (next >= cb->size) {
    next = 0U;
  }

  return next;
}

/*################################### END OF FILE ######################################*/
