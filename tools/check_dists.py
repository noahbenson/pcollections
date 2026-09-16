#! /usr/bin/env python
################################################################################
# tools/check_dists.py
# Checks the distributions built for a release before they are published.
# By Noah C. Benson

"""Checks a directory of pcollections distributions before a release.

    python tools/check_dists.py DIST_DIR [--version VERSION]
                                [--pythons TAG ...] [--platforms NAME ...]

Checks that:

- there is exactly one sdist, with the expected version, the C sources, the
  type stubs, and the license;
- there is exactly one wheel for each expected Python build (cp312, say, or
  cp314t for a free-threaded build) and platform, and no other wheels (in
  particular, no pure-Python wheel);
- each wheel has the expected version, is not marked as pure Python, and
  contains exactly one C extension module, pcollections._c._core, built for
  the wheel's Python version and platform, along with the pure-Python
  modules and the type stubs.

The version defaults to the one in src/pcollections/__init__.py. Exits with
status 1, after listing every problem, if any check fails.
"""

import argparse
import os
import re
import sys
import tarfile
import zipfile

# The wheels a release must include: every CPython in PYTHONS on every
# platform in PLATFORMS (see pyproject.toml's [tool.cibuildwheel] and
# .github/workflows/deploy.yml).
PYTHONS = ['cp38', 'cp39', 'cp310', 'cp311', 'cp312', 'cp313', 'cp314',
           'cp315', 'cp314t', 'cp315t']
# name: (regex matching the wheel's platform tag, the kind of platform, which
# determines the extension module's file name; see extension_pattern()).
PLATFORMS = {
    'manylinux-x86_64': (r'manylinux[_0-9a-z.]*_x86_64', 'linux-x86_64'),
    'musllinux-x86_64': (r'musllinux_\d+_\d+_x86_64', 'linux-x86_64'),
    'manylinux-aarch64': (r'manylinux[_0-9a-z.]*_aarch64', 'linux-aarch64'),
    'musllinux-aarch64': (r'musllinux_\d+_\d+_aarch64', 'linux-aarch64'),
    'macos-x86_64': (r'macosx_\d+_\d+_x86_64', 'darwin'),
    'macos-arm64': (r'macosx_\d+_\d+_arm64', 'darwin'),
    'windows-amd64': (r'win_amd64', 'win_amd64'),
    # For testing this script with locally built (unrepaired) wheels.
    'local-linux-x86_64': (r'linux_x86_64', 'linux-x86_64'),
}
RELEASE_PLATFORMS = [name for name in PLATFORMS if not name.startswith('local')]

WHEEL_RE = re.compile(
    r'^pcollections-(?P<version>[^-]+)-(?P<py>cp\d+)-(?P<abi>cp\d+t?)'
    r'-(?P<plat>[^-]+)\.whl$')
REQUIRED_WHEEL_FILES = [
    'pcollections/__init__.py', 'pcollections/__init__.pyi',
    'pcollections/py.typed', 'pcollections/_dict.py', 'pcollections/_list.py',
    'pcollections/_set.py', 'pcollections/_lazy.py',
    'pcollections/_lazybase.py', 'pcollections/abc/__init__.py',
    'pcollections/test/__init__.py',
]
REQUIRED_SDIST_FILES = [
    'LICENSE', 'README.md', 'CHANGELOG.md', 'pyproject.toml', 'setup.py',
    'PKG-INFO', 'src/pcollections/__init__.py',
    'src/pcollections/__init__.pyi', 'src/pcollections/py.typed',
    'src/pcollections/_c/_core.c', 'src/pcollections/_c/core.h',
    'src/pcollections/_c/trie.h', 'src/pcollections/_c/amt.h',
    'src/pcollections/_c/fat.h', 'src/pcollections/_c/uintbits.h',
    'src/pcollections/_c/dict.c.h', 'src/pcollections/_c/list.c.h',
    'src/pcollections/_c/set.c.h', 'src/pcollections/_c/lazy.c.h',
]


def source_version():
    here = os.path.dirname(os.path.abspath(__file__))
    path = os.path.join(here, '..', 'src', 'pcollections', '__init__.py')
    with open(path) as f:
        for line in f:
            if line.startswith('__version__'):
                return line.split('"')[1]
    raise RuntimeError(f"no __version__ in {path}")


def metadata_version(text):
    m = re.search(r'^Version: (.+)$', text, re.M)
    return m.group(1).strip() if m else None


def extension_pattern(pytag, platform_kind):
    """The file name that pcollections._c._core has in a wheel for Python
    `pytag` (such as cp312 or cp314t)."""
    m = re.fullmatch(r'cp(\d)(\d+)(t?)', pytag)
    (major, minor, ft) = m.groups()
    if platform_kind == 'win_amd64':
        return re.escape(f'_core.cp{major}{minor}{ft}-win_amd64.pyd')
    if platform_kind == 'darwin':
        tail = r'-darwin\.so'
    else:
        (_, arch) = platform_kind.split('-')
        tail = rf'-{arch}-linux-(gnu|musl)\.so'
    return rf'_core\.cpython-{major}{minor}{ft}{tail}'


def check_sdist(path, version, problems):
    with tarfile.open(path) as tf:
        names = tf.getnames()
        prefix = f'pcollections-{version}/'
        for required in REQUIRED_SDIST_FILES:
            if prefix + required not in names:
                problems.append(f"{os.path.basename(path)}: missing {required}")
        member = tf.extractfile(prefix + 'PKG-INFO')
        got = metadata_version(member.read().decode()) if member else None
        if got != version:
            problems.append(f"{os.path.basename(path)}: PKG-INFO version "
                            f"{got!r}, expected {version!r}")


def check_wheel(path, m, platform_kind, problems):
    name = os.path.basename(path)
    with zipfile.ZipFile(path) as zf:
        names = zf.namelist()
        info = f"pcollections-{m['version']}.dist-info/"
        try:
            wheel = zf.read(info + 'WHEEL').decode()
            metadata = zf.read(info + 'METADATA').decode()
        except KeyError as e:
            problems.append(f"{name}: missing {e}")
            return
    if 'Root-Is-Purelib: false' not in wheel:
        problems.append(f"{name}: not marked as a platform wheel")
    tags = re.findall(r'^Tag: (.+)$', wheel, re.M)
    expected_tag = f"{m['py']}-{m['abi']}-"
    if not tags or any(not t.startswith(expected_tag) for t in tags):
        problems.append(f"{name}: WHEEL tags {tags} don't match the file name")
    if metadata_version(metadata) != m['version']:
        problems.append(f"{name}: METADATA version "
                        f"{metadata_version(metadata)!r}")
    for required in REQUIRED_WHEEL_FILES:
        if required not in names:
            problems.append(f"{name}: missing {required}")
    extensions = [n for n in names
                  if n.endswith(('.so', '.pyd', '.dylib', '.dll'))
                  and not n.startswith(('pcollections.libs/',
                                        'pcollections-' + m['version']))]
    core = [n for n in extensions if n.startswith('pcollections/_c/_core')]
    if len(core) != 1:
        problems.append(f"{name}: expected one pcollections/_c/_core "
                        f"extension, found {core}")
    else:
        pattern = extension_pattern(m['abi'], platform_kind)
        if not re.fullmatch(pattern, core[0].rsplit('/', 1)[1]):
            problems.append(f"{name}: extension {core[0]} doesn't match "
                            f"the wheel's Python and platform")
    others = [n for n in extensions if n not in core]
    if others:
        problems.append(f"{name}: unexpected binary files {others}")


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__.split('\n')[0])
    parser.add_argument('dist_dir')
    parser.add_argument('--version', default=None)
    parser.add_argument('--pythons', nargs='+', default=PYTHONS)
    parser.add_argument('--platforms', nargs='+', default=RELEASE_PLATFORMS,
                        choices=sorted(PLATFORMS))
    args = parser.parse_args(argv)
    version = args.version or source_version()
    problems = []
    files = sorted(os.listdir(args.dist_dir))
    sdists = [f for f in files if f.endswith('.tar.gz')]
    wheels = [f for f in files if f.endswith('.whl')]
    others = [f for f in files if f not in sdists and f not in wheels]
    if others:
        problems.append(f"unexpected files: {others}")
    if sdists != [f'pcollections-{version}.tar.gz']:
        problems.append(f"expected one sdist for version {version}, "
                        f"found {sdists}")
    else:
        check_sdist(os.path.join(args.dist_dir, sdists[0]), version, problems)
    # Match each wheel to an expected (python, platform) pair.
    found = {}
    for w in wheels:
        m = WHEEL_RE.match(w)
        if not m:
            problems.append(f"{w}: not a CPython platform wheel")
            continue
        if m['version'] != version:
            problems.append(f"{w}: version {m['version']}, expected {version}")
        # The ABI tag names the build: cp314 or, free-threaded, cp314t.
        build = m['abi']
        if build.rstrip('t') != m['py']:
            problems.append(f"{w}: Python tag and ABI tag don't match")
        platforms = [p for p in args.platforms
                     if re.fullmatch(PLATFORMS[p][0], m['plat'])]
        if len(platforms) != 1 or build not in args.pythons:
            problems.append(f"{w}: not an expected Python and platform")
            continue
        key = (build, platforms[0])
        found.setdefault(key, []).append(w)
        check_wheel(os.path.join(args.dist_dir, w), m,
                    PLATFORMS[platforms[0]][1], problems)
    for py in args.pythons:
        for plat in args.platforms:
            ws = found.get((py, plat), [])
            if len(ws) != 1:
                problems.append(f"{py} on {plat}: expected one wheel, "
                                f"found {ws}")
    expected = len(args.pythons) * len(args.platforms)
    print(f"Checked {len(sdists)} sdist(s) and {len(wheels)} wheel(s) "
          f"(expected 1 and {expected}) for version {version}.")
    if problems:
        print(f"{len(problems)} problem(s):")
        for p in problems:
            print("  - " + p)
        return 1
    print("All distributions look right.")
    return 0


if __name__ == '__main__':
    sys.exit(main())
