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

import os
import warnings

from setuptools import setup, Extension
from setuptools.command.build_ext import build_ext
from setuptools.errors import CCompilerError, ExecError, PlatformError

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
# see optional_build_ext below, which lets each extension fail independently
# and simply omits it from the install rather than aborting the whole build.
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
        extra_compile_args=['-std=c11'],
        extra_link_args=['-pthread'],
    )
    for name in _ext_names
]

# Exceptions that a failed C compile/link can surface, across platforms and
# setuptools versions (older setuptools raises the equivalent errors from
# distutils.errors instead; those are themselves aliases for/of these on the
# setuptools versions this package requires, so no separate import is
# needed).
_BUILD_EXT_ERRORS = (CCompilerError, ExecError, PlatformError, OSError)


class optional_build_ext(build_ext):
    """A build_ext that treats failing to build the optional C extension
    modules as a warning instead of a fatal error.

    pcollections has a complete, interface- and behavior-identical
    pure-Python fallback for every type the C extensions provide (see
    `pcollections/__init__.py` and `pcollections.test`), so a
    platform/toolchain that can't build them should still end up with a
    fully working install of pcollections -- just without the C speedups --
    rather than a failed `pip install`.
    """

    def run(self):
        try:
            build_ext.run(self)
        except _BUILD_EXT_ERRORS as e:
            warnings.warn(
                "pcollections: could not build the optional C extension "
                f"modules ({e!r}). Falling back to the pure-Python "
                "implementation -- this is not an error, pcollections will "
                "work normally, just without the C speedups.")

    def build_extension(self, ext):
        try:
            build_ext.build_extension(self, ext)
        except _BUILD_EXT_ERRORS as e:
            warnings.warn(
                f"pcollections: could not build the {ext.name!r} C "
                f"extension module ({e!r}); the types it would have "
                "provided will use their pure-Python fallback "
                "implementation instead.")


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
    cmdclass={'build_ext': optional_build_ext},
    zip_safe=False,
    include_package_data=True)
