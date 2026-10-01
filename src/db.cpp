#include "db.h"

DataBase::DataBase() : _fs(LittleFS) {}

void DataBase::setup() {
    _fileMutex = xSemaphoreCreateMutex();
    if (_fileMutex == NULL) {
        Serial.println("FATAL: Could not create DB mutex!");
        return; // Or handle error more gracefully
    }

    // CRITICAL FIX: Set to 'false' to prevent erasing data on every reboot.
    if (!LittleFS.begin(false)) {
        Serial.println("LittleFS Mount Failed. Formatting...");
        if (!LittleFS.begin(true)) {
            Serial.println("FATAL: LittleFS format failed!");
            return;
        }
    }

    if (!_fs.exists(_outboxPath)) {
        File file = _fs.open(_outboxPath, FILE_WRITE);
        if (file) {
            file.close();
        }
    }
}

void DataBase::_lock() {
    xSemaphoreTake(_fileMutex, portMAX_DELAY);
}

void DataBase::_unlock() {
    xSemaphoreGive(_fileMutex);
}

void DataBase::appendRead(const String& data) {
    _lock();
    File file = _fs.open(_outboxPath, FILE_APPEND);
    if (!file) {
        Serial.println("- failed to open file for appending");
        _unlock();
        return;
    }
    if (file.println(data)) {
        Serial.println("- message appended to outbox");
    } else {
        Serial.println("- outbox append failed");
    }
    file.close();
    _unlock();
}

std::vector<String> DataBase::getAllReads() {
    _lock();
    std::vector<String> reads;
    File file = _fs.open(_outboxPath, FILE_READ);
    if (!file || file.isDirectory()) {
        if (file) file.close();
        _unlock();
        return reads;
    }
    while (file.available()) {
        String line = file.readStringUntil('\n');
        line.trim();
        if (line.length() > 0) {
            reads.push_back(line);
        }
    }
    file.close();
    _unlock();
    return reads;
}
void DataBase::removeSentReads(const std::vector<String>& sentMessages) {
    if (sentMessages.empty()) return;

    _lock();
    File originalFile = _fs.open(_outboxPath, FILE_READ);
    File tempFile = _fs.open(_tempOutboxPath, FILE_WRITE);
    if (!originalFile || !tempFile) {
        Serial.println("Error opening files for outbox cleanup.");
        if (originalFile) originalFile.close();
        if (tempFile) tempFile.close();
        _unlock();
        return;
    }

    while (originalFile.available()) {
        String line = originalFile.readStringUntil('\n');
        line.trim();
        if (line.length() == 0) continue;

        bool wasSent = false;
        for (const auto& sent : sentMessages) {
            if (line.equals(sent)) {
                wasSent = true;
                break;
            }
        }
        if (!wasSent) {
            tempFile.println(line);
        }
    }

    originalFile.close();
    tempFile.close();

    _fs.remove(_outboxPath);
    _fs.rename(_tempOutboxPath, _outboxPath);
    Serial.printf("Removed %d sent messages from outbox.\n", sentMessages.size());
    _unlock();
}