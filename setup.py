#! /usr/bin/env python
################################################################################
# setup.py
# Build/install script for pcollections.
#
# The package lives under src/pcollections (a "src layout"), and, alongside
# the pure-Python reference implementation, ships four optional C extension
# modules under pcollections/_c (dict.c, list.c, set.c, lazy.c) that
# `pcollections/__init__.py` prefers at import time when they're available.
# Because pcollections has a complete, interface-identical pure-Python
# fallback for everything the extensions provide (see that module's
# docstring, and pcollections.test, which tests both backends), a failure to
# compile the extensions here is treated as non-fatal: install still
# succeeds, just without the C speedups. This matters because the
# extensions use C11 (`<stdatomic.h>`) and are only tested on
# CPython's C API, so a install on an unusual platform/compiler/Python
# implementation should still end up with a working, pure-Python
# pcollections rather than a failed install.
#
# That non-fatal behavior is provided entirely by each Extension's own
# `optional=True` flag below -- distutils/setuptools' build_ext command
# already understands this flag natively: it catches a failing extension's
# compiler/linker errors and demotes them to a warning
# (`_filter_build_errors` in setuptools._distutils.command.build_ext), *and*
# `copy_extensions_to_source` (used by `pip install -e .`'s editable-install
# path) skips copying a missing output rather than erroring -- but only when
# the extension is marked optional. An earlier version of this file
# reimplemented a version of this by hand via a custom `build_ext` cmdclass
# subclass that only wrapped `build_extension`/`run` -- which caught the
# compile failure but, since it never marked the extensions `optional`, left
# `copy_extensions_to_source` unaware they were allowed to be missing, so it
# tried to copy a .so/.pyd that was never built and raised a
# DistutilsFileError our wrapper's exception list didn't even catch (broke
# `pip install -e .` on every platform where any extension failed to
# compile -- confirmed directly: a deliberately-broken extension crashed
# the editable install with that exact error under the old cmdclass, and
# installed cleanly with a pure-Python fallback for just that one type once
# switched to `optional=True`). Using the built-in `optional=True` avoids
# reinventing -- and mis-implementing -- machinery setuptools already
# provides for exactly this case.

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

# The four optional C extension modules. Each is built from a single .c file
# in pcollections/_c, and each compiles standalone (they don't link against
# each other), so a failure in one doesn't need to take down the others --
# `optional=True` (see the file header above) is what makes that true.
#
# `-std=c11`/`-pthread` are GCC/Clang spellings -- MSVC (used on Windows)
# doesn't recognize either flag and would fail the whole compile on a
# spurious "unrecognized command-line option" before ever getting to the
# real question of whether it supports what the code actually needs
# (`<stdatomic.h>`, which MSVC's C mode has only very limited/recent support
# for). We could pass MSVC's nearest equivalents (`/std:c11`, and threads
# are linked implicitly, no `/...pthread` equivalent needed), but since a
# failure here just falls back to the pure-Python implementation either way
# (via `optional=True`), it's simplest -- and avoids the confusing unrelated
# "bad flag" error in CI logs -- to just not pass the Unix-only flags on
# Windows and let the *real* compatibility question (stdatomic.h support)
# decide the outcome.
_is_windows = (platform.system() == 'Windows')
_c_dir = os.path.join('src', 'pcollections', '_c')
_ext_names = ('dict', 'list', 'set', 'lazy')
ext_modules = [
    Extension(
        f'pcollections._c.{name}',
        sources=[os.path.join(_c_dir, f'{name}.c')],
        include_dirs=[_c_dir],
        # lazy.c uses <stdatomic.h>/pthread primitives for its thread-safe
        # lazy-value implementation; the others don't need -pthread but it's
        # harmless to link it in for all four uniformly.
        extra_compile_args=[] if _is_windows else ['-std=c11'],
        extra_link_args=[] if _is_windows else ['-pthread'],
        optional=True,
    )
    for name in _ext_names
]

# All other static metadata (name, description, authors, license,
# classifiers, dependencies, urls, ...) lives in pyproject.toml's [project]
# table; only `version` (read dynamically above) and the build configuration
# that pyproject.toml's [project] table can't express -- the src/ package
# layout and the optional C extension modules -- are set here.
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
