# Nyabula icon sources

`material_icon_paths.js` is the deterministic source map used by
`tools/generate_vector_icons.py`. The selected paths originate from Material
Design Icons and are distributed under Apache License 2.0:

<https://github.com/Templarian/MaterialDesign>

The generated C contours preserve the approved geometry at 4096-unit precision
so the LVGL renderer can animate path reveal and fill without a runtime SVG or
icon-font dependency.
