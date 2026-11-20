# Dalos

Dalos is a canvas of nodes wired together. Each link carries a file: a node reads the files on its inputs and hands a file to whatever is connected to its outputs. Chaining nodes lets you cut a slice out of a disc image or a file, decode it, and look at the result with the same viewers the ISO browser uses.

Open it with `Dalos -> Show Canvas` in the main menu.

## Using the canvas

- Right-click an empty spot of the canvas to open the menu that adds nodes.
- Drag the canvas background with any mouse button to pan the view.
- Drag a node by its title bar to move it. Right-clicking the title bar offers to delete the node, or to save it as a new node type.
- Drag from a node's output pin, on its right side, to another node's input pin, on its left side, to link them. Right-click an input pin to cut its link. A link that would close a loop is refused.

## Nodes

- **File** opens a file from disk. Type its path and press Enter.
- **Slice** takes part of its input: a start offset, in hexadecimal, and a size. A size of -1 takes everything up to the end.
- **Struct** reads fields out of its input at a given offset. Tick `Edit fields` to add, remove and type the fields: `u8`, `s8`, `u16`, `s16`, `u32`, `s32`, `u64`, `s64`, `f32`, `f64` and `char`. A count above 1 makes an array, and for `char` it is the string length. All values are read as little endian, like everything on the PS1. Click a field to send its bytes to the node's output.
- **Lua filter** runs a Lua chunk on its input. The chunk gets the input [file](../Lua/file-api.md) as its argument, and returns either a file or a string, which becomes a buffer. Press `Run` to run it again after editing the code. Errors are shown in the node.
- **Viewer** opens a viewer window on its input, with a tab for each way of looking at it: raw image, text, sound, and TIM when the data parses as one.

The ISO browser's `Send to Dalos` menu item adds a node holding the selected file or sectors of the disc image.

## Saving nodes

`Save as a new node...` stores a node's setup under a name, for instance a Struct describing a file format, or a Lua filter that decompresses something. Saved nodes appear in the add menu, and `Forget a saved node` removes them. They are kept in `dalos.json`, next to the emulator's `pcsx.json` settings file.
