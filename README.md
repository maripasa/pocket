# board

Clone `pico-sdk`:

```bash
git clone https://github.com/raspberrypi/pico-sdk.git
```

Load the SDK submodules:

```bash
cd pico-sdk
git submodule update --init --recursive
cd ..
```

Build:

```bash
cmake -S . -B build
cmake --build build
```

Run and flash:

```bash
./run.sh pico
```

Or:

```bash
./run.sh pico_w
```
