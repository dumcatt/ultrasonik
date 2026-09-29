# Ultrasonik

Ultrasonik is a reimplementation of DirectSound built on top of ASIO, built with low-latency use cases in mind. This is currently very early alpha software which is missing large chunks of functionality. Patches welcome.

## Configuration

Ultrasonik loads its settings from a `ultrasonik.ini` file placed in the same directory as `dsound.dll`. If the file is not present, defaults are used.

### Default Settings

```ini
; Ultrasonik ASIO Configuration

; ASIO device name (leave empty for first available ASIO device)
device=

; Sample rate in Hz (default: 44100)
sample_rate=44100

; Bit depth: 16, 24, or 32 (default: 24)
; Note: the actual output format is determined by the ASIO driver's
; native format. This setting is used for the internal processing format
; reported to DirectSound clients.
bit_depth=24

; Buffer size in samples (default: 192, which is ~4ms at 44100Hz)
; Set to 0 to use the ASIO driver's preferred buffer size
; Common values:
;   64  = ~1.5ms (very low latency, may cause crackling)
;   128 = ~2.9ms
;   192 = ~4.4ms (recommended, matches beatmania IIDX INFINITAS)
;   256 = ~5.8ms
;   512 = ~11.6ms (safe fallback)
buffer_size=192
```

The defaults (24-bit Int24LSB at 44100Hz with ~4ms buffer) match the audio configuration used by modern beatmania IIDX (INFINITAS).

## Requirements

An ASIO-compatible sound card with appropriate drivers installed is required. Examples include:

- ASUS Xonar AE (used in arcade cabinets)
- Any professional audio interface with ASIO drivers
- ASIO4ALL (generic ASIO wrapper, not recommended for lowest latency)

## Building

This project uses the Meson build system, which in turn uses ninja to drive the compilation process (usually). If you are familiar with Autotools, then the commands `meson` and `ninja` act as the rough equivalents of the `./configure` and `make` commands from the procedure for compiling Autotools software packages.

To build this project on Windows, you will first need to install a recent version of MSYS2 and fully update it in accordance with the MSYS2 documentation. Once the base MSYS2 system is installed and fully updated, install the following packages inside the MSYS2 environment:

```
$ pacman -S mingw-w64-i686-meson mingw-w64-i686-toolchain
```

On Linux development hosts, you will need to install suitable equivalents using your package manager. The w64-mingw32 cross-compilation toolchain is required.

Once you have installed the necessary packages on your development platform, a debug build can be compiled as follows (omit `--cross w64-mingw32.txt` if compiling on MSYS2)

```
$ meson debug --cross w64-mingw32.txt
$ cd debug
$ ninja
```

A directory name other than `debug` can be passed to meson if desired.

Alternatively, to make a release build:

```
$ meson release --buildtype release --cross w64-mingw32.txt
$ cd release
$ ninja
```

A `src/dsound.dll` binary will be produced inside the respective build directory.

## License

This project is released under the terms of the MIT License.
