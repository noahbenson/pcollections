# -*- coding: utf-8 -*-
################################################################################
# pcollections/test/_interp.py
# Tests for using pcollections in subinterpreters.
# By Noah C. Benson

"""Subinterpreter tests.

The C backend keeps its state per interpreter, so each subinterpreter gets
its own, independent pcollections types. These tests create subinterpreters
(sequentially, repeatedly, and concurrently from several threads) and run a
small workload in each.

Each test runs in a child process, so a crash fails the test rather than the
suite. The tests are skipped when the running Python offers no way to create
subinterpreters from Python code.

On Python 3.8, only one interpreter per process can load the C backend, so a
subinterpreter falls back to the pure-Python backend there.
"""

import os
import subprocess
import sys
import sysconfig
import textwrap
import unittest

# True for a free-threaded CPython built against musl (as in musllinux
# wheels). There, os.listdir() (and so importing) intermittently fails with
# BlockingIOError (EAGAIN) while several threads run subinterpreters, whether
# or not they use pcollections.
_FREE_THREADED_MUSL = (
    bool(sysconfig.get_config_var('Py_GIL_DISABLED'))
    and 'musl' in (sysconfig.get_config_var('EXT_SUFFIX') or ''))

_PKG_PARENT = os.path.dirname(os.path.dirname(os.path.dirname(
    os.path.abspath(__file__))))

# Defines run_in_subinterpreter(code) for whichever API this Python has.
_RUNNER = r'''
import sys
try:
    from concurrent import interpreters as _ci
except ImportError:
    _ci = None
if _ci is not None:
    def run_in_subinterpreter(code):
        interp = _ci.create()
        try:
            interp.exec(code)
        finally:
            interp.close()
else:
    try:
        import _interpreters as _xi
    except ImportError:
        import _xxsubinterpreters as _xi
    def run_in_subinterpreter(code):
        iid = _xi.create()
        try:
            if hasattr(_xi, 'run_string'):
                _xi.run_string(iid, code)
            else:
                err = _xi.exec(iid, code)
                if err is not None:
                    raise RuntimeError(f"subinterpreter failed: {err}")
        finally:
            _xi.destroy(iid)
'''

# The workload each subinterpreter runs. It raises on any failure.
_WORKLOAD = r'''
import gc, pickle, sys
import pcollections as pc
assert pc.using_c_extension == EXPECT_C, (pc.using_c_extension, EXPECT_C)
d = pc.pdict((k, str(k)) for k in range(2000))
t = d.transient()
for k in range(0, 2000, 3):
    del t[k]
t['x'] = pc.plist(range(100))
d2 = t.persistent()
assert len(d2) == 2000 - len(range(0, 2000, 3)) + 1
assert d2['x'][99] == 99 and d[0] == '0'
s = pc.pset(range(500)).discard(3).add(-1)
assert 3 not in s and -1 in s
ld = pc.ldict(a=pc.lazy(lambda: 41 + 1))
assert ld['a'] == 42
assert pickle.loads(pickle.dumps(d2)) == d2
del d, t, d2, s, ld
gc.collect()
'''


def _have_subinterpreters():
    for name in ('concurrent.interpreters', '_interpreters',
                 '_xxsubinterpreters'):
        try:
            __import__(name)
            return True
        except ImportError:
            pass
    return False


@unittest.skipUnless(_have_subinterpreters(),
                     "this Python cannot create subinterpreters")
class TestSubinterpreters(unittest.TestCase):
    """Runs pcollections in subinterpreters (C backend, when available)."""

    def run_script(self, body, timeout=600):
        import pcollections
        have_c = pcollections.using_c_extension
        # On 3.8 only the first interpreter to load the C backend gets it.
        py38 = sys.version_info < (3, 9)
        src = (_RUNNER
               + f"WORKLOAD = {_WORKLOAD!r}\n"
               + f"C = {have_c!r}\n"
               + f"LATER = {have_c and not py38!r}\n"
               + "def workload(expect_c):\n"
               + "    return f'EXPECT_C = {expect_c!r}\\n' + WORKLOAD\n"
               + textwrap.dedent(body))
        env = dict(os.environ)
        env['PYTHONPATH'] = os.pathsep.join(
            [_PKG_PARENT] + ([env['PYTHONPATH']] if env.get('PYTHONPATH') else []))
        env['PYTHONFAULTHANDLER'] = '1'
        if py38:
            # The subinterpreters use the pure-Python backend on 3.8, which
            # PCOLLECTIONS_REQUIRE_C would turn into an ImportError.
            env.pop('PCOLLECTIONS_REQUIRE_C', None)
        proc = subprocess.run(
            [sys.executable, '-c', src],
            env=env, capture_output=True, text=True, errors='replace',
            timeout=timeout)
        detail = (f"exit status {proc.returncode}\n"
                  f"--- stdout ---\n{proc.stdout[-3000:]}\n"
                  f"--- stderr ---\n{proc.stderr[-3000:]}")
        self.assertEqual(proc.returncode, 0, detail)
        self.assertEqual(proc.stdout.strip().splitlines()[-1:], ['ok'], detail)

    def test_main_and_subinterpreter(self):
        # The main interpreter loads pcollections first, then a
        # subinterpreter loads its own copy.
        self.run_script("""
            exec(workload(C))
            run_in_subinterpreter(workload(LATER))
            exec(workload(C))
            print('ok')
        """)

    def test_subinterpreter_first(self):
        # A subinterpreter loads pcollections before the main interpreter.
        self.run_script("""
            run_in_subinterpreter(workload(C))
            exec(workload(LATER))
            print('ok')
        """)

    def test_repeated_subinterpreters(self):
        # Creating and destroying many interpreters frees each one's state.
        self.run_script("""
            exec(workload(C))
            for _ in range(20):
                run_in_subinterpreter(workload(LATER))
            exec(workload(C))
            print('ok')
        """)

    # On Python 3.12, creating and destroying subinterpreters from several
    # threads at once corrupts memory inside CPython (in the import system
    # and the collector, with or without pcollections), so this test only
    # runs on other versions.
    # On free-threaded builds that use musl, CPython itself fails
    # intermittently in this test (see _FREE_THREADED_MUSL).
    @unittest.skipIf(sys.version_info[:2] == (3, 12),
                     "concurrent subinterpreters are unreliable in Python 3.12")
    @unittest.skipIf(_FREE_THREADED_MUSL,
                     "concurrent subinterpreters are unreliable in"
                     " free-threaded Python with musl")
    def test_concurrent_subinterpreters(self):
        # Several threads, each running its own interpreters, at once.
        self.run_script("""
            import threading
            exec(workload(C))
            errors = []
            def worker():
                try:
                    for _ in range(5):
                        run_in_subinterpreter(workload(LATER))
                except BaseException as e:
                    errors.append(repr(e))
            threads = [threading.Thread(target=worker) for _ in range(8)]
            for th in threads:
                th.start()
            for th in threads:
                th.join()
            assert not errors, errors
            exec(workload(C))
            print('ok')
        """)
