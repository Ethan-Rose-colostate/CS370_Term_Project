## Architecture

![Architecture](docs/architecture.svg)

At boot the program calls `fork()`: the parent process becomes the light monitor and the child process becomes the moisture monitor, each single-threaded and sleeping on a 20-minute timer (`timerfd`). When the child's timer fires, it reads the moisture probe over SPI through an MCP3008 ADC (`/dev/spidev0.0`), day or night. If the reading is in the expected range, or out of range while any of the previous 4 readings were in range (treated as a one-off), it is stored. If it is the 5th out-of-range reading in a row and the time is between 10 am and 6 pm, the child also sends a "time to water" notification to the LAN status page or an LED, never the internet. In every case the value and its time are stored.

| Component | Job | Menu mechanism |
|---|---|---|
| Parent (light monitor) | Reads light over I2C (BH1750, `/dev/i2c-1`) every 20 min during daytime only: between expected sunrise and sunset for the configured time zone, hemisphere, and season (the date may need to be supplied at startup); applies the range / one-off / 5-in-a-row check and notifies "too high" or "too low" 10 am–6 pm; on `SIGCHLD` logs it and re-forks the child | E |
| Child (moisture monitor) | Reads moisture over SPI (MCP3008, `/dev/spidev0.0`), day and night, every 20 min; same check, notifies "time to water" 10 am–6 pm | E (isolated, so a water-damaged sensor can't stop light monitoring) |
| Storage layer (shared code) | One fixed-size ring file per sensor (30 days × 72 slots, 16 B records + CRC); a record is kept for 30 days, then overwritten by waiting data or, if none is waiting, cleared and marked free; if a record must be stored and no slot is free, the oldest is overwritten; append + `fsync` per record; reads the last 4 records for the check | D |

### Data-flow rates

| Flow | Rate / size |
|---|---|
| Sensor reads | Moisture: 1 per 20 min, day and night (72/day, 3 B each). Light: 1 per 20 min, sunrise to sunset only (3 per daylight hour, 2 B each) |
| Storage writes | 1 record (16 B, `fsync` each) per sensor read: moisture 72/day; light 3 per daylight hour |
| Storage reads | Last 4 records (64 B), only when a reading is out of range |
| SD card writes | Moisture 1,152 B/day + light 48 B per daylight hour (e.g. 12 h of daylight → 576 B, ~1.7 KB/day total) |
| SD card footprint | ~70 KB total, fixed (2 rings × 2,160 slots × 16 B) |
| Notifications | At most 1 per sensor per 20 min, 10 am–6 pm only (light only while it is daytime) |
| `fork()` | Once at boot, again whenever the child dies |

**Known gap:** nothing restarts the parent if it crashes; a small separate supervisor process is planned to cover this.

## Mechanism mapping

D: Custom storage layer

Component: the shared storage layer used by both processes. It keeps one fixed-size ring file per sensor (30 days × 72 slots), with 16-byte records that each carry a CRC, and it fsyncs after every record.

Justification: Our alert rule only fires after five out-of-range readings in a row, so every decision depends on the last four stored readings being present and correct. A device sitting on a windowsill gets bumped, unplugged, and power-cycled. If a power cut lost the history, a dry plant’s alert would be delayed by another 100 minutes. If it left a half-written record that read back as valid, it could send a false “water now,” and a plant owner who waters on a false alarm is overwatering, the exact failure we exist to prevent. That rules out a plain append file, hence a crash-consistent log with a CRC on every record and an fsync after each write. Because the device runs unattended for months, the file is a fixed-size ring: a record is kept for 30 days, then overwritten by waiting data or, if none is waiting, cleared and marked free; if a record must be stored and no slot is free, the oldest is overwritten, so the SD card can never fill.

E: Multi-process architecture with restart

Component: the program fork()s at boot. The parent process monitors light, and the child process monitors moisture. The parent receives SIGCHLD when the child exits, logs it, and forks a new child.

Justification: The moisture probe sits in soil that the owner waters, so it’s the part most likely to short out, return garbage, or hang a read. Sunburn on a windowsill happens within hours, so light monitoring has to keep running when the moisture side fails. If both sensors ran in one process, one bad probe would silence the whole device, and plant owners would keep trusting a monitor that had stopped watching. Hence each sensor gets its own process, and the parent restarts the moisture child automatically, because plant owners aren’t watching a status page waiting to restart it themselves.

Known gap: if the parent itself crashes, nothing restarts it. A small separate supervisor process is planned to close this.
