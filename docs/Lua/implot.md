# Plots

The global `implot` table binds [ImPlot](https://github.com/epezent/implot), the plotting library that sits on top of ImGui. Like `imgui`, it can only be used inside the ImGui context, for instance from `DrawImguiFrame`. The ImPlot API is documented in its [header](https://github.com/epezent/implot/blob/master/implot.h); this page describes how the Lua binding differs from it.

```lua
local ffi = require 'ffi'
local mem = PCSX.getMemPtr()

function DrawImguiFrame()
    if imgui.Begin('RAM plot') then
        implot.safe.BeginPlot('bytes', -1, 250, function()
            implot.SetupAxes('offset', 'value')
            implot.SetupAxesLimits(0, 256, 0, 255)
            implot.PlotLine('ram', mem + 0x1000, 256)
        end)
    end
    imgui.End()
end
```

## Conventions

- Names follow ImPlot, without the `ImPlot` prefix: `implot.BeginPlot`, `implot.SetupAxis`, `implot.PlotLine`, and so on. Enumerations are in `implot.constant`, grouped by their ImPlot type: `implot.constant.Axis.X1`, `implot.constant.Flags.NoTitle`, `implot.constant.Marker.Circle`, `implot.constant.Cond`, `implot.constant.Colormap`, and so on. Run `pprint(implot.constant)` for the list. `implot.AUTO` is `IMPLOT_AUTO`.
- Every `Begin` whose `End` is only required when it returned true has a counterpart in `implot.safe`, which takes the body as its last argument and pairs the calls, even if the body raises an error: `BeginPlot`, `BeginSubplots`, `BeginAlignedPlots`, `BeginLegendPopup`, and the `BeginDragDrop*` family. Prefer these.
- ImPlot assertions are raised as Lua errors, located at the caller.
- Data arguments are either Lua tables of numbers, which are copied, or FFI arrays and pointers of a supported scalar type, which are read in place. The latter is what makes plotting live memory above possible. All the data arguments of a call have to share the same element type, and `count` is required as soon as one of them is FFI data. With tables only, `count` defaults to the length of the shortest.
- ImPlot overloads which only differ by arity are resolved by the shape of the arguments: when the argument after the first data array is a number or `nil`, it's the values form, `PlotLine(label, values, count, ...)`. When it's an array, it's the `xs`/`ys` form, `PlotLine(label, xs, ys, count, ...)`. The comments above each function in `src/gui/implotffi.lua` list the accepted forms.
- Colors are tables with `r`, `g`, `b`, `a` fields, from 0 to 1. A `nil` color is the automatic one.
- Functions with output pointers return their results instead: `implot.DragLineX(id, x, ...)` returns `changed, x, clicked, hovered, held`.
- The trailing `spec` argument of the plot functions is a table of `ImPlotSpec` fields, `LineColor`, `LineWeight`, `FillColor`, `FillAlpha`, `Marker`, `MarkerSize`, `MarkerLineColor`, `MarkerFillColor`, `Size`, `Offset`, `Stride`, `Flags`, and the per-point arrays `LineColors`, `FillColors`, `MarkerLineColors`, `MarkerFillColors`, `MarkerSizes`. An unknown field is an error. `implot.Spec(table)` builds the cdata once for reuse.

```lua
implot.PlotScatter('points', xs, ys, #xs, {
    Marker = implot.constant.Marker.Circle,
    MarkerSize = 4,
    MarkerFillColor = { r = 1, g = 0.4, b = 0, a = 1 },
})
```

## What is bound

- Plots: `BeginPlot`, `BeginSubplots`, `BeginAlignedPlots`, legend popups and drag and drop targets and sources.
- Setup: `SetupAxis`, `SetupAxisLimits`, `SetupAxisFormat`, `SetupAxisTicks`, `SetupAxisScale`, the constraint functions, `SetupAxes`, `SetupAxesLimits`, `SetupLegend`, `SetupMouseText`, `SetupFinish`, and the `SetNext*` and `SetAxis` families.
- Items: `PlotLine`, `PlotScatter`, `PlotStairs`, `PlotBubbles`, `PlotPolygon`, `PlotShaded`, `PlotBars`, `PlotBarGroups`, `PlotErrorBars`, `PlotStems`, `PlotInfLines`, `PlotPieChart`, `PlotHeatmap`, `PlotHistogram`, `PlotHistogram2D`, `PlotDigital`, `PlotText`, `PlotDummy`.
- Tools and queries: `DragPoint`, `DragLineX`, `DragLineY`, `DragRect`, `Annotation`, `TagX`, `TagY`, `PixelsToPlot`, `PlotToPixels`, `GetPlotPos`, `GetPlotSize`, `GetPlotMousePos`, `GetPlotLimits`, `GetPlotSelection`, and the `Is*Hovered` and `IsPlotSelected` functions.
- Styles and colormaps: `PushStyleColor`, `PushStyleVar`, `StyleColors*`, `AddColormap`, `PushColormap`, `SampleColormap`, `ColormapScale`, and related.
- Demo windows: `ShowDemoWindow`, `ShowStyleEditor`, `ShowMetricsWindow`, `ShowUserGuide`.
