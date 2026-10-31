#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""Compile external C/C++ consumers using libwebsockets Make exports."""
from pathlib import Path
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[2]
BASE = 'ifeq ($(CONFIG_NETUTILS_LIBWEBSOCKETS),y)\nCONFIGURED_APPS += $(APPDIR)/netutils/libwebsockets\n\n# Allows `<libwebsockets/<>.h>` import.\n\nCFLAGS +=  ${INCDIR_PREFIX}$(APPDIR)/netutils/libwebsockets/libwebsockets/include\nCXXFLAGS +=  ${INCDIR_PREFIX}$(APPDIR)/netutils/libwebsockets/libwebsockets/include\n\nendif\n'


class IncludeTest(unittest.TestCase):
    def test_configuration_header_is_exported(self):
        with tempfile.TemporaryDirectory(prefix='ny-lws-make-') as directory:
            root = Path(directory)
            apps = root / 'apps'
            wrapper = apps / 'netutils/libwebsockets'
            public = wrapper / 'libwebsockets/include'
            public.mkdir(parents=True)
            (wrapper / 'Make.defs').write_text(BASE)
            (wrapper / 'lws_config.h').write_text('#define LWS_TEST_CONFIG 42\n')
            (public / 'libwebsockets.h').write_text('#include "lws_config.h"\n')
            source = '#include <libwebsockets.h>\nint value = LWS_TEST_CONFIG;\n'
            (root / 'consumer.c').write_text(source)
            (root / 'consumer.cpp').write_text(source)
            (root / 'Makefile').write_text(
                'APPDIR := ' + str(apps) + '\nINCDIR_PREFIX := -I\n'
                'CONFIG_NETUTILS_LIBWEBSOCKETS := y\n'
                'include $(APPDIR)/netutils/libwebsockets/Make.defs\n'
                'c:\n\t$(CC) $(CFLAGS) -Werror -c consumer.c -o c.o\n'
                'cxx:\n\t$(CXX) $(CXXFLAGS) -Werror -c consumer.cpp -o cxx.o\n')
            baseline = subprocess.run(['make', '-C', str(root), 'c'],
                                      capture_output=True, text=True)
            self.assertNotEqual(baseline.returncode, 0)
            self.assertIn('lws_config.h', baseline.stderr)
            patch = (ROOT / 'patches/apps-libwebsockets-make-includes.patch').read_bytes()
            subprocess.run(['patch', '--batch', '-d', str(apps), '-p1'],
                           input=patch, check=True, capture_output=True)
            fixed = subprocess.run(['make', '-C', str(root), 'c', 'cxx'],
                                   capture_output=True, text=True)
            self.assertEqual(fixed.returncode, 0, fixed.stdout + fixed.stderr)


if __name__ == '__main__':
    unittest.main()
