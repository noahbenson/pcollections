#! /usr/bin/env python
################################################################################
# setup.py
# Build/install script for pcollections.
#
# The package lives under src/pcollections (a "src layout"). Alongside the
# pure-Python implementation it ships an optional C extension,
# pcollections._c._core, which pcollections/__init__.py prefers at import
# time. The pure-Python backend implements the same interface (pcollections.test
# runs the same tests against both), so a failure to compile the extension is
# not fatal: the extension is marked `optional=True`, which makes setuptools'
# build_ext report the failure as a warning and makes editable installs skip
# the missing output.
#
# Setting PCOLLECTIONS_NO_C_EXTENSIONS=1 builds without the extension at all
# (see below).

import os
import platform

from setuptools import setup, Extension

# Get the version from src/pcollections/__init__.py.
_here = os.path.dirname(os.path.abspath(__file__))
_src = os.path.join(_here, 'src')
with open(os.path.join(_src, 'pcollections', '__init__.py'), 'rt') as fl:
    lns = fl.readlines()
version = next(ln for ln in lns if "__version__ = " in ln)
version = version.split('"')[1]

# The optional C extension module. The whole C backend is one extension,
# pcollections._c._core, compiled from the single translation unit _core.c
# (which includes core.h and the per-type parts dict.c.h, list.c.h, set.c.h,
# and lazy.c.h). `optional=True` makes a compile failure non-fatal: the
# install still succeeds and pcollections uses its pure-Python backend.
#
# `-std=c11`/`-pthread` are GCC/Clang spellings that MSVC rejects, so they are
# passed only off Windows (MSVC needs neither: core.h has Win32 fallbacks for
# the threading primitives it uses).
_is_windows = (platform.system() == 'Windows')
_c_dir = os.path.join('src', 'pcollections', '_c')
_c_headers = [os.path.join(_c_dir, name)
              for name in ('core.h', 'trie.h', 'amt.h', 'fat.h', 'uintbits.h',
                           'dict.c.h', 'list.c.h', 'set.c.h', 'lazy.c.h')]

# PCOLLECTIONS_NO_C_EXTENSIONS=1 builds and installs without the C backend,
# even where it would compile. CI's "python-only" job uses this to test the
# pure-Python backend on its own.
_no_c_ext = (os.environ.get('PCOLLECTIONS_NO_C_EXTENSIONS', '')
             not in ('', '0', 'false', 'False'))

ext_modules = [] if _no_c_ext else [
    Extension(
        'pcollections._c._core',
        sources=[os.path.join(_c_dir, '_core.c')],
        depends=_c_headers,
        include_dirs=[_c_dir],
        extra_compile_args=[] if _is_windows else ['-std=c11'],
        extra_link_args=[] if _is_windows else ['-pthread'],
        optional=True,
    )
]

# All other static metadata (name, description, authors, license,
# classifiers, dependencies, urls, ...) lives in pyproject.toml's [project]
# table; only `version` (read dynamically above) and the build configuration
# that pyproject.toml's [project] table can't express -- the src/ package
# layout and the optional C extension -- are set here.
setup(
    version=version,
    package_dir={'': 'src'},
    packages=['pcollections',
              'pcollections.abc',
              'pcollections.util',
              'pcollections.test',
              'pcollections._c'],
    package_data={
        '': ['LICENSE.txt'],
        # Ship the C headers/sources too (not just the compiled .so, which
        # setuptools already includes for us): useful for source
        # distributions and for anyone building the extensions themselves.
        'pcollections._c': ['*.h', '*.c'],
    },
    ext_modules=ext_modules,
    zip_safe=False,
    include_package_data=True)
