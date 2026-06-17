# Pocket 0.0.1 (Digital Riff) ABAVEH LABS


## Setup

Clone the Pico SDK inside this project:
```bash
git clone https://github.com/raspberrypi/pico-sdk.git
cd pico-sdk
git submodule update --init --recursive
cd ..
```

Build and flash for pico:

```bash
./run.sh pico
```

Manual build:

```bash
cmake -S . -B build -DPICO_BOARD=pico
cmake --build build
picotool load build/board.uf2 -xv
# Or move to RPI/RF2, if you don't have picotool
```

## References

- Pico SDK: https://github.com/raspberrypi/pico-sdk
- Pico examples: https://github.com/raspberrypi/pico-examples
- Pico C/C++ SDK documentation: https://www.raspberrypi.com/documentation/microcontrollers/c_sdk.html
- Raspberry Pi Pico documentation: https://www.raspberrypi.com/documentation/microcontrollers/pico-series.html
- PCM1808 datasheet: https://www.ti.com/lit/ds/symlink/pcm1808.pdf
- PCM5102 datasheet: https://www.ti.com/lit/ds/symlink/pcm5102.pdf
