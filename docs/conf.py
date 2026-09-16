# Sphinx configuration for the pcollections documentation.

import pcollections

# The documentation describes the C implementation, which must be installed
# (pip install .[docs]).
if not pcollections.using_c_extension:
    raise RuntimeError("the docs must be built with the C backend installed")

project = 'pcollections'
author = 'Noah C. Benson'
copyright = '2022-2026, Noah C. Benson'
release = pcollections.__version__
version = '.'.join(release.split('.')[:2])

extensions = [
    'sphinx.ext.autodoc',
    'sphinx.ext.intersphinx',
    'sphinx.ext.napoleon',
    'sphinx.ext.viewcode',
    'myst_parser',
]
source_suffix = {'.rst': 'restructuredtext', '.md': 'markdown'}
exclude_patterns = ['_build']
myst_heading_anchors = 3

autodoc_member_order = 'bysource'
autodoc_default_options = {
    'members': True,
    'inherited-members': True,
    'undoc-members': False,
}
autodoc_typehints = 'none'
napoleon_use_ivar = True
intersphinx_mapping = {'python': ('https://docs.python.org/3', None)}

html_theme = 'furo'
html_title = f'pcollections {release}'
html_baseurl = 'https://nben.net/pcollections/'
