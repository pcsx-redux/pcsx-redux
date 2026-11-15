# Openbios

[Openbios](https://github.com/pcsx-redux/nugget/tree/main/openbios) is, as its name implies, an open-source alternative to a retail PSX bios that can be non-trivial to dump.

## Purposes of Openbios

  * Educational
  * Ease of distribution
  * Automated testing

See [this page](https://github.com/pcsx-redux/nugget/tree/main/openbios) for more details.

## Building

It is not part of the default `pcsx-redux` build, and is compiled on its own, from the `src/mips/openbios` directory. On Linux and MacOS, the top level `Makefile` has an `openbios` target for it.  

See the corresponding sections in [Compiling](./compiling.md) for instructions.  

The result of the compilation should be a file called `openbios.elf` that contains all useful debugging symbols,  
and a file called `openbios.bin` which can be used in emulators or even burned to a chip and placed on a retail console.  
When the configured BIOS cannot be opened, PCSX-Redux falls back to OpenBIOS, and looks for `openbios.elf` before `openbios.bin`. Both load the same ROM, but the elf also brings in the BIOS symbols, so its functions are named in the debugger and the profiler. Setting the BIOS to `openbios.elf` directly works the same way.  

## Status

This subproject is fairly functional and usable. OpenBIOS does almost all the same things as the retail BIOS does when booting, and implements most of its features.  
Many games are booting and working properly with this code. Read the [compatibility](https://github.com/pcsx-redux/nugget/blob/main/openbios/COMPATIBILITY.md) document for more information.  
It can be used in emulators or on the real console, either while replacing the rom chip, or by using the "cart" build and programming the flash chip of a cheat cart with the result.

## Organization

The BIOS is split in two major parts: the low level code for the bios itself, and the shell, which is the binary that's being loaded into memory at boot time by the bios, to display the SONY sound and logo, and has a small utility menu for playing audio discs, or shuffling around memory cards.

While the first part is the main one that's being targeted here, the second one hasn't been reversed, and has been fully replaced with a completely different project. This may change in the future, but this isn't currently the focus of this project.

The original code was most likely chunked into several sub-projects, that were all linked together like a giant patchwork. This approach is less readable, and for this reason, we're not going to do this.  
However this will result in the ROM/RAM split to be less obvious, and slower at times than the original. Tuning of the hot functions is eventually required.

## Technicalities

The code has been rewritten based off the reverse engineering of a dump of the BIOS of an american **SCPH-7001** machine. *MD5sum: 1e68c231d0896b7eadcad1d7d8e76129*

The ghidra database for it is currently being hosted on a server, alongside a few other pieces of software being reversed. Contact one of the authors if you want access.

## Commentary

The retail PlayStation BIOS code is a constellation of bugs and bad design.  
The fact that the retail console boots at all is nothing short of a miracle. Half of the provided libc in the A0 table is buggy.  
The BIOS code is barely able to initialize the CD-Rom, and read the game's binary off of it to boot it; anything beyond that will be crippled with bugs.  
And this only is viable if you respect a very strict method to create your CD-Rom. The memory card and gamepad code is a steaming-hot heap of human excrement.  
The provided GPU stubs are inefficient at best.  

The only sane thing that any software running on the PlayStation ought to do is to immediately disable interrupts, grab the function pointer located at *0x00000310* for `FlushCache`,  
in order put it inside a wrapper that disables interrupts before calling it, and then trash the whole memory to install its own code.  
The only reason `FlushCache` is required from the retail code is because since the function will unplug the main memory bus off the CPU in order to work, it HAS to run from the *0xbfc* memory map, which will still be connected.  
Anything else from the retail code is virtually useless, and shouldn't be relied upon.  

## Legality

*Disclaimer: the author is not a lawyer, and the following statement hasn't been reviewed by a professional of the law, so the rest of this document cannot be taken as legal advice.* 

As explained above, this code has been written using disassembly and reverse engineering of a retail bios the author dumped from a second hand console. The same exact methodology was employed by Connectix for their PS1 bios. The conclusion of [their lawsuit](https://en.wikipedia.org/wiki/Sony_Computer_Entertainment,_Inc._v._Connectix_Corp.), and that of [Sega v. Accolade](https://en.wikipedia.org/wiki/Sega_Enterprises,_Ltd._v._Accolade,_Inc.) seems to indicate that this project here follows and is impacted by the same doctrine.
