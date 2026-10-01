#pragma once

#include "freertos/semphr.h"
#include <Arduino.h>
#include <PubSubClient.h>
#include "db.h"
#include "network_manager.h"
#include "whitelist.h"
#include <vector>
#include "timer.h"
#include <esp_task_wdt.h>

extern SemaphoreHandle_t networkClientMutex;

class MqttClient {
private:
    const char* mqtt_server;
    const int mqtt_port;
    const char* mqtt_username;
    const char* mqtt_password;

    // Topics
    static constexpr const char* reads_topic_path = "device/reader_real/events/read";
    static constexpr const char* whitelist_request_topic_path = "device/reader_real/whitelist/request";
    static constexpr const char* whitelist_response_topic_path = "device/reader_real/whitelist/response";

    NetworkManager* netManager;
    PubSubClient mqtt;
    DataBase* db_ptr;
    Whitelist* whitelist_ptr;

    xQueueHandle queue_handle_;

    unsigned long last_outbox_check_ = 0;
    static constexpr unsigned long outbox_check_interval_ = 5000;

    // Static instance pointer for callback
    static MqttClient* _instance;

    // MQTT Callback (static)
    static void mqttCallback(char* topic, byte* payload, unsigned int length) {
        Serial.printf("MQTT: Incoming message on topic [%s], length=%d\n", topic, length); // ADD THIS
        if (!_instance) return;

        String t = topic;
        if (t == whitelist_response_topic_path) {
            if (_instance->whitelist_ptr) {
                String msg;
                msg.reserve(length + 1);
                for (unsigned int i = 0; i < length; i++) msg += (char)payload[i];
                _instance->whitelist_ptr->processChunk(msg);  // changed from loadFromJson
            }
        }
        // TODO: Add handling for COMMAND_TOPIC if needed
    }

    void reconnect() {
        if (mqtt.connected()) return;
        esp_task_wdt_reset(); // ADD THIS
        Serial.println("MQTT: Attempting to connect...");

        
        Client& netClient = netManager->getClient();

        // CRITICAL FIX 1: Set a short timeout on the network client.
        // This makes the connect() call non-blocking for the WDT.
        // Timeout is in seconds.
        netClient.setTimeout(5); // Fail after 5 seconds instead of blocking forever

        mqtt.setClient(netClient);
        mqtt.setServer(mqtt_server, mqtt_port);

        String clientId = "ProhidnaReader-";
        clientId += String(random(0xffff), HEX);

        if (mqtt.connect(clientId.c_str(), mqtt_username, mqtt_password)) {
            Serial.println("MQTT: Connection SUCCESSFUL!");

            // Subscribe to important topics
            mqtt.setBufferSize(4096); // must be large enough for the full whitelist JSON
            if (mqtt.subscribe(whitelist_response_topic_path)) {
                Serial.println("MQTT: Subscribed to whitelist response topic OK");
            } else {
                Serial.println("MQTT: Failed to subscribe to whitelist response topic!");
            }
            _instance = this;                    // Register this instance for callback
            mqtt.setCallback(mqttCallback);
        } else {
            Serial.print("MQTT: Connection FAILED, rc=");
            Serial.println(mqtt.state());
        }
    }
    void processOutbox() {
        if (!db_ptr) return;
        std::vector<String> messages = db_ptr->getAllReads();
        if (messages.empty()) return;

        Serial.printf("Found %d messages in outbox. Attempting to send...\n", messages.size());
        std::vector<String> successfullySent;

        for (const auto& msg : messages) {
            if (!mqtt.connected()) {
                Serial.println("MQTT: Connection lost during outbox send.");
                break;
            }

            String payload = replaceMillisWithTime(msg);

            if (mqtt.publish(reads_topic_path, payload.c_str())) {
                successfullySent.push_back(msg);
                Serial.printf("Sent: %s\n", payload.c_str());
                vTaskDelay(pdMS_TO_TICKS(50));        // Small delay to avoid flooding
            } else {
                Serial.println("Failed to publish from outbox, will retry later.");
                break;
            }
        }

        if (!successfullySent.empty()) {
            db_ptr->removeSentReads(successfullySent);
        }
    }

public:
    MqttClient(NetworkManager& manager, 
               const char* server, 
               int port, 
               const char* username, 
               const char* password)
        : netManager(&manager),
          mqtt_server(server),
          mqtt_port(port),
          mqtt_username(username),
          mqtt_password(password),
          mqtt() {}

    void setup(DataBase& db, xQueueHandle queue, Whitelist& wl) {
        db_ptr = &db;
        queue_handle_ = queue;
        whitelist_ptr = &wl;

        mqtt.setKeepAlive(10);
    }

    // ЗМІНЕНО: Повністю переписана логіка для правильної офлайн-роботи
    void process() {
        // --- Крок 1: Завжди обробляти чергу, незалежно від стану мережі ---
        char received_data[64];
        // Перевіряємо, чи є щось нове в черзі від NFC
        if (xQueueReceive(queue_handle_, &received_data, 0) == pdPASS) {
            Serial.printf("MQTT Task: Received from queue: %s\n", received_data);

            bool published = false;
            // Намагаємось опублікувати, тільки якщо є стабільне з'єднання

            if (xSemaphoreTake(networkClientMutex, pdMS_TO_TICKS(50)) == pdTRUE) {
                if (netManager->isStable() && mqtt.connected()) {
                    String payload = replaceMillisWithTime(String(received_data));
                    if (mqtt.publish(reads_topic_path, payload.c_str())) {
                        Serial.println("MQTT Task: Published live message successfully.");
                        published = true;
                    }
                }
                xSemaphoreGive(networkClientMutex);
            }

            
            // Якщо не вдалося опублікувати, зберігаємо у файл-outbox
            if (!published) {
                Serial.println("MQTT Task: Network unavailable or publish failed. Saving to outbox.");
                if (db_ptr) {
                    db_ptr->appendRead(String(received_data));
                }
            }
        }

        // --- Крок 2: Управління з'єднанням та відправка старих даних з outbox ---
        if (xSemaphoreTake(networkClientMutex, pdMS_TO_TICKS(10)) == pdTRUE) {
            if (netManager->isStable()) {
                if (!mqtt.connected()) {
                    reconnect();
                } else {
                    // mqtt.loop() важливий для підтримки з'єднання
                    mqtt.loop(); 
                }

                // Якщо ми підключені, обробляємо outbox
                if (mqtt.connected()) {
                    if (millis() - last_outbox_check_ > outbox_check_interval_) {
                        processOutbox();
                        last_outbox_check_ = millis();
                    }
                }
            } else {
                if (mqtt.connected()) {
                    mqtt.disconnect();
                }
            }
            xSemaphoreGive(networkClientMutex);
        }

        // === 3. Whitelist synchronization request ===
        if (whitelist_ptr && whitelist_ptr->needsSync()) {
            if (mqtt.connected()) {
                whitelist_ptr->setLoading();
                mqtt.publish(whitelist_request_topic_path, "");  // empty payload = request full list
                Serial.println("MQTT: Requested whitelist update");
            }
        }
    }

    static void taskFunction(void* parameter) {
        MqttClient* instance = (MqttClient*)parameter;
        for (;;) {
            instance->process();
            vTaskDelay(pdMS_TO_TICKS(50));
        }
    }
};

// Initialize static member
MqttClient* MqttClient::_instance = nullptr;