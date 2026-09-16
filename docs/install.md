# Installation and backends

```sh
pip install pcollections
```

`pcollections` has two implementations, or backends, with the same
interface: a compiled C extension and a much slower pure-Python fallback.
Importing `pcollections` uses the C backend whenever it can be loaded. **An
installation can end up with only the Python backend without failing**, so
check which backend you have (see
[Checking the backend](#checking-the-backend)).

## Supported Python versions

`pcollections` supports CPython 3.8 through 3.15, including the
free-threaded ("no-GIL") builds of 3.13 and later.

```{warning}
Support for Python 3.8 is deprecated. It will be removed in a future minor
release of `pcollections` (1.1 or 1.2, for example). Python 3.8 also has
limited support for subinterpreters (see [Subinterpreters](#subinterpreters)).
```

## Wheels and building from source

`pip` installs a prebuilt wheel, which contains the compiled C backend, for:

- CPython 3.8 through 3.15, and the free-threaded builds of 3.14 and 3.15;
- Linux on x86-64 and ARM64 (glibc and musl), macOS on Intel and Apple
  silicon, and Windows on x86-64.

Elsewhere (Python 3.13t, 32-bit Python, Windows on ARM, or a platform
without wheels), `pip` builds `pcollections` from its source distribution,
which needs a C compiler. **If the C extension doesn't compile, the
installation still succeeds** and `pcollections` uses the Python backend.
The compiler's error appears only in `pip`'s verbose output
(`pip install -v`).

To make sure that you get the C backend, do one of these when installing:

- Refuse to build from source: `pip install --only-binary pcollections
  pcollections`. This fails if no wheel matches your Python and platform.
- Require the C extension when building from source:
  `PCOLLECTIONS_REQUIRE_C=1 pip install --no-cache-dir pcollections`. The
  installation then fails if the C extension doesn't compile. (`pip` may
  otherwise reuse a wheel that it built and cached earlier.)

## Checking the backend

```sh
python -c "import pcollections; print(pcollections.using_c_extension)"
```

prints `True` for the C backend and `False` for the Python backend. In code:

- `pcollections.using_c_extension` is `True` when the C backend is in use.
- `pcollections.backend_error` is the exception that prevented the C backend
  from loading, or `None`.
- When the C backend can't be loaded, importing `pcollections` issues a
  `RuntimeWarning` that includes the reason.

## Choosing the backend

Two environment variables choose the backend. `pcollections` reads them when
it is imported, and `setup.py` reads them when it is built:

| Variable | When importing | When building |
|---|---|---|
| `PCOLLECTIONS_REQUIRE_C=1` | Importing fails with `ImportError` if the C backend can't be loaded. | The build fails if the C extension doesn't compile. |
| `PCOLLECTIONS_NO_C_EXTENSIONS=1` | The Python backend is used, without a warning. | The C extension isn't built. |

Setting both is an error. For a deployment, a service, or a test suite that
depends on the C backend's speed, set `PCOLLECTIONS_REQUIRE_C=1`, so that a
missing C backend is an error rather than a slowdown.

The choice applies to the whole process: the lazy collections are
subclasses of the persistent collections of the same backend, so the two
backends are never mixed. Objects pickled with one backend can be unpickled
with the other.

## Subinterpreters

The C backend keeps its state per interpreter, so each interpreter that
imports `pcollections` has its own types, and the C backend supports
interpreters with their own GIL (Python 3.12 and later). As with any Python
objects, a `pcollections` object belongs to the interpreter that created it.
(Python 3.12 itself can corrupt memory when several threads create and
destroy subinterpreters at the same time, whether or not they use
`pcollections`; Python 3.13 and later don't.)

```{warning}
On Python 3.8, only the first interpreter in a process that imports
`pcollections` gets the C backend. Every other interpreter, whether a
subinterpreter or the main interpreter, uses the Python backend and issues a
`RuntimeWarning` (or, if `PCOLLECTIONS_REQUIRE_C` is set, fails to import
`pcollections`). This remains so after the first interpreter exits.
```

## Running the tests

```sh
python -m pcollections.test
```

runs the test suite against every backend that can be loaded. The
randomized stress tests run 100,000 operations each; set
`PCOLLECTIONS_STRESS_STEPS` to change that, and `PCOLLECTIONS_STRESS_SEED`
to an integer to change the random sequence.
