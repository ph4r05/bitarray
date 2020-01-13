import re
from os.path import join
from distutils.core import setup, Extension


kwds = {}
try:
    kwds['long_description'] = open('README.md').read()
except IOError:
    pass

# Read version from bitarray/__init__.py
pat = re.compile(r'__version__\s*=\s*(\S+)', re.M)
data = open(join('bitarray', '__init__.py')).read()
kwds['version'] = eval(pat.search(data).group(1))


setup(
    name = "bitarray_ph4",
    author = "Ilan Schnell",
    author_email = "ilanschnell@gmail.com",
    maintainer = "Dusan Klinec (ph4r05)",
    maintainer_email = "dusan.klinec@gmail.com",
    url = "https://github.com/ph4r05/bitarray",
    license = "PSF",
    classifiers = [
        "License :: OSI Approved :: Python Software Foundation License",
        "Development Status :: 5 - Production/Stable",
        "Intended Audience :: Developers",
        "Operating System :: OS Independent",
        "Programming Language :: C",
        "Programming Language :: Python :: 2",
        "Programming Language :: Python :: 2.6",
        "Programming Language :: Python :: 2.7",
        "Programming Language :: Python :: 3",
        "Programming Language :: Python :: 3.3",
        "Programming Language :: Python :: 3.4",
        "Programming Language :: Python :: 3.5",
        "Programming Language :: Python :: 3.6",
        "Programming Language :: Python :: 3.7",
        "Programming Language :: Python :: 3.8",
        "Topic :: Utilities",
    ],
    description = "efficient arrays of booleans -- C extension",
    packages = ["bitarray"],
    ext_modules = [Extension(name = "bitarray._bitarray",
                             sources = ["bitarray/_bitarray.c"]),
                   Extension(name = "bitarray._util",
                             sources = ["bitarray/_util.c"])],
    **kwds
)
