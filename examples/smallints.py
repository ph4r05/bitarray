#
# Thanks to David Kammeyer for the idea to apply a bitarray in this way.
#
from bitarray import bitarray
from bitarray.utils import int2ba, ba2int


class SmallIntArray(object):
    """
    A class which allows efficiently storeing an array of integers
    represented by a specified number of bits (1..8).
    For example, an array with 1000 5 bit integers can be created,
    allowing each element in the array to take values form 0 to 31,
    while the size of the object is 625 (5000/8) bytes.
    """
    def __init__(self, N, k):
        assert 0 < k <= 8
        self.N = N  # number of integers
        self.k = k  # bits for each integer
        self.data = bitarray(N * k)

    def slice_i(self, i):
        assert 0 <= i < self.N
        return slice(self.k * i, self.k * (i + 1))

    def __getitem__(self, i):
        return ba2int(self.data[self.slice_i(i)])

    def __setitem__(self, i, v):
        assert 0 <= v < 2 ** self.k
        self.data[self.slice_i(i)] = int2ba(v, self.k)


if __name__ == '__main__':
    from random import randint

    # define array with 1000 integers, each represented by 5 bits
    a = SmallIntArray(1000, 5)

    b = [] # store values, for assertion below
    for i in range(1000):
        v = randint(0, 31)
        b.append(v)
        a[i] = v

    print(b[:5])
    print(a.data.buffer_info())
    print(a.data[:25])

    for i in range(1000):
        assert a[i] == b[i]
