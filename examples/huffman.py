"""
The non-trivial part of the code is derived from:
http://en.literateprograms.org/Huffman_coding_(Python)

The link also contains a good description of the algorithm.
"""
from __future__ import print_function

import os
import sys
import heapq
from collections import defaultdict
from bitarray import bitarray

is_py3k = bool(sys.version_info[0] == 3)


def huffCode(freq):
    """
    Given a dictionary mapping symbols to thier frequency,
    return the Huffman code in the form of
    a dictionary mapping the symbols to bitarrays.
    """
    minheap = []
    for i, c in enumerate(sorted(freq)):
        # having the exact same frequency for different symbols causes
        # problems with heapq in Python 3, so we simply add a small float
        heapq.heappush(minheap, (freq[c] + 1E-3 * i, c))

    while len(minheap) > 1:
        childR = heapq.heappop(minheap)
        childL = heapq.heappop(minheap)
        parent = (childL[0] + childR[0], childL, childR)
        heapq.heappush(minheap, parent)

    # minheap[0] is now the root node of the Huffman tree
    # now traverse the tree to create the Huffman codes
    result = {}

    def traverse(tree, prefix=bitarray()):
        if len(tree) == 2:  # leave
            result[tree[1]] = prefix
        else: # parent, so traverse each of the children
            for i in range(2):
                traverse(tree[i+1], prefix + bitarray([i]))

    traverse(minheap[0])
    return result


def freq_string(s):
    """
    Given a string, return a dict mapping characters to thier frequency.
    """
    res = defaultdict(int)
    for c in s:
        res[c] += 1
    return res


def read_file(filename):
    with open(filename, 'rb') as fi:
        return fi.read()


special_ascii = {0: 'NULL', 9: 'TAB', 10: 'LF', 13: 'CR', 127: 'DEL'}
def disp_char(i):
    if 32 <= i < 127:
        return repr(chr(i))
    return special_ascii.get(i, '')

def print_code(filename):
    freq = freq_string(read_file(filename))
    code = huffCode(freq)
    print(' symbol     char    hex   frequency     Huffman code')
    print(70 * '-')
    for c in sorted(code, key=lambda c: (freq[c], c), reverse=True):
        i = c if is_py3k else ord(c)
        print('%7r     %-4s    0x%02x %10i     %s' % (
            c, disp_char(i),
            i, freq[c], code[c].to01()))


def encode(filename):
    s = read_file(filename)
    code = huffCode(freq_string(s))
    with open(filename + '.huff', 'wb') as fo:
        for c in sorted(code):
            fo.write(('%02x %s\n' % (c if is_py3k else ord(c),
                                     code[c].to01())).encode())
        a = bitarray(endian='little')
        a.encode(code, s)
        # write unused bits
        fo.write(b'unused %s\n' % str(a.buffer_info()[3]).encode())
        a.tofile(fo)
    print('Ratio =%6.2f%%' % (100.0 * a.buffer_info()[1] / len(s)))


def decode(filename):
    assert filename.endswith('.huff')
    code = {}

    with open(filename, 'rb') as fi:
        while 1:
            line = fi.readline()
            c, b = line.split()
            if c == b'unused':
                u = int(b)
                break
            i = int(c, 16)
            code[i if is_py3k else chr(i)] = bitarray(b)
        a = bitarray(endian='little')
        a.fromfile(fi)

    if u:
        del a[-u:]

    with open(filename[:-5] + '.out', 'wb') as fo:
        for c in a.iterdecode(code):
            fo.write(chr(c).encode('ISO-8859-1') if is_py3k else c)


def main():
    from optparse import OptionParser

    p = OptionParser("usage: %prog [options] FILE")
    p.add_option(
        '-p', '--print',
        action="store_true",
        help="calculate and print the Huffman code for the "
             "frequency of characters in FILE")
    p.add_option(
        '-e', '--encode',
        action="store_true",
        help="encode FILE using the Huffman code calculated for the "
             "frequency of characters in FILE itself. "
             "The output is FILE.huff which contains both the Huffman "
             "code and the bitarray resulting from the encoding.")
    p.add_option(
        '-d', '--decode',
        action="store_true",
        help="decode FILE.huff and write the output to FILE.out")
    p.add_option(
        '-t', '--test',
        action="store_true",
        help="encode FILE, decode FILE.huff, compare FILE with FILE.out, "
             "and unlink created files.")
    opts, args = p.parse_args()
    if len(args) != 1:
        p.error('exactly one argument required')
    filename = args[0]

    if opts.print:
        print_code(filename)

    if opts.encode:
        encode(filename)

    if opts.decode:
        decode(filename + '.huff')

    if opts.test:
        huff = filename + '.huff'
        out = filename + '.out'
        encode(filename)
        decode(huff)
        assert open(filename, 'rb').read() == open(out, 'rb').read()
        os.unlink(huff)
        os.unlink(out)


if __name__ == '__main__':
    main()
