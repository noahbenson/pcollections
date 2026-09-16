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

import setuptools
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

def _env_flag(name):
    value = os.environ.get(name, '').strip().lower()
    return value not in ('', '0', 'false', 'no', 'off')

# PCOLLECTIONS_NO_C_EXTENSIONS=1 builds and installs without the C backend,
# even where it would compile. CI's "python-only" job uses this to test the
# pure-Python backend on its own.
_no_c_ext = _env_flag('PCOLLECTIONS_NO_C_EXTENSIONS')

# PCOLLECTIONS_REQUIRE_C=1 makes a failure to compile the C extension fail
# the build, instead of producing an installation that silently uses the
# pure-Python backend.
_require_c = _env_flag('PCOLLECTIONS_REQUIRE_C')
if _no_c_ext and _require_c:
    raise SystemExit("PCOLLECTIONS_NO_C_EXTENSIONS and PCOLLECTIONS_REQUIRE_C "
                     "are both set")

ext_modules = [] if _no_c_ext else [
    Extension(
        'pcollections._c._core',
        sources=[os.path.join(_c_dir, '_core.c')],
        depends=_c_headers,
        include_dirs=[_c_dir],
        extra_compile_args=[] if _is_windows else ['-std=c11'],
        extra_link_args=[] if _is_windows else ['-pthread'],
        optional=not _require_c,
    )
]

# License metadata. setuptools 77 and later take an SPDX expression (PEP 639);
# the older setuptools used for Python 3.8 builds takes the older fields.
def _setuptools_version():
    parts = []
    for part in setuptools.__version__.split('.')[:2]:
        digits = ''.join(ch for ch in part if ch.isdigit())
        parts.append(int(digits or 0))
    return tuple(parts)

if _setuptools_version() >= (77, 0):
    license_args = {'license_expression': 'MIT', 'license_files': ['LICENSE']}
else:
    license_args = {'license': 'MIT', 'license_files': ['LICENSE']}

# All other static metadata (name, description, authors,
# classifiers, dependencies, urls, ...) lives in pyproject.toml.
setup(
    version=version,
    package_dir={'': 'src'},
    packages=['pcollections',
              'pcollections.abc',
              'pcollections.util',
              'pcollections.test',
              'pcollections._c'],
    package_data={
        'pcollections': ['py.typed', '*.pyi'],
        'pcollections._c': ['*.h', '*.c'],
    },
    ext_modules=ext_modules,
    zip_safe=False,
    **license_args,
    include_package_data=True)
