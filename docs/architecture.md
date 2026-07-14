# System Architecture

## Product

Industrial Ethernet IoT Gateway built with ESP-IDF and Embedded C.

The gateway firmware targets an ESP32-S3 with a W5500 Ethernet controller. It communicates with industrial devices through Modbus TCP and uploads processed engineering values to Internet services through manually generated HTTP/HTTPS requests on the same Ethernet interface.

## Hardware Roles

- Gateway: ESP32-S3, W5500, RJ45 Ethernet, USB UART debug.
- Industrial Device Simulator: ESP32-WROOM32, W5500, RJ45 Ethernet.

The simulator behaves like a PLC by exposing Modbus TCP holding registers with slowly changing industrial values.

## Runtime Tasks

- Main Task
- Network Manager Task
- Ethernet Monitor Task
- Modbus Poll Task
- HTTP Upload Task
- Data Processing Task
- Diagnostics Task
- Logger Task
- Watchdog Task
- LED Status Task

Tasks communicate through queues, event groups, and mutexes. Shared global state is avoided.

## Network Responsibilities

The single Ethernet interface shall support:

- Modbus TCP
- HTTP
- Future HTTPS
- Future MQTT
- Future OTA
- Future NTP

## Reliability Goals

The system must recover from cable disconnects, PLC downtime, router restarts, socket exhaustion, HTTP failures, DNS failures, and cloud outages without unnecessary MCU resets.
