#pragma once
#include <Arduino.h>
#include <set>
#include <ArduinoJson.h>
#include <LittleFS.h>

class Whitelist {
public:
    enum class Status { EMPTY, LOADING, READY };

private:
    std::set<String> _cards;
    Status _status = Status::EMPTY;
    unsigned long _lastSync = 0;
    static constexpr unsigned long SYNC_INTERVAL_MS = 24UL * 60 * 60 * 1000;
    const char* _cachePath = "/whitelist.txt";

    // Chunked transfer state
    String _assemblyBuffer = "";
    int _expectedChunks = -1;
    int _receivedChunks = 0;

    void _saveToFile() {
        File file = LittleFS.open(_cachePath, FILE_WRITE, true);
        if (!file) return;
        for (const auto& uid : _cards) file.println(uid);
        file.close();
        Serial.printf("Whitelist: Saved %d cards.\n", _cards.size());
    }

    void _loadFromFile() {
        File file = LittleFS.open(_cachePath, FILE_READ);
        if (!file) { Serial.println("Whitelist: No cache file."); return; }
        _cards.clear();
        while (file.available()) {
            String line = file.readStringUntil('\n');
            line.trim();
            if (line.length() > 0) _cards.insert(line);
        }
        file.close();
        _status = Status::READY;
        _lastSync = 0;
        Serial.printf("Whitelist: Loaded %d cards from cache.\n", _cards.size());
    }

public:
    Status getStatus() const { return _status; }
    bool isReady() const { return _status == Status::READY; }

    void setup() { _loadFromFile(); }
    void setLoading() {
        _status = Status::LOADING;
        _assemblyBuffer = "";
        _expectedChunks = -1;
        _receivedChunks = 0;
    }

    // Called for each incoming MQTT chunk message
    // Expected format: "CHUNK:<index>:<total>:<json_part>"
    bool processChunk(const String& message) {
        if (!message.startsWith("CHUNK:")) {
            return loadFromJson(message);
        }

        // Format: "CHUNK:<index>:<total>:<data>"
        // Search starts at 6 to skip past the "CHUNK:" prefix colon at position 5
        int sep1 = message.indexOf(':', 6);   // colon after chunk index
        int sep2 = message.indexOf(':', sep1 + 1); // colon after total count

        if (sep1 == -1 || sep2 == -1) {
            Serial.println("Whitelist: Bad chunk format");
            return false;
        }

        int chunkIndex  = message.substring(6, sep1).toInt();
        int totalChunks = message.substring(sep1 + 1, sep2).toInt();
        String part     = message.substring(sep2 + 1); // everything after "CHUNK:N:M:"

        if (totalChunks <= 0) {
            Serial.println("Whitelist: Invalid chunk total");
            return false;
        }

        if (_expectedChunks == -1) {
            _expectedChunks = totalChunks;
            _assemblyBuffer = "";
            _receivedChunks = 0;
            Serial.printf("Whitelist: Expecting %d chunks.\n", totalChunks);
        }

        if (totalChunks != _expectedChunks) {
            Serial.println("Whitelist: Chunk total mismatch, resetting.");
            setLoading();
            return false;
        }

        _assemblyBuffer += part;
        _receivedChunks++;
        Serial.printf("Whitelist: Got chunk %d/%d\n", chunkIndex + 1, totalChunks);

        if (_receivedChunks == _expectedChunks) {
            Serial.println("Whitelist: All chunks received, parsing...");
            return loadFromJson(_assemblyBuffer);
        }
        return true;
    }

    bool loadFromJson(const String& json) {
        JsonDocument doc;
        if (deserializeJson(doc, json) != DeserializationError::Ok) {
            Serial.println("Whitelist: JSON parse failed");
            return false;
        }
        _cards.clear();
        for (const char* uid : doc.as<JsonArray>()) {
            String s = String(uid);
            s.trim();
            _cards.insert(s);
        }
        _status = Status::READY;
        _lastSync = millis();
        _assemblyBuffer = "";
        _expectedChunks = -1;
        _receivedChunks = 0;
        Serial.printf("Whitelist: Updated with %d cards.\n", _cards.size());
        _saveToFile();
        return true;
    }

    bool isAllowed(const String& uid) const {
        String trimmed = uid;
        trimmed.trim();
        return _cards.count(trimmed) > 0;
    }

    bool needsSync() const {
        if (_status == Status::EMPTY) return true;
        if (_status == Status::LOADING) return false;
        return (millis() - _lastSync) > SYNC_INTERVAL_MS;
    }
};