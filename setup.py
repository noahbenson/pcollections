#! /usr/bin/env python
################################################################################

import os
from setuptools import setup

# Get the version from __init__.py
with open(os.path.join('pcollections', '__init__.py'), 'rt') as fl:
    lns = fl.readlines()
version = next(ln for ln in lns if "__version__ = " in ln)
version = version.split('"')[1]

setup(
    name='pcollections',
    version=version,
    description='Persistent Collections for Python',
    keywords='persistent immutable functional', 
    author='Noah C. Benson',
    author_email='nben@uw.edu',
    url='https://github.com/noahbenson/pcollections/',
    download_url='https://github.com/noahbenson/pcollections/',
    license='MIT',
    classifiers=[
        'Development Status :: 3 - Alpha',
        'Intended Audience :: Developers',
        'License :: OSI Approved :: MIT License',
        'Operating System :: Microsoft :: Windows',
        'Operating System :: POSIX',
        'Operating System :: Unix',
        'Operating System :: MacOS',
        'Programming Language :: Python :: 3',
        'Programming Language :: Python :: 3.7',
        'Programming Language :: Python :: 3.8',
        'Programming Language :: Python :: 3.9',
        'Programming Language :: Python :: 3.10',
        'Programming Language :: Python :: 3.11',
        'Topic :: Software Development',
        'Topic :: Software Development :: Libraries',
        'Topic :: Software Development :: Libraries :: Python Modules'],
    packages=['pcollections',
              'pcollections.abc',
              'pcollections.util',
              'pcollections.test'],
    package_data={'': ['LICENSE.txt']},
    zip_safe=False,
    include_package_data=True,
    install_requires=['phamt >= 0.1.6'])
