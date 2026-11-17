# Ralvioli Suite

Three native x64 Windows tools for HP-GL plot files.

- **Merger** places, rotates, and combines files within a specified paper width.
- **Viewer** previews, rotates, and crops files.
- **Plotter** prepares a print queue, applies paper calibration, and sends HP-GL/2 over USB. The status bar reports Windows USB detection, transfer progress, and errors.

## Build the installer

1. Install MSYS2 and run `pacman -S mingw-w64-ucrt-x86_64-toolchain` in its UCRT64 shell.
2. Install Inno Setup 6.
3. Run `BUILD_SUITE.bat` from Command Prompt. It builds all three applications and creates `build\Ralvioli Suite Setup.exe`.

Set `MSYS2_UCRT64_BIN` or `ISCC` first if your tools are in custom locations.

## Plotter

The default backend uses WinUSB for Algotex USB device `135D:F001`. Configure the Windows USB driver binding separately.

**Test USB** checks whether the app can open the plotter interface and reports its endpoints. Use **Calibrate** to enter expected and measured lengths for the feed and across-paper axes.

Plotter settings are stored in `%LOCALAPPDATA%\Ralvioli Suite\Ralvioli Plotter.ini` by default. A copy of `RalvioliPlotter.ini` named `Ralvioli Plotter.ini` alongside a portable build takes precedence. The sample INI also shows the dry-run and legacy libusbK options.

The status bar tracks host-side USB state and byte transfer. VEGA printing and printer-reported conditions such as paper and ink errors still require verification against a working Algotex USB session.
