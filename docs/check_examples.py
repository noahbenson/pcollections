"""Runs the interactive examples (>>> lines) in the documentation's Markdown
files as doctests: python docs/check_examples.py"""

import doctest
import pathlib
import sys

failed = 0
for path in sorted(pathlib.Path(__file__).parent.glob('*.md')):
    # Code fences end an example's expected output.
    text = '\n'.join('' if line.startswith('```') else line
                     for line in path.read_text().splitlines())
    test = doctest.DocTestParser().get_doctest(text, {}, path.name,
                                               str(path), 0)
    result = doctest.DocTestRunner().run(test)
    failed += result.failed
    print(f"{path.name}: {result.attempted} examples, {result.failed} failed")
sys.exit(1 if failed else 0)
