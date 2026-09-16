# -*- coding: utf-8 -*-
################################################################################
# pcollections/_lazybase.py
# Lazy-value support shared by both backends.
# By Noah C. Benson

"""Lazy-value support shared by the C and pure-Python backends.

Both backends raise the same `LazyError` class and use the same
`lazy_error_unwrap` object, so code that catches or unwraps lazy errors
works the same whichever backend is loaded. The backends call the helpers
here to build the error when a lazy value fails.

A `lazy` value is in one of four states:

- pending: not yet computed;
- running: being computed by some thread;
- ready: computed; the value is cached, and the function and its arguments
  have been released;
- failed: the computation raised an `Exception`. The exception is kept, and
  every later request raises a new `LazyError` whose `__cause__` is that
  exception, without running the computation again.

A computation interrupted by an exception that is not an `Exception` (such
as `KeyboardInterrupt`) leaves the lazy value pending.
"""

import os
import reprlib

__all__ = ('LazyError', 'lazy_error_unwrap')

#: Whether new lazy values record the full stack at their creation (see
#: `lazy.trace`). Set from the PCOLLECTIONS_LAZY_TRACE environment variable.
TRACE_DEFAULT = os.environ.get('PCOLLECTIONS_LAZY_TRACE', '').strip().lower() \
    not in ('', '0', 'false', 'no', 'off')

_MAX_CALL_LEN = 200
_MAX_CAUSE_LEN = 200

_repr = reprlib.Repr()
_repr.maxstring = 40
_repr.maxother = 40
_repr.maxlevel = 3


class LazyError(RuntimeError):
    """The error raised when a `lazy` value cannot be computed.

    When the computation raised an exception, that exception is the
    `LazyError`'s `__cause__` (also available as `cause`). A lazy value
    that fails remembers its failure: every later request for it raises a
    new `LazyError` with the same cause, without running the computation
    again. A lazy value whose computation requests the value itself raises a
    `LazyError` with no cause.

    Attributes
    ----------
    cause : BaseException or None
        The exception raised by the computation.
    func, func_args, func_kwargs
        The function of the lazy value and the arguments it was called with.
    origin : tuple or None
        ``(filename, lineno, function_name)`` of the code that created the
        lazy value.
    origin_stack : list of str or None
        The formatted stack at the lazy value's creation, if `lazy.trace`
        was true when it was created.
    """
    # Filled in by _make_error; None for a LazyError constructed directly.
    func = None
    func_args = None
    func_kwargs = None
    origin = None
    origin_stack = None
    _cause_traceback = None

    @property
    def cause(self):
        """The exception that caused this error, or None."""
        return self.__cause__

LazyError.__module__ = 'pcollections'


class LazyErrorUnwrapper:
    """A function and context manager for unwrapping `LazyError` exceptions.

    `lazy_error_unwrap(error)` returns the cause of `error` if `error` is a
    `LazyError` with a cause; otherwise it returns `error` unchanged.

    Used as a context manager, `lazy_error_unwrap` raises the cause of any
    `LazyError` that propagates out of its block in place of the
    `LazyError`.

    Examples
    --------
    >>> from pcollections import lazy_error_unwrap, ldict, lazy
    >>> d = ldict(x=lazy(lambda: 0[0]))
    >>> try:
    ...     with lazy_error_unwrap:
    ...         d['x']
    ... except TypeError:
    ...     print("TypeError raised.")
    TypeError raised.

    Without `lazy_error_unwrap`, `d['x']` raises a `LazyError` whose cause
    is the `TypeError`.
    """
    __slots__ = ()

    def __call__(self, err):
        if isinstance(err, LazyError) and err.__cause__ is not None:
            return err.__cause__
        return err

    def __enter__(self):
        return self

    def __exit__(self, ex_type, ex_val, tb):
        if not isinstance(ex_val, LazyError) or ex_val.__cause__ is None:
            return False
        cause = ex_val.__cause__
        # The cause is shared by every LazyError raised for the same lazy
        # value (possibly in several threads). Raising it here would grow its
        # traceback and set its __context__, so restore both: the traceback
        # to the one it had when the computation failed.
        context = cause.__context__
        suppress = cause.__suppress_context__
        saved_tb = ex_val._cause_traceback
        try:
            raise cause.with_traceback(saved_tb)
        finally:
            cause.__context__ = context
            cause.__suppress_context__ = suppress

    def __repr__(self):
        return 'lazy_error_unwrap'

    def __reduce__(self):
        return 'lazy_error_unwrap'

LazyErrorUnwrapper.__module__ = 'pcollections'
lazy_error_unwrap = LazyErrorUnwrapper()


def capture_stack(frame):
    """Returns the formatted stack ending at `frame` (a list of str)."""
    import traceback
    return traceback.format_stack(frame)


def _safe(fn, obj, default):
    try:
        return fn(obj)
    except Exception:
        return default


def _describe_call(func, args, kwargs):
    name = (getattr(func, '__qualname__', None)
            or getattr(func, '__name__', None))
    if not isinstance(name, str):
        name = _safe(_repr.repr, func, '<function>')
    parts = [_safe(_repr.repr, a, '<?>') for a in (args or ())]
    parts.extend(f"{k}={_safe(_repr.repr, v, '<?>')}"
                 for (k, v) in (kwargs or {}).items())
    s = ', '.join(parts)
    if len(s) > _MAX_CALL_LEN:
        s = s[:_MAX_CALL_LEN - 3] + '...'
    return f"{name}({s})"


def _describe_origin(origin):
    if origin is None:
        return "at an unknown location"
    (filename, lineno, funcname) = origin
    base = os.path.basename(filename) if filename else '<unknown>'
    return f"at {base}:{lineno} in {funcname}()"


def make_error(kind, func, args, kwargs, origin_code, origin_line,
               origin_stack, cause=None, cause_tb=None):
    """Returns a new `LazyError` for a lazy value.

    `kind` is ``'failed'`` (the computation raised `cause`, whose traceback
    at that moment was `cause_tb`) or ``'recursive'`` (the computation
    requested its own value).
    """
    if origin_code is None:
        origin = None
    else:
        origin = (origin_code.co_filename, origin_line, origin_code.co_name)
    call = _describe_call(func, args, kwargs)
    where = _describe_origin(origin)
    if kind == 'recursive':
        msg = f"lazy value created {where} depends on itself (calling {call})"
    else:
        what = _safe(str, cause, '')
        if len(what) > _MAX_CAUSE_LEN:
            what = what[:_MAX_CAUSE_LEN - 3] + '...'
        what = f"{type(cause).__name__}: {what}" if what else \
            type(cause).__name__
        msg = f"lazy value created {where} failed calling {call}: {what}"
    err = LazyError(msg)
    err.func = func
    err.func_args = args
    err.func_kwargs = kwargs
    err.origin = origin
    err.origin_stack = origin_stack
    if cause is not None:
        err.__cause__ = cause
        err.__suppress_context__ = True
        err._cause_traceback = cause_tb
    return err


def ready_lazy(cls, value):
    """Returns a computed lazy value of class `cls` holding `value` (used to
    unpickle lazy values)."""
    return cls._from_value(value)
