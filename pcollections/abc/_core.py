# -*- coding: utf-8 -*-
################################################################################
# pcollections/abc/_core.py
# The definitions of the abstract base classes for the persistent data types.
# By Noah C. Benson

from collections.abc import Hashable

class Persistent(Hashable):
    # Abstract methods.
    def transient(self):
        """Efficiently returns a transient copy of the persistent object."""
        raise NotImplementedError()
    # Methods that should throw errors in children.
    def __setattr__(self, k, v):
        raise TypeError(f"{type(self)} is immutable")
    def __setitem__(self, k, v):
        raise TypeError(f"{type(self)} is immutable")
    def __delitem__(self, k, v):
        raise TypeError(f"{type(self)} is immutable")
    # Implementation methods that are probably fine in all children.
    def copy(self):
        """Returns the persistent object (persistent objects needn't be
        copied)."""
        return self

class Transient:
    def persistent(self):
        """Efficiently returns a persistent copy of the transient object."""
        raise NotImplementedError()
    # Implementations that are probably fine for most children.
    def copy(self):
        """Returns a copy of the transient object."""
        return self.persistent().transient()
    
