#pragma once

#include "freertos/semphr.h"
#include <Arduino.h>
#include <WiFi.h>
#include <SPI.h>
#include <EthernetUdp.h>
#include "time.h"
#include "waveshare_pinout.h"
#include "secrets.h"

extern SemaphoreHandle_t networkClientMutex;

enum class NetworkType { NONE, ETH, WIFI };

class NetworkManager {
private:
    enum class State {
        INIT,
        ETH_CONNECTING,
        WIFI_CONNECTING,
        CONNECTED_ETH,
        CONNECTED_WIFI,
        TRANSITION_DELAY 
    };

    // --- Configuration ---
    static constexpr uint32_t ETH_CONNECT_TIMEOUT_MS = 10000;
    static constexpr uint32_t WIFI_CONNECT_TIMEOUT_MS = 15000;
    static constexpr uint32_t TRANSITION_DELAY_MS = 2000;
    // --- Network Clients ---
    WiFiClient _wifiClient;
    EthernetClient _ethClient;
    byte _mac[6] = {0x0A, 0x8A, 0xDA, 0x11, 0x42, 0x01};

    // --- State Machine ---
    State _currentState = State::INIT;
    State _nextStateAfterDelay = State::INIT;
    NetworkType _activeConnection = NetworkType::NONE;
    uint32_t _stateTimer = 0;

    // --- NTP Configuration ---
    static constexpr const char* NTP_SERVER = "pool.ntp.org";
    static constexpr long GMT_OFFSET_SEC = 3600 * 2;
    static constexpr int DAYLIGHT_OFFSET_SEC = 0;
    EthernetUDP _ethUdp;

    unsigned long getNtpTime() {
            byte packetBuffer[48];
            memset(packetBuffer, 0, 48);
            packetBuffer[0] = 0b11100011;   // LI, Version, Mode
            packetBuffer[1] = 0;     // Stratum, or type of clock
            packetBuffer[2] = 6;     // Polling Interval
            packetBuffer[3] = 0xEC;  // Peer Clock Precision
            packetBuffer[12] = 49;
            packetBuffer[13] = 0x4E;
            packetBuffer[14] = 49;
            packetBuffer[15] = 52;
            _ethUdp.beginPacket(NTP_SERVER, 123);
            _ethUdp.write(packetBuffer, 48);
            _ethUdp.endPacket();
            uint32_t beginWait = millis();
            while (millis() - beginWait < 5000) {
                vTaskDelay(10 / portTICK_PERIOD_MS);
                int size = _ethUdp.parsePacket();
                if (size >= 48) {
                    _ethUdp.read(packetBuffer, 48);
                    unsigned long secsSince1900 = (unsigned long)packetBuffer[40] << 24;
                    secsSince1900 |= (unsigned long)packetBuffer[41] << 16;
                    secsSince1900 |= (unsigned long)packetBuffer[42] << 8;
                    secsSince1900 |= (unsigned long)packetBuffer[43];
                    return secsSince1900 - 2208988800UL;  // Convert to Unix time
                }
            }
            return 0;
        }
    void syncNTP() {
        // CHANGED: Use manual UDP for Ethernet, original configTime for WiFi
        if (_activeConnection == NetworkType::WIFI) {
            configTime(GMT_OFFSET_SEC, DAYLIGHT_OFFSET_SEC, NTP_SERVER);
            struct tm timeinfo;
            if (getLocalTime(&timeinfo, 5000)) {
                Serial.println("NTP: Time synchronized.");
            } else {
                Serial.println("NTP: Failed to obtain time.");
            }
        } else { 
            _ethUdp.begin(8888);
            time_t unixTime = getNtpTime();
            if (unixTime) {
                struct timeval tv = { .tv_sec = unixTime, .tv_usec = 0 };
                settimeofday(&tv, NULL);
                char tz_str[10];
                sprintf(tz_str, "STD%ld", -(GMT_OFFSET_SEC / 3600));
                setenv("TZ", tz_str, 1);
                tzset();
                Serial.println("NTP: Time synchronized.");
            } else {
                Serial.println("NTP: Failed to obtain time.");
            }
        }
    }

    void enterState(State newState) {
        _currentState = newState;
        _stateTimer = 0;
    }

public:
    NetworkManager() = default;

    void handleConnection() {
        if (xSemaphoreTake(networkClientMutex, pdMS_TO_TICKS(10)) == pdTRUE) {
            switch (_currentState) {
                case State::INIT:
                    Serial.println("NetMan: Initializing... Attempting Ethernet.");
                    enterState(State::ETH_CONNECTING);
                    // No break, fall through to start immediately
                
                case State::ETH_CONNECTING:
                    if (_stateTimer == 0) {
                        Serial.println("NetMan: Starting Ethernet connection...");
                        Ethernet.init(ETH_SPI_CS_PIN);
                        SPI.begin(ETH_SPI_SCLK_PIN, ETH_SPI_MISO_PIN, ETH_SPI_MOSI_PIN);
                        _stateTimer = millis();
                        if (Ethernet.begin(_mac, ETH_CONNECT_TIMEOUT_MS) == 0) {
                            Serial.println("NetMan: DHCP failed during begin().");
                        }
                    }
                    if (Ethernet.linkStatus() == LinkON && Ethernet.localIP() != IPAddress(0,0,0,0)) {
                        Serial.print("NetMan: Ethernet Connected. IP: ");
                        Serial.println(Ethernet.localIP());
                        _activeConnection = NetworkType::ETH;
                        syncNTP();
                        enterState(State::CONNECTED_ETH);
                    } else if (millis() - _stateTimer > ETH_CONNECT_TIMEOUT_MS) {
                        Serial.println("NetMan: Ethernet failed. Waiting to try Wi-Fi.");
                        _nextStateAfterDelay = State::WIFI_CONNECTING;
                        enterState(State::TRANSITION_DELAY);
                        _stateTimer = millis();
                    }
                    break;
                
                case State::WIFI_CONNECTING:
                     if (_stateTimer == 0) {
                        Serial.println("NetMan: Starting Wi-Fi connection...");
                        WiFi.mode(WIFI_STA);
                        // WiFi.setCountry("UA");
                        Serial.println(WIFI_SSID);
                        Serial.println(WIFI_PASS);
                        WiFi.begin(WIFI_SSID, WIFI_PASS);
                        _stateTimer = millis();
                    }

                    if (WiFi.status() == WL_CONNECTED) {
                        Serial.print("NetMan: Wi-Fi Connected. IP: ");
                        Serial.println(WiFi.localIP());
                        _activeConnection = NetworkType::WIFI;
                        syncNTP();
                        enterState(State::CONNECTED_WIFI);
                        _stateTimer = millis();
                    } else if (millis() - _stateTimer > WIFI_CONNECT_TIMEOUT_MS) {
                        Serial.println("NetMan: Wi-Fi failed. Waiting to try Ethernet.");
                        WiFi.disconnect(true);
                        WiFi.mode(WIFI_OFF);
                        _nextStateAfterDelay = State::ETH_CONNECTING;
                        enterState(State::TRANSITION_DELAY);
                        _stateTimer = millis();
                    }
                    break;

                case State::CONNECTED_ETH:
                    if (Ethernet.linkStatus() != LinkON) {
                        Serial.println("NetMan: Ethernet connection lost. Waiting to try Wi-Fi.");
                        _activeConnection = NetworkType::NONE;
                        SPI.end();                                   // Release SPI bus (critical for recovery)
                        delay(100);
                        _nextStateAfterDelay = State::WIFI_CONNECTING;
                        enterState(State::TRANSITION_DELAY);
                        _stateTimer = millis();
                    } else {
                        Ethernet.maintain();
                    }
                    break;
                
            case State::CONNECTED_WIFI:
                if (WiFi.status() != WL_CONNECTED) {
                    Serial.println("NetMan: Wi-Fi connection lost. Waiting to try Ethernet.");
                    _activeConnection = NetworkType::NONE;
                    WiFi.disconnect(true);
                    WiFi.mode(WIFI_OFF);
                    _nextStateAfterDelay = State::ETH_CONNECTING;
                    enterState(State::TRANSITION_DELAY);
                    _stateTimer = millis();
                }
                break;
                
                case State::TRANSITION_DELAY:
                    if (millis() - _stateTimer > TRANSITION_DELAY_MS) {
                        Serial.println("NetMan: Delay over. Proceeding to next connection type.");
                        enterState(_nextStateAfterDelay);
                    }
                    break;
            }
            xSemaphoreGive(networkClientMutex);
        }
    }

    Client& getClient() {
        if (_activeConnection == NetworkType::ETH) return _ethClient;
        return _wifiClient;
    }
    
    bool isStable() { 
        return (_currentState == State::CONNECTED_ETH || _currentState == State::CONNECTED_WIFI); 
    }
    static void taskFunction(void* parameter) {
        auto* instance = static_cast<NetworkManager*>(parameter);
        for (;;) {
            instance->handleConnection();
            vTaskDelay(500 / portTICK_PERIOD_MS); 
        }
    }
};