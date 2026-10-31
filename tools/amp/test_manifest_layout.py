#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""Check nyampctl build paths through the manifest's application symlink."""

import pathlib
import shutil
import subprocess
import tempfile
import unittest


REPO = pathlib.Path(__file__).resolve().parents[2]


class ManifestLayoutTest(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory(prefix="nyamp-manifest-")
        self.addCleanup(self.temp.cleanup)
        self.root = pathlib.Path(self.temp.name)
        team = self.root / "team"
        self.apps = self.root / "apps"
        app = team / "app/nyampctl"
        shutil.copytree(REPO / "app/nyampctl", app)
        protocol = team / "tools/amp/protocol"
        protocol.parent.mkdir(parents=True)
        protocol.symlink_to(REPO / "tools/amp/protocol", target_is_directory=True)
        (self.apps / "examples").mkdir(parents=True)
        self.link = self.apps / "examples/nyampctl"
        self.link.symlink_to(app, target_is_directory=True)
        (self.root / "vendor/rockchip").mkdir(parents=True)
        (self.root / "nuttx").mkdir()
        self.assertFalse((self.root / "vendor/rockchip/tools").exists())

    def run_command(self, *args):
        result = subprocess.run(args, capture_output=True, text=True)
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)

    def test_make_protocol_source_and_header(self):
        (self.apps / "Make.defs").write_text("INCDIR_PREFIX = -I\n")
        (self.apps / "Application.mk").write_text(
            ".PHONY: check\n"
            "check: nyamp_protocol.c\n"
            "\t$(CC) $(CFLAGS) -fsyntax-only $<\n"
        )
        self.run_command("make", "-C", str(self.link), "check",
                         f"APPDIR={self.apps}", f"TOPDIR={self.root / 'nuttx'}")

    def test_cmake_protocol_source_and_header(self):
        (self.root / "CMakeLists.txt").write_text(
            "cmake_minimum_required(VERSION 3.16)\n"
            "project(manifest_layout C)\n"
            "set(CONFIG_EXAMPLES_NYAMPCTL y)\n"
            'set(NUTTX_APPS_DIR "${CMAKE_CURRENT_SOURCE_DIR}/apps")\n'
            "function(nuttx_add_application)\n"
            '  cmake_parse_arguments(APP "" "MODULE;NAME;STACKSIZE;PRIORITY" '
            '"SRCS;INCLUDE_DIRECTORIES" ${ARGN})\n'
            # The codec is found by name, not by position: the application's
            # own sources come and go around it.
            '  list(FILTER APP_SRCS INCLUDE REGEX "nyamp_protocol[.]c$")\n'
            "  list(GET APP_SRCS 0 protocol_source)\n"
            '  add_library(protocol STATIC "${protocol_source}")\n'
            "  target_include_directories(protocol PRIVATE ${APP_INCLUDE_DIRECTORIES})\n"
            "endfunction()\n"
            'add_subdirectory("${NUTTX_APPS_DIR}/examples/nyampctl" app)\n'
        )
        build = self.root / "build"
        self.run_command("cmake", "-S", str(self.root), "-B", str(build))
        self.run_command("cmake", "--build", str(build), "-j2")


if __name__ == "__main__":
    unittest.main()
