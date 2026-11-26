# Ralvioli Suite

Three native x64 Windows tools for HP-GL plot files.

- **Merger** places, rotates, and combines files within a specified paper width.
- **Viewer** previews, rotates, and crops files.
- **Plotter** queues HP-GL files and prints them directly to an Algotex VEGA.

## Download

Download **Ralvioli Suite Setup.exe** from the [latest GitHub release](https://github.com/Ralvi07/ralvioli-suite/releases/latest). The Windows x64 installer includes Merger, Viewer, and Plotter.

## Build the installer from source

1. Clone this repository or extract the source ZIP from GitHub.
2. Install MSYS2. Open its **UCRT64** shell and run `pacman -S --needed mingw-w64-ucrt-x86_64-toolchain`.
3. Install Inno Setup 6.
4. Open Command Prompt in the source folder and run `BUILD_SUITE.bat`. The script builds all three apps and packages them as `build\Ralvioli Suite Setup.exe`.

Set `MSYS2_UCRT64_BIN` or `ISCC` first if your tools are in custom locations.

## Preview navigation

Drag with the middle mouse button at any zoom level. When zoomed in, hover over the arrows at the edges of the preview to pan. **Fit** restores the full drawing.

## Plotter

Direct VEGA printing uses the libusbK driver for USB device `135D:F001`. Suite Setup installs the x64 `libusbK.dll` with Plotter. Select **Install the Algotex VEGA USB driver** during setup to run the bundled driver wizard. For a portable build, use the DLL in `drivers/Algotex_USB_Plotter` next to the Plotter executable.

Use **Connect** before printing. **Print auto** previews the queue in print order and prints it one file at a time. **Configuration** contains paper settings, print direction, calibration, history, and test patterns. **Shift(mm)** adjusts the return pass in bidirectional printing. Calibration rectangles can also be printed.

Plotter settings default to `%LOCALAPPDATA%\Ralvioli Suite\Ralvioli Plotter.ini`. For a portable build, place the sample `RalvioliPlotter.ini` next to the executable as `Ralvioli Plotter.ini`.
