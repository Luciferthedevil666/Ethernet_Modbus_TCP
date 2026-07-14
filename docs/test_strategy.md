# Test Strategy

## Unit Testing

- Validate parameter checks for every public API.
- Validate endian conversion and protocol field encoding.
- Validate Modbus MBAP parsing and exception handling.
- Validate JSON formatting and buffer length handling.
- Validate HTTP status code parsing.

## Integration Testing

- Verify SPI communication with W5500 version register.
- Verify link detection and static IP configuration.
- Verify TCP connect, send, receive, disconnect, and reconnect.
- Verify Modbus TCP polling against the ESP32-WROOM32 dummy PLC.
- Verify JSON upload path to Google Apps Script.

## Stress Testing

- Run 24-hour endurance test at 1-second polling.
- Poll 100 registers per cycle.
- Disconnect and reconnect Ethernet cable.
- Restart router or switch.
- Power cycle dummy PLC.
- Simulate cloud outage.
- Increase polling rate to find saturation limits.
- Monitor heap, stack, queue depth, packet counters, and reconnect counters.
