#pragma once
#ifndef ESPRACK_HW_BOARD_TABLES_H
#define ESPRACK_HW_BOARD_TABLES_H

// Per-chip pin and peripheral facts (LIB-FEAT-001). Values from the
// pioarduino 54.03.21-2 IDF headers (soc/<chip>/include/soc/{spi_pins,
// uart_pins,usb_pins,adc_channel,soc_caps}.h) and the datasheets for what
// the headers don't define (C3/C6 USB pins, strapping pins). On the target,
// hw/BoardProfile.cpp static_asserts these against the SoC headers, so a
// wrong number fails the build instead of reserving the wrong pin.

#include <stddef.h>
#include <stdint.h>

namespace ESPRack {
namespace hwtab {

// ESP32 (classic)
constexpr int8_t kEsp32Flash[] = {6, 7, 8, 9, 10, 11};  // CLK, Q, D, HD, WP, CS0
constexpr int8_t kEsp32Psram[] = {16, 17};               // PSRAM CS, CLK (WROVER)
constexpr int8_t kEsp32Uart0[] = {1, 3};                 // TX, RX
constexpr int8_t kEsp32Adc2[] = {4, 0, 2, 15, 13, 12, 14, 27, 25, 26};  // ADC2 ch 0..9
constexpr uint64_t kEsp32ValidMask =
    0xFFFFFFFFFFULL & ~((1ULL << 24) | (1ULL << 28) | (1ULL << 29) | (1ULL << 30) | (1ULL << 31));
constexpr uint64_t kEsp32OutputMask =
    kEsp32ValidMask & ~((1ULL << 34) | (1ULL << 35) | (1ULL << 36) | (1ULL << 37) | (1ULL << 38) | (1ULL << 39));

// ESP32-S3
constexpr int8_t kS3Flash[] = {27, 28, 29, 30, 31, 32};  // HD, WP, CS0, CLK, Q, D
constexpr int8_t kS3PsramCs = 26;                        // MSPI CS1
constexpr int8_t kS3Octal[] = {33, 34, 35, 36, 37};      // D4..D7, DQS
constexpr int8_t kS3Uart0[] = {43, 44};                  // TX, RX
constexpr int8_t kS3Usb[] = {19, 20};                    // D-, D+
constexpr int8_t kS3Adc2[] = {11, 12, 13, 14, 15, 16, 17, 18, 19, 20};
constexpr uint64_t kS3ValidMask =
    0x1FFFFFFFFFFFFULL & ~((1ULL << 22) | (1ULL << 23) | (1ULL << 24) | (1ULL << 25));

// ESP32-C3
constexpr int8_t kC3Flash[] = {12, 13, 14, 15, 16, 17};  // HD, WP, CS0, CLK, D, Q
constexpr int8_t kC3Uart0[] = {21, 20};                  // TX, RX
constexpr int8_t kC3Usb[] = {18, 19};                    // D-, D+ (datasheet)
constexpr int8_t kC3Adc2[] = {5};
constexpr uint64_t kC3ValidMask = (1ULL << 22) - 1;

// ESP32-C6
constexpr int8_t kC6Flash[] = {24, 25, 26, 27, 28, 29, 30};  // in-package SPI flash (27: VDD_SPI)
constexpr int8_t kC6Uart0[] = {16, 17};                      // TX, RX
constexpr int8_t kC6Usb[] = {12, 13};                        // D-, D+ (datasheet)
constexpr uint64_t kC6ValidMask = (1ULL << 31) - 1;

// Peripheral budgets: UARTs, I2C buses and SPI hosts usable by modules
// (HP only; SPI0/1 are the flash), RMT TX/RX and the shared pool, LEDC
// channels (classic ESP32: 8 high-speed + 8 low-speed), PCNT units.
struct ChipBudget {
  uint8_t uarts, i2cBuses, spiBuses, rmtTx, rmtRx, rmtTotal, ledc, pcnt;
};
constexpr ChipBudget kEsp32Budget{3, 2, 2, 8, 8, 8, 16, 8};
constexpr ChipBudget kS3Budget{3, 2, 2, 4, 4, 8, 8, 4};
constexpr ChipBudget kC3Budget{2, 1, 1, 2, 2, 4, 6, 0};
constexpr ChipBudget kC6Budget{2, 1, 1, 2, 2, 4, 6, 4};

template <size_t N>
constexpr bool hasPin(const int8_t (&pins)[N], int pin) {
  for (size_t i = 0; i < N; ++i) {
    if (pins[i] == pin) return true;
  }
  return false;
}

template <size_t N>
constexpr uint64_t maskOf(const int8_t (&pins)[N]) {
  uint64_t m = 0;
  for (size_t i = 0; i < N; ++i) m |= 1ULL << pins[i];
  return m;
}

}  // namespace hwtab
}  // namespace ESPRack

#endif  // ESPRACK_HW_BOARD_TABLES_H
