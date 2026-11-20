# Rendering

PCSX-Redux is entirely running as an OpenGL3 application. All of its
aspects, including the UI elements, are rendered using OpenGL
primitives. This means there is very little boundaries between the
various rendered elements on the screen.

The rendering of the UI is done through [ImGui](https://github.com/ocornut/imgui), and a chunk of its API is
bound to Lua using [bindings](https://github.com/grumpycoders/pcsx-redux/tree/main/third_party/imgui_lua_bindings).

A good portion of the OpenGL3 API is also bound to Lua, as well as the
[ThorVG library](https://github.com/thorvg/thorvg).

## Emulated GPU rendering pipeline

The content of the Output region is rendered in two steps. The first
step is called the "Offscreen rendering", and is done during the
emulated GPU vsyncs. Its job is to flush the contents of the VRAM
texture to an offscreen texture, which may be of a different
resolution. The resolution of the offscreen texture should be pixel
perfect with that of the Output region. By default, the associated
shader with this operation should only do a simple copy and
interpolation, but as the first stage of the rendering pipeline, this
can be used for some first pass output effect such as the first pass of
a crt shader.

The second step is called the "Output rendering", and is done every
time the UI wants to refresh its display, which may or may not be at
the same time as the emulated vsync. The resolution of the Output region will
match exactly the resolution of the input texture, and the default
shader should simply copy all the texels without any sort of
interpolation, but as the second stage of the rendering pipeline, this
can still be used for the second pass output effect.

The [crt-lottes](https://github.com/grumpycoders/pcsx-redux/blob/main/src/gui/shaders)
implementation leverages these two passes to do the full CRT-like
output.

## Shader editor

The shader editor is a simple text editor that allows to edit the
shader code. It is not a full IDE, and it is not meant to be. Its
point is to do quick iterations on the shader code, and to be able to
see the result of the changes in real time.

The shader editor is split in 3 regions:

  - The left tab is the vertex shader code. It is technically editable,
  but there shouldn't be much reason to edit it.

  - The middle tab is the fragment shader code. This is the main shader
  code. It is editable, and the changes will be reflected in real time.

  - The right tab is the Lua invoker code. This is the code that will
  be executed under multiple circumstances. It is editable, and the
  changes will be reflected in real time.

The Lua invoker code will be compiled and executed in a soft sandbox
environment. The code can still access already created globals and mutate them,
but any newly created global will be kept within the sandbox and won't be
accessible from other Lua code. When the `Draw` function returns true, all these
globals will be saved to a JSON file, and they will be restored the next time
the shader is compiled.

When the shaders are compiled, the Vertex and Fragment shader code will be
compiled together, and if the resulting program is valid, the Lua invoker code
will be compiled and executed. If the Lua code fails to compile or execute, the
error will be displayed in the shader editor, but the shader program itself
will still be used.

This compilation order allows the Lua code to access the shader program
uniforms, and to set them up as needed. The global `shaderProgramID` will be
available to the Lua code, and will contain the ID of the shader program.

The code is expected to export a few functions:

  - `Draw`, which will be called periodically within the ImGui context,
  allowing to draw UI elements. The global `configureme` will be set to true
  when the user selects the "Configure Shaders" menu item. This allows to
  display a configuration UI to the user during this function call.

  - `Image(textureID, srcSizeX, srcSizeY, dstSizeX, dstSizeY)`, which will be
  called periodically within the ImGui context, when the emulator needs to draw the texture
  `textureID` at the given size. The texture ID is the OpenGL texture ID, and
  the size is in pixels. The code is at best expected to do a simple call
  to `imgui.Image(textureID, dstSizeX, dstSizeY, 0, 0, 1, 1)` to draw the
  texture. For the Emulated GPU Pipeline, this function will only be called on
  the Output shader, when being drawn to the Output region. As the function will be called
  during the ImGui context, it can capture certain ImGui state, such as the
  current ImGui cursor position, and use it to draw additional UI elements.
  Note that as with any normal ImGui function, this isn't the moment when the
  UI elements are actually drawn, but rather when the UI elements are queued
  to be drawn, meaning this isn't when the shader program will be executed,
  which is the point of the next function.

  - `BindAttributes(textureID, shaderProgramID, srcLocX, srcLocY, srcSizeX, srcSizeY, dstSizeX, dstSizeY)`
  will be called when the shader program is about to be executed, and needs
  to bind the attributes. The texture ID is the OpenGL texture ID, and the
  shader program ID is the OpenGL shader program ID. The location and sizes are only
  passed for the Emulated GPU Pipeline, when the Offscreen shader is being
  executed, as it needs to grab a portion of the VRAM texture to be rendered
  to the offscreen texture. The source location and size are normalized texture
  coordinates within the VRAM texture, while the destination size is in pixels.

Additionally, it is possible to programmatically set the content of the editors using the following methods:

```lua
PCSX.GUI.OffscreenShader.setDefaults()
PCSX.GUI.OffscreenShader.setTextVS(text)
PCSX.GUI.OffscreenShader.setTextPS(text)
PCSX.GUI.OffscreenShader.setTextL(text)
PCSX.GUI.OutputShader.setDefaults()
PCSX.GUI.OutputShader.setTextVS(text)
PCSX.GUI.OutputShader.setTextPS(text)
PCSX.GUI.OutputShader.setTextL(text)
```

The `setDefaults` method will set the default shader code, and the `setText*` methods will set the
shader code to the given string. The `text` argument can be either an actual string, or a [`File` object](file-api.md).

## ImGui

The ImGui API is bound to Lua, and can be used to draw UI elements. The
ImGui API is documented on the [ImGui source code](https://github.com/ocornut/imgui/blob/docking/imgui.h).
There is also an [interactive manual available](https://pthom.github.io/imgui_manual_online/manual/imgui_manual.html).

Not all functions are necessarily bound to Lua, and one can check the
[bindings code](https://github.com/grumpycoders/pcsx-redux/blob/main/third_party/imgui_lua_bindings/imgui_iterator.inl)
to see which functions are bound, and why some functions are not bound.

The main reason for not binding a function is that its arguments or return
values are not trivial to bind. For example, the `ImGui::Text` C++ function is not
bound, as it takes a variadic number of arguments, which is not possible to
bind in Lua easily. Instead, the `ImGui::TextUnformatted` C++ function is bound, which
takes a single string argument.

The emulator will periodically try to call the global function `DrawImguiFrame` with no
arguments. If the function is not defined, nothing will happen. If the function
fails to execute, it will be removed from the global environment, and the
emulator will stop trying to call it until a new global is defined.

The `DrawImguiFrame` function is expected to call the `imgui.Begin` function
to create a new ImGui window, as there is no default window created by the
emulator for the Lua context. The `DrawImguiFrame` function is also expected
to call the `imgui.End` function as normal with the ImGui API.

Some extra functions are bound to Lua beyond the API listed above:

  - `imgui.extra.ImVec2.New(x, y)` will create a new FFI `ImVec2` object. The `ImVec2`
  object is a simple struct with two fields, `x` and `y`. The `New` function
  takes two optional arguments, the `x` and `y` values, and returns the new `ImVec2`
  object.

  - `imgui.extra.getCurrentViewportId()` will return the current viewport ID.
  Viewports in ImGui are a way to split the ImGui context into multiple
  independent contexts, and the viewport ID is a unique identifier for each
  viewport. Basically, each viewport is a physical window from the operating
  system, and it can contain one or more ImGui windows.

  - `imgui.extra.InputTextMultiline(label, text, [width, height, [flags]])` draws a
  multi-line text box over a Lua string. It returns whether the text changed and
  the new text. `width` and `height` default to 0, which lets ImGui pick a size; a
  negative value fills the available space minus that amount. `flags` are
  `ImGuiInputTextFlags`.

  - `imgui.extra.getViewportFlags(id)` will return the viewport flags for the
  specified viewport. The viewport flags are of the type `ImGuiViewportFlags_`
  in the ImGui C++ API, and is a bitmask of flags, which are exposed as
  individual values in the Lua generated bindings.

  - `imgui.extra.setViewportFlags(id, flags)` will set the viewport flags for
  the specified viewport. The proper usage of this function is to call
  `imgui.extra.getViewportFlags` to get the current flags, modify the flags
  as needed, and then call `imgui.extra.setViewportFlags` to set the new flags.

  - `imgui.extra.getViewportPos(id)` will return the position of the specified
  viewport. The position is returned as an `ImVec2` object.

  - `imgui.extra.getViewportSize(id)` will return the size of the specified
  viewport. The size is returned as an `ImVec2` object.

  - `imgui.extra.getViewportWorkPos(id)` will return the work position of the
  specified viewport. The work position is returned as an `ImVec2` object.

  - `imgui.extra.getViewportWorkSize(id)` will return the work size of the
  specified viewport. The work size is returned as an `ImVec2` object.

  - `imgui.extra.getViewportDpiScale(id)` will return the DPI scale of the
  specified viewport. The DPI scale is returned as a number. A value of 1.0
  means that the DPI scale for this viewport is 100%.

  - `imgui.extra.InputText(label, text[, flags])` will create an input text
  widget. The `label` is the label to display next to the input text, and the
  `text` is the current text to display in the input text. The `flags` are
  optional, and are the same flags as the ones used by the `imgui::InputText`
  C++ function. The function will return a boolean indicating if the text has
  changed or not, and the new text.

  - `imgui.extra.InputTextWithHint(label, hint, text[, flags])` will create an
  input text widget. The `label` is the label to display next to the input
  text, and the `hint` is the hint to display in the input text when the text
  is empty. The `text` is the current text to display in the input text. The
  `flags` are optional, and are the same flags as the ones used by the
  `imgui::InputTextWithHint` C++ function. The function will return a boolean
  indicating if the text has changed or not, and the new text.

  - `imgui.extra.logText(text)` will call the `imgui::LogText` C++ function,
  which will add the given text to current log buffer.

  - `PCSX.GUI.useMainFont()` will call the `imgui::PushFont` C++ function with
  the proportional font. It will need to be followed by a call to `imgui.PopFont()`.

  - `PCSX.GUI.useMonoFont()` will call the `imgui::PushFont` C++ function with
  the monospace font. It will need to be followed by a call to `imgui.PopFont()`.

### Safety

The ImGui API will frequently assert and crash the process if the API calls
are imbalanced. For example, if the `imgui.BeginTable` function is called without
calling the `imgui.EndTable` function, the process will most likely crash.

This can be problematic when using the ImGui API from Lua, as the Lua code
is not able to catch the crash, and the process will crash without any
indication of what went wrong.

The main reason for imbalanced API calls can be attributed to the user code
throwing an exception, which will cause the Lua code to unwind the stack,
and the ImGui API will not be able to properly clean up its state.

For example, consider the following code:

```lua
function DrawImguiFrame()
    if imgui.Begin("My Window") then
        error("Something went wrong")
    end
    imgui.End()
end
```

The `imgui.Begin` function will be called, but the `imgui.End` function will
not be called, as the `error` function will unwind the stack, and the
`imgui.End` function will never be called.

In order to mitigate this, safe wrappers are provided for all of the ImGui
Begin\*/End\* functions, except `imgui.BeginItemTooltip`. Each wrapper takes the same arguments as the
corresponding Begin\* function, followed by a function to call for the contents.
The safe wrappers will catch any exception thrown by the user code, and will
call the corresponding End\* function. The error will be rethrown after the
End\* function is called. The wrapped function will only be called if the
Begin\* function returned true, and receives the same arguments as the Begin\*
function. The wrapper returns the values returned by the Begin\* function.

The End\* function is called following the ImGui rules for each pair:

- `imgui.safe.Begin`, `imgui.safe.BeginChild` and `imgui.safe.BeginChild_4` always
call `imgui.End` or `imgui.EndChild`, whatever the Begin\* function returned.
- `imgui.safe.BeginDisabled` and `imgui.safe.BeginGroup` always call the wrapped
function, since `imgui.BeginDisabled` and `imgui.BeginGroup` return nothing, and
then always call `imgui.EndDisabled` or `imgui.EndGroup`.
- All the other wrappers only call the End\* function if the Begin\* function
returned true.

The example above can be rewritten as:

```lua
function DrawImguiFrame()
    imgui.safe.Begin("My Window", function()
        error("Something went wrong")
    end)
end
```

## ThorVG

The [ThorVG](https://github.com/thorvg/thorvg) library is bound to Lua under the global
`tvg` table, and can be used to draw arbitrary vector graphics, SVG and Lottie files,
PNG and JPEG images, and text on top of the emulator. The rendering happens after the
ImGui rendering, meaning that it will be on top of the ImGui rendering, regardless of
the order in which the ThorVG and ImGui functions are called.

The binding exposes the ThorVG object model directly, through its
[C API](https://github.com/thorvg/thorvg/blob/main/src/bindings/capi/thorvg_capi.h).
Objects are created with the following constructors:

- `tvg.Shape()`
- `tvg.Scene()`
- `tvg.Picture()`
- `tvg.Text()`
- `tvg.LinearGradient([x1, y1, x2, y2])`
- `tvg.RadialGradient([cx, cy, r[, fx, fy[, fr]]])`, where `fx` and `fy` default to `cx` and `cy`, and `fr` defaults to 0.
- `tvg.Animation()`
- `tvg.LottieAnimation()`

Their methods are named after the C functions, in camel case, without the
`tvg_<type>_` prefix, and without the object argument. For example,
`tvg_shape_append_rect(shape, ...)` becomes `shape:appendRect(...)`, and
`tvg_paint_translate(paint, x, y)` becomes `paint:translate(x, y)`. Shapes, scenes,
pictures and texts also have all the `tvg_paint_*` methods. Gradients have the
`tvg_gradient_*` methods, plus `:setLinear`, `:getLinear`, `:setRadial` and `:getRadial`.
Animations have the `tvg_animation_*` and `tvg_lottie_animation_*` methods. The
methods return the raw `Tvg_Result` value of the C function, where 0 means success.
The raw C functions are also available in `tvg.C`.

A few methods have default arguments on top of the C API:

- `shape:appendRect(x, y, w, h[, rx[, ry[, cw]]])`: `rx` defaults to 0, `ry` defaults to `rx`, and `cw` defaults to true.
- `shape:appendCircle(cx, cy, rx[, ry[, cw]])`: `ry` defaults to `rx`, and `cw` defaults to true.
- `shape:setFillColor(r, g, b[, a])` and `shape:setStrokeColor(r, g, b[, a])`: the components are integers between 0 and 255, and `a` defaults to 255.
- `gradient:setColorStops(stops)` also accepts a table of `{ offset, r, g, b[, a] }` entries, where `a` defaults to 255.
- `shape:setGradient(gradient)`, `shape:setStrokeGradient(gradient)` and `text:setGradient(gradient)` use a copy of the gradient, so the gradient can be reused afterwards.

Additionally, the following functions are available:

- `tvg.getViewportScene([viewportId])` returns the scene rendered on top of the
given ImGui viewport, or `nil` if ThorVG isn't available. The viewport defaults to
the one of the ImGui window currently being drawn, as returned by
`imgui.extra.getCurrentViewportId()`. Paints in this scene are expressed in ImGui
coordinates, which are absolute screen coordinates when multi-viewports are enabled.

- `tvg.drawBezierArrow(width, p1, c1, c2, p2[, innerColor[, outerColor]])` draws an
arrow on top of the current ImGui viewport, for the current frame only. The points
are tables or objects with `x` and `y` fields, such as the ones created by
`imgui.extra.ImVec2.New`. The colors are tables with either `r`, `g`, `b`, `a` fields
or 4 array entries, with components between 0.0 and 1.0. The inner color defaults
to opaque white, and the outer color to opaque grey.

- `tvg.loadFont(path)` and `tvg.unloadFont(path)` load and unload a TTF or OTF font
file. A loaded font is referred to by its file name without the extension. The
emulator's font is loaded at startup, so `text:setFont('NotoSans-Regular')` works
without loading a font first.

The rendering is retained, not immediate: a paint added to the viewport scene with
`scene:add(paint)` is displayed on every frame until it is removed with
`scene:remove(paint)`, and it can be modified in place in between. Lua holds its own
reference on the objects it creates, and releases it when they are garbage collected,
but adding a paint to a scene keeps it alive as long as it is in the scene. Remove
the paints from the scene when they should no longer be displayed.

## Example of using everything together

As the vector graphics are drawn on top of the ImGui viewports, it is required to
use the ImGui API to draw some UI, grab the positions of the vector graphics to add,
and then update some ThorVG paints accordingly.

The following example will draw a red rectangle in the middle of the Output
region. The rectangle will be 100x100 pixels in size, and will be drawn on top
of the emulator rendering. It should follow around the Output region when
resizing or moving the window.

In order to work, this example requires the code to be executed in the `Image` function
of the Output shader invoker, so we can get the position of the Output region
to draw to.

```lua
local square = tvg.Shape()
square:setFillColor(255, 0, 0)
local scene

function Image(textureID, srcSizeX, srcSizeY, dstSizeX, dstSizeY)
    -- The top left corner of the image, in ImGui coordinates.
    local x, y = imgui.GetCursorScreenPos()

    -- The Output window may move to another viewport, so the square
    -- needs to follow it into the matching scene.
    local current = tvg.getViewportScene()
    if current ~= scene then
        if scene then scene:remove(square) end
        scene = current
        if scene then scene:add(square) end
    end

    -- Resetting a shape clears its path, but keeps its colors.
    square:reset()
    square:appendRect(x + dstSizeX / 2 - 50, y + dstSizeY / 2 - 50, 100, 100)
    imgui.Image(textureID, dstSizeX, dstSizeY, 0, 0, 1, 1)
end
```
