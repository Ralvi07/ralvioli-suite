# Ralvioli Suite

Three native x64 Windows tools for HP-GL plot files.

- **Merger** places, rotates, and combines files within a specified paper width.
- **Viewer** previews, rotates, and crops files.
- **Plotter** queues HP-GL files, renders calibrated monochrome raster previews, and checks communication with an Algotex VEGA.

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

**Export raster** writes one PBM per queued file to the existing folder configured in `RasterOutput`. The output uses the selected data step, header option, range, paper width, and calibration. The default 300 dpi is for raster inspection. VEGA printing still requires the print-job handshake and raster encoding to be verified on a working device session.

Plotter settings default to `%LOCALAPPDATA%\Ralvioli Suite\Ralvioli Plotter.ini`. For a portable build, place the sample `RalvioliPlotter.ini` next to the executable as `Ralvioli Plotter.ini`. If no INI sets `RasterOutput`, raster export is unavailable.

In **Calibrate**, enter a test square side in centimeters and select **Export test square**. Save it as HP-GL/2 for a working print setup or as PBM for raster inspection. The export applies the factors currently shown in the dialog and fills both expected lengths in millimeters.
