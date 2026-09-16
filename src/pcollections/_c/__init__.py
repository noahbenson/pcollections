# -*- coding: utf-8 -*-
################################################################################
# pcollections/_c/__init__.py
# Marks `pcollections._c` as a regular (not implicit-namespace) package, so
# that packaging tools (setuptools' `packages=[...]` list in setup.py) and
# import-time error messages behave predictably.
#
# The actual C extension modules (`dict`, `list`, `set`, `lazy`) are built
# from the `.c` files in this directory and installed alongside this file --
# they are compiled extension modules, not something this file needs to
# import or re-export itself. `pcollections/__init__.py` is what decides,
# at import time, whether to use them (falling back to the pure-Python
# implementation in `pcollections._dict`/`_list`/`_set`/`_lazy` if they
# aren't importable, e.g. because they were never built for this platform).
# By Noah C. Benson
