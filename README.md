# HEIC to JPG Converter

A fast, no-install command-line tool that batch-converts iPhone `.heic` photos to `.jpg`. Multithreaded, works on Windows and macOS, and the downloads are single self-contained binaries — no libraries or runtimes to install.

## Download

Grab the latest release from the [**Releases page**](https://github.com/assekho2/Heic_to_Jpg_Converter/releases):

| Platform | File |
|---|---|
| Windows 10/11 (x64) | `heic_converter_mt-windows-x64.zip` |
| macOS (Apple Silicon) | `heic_converter_mt-macos-arm64.tar.gz` |

## Quick start

### Windows

1. Unzip the download — inside is a single `heic_converter_mt.exe`.
2. Open the folder containing your `.heic` photos in Explorer, type `cmd` in the address bar, and press Enter (opens a terminal in that folder).
3. Run the converter:

   ```
   C:\path\to\heic_converter_mt.exe
   ```

   Every `.heic` in the folder is converted; JPEGs appear in a new `output` subfolder.

> **"Windows protected your PC"?** SmartScreen warns about unsigned downloads. Click **More info → Run anyway**.

### macOS

```bash
tar -xzf heic_converter_mt-macos-arm64.tar.gz
xattr -d com.apple.quarantine ./heic_converter_mt   # required: tells macOS to allow the unsigned binary
cd /path/to/your/heic/photos
/path/to/heic_converter_mt
```

JPEGs appear in an `output` subfolder next to your photos.

## Usage

```
heic_converter_mt [options] [input_dir]

Converts every .heic file in input_dir (default: current directory)
to a .jpg in the output directory, using multiple threads.

Options:
  -o, --output <dir>   output directory (default: output)
  -q, --quality <n>    JPEG quality 1-100 (default: 90)
  -t, --threads <n>    worker threads (default: CPU cores)
  -h, --help           show this help and exit
  -V, --version        show version and exit
```

Examples:

```bash
heic_converter_mt ~/Pictures/vacation            # convert a specific folder
heic_converter_mt -q 85 -o converted             # quality 85, custom output folder
heic_converter_mt -t 4                           # limit to 4 threads
```

## Building from source

The repo contains two converters:

- `heic_converter_mt.cpp` — the multithreaded converter described above (this is what releases ship)
- `heic_converter.c` — a simpler single-threaded version

Both build on Windows, macOS, and Linux via the Makefile.

### Dependencies

- **macOS:** `brew install libheif jpeg`
- **Windows:** install [MSYS2](https://www.msys2.org), then in a UCRT64 shell:
  `pacman -S mingw-w64-ucrt-x86_64-gcc mingw-w64-ucrt-x86_64-libheif mingw-w64-ucrt-x86_64-libjpeg-turbo make`
- **Linux:** `sudo apt install build-essential libheif-dev libjpeg-dev`

### Build

```bash
make                  # both converters, linked against system libraries
make static           # portable heic_converter_mt with libheif/libde265/libjpeg
                      # statically linked (what CI builds for releases)
```

`make static` runs `scripts/build_static.sh`, which downloads and builds libheif (HEIC decode only) from source. It needs cmake/ninja/pkgconf on Windows (`pacman -S mingw-w64-ucrt-x86_64-cmake mingw-w64-ucrt-x86_64-ninja mingw-w64-ucrt-x86_64-pkgconf`) or `brew install cmake ninja jpeg-turbo` on macOS.

## Releases / CI

GitHub Actions builds Windows and macOS binaries on every push. Pushing a tag like `v1.1.0` builds, packages, and attaches the binaries to a GitHub Release automatically.
