# Epomaker x Aula F75 Max Linux Utility

A Linux command-line utility for the Epomaker x Aula F75 Max keyboard. Controls
the keyboard's built-in 128x128 display and clock via USB HID, without needing
the official Windows software.

## Features

- Upload an animated GIF to the keyboard display
- Sync the keyboard clock to system time
- Manual time override for individual fields (year, month, day, hour, minute, second)
- Factory reset

## Planned

- RGB lighting control
- Key remapping
- Macro programming
- Qt GUI

## Dependencies

- [libusb-1.0](https://libusb.info)
- [giflib](http://giflib.sourceforge.net)
- [cmake](https://cmake.org) (build only)

On Arch Linux:
```bash
sudo pacman -S libusb giflib cmake
```

On Fedora:
```bash
sudo dnf install libusb1-devel giflib-devel cmake
```

On Ubuntu/Debian:
```bash
sudo apt install libusb-1.0-0-dev libgif-dev cmake
```

## Building

```bash
git clone https://github.com/seanauer42/aula-f75max.git
cd aula-f75max
mkdir build && cd build
cmake ..
make
```

The binary is created at `build/aula-cli`.

## Installation

```bash
sudo make install
```

This installs `aula-cli` to `/usr/bin/aula-cli`.

### Permissions

By default, USB devices require root to access. To run without `sudo`, install
the included udev rule:

```bash
sudo cp udev/99-aula-f75max.rules /etc/udev/rules.d/
sudo udevadm control --reload-rules
sudo udevadm trigger
```

Then add yourself to the `plugdev` group if you aren't already:

```bash
sudo usermod -aG plugdev $USER
```

Log out and back in for the group change to take effect.

## Usage

### Upload a GIF

The GIF must be exactly 128x128 pixels and no more than 255 frames.
To resize an existing GIF:

```bash
gifsicle --resize 128x128 input.gif -o output.gif
```

Upload to slot 1 (overwrites any existing content in that slot):

```bash
aula-cli --gif animation.gif
```

Upload to a specific slot:

```bash
aula-cli --gif animation.gif --slot 2
```

### Sync Clock

Sync to system time:

```bash
aula-cli --time
```

Override specific fields:

```bash
aula-cli --time --hour 14 --minute 30
```

### Full Usage

```
Usage: aula-cli [command] [options]

Commands:
  --gif,    -g <file>   Upload a 128x128 GIF to the display
                        (also syncs time automatically)
  --time,   -t          Sync system time to keyboard
  --reset,  -r          Factory reset

Time override options (used with --time):
  --year,   -Y <n>      Override year
  --month,  -M <n>      Override month (1-12)
  --day,    -D <n>      Override day (1-31)
  --hour,   -h <n>      Override hour (0-23)
  --minute, -m <n>      Override minute (0-59)
  --second, -s <n>      Override second (0-59)
  --slot,   -S <n>      Display slot to write (1-255, default 1)
```

## How It Works

The F75 Max communicates over USB HID. The keyboard exposes four interfaces:

| Interface | Purpose |
|-----------|---------|
| 0 | Standard keyboard input (keypresses) |
| 1 | Media keys / knob |
| 2 | Display pixel data (interrupt transfers) |
| 3 | Command channel (HID SET_REPORT / GET_REPORT) |

The display protocol was reverse-engineered by capturing USB traffic with
Wireshark and usbmon while operating the official Epomaker software on Windows.

GIF frames are composited correctly — delta frames and transparency are handled
so animated GIFs display as intended.

## Credits

Protocol research for the display stream format and command structure was
greatly assisted by
[Aula-F75-Max-OSX](https://github.com/RoseWaveStudio/Aula-F75-Max-OSX)
by RoseWaveStudio, a macOS implementation of the same protocol.

## AI Disclosure

This program was developed with the assistance of
[Claude Sonnet 4.6](https://claude.ai) by Anthropic. The project was also a
learning exercise in USB HID protocol reverse engineering and low-level C
programming.

## License

MIT License — see [LICENSE](LICENSE) for details.

## Contributing

Issues and pull requests welcome. If you own an F75 Max and want to help
reverse-engineer the RGB lighting or key remapping protocol, captures of USB
traffic from the official software are the most useful contribution.
