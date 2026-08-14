# Universal Voltronic / Inverter Web Gateway. All Voltronic Based Inverters Supported (Inverex, Ziewnic, Fronus, XYZ) on 12,24,48 V Supported

A lightweight, high-performance ESP32-powered serial-to-Wi-Fi bridging gateway designed explicitly for Voltronic, Axpert, and Ziewnic split-phase/hybrid inverters. This gateway acts as a standalone matrix conduit, transforming the inverter's proprietary 2400-baud RS-232 serial connection into a modern, real-time web dashboard and MQTT telemetry publisher.

---

## 🚀 Key Features

* **Classic Dashboard Interface:** A responsive, lightweight, dark-themed UI that showcases real-time grid input, inverter output states, load capacities, and battery metrics at a glance.
* **Pure Hex Diagnostic Terminal:** A manual raw byte field allowing you to test commands manually live. Enter space-separated hex instructions (like `51 50 49 47 53 0D B7 A9 0D`) to bypass core parsers and directly interrogate the raw copper serial bus rail.
* **Autonomous Time-Based Scheduler:** Configurable automated slots saved directly to non-volatile storage (`Preferences`). Runs entirely on the microchip core without requiring a browser window to be kept open.
* **Advanced Post-Window Fallback:** Each scheduler slot features an explicit **Time-Expired End Mode** drop-down, forcing the inverter to transition to a designated fallback state (e.g., automatically returning to Utility mode) the exact minute a slot expires.
* **Voltronic Protocol Hardcoded Intercepts:** Commands targeting SBU mode (`POP02`) are automatically intercepted by the core firmware and routed through an isolated, pre-calculated raw hex sequence (`50 4F 50 30 32 E2 0B 0D`). This safely bypasses the notorious forbidden low/high byte microprocessor bugs where the inverter drops framing sync and throws a `NAK`.
* **Robust MQTT Telemetry & Controls:** Fully configurable client configuration panels to link directly into home automation hubs like Home Assistant or Node-RED, publishing space-separated live parameters and JSON payloads.
* **Seamless OTA Firmware Updates:** Built-in over-the-air binary uploading platform. Update, tweak, or flash new versions of the firmware straight from your browser without ever crawling into your inverter box to replug your physical USB programming cables.

---

## 📸 Screenshots

### Live Matrix Dashboard
<img width="1366" height="1661" alt="image" src="https://github.com/user-attachments/assets/427415c3-ec7f-4145-b5c1-2851033ca947" />


### Advanced Scheduler & Fallback Engine
<img width="1366" height="689" alt="image" src="https://github.com/user-attachments/assets/520a7889-d856-42ce-bd8f-a16d7ffde531" />

### Universal Hex & ASCII Diagnostic Terminal
<img width="1366" height="1103" alt="image" src="https://github.com/user-attachments/assets/8083774a-5ad7-4787-811f-50c993b87bfd" />
<img width="1366" height="1248" alt="image" src="https://github.com/user-attachments/assets/aa475acf-8a71-40fc-ac60-3462715eb164" />

---

## 🛠️ Hardware Requirements & Pin Mapping

* **Microcontroller:** ESP32 Development Board (Locked to high-performance dual-core architectures).
* **RS-232 Transceiver:** MAX3232 TTL to RS-232 Module with shared common reference points.
* **Power:** High-efficiency LM2596 Buck Converter step-down rail tapped into a stable supply voltage.

### Verified Pin Layout
```text
  ESP32 Pin     MAX3232 Pin     Inverter Port RJ45/DB9
  ======================================================
    GPIO 25  --->   RXD2     --->    TX Pin 
    GPIO 26  --->   TXD2     --->    RX Pin
     GND     --->   GND      --->    Shared Common Ground

## 👨‍💻 Contributing & Pull Requests

**Any contributions to this project are most welcome!** Whether you want to fix a bug, optimize the frontend UI, or add support for new inverter query codes, your help makes this gateway better for everyone.

### Areas where you can help:
* **Pre-calculated Hex Profiles:** Adding verified hex payloads for other battery setups (like 12V or 48V banks) for commands like `PBDV`.
* **Telemetry Decoders:** Expanding the C++ or JavaScript parsers to extract more hidden indexes from `QPIRI` or `QFLAG`.
* **UI Themes:** Enhancing the CSS or adding dynamic charts to the dashboard template.

Issues and feature requests are open for community tracking. Let's build the ultimate inverter bridge together!
