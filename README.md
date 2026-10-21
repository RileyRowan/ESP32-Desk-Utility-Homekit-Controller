# ESP32 Desk Utility HomeKit Controller

An ESP32 that runs my desk. It's a HomeKit bridge built on [HomeSpan](https://github.com/HomeSpan/HomeSpan) that controls a Samsung TV over IR, drives a WS2812B monitor backlight, and takes input from a single capacitive touch pad.

## Touch Commands

| Gesture | Action |
|---|---|
| 1 tap | TV Volume Up |
| 2 taps | TV Volume Down |
| 3 taps | TV Mode |
| 4 taps | Monitor Light on/off (turns on in warm white at 50%) |
| 5 taps | Next light effect: Solid → Chase Red → Chase Blue → Color Cycle → Fireflies |
| Hold 1 s | Desk Switch ON: light fades up warm, TV power sent after 5 s |
| Hold 3 s | Desk Switch OFF: light fades out |

Taps must land within 400 ms of each other.

## HomeKit Accessories

| Tile | What it does |
|---|---|
| Desk Switch | Desk scene on/off. ON fades the light up warm and sends TV power after 5 s |
| TV Power / Volume Up / Volume Down / Mode | Momentary buttons that send the Samsung IR code |
| Monitor Light | Full color bulb (hue, saturation, brightness) for the whole strip |
| Chase Red / Chase Blue | Red or blue wave drifting over the Monitor Light color |
| Color Cycle | Red/green/blue wheel rotating around the monitor, 10 s per loop |
| Fireflies | Warm base at 10% with random pixels swelling up and back down |

The effect tiles work like radio buttons: turning one on turns the others off. Every mode change crossfades instead of snapping.

## Hardware

| Part | GPIO |
|---|---|
| ESP32-WROOM-32E | — |
| TTP223B touch sensor | 18 |
| IR LED (Samsung codes) | 4 |
| WS2812B strip, 12 V, 114 LEDs (38 addressable groups of 3) | 5 |

The strip runs from its own 12 V supply and shares ground with the ESP32.

## Software

- Arduino IDE, ESP32 core **2.0.14**
- HomeSpan **1.9.1** (its built-in `Pixel` class drives the LEDs)
- IRremoteESP8266 **2.9.0**

## Setup

1. Flash `DeskIRController.ino`.
2. Open the Serial Monitor at 115200 baud and type `W` to enter WiFi credentials.
3. Add the bridge in the Home app with setup code **466-37-726**.

## Serial Commands

| Command | Action |
|---|---|
| `?` | HomeSpan command list |
| `W` | WiFi setup |
| `E` | Erase stored data (WiFi and pairing) |
| `@W` | Save the Monitor Light's current color as the new warm white (kept across reboots) |
