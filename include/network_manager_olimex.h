#pragma once

#include "freertos/semphr.h"
#include <Arduino.h>
#include <WiFi.h>
#include <ETH.h>          // ESP32 built-in RMII, replaces SPI Ethernet
#include "time.h"
#include "olimex_pinout.h"
#include "secrets.h"

extern SemaphoreHandle_t networkClientMutex;

enum class NetworkType { NONE, ETH, WIFI };

// LAN8720 RMII config from olimex_pinout.h
#define ETH_CLK_MODE ETH_CLOCK_GPIO0_IN

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

    static constexpr uint32_t ETH_CONNECT_TIMEOUT_MS  = 10000;
    static constexpr uint32_t WIFI_CONNECT_TIMEOUT_MS = 15000;
    static constexpr uint32_t TRANSITION_DELAY_MS     = 2000;

    // --- Network Clients ---
    WiFiClient _wifiClient;
    WiFiClient _ethClient; // ETH.h exposes WiFiClient-compatible interface

    // --- State Machine ---
    State _currentState          = State::INIT;
    State _nextStateAfterDelay   = State::INIT;
    NetworkType _activeConnection = NetworkType::NONE;
    uint32_t _stateTimer         = 0;

    // --- ETH event flag (set from static callback) ---
    volatile bool _ethGotIp   = false;
    volatile bool _ethLostIp  = false;

    // --- NTP ---
    static constexpr const char* NTP_SERVER      = "pool.ntp.org";
    static constexpr long        GMT_OFFSET_SEC  = 3600 * 2;
    static constexpr int         DAYLIGHT_OFFSET_SEC = 0;

    // ETH.h fires Arduino events — we catch them here
    static void ethEventHandler(WiFiEvent_t event) {
        // 'this' not available in static; use a global/singleton pointer
        // See note below about _instance
        switch (event) {
            case ARDUINO_EVENT_ETH_GOT_IP:
                Serial.print("NetMan: Ethernet got IP: ");
                Serial.println(ETH.localIP());
                if (_instance) _instance->_ethGotIp = true;
                break;
            case ARDUINO_EVENT_ETH_DISCONNECTED:
                Serial.println("NetMan: Ethernet disconnected.");
                if (_instance) _instance->_ethLostIp = true;
                break;
            default: break;
        }
    }

    void syncNTP() {
        // Both paths can use configTime since ETH.h integrates with lwIP
        configTime(GMT_OFFSET_SEC, DAYLIGHT_OFFSET_SEC, NTP_SERVER);
        struct tm timeinfo;
        if (getLocalTime(&timeinfo, 5000)) {
            Serial.println("NTP: Time synchronized.");
        } else {
            Serial.println("NTP: Failed to obtain time.");
        }
    }

    void enterState(State newState) {
        _currentState = newState;
        _stateTimer   = 0;
    }

    void startEth() {
        Serial.println("NetMan: Starting Ethernet (LAN8720 RMII)...");
        _ethGotIp  = false;
        _ethLostIp = false;
        ETH.begin(
            ETH_PHY_ADDR,    // from olimex_pinout.h
            ETH_POWER_PIN,   // from olimex_pinout.h
            ETH_MDC_PIN,     // from olimex_pinout.h
            ETH_MDIO_PIN,    // from olimex_pinout.h
            ETH_PHY_LAN8720, // chip type
            ETH_CLK_MODE     // GPIO0_IN
        );
        _stateTimer = millis();
    }

    void startWifi() {
        Serial.println("NetMan: Starting Wi-Fi...");
        WiFi.mode(WIFI_STA);
        WiFi.begin(WIFI_SSID, WIFI_PASS);
        _stateTimer = millis();
    }

public:
    // Needed for static ETH event callback to reach instance state
    static NetworkManager* _instance;

    NetworkManager() { _instance = this; }

    void setup() {
        WiFi.onEvent(ethEventHandler, ARDUINO_EVENT_ETH_GOT_IP);
        WiFi.onEvent(ethEventHandler, ARDUINO_EVENT_ETH_DISCONNECTED);
    }

    void handleConnection() {
        if (xSemaphoreTake(networkClientMutex, pdMS_TO_TICKS(10)) != pdTRUE) return;

        switch (_currentState) {
            case State::INIT:
                Serial.println("NetMan: Initializing...");
                enterState(State::ETH_CONNECTING);
                startEth();
                break;

            case State::ETH_CONNECTING:
                if (_ethGotIp) {
                    _activeConnection = NetworkType::ETH;
                    syncNTP();
                    enterState(State::CONNECTED_ETH);
                } else if (millis() - _stateTimer > ETH_CONNECT_TIMEOUT_MS) {
                    Serial.println("NetMan: Ethernet timeout. Trying Wi-Fi.");
                    _nextStateAfterDelay = State::WIFI_CONNECTING;
                    enterState(State::TRANSITION_DELAY);
                    _stateTimer = millis();
                }
                break;

            case State::WIFI_CONNECTING:
                if (_stateTimer == 0) startWifi();

                if (WiFi.status() == WL_CONNECTED) {
                    Serial.print("NetMan: Wi-Fi connected. IP: ");
                    Serial.println(WiFi.localIP());
                    _activeConnection = NetworkType::WIFI;
                    syncNTP();
                    enterState(State::CONNECTED_WIFI);
                } else if (millis() - _stateTimer > WIFI_CONNECT_TIMEOUT_MS) {
                    Serial.println("NetMan: Wi-Fi timeout. Trying Ethernet.");
                    WiFi.disconnect(true);
                    WiFi.mode(WIFI_OFF);
                    _nextStateAfterDelay = State::ETH_CONNECTING;
                    enterState(State::TRANSITION_DELAY);
                    _stateTimer = millis();
                }
                break;

            case State::CONNECTED_ETH:
                if (_ethLostIp) {
                    Serial.println("NetMan: Ethernet lost. Trying Wi-Fi.");
                    _activeConnection = NetworkType::NONE;
                    _nextStateAfterDelay = State::WIFI_CONNECTING;
                    enterState(State::TRANSITION_DELAY);
                    _stateTimer = millis();
                }
                // No Ethernet.maintain() needed — ETH.h handles it internally
                break;

            case State::CONNECTED_WIFI:
                if (WiFi.status() != WL_CONNECTED) {
                    Serial.println("NetMan: Wi-Fi lost. Trying Ethernet.");
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
                    Serial.println("NetMan: Delay over. Proceeding.");
                    if (_nextStateAfterDelay == State::ETH_CONNECTING) startEth();
                    enterState(_nextStateAfterDelay);
                }
                break;
        }

        xSemaphoreGive(networkClientMutex);
    }

    // ETH.h client and WiFiClient share the same interface
    Client& getClient() {
        // Both ETH and WiFi use WiFiClient — ETH.h uses lwIP under the hood
        return _wifiClient;
    }

    bool isStable() {
        return (_currentState == State::CONNECTED_ETH ||
                _currentState == State::CONNECTED_WIFI);
    }

    static void taskFunction(void* parameter) {
        auto* instance = static_cast<NetworkManager*>(parameter);
        for (;;) {
            instance->handleConnection();
            vTaskDelay(500 / portTICK_PERIOD_MS);
        }
    }
};

// Definition of static member — put this in network_manager.cpp or main.cpp
NetworkManager* NetworkManager::_instance = nullptr;




