# Dependency Graph

```text
main
  -> logical application layer
      -> logical task layer
          -> logical ethernet/tcp/modbus/http layers
              -> logical w5500/spi/bsp layers
                  -> common headers
```

The active build is a single ESP-IDF `main` component. Source files are physically flattened into `main/`, but code should still preserve the logical dependency order above.

## Layering Rules

- Application code must not use GPIO numbers directly.
- Application code must not access W5500 registers directly.
- Modbus and HTTP code must use the TCP abstraction.
- TCP code owns socket lifecycle above the W5500 driver.
- SPI code must contain no W5500-specific logic.
- Storage owns persistent configuration.
- Diagnostics, logger, and watchdog observe subsystem health without owning business logic.
