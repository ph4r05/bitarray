"""
Tests for bitarray

Author: Ilan Schnell
"""
import os
import sys
import unittest
import tempfile
import shutil
from random import randint

is_py3k = bool(sys.version_info[0] == 3)

if is_py3k:
    from io import StringIO
else:
    from cStringIO import StringIO


from bitarray import bitarray, bitdiff, bits2bytes, __version__


tests = []

if sys.version_info[:2] < (2, 6):
    def next(x):
        return x.next()


def to_bytes(s):
    if is_py3k:
        return bytes(s.encode('latin1'))
    elif sys.version_info[:2] >= (2, 6):
        return bytes(s)
    else:
        return s


class Util(object):

    def randombitarrays(self):
        for n in list(range(25)) + [randint(1000, 2000)]:
            a = bitarray(endian=['little', 'big'][randint(0, 1)])
            a.frombytes(os.urandom(bits2bytes(n)))
            del a[n:]
            yield a

    def randomlists(self):
        for n in list(range(25)) + [randint(1000, 2000)]:
            yield [bool(randint(0, 1)) for d in range(n)]

    def rndsliceidx(self, length):
        if randint(0, 1):
            return None
        else:
            return randint(-2 * length, 2 * length - 1)

    def slicelen(self, r, length):
        return getIndicesEx(r, length)[-1]

    def check_obj(self, a):
        self.assertEqual(repr(type(a)), "<class 'bitarray.bitarray'>")
        unused = 8 * a.buffer_info()[1] - len(a)
        self.assert_(0 <= unused < 8)
        self.assertEqual(unused, a.buffer_info()[3])

    def assertEQUAL(self, a, b):
        self.assertEqual(a, b)
        self.assertEqual(a.endian(), b.endian())
        self.check_obj(a)
        self.check_obj(b)

    def assertStopIteration(self, it):
        if is_py3k:
            return
        self.assertRaises(StopIteration, it.next)


def getIndicesEx(r, length):
    if not isinstance(r, slice):
        raise TypeError("slice object expected")
    start = r.start
    stop  = r.stop
    step  = r.step
    if r.step is None:
        step = 1
    else:
        if step == 0:
            raise ValueError("slice step cannot be zero")

    if step < 0:
        defstart = length - 1
        defstop = -1
    else:
        defstart = 0
        defstop = length

    if r.start is None:
        start = defstart
    else:
        if start < 0: start += length
        if start < 0: start = [0, -1][step < 0]
        if start >= length: start = [length, length - 1][step < 0]

    if r.stop is None:
        stop = defstop
    else:
        if stop < 0: stop += length
        if stop < 0: stop = -1
        if stop > length: stop = length

    if (step < 0 and stop >= length) or (step > 0 and start >= stop):
        slicelength = 0
    elif step < 0:
        slicelength = (stop - start + 1) / step + 1
    else:
        slicelength = (stop - start - 1) / step + 1

    if slicelength < 0:
        slicelength = 0

    return start, stop, step, slicelength

# ---------------------------------------------------------------------------

class SliceTests(unittest.TestCase, Util):

    def test_setitem4(self):
        for a in self.randombitarrays():
            la = len(a)
            if la == 0: continue
            for dum in range(100):
                step = self.rndsliceidx(la)
                if step == 0: step = None
                s = slice(self.rndsliceidx(la),
                          self.rndsliceidx(la), step)
                for b in self.randombitarrays():
                    if len(b) == self.slicelen(s, len(a)) or step is None:
                        c = bitarray(a)
                        d = c
                        c[s] = b
                        self.assert_(c is d)
                        self.check_obj(c)
                        cc = a.tolist()
                        cc[s] = b.tolist()
                        if c != bitarray(cc):
                            print("Error, slice: %s, b: %s, a: %s, c: %s" % (s, b, a, c))
                        self.assertEqual(c, bitarray(cc))

    def test_delitem2(self):
        for a in self.randombitarrays():
            la = len(a)
            if la == 0: continue
            for dum in range(300):
                step = self.rndsliceidx(la)
                if step == 0: step = None
                s = slice(self.rndsliceidx(la),
                          self.rndsliceidx(la), step)
                c = bitarray(a)
                d = c
                del c[s]
                self.assert_(c is d)
                self.check_obj(c)
                cc = a.tolist()
                del cc[s]
                self.assertEQUAL(c, bitarray(cc, endian=c.endian()))


tests.append(SliceTests)

# ---------------------------------------------------------------------------

def run(verbosity=1, repeat=1):
    print('bitarray is installed in: %s' % os.path.dirname(__file__))
    print('bitarray version: %s' % __version__)
    print('Python version: %s' % sys.version)

    suite = unittest.TestSuite()
    for cls in tests:
        for _ in range(repeat):
            suite.addTest(unittest.makeSuite(cls))

    runner = unittest.TextTestRunner(verbosity=verbosity)
    return runner.run(suite)


if __name__ == '__main__':
    run()
