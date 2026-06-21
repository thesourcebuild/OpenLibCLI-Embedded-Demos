/**
 * @file cli_env_detect.h
 * @brief Compile-time OS / environment detection for C and C++ projects.
 *
 * Automatically classifies the build environment into one of three mutually
 * exclusive categories and exposes the result through the @c ENV_HAS_*
 * output macros:
 *
 *  - @c ENV_HAS_OS        - full hosted OS (Windows, Linux, macOS, …)
 *  - @c ENV_HAS_BAREMETAL - non-hosted target (bare-metal MCU, RTOS, Arduino, AVR, ARM Cortex-M,
 * RISC-V ...)
 *  - @c ENV_HAS_UNKNOWN   - could not determine the environment
 *
 * Detection order: **OS > Bare Metal > Unknown**.
 *
 * Build-time overrides (define exactly one):
 * @code
 *   -DENV_FORCE_OS=1
 *   -DENV_FORCE_BAREMETAL=1
 *   -DENV_FORCE_UNKNOWN=1
 * @endcode
 *
 * @note @c ENV_HAS_* are **output** macros defined by this header — do not
 *       define them yourself.  Use @c ENV_FORCE_* to override detection.
 *
 * @copyright Copyright (c) 2026 Muhammad Hassaan Shah.
 *
 * SPDX-License-Identifier: MIT
 */

#ifndef CLI_ENV_DETECT_H
#define CLI_ENV_DETECT_H

/* C++ detection */
#ifdef __cplusplus
extern "C" {
#endif

/*=======================================================================================
 * Includes
 *=======================================================================================*/

#if defined(__has_include)
#if __has_include(<TargetConditionals.h>)
#include <TargetConditionals.h>
#endif
#endif

/*=======================================================================================
 * Public Defines
 *=======================================================================================*/

/* Specific OS / Platform Detection -----------------------------------------------------*/

/** @defgroup ENV_Platform Platform Detection Macros
 *  @brief Set to 1 when the corresponding platform is detected, 0 otherwise.
 *  @{
 */

/** @brief 1 when compiling for Windows (Win32 or Win64). */
#if defined(_WIN32) || defined(_WIN64) || defined(__WINDOWS__) || defined(WIN32)
#define ENV_WINDOWS 1
#else
#define ENV_WINDOWS 0
#endif

/** @brief 1 when compiling for Linux. */
#if defined(__linux__) || defined(__linux)
#define ENV_LINUX 1
#else
#define ENV_LINUX 0
#endif

/** @brief 1 when compiling for Android. */
#if defined(__ANDROID__)
#define ENV_ANDROID 1
#else
#define ENV_ANDROID 0
#endif

/** @brief 1 when compiling for any BSD variant (FreeBSD, NetBSD, OpenBSD,
 * DragonFly). */
#if defined(__FreeBSD__) || defined(__NetBSD__) || defined(__OpenBSD__) || defined(__bsdi__) ||    \
    defined(__DragonFly__)
#define ENV_BSD 1
#else
#define ENV_BSD 0
#endif

/**
 * @brief 1 when compiling for any Apple platform.
 *
 * @c __APPLE__ covers macOS, iOS, tvOS, watchOS, and Mac Catalyst.
 * Use the finer-grained @c ENV_MACOS / @c ENV_IOS / etc. macros for
 * platform-specific code.
 */
#if defined(__APPLE__)
#define ENV_APPLE 1
#else
#define ENV_APPLE 0
#endif

#if ENV_APPLE
/** @brief 1 on Mac Catalyst (iOS app running on macOS). */
#if defined(TARGET_OS_MACCATALYST) && TARGET_OS_MACCATALYST
#define ENV_MACCATALYST 1
#else
#define ENV_MACCATALYST 0
#endif

#if defined(TARGET_OS_IPHONE) && TARGET_OS_IPHONE
#define ENV_IOS     1 /**< 1 on iOS. */
#define ENV_MACOS   0 /**< 0 (not macOS). */
#define ENV_TVOS    0 /**< 0 (not tvOS). */
#define ENV_WATCHOS 0 /**< 0 (not watchOS). */
#elif defined(TARGET_OS_TV) && TARGET_OS_TV
#define ENV_IOS     0 /**< 0 (not iOS). */
#define ENV_MACOS   0 /**< 0 (not macOS). */
#define ENV_TVOS    1 /**< 1 on tvOS. */
#define ENV_WATCHOS 0 /**< 0 (not watchOS). */
#elif defined(TARGET_OS_WATCH) && TARGET_OS_WATCH
#define ENV_IOS     0 /**< 0 (not iOS). */
#define ENV_MACOS   0 /**< 0 (not macOS). */
#define ENV_TVOS    0 /**< 0 (not tvOS). */
#define ENV_WATCHOS 1 /**< 1 on watchOS. */
#elif defined(TARGET_OS_OSX) && TARGET_OS_OSX
#define ENV_IOS     0 /**< 0 (not iOS). */
#define ENV_MACOS   1 /**< 1 on macOS. */
#define ENV_TVOS    0 /**< 0 (not tvOS). */
#define ENV_WATCHOS 0 /**< 0 (not watchOS). */
#else
/* Fallback for Apple targets where the platform is not fully clear. */
#define ENV_IOS     0
#define ENV_MACOS   1
#define ENV_TVOS    0
#define ENV_WATCHOS 0
#endif
#else
#define ENV_MACCATALYST 0 /**< 0 (not Mac Catalyst). */
#define ENV_IOS         0 /**< 0 (not iOS). */
#define ENV_MACOS       0 /**< 0 (not macOS). */
#define ENV_TVOS        0 /**< 0 (not tvOS). */
#define ENV_WATCHOS     0 /**< 0 (not watchOS). */
#endif

/**
 * @brief 1 on any Unix-like platform (Linux, macOS, iOS, tvOS, watchOS,
 *        Android, BSD, or any target that defines @c __unix__).
 */
#if ENV_LINUX || ENV_MACOS || ENV_IOS || ENV_TVOS || ENV_WATCHOS || ENV_ANDROID || ENV_BSD ||      \
    defined(__unix__) || defined(__unix)
#define ENV_UNIX_LIKE 1
#else
#define ENV_UNIX_LIKE 0
#endif

/** @} */

/* Arduino Platform Detection ------------------------------------------------------------*/

/** @defgroup ENV_Arduino Arduino Sub-Architecture Detection
 *  @{
 */

/** @brief 1 when compiling under the Arduino framework. */
#if defined(ARDUINO)
#define ENV_ARDUINO 1
#else
#define ENV_ARDUINO 0
#endif

/** @brief 1 on Arduino with an AVR processor (UNO, Nano, Mega, …). */
#if ENV_ARDUINO && (defined(__AVR__) || defined(ARDUINO_ARCH_AVR))
#define ENV_ARDUINO_AVR 1
#else
#define ENV_ARDUINO_AVR 0
#endif

/** @brief 1 on Arduino with an ESP32 processor. */
#if ENV_ARDUINO && (defined(ESP32) || defined(ARDUINO_ARCH_ESP32))
#define ENV_ARDUINO_ESP32 1
#else
#define ENV_ARDUINO_ESP32 0
#endif

/** @brief 1 on Arduino with an ESP8266 processor. */
#if ENV_ARDUINO && (defined(ESP8266) || defined(ARDUINO_ARCH_ESP8266))
#define ENV_ARDUINO_ESP8266 1
#else
#define ENV_ARDUINO_ESP8266 0
#endif

/** @brief 1 on Arduino with a SAMD processor (MKR, Zero, …). */
#if ENV_ARDUINO && defined(ARDUINO_ARCH_SAMD)
#define ENV_ARDUINO_SAMD 1
#else
#define ENV_ARDUINO_SAMD 0
#endif

/** @brief 1 on Arduino with an STM32 processor. */
#if ENV_ARDUINO && defined(ARDUINO_ARCH_STM32)
#define ENV_ARDUINO_STM32 1
#else
#define ENV_ARDUINO_STM32 0
#endif

/** @brief 1 on Arduino with an RP2040 processor (Raspberry Pi Pico). */
#if ENV_ARDUINO && (defined(ARDUINO_ARCH_RP2040) || defined(PICO_SDK_VERSION_MAJOR))
#define ENV_ARDUINO_RP2040 1
#else
#define ENV_ARDUINO_RP2040 0
#endif

/** @brief 1 on Arduino with an nRF52 processor. */
#if ENV_ARDUINO && (defined(ARDUINO_ARCH_NRF52) || defined(NRF52) || defined(NRF52840_XXAA))
#define ENV_ARDUINO_NRF52 1
#else
#define ENV_ARDUINO_NRF52 0
#endif

/** @brief 1 on Arduino with a RISC-V processor. */
#if ENV_ARDUINO && (defined(ARDUINO_ARCH_RISCV) || defined(__riscv))
#define ENV_ARDUINO_RISCV 1
#else
#define ENV_ARDUINO_RISCV 0
#endif

/** @brief 1 on Teensy (PJRC) boards. */
#if defined(TEENSYDUINO)
#define ENV_ARDUINO_TEENSY 1
#else
#define ENV_ARDUINO_TEENSY 0
#endif

/** @} */

/* Embedded Detection Hints -------------------------------------------------------------*/

/** @defgroup ENV_RTOS Embedded Detection Hints
 *  @brief Internal detection hints — use @c ENV_HAS_* for final results.
 *  @{
 */

/**
 * @brief 1 when a known RTOS API header is visible at compile time.
 *
 * Detects FreeRTOS, ThreadX, Zephyr, µC/OS, and CMSIS-RTOS by checking for
 * characteristic preprocessor symbols defined by their headers.
 */
#if defined(FREERTOS) || defined(FREERTOS_KERNEL) || defined(INCLUDE_xTaskGetSchedulerState)
#define ENV_RTOS_HINT 1
#elif defined(THREADX) || defined(TX_PORT) || defined(TX_SOURCE)
#define ENV_RTOS_HINT 1
#elif defined(ZEPHYR) || defined(CONFIG_ZEPHYR)
#define ENV_RTOS_HINT 1
#elif defined(uC_OS_III) || defined(OS_CFG_H) || defined(UCOSII) || defined(UCOS)
#define ENV_RTOS_HINT 1
#elif defined(CMSIS_RTOS) || defined(RTX_VERSION) || defined(RTX5)
#define ENV_RTOS_HINT 1
#else
#define ENV_RTOS_HINT 0
#endif

/**
 * @brief 1 when explicit MCU / embedded toolchain markers are present.
 *
 * Intentionally avoids the generic @c __ARM_ARCH to prevent false positives
 * on Cortex-A application processors running Linux.
 */
#if ENV_ARDUINO || defined(__AVR__) || defined(__CORTEX_M) || defined(__ARM_ARCH_7M__) ||          \
    defined(__ARM_ARCH_7EM__) || defined(__ARM_ARCH_8M_BASE__) || defined(__ARM_ARCH_8M_MAIN__) || \
    defined(__thumb__) || defined(__thumb2__) || defined(__riscv) || defined(ESP32) ||             \
    defined(ESP8266) || defined(PICO_SDK_VERSION_MAJOR) || defined(NRF52) ||                       \
    defined(NRF52840_XXAA) || defined(TEENSYDUINO)
#define ENV_EMBEDDED_HINT 1
#else
#define ENV_EMBEDDED_HINT 0
#endif

/** @} */

/* User Override Requests ---------------------------------------------------------------*/

/** @defgroup ENV_Force Build-time Override Macros
 *  @brief Define exactly one of these to bypass automatic detection.
 *  @{
 */

/** @brief Internal: 1 when @c ENV_FORCE_OS was set by the user. */
#if defined(ENV_FORCE_OS) && (ENV_FORCE_OS)
#define ENV_REQUEST_OS 1
#else
#define ENV_REQUEST_OS 0
#endif

/** @brief Internal: 1 when @c ENV_FORCE_BAREMETAL was set by the user. */
#if defined(ENV_FORCE_BAREMETAL) && (ENV_FORCE_BAREMETAL)
#define ENV_REQUEST_BAREMETAL 1
#else
#define ENV_REQUEST_BAREMETAL 0
#endif

/** @brief Internal: 1 when @c ENV_FORCE_UNKNOWN was set by the user. */
#if defined(ENV_FORCE_UNKNOWN) && (ENV_FORCE_UNKNOWN)
#define ENV_REQUEST_UNKNOWN 1
#else
#define ENV_REQUEST_UNKNOWN 0
#endif

#if ((ENV_REQUEST_OS + ENV_REQUEST_BAREMETAL + ENV_REQUEST_UNKNOWN) > 1)
#error "Define only one of ENV_FORCE_OS, ENV_FORCE_BAREMETAL, or ENV_FORCE_UNKNOWN"
#endif

/** @} */

/* Automatic Classification -------------------------------------------------------------*/

#if (ENV_REQUEST_OS || ENV_REQUEST_BAREMETAL || ENV_REQUEST_UNKNOWN)

#define ENV_DETECTED_OS        0
#define ENV_DETECTED_BAREMETAL 0
#define ENV_DETECTED_UNKNOWN   0

#else

#if ENV_WINDOWS || ENV_LINUX || ENV_MACOS || ENV_IOS || ENV_TVOS || ENV_WATCHOS ||                 \
    ENV_MACCATALYST || ENV_ANDROID || ENV_BSD
#define ENV_DETECTED_OS        1
#define ENV_DETECTED_BAREMETAL 0
#define ENV_DETECTED_UNKNOWN   0
#elif ENV_RTOS_HINT || ENV_EMBEDDED_HINT
#define ENV_DETECTED_OS        0
#define ENV_DETECTED_BAREMETAL 1
#define ENV_DETECTED_UNKNOWN   0
#else
#define ENV_DETECTED_OS        0
#define ENV_DETECTED_BAREMETAL 0
#define ENV_DETECTED_UNKNOWN   1
#endif

#endif

/* Final Public Classification Macros ---------------------------------------------------*/

/** @defgroup ENV_Result Public Environment Classification
 *  @brief Read these macros to determine the active environment class.
 *        Exactly one will be 1; the others will be 0.
 *  @{
 */

#if defined(ENV_HAS_OS) || defined(ENV_HAS_BAREMETAL) || defined(ENV_HAS_UNKNOWN)
#error "Use ENV_FORCE_OS, ENV_FORCE_BAREMETAL, or ENV_FORCE_UNKNOWN. ENV_HAS_* are output macros."
#endif

#if ENV_REQUEST_OS || ENV_DETECTED_OS
#define ENV_HAS_OS        1 /**< 1 when running on a full hosted OS. */
#define ENV_HAS_BAREMETAL 0 /**< 0 (not bare-metal). */
#define ENV_HAS_UNKNOWN   0 /**< 0 (not unknown). */
#elif ENV_REQUEST_BAREMETAL || ENV_DETECTED_BAREMETAL
#define ENV_HAS_OS        0 /**< 0 (not OS). */
#define ENV_HAS_BAREMETAL 1 /**< 1 when a non-hosted target is detected. */
#define ENV_HAS_UNKNOWN   0 /**< 0 (not unknown). */
#else
#define ENV_HAS_OS        0 /**< 0 (not OS). */
#define ENV_HAS_BAREMETAL 0 /**< 0 (not bare-metal). */
#define ENV_HAS_UNKNOWN   1 /**< 1 when no environment could be determined. */
#endif

/** @} */

/* Canonical Classification Values ------------------------------------------------------*/

/** @defgroup ENV_Kind Environment Kind Enum Values
 *  @{
 */

#define ENV_KIND_UNKNOWN   0 /**< Numeric value for the unknown environment kind. */
#define ENV_KIND_OS        1 /**< Numeric value for the OS environment kind. */
#define ENV_KIND_BAREMETAL 2 /**< Numeric value for the bare-metal environment kind. */

#if defined(ENV_HAS_OS) && ENV_HAS_OS
#define ENV_OS        1           /**< 1 when the environment is a full OS. */
#define ENV_NON_OS    0           /**< 0 (not non-OS). */
#define ENV_BAREMETAL 0           /**< 0 (not bare-metal). */
#define ENV_UNKNOWN   0           /**< 0 (not unknown). */
#define ENV_KIND      ENV_KIND_OS /**< Numeric kind: OS. */
#elif defined(ENV_HAS_BAREMETAL) && ENV_HAS_BAREMETAL
#define ENV_OS        0                  /**< 0 (not OS). */
#define ENV_NON_OS    1                  /**< 1 for non-hosted targets. */
#define ENV_BAREMETAL 1                  /**< 1 when the environment is bare-metal or RTOS-like. */
#define ENV_UNKNOWN   0                  /**< 0 (not unknown). */
#define ENV_KIND      ENV_KIND_BAREMETAL /**< Numeric kind: bare-metal. */
#elif defined(ENV_HAS_UNKNOWN) && ENV_HAS_UNKNOWN
#define ENV_OS        0                /**< 0 (not OS). */
#define ENV_NON_OS    0                /**< 0 (unknown: cannot classify). */
#define ENV_BAREMETAL 0                /**< 0 (not bare-metal). */
#define ENV_UNKNOWN   1                /**< 1 when no environment was detected. */
#define ENV_KIND      ENV_KIND_UNKNOWN /**< Numeric kind: unknown. */
#else
/* Safety net */
#define ENV_OS        0
#define ENV_NON_OS    0
#define ENV_BAREMETAL 0
#define ENV_UNKNOWN   1
#define ENV_KIND      ENV_KIND_UNKNOWN
#endif

/* Sanity check: exactly one environment class should be active. */
#if ((ENV_OS + ENV_BAREMETAL + ENV_UNKNOWN) != 1)
#error "Exactly one environment class must be active"
#endif

/** @} */

/*=======================================================================================
 * Public Macros
 *=======================================================================================*/

/* Friendly Boolean Aliases -------------------------------------------------------------*/

/** @defgroup ENV_Aliases Friendly Boolean Aliases
 *  @brief Readable aliases for platform and environment detection results.
 *  @{
 */

#define ENV_IS_OS_ENVIRONMENT     (ENV_OS)     /**< @c true when a full OS is detected. */
#define ENV_IS_NON_OS_ENVIRONMENT (ENV_NON_OS) /**< @c true for non-hosted targets. */
#define ENV_IS_BAREMETAL_ENVIRONMENT                                                               \
  (ENV_BAREMETAL) /**< @c true on bare-metal MCU targets.                                          \
                   */
#define ENV_IS_UNKNOWN_ENVIRONMENT                                                                 \
  (ENV_UNKNOWN) /**< @c true when the environment is unclassified. */

#define ENV_IS_WINDOWS     (ENV_WINDOWS)     /**< @c true on Windows. */
#define ENV_IS_LINUX       (ENV_LINUX)       /**< @c true on Linux. */
#define ENV_IS_MACOS       (ENV_MACOS)       /**< @c true on macOS. */
#define ENV_IS_IOS         (ENV_IOS)         /**< @c true on iOS. */
#define ENV_IS_TVOS        (ENV_TVOS)        /**< @c true on tvOS. */
#define ENV_IS_WATCHOS     (ENV_WATCHOS)     /**< @c true on watchOS. */
#define ENV_IS_MACCATALYST (ENV_MACCATALYST) /**< @c true on Mac Catalyst. */
#define ENV_IS_ANDROID     (ENV_ANDROID)     /**< @c true on Android. */
#define ENV_IS_BSD         (ENV_BSD)         /**< @c true on BSD variants. */
#define ENV_IS_APPLE       (ENV_APPLE)       /**< @c true on any Apple platform. */
#define ENV_IS_UNIX_LIKE   (ENV_UNIX_LIKE)   /**< @c true on any Unix-like OS. */
#define ENV_IS_ARDUINO     (ENV_ARDUINO)     /**< @c true under the Arduino framework. */

#define ENV_IS_ARDUINO_AVR     (ENV_ARDUINO_AVR)     /**< @c true on Arduino/AVR. */
#define ENV_IS_ARDUINO_ESP32   (ENV_ARDUINO_ESP32)   /**< @c true on Arduino/ESP32. */
#define ENV_IS_ARDUINO_ESP8266 (ENV_ARDUINO_ESP8266) /**< @c true on Arduino/ESP8266. */
#define ENV_IS_ARDUINO_SAMD                                                                        \
  (ENV_ARDUINO_SAMD)                               /**< @c true on Arduino/SAMD.                   \
                                                    */
#define ENV_IS_ARDUINO_STM32  (ENV_ARDUINO_STM32)  /**< @c true on Arduino/STM32. */
#define ENV_IS_ARDUINO_RP2040 (ENV_ARDUINO_RP2040) /**< @c true on Arduino/RP2040. */
#define ENV_IS_ARDUINO_NRF52  (ENV_ARDUINO_NRF52)  /**< @c true on Arduino/nRF52. */
#define ENV_IS_ARDUINO_RISCV  (ENV_ARDUINO_RISCV)  /**< @c true on Arduino/RISC-V. */
#define ENV_IS_ARDUINO_TEENSY (ENV_ARDUINO_TEENSY) /**< @c true on Teensy boards. */

/** @} */

/*=======================================================================================
 * Optional String Labels
 *=======================================================================================*/

/** @defgroup ENV_Strings Environment String Labels
 *  @brief Human-readable string constants for the detected platform and kind.
 *  @{
 */

#if ENV_IS_WINDOWS
#define ENV_OS_NAME "WINDOWS" /**< OS name string: @c "WINDOWS". */
#elif ENV_IS_LINUX
#define ENV_OS_NAME "LINUX" /**< OS name string: @c "LINUX". */
#elif ENV_IS_MACCATALYST
#define ENV_OS_NAME "MACCATALYST" /**< OS name string: @c "MACCATALYST". */
#elif ENV_IS_MACOS
#define ENV_OS_NAME "MACOS" /**< OS name string: @c "MACOS". */
#elif ENV_IS_IOS
#define ENV_OS_NAME "IOS" /**< OS name string: @c "IOS". */
#elif ENV_IS_TVOS
#define ENV_OS_NAME "TVOS" /**< OS name string: @c "TVOS". */
#elif ENV_IS_WATCHOS
#define ENV_OS_NAME "WATCHOS" /**< OS name string: @c "WATCHOS". */
#elif ENV_IS_ANDROID
#define ENV_OS_NAME "ANDROID" /**< OS name string: @c "ANDROID". */
#elif ENV_IS_BSD
#define ENV_OS_NAME "BSD" /**< OS name string: @c "BSD". */
#elif ENV_IS_ARDUINO
#define ENV_OS_NAME "ARDUINO" /**< OS name string: @c "ARDUINO". */
#else
#define ENV_OS_NAME "UNKNOWN_OS" /**< OS name string: @c "UNKNOWN_OS". */
#endif

#if ENV_IS_OS_ENVIRONMENT
#define ENV_NAME "OS" /**< Environment name string: @c "OS". */
#elif ENV_IS_BAREMETAL_ENVIRONMENT
#define ENV_NAME "BAREMETAL" /**< Environment name string: @c "BAREMETAL". */
#else
#define ENV_NAME "UNKNOWN" /**< Environment name string: @c "UNKNOWN". */
#endif

#if ENV_KIND == ENV_KIND_OS
#define ENV_KIND_NAME "OS" /**< Kind name string: @c "OS". */
#elif ENV_KIND == ENV_KIND_BAREMETAL
#define ENV_KIND_NAME "BAREMETAL" /**< Kind name string: @c "BAREMETAL". */
#else
#define ENV_KIND_NAME "UNKNOWN" /**< Kind name string: @c "UNKNOWN". */
#endif

/** @} */

/*=======================================================================================
 * Public Types
 *=======================================================================================*/

/** @defgroup ENV_Types Environment Types
 *  @{
 */

/** @} */

/*=======================================================================================
 * External Data Variables
 *=======================================================================================*/

/** @defgroup ENV_ExternVars Environment External Variables
 *  @{
 */

/* No public external data variables. */

/** @} */

/*=======================================================================================
 * Public Function Prototypes
 *=======================================================================================*/

/** @defgroup ENV_Functions Environment Public Functions
 *  @{
 */

/* No public functions declared in this header. */

/** @} */

#ifdef __cplusplus
}
#endif

#endif /* CLI_ENV_DETECT_H */

/*################################### END OF FILE ######################################*/
