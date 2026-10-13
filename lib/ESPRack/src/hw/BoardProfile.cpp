#include "hw/BoardProfile.h"

#include <Arduino.h>  // psramFound()
#include <esp_bit_defs.h>
#include <sdkconfig.h>
#include <soc/soc_caps.h>
#include <soc/spi_pins.h>
#include <soc/uart_pins.h>
#if SOC_ADC_PERIPH_NUM > 1
#include <soc/adc_channel.h>
#endif
#if CONFIG_IDF_TARGET_ESP32S3
#include <soc/usb_pins.h>
#endif

#include "hw/BoardTables.h"

namespace ESPRack {

using namespace hwtab;

// ---- The tables against the SoC headers: a wrong number fails the build.
#if CONFIG_IDF_TARGET_ESP32
static_assert(hasPin(kEsp32Flash, MSPI_IOMUX_PIN_NUM_CLK) && hasPin(kEsp32Flash, MSPI_IOMUX_PIN_NUM_MISO) &&
                  hasPin(kEsp32Flash, MSPI_IOMUX_PIN_NUM_MOSI) && hasPin(kEsp32Flash, MSPI_IOMUX_PIN_NUM_HD) &&
                  hasPin(kEsp32Flash, MSPI_IOMUX_PIN_NUM_WP) && hasPin(kEsp32Flash, MSPI_IOMUX_PIN_NUM_CS0),
              "kEsp32Flash != soc/spi_pins.h");
static_assert(kEsp32Uart0[0] == U0TXD_GPIO_NUM && kEsp32Uart0[1] == U0RXD_GPIO_NUM, "kEsp32Uart0");
static_assert(kEsp32ValidMask == SOC_GPIO_VALID_GPIO_MASK, "kEsp32ValidMask");
static_assert(kEsp32OutputMask == SOC_GPIO_VALID_OUTPUT_GPIO_MASK, "kEsp32OutputMask");
static_assert(kEsp32Adc2[0] == ADC2_CHANNEL_0_GPIO_NUM && kEsp32Adc2[1] == ADC2_CHANNEL_1_GPIO_NUM &&
                  kEsp32Adc2[2] == ADC2_CHANNEL_2_GPIO_NUM && kEsp32Adc2[3] == ADC2_CHANNEL_3_GPIO_NUM &&
                  kEsp32Adc2[4] == ADC2_CHANNEL_4_GPIO_NUM && kEsp32Adc2[5] == ADC2_CHANNEL_5_GPIO_NUM &&
                  kEsp32Adc2[6] == ADC2_CHANNEL_6_GPIO_NUM && kEsp32Adc2[7] == ADC2_CHANNEL_7_GPIO_NUM &&
                  kEsp32Adc2[8] == ADC2_CHANNEL_8_GPIO_NUM && kEsp32Adc2[9] == ADC2_CHANNEL_9_GPIO_NUM,
              "kEsp32Adc2");
static_assert(kEsp32Budget.ledc == SOC_LEDC_CHANNEL_NUM * 2 && SOC_LEDC_SUPPORT_HS_MODE, "ESP32 LEDC: HS + LS");
#define ESPRACK_HW_BUDGET kEsp32Budget
#define ESPRACK_HW_CHIP HwChip::Esp32
#define ESPRACK_HW_LEDC_CHECKED 1

#elif CONFIG_IDF_TARGET_ESP32S3
static_assert(hasPin(kS3Flash, MSPI_IOMUX_PIN_NUM_CLK) && hasPin(kS3Flash, MSPI_IOMUX_PIN_NUM_MISO) &&
                  hasPin(kS3Flash, MSPI_IOMUX_PIN_NUM_MOSI) && hasPin(kS3Flash, MSPI_IOMUX_PIN_NUM_HD) &&
                  hasPin(kS3Flash, MSPI_IOMUX_PIN_NUM_WP) && hasPin(kS3Flash, MSPI_IOMUX_PIN_NUM_CS0),
              "kS3Flash != soc/spi_pins.h");
static_assert(kS3PsramCs == MSPI_IOMUX_PIN_NUM_CS1, "kS3PsramCs");
static_assert(hasPin(kS3Octal, MSPI_IOMUX_PIN_NUM_D4) && hasPin(kS3Octal, MSPI_IOMUX_PIN_NUM_D5) &&
                  hasPin(kS3Octal, MSPI_IOMUX_PIN_NUM_D6) && hasPin(kS3Octal, MSPI_IOMUX_PIN_NUM_D7) &&
                  hasPin(kS3Octal, MSPI_IOMUX_PIN_NUM_DQS),
              "kS3Octal != soc/spi_pins.h");
static_assert(kS3Uart0[0] == U0TXD_GPIO_NUM && kS3Uart0[1] == U0RXD_GPIO_NUM, "kS3Uart0");
static_assert(kS3Usb[0] == USBPHY_DM_NUM && kS3Usb[1] == USBPHY_DP_NUM, "kS3Usb");
static_assert(kS3ValidMask == SOC_GPIO_VALID_GPIO_MASK && kS3ValidMask == SOC_GPIO_VALID_OUTPUT_GPIO_MASK,
              "kS3ValidMask");
static_assert(kS3Adc2[0] == ADC2_CHANNEL_0_GPIO_NUM && kS3Adc2[1] == ADC2_CHANNEL_1_GPIO_NUM &&
                  kS3Adc2[2] == ADC2_CHANNEL_2_GPIO_NUM && kS3Adc2[3] == ADC2_CHANNEL_3_GPIO_NUM &&
                  kS3Adc2[4] == ADC2_CHANNEL_4_GPIO_NUM && kS3Adc2[5] == ADC2_CHANNEL_5_GPIO_NUM &&
                  kS3Adc2[6] == ADC2_CHANNEL_6_GPIO_NUM && kS3Adc2[7] == ADC2_CHANNEL_7_GPIO_NUM &&
                  kS3Adc2[8] == ADC2_CHANNEL_8_GPIO_NUM && kS3Adc2[9] == ADC2_CHANNEL_9_GPIO_NUM,
              "kS3Adc2");
#define ESPRACK_HW_BUDGET kS3Budget
#define ESPRACK_HW_CHIP HwChip::Esp32S3

#elif CONFIG_IDF_TARGET_ESP32C3
static_assert(hasPin(kC3Flash, MSPI_IOMUX_PIN_NUM_CLK) && hasPin(kC3Flash, MSPI_IOMUX_PIN_NUM_MISO) &&
                  hasPin(kC3Flash, MSPI_IOMUX_PIN_NUM_MOSI) && hasPin(kC3Flash, MSPI_IOMUX_PIN_NUM_HD) &&
                  hasPin(kC3Flash, MSPI_IOMUX_PIN_NUM_WP) && hasPin(kC3Flash, MSPI_IOMUX_PIN_NUM_CS0),
              "kC3Flash != soc/spi_pins.h");
static_assert(kC3Uart0[0] == U0TXD_GPIO_NUM && kC3Uart0[1] == U0RXD_GPIO_NUM, "kC3Uart0");
static_assert(kC3ValidMask == SOC_GPIO_VALID_GPIO_MASK && kC3ValidMask == SOC_GPIO_VALID_OUTPUT_GPIO_MASK,
              "kC3ValidMask");
static_assert(kC3Adc2[0] == ADC2_CHANNEL_0_GPIO_NUM, "kC3Adc2");
#define ESPRACK_HW_BUDGET kC3Budget
#define ESPRACK_HW_CHIP HwChip::Esp32C3

#elif CONFIG_IDF_TARGET_ESP32C6
static_assert(hasPin(kC6Flash, MSPI_IOMUX_PIN_NUM_CLK) && hasPin(kC6Flash, MSPI_IOMUX_PIN_NUM_MISO) &&
                  hasPin(kC6Flash, MSPI_IOMUX_PIN_NUM_MOSI) && hasPin(kC6Flash, MSPI_IOMUX_PIN_NUM_HD) &&
                  hasPin(kC6Flash, MSPI_IOMUX_PIN_NUM_WP) && hasPin(kC6Flash, MSPI_IOMUX_PIN_NUM_CS0),
              "kC6Flash != soc/spi_pins.h");
static_assert(kC6Uart0[0] == U0TXD_GPIO_NUM && kC6Uart0[1] == U0RXD_GPIO_NUM, "kC6Uart0");
static_assert(kC6ValidMask == SOC_GPIO_VALID_GPIO_MASK && kC6ValidMask == SOC_GPIO_VALID_OUTPUT_GPIO_MASK,
              "kC6ValidMask");
#define ESPRACK_HW_BUDGET kC6Budget
#define ESPRACK_HW_CHIP HwChip::Esp32C6
#endif

#ifdef ESPRACK_HW_BUDGET
static_assert(ESPRACK_HW_BUDGET.uarts == SOC_UART_HP_NUM, "UART budget");
static_assert(ESPRACK_HW_BUDGET.i2cBuses == SOC_HP_I2C_NUM, "I2C budget");
static_assert(ESPRACK_HW_BUDGET.spiBuses == SOC_SPI_PERIPH_NUM - 1, "SPI budget (SPI0/1 = flash)");
static_assert(ESPRACK_HW_BUDGET.rmtTx == SOC_RMT_TX_CANDIDATES_PER_GROUP &&
                  ESPRACK_HW_BUDGET.rmtRx == SOC_RMT_RX_CANDIDATES_PER_GROUP &&
                  ESPRACK_HW_BUDGET.rmtTotal == SOC_RMT_CHANNELS_PER_GROUP,
              "RMT budget");
#ifndef ESPRACK_HW_LEDC_CHECKED
static_assert(ESPRACK_HW_BUDGET.ledc == SOC_LEDC_CHANNEL_NUM, "LEDC budget");
#endif
#if SOC_PCNT_SUPPORTED
static_assert(ESPRACK_HW_BUDGET.pcnt == SOC_PCNT_UNITS_PER_GROUP, "PCNT budget");
#else
static_assert(ESPRACK_HW_BUDGET.pcnt == 0, "PCNT budget");
#endif
#endif

HwChip currentChip() {
#ifdef ESPRACK_HW_CHIP
  return ESPRACK_HW_CHIP;
#else
  return HwChip::Unknown;  // no pin table: only conflicts between instances are checked
#endif
}

BoardFlags currentBoardFlags() {
  BoardFlags f;
  f.psram = psramFound();
#if CONFIG_IDF_TARGET_ESP32S3
  bool octal = false;
#if CONFIG_SPIRAM_MODE_OCT
  octal = f.psram;
#endif
#if CONFIG_ESPTOOLPY_OCT_FLASH
  octal = true;
#endif
  f.octalPsramOrFlash = octal;
#endif

#ifdef ESPRACK_HW_USB_IN_USE
  f.usbInUse = ESPRACK_HW_USB_IN_USE;
#elif SOC_USB_SERIAL_JTAG_SUPPORTED
  // The prebuilt Arduino libs run a secondary console on USB-Serial-JTAG;
  // Serial on USB CDC uses the same pins.
#if CONFIG_ESP_CONSOLE_USB_SERIAL_JTAG || CONFIG_ESP_CONSOLE_SECONDARY_USB_SERIAL_JTAG || ARDUINO_USB_CDC_ON_BOOT
  f.usbInUse = true;
#endif
#endif

#ifdef ESPRACK_HW_UART0_CONSOLE
  f.uart0Console = ESPRACK_HW_UART0_CONSOLE;
#elif ARDUINO_USB_CDC_ON_BOOT
  // Serial is USB CDC, but the IDF console may still print on UART0 (it does
  // on the prebuilt libs).
  f.uart0Console = currentConsoleUart().num == 0;
#else
  f.uart0Console = true;  // Serial is UART0
#endif
  return f;
}

ConsoleUart currentConsoleUart() {
  ConsoleUart c;
#if defined(CONFIG_ESP_CONSOLE_NONE) && CONFIG_ESP_CONSOLE_NONE
  // no console at all
#elif defined(CONFIG_ESP_CONSOLE_UART_NUM)
  c.num = CONFIG_ESP_CONSOLE_UART_NUM;  // -1 when the console is on USB
#if defined(CONFIG_ESP_CONSOLE_UART_CUSTOM) && CONFIG_ESP_CONSOLE_UART_CUSTOM
  c.tx = CONFIG_ESP_CONSOLE_UART_TX_GPIO;
  c.rx = CONFIG_ESP_CONSOLE_UART_RX_GPIO;
#endif
#endif
  return c;
}

BoardInfo currentBoardInfo() { return boardInfoFor(currentChip(), currentBoardFlags()); }

}  // namespace ESPRack
