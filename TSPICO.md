# Fuse with a TS-Pico

This is [Fuse](https://sourceforge.net/projects/fuse-emulator/), the Free Unix
Spectrum Emulator, with a TS-2068 that has a
[TS-Pico](https://github.com/timex-sinclair-projects/tspico-firmware-build)
plugged in. `master` follows upstream Fuse; the TS-Pico work is on
`tspico-device`.

The TS-Pico itself isn't emulated here. Every access to ports 0Eh/0Fh goes to
`pico_host`, which runs the real TS-Pico firmware with its SD card in a folder
on your computer.

What `tspico-device` adds:

- **16K EXROM.** The TS-2068's EXROM can be 16K as well as 8K. The TS-Pico ROM
  is 32K: a 16K HOME ROM and a 16K EXROM.
- **The TS-Pico interface** (`peripherals/tspico.c`), TS-2068 only: version 1
  of
  [the bridge spec](https://github.com/timex-sinclair-projects/tspico-firmware-build/blob/main/docs/EMULATOR_BRIDGE.md).

## Running it

1. Split the TS-Pico ROM (in tspico-firmware-build's [`src/rom/`](https://github.com/timex-sinclair-projects/tspico-firmware-build/tree/main/src/rom);
   `TSPICO-23.ROM`, ROM 2.3, at the time of writing; use it with the `pico_host` from the
   same release) into its two halves:

       head -c 16384 TSPICO-23.ROM > tspico-home.rom
       tail -c 16384 TSPICO-23.ROM > tspico-exrom.rom

2. Start `pico_host`. `pico_host` comes with each [TS-Pico firmware release](https://github.com/timex-sinclair-projects/tspico-firmware-build/releases/latest):
   `pico_host-macos-arm64.zip`, `pico_host-windows-x86_64.zip` or
   `pico_host-linux-x86_64.zip`. Unzip it and run it; it keeps the Pico's flash
   and SD card in `~/TS-Pico-emulator`. (Or run `python3 tools/emu/pico_host.py`
   in a checkout of tspico-firmware-build.)
3. Start Fuse:

       fuse --machine ts2068 --rom-ts2068-0 tspico-home.rom \
            --rom-ts2068-1 tspico-exrom.rom --tspico

`--tspico` is also the "TS-Pico interface" checkbox in Options, Peripherals,
General. Fuse finds `pico_host` at `tcp:127.0.0.1:2068`. To use another
address, set `--tspico-bridge` or `$TSPICO_BRIDGE` to `tcp:HOST:PORT`, or
`unix:PATH` (not on Windows). Without `pico_host`, the 2068 runs as if no
TS-Pico were plugged in.

## Building

As upstream: `./autogen.sh && ./configure && make`. It needs libspectrum 1.7.0
or later, from upstream. The TS-Pico interface is built whenever sockets are
available.
