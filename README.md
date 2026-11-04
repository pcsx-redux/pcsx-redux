![Debugger screenshot](https://pcsx-redux.consoledev.net/images/debugger1.png)

<a href='https://ko-fi.com/nicolasnoble' target='_blank'><img height='35' style='border:0px;height:46px;' src='https://github.githubassets.com/assets/ko_fi-53a60c17e75c.svg' border='0' alt='Buy Me a Coffee at ko-fi.com' />Buy Me a Coffee at ko-fi.com</a>

|Platform|Build status|Download|
|--------|------------|--------|
|Windows x64 build|[![Windows x64 CI](https://dev.azure.com/grumpycoders/pcsx-redux/_apis/build/status/pcsx-redux%20Windows%20build?branchName=main)](https://dev.azure.com/grumpycoders/pcsx-redux/_build/latest?definitionId=1&branchName=main)|[Windows Intel 64-bits](https://distrib.app/pub/org/pcsx-redux/project/dev-win-x64)|
|Linux x64 build|[![Linux x64 CI](https://github.com/pcsx-redux/pcsx-redux/workflows/Linux%20CI/badge.svg?branch=main)](https://github.com/pcsx-redux/pcsx-redux/actions?query=workflow%3A%22Linux+CI%22+branch%3Amain)|[Linux Intel 64-bits (AppImage)](https://distrib.app/pub/org/pcsx-redux/project/dev-linux-x64)|
|Linux arm64 build|[![Linux arm64 CI](https://github.com/pcsx-redux/pcsx-redux/workflows/Linux%20CI/badge.svg?branch=main)](https://github.com/pcsx-redux/pcsx-redux/actions?query=workflow%3A%22Linux+CI%22+branch%3Amain)|[Linux Arm 64-bits (AppImage)](https://distrib.app/pub/org/pcsx-redux/project/dev-linux-arm64)|
|macOS build|[![macOS CI](https://github.com/pcsx-redux/pcsx-redux/workflows/macOS%20CI/badge.svg?branch=main)](https://github.com/pcsx-redux/pcsx-redux/actions?query=workflow%3A%22macOS+CI%22+branch%3Amain)|[macOS Arm](https://distrib.app/pub/org/pcsx-redux/project/dev-macos-arm)|
|Browser build|[![xmake build](https://github.com/pcsx-redux/pcsx-redux/actions/workflows/xmake-build.yml/badge.svg?branch=main)](https://github.com/pcsx-redux/pcsx-redux/actions/workflows/xmake-build.yml?query=branch%3Amain)|[Run in the browser (experimental)](https://tools.psx.dev/redux/)|

To discuss this emulator specifically, please join our Discord server:

[![Discord](https://discord.com/api/guilds/567975889879695361/widget.png?style=banner2)](https://discord.gg/KG5uCqw)

To discuss PlayStation 1 development, hacking, and reverse engineering in general, please join the PSX.Dev Discord server:

[![Discord](https://discord.com/api/guilds/642647820683444236/widget.png?style=banner2)](https://discord.gg/QByKPpH)

# PCSX-Redux

## What?
The PCSX-Redux project is a collection of tools, research, hardware design, and libraries aiming at development and reverse engineering on the PlayStation 1. The core product itself, PCSX-Redux, is yet another fork of the Playstation emulator, PCSX. While the work here is very much in progress, the goal is roughly the following:

 - Bring the codebase to more up to date code standards.
 - Get rid of the plugin system and create a single monolithic codebase that handles all aspects of the playstation emulation.
 - Write everything on top of OpenGL3+/ImGui for portability and readability.
 - Improve the debugging experience.
 - Improve the rendering experience.

Please consult [the documentation pages](https://pcsx-redux.consoledev.net) for more information on the emulator itself.

The [tools](tools) directory contains a few tools that can be used to work with PlayStation 1 software. The [wiki](https://github.com/pcsx-redux/pcsx-redux/wiki) directory contains a few research information that are not directly related to the emulator itself. The [mips](src/mips) directory contains various pieces of code targeting the PlayStation 1, and importantly, the OpenBIOS project, which is a MIPS R3000A BIOS implementation that can be used to boot PlayStation 1 games without the need for a retail BIOS. And the [hardware](hardware) directory contains various hardware designs that can be used with the PlayStation 1.

## Where?
|Download page|
|--------|
|[Windows Intel 64-bits](https://distrib.app/pub/org/pcsx-redux/project/dev-win-x64)|
|[Windows Intel 64-bits CLI](https://distrib.app/pub/org/pcsx-redux/project/dev-win-cli-x64)|
|[Linux Intel 64-bits (AppImage)](https://distrib.app/pub/org/pcsx-redux/project/dev-linux-x64)|
|[Linux Arm 64-bits (AppImage)](https://distrib.app/pub/org/pcsx-redux/project/dev-linux-arm64)|
|[MacOS Arm](https://distrib.app/pub/org/pcsx-redux/project/dev-macos-arm)|
|[Browser (WebAssembly, experimental)](https://tools.psx.dev/redux/)|

All native builds are 64-bit only, and need a GPU and driver that can create at least an OpenGL 3.0 context. The emulator asks for a 3.3 core profile first and falls back to 3.0 if that fails.

The browser build is highly experimental and not really functional yet: expect missing features and breakage. It is redeployed from each push to main whose wasm build succeeds. It runs the CPU interpreter only and boots OpenBIOS, and it needs WebGL2 and SharedArrayBuffer. The first visit reloads the page once to enable SharedArrayBuffer.

The Linux AppImages are built on Ubuntu 24.04 and need a distribution at least that recent. On older ones, the Windows build runs under Wine 11: replace `pcsx-redux.exe`, the launcher that checks for the Visual C++ runtime, with `pcsx-redux.main` renamed to `pcsx-redux.exe`. The Windows build is x86-64, so on an ARM64 host Wine needs an x86-64 emulation layer such as Box64 or FEX under it.

### Note:
The macOS version of PCSX-Redux is not currently signed with a developer certificate so you may see a security warning when opening the application.

If you see a dialog with the message:

> “PCSX-Redux” can’t be opened because Apple cannot check it for malicious software.

You can allow the application to run by doing either of the following. You only need to do this once for freshly downloaded copies of PCSX-Redux.

- Right-click the app and select Open. This signals an explicit decision on your part that you really want to open it, and adds a new "Open" button to the security warning dialog.

- Alternatively, go to macOS Settings > Privacy & Security. Scroll down to see a section that will let you allow the app.

## How?
The code is meant to be built using very modern compilers. Also it's still fairly experimental, and lots of things can break. If you still want to proceed, here are instructions to build it on Linux, MacOS and Windows. The code now comes in two big parts: the emulator itself, and [OpenBIOS](https://github.com/pcsx-redux/nugget/tree/main/openbios), which can be used as an alternative to the retail, copyright protected BIOS.

### Getting sources
The only location for the source is [on GitHub](https://github.com/pcsx-redux/pcsx-redux/).
Clone recursively, as the project uses submodules:
```
git clone --recursive https://github.com/pcsx-redux/pcsx-redux.git
```

### Windows
Install [Visual Studio 2022 Community Edition](https://visualstudio.microsoft.com/thank-you-downloading-visual-studio/?sku=Community&rel=16) using the `Desktop development with C++` workload. Open the file `vsprojects\pcsx-redux.sln`, select `pcsx-redux -> pcsx-redux`, right click, `Set as Startup Project`, and hit `F7` to build. The project follows the open-and-build paradigm with no extra step, so no specific dependency ought to be needed, as [NuGet](https://www.nuget.org/) will take care of downloading them automatically for you on the first build.

Note: If you get an error saying `hresult e_fail has been returned from a call to a com component`, you might need to delete the .suo file in vsproject/vs, restart Visual Studio and retry.

If you want to compile OpenBIOS or other PS1 software, you need to install a MIPS toolchain on Windows. You can do this by copy-pasting the following into a command prompt:

```powershell
powershell -c "& { iwr -UseBasicParsing https://bit.ly/mips-ps1 | iex }"
```

Then, open a new command prompt, and type the following:

```
mips install 16.2.0
```

To manually install this script, you can download it from [here](https://bit.ly/mips-ps1), and then install it with the following command:

```powershell
powershell -ExecutionPolicy Unrestricted -File mips.ps1 self-install C:\path\to\destination
```

You can leave the installation path blank to install the script in the Application Data folder.

Once the toolchain is installed, you can compile OpenBIOS using `make -C src/mips/openbios`.

### Linux
Run `./dockermake.sh appimage`. You need [docker](https://en.wikipedia.org/wiki/Docker_(software)) for this to work. This will create an [AppImage](https://appimage.org/) file, called `PCSX-Redux-HEAD-x86_64.AppImage` or `PCSX-Redux-HEAD-aarch64.AppImage` depending on your machine, which a self-hosted binary containing all of its required dependencies. Alternatively, if you do not want to use Docker, you can also simply install the dependencies listed below and run `make`.

#### GNU/Linux Dependencies

If you're only interested in compiling psx code, you can simply clone the pcsx-redux repo, then build the mips toolchain with [this script](https://github.com/pcsx-redux/pcsx-redux/blob/main/tools/linux-mips/spawn-compiler.sh), then follow the instructions in `/pcsx-redux/src/mips/psyq/README.md` to convert the PsyQ libraries. You might find them pre-compiled online.

 - Debian derivatives ( for full emulator compilation ):

```bash
sudo apt-get install -y build-essential git make pkg-config clang g++ libcapstone-dev libfreetype-dev libavcodec-dev libavformat-dev libavutil-dev libcurl4-openssl-dev libsdl3-dev libswresample-dev libuv1-dev zlib1g-dev
```

The mips toolchain isn't packaged by Debian or Ubuntu, so build it from source:

```bash
sudo apt-get install -y wget bzip2 xz-utils bison flex texinfo libgmp-dev libmpfr-dev libmpc-dev
sudo bash tools/linux-mips/spawn-compiler.sh
```

 - Arch derivatives :

The `pcsx-redux-git` package can be installed from the AUR using your AUR helper of choice (e.g., paru):

```bash
paru -S pcsx-redux-git
```

Alternatively, the following steps describe how to install dependencies and compile manually:

```bash
sudo pacman -S --needed capstone clang git make pkg-config ffmpeg libuv zlib sdl3 curl xorg-server-xvfb imagemagick
```
The mipsel environment can be installed from [AUR](https://wiki.archlinux.org/index.php/Aur) : [mipsel-none-elf-binutils](https://aur.archlinux.org/packages/mipsel-none-elf-binutils/) and [mipsel-none-elf-gcc](https://aur.archlinux.org/packages/mipsel-none-elf-gcc/) using your [AURhelper](https://wiki.archlinux.org/index.php/AUR_helpers) of choice:

```bash
trizen -S mipsel-none-elf-binutils mipsel-none-elf-gcc
```
You can then just enter the 'pcsx-redux' directory and compile without using docker with `make`.

Building OpenBIOS on Linux can be done with `./dockermake.sh -C src/mips/openbios`, or with `make -C src/mips/openbios` once you have a mipsel-none-elf toolchain. If you have a different mips compiler, you'll need to override some variables, such as `PREFIX` and `FORMAT`.

### MacOS
You need MacOS Catalina or later with the latest XCode to build, as well as a few [homebrew](https://brew.sh/) packages. Run the [brew installation script](https://github.com/pcsx-redux/pcsx-redux/blob/main/.github/scripts/install-brew-dependencies.sh) to get all the necessary dependencies. Simply run `make` to build.

Compiling OpenBIOS will require a mips compiler, which you can install from our [Homebrew tap](https://github.com/pcsx-redux/homebrew-mips):
```bash
brew install pcsx-redux/mips/mipsel-none-elf-gcc
```

Then, you can compile OpenBIOS using `make -C ./src/mips/openbios`.

## Who?
I used to contribute to the PCSX codebase. It is very likely that a sourceforge account of mine still has write access to the old cvs repository for PCSX. A long time ago, I contributed the telnet debugger, and the parallel port support. This means I am fairly familiar with this codebase, and I am also ashamed of the contributions I have done 15+ years ago, as one should.

Since the inception of this codebase, several people have contributed to it. Please refer to the [AUTHORS](AUTHORS) file for a (hopefully) exhaustive list.

## Why?
When Sony released the Playstation Classic recently, I came to realize two things: first, the state of the Playstation emulation isn't that great, and second, the only half-decent debugging tool still available for this console is that old telnet debugger I wrote eons ago, while other emulators out there for other consoles gained a lot of debugging superpowers. I think it was time for the Playstation emulation to get to better standards with regards to debuggability. I also felt I had a responsability to cleaning up some of the horrors I've introduced myself in the codebase long ago, and that made me cry a little looking at them. Hopefully, I got better at programming. Hopefully.

## Status?
The codebase still requires a lot of cleanup, and while the product is usable in its current state and lots can be achieved with it, there is still ways to go for reaching the first stable release. If you want to help with localization, you can find the translation project [on transifex](https://explore.transifex.com/grumpycoders/pcsx-redux/).

### What works?
- Dynamic Recompiler (x86-64, experimental arm64 support)
- interpreted CPU
- software GPU
- OpenGL GPU (highly experimental, still in active development)
- support for visual enhancements:
  - Linear and nearest neighbour filtering (Software and OpenGL renderer)
  - Rendering in true colour (Results in better colours, OpenGL renderer only)
  - Antialiasing (Implemented via MSAA, OpenGL renderer only)
  - Ability to optionally render polygons as wireframe or as vertices (Helps with debugging, OpenGL renderer only)
  - Hopefully more to come in the feature, such as upscaling, integer scaling, PGXP, and more
- VRAM viewer and debugger
- fully featured MIPS debugger
- memory cards
- memory card manager
- XBox controller support
- digital and analog controller emulation, Playstation Mouse emulation
- Lua scripting
- SPU debugger
- in-app shader editor and built-in crt-lottes shader
- customizable UI
- save states

### What still requires some work?
- fix remaining OpenGL renderer bugs (No mask bit emulation, missing commands, etc) and add more graphics enhancements
- proper SPU multithreaded code
- better customization
- more generic dynarec
- hook more emulator functions to Lua
- see [TODO](TODO.md) for more

[![Redux definition](https://pbs.twimg.com/media/ENJhNwGWwAEbrGb?format=jpg)](https://twitter.com/MerriamWebster/status/1212357808026341376)
