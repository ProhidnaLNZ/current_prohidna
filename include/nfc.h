#pragma once

#include <Arduino.h>
#include <SPI.h>
#include <Adafruit_PN532.h>
#include "waveshare_pinout.h"
#include "db.h"
#include "led.h"
#include "buzzer.h"
#include "uuid.h"
#include "whitelist.h"

// --- Configuration Constants ---
static constexpr uint32_t CARD_READ_COOLDOWN_MS = 2000;
static constexpr uint32_t HEALTH_CHECK_INTERVAL_MS = 2500;

class Nfc {
private:
    Adafruit_PN532 nfc_chip;

    // --- State & Timers ---
    bool is_connected_ = false;
    unsigned long last_health_check_timestamp_ = 0;
    String last_processed_uid_ = "";
    unsigned long last_processed_read_timestamp_ = 0;

    // --- Pointers to other modules ---
    DataBase* db_ptr = nullptr;
    Led* led = nullptr;
    Buzzer* buzzer = nullptr;
    xQueueHandle queue_handle_ = NULL;
    Whitelist* whitelist = nullptr;

    String uidArrayToString(uint8_t *uid, uint8_t uidLength) {
        String result = "";
        for (uint8_t i = 0; i < uidLength; i++) {
            // Use String(value, HEX) for consistent formatting if needed, but decimal is fine.
            result += String(uid[i]);
            if (i < uidLength - 1) result += "-";
        }
        result.trim(); // ADD THIS
        return result;
    }

    void tryConnect() {
        Serial.println("NFC: Attempting to connect/reconnect to PN532...");
        if (led) led->animation = loading;

        nfc_chip.begin();
        uint32_t versiondata = nfc_chip.getFirmwareVersion();

        if (versiondata) {
            nfc_chip.SAMConfig();
            is_connected_ = true;
            Serial.println("NFC: Connection successful!");
            if (led) led->animation = waiting;
        } else {
            is_connected_ = false;
            Serial.println("NFC: Connection failed.");
        }
        last_health_check_timestamp_ = millis();
    }
    
    void healthCheck() {
        if (millis() - last_health_check_timestamp_ > HEALTH_CHECK_INTERVAL_MS) {
            if (nfc_chip.getFirmwareVersion() == 0) {
                Serial.println("NFC: Health check failed. Module is disconnected.");
                is_connected_ = false;
                if (led) led->animation = loading;
            }
            last_health_check_timestamp_ = millis();
        }
    }

public:
    Nfc() : nfc_chip(NFC_SCLK, NFC_MISO, NFC_MOSI, NFC_CS) {}

    void setup(DataBase& dab, Led& led_instance, Buzzer& buzzer_instance, xQueueHandle queue, Whitelist& wl) {
        db_ptr = &dab;
        led = &led_instance;
        this->buzzer = &buzzer_instance;
        this->queue_handle_ = queue;
        this->whitelist = &wl;
        tryConnect();
    }

    void read() {
        if (!is_connected_) {
            tryConnect();
            return;
        }

        healthCheck();

        uint8_t uid[7] = {0};
        uint8_t uidLength;
        bool success = nfc_chip.readPassiveTargetID(PN532_MIFARE_ISO14443A, uid, &uidLength, 50);

        if (success) {

            String current_uid_str = uidArrayToString(uid, uidLength);
            
            bool isNewCard = (current_uid_str != last_processed_uid_);
            bool cooldownPassed = (millis() - last_processed_read_timestamp_ > CARD_READ_COOLDOWN_MS);
            if (isNewCard || cooldownPassed) {
            bool allowed = false;
            if (whitelist && whitelist->isReady()) {
                allowed = whitelist->isAllowed(current_uid_str);
            } else {
                Serial.println("NFC: Whitelist not ready yet, denying card.");
            }

                if (allowed) {
                    if (led) led->approval(700);
                    if (buzzer) buzzer->playApproval();
                } else {
                    Serial.printf("NFC: Card not in whitelist: %s\n", current_uid_str.c_str());
                    if (led) led->denial(350);
                    if (buzzer) buzzer->playDenial();
                }
                Serial.printf("\nNFC: Processing new read. UID: %s\n", current_uid_str.c_str());
                last_processed_uid_ = current_uid_str;
                last_processed_read_timestamp_ = millis();
                
                // --- LEGACY CARD PROCESSING START ---
                uint64_t custom_code = 0;
                bool custom_code_read_success = false;

                if (uidLength == 4) {
                    Serial.println("NFC: MIFARE Classic card detected. Trying to read block 4...");
                    uint8_t keya[6] = { 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF };
                    
                    if (nfc_chip.mifareclassic_AuthenticateBlock(uid, uidLength, 4, 0, keya)) {
                        uint8_t block_data[16];
                        if (nfc_chip.mifareclassic_ReadDataBlock(4, block_data)) {
                            // Reconstruct the 64-bit code from the first 8 bytes
                            for (uint8_t i = 0; i < 8; i++) {
                                custom_code = (custom_code << 8) | block_data[i];
                            }
                            custom_code_read_success = true;
                            // Note: Arduino's String class correctly handles uint64_t
                            Serial.println("NFC: Successfully read custom code: " + String(custom_code));
                        } else {
                            Serial.println("NFC: Failed to read data from block 4 after auth.");
                        }
                    } else {
                        Serial.println("NFC: Failed to authenticate block 4.");
                    }
                }
                // --- LEGACY CARD PROCESSING END ---
                
                static bool ntpSynced = false;
                static unsigned long ntpSyncMillis = 0;
                static time_t ntpSyncTime = 0;

                if (!ntpSynced) {
                    time_t now;
                    time(&now);
                    if (now > 1700000000UL) {               // якщо час адекватний (після 2023 року)
                        ntpSynced = true;
                        ntpSyncMillis = millis();
                        ntpSyncTime = now;
                    }
                }

                unsigned long readMillis = millis();

                String uuid = generateUUID();
                // Updated data format to include the custom code
                String data_to_log = current_uid_str + ":" + 
                     (custom_code_read_success ? String(custom_code) : "0") + ":" +
                     String(readMillis) + ":" + 
                     uuid + ":" +
                     (allowed ? "1" : "0"); 
           
                if (queue_handle_ != NULL) {
                    char data_buffer[128];
                    strncpy(data_buffer, data_to_log.c_str(), sizeof(data_buffer));
                    data_buffer[sizeof(data_buffer) - 1] = '\0';
                    if (xQueueSend(queue_handle_, data_buffer, pdMS_TO_TICKS(100)) != pdPASS) {
                        Serial.println("NFC: Failed to send read to queue (it might be full).");
                    }
                }
            }
        }
    }
};