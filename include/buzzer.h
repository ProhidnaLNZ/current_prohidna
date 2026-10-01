#pragma once

#include <Arduino.h>
#include "pitches.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

class Melody {
public:
    const int* notes;
    const int* noteDurations;
    const int length;
    const int duration;

    Melody(const int* mel, const int* dur, int len, int dur1) 
        : notes(mel), noteDurations(dur), length(len), duration(dur1) {}
};

class Buzzer {
private:
    enum class State { IDLE, PLAYING };

    int _buzzerPin;
    const Melody* _approval;
    const Melody* _denial;

    State _currentState = State::IDLE;
    const Melody* _currentMelody = nullptr;
    int _currentNoteIndex = 0;
    unsigned long _nextNoteTime = 0;

public:
    Buzzer(int pin, const Melody* appr, const Melody* den) 
        : _buzzerPin(pin), _approval(appr), _denial(den) {}

    // Must be called from a continuously running task
    void update() {
        if (_currentState != State::PLAYING) {
            return;
        }

        if (millis() > _nextNoteTime) {
            noTone(_buzzerPin); // Stop previous note's tone

            if (_currentNoteIndex >= _currentMelody->duration) {
                // Melody finished
                _currentState = State::IDLE;
                return;
            }

            int noteDuration = _currentMelody->length / _currentMelody->noteDurations[_currentNoteIndex];
            tone(_buzzerPin, _currentMelody->notes[_currentNoteIndex], noteDuration);
            
            int pauseBetweenNotes = noteDuration * 1.30;
            _nextNoteTime = millis() + pauseBetweenNotes;
            
            _currentNoteIndex++;
        }
    }

    void playApproval() {
        if (_currentState == State::PLAYING) return; // Ignore if already playing
        _currentMelody = _approval;
        _currentNoteIndex = 0;
        _nextNoteTime = millis();
        _currentState = State::PLAYING;
    }
    
    void playDenial() {
        if (_currentState == State::PLAYING) return;
        _currentMelody = _denial;
        _currentNoteIndex = 0;
        _nextNoteTime = millis();
        _currentState = State::PLAYING;
    }
};