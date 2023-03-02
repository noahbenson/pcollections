# pcollections

A persistent collections library for Python.


## About

`pcollections` is a library of persistent (immutable) collections inspired by
the immutable data structures of [Clojure](clojure.org) but built to resemble
the native Python collections as closely as possible. The library is implemented
in Python but employs the [`phamt`](https://github.com/noahbenson/phamt)
(Persistent Hash Array Mapped Tries) library, which is implemented in C, to
perform efficient low-level operations.

The library implements three persistent types: `plist`, `pset`, and
`pdict`. These are immutable versions of the builtin `list`, `set`, and `dict`
types. The persistent object interfaces are as similar as possible to the native
types, but the method signatures differ in ways necessary to accomodate
efficient immutable ways of doing things. For example, the `pdict` constructor
is identical to the `dict` constructor and always returns a `pdict` equal to the
`dict` that would be created with the same arguments. However, instead of
supporting operations like `d[key] = val`, `pdicts` support a `set` method: `d =
d.set(key, val)`.

In addition to the persistent types, there are two lazy types, `llist` and
`ldict`. These types are enabled by the `lazy` type. A `lazy` object is
basically a `partial` object that, when called, caches the function's return
value and returns that value without rerunning the function on subsequent
calls. The `llist` and `ldict `types are equivalent to the `plist` and `pdict`
types with one exception. Elements of an `llist` and values of an `ldict` that
are of the `lazy` type are dereferenced when requested. This allows a programmer
to easily create data structures (potentially nested data structures) whose
items are the results of complex or long-running computations that only get
computed once requested. The persistent data structures allow the arguments to
these lazy functions to be safe from mutation.

Finally, the persistent and lazy types have transient correlaries that enable
more efficient batch-mutation of the persistent types. The transient types
`tlist`, `tset`, `tdict`, `tllist`, and `tldict` all have interfaces equivalent
to their standard mutable correlaries (transient types are mutable).


## License

MIT License

Copyright (c) 2022-2023 Noah C. Benson

Permission is hereby granted, free of charge, to any person obtaining a copy
of this software and associated documentation files (the "Software"), to deal
in the Software without restriction, including without limitation the rights
to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
copies of the Software, and to permit persons to whom the Software is
furnished to do so, subject to the following conditions:

The above copyright notice and this permission notice shall be included in all
copies or substantial portions of the Software.

THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
SOFTWARE.


