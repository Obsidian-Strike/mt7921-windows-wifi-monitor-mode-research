# MT7921 Monitor Mode Filter Driver — Build Guide

## Requirements

1. Visual Studio 2022 (Community is free)
   https://visualstudio.microsoft.com/

2. Windows Driver Kit (WDK) for Windows 11
   https://learn.microsoft.com/en-us/windows-hardware/drivers/download-the-wdk

3. Enable Test Signing (run once as Administrator):
   bcdedit /set testsigning on
   bcdedit /set nointegritychecks on
   → Reboot

## Build Steps

### Option A: Visual Studio (Recommended)

1. Open Visual Studio 2022
2. File → New → Project → "Empty WDM Driver" (search for WDM)
3. Add all .c and .h files from this folder to the project
4. Project Properties:
   - Configuration Type: Driver (.sys)
   - Target Platform: Windows 10 / 11 (x64)
   - NDIS Version: 6.85
   - Additional Dependencies: ndis.lib, wdm.lib
   - Additional Include Directories: $(DDK_INC_PATH)
5. Build → Build Solution (Ctrl+Shift+B)

### Option B: Command Line (WDK)

Open "Developer Command Prompt for VS 2022":
   msbuild src\mt7921mon.vcxproj /p:Configuration=Release /p:Platform=x64

## Install Driver

After build, from an Administrator command prompt:

   devcon install src\mt7921mon.inf ms_mt7921mon

Or using netcfg:
   netcfg -l src\mt7921mon.inf -c s -i ms_mt7921mon

## Verify Installation

   sc query mt7921mon
   netsh wlan show interfaces

If loaded correctly, monitor mode is now available via \\.\mt7921mon

## Uninstall

   netcfg -u ms_mt7921mon
   sc delete mt7921mon

## Test Signing

To check if test signing is active:
   bcdedit | findstr testsigning

To disable after testing:
   bcdedit /set testsigning off
   (requires reboot)
