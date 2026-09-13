# -*- coding: utf-8 -*-
################################################################################
# pcollections/abc/__init__.py
# Initialization file for the pcollections library abstract base class core.
# By Noah C. Benson

"""Abstract base classes for persistent and transient collections for Python.
"""

from ._core import (Persistent,         Transient)
from ._seq  import (PersistentSequence, TransientSequence)
from ._set  import (PersistentSet,      TransientSet)
from ._map  import (PersistentMapping,  TransientMapping)
