#pragma once
#include <Arduino.h>
#include <Adafruit_NeoPixel.h>

// ADDED 'ERROR' state
enum State {waiting, loading, error};

class Led {
    public:
        int led_pin;
        int led_num;
        Adafruit_NeoPixel strip;

        int freq = 60;
        enum State animation = loading;
        int a_delay;

        int i = 0;
        int d = 0;

        int approval_timer = 0;
        int denial_timer = 0;

        int approval_color;
        int denial_color;
        int loading_color; // ADDED for orange
        int error_color;   // ADDED for NFC error
        
        Led(int pin, int num) : 
            led_pin(pin), 
            led_num(num),
            strip(num, pin, NEO_GRB + NEO_KHZ800) {
        }

        int mir(int i){
            if (i>=0){
                return i;
            }
            else return i + led_num;
        }

        void setup() {
            strip.begin();
            strip.clear();
            strip.show();
            a_delay = 1000/freq;

            approval_color = strip.Color(0, 255, 0);   // Green
            denial_color = strip.Color(255, 0, 0);     // Red
            error_color = strip.Color(255, 0, 0);      // Red for error
            loading_color = strip.Color(255, 100, 0);  // Orange
        }

        void approval(int time_ms){
            approval_timer = (int)floor(time_ms/a_delay);
            denial_timer = 0;
        }

        void denial(int time_ms){
            denial_timer = (int)floor(time_ms/a_delay);
            approval_timer = 0;
        }

        void iteration() {
            strip.clear();

            if (approval_timer > 0) {
                strip.fill(approval_color, 0);
                approval_timer--;
            }
            else if (denial_timer > 0) {
                strip.fill(denial_color, 0);
                denial_timer--;
            }
            else {
                switch (animation){
                    case loading:
                        strip.setPixelColor(d, loading_color);
                        strip.setPixelColor(mir(d-1), strip.Color(255/2, 100/2, 0));
                        strip.setPixelColor(mir(d-2), strip.Color(255/3, 100/3, 0));
                        strip.setPixelColor(mir(d-3), strip.Color(255/4, 100/4, 0));
                        strip.setPixelColor(mir(d-4), strip.Color(255/6, 100/6, 0));
                        strip.setPixelColor(mir(d-5), strip.Color(255/8, 100/8, 0));
                        strip.setPixelColor(mir(d-6), strip.Color(255/14, 100/14, 0));
                        if (i%4==0) d++;
                        if (d>=led_num) d = 0;
                        break;
                    case waiting: 
                        strip.fill(strip.Color(0, 0, abs((int)(sin((float)i/20.0)*127.0 + 127.0))), 0);
                        break;
                    case error: // ADDED: Solid red light for error state
                        strip.fill(error_color);
                        break;
            }
            }

            strip.show();
            vTaskDelay(pdMS_TO_TICKS(a_delay));
            i++;
        }
};