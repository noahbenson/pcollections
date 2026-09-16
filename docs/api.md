# API reference

All of the public types and functions are available from the `pcollections`
package. The abstract base classes are in `pcollections.abc`.

## Persistent collections

```{eval-rst}
.. autoclass:: pcollections.plist
.. autoclass:: pcollections.pset
.. autoclass:: pcollections.pdict
```

## Transient collections

```{eval-rst}
.. autoclass:: pcollections.tlist
.. autoclass:: pcollections.tset
.. autoclass:: pcollections.tdict
```

## Lazy values

```{eval-rst}
.. autoclass:: pcollections.lazy
   :members: is_ready
   :no-inherited-members:
.. autofunction:: pcollections.unlazy
.. autofunction:: pcollections.holdlazy
.. autoexception:: pcollections.LazyError
   :no-members:
.. data:: pcollections.lazy_error_unwrap

   A function and context manager for unwrapping `LazyError` exceptions.
   ``lazy_error_unwrap(error)`` returns the cause of ``error`` if it is a
   `LazyError` with a cause, and otherwise returns ``error``. Used as a
   context manager (``with lazy_error_unwrap: ...``), it re-raises the cause
   of any `LazyError` that propagates out of its block.
```

## Lazy collections

```{eval-rst}
.. autoclass:: pcollections.llist
.. autoclass:: pcollections.tllist
.. autoclass:: pcollections.ldict
.. autoclass:: pcollections.tldict
```

## Backend information

```{eval-rst}
.. data:: pcollections.using_c_extension

   `True` if the C implementation is in use, and `False` if the pure-Python
   implementation is.

.. data:: pcollections.backend_error

   The exception that prevented the C implementation from loading, or
   `None`.
```

## Abstract base classes

```{eval-rst}
.. automodule:: pcollections.abc
   :members: Persistent, Transient, PersistentSequence, TransientSequence,
             PersistentSet, TransientSet, PersistentMapping, TransientMapping
```

## Utilities

```{eval-rst}
.. automodule:: pcollections.util
   :members: setcmp, seqcmp, seqeq, seqorder, seqstr, frozenset_hash
```
