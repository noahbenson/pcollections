# -*- coding: utf-8 -*-
################################################################################
# pcollections/test/__main__.py
# Entry point for `python -m pcollections.test` (used by
# .github/workflows/tests.yml's "Run Tests" step).
#
# Without this file, `python -m pcollections.test` fails outright with
# "No module named pcollections.test.__main__; 'pcollections.test' is a
# package and cannot be directly executed" -- `python -m <package>` always
# requires a `__main__.py`, `unittest.suite()`/`_TestModuleNamespace` alone
# aren't enough. (Discovered while re-verifying the test suite end to end
# during the phamt-removal work: the CI workflow's "Run Tests" step was
# already spelled this way, so this was silently broken before this file
# existed -- every previous CI run of this exact command would have failed.)
# By Noah C. Benson

import sys
import unittest

from . import suite

if __name__ == '__main__':
    runner = unittest.TextTestRunner(verbosity=2)
    result = runner.run(suite())
    sys.exit(0 if result.wasSuccessful() else 1)
