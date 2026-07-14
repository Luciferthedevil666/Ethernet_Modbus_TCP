# Industrial Ethernet IoT Gateway

Commercial-grade ESP-IDF firmware for an industrial Ethernet gateway.

The gateway runs on an ESP32-S3 with a W5500 Ethernet controller. It polls industrial devices using Modbus TCP, processes raw registers into engineering values, and uploads JSON payloads to Internet services through manually built HTTP/HTTPS requests on the same Ethernet interface.

## Hardware

- Gateway: ESP32-S3, W5500 Ethernet controller, RJ45 Ethernet, USB UART debug.
- Industrial Device Simulator: ESP32-WROOM32, W5500 Ethernet controller, RJ45 Ethernet.

The ESP32-WROOM32 acts as a dummy PLC. It exposes 100 Modbus TCP holding registers that represent realistic process values such as temperature, humidity, pressure, voltage, current, power, frequency, motor speed, flow, water level, energy, alarm status, device status, firmware version, and serial number.

## Project Layout

```text
IndustrialGateway/
  CMakeLists.txt
  sdkconfig.defaults
  README.md
  readme.md
  brain.md
  docs/
  main/
  config/
  tests/
  scripts/
```

Current build layout is intentionally flat. All firmware `.c` and `.h` files are in `main/` and compiled by `main/CMakeLists.txt`.

```text
main/
  main.c
  board.h / board.c
  spi_bus.h / spi_bus.c
  w550.h / w5500.c
  w5500_regs.h
  w5500_socket.h / w5500_socket.c
  ethernet.h / ethernet.c
  tcp_client.c
  tcp_server.c
  mbap.c
  modbus_client.c
  modbus_server.c
  register.c
  http_client.h / http_client.c
  json_writer.c
  config.h
  error.h
  log.h
  types.h
```

## Logical Modules

- Common: shared types, errors, and configuration primitives.
- BSP: board pins, GPIO, LEDs, reset, interrupts, chip selects.
- SPI: SPI bus initialization, transfer, burst read/write, timeout, mutex protection.
- W5500: W5500 reset, register access, sockets, buffers, PHY, interrupts, recovery.
- Ethernet: network configuration, link monitoring, reconnect, diagnostics.
- TCP: hardware-independent TCP abstraction.
- Modbus: Modbus TCP server/client protocol implementation.
- JSON: manual JSON serializer.
- HTTP: manual HTTP/HTTPS-ready client abstraction.

## Architecture Docs

- `docs/architecture.md`
- `docs/dependency_graph.md`
- `docs/test_strategy.md`

## Development Rules

- Embedded C only.
- ESP-IDF only.
- No Arduino.
- No C++.
- No external W5500, Modbus, JSON, or HTTP libraries.
- No dynamic allocation after startup.
- Task communication through queues, event groups, and mutexes.
- Hardware drivers remain separate from application logic.
- Every implementation milestone must compile before the next layer is added.
