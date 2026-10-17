# Battery and solar panel

Notes from 2026-10-13. The device can run on a battery and a solar panel without using any GPIO. The charge percentage also fits without taking pins from the sensor and the actuator, but it uses the two pins left over. The firmware reads the MAX17048 since commit `98588a9` (see the README, power section); the INA219 not yet.

The ESP-01 has only four GPIOs: GPIO0, GPIO2, TX and RX. The sensor takes one, the actuator another. The remaining two are the only place to measure the battery. The chip's analog pin does not reach the connector, so a voltage divider straight on the module does not work.

## Power only, measuring nothing

The battery and the panel sit in the supply path, before the 3.3 V pin. Today's firmware boots the same way and needs no change.

- The cell goes through a charger and a 3.3 V regulator. The cell voltage, up to 4.2 V, does not reach VCC.
- The panel goes into the charger, not a GPIO. For a panel, the charger has to be made for a solar source (for example the CN3791). The TP4056 expects a fixed 5 V from USB and behaves badly with a panel.
- A capacitor of a few hundred µF on the regulator output holds the Wi-Fi peak, which goes past 300 mA. Without it the device reboots when connecting.

This firmware keeps the radio awake all the time, with the CPU at 160 MHz (TLS handshakes in about half the time of 80 MHz; the CPU adds roughly 10 to 15 mA to the radio's ~70 mA average). The numbers work with a cell of about 2000 mAh and a panel of about 1 to 2 W in a sunny spot. A small cell or an indoor panel does not last the night.

## Charge on screen, without touching the sensor or the actuator

The two free pins become an I2C bus, and a gauge goes on it. The sensor and the actuator stay on their pins.

| Chip | What it reports | Pins |
| --- | --- | --- |
| MAX17048 | Cell voltage and charge in % | the two free ones (SDA and SCL) |
| INA219, on the same bus | Voltage and current; shows whether the panel is charging | the same two |

The best pair for this bus is TX and RX: they are not boot pins. GPIO0 and GPIO2 stay with the sensor and the actuator, high at reset. The cost is losing the serial log while I2C sits on those pins.

This setup uses the two pins left over. The stop button planned in [PORTING-FROM-TASMOTA.md](PORTING-FROM-TASMOTA.md) does not fit alongside, unless the sensor becomes an SHT31. Then the battery gauge shares the sensor's I2C bus, the actuator keeps its pin and the fourth pin is free for the button again.

The firmware reads the MAX17048 (charge, voltage and charge rate) every 10 s and shows at the top of the panel whether the device is on supply, on battery (with the charge) or charging, with the details under **Status**. The charge does not go to the API or MQTT yet, and the INA219 is not read.
