/**
 * @file circular_buffer.h
 * @brief Byte circular buffer (FIFO) for one producer and one consumer.
 *
 * A fixed-size FIFO over storage the caller provides: 100 % static allocation.
 *
 * One side may run in an interrupt handler and the other in the main loop without
 * locking. The producer writes only @c head and the consumer only @c tail, and every
 * access to the shared state is volatile, so neither side sees the other's update before
 * the byte it covers. This needs each index read and written in one access, as a
 * @c size_t is on a 32-bit MCU (Cortex-M).
 *
 * One storage byte is kept free to tell a full buffer from an empty one: storage of
 * @c size bytes holds up to @c size - 1.
 *
 * @copyright Copyright (c) 2026 thesourcebuild and Muhammad Hassaan Shah
 *
 * SPDX-License-Identifier: MIT
 */

#ifndef OPENLIBCLI_CIRCULAR_BUFFER_H
#define OPENLIBCLI_CIRCULAR_BUFFER_H

/* C++ detection */
#ifdef __cplusplus
extern "C" {
#endif

/*=======================================================================================
 * Includes
 *=======================================================================================*/

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/*=======================================================================================
 * Public Defines
 *=======================================================================================*/

/* No public constants. */

/*=======================================================================================
 * Public Macros
 *=======================================================================================*/

/* No public function-like macros. */

/*=======================================================================================
 * Public Types
 *=======================================================================================*/

/** @brief A circular buffer; set up with @c circular_buffer_init(). */
typedef struct {
  volatile uint8_t *data; /**< Storage provided by the caller. */
  size_t size;            /**< Bytes of storage (one more than it holds). */
  volatile size_t head;   /**< Next slot to write; changed by the producer only. */
  volatile size_t tail;   /**< Next slot to read; changed by the consumer only. */
} circular_buffer_struct_t;

/*=======================================================================================
 * External Data Variables
 *=======================================================================================*/

/* No externally visible data variables. */

/*=======================================================================================
 * Public Function Prototypes
 *=======================================================================================*/

/**
 * @brief Set up an empty buffer over @p storage.
 *
 * Neither side may use the buffer while it is set up.
 *
 * @param[out] cb       Buffer to set up.
 * @param[in]  storage  Storage for the bytes; it must outlive the buffer.
 * @param[in]  size     Bytes of @p storage, at least 2 (the buffer holds @p size - 1).
 *
 * @return @c true when set up; @c false for a @c NULL argument or a @p size below 2.
 */
bool circular_buffer_init(circular_buffer_struct_t *cb, uint8_t *storage, size_t size);

/**
 * @brief Append a byte (producer side).
 *
 * @param[in,out] cb    Buffer.
 * @param[in]     byte  Byte to append.
 *
 * @return @c true when stored; @c false when the buffer is full (the byte is not stored).
 */
bool circular_buffer_put(circular_buffer_struct_t *cb, uint8_t byte);

/**
 * @brief Take the oldest byte (consumer side).
 *
 * @param[in,out] cb    Buffer.
 * @param[out]    byte  Receives the byte.
 *
 * @return @c true when a byte was taken; @c false when the buffer is empty.
 */
bool circular_buffer_get(circular_buffer_struct_t *cb, uint8_t *byte);

/**
 * @brief Number of bytes waiting to be taken.
 *
 * Called by the consumer it is a lower bound: the producer may add more meanwhile.
 *
 * @param[in] cb  Buffer.
 *
 * @return Bytes stored; 0 for a @c NULL buffer.
 */
size_t circular_buffer_count(const circular_buffer_struct_t *cb);

#ifdef __cplusplus
}
#endif

#endif /* OPENLIBCLI_CIRCULAR_BUFFER_H */

/*################################### END OF FILE ######################################*/
