# Ralvioli Suite

Three native x64 Windows tools for HP-GL plot files.

- **Merger** places, rotates, and combines files within a specified paper width.
- **Viewer** previews, rotates, and crops files.
- **Plotter** prepares a print queue, applies paper calibration, and sends HP-GL/2 over USB. The status bar reports Windows USB detection, transfer progress, and errors.

## Download

Download **Ralvioli Suite Setup.exe** from the [latest GitHub release](https://github.com/Ralvi07/ralvioli-suite/releases/latest). The Windows x64 installer includes Merger, Viewer, and Plotter.

## Build the installer from source

1. Clone this repository or extract the source ZIP from GitHub.
2. Install MSYS2. Open its **UCRT64** shell and run `pacman -S --needed mingw-w64-ucrt-x86_64-toolchain`.
3. Install Inno Setup 6.
4. Open Command Prompt in the source folder and run `BUILD_SUITE.bat`. The script builds all three apps and packages them as `build\Ralvioli Suite Setup.exe`.

Set `MSYS2_UCRT64_BIN` or `ISCC` first if your tools are in custom locations.

## Plotter

The default backend uses WinUSB for Algotex USB device `135D:F001`. Configure the Windows USB driver binding separately.

**Test USB** checks whether the app can open the plotter interface and reports its endpoints. Use **Calibrate** to enter expected and measured lengths for the feed and across-paper axes.

Plotter settings are stored in `%LOCALAPPDATA%\Ralvioli Suite\Ralvioli Plotter.ini` by default. A copy of `RalvioliPlotter.ini` named `Ralvioli Plotter.ini` alongside a portable build takes precedence. The sample INI also shows the dry-run and legacy libusbK options.

The status bar tracks USB detection and byte transfers and displays Windows error codes when available. VEGA printing and printer-reported conditions such as paper and ink errors still require verification against a working Algotex USB session.
