#pragma once

#include <Arduino.h>
#include <FS.h>
#include <LittleFS.h>
#include <vector>
#include "freertos/semphr.h"

class DataBase {
public:
    DataBase();
    void setup();
    void appendRead(const String& data);
    std::vector<String> getAllReads();
    void removeSentReads(const std::vector<String>& sentMessages);

private:
    FS& _fs;
    const char* _outboxPath = "/outbox.txt";
    const char* _tempOutboxPath = "/outbox.tmp";
    SemaphoreHandle_t _fileMutex; // Mutex for thread-safe file access

    void _lock();
    void _unlock();
};