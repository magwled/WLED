# VL53L4CD Gestures Usermod

This usermod adds gesture support using the VL53L4CD Time-of-Flight sensor.

## Features

- **Short gesture (swipe)**: Wave your hand in front of the sensor to trigger on/off
- **Long gesture (hold)**: Hold your hand in front of the sensor for 1 second, then adjust distance to control brightness

## Hardware

Connect the VL53L4CD sensor to your ESP32's I2C pins:
- VCC to 3.3V
- GND to GND
- SDA to I2C SDA pin
- SCL to I2C SCL pin

## Configuration

### Enabling

Add `VL53L4CD_gestures` to `custom_usermods` in your `platformio_override.ini`:

```ini
custom_usermods = ${env.custom_usermods} VL53L4CD_gestures

build_flags = ${env.build_flags}
  -D I2CSDAPIN=6   ; your SDA pin
  -D I2CSCLPIN=7   ; your SCL pin
```

No external library dependency is required — the usermod talks to the sensor directly over I2C.

### Runtime Configuration

The following parameters can be configured in WLED's Config > Usermods:

| Parameter | Default | Description |
|-----------|---------|-------------|
| enabled | true | Enable/disable the usermod |
| maxRange | 230 | Maximum detection range in mm |
| minOffset | 60 | Minimum range offset in mm |
| delay | 100 | Polling interval in ms |
| longDelay | 1000 | Time to trigger long gesture in ms |

## Differences from VL53L0X

This usermod uses the VL53L4CD sensor which:
- Has better accuracy at short range (1-1300mm)
- Has a different initialization sequence (handled directly in the usermod)
