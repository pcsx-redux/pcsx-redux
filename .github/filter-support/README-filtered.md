# PCSX-Redux's Support & Tools

This repository is a read-only reduced mirror of
[the PCSX-Redux project](https://github.com/pcsx-redux/pcsx-redux). It only contains the necessary parts to build the [tools](https://github.com/pcsx-redux/pcsx-redux/tree/main/tools) only, as well as the [support libraries](https://github.com/pcsx-redux/pcsx-redux/tree/main/support).
Its purpose is to be used as a submodule for projects that want to use the tools and libraries
contained herein without bringing the whole of PCSX-Redux's codebase.

Building the tools is simply done using the `make` command. It is possible to install them on your system using `make install`. There is no build system set for the libraries as they are meant to be used as a pick-and-choose buffet.

Please consult [the upstream repository](https://github.com/pcsx-redux/pcsx-redux) and [its documentation](https://pcsx-redux.consoledev.net) for more information. There is also some documentation nested within the folders.

This repository will be updated periodically to match the upstream repository, and its history will be rewritten to remove all commits that are not related to the tools. While care is taken to try and ensure some sort of consistency, a simple `git pull` may not work to update it. Resetting to the remote HEAD is recommended when updating.

All of the code here is MIT-licensed. See the [LICENSE](LICENSE) file for more information.
