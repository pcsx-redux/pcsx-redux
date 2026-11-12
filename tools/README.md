# Tools

This directory contains tools designed to help with the PCSX-Redux project somehow. The top level, directly usable tools are:

* [exe2elf](exe2elf) - Converts a PS-EXE executable to an ELF file, which can be useful for loading and debugging through gdb.
* [exe2exe](exe2exe) - Converts a PlayStation executable to a PS-EXE executable, normalizing the binary in the process.
* [exe2iso](exe2iso) - Converts a PS-EXE executable to a bootable ISO file containing a single PSX.EXE, conformant to the ISO9660 standard and bootable on a retail PlayStation 1.
* [exe2sys573](exe2sys573) - Converts a PS-EXE executable to a Konami System 573 flash image, bootable from either the onboard flash or a PCMCIA flash card.
* [ghidra_scripts](ghidra_scripts) - A collection of Ghidra scripts that can be used to integrate some parts of PCSX-Redux into Ghidra and vice versa.
* [ps1-packer](ps1-packer) - A tool for compressing PlayStation 1 executables into a single self-decompressing binary in various formats.
* [psyq-obj-parser](psyq-obj-parser) - A tool for parsing the object files produced by the Psy-Q SDK, and converting them to ELF files.

For the tools that need to be built, the top level Makefile can be used to build them all using the `tools` target. On Windows, the tools are present within the PCSX-Redux solution file in the `vsprojects` folder.

The scripts that build the MIPS cross-compiler, for Linux and Windows, live in [nugget](https://github.com/pcsx-redux/nugget/tree/main/tools), checked out here as [src/mips/tools](../src/mips/tools). On macOS, use the [Homebrew tap](https://github.com/pcsx-redux/homebrew-mips) instead.

The rest of the folders are the sources for some other internal tools that are not directly usable here, but published to other platforms.
