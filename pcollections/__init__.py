# -*- coding: utf-8 -*-
################################################################################
# pcollections/__init__.py
# Initialization file for the pcollections library.
# By Noah C. Benson

"""Persistent and Transient Collections for Python
"""

from ._list import (plist, tlist)
from ._set  import (pset,  tset)
from ._dict import (pdict, tdict)
from ._lazy import (lazy, llist, ldict)

# We don't include the abc types in the __all__; they are probably not as
# frequently used and don't really need to be here. One can always `import
# pcollections.abc` if they are needed.
# 
# from .abc   import (
#     Persistent,         Transient,
#     PersistentSequence, TransientSequence,
#     PersistentSet,      TransientSet,
#     PersistentMapping,  TransientMapping
# )

__all__ = [
    "plist", "tlist",
    "pset",  "tset",
    "pdict", "tdict",
    "lazy", "llist", "ldict"
]

__version__ = "0.1.0"

