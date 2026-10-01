// waveshare_pinout.h

// ЗМІНЕНО: Піни для NFC переїхали на другий SPI бас (HSPI)
#define NFC_SCLK 3
#define NFC_MOSI 1
#define NFC_MISO 2
#define NFC_CS   0

// --- LED & Buzzer Pins ---
#define LED 21
#define LED_NUM 24
#define BUZZER 16

// --- Ethernet Module Pins ---
#define ETH_SPI_MISO_PIN 12
#define ETH_SPI_MOSI_PIN 11
#define ETH_SPI_SCLK_PIN 13
#define ETH_SPI_CS_PIN   14
#define ETH_RST_PIN 9
#define ETH_INT_PIN 10