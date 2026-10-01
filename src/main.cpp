// main.cpp

#include <Arduino.h>
#include "db.h"
#include "nfc.h"
#include "led.h"
#include "buzzer.h"
#include "pitches.h"
#include "waveshare_pinout.h"
#include "secrets.h"
#include "network_manager.h"
#include "mqtt.h"
#include <esp_task_wdt.h>
#include "WiFi.h"
#include "whitelist.h"

// --- (визначення мелодій залишаються без змін) ---
int approvalNotes[] = {NOTE_C4, NOTE_E4, NOTE_G4, NOTE_C5};
int approvalDurations[] = {4, 4, 4, 4};
Melody approvalMelody(approvalNotes, approvalDurations, 300, 4);
int denialNotes[] = {NOTE_C5, NOTE_A4, NOTE_E4, NOTE_C4};
int denialDurations[] = {4, 4, 4, 4};
Melody denialMelody(denialNotes, denialDurations, 300, 4);



// --- Глобальні об'єкти ---

Buzzer buzzer = Buzzer(BUZZER, &approvalMelody, &denialMelody);
DataBase db;
Led led = Led(LED, LED_NUM);
NetworkManager networkManager;
Nfc nfc;
MqttClient mqttClient(networkManager, MQTT_SERVER, MQTT_PORT, MQTT_USER, MQTT_PASS);
SemaphoreHandle_t networkClientMutex;
Whitelist whitelist;

// ЗМІНЕНО: Створюємо хендл для черги
xQueueHandle readQueue;

#define WDT_TIMEOUT_S 30

void housekeepingTask(void* parameter) {
    for (;;) {
        led.iteration(); // This function already has its own vTaskDelay
        buzzer.update(); // The new update function is non-blocking and fast
    }
}

void WiFiEvent(WiFiEvent_t event, WiFiEventInfo_t info) {
    Serial.printf("[WiFi-event] event: %d\n", event);
    switch (event) {
        case ARDUINO_EVENT_WIFI_READY:
            Serial.println("WiFi interface ready");
            break;
        case ARDUINO_EVENT_WIFI_STA_START:
            Serial.println("WiFi client started");
            break;
        case ARDUINO_EVENT_WIFI_STA_CONNECTED:
            Serial.println("Connected to AP successfully!");
            break;
        case ARDUINO_EVENT_WIFI_STA_DISCONNECTED:
            Serial.println("Disconnected from WiFi access point");
            Serial.printf("Reason code: %d\n", info.wifi_sta_disconnected.reason);
            break;
        case ARDUINO_EVENT_WIFI_STA_GOT_IP:
            Serial.print("Obtained IP address: ");
            Serial.println(WiFi.localIP());
            break;
        default: break;
    }
}

void mqttTask(void* parameter) {
    // Add this task to the watchdog timer
    esp_task_wdt_add(NULL);

    for (;;) {
        // Reset the watchdog timer at the start of every loop
        esp_task_wdt_reset();
        mqttClient.process();
        esp_task_wdt_reset();
        vTaskDelay(pdMS_TO_TICKS(50));
    }
}

void setup(){
    Serial.begin(115200);
    delay(1000);
    Serial.println("--- Starting Setup ---");
    
    WiFi.onEvent(WiFiEvent);

    Serial.printf("Initializing Task Watchdog with timeout of %d seconds\n", WDT_TIMEOUT_S);
    esp_task_wdt_init(WDT_TIMEOUT_S, true);
    esp_task_wdt_add(NULL);

    // ЗМІНЕНО: Ініціалізуємо чергу.
    // 10 - максимальна кількість елементів у черзі
    // sizeof(char[64]) - розмір одного елемента (рядок до 64 символів)
    readQueue = xQueueCreate(50, sizeof(char[64]));

    networkClientMutex = xSemaphoreCreateMutex();

    db.setup();
    whitelist.setup();
    led.setup();
    // ЗМІНЕНО: Передаємо чергу в setup методи
    nfc.setup(db, led, buzzer, readQueue, whitelist);
    mqttClient.setup(db, readQueue, whitelist);

    Serial.println("--- Starting Tasks on Specific Cores ---");

    // ... (створення задач залишається без змін) ...
    xTaskCreatePinnedToCore(
        NetworkManager::taskFunction, "Network Task", 8192, &networkManager, 1, NULL, 0
    );
    xTaskCreatePinnedToCore(
       [](void*){
           for(;;) {
               nfc.read();
               vTaskDelay(10 / portTICK_PERIOD_MS);
           }
       }, "NFC Reader Task", 4096, NULL, 1, NULL, 1
    );

    xTaskCreatePinnedToCore(
       housekeepingTask, "Housekeeping Task", 2048, NULL, 1, NULL, 1
    );
    
    xTaskCreatePinnedToCore(
        mqttTask, "MQTT Client Task", 4096, NULL, 0, NULL, 0
    );

    Serial.println("--- Setup Complete. All tasks are running. ---");
}

void loop(){
    // Ця функція залишається порожньою або з діагностикою, як у попередній версії
    esp_task_wdt_reset();
    vTaskDelay(1000 / portTICK_PERIOD_MS);
}