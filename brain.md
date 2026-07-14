# Industrial Ethernet IoT Gateway Brain

## Source Of Truth

The active project direction is the MASTER DEVELOPMENT PROMPT for the **Industrial Ethernet IoT Gateway**.

Always maintain:

- `brain.md`
- `readme.md`
- `README.md`

## Objective

Build a commercial-grade Industrial Ethernet IoT Gateway using ESP-IDF and Embedded C.

The gateway shall run on ESP32-S3 with W5500 Ethernet and communicate with industrial devices using Modbus TCP over Ethernet. It shall process engineering data and upload it through HTTP/HTTPS using the same Ethernet interface.

The ESP32-WROOM32 with W5500 acts as a simulated PLC / industrial controller. It generates realistic Modbus TCP register values without physical sensors.

## Non-Negotiable Engineering Rules

- No Arduino.
- No C++.
- No external networking libraries for core firmware.
- No W5500 libraries.
- No Modbus libraries.
- No JSON libraries.
- No HTTP libraries.
- No dynamic memory allocation after startup.
- No monolithic files.
- No shared globals for task communication.
- No placeholder functions.
- No TODO comments.
- Every module must compile independently.
- Every public API must return proper error codes once implementation begins.
- Every module must be documented.

## Repository Shape

```text
IndustrialGateway/
  CMakeLists.txt
  sdkconfig.defaults
  readme.md
  brain.md
  docs/
  main/
  config/
  tests/
  scripts/
```

Current user preference: no `components/` folder. All firmware headers and source files live in `main/` and are compiled by `main/CMakeLists.txt`.

```text
main/
  main.c
  board.h
  board.c
  spi_bus.h
  spi_bus.c
  w550.h
  w5500.c
  w5500_regs.h
  w5500_socket.h
  w5500_socket.c
  ethernet.h
  ethernet.c
  tcp_client.c
  tcp_server.c
  mbap.c
  modbus_client.c
  modbus_server.c
  register.c
  http_client.h
  http_client.c
  json_writer.c
  config.h
  error.h
  log.h
  types.h
```

## Logical Modules

Even though the files are now flattened into `main/`, keep these logical module boundaries in code:

- common
- bsp
- spi
- w5500
- ethernet
- tcp
- modbus
- json
- http
- storage
- diagnostics
- watchdog
- logger
- tasks
- application
- utilities

## FreeRTOS Architecture

Tasks:

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

Task communication must use:

- Queues
- Event Groups
- Mutexes

## Development Sequence

1. System architecture
2. Folder structure
3. Build system
4. BSP
5. SPI Driver
6. W5500 Driver
7. Ethernet Manager
8. TCP Layer
9. Modbus TCP Server for Dummy PLC
10. Modbus TCP Client for Gateway
11. Data Processing
12. JSON Serializer
13. HTTP Client
14. Google Sheets Integration
15. Diagnostics
16. Logger
17. Watchdog
18. Configuration Storage
19. Integration Testing
20. Performance Optimization
21. Documentation

Do not skip steps. Every generated module must compile before moving to the next implementation layer.

## Current State

Steps 1-3 are being established:

- System architecture documentation exists under `docs/`.
- The active build layout has been flattened into the single ESP-IDF `main` component at the user's request.
- There is no active `components/` folder.
- The previous local `components/json` shadowing issue is gone because the local component folder was removed. Gateway application code still follows the project rule: manual JSON serialization, no cJSON usage for product firmware.
- Google Apps Script endpoint is configured in `main/config.h`:
  - Host: `script.google.com`
  - Port: `443`
  - Path: `/macros/s/AKfycby1Htg3a0idXy6TQNXRXqb0zqzmIL4bxeptHTPGlPYOqZ5d1V_3A0G0lpp-6vMSe-jg/exec`
  - Transport requirement: HTTPS/TLS

No deep driver or protocol implementation should be generated until the build structure is verified in an ESP-IDF environment.
