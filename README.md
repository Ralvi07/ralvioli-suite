# Ralvioli Suite

Three native x64 Windows tools for HP-GL plot files.

- **Merger** places, rotates, and combines files within a specified paper width.
- **Viewer** previews, rotates, and crops files.
- **Plotter** queues HP-GL files, submits calibrated print jobs to Windows printer queues, and checks communication with an Algotex VEGA.

## Download

Download **Ralvioli Suite Setup.exe** from the [latest GitHub release](https://github.com/Ralvi07/ralvioli-suite/releases/latest). The Windows x64 installer includes Merger, Viewer, and Plotter.

## Build the installer from source

1. Clone this repository or extract the source ZIP from GitHub.
2. Install MSYS2. Open its **UCRT64** shell and run `pacman -S --needed mingw-w64-ucrt-x86_64-toolchain`.
3. Install Inno Setup 6.
4. Open Command Prompt in the source folder and run `BUILD_SUITE.bat`. The script builds all three apps and packages them as `build\Ralvioli Suite Setup.exe`.

Set `MSYS2_UCRT64_BIN` or `ISCC` first if your tools are in custom locations.

## Plotter

The default backend uses WinUSB for Algotex USB device `135D:F001`. Configure the Windows USB driver binding separately. The legacy libusbK backend requires the corresponding driver and an x64 libusbK DLL.

**Check plotter** sends the legacy status request and verifies the acknowledgment, reply header and checksum. It logs the endpoints and returned bytes alongside the active INI as `Ralvioli Plotter.usb.txt`. The legacy OCX encodes the status payload as `00 1A` (calls at `0x1000f746` and `0x1001a0c7`) and uses bulk OUT `0x01` and IN `0x81` (calls at `0x1001b2c0` and `0x1001b303`).

**Print** opens the Windows printer picker and submits the selected jobs to the chosen printer driver at physical size. The paper size selected in the driver must have a printable area large enough for the plot. Use **Calibrate → Print test rectangle** to print a test shape with independent width and feed length in centimeters. It uses the factors shown in the calibration dialog and fills the expected measurements in millimeters.

The Windows print path requires a working printer queue for the destination. A VEGA bound to WinUSB or libusbK is a USB interface and does not by itself provide a Windows printer queue. Direct VEGA USB printing still needs the head-specific raster encoding and job handshake. The old OCX writes 512-byte data blocks to endpoint `0x02` after checking endpoint `0x82` (functions at `0x100127c0`, `0x1001b320`, `0x1001b360`, and `0x1001b3a0`).

Plotter settings default to `%LOCALAPPDATA%\Ralvioli Suite\Ralvioli Plotter.ini`. For a portable build, place the sample `RalvioliPlotter.ini` next to the executable as `Ralvioli Plotter.ini`.
