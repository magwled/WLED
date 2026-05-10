# Modulino Knob Usermod

Adds support for the [Arduino Modulino® Knob](https://store-usa.arduino.cc/products/modulino-knob) (ABX00107) — an STM32-backed I2C rotary encoder with push-button.

Wire it to WLED's I2C bus via the Qwiic connector. No GPIOs are used beyond the configured `i2c_sda` / `i2c_scl` pins.

## Optional: four-line OLED display

If you also build with `usermod_v2_four_line_display_ALT`, this usermod auto-detects it (no extra build flag) and drives it for visual feedback:

- Brightness / speed / intensity rotation updates the corresponding line.
- Palette / effect rotation shows the current name.
- Short press flashes the new mode name as an overlay (sun / skip-fwd / fire / palette / effect glyph) and moves the cursor.
- The first knob input after the screen sleeps is swallowed and just wakes the display.

## Behaviour

- **Rotation** changes the currently selected parameter.
- **Short press** cycles to the next parameter.
- **Long press** (default 1 s) toggles the lights on/off.

Selectable parameters, in order:

1. Brightness
2. Effect speed
3. Effect intensity
4. Palette
5. Effect

## Configuration

In the WLED Usermods settings page, the `Modulino-Knob` block exposes:

| Field | Default | Notes |
|---|---|---|
| `enabled` | `true` | Disable without rebuilding. |
| `I2C-address` | `0x76` | The documented 8-bit Modulino address. Accepts hex (`0x76`, `0x74`) or decimal (`118`, `116`). Converted internally to the 7-bit form Wire expects. |
| `poll-interval-ms` | `30` | How often to read the knob over I2C. |
| `long-press-ms` | `1000` | Long-press threshold for the on/off toggle. |
| `brightness-step` | `4` | Increment per detent for brightness. The encoder has 30 detents/turn, so `4` sweeps 0–255 in ≈ 2 turns. |
| `speed-step` | `4` | Increment per detent for effect speed. |
| `intensity-step` | `4` | Increment per detent for effect intensity. |

(Palette and effect always step by 1 per detent — finer values would skip choices.)

The I2C SDA/SCL pins themselves are configured in WLED's Hardware setup, not here.

## Build

Add to your `platformio_override.ini`:

```ini
[env:my_board]
extends = env:esp32dev
custom_usermods = ${env:esp32dev.custom_usermods} modulino_knob
```

Override defaults at compile time:

```ini
build_flags =
  ${env:esp32dev.build_flags}
  -D MODULINO_KNOB_ADDRESS=0x74
  -D MODULINO_KNOB_BRIGHTNESS_STEP=2  ; finer brightness control (~4 turns end-to-end)
  -D MODULINO_KNOB_SPEED_STEP=8       ; coarser speed control (~1 turn end-to-end)
```

Available compile-time defines:

- `MODULINO_KNOB_ADDRESS` — documented 8-bit I2C address (default `0x76`; alt `0x74`).
- `MODULINO_KNOB_POLL_MS` — polling period in milliseconds (default `30`).
- `MODULINO_KNOB_LONG_PRESS_MS` — long-press threshold (default `1000`).
- `MODULINO_KNOB_BRIGHTNESS_STEP` / `MODULINO_KNOB_SPEED_STEP` / `MODULINO_KNOB_INTENSITY_STEP` — per-detent steps for the analog parameters (default `4` each).
- `USERMOD_MODULINO_KNOB_DEBUG` — flag (no value): emit serial logs on probe, rotation, button press, long-press, mode change, and I2C read failures. Independent of `WLED_DEBUG`.
