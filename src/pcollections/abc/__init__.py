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

# The plain (non-ABCMeta) "base" mixins that hold each family's concrete
# method bodies (e.g., _PersistentMappingBase holds PersistentMapping's
# methods without the Mapping/Persistent ABCs). They are not public API; the
# classes above mix them in alongside the collections.abc ABCs.
# pcollections._c._core builds its heap types on these, because CPython 3.14
# does not allow an ABCMeta base there (see _core.py's _PersistentBase
# docstring). build_abc_subtype() in _c/core.h looks them up by name on this
# package.
from ._core import (_PersistentBase,)
from ._seq  import (_PersistentSequenceBase, _TransientSequenceBase)
from ._set  import (_PersistentSetBase,      _TransientSetBase)
from ._map  import (_PersistentMappingBase,  _TransientMappingBase)

# Likewise for the dict-view mixins (see _view.py's module docstring), which
# pcollections._c._core's build_view_type() looks up by name.
from ._view import (_MappingViewBase, _KeysViewBase, _ItemsViewBase,
                     _ValuesViewBase)
