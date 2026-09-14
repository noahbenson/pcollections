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
#
# Separately, the PCOLLECTIONS_NO_C_EXTENSIONS environment variable (see
# below, near ext_modules) forces a build/install with NO C extensions at
# all, regardless of whether they'd compile -- used by
# .github/workflows/tests.yml's dedicated "python-only" job to verify the
# pure-Python fallback deliberately, rather than relying on some platform's
# C build happening to fail.

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

# PCOLLECTIONS_NO_C_EXTENSIONS: an explicit escape hatch to build/install
# with NO C extensions at all, even on a platform where they'd otherwise
# compile just fine. `optional=True` above already means every *existing*
# CI job ends up exercising the pure-Python implementation -- every test in
# pcollections.test is written once and parametrized over whichever
# backends are available (see pcollections/test/_backends.py), so the
# pure-Python classes always run *alongside* the C ones wherever the C
# extensions happen to build -- but that's incidental coverage, not a
# guarantee: as of the Windows/MSVC fixes, the C extensions now build
# successfully on every platform this project tests, which means there is
# no longer any CI job where the pure-Python fallback runs *on its own*.
# A bug that only manifests when the C extension is truly absent (most
# plausibly in pcollections/__init__.py's own fallback/import logic, but
# in principle anywhere) could pass every job and still go unnoticed. This
# flag lets .github/workflows/tests.yml's dedicated "python-only" job force
# that scenario deliberately, instead of depending on some platform's C
# build happening to fail.
_no_c_ext = (os.environ.get('PCOLLECTIONS_NO_C_EXTENSIONS', '')
             not in ('', '0', 'false', 'False'))

ext_modules = [] if _no_c_ext else [
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
