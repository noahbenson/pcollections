# -*- coding: utf-8 -*-
################################################################################
# pcollections/test/__main__.py
# Entry point for `python -m pcollections.test` (used by
# .github/workflows/tests.yml's "Run Tests" step); `python -m` on a package
# requires a `__main__.py`.
# By Noah C. Benson

import sys
import unittest

from . import suite

if __name__ == '__main__':
    runner = unittest.TextTestRunner(verbosity=2)
    result = runner.run(suite())
    sys.exit(0 if result.wasSuccessful() else 1)
