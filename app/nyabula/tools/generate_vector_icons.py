#!/usr/bin/env python3

"""Preserve approved Nyabula SVG paths as deterministic C vector commands."""

from __future__ import annotations

import argparse
import json
import re
from pathlib import Path

from fontTools.pens.recordingPen import RecordingPen
from fontTools.svgLib.path import parse_path


def vector_commands(path_data):
    pen = RecordingPen()
    parse_path(path_data, pen)
    output = []
    current = (0.0, 0.0)
    origin = current
    for command, args in pen.value:
        if command == "moveTo":
            current = args[0]
            origin = current
            output.append(("MOVE", current))
        elif command == "lineTo":
            current = args[0]
            output.append(("LINE", current))
        elif command == "curveTo":
            output.append(("CUBIC", args[0], args[1], args[2]))
            current = args[2]
        elif command == "qCurveTo":
            points = list(args)
            end = origin if points[-1] is None else points[-1]
            points.pop()
            while len(points) > 1:
                implied = ((points[0][0] + points[1][0]) * 0.5,
                           (points[0][1] + points[1][1]) * 0.5)
                output.append(("QUAD", points[0], implied))
                current = implied
                points.pop(0)
            output.append(("QUAD", points[0], end))
            current = end
        elif command in ("closePath", "endPath"):
            if command == "closePath":
                output.append(("CLOSE",))
            current = origin
        else:
            raise ValueError(f"unsupported SVG command: {command}")
    return output


def read_svg(path):
    text = path.read_text(encoding="utf-8")
    viewbox = re.search(r'viewBox="([^"]+)"', text)
    path_data = re.search(r'<path[^>]+d="([^"]+)"', text)
    if not viewbox or not path_data:
        raise ValueError(f"missing viewBox/path in {path}")
    values = [float(value) for value in viewbox.group(1).split()]
    return values, vector_commands(path_data.group(1))


def read_icon_map(path):
    text = path.read_text(encoding="utf-8")
    match = re.search(r"Object\.freeze\((\{.*\})\);", text, re.DOTALL)
    if match is None:
        raise ValueError(f"missing icon map in {path}")
    source = json.loads(match.group(1))
    return [
        (ident(name), icon["viewBox"], vector_commands(icon["path"]))
        for name, icon in sorted(source.items())
    ]


def ident(name):
    return re.sub(r"[^a-z0-9]+", "_", name.lower()).strip("_")


def render(source, output_h, output_c):
    if source.is_file():
        icons = read_icon_map(source)
    else:
        icons = []
        for path in sorted(source.glob("*.svg")):
            viewbox, commands = read_svg(path)
            icons.append((ident(path.stem), viewbox, commands))

    header = """/****************************************************************************
 * app/nyabula/src/generated/nyabula_eye_icons.h
 *
 * Licensed to the Apache Software Foundation (ASF) under one or more
 * contributor license agreements.  See the NOTICE file distributed with
 * this work for additional information regarding copyright ownership.  The
 * ASF licenses this file to you under the Apache License, Version 2.0 (the
 * "License"); you may not use this file except in compliance with the
 * License.  You may obtain a copy of the License at
 *
 *   http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS, WITHOUT
 * WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.  See the
 * License for the specific language governing permissions and limitations
 * under the License.
 ****************************************************************************/

#ifndef __APP_NYABULA_SRC_GENERATED_NYABULA_EYE_ICONS_H
#define __APP_NYABULA_SRC_GENERATED_NYABULA_EYE_ICONS_H

#include <stdint.h>

enum nyabula_eye_icon_operation_e
{
  NYABULA_EYE_ICON_MOVE = 0,
  NYABULA_EYE_ICON_LINE,
  NYABULA_EYE_ICON_QUAD,
  NYABULA_EYE_ICON_CUBIC,
  NYABULA_EYE_ICON_CLOSE
};

struct nyabula_eye_icon_command_s
{
  uint8_t operation;
  int32_t x1;
  int32_t y1;
  int32_t x2;
  int32_t y2;
  int32_t x3;
  int32_t y3;
};

struct nyabula_eye_icon_s
{
  const char *name;
  const struct nyabula_eye_icon_command_s *commands;
  uint16_t command_count;
  uint32_t width;
  uint32_t height;
};

const struct nyabula_eye_icon_s *nyabula_eye_icon_find(const char *name);

#endif
"""
    output_h.parent.mkdir(parents=True, exist_ok=True)
    output_h.write_text(header, encoding="utf-8", newline="\n")

    body = ["""/****************************************************************************
 * app/nyabula/src/generated/nyabula_eye_icons.c
 *
 * Licensed to the Apache Software Foundation (ASF) under one or more
 * contributor license agreements.  See the NOTICE file distributed with
 * this work for additional information regarding copyright ownership.  The
 * ASF licenses this file to you under the Apache License, Version 2.0 (the
 * "License"); you may not use this file except in compliance with the
 * License.  You may obtain a copy of the License at
 *
 *   http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS, WITHOUT
 * WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.  See the
 * License for the specific language governing permissions and limitations
 * under the License.
 ****************************************************************************/

#include <stddef.h>
#include <string.h>

#include \"nyabula_eye_icons.h\"
"""]
    entries = []
    coordinate_width = 4096
    for name, viewbox, commands in icons:
        coordinate_height = round(viewbox[3] / viewbox[2] * coordinate_width)
        rows = []
        for command in commands:
            values = []
            for point in command[1:]:
                values.extend((
                    round((point[0] - viewbox[0]) / viewbox[2] *
                          coordinate_width),
                    round((point[1] - viewbox[1]) / viewbox[3] *
                          coordinate_height),
                ))
            values.extend([0] * (6 - len(values)))
            rows.append(f"{{NYABULA_EYE_ICON_{command[0]}, " +
                        ", ".join(str(value) for value in values) + "}")
        command_text = ",\n  ".join(rows)
        body.append(
            f"\nstatic const struct nyabula_eye_icon_command_s "
            f"g_{name}_commands[] =\n{{\n  {command_text}\n}};\n")
        entries.append(
            f'  {{"{name}", g_{name}_commands, {len(commands)}, '
            f'{coordinate_width}, '
            f'{coordinate_height}}}')
    body.append("\nstatic const struct nyabula_eye_icon_s g_icons[] =\n{\n" +
                ",\n".join(entries) + "\n};\n")
    body.append("""
const struct nyabula_eye_icon_s *nyabula_eye_icon_find(const char *name)
{
  size_t index;

  for (index = 0; index < sizeof(g_icons) / sizeof(g_icons[0]); index++)
    {
      if (strcmp(g_icons[index].name, name) == 0)
        {
          return &g_icons[index];
        }
    }

  return NULL;
}
""")
    output_c.write_text("".join(body), encoding="utf-8", newline="\n")


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("source", type=Path)
    parser.add_argument("header", type=Path)
    parser.add_argument("source_output", type=Path)
    args = parser.parse_args()
    render(args.source, args.header, args.source_output)


if __name__ == "__main__":
    main()
