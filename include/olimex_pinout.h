// olimex_pinout.h

// --- NFC (SPI) ---
#define NFC_SCLK  16
#define NFC_MOSI  14
#define NFC_MISO  15
#define NFC_CS    13

// --- LED & Buzzer ---
#define LED       2
#define LED_NUM   24
#define BUZZER    4

// --- Ethernet (LAN8720, RMII) ---
#define ETH_MDC_PIN      23
#define ETH_MDIO_PIN     18
#define ETH_POWER_PIN    12
#define ETH_PHY_ADDR     1
#define ETH_CLK_MODE  ETH_CLOCK_GPIO0_IN
// ETH_TYPE: LAN8720