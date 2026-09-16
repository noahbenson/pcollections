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

# The plain (non-ABCMeta) "base" mixin classes that hold each family's
# concrete method bodies -- e.g. _PersistentMappingBase holds everything
# PersistentMapping implements concretely, minus the real Mapping/Persistent
# ABCs themselves. These aren't meant for ordinary use (the classes above,
# which mix them in alongside the real collections.abc ABCs, are the public
# API and behave exactly as they always have) -- they exist so that
# pcollections._c._core can build its heap types on top of a
# base whose metaclass is plain `type`, not `ABCMeta`, which CPython 3.14
# requires (see _core.py's _PersistentBase docstring for the full story).
# Imported here (rather than left as each submodule's own private name) so
# that PyObject_GetAttrString(pcollections.abc, "_PersistentMappingBase")
# (etc.) -- called from build_abc_subtype() in _c/core.h -- finds them
# directly on the package, the same way it already looks up "PersistentMapping"
# and friends.
from ._core import (_PersistentBase,)
from ._seq  import (_PersistentSequenceBase, _TransientSequenceBase)
from ._set  import (_PersistentSetBase,      _TransientSetBase)
from ._map  import (_PersistentMappingBase,  _TransientMappingBase)

# Likewise for the dict-view-family plain mixins (see _view.py's module
# docstring) -- pcollections._c._core's build_view_type() looks these up by
# name on this package the same way.
from ._view import (_MappingViewBase, _KeysViewBase, _ItemsViewBase,
                     _ValuesViewBase)
