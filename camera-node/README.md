# Reversing camera node

ESP32-S3-DevKitC-1 (N16R8) + Waveshare OV5640 Camera Board (C).

It makes its own WiFi network, **HESAB-CAM** (password `hesabcam1`). The cab display joins it
and shows the picture on the Camera tab. A phone can join it too, for testing.

## Wiring (camera board → DevKitC)

Keep the wires short (10 cm or less) if you can: PCLK and XCLK are fast signals.

| Camera board | DevKitC | | Camera board | DevKitC |
|---|---|---|---|---|
| 3.3V | 3V3 | | D0 | GPIO11 |
| GND | GND | | D1 | GPIO9 |
| SIOC | GPIO5 | | D2 | GPIO8 |
| SIOD | GPIO4 | | D3 | GPIO10 |
| VSYNC | GPIO6 | | D4 | GPIO12 |
| HREF | GPIO7 | | D5 | GPIO18 |
| PCLK | GPIO13 | | D6 | GPIO17 |
| XCLK | GPIO15 | | D7 | GPIO16 |
| PWDN | GPIO1 | | RST | GPIO2 |

Everything except PWDN and RST is on the DevKitC's left pin row (the side with 3V3 at the top).
PWDN and RST are on the right row, next to TX/RX.

Don't use GPIO35, 36 or 37: on the N16R8 they belong to the PSRAM.

## Build and flash

1. In VS Code, open **this folder** (`camera-node`) as its own PlatformIO project.
2. Connect the DevKitC by the USB port marked **UART**.
3. Upload, then open the serial monitor.

The log should show:

```
WiFi network "HESAB-CAM" started
Camera found: sensor PID 0x5640
First picture: 640x480, ... bytes
```

## Test with a phone

Join the WiFi network **HESAB-CAM** and open **http://192.168.4.1**.
The page shows the live picture and has buttons for the LEDs, Mirror and Flip.
Mirror and Flip are remembered after a restart, so set them once the camera is mounted.
