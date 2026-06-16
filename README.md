# Windows ODP Drivers
This repository contains Windows drivers that demonstrate and enable Open Device Platform (ODP) scenarios.

## Code Organization
Code is organized by platform under the drivers directory.
Place platform-specific drivers in their platform folder, and place shared drivers in a common folder when applicable.

## GitHub Workflows

`.github/workflows/build-drivers.yml` - Builds all drivers and publishes build artifacts.

## Compiling Code Locally
Build drivers locally with MSBuild from an Enterprise WDK (EWDK) command prompt.

### 1. Get the EWDK
1. Open the Microsoft WDK download page and download the Enterprise WDK ISO that matches your target SDK/WDK release.
2. Mount the ISO in Windows Explorer.
3. Open the mounted drive and run LaunchBuildEnv.cmd.
4. In the build environment prompt, go to your repo root:

   `cd /d D:\git\odp-windows-drivers`

Notes:
- Build from the EWDK prompt so INCLUDE, LIB, PATH, and toolchain variables are already configured.
- If your EWDK shell opens in x86 mode, switch to the x64 build environment before compiling drivers.

### 2. Restore NuGet packages
Run restore for each project before building:

   `nuget restore drivers\qemu\pl061gpio\pl061gpio.vcxproj -ConfigFile %APPDATA%\NuGet\NuGet.Config`

### 3. Build each driver project
Build using the same configuration used by CI (Release + ARM64):

   `msbuild drivers\qemu\pl061gpio\pl061gpio.vcxproj /p:Configuration=Release /p:Platform=ARM64 /p:WarningLevel=4`

### 4. Optional: Build all driver vcxproj files recursively
If new drivers are added, you can build all project files under drivers in one loop from the EWDK prompt:

```
   for /r drivers %F in (*.vcxproj) do (
     nuget restore "%F" -ConfigFile %APPDATA%\NuGet\NuGet.Config
     msbuild "%F" /p:Configuration=Release /p:Platform=ARM64 /p:WarningLevel=4
   )
```

### 5. Build output
Driver binaries (.sys) are generated in each project output folder under drivers.

