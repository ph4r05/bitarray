/*
   Copyright (c) 2008 - 2019, Ilan Schnell
   bitarray is published under the PSF license.

   This file is the C part of the bitarray package.
   All functionality is implemented here.

   Author: Ilan Schnell
*/
#if (defined(__GNUC__) || defined(__clang__))
#define HAS_VECTORS 1
#else
#define HAS_VECTORS 0
#endif

#if HAS_VECTORS
typedef char vec __attribute__((vector_size(16)));
#endif

#if (defined(__GNUC__) || defined(__clang__))
#define ATTR_UNUSED __attribute__((__unused__))
#else
#define ATTR_UNUSED
#endif

#define UNUSEDVAR(x) (void)x

#define PY_SSIZE_T_CLEAN
#include "Python.h"

#if PY_MAJOR_VERSION >= 3
#define IS_PY3K
#endif

#ifdef IS_PY3K
#define Py_TPFLAGS_HAVE_WEAKREFS  0
#endif

#if PY_MAJOR_VERSION == 3 || (PY_MAJOR_VERSION == 2 && PY_MINOR_VERSION == 7)
/* (new) buffer protocol */
#define WITH_BUFFER
#endif

#ifdef STDC_HEADERS
#include <stddef.h>
#else  /* !STDC_HEADERS */
#ifdef HAVE_SYS_TYPES_H
#include <sys/types.h>      /* For size_t */
#endif /* HAVE_SYS_TYPES_H */
#endif /* !STDC_HEADERS */


typedef long long int idx_t;

/* throughout:  0 = little endian   1 = big endian */
#define DEFAULT_ENDIAN  1

/* Unlike the normal convention, ob_size is the byte count, not the number
   of elements.  The reason for doing this is that we can use our own
   special idx_t for the number of bits (which can exceed 2^32 on a 32 bit
   machine.  */
typedef struct {
    PyObject_VAR_HEAD
    char *ob_item;
    Py_ssize_t allocated;       /* how many bytes allocated */
    idx_t nbits;                /* length of bitarray, i.e. elements */
    int endian;                 /* bit endianness of bitarray */
    int ob_exports;             /* how many buffer exports */
    PyObject *weakreflist;      /* list of weak references */
} bitarrayobject;

static PyTypeObject Bitarraytype;

#define bitarray_Check(obj)  PyObject_TypeCheck(obj, &Bitarraytype)

#define BITS(bytes)  ((idx_t) (bytes) << 3)

/* number of bytes necessary to store given bits */
#define BYTES(bits)  (((bits) == 0) ? 0 : (((bits) - 1) / 8 + 1))

#define BITMASK(endian, i)  (((char) 1) << ((endian) ? (7 - (i)%8) : (i)%8))

/* ------------ low level access to bits in bitarrayobject ------------- */

#ifndef NDEBUG
static inline int GETBIT(bitarrayobject *self, idx_t i) {
    assert(0 <= i && i < self->nbits);
    return ((self)->ob_item[(i) / 8] & BITMASK((self)->endian, i) ? 1 : 0);
}
#else
#define GETBIT(self, i)  \
    ((self)->ob_item[(i) / 8] & BITMASK((self)->endian, i) ? 1 : 0)
#endif

#if HAS_VECTORS
/*
 * Perform bitwise operation OP on 16 bytes of memory at a time.
 */
#define vector_op(A, B, OP) do {  \
    vec __a, __b, __r;            \
    memcpy(&__a, A, sizeof(vec)); \
    memcpy(&__b, B, sizeof(vec)); \
    __r = __a OP __b;             \
    memcpy(A, &__r, sizeof(vec)); \
} while(0);
#endif

static void
setbit(bitarrayobject *self, idx_t i, int bit)
{
    char *cp, mask;

    assert(0 <= i && i < BITS(Py_SIZE(self)));
    mask = BITMASK(self->endian, i);
    cp = self->ob_item + i / 8;
    if (bit)
        *cp |= mask;
    else
        *cp &= ~mask;
}

static int inline
check_overflow(idx_t nbits)
{
    assert(nbits >= 0);
    if (sizeof(void *) == 4) {  /* 32bit system */
        const idx_t max_bits = ((idx_t) 1) << 34;  /* 2^34 = 16 Gbits*/
        if (nbits > max_bits) {
            char buff[256];
            sprintf(buff, "cannot create bitarray of size %lld, "
                          "max size is %lld", nbits, max_bits);
            PyErr_SetString(PyExc_OverflowError, buff);
            return -1;
        }
    }
    return 0;
}

static int
resize(bitarrayobject *self, idx_t nbits)
{
    Py_ssize_t newsize;
    size_t new_allocated;
    Py_ssize_t allocated = self->allocated;

    if (check_overflow(nbits) < 0)
        return -1;
    newsize = (Py_ssize_t) BYTES(nbits);

    /* Bypass realloc() when a previous overallocation is large enough
       to accommodate the newsize.  If the newsize falls lower than half
       the allocated size, then proceed with the realloc() to shrink.
    */
    if (allocated >= newsize && newsize >= (allocated >> 1)) {
        assert(self->ob_item != NULL || newsize == 0);
        Py_SIZE(self) = newsize;
        self->nbits = nbits;
        return 0;
    }

    new_allocated = (size_t) newsize;
    if (newsize < Py_SIZE(self) + 65536)
        /* Over-allocate unless the size increase is very large.
           This over-allocates proportional to the bitarray size, making
           room for additional growth.
           The growth pattern is:  0, 4, 8, 16, 25, 34, 44, 54, 65, 77, ...
           Note, the pattern starts out the same as for lists but then
           grows at a smaller rate so that larger bitarrays only overallocate
           by about 1/16th -- this is done because bitarrays are assumed
           to be memory critical.
        */
        new_allocated += (newsize >> 4) + (newsize < 8 ? 3 : 7);

    if (newsize == 0)
        new_allocated = 0;
    self->ob_item = PyMem_Realloc(self->ob_item, new_allocated);
    if (self->ob_item == NULL) {
        PyErr_NoMemory();
        return -1;
    }
    Py_SIZE(self) = newsize;
    self->allocated = new_allocated;
    self->nbits = nbits;
    return 0;
}

/* create new bitarray object without initialization of buffer */
static PyObject *
newbitarrayobject(PyTypeObject *type, idx_t nbits, int endian)
{
    bitarrayobject *obj;
    Py_ssize_t nbytes;

    if (check_overflow(nbits) < 0)
        return NULL;

    obj = (bitarrayobject *) type->tp_alloc(type, 0);
    if (obj == NULL)
        return NULL;

    nbytes = (Py_ssize_t) BYTES(nbits);
    Py_SIZE(obj) = nbytes;
    obj->nbits = nbits;
    obj->endian = endian;
    if (nbytes == 0) {
        obj->ob_item = NULL;
    }
    else {
        obj->ob_item = (char *) PyMem_Malloc((size_t) nbytes);
        if (obj->ob_item == NULL) {
            PyObject_Del(obj);
            PyErr_NoMemory();
            return NULL;
        }
    }
    obj->allocated = nbytes;
    obj->weakreflist = NULL;
    return (PyObject *) obj;
}

static void
bitarray_dealloc(bitarrayobject *self)
{
    if (self->weakreflist != NULL)
        PyObject_ClearWeakRefs((PyObject *) self);

    if (self->ob_item != NULL)
        PyMem_Free((void *) self->ob_item);

    Py_TYPE(self)->tp_free((PyObject *) self);
}

/* copy n bits from other (starting at b) onto self (starting at a) */
static void
copy_n(bitarrayobject *self, idx_t a,
       bitarrayobject *other, idx_t b, idx_t n)
{
    idx_t i;

    assert(0 <= n && n <= self->nbits && n <= other->nbits);
    assert(0 <= a && a <= self->nbits - n);
    assert(0 <= b && b <= other->nbits - n);
    if (n == 0)
        return;

    /* When the start positions are at byte positions, we can copy whole
       bytes using memmove, and copy the remaining few bits individually.
       Note that the order of these two operations matters when copying
       self to self. */
    if (self->endian == other->endian && a % 8 == 0 && b % 8 == 0 && n >= 8)
    {
        const Py_ssize_t bytes = (Py_ssize_t) n / 8;
        const idx_t bits = BITS(bytes);

        assert(bits <= n && n < bits + 8);
        if (a <= b)
            memmove(self->ob_item + a / 8, other->ob_item + b / 8, bytes);

        if (n != bits)
            copy_n(self, bits + a, other, bits + b, n - bits);

        if (a > b)
            memmove(self->ob_item + a / 8, other->ob_item + b / 8, bytes);

        return;
    }

    /* The two different types of looping are only relevant when copying
       self to self, i.e. when copying a piece of an bitarrayobject onto
       itself. */
    if (a <= b) {
        for (i = 0; i < n; i++)             /* loop forward (delete) */
            setbit(self, i + a, GETBIT(other, i + b));
    }
    else {
        for (i = n - 1; i >= 0; i--)      /* loop backwards (insert) */
            setbit(self, i + a, GETBIT(other, i + b));
    }
}

/* starting at start, delete n bits from self */
static int
delete_n(bitarrayobject *self, idx_t start, idx_t n)
{
    assert(0 <= start && start <= self->nbits);
    assert(0 <= n && n <= self->nbits - start);
    if (n == 0)
        return 0;

    copy_n(self, start, self, start + n, self->nbits - start - n);
    return resize(self, self->nbits - n);
}

/* starting at start, insert n (uninitialized) bits into self */
static int
insert_n(bitarrayobject *self, idx_t start, idx_t n)
{
    assert(0 <= start && start <= self->nbits);
    assert(n >= 0);
    if (n == 0)
        return 0;

    if (resize(self, self->nbits + n) < 0)
        return -1;
    copy_n(self, start + n, self, start, self->nbits - start - n);
    return 0;
}

/* sets ususet bits to 0, i.e. the ones in the last byte (if any),
   and return the number of bits set -- self->nbits is unchanged */
static int
setunused(bitarrayobject *self)
{
    const idx_t n = BITS(Py_SIZE(self));
    idx_t i;
    int res = 0;

    for (i = self->nbits; i < n; i++) {
        setbit(self, i, 0);
        res++;
    }
    assert(res < 8);
    return res;
}

/* repeat self n times */
static int
repeat(bitarrayobject *self, idx_t n)
{
    idx_t nbits, i;

    if (n <= 0) {
        if (resize(self, 0) < 0)
            return -1;
    }
    if (n > 1) {
        nbits = self->nbits;
        if (resize(self, nbits * n) < 0)
            return -1;
        for (i = 1; i < n; i++)
            copy_n(self, i * nbits, self, 0, nbits);
    }
    return 0;
}


enum op_type {
    OP_and,
    OP_or,
    OP_xor,
};

/* perform bitwise in-place operation */
static int
bitwise(bitarrayobject *self, PyObject *arg, enum op_type oper)
{
    bitarrayobject *other;
    Py_ssize_t i = 0;

    if (!bitarray_Check(arg)) {
        PyErr_SetString(PyExc_TypeError,
                        "bitarray object expected for bitwise operation");
        return -1;
    }
    other = (bitarrayobject *) arg;
    if (self->nbits != other->nbits) {
        PyErr_SetString(PyExc_ValueError,
               "bitarrays of equal length expected for bitwise operation");
        return -1;
    }
    setunused(self);
    setunused(other);
    Py_ssize_t size = Py_SIZE(self);

#if HAS_VECTORS
#define BITWISE_VECTOR_OP(OP)                                     \
  for (; (Py_ssize_t)(i + sizeof(vec)) < size; i += sizeof(vec))  \
    vector_op(self->ob_item + i, other->ob_item + i, OP)
#else
#define BITWISE_VECTOR_OP(OP)
#endif

    switch (oper) {
    case OP_and:
        BITWISE_VECTOR_OP(&)
        for (; i < size; ++i)
            self->ob_item[i] &= other->ob_item[i];
        break;
    case OP_or:
        BITWISE_VECTOR_OP(|)
        for (; i < size; ++i)
            self->ob_item[i] |= other->ob_item[i];
        break;
    case OP_xor:
        BITWISE_VECTOR_OP(^)
        for (; i < size; ++i)
            self->ob_item[i] ^= other->ob_item[i];
        break;
    default:  /* should never happen */
        return -1;
    }
    return 0;
#undef VECTOR_BITWISE_OP
}

/* set the bits from start to stop (excluding) in self to val */
static void
setrange(bitarrayobject *self, idx_t start, idx_t stop, int val)
{
    idx_t i;

    assert(0 <= start && start <= self->nbits);
    assert(0 <= stop && stop <= self->nbits);

    if (self->nbits == 0 || start >= stop)
        return;

    if (stop >= start + 8) {
        const Py_ssize_t byte_start = BYTES(start);
        const Py_ssize_t byte_stop = (Py_ssize_t) stop / 8;
        for (i = start; i < BITS(byte_start); i++)
            setbit(self, i, val);
        memset(self->ob_item + byte_start, val ? 0xff : 0x00,
               byte_stop - byte_start);
        for (i = BITS(byte_stop); i < stop; i++)
            setbit(self, i, val);
    }
    else {
        for (i = start; i < stop; i++)
            setbit(self, i, val);
    }
}

static void
invert(bitarrayobject *self)
{
    Py_ssize_t i;

    for (i = 0; i < Py_SIZE(self); i++)
        self->ob_item[i] = ~self->ob_item[i];
}

/* reverse the order of bits in each byte of the buffer */
static void
bytereverse(bitarrayobject *self)
{
    static char trans[256];
    static int setup = 0;
    Py_ssize_t i;
    unsigned char c;

    if (!setup) {
        /* setup translation table, which maps each byte to it's reversed:
           trans = {0, 128, 64, 192, 32, 160, ..., 255} */
        int j, k;
        for (k = 0; k < 256; k++) {
            trans[k] = 0x00;
            for (j = 0; j < 8; j++)
                if (1 << (7 - j) & k)
                    trans[k] |= 1 << j;
        }
        setup = 1;
    }

    setunused(self);
    for (i = 0; i < Py_SIZE(self); i++) {
        c = self->ob_item[i];
        self->ob_item[i] = trans[c];
    }
}


static int bitcount_lookup[256] = {
    0, 1, 1, 2, 1, 2, 2, 3, 1, 2, 2, 3, 2, 3, 3, 4,
    1, 2, 2, 3, 2, 3, 3, 4, 2, 3, 3, 4, 3, 4, 4, 5,
    1, 2, 2, 3, 2, 3, 3, 4, 2, 3, 3, 4, 3, 4, 4, 5,
    2, 3, 3, 4, 3, 4, 4, 5, 3, 4, 4, 5, 4, 5, 5, 6,
    1, 2, 2, 3, 2, 3, 3, 4, 2, 3, 3, 4, 3, 4, 4, 5,
    2, 3, 3, 4, 3, 4, 4, 5, 3, 4, 4, 5, 4, 5, 5, 6,
    2, 3, 3, 4, 3, 4, 4, 5, 3, 4, 4, 5, 4, 5, 5, 6,
    3, 4, 4, 5, 4, 5, 5, 6, 4, 5, 5, 6, 5, 6, 6, 7,
    1, 2, 2, 3, 2, 3, 3, 4, 2, 3, 3, 4, 3, 4, 4, 5,
    2, 3, 3, 4, 3, 4, 4, 5, 3, 4, 4, 5, 4, 5, 5, 6,
    2, 3, 3, 4, 3, 4, 4, 5, 3, 4, 4, 5, 4, 5, 5, 6,
    3, 4, 4, 5, 4, 5, 5, 6, 4, 5, 5, 6, 5, 6, 6, 7,
    2, 3, 3, 4, 3, 4, 4, 5, 3, 4, 4, 5, 4, 5, 5, 6,
    3, 4, 4, 5, 4, 5, 5, 6, 4, 5, 5, 6, 5, 6, 6, 7,
    3, 4, 4, 5, 4, 5, 5, 6, 4, 5, 5, 6, 5, 6, 6, 7,
    4, 5, 5, 6, 5, 6, 6, 7, 5, 6, 6, 7, 6, 7, 7, 8,
};

//types and constants used in the functions below
//uint64_t is an unsigned 64-bit integer variable type (defined in C99 version of C language)
static const uint64_t bit_m1  = 0x5555555555555555; //binary: 0101...
static const uint64_t bit_m2  = 0x3333333333333333; //binary: 00110011..
static const uint64_t bit_m4  = 0x0f0f0f0f0f0f0f0f; //binary:  4 zeros,  4 ones ...
static const uint64_t bit_h01 = 0x0101010101010101; //the sum of 256 to the power of 0,1,2,3...

// http://bisqwit.iki.fi/source/misc/bitcounting/#Wp3NiftyRevised
#define BITWISE_HW_WP3(tmpx, hw) do {                    \
 tmpx -= (tmpx >> 1) & bit_m1;                       \
 tmpx = (tmpx & bit_m2) + ((tmpx >> 2) & bit_m2);    \
 tmpx = (tmpx + (tmpx >> 4)) & bit_m4;               \
 hw += (tmpx * bit_h01) >> 56;                       \
 } while(0)

#if HAS_VECTORS
static const vec bitv_00  = {0x00, 0x00, 0x00, 0x00, (char)0x00, (char)0x00, (char)0x00, (char)0x00, (char)0x00, (char)0x00, (char)0x00, (char)0x00, (char)0x00, (char)0x00, (char)0x00, (char)0x00};
static const vec bitv_ff  = {(char)0xff, (char)0xff, (char)0xff, (char)0xff, (char)0xff, (char)0xff, (char)0xff, (char)0xff, (char)0xff, (char)0xff, (char)0xff, (char)0xff, (char)0xff, (char)0xff, (char)0xff, (char)0xff};
#endif

/* returns number of 1 bits */
static idx_t
count(bitarrayobject *self, int vi, idx_t start, idx_t stop)
{
    idx_t i, res = 0;
    unsigned char c;

    assert(0 <= start && start <= self->nbits);
    assert(0 <= stop && stop <= self->nbits);
    assert(0 <= vi && vi <= 1);
    assert(BYTES(stop) <= Py_SIZE(self));

    if (self->nbits == 0 || start >= stop)
        return 0;

    if (stop >= start + 8) {
        const Py_ssize_t byte_start = BYTES(start);
        const Py_ssize_t byte_stop = (Py_ssize_t) (stop / 8);
        const uint64_t * data64 = (const uint64_t *) (self->ob_item + byte_start);
        uint64_t tmp = 0;
        Py_ssize_t j;

        for (i = start; i < BITS(byte_start); i++)
            res += GETBIT(self, i);
        for (j = byte_start, i = 0; j + 8 < byte_stop; j += 8, ++i) {
            tmp = data64[i];
            BITWISE_HW_WP3(tmp, res);
        }
        for (; j < byte_stop; j++) {
            c = self->ob_item[j];
            res += bitcount_lookup[c];
        }
        for (i = BITS(byte_stop); i < stop; i++)
            res += GETBIT(self, i);
    }
    else {
        for (i = start; i < stop; i++)
            res += GETBIT(self, i);
    }
    return vi ? res : stop - start - res;
}

/* return index of first occurrence of vi, -1 when x is not in found. */
static idx_t
findfirst(bitarrayobject *self, int vi, idx_t start, idx_t stop)
{
    Py_ssize_t j;
    idx_t i;

    assert(0 <= start && start <= self->nbits);
    assert(0 <= stop && stop <= self->nbits);
    assert(0 <= vi && vi <= 1);
    assert(BYTES(stop) <= Py_SIZE(self));

    if (self->nbits == 0 || start >= stop)
        return -1;

    if (stop >= start + 8) {
        /* seraching for 1 means: break when byte is not 0x00
           searching for 0 means: break when byte is not 0xff */
        const char c = vi ? 0x00 : 0xff;

        /* skip ahead by checking whole bytes */
        for (j = (Py_ssize_t) (start / 8); j < BYTES(stop); j++)
            if (c ^ self->ob_item[j])
                break;

        if (start < BITS(j))
            start = BITS(j);
    }

    /* fine grained search */
    for (i = start; i < stop; i++)
        if (GETBIT(self, i) == vi)
            return i;

    return -1;
}

/* search for the first occurrence of bitarray xa (in self), starting at p,
   and return its position (or -1 when not found)
*/
static idx_t
search(bitarrayobject *self, bitarrayobject *xa, idx_t p)
{
    idx_t i;

    assert(p >= 0);
    while (p < self->nbits - xa->nbits + 1) {
        for (i = 0; i < xa->nbits; i++)
            if (GETBIT(self, p + i) != GETBIT(xa, i))
                goto next;

        return p;
    next:
        p++;
    }
    return -1;
}

static int
set_item(bitarrayobject *self, idx_t i, PyObject *v)
{
    long vi;

    assert(0 <= i && i < self->nbits);
    vi = PyObject_IsTrue(v);
    if (vi < 0)
        return -1;
    setbit(self, i, vi);
    return 0;
}

static int
append_item(bitarrayobject *self, PyObject *item)
{
    if (resize(self, self->nbits + 1) < 0)
        return -1;
    return set_item(self, self->nbits - 1, item);
}

static PyObject *
unpack(bitarrayobject *self, char zero, char one)
{
    PyObject *result;
    Py_ssize_t i;
    char *str;

    if (self->nbits > PY_SSIZE_T_MAX) {
        PyErr_SetString(PyExc_OverflowError, "bitarray too large to unpack");
        return NULL;
    }
    str = (char *) PyMem_Malloc((size_t) self->nbits);
    if (str == NULL) {
        PyErr_NoMemory();
        return NULL;
    }
    for (i = 0; i < self->nbits; i++) {
        *(str + i) = GETBIT(self, i) ? one : zero;
    }
    result = PyBytes_FromStringAndSize(str, (Py_ssize_t) self->nbits);
    PyMem_Free((void *) str);
    return result;
}

static int
extend_bitarray(bitarrayobject *self, bitarrayobject *other)
{
    idx_t n_sum;
    idx_t n_other_bits;

    if (other->nbits == 0)
        return 0;

    /* Note that other may be self.  Thus we take the size before we resize,
       ensuring we only copy the right parts of the array. */
    n_other_bits = other->nbits;
    n_sum = self->nbits + other->nbits;

    if (resize(self, n_sum) < 0)
        return -1;

    copy_n(self, n_sum - n_other_bits, other, 0, n_other_bits);
    return 0;
}

static int
extend_iter(bitarrayobject *self, PyObject *iter)
{
    PyObject *item;

    assert(PyIter_Check(iter));
    while ((item = PyIter_Next(iter)) != NULL) {
        if (append_item(self, item) < 0) {
            Py_DECREF(item);
            return -1;
        }
        Py_DECREF(item);
    }
    if (PyErr_Occurred())
        return -1;

    return 0;
}

static int
extend_list(bitarrayobject *self, PyObject *list)
{
    PyObject *item;
    Py_ssize_t n, i;

    assert(PyList_Check(list));
    n = PyList_Size(list);
    if (n == 0)
        return 0;

    if (resize(self, self->nbits + n) < 0)
        return -1;

    for (i = 0; i < n; i++) {
        item = PyList_GetItem(list, i);
        if (item == NULL)
            return -1;
        if (set_item(self, self->nbits - n + i, item) < 0)
            return -1;
    }
    return 0;
}

static int
extend_tuple(bitarrayobject *self, PyObject *tuple)
{
    PyObject *item;
    Py_ssize_t n, i;

    assert(PyTuple_Check(tuple));
    n = PyTuple_Size(tuple);
    if (n == 0)
        return 0;

    if (resize(self, self->nbits + n) < 0)
        return -1;

    for (i = 0; i < n; i++) {
        item = PyTuple_GetItem(tuple, i);
        if (item == NULL)
            return -1;
        if (set_item(self, self->nbits - n + i, item) < 0)
            return -1;
    }
    return 0;
}

/* extend_bytes(): extend the bitarray from a PyBytes object, where each
   whole character is converted to a single bit */
enum conv_t {
    BYTES_01,    /*  '0' -> 0    '1'  -> 1   no other characters allowed */
    BYTES_RAW,   /*  0x00 -> 0   other -> 1                              */
};

static int
extend_bytes(bitarrayobject *self, PyObject *bytes, enum conv_t conv)
{
    Py_ssize_t strlen, i;
    char c, *str;
    int vi = 0;

    assert(PyBytes_Check(bytes));
    strlen = PyBytes_Size(bytes);
    if (strlen == 0)
        return 0;

    if (resize(self, self->nbits + strlen) < 0)
        return -1;

    str = PyBytes_AsString(bytes);

    for (i = 0; i < strlen; i++) {
        c = *(str + i);
        /* depending on conv, map c to bit */
        switch (conv) {
        case BYTES_01:
            switch (c) {
            case '0': vi = 0; break;
            case '1': vi = 1; break;
            default:
                PyErr_Format(PyExc_ValueError,
                             "character must be '0' or '1', found '%c'", c);
                return -1;
            }
            break;
        case BYTES_RAW:
            vi = c ? 1 : 0;
            break;
        default:  /* should never happen */
            return -1;
        }
        setbit(self, self->nbits - strlen + i, vi);
    }
    return 0;
}

static int
extend_rawbytes(bitarrayobject *self, PyObject *bytes)
{
    Py_ssize_t strlen;
    char *str;

    assert(PyBytes_Check(bytes) && self->nbits % 8 == 0);
    strlen = PyBytes_Size(bytes);
    if (strlen == 0)
        return 0;

    if (resize(self, self->nbits + BITS(strlen)) < 0)
        return -1;

    str = PyBytes_AsString(bytes);
    memcpy(self->ob_item + (Py_SIZE(self) - strlen), str, strlen);
    return 0;
}

static int
extend_dispatch(bitarrayobject *self, PyObject *obj)
{
    PyObject *iter;
    int ret;

    /* dispatch on type */
    if (bitarray_Check(obj))                              /* bitarray */
        return extend_bitarray(self, (bitarrayobject *) obj);

    if (PyList_Check(obj))                                    /* list */
        return extend_list(self, obj);

    if (PyTuple_Check(obj))                                  /* tuple */
        return extend_tuple(self, obj);

    if (PyBytes_Check(obj))                               /* bytes 01 */
        return extend_bytes(self, obj, BYTES_01);

    if (PyUnicode_Check(obj)) {                /* (unicode) string 01 */
        PyObject *bytes;
        bytes = PyUnicode_AsEncodedString(obj, NULL, NULL);
        ret = extend_bytes(self, bytes, BYTES_01);
        Py_DECREF(bytes);
        return ret;
    }

    if (PyIter_Check(obj))                                    /* iter */
        return extend_iter(self, obj);

    /* finally, try to get the iterator of the object */
    iter = PyObject_GetIter(obj);
    if (iter == NULL) {
        PyErr_SetString(PyExc_TypeError, "could not extend bitarray");
        return -1;
    }
    ret = extend_iter(self, iter);
    Py_DECREF(iter);
    return ret;
}

/* --------- helper functions NOT involving bitarrayobjects ------------ */

#define ENDIAN_STR(ba)  (((ba)->endian) ? "big" : "little")

#ifdef IS_PY3K
#define IS_INDEX(x)  (PyLong_Check(x) || PyIndex_Check(x))
#define IS_INT_OR_BOOL(x)  (PyBool_Check(x) || PyLong_Check(x))
#else  /* Py 2 */
#define IS_INDEX(x)  (PyInt_Check(x) || PyLong_Check(x) || PyIndex_Check(x))
#define IS_INT_OR_BOOL(x)  (PyBool_Check(x) || PyInt_Check(x) || \
                                               PyLong_Check(x))
#endif

/* given an PyLong (which must be 0 or 1), or a PyBool, return 0 or 1,
   or -1 on error */
static int
IntBool_AsInt(PyObject *v)
{
    long x;

    if (PyBool_Check(v))
        return PyObject_IsTrue(v);

#ifndef IS_PY3K
    if (PyInt_Check(v)) {
        x = PyInt_AsLong(v);
    }
    else
#endif
    if (PyLong_Check(v)) {
        x = PyLong_AsLong(v);
    }
    else {
        PyErr_SetString(PyExc_TypeError, "integer or bool expected");
        return -1;
    }

    if (x < 0 || x > 1) {
        PyErr_SetString(PyExc_ValueError,
                        "integer value between 0 and 1 expected");
        return -1;
    }
    return (int) x;
}

/* Normalize index (which may be negative), such that 0 <= i <= n */
static void
normalize_index(idx_t n, idx_t *i)
{
    if (*i < 0) {
        *i += n;
        if (*i < 0)
            *i = 0;
    }
    if (*i > n)
        *i = n;
}

/* Extract a slice index from a PyInt or PyLong or an object with the
   nb_index slot defined, and store in *i.
   However, this function returns -1 on error and 0 on success.

   This is almost _PyEval_SliceIndex() with Py_ssize_t replaced by idx_t
*/
static int
getIndex(PyObject *v, idx_t *i)
{
    idx_t x;

#ifndef IS_PY3K
    if (PyInt_Check(v)) {
        x = PyInt_AS_LONG(v);
    }
    else
#endif
    if (PyLong_Check(v)) {
        x = PyLong_AsLongLong(v);
    }
    else if (PyIndex_Check(v)) {
        x = PyNumber_AsSsize_t(v, NULL);
        if (x == -1 && PyErr_Occurred())
            return -1;
    }
    else {
        PyErr_SetString(PyExc_TypeError, "slice indices must be integers or "
                                         "None or have an __index__ method");
        return -1;
    }
    *i = x;
    return 0;
}

/* this is PySlice_GetIndicesEx() with Py_ssize_t replaced by idx_t */
static int
slice_GetIndicesEx(PySliceObject *r, idx_t length,
                   idx_t *start, idx_t *stop, idx_t *step, idx_t *slicelength)
{
    idx_t defstart, defstop;

    if (r->step == Py_None) {
        *step = 1;
    }
    else {
        if (getIndex(r->step, step) < 0)
            return -1;
        if (*step == 0) {
            PyErr_SetString(PyExc_ValueError, "slice step cannot be zero");
            return -1;
        }
    }
    defstart = *step < 0 ? length - 1 : 0;
    defstop = *step < 0 ? -1 : length;

    if (r->start == Py_None) {
        *start = defstart;
    }
    else {
        if (getIndex(r->start, start) < 0)
            return -1;
        if (*start < 0) *start += length;
        if (*start < 0) *start = (*step < 0) ? -1 : 0;
        if (*start >= length) *start = (*step < 0) ? length - 1 : length;
    }

    if (r->stop == Py_None) {
        *stop = defstop;
    }
    else {
        if (getIndex(r->stop, stop) < 0)
            return -1;
        if (*stop < 0) *stop += length;
        if (*stop < 0) *stop = -1;
        if (*stop > length) *stop = length;
    }

    if ((*step < 0 && *stop >= *start) || (*step > 0 && *start >= *stop)) {
        *slicelength = 0;
    }
    else if (*step < 0) {
        *slicelength = (*stop - *start + 1) / (*step) + 1;
    }
    else {
        *slicelength = (*stop - *start - 1) / (*step) + 1;
    }

    return 0;
}

/**************************************************************************
                         Implementation of API methods
 **************************************************************************/

static PyObject *
bitarray_length(bitarrayobject *self)
{
    return PyLong_FromLongLong(self->nbits);
}

PyDoc_STRVAR(length_doc,
"length() -> int\n\
\n\
Return the length, i.e. number of bits stored in the bitarray.\n\
This method is preferred over `__len__` (used when typing `len(a)`),\n\
since `__len__` will fail for a bitarray object with 2^31 or more elements\n\
on a 32bit machine, whereas this method will return the correct value,\n\
on 32bit and 64bit machines.");

PyDoc_STRVAR(len_doc,
"__len__() -> int\n\
\n\
Return the length, i.e. number of bits stored in the bitarray.\n\
This method will fail for a bitarray object with 2^31 or more elements\n\
on a 32bit machine.  Use bitarray.length() instead.");


static PyObject *
bitarray_copy(bitarrayobject *self)
{
    PyObject *res;

    res = newbitarrayobject(Py_TYPE(self), self->nbits, self->endian);
    if (res == NULL)
        return NULL;

    memcpy(((bitarrayobject *) res)->ob_item, self->ob_item, Py_SIZE(self));
    return res;
}

PyDoc_STRVAR(copy_doc,
"copy() -> bitarray\n\
\n\
Return a copy of the bitarray.");


static PyObject *
bitarray_count(bitarrayobject *self, PyObject *args)
{
    PyObject *x = Py_True;
    idx_t start = 0, stop = self->nbits;
    long vi;

    if (!PyArg_ParseTuple(args, "|OLL:count", &x, &start, &stop))
        return NULL;

    vi = PyObject_IsTrue(x);
    if (vi < 0)
        return NULL;

    normalize_index(self->nbits, &start);
    normalize_index(self->nbits, &stop);

    return PyLong_FromLongLong(count(self, vi, start, stop));
}

PyDoc_STRVAR(count_doc,
"count(value=True, start=0, stop=<end of array>, /) -> int\n\
\n\
Count the number of occurrences of bool(value) in the bitarray.");


static PyObject *
bitarray_index(bitarrayobject *self, PyObject *args)
{
    PyObject *x;
    idx_t i, start = 0, stop = self->nbits;
    long vi;

    if (!PyArg_ParseTuple(args, "O|LL:index", &x, &start, &stop))
        return NULL;

    vi = PyObject_IsTrue(x);
    if (vi < 0)
        return NULL;

    normalize_index(self->nbits, &start);
    normalize_index(self->nbits, &stop);

    i = findfirst(self, vi, start, stop);
    if (i < 0) {
        PyErr_SetString(PyExc_ValueError, "index(x): x not in bitarray");
        return NULL;
    }
    return PyLong_FromLongLong(i);
}

PyDoc_STRVAR(index_doc,
"index(value, start=0, stop=<end of array>, /) -> int\n\
\n\
Return index of the first occurrence of `bool(value)` in the bitarray.\n\
Raises `ValueError` if the value is not present.");


static PyObject *
bitarray_extend(bitarrayobject *self, PyObject *obj)
{
    if (extend_dispatch(self, obj) < 0)
        return NULL;
    Py_RETURN_NONE;
}

PyDoc_STRVAR(extend_doc,
"extend(iterable, /)\n\
\n\
Append bits to the end of the bitarray.  The objects which can be passed\n\
to this method are the same iterable objects which can given to a bitarray\n\
object upon initialization.");


static PyObject *
bitarray_contains(bitarrayobject *self, PyObject *x)
{
    long res;

    if (IS_INT_OR_BOOL(x)) {
        int vi;

        vi = IntBool_AsInt(x);
        if (vi < 0)
            return NULL;
        res = findfirst(self, vi, 0, self->nbits) >= 0;
    }
    else if (bitarray_Check(x)) {
        res = search(self, (bitarrayobject *) x, 0) >= 0;
    }
    else {
        PyErr_SetString(PyExc_TypeError, "bitarray or bool expected");
        return NULL;
    }
    return PyBool_FromLong(res);
}

PyDoc_STRVAR(contains_doc,
"__contains__(value, /) -> bool\n\
\n\
Return True if bitarray contains value, False otherwise.\n\
The value may be a boolean (or integer between 0 and 1), or a bitarray.");


static PyObject *
bitarray_search(bitarrayobject *self, PyObject *args)
{
    PyObject *list = NULL;   /* list of matching positions to be returned */
    PyObject *x, *item = NULL;
    Py_ssize_t limit = -1;
    bitarrayobject *xa;
    idx_t p;

    if (!PyArg_ParseTuple(args, "O|n:_search", &x, &limit))
        return NULL;

    if (!bitarray_Check(x)) {
        PyErr_SetString(PyExc_TypeError, "bitarray expected for search");
        return NULL;
    }
    xa = (bitarrayobject *) x;
    if (xa->nbits == 0) {
        PyErr_SetString(PyExc_ValueError, "can't search for empty bitarray");
        return NULL;
    }
    list = PyList_New(0);
    if (list == NULL)
        return NULL;
    if (xa->nbits > self->nbits || limit == 0)
        return list;

    p = 0;
    while (1) {
        p = search(self, xa, p);
        if (p < 0)
            break;
        item = PyLong_FromLongLong(p);
        p++;
        if (item == NULL || PyList_Append(list, item) < 0) {
            Py_XDECREF(item);
            Py_XDECREF(list);
            return NULL;
        }
        Py_DECREF(item);
        if (limit > 0 && PyList_Size(list) >= limit)
            break;
    }
    return list;
}

PyDoc_STRVAR(search_doc,
"search(bitarray, limit=<none>, /) -> list\n\
\n\
Searches for the given bitarray in self, and return the list of start\n\
positions.\n\
The optional argument limits the number of search results to the integer\n\
specified.  By default, all search results are returned.");


static PyObject *
bitarray_buffer_info(bitarrayobject *self)
{
    PyObject *res, *ptr;

    ptr = PyLong_FromVoidPtr(self->ob_item),
    res = Py_BuildValue("OLsiL",
                        ptr,
                        (idx_t) Py_SIZE(self),
                        ENDIAN_STR(self),
                        (int) (BITS(Py_SIZE(self)) - self->nbits),
                        (idx_t) self->allocated);
    Py_DECREF(ptr);
    return res;
}

PyDoc_STRVAR(buffer_info_doc,
"buffer_info() -> tuple\n\
\n\
Return a tuple (address, size, endianness, unused, allocated) giving the\n\
current memory address, the size (in bytes) used to hold the bitarray's\n\
contents, the bit endianness as a string, the number of unused bits\n\
(e.g. a bitarray of length 11 will have a buffer size of 2 bytes and\n\
5 unused bits), and the size (in bytes) of the allocated memory.");


static PyObject *
bitarray_endian(bitarrayobject *self)
{
#ifdef IS_PY3K
    return PyUnicode_FromString(ENDIAN_STR(self));
#else
    return PyString_FromString(ENDIAN_STR(self));
#endif
}

PyDoc_STRVAR(endian_doc,
"endian() -> str\n\
\n\
Return the bit endianness as a string (either `little` or `big`).");


static PyObject *
bitarray_append(bitarrayobject *self, PyObject *v)
{
    if (append_item(self, v) < 0)
        return NULL;

    Py_RETURN_NONE;
}

PyDoc_STRVAR(append_doc,
"append(item, /)\n\
\n\
Append the value `bool(item)` to the end of the bitarray.");


static PyObject *
bitarray_all(bitarrayobject *self)
{
    if (findfirst(self, 0, 0, self->nbits) >= 0)
        Py_RETURN_FALSE;
    else
        Py_RETURN_TRUE;
}

PyDoc_STRVAR(all_doc,
"all() -> bool\n\
\n\
Returns True when all bits in the array are True.");


static PyObject *
bitarray_any(bitarrayobject *self)
{
    if (findfirst(self, 1, 0, self->nbits) >= 0)
        Py_RETURN_TRUE;
    else
        Py_RETURN_FALSE;
}

PyDoc_STRVAR(any_doc,
"any() -> bool\n\
\n\
Returns True when any bit in the array is True.");


static PyObject *
bitarray_reduce(bitarrayobject *self)
{
    PyObject *dict, *repr = NULL, *result = NULL;
    char *str;

    dict = PyObject_GetAttrString((PyObject *) self, "__dict__");
    if (dict == NULL) {
        PyErr_Clear();
        dict = Py_None;
        Py_INCREF(dict);
    }
    /* the first byte indicates the number of unused bits at the end, and
       the rest of the bytes consist of the raw binary data */
    str = (char *) PyMem_Malloc(Py_SIZE(self) + 1);
    if (str == NULL) {
        PyErr_NoMemory();
        goto error;
    }
    str[0] = (char) setunused(self);
    memcpy(str + 1, self->ob_item, Py_SIZE(self));
    repr = PyBytes_FromStringAndSize(str, Py_SIZE(self) + 1);
    if (repr == NULL)
        goto error;
    PyMem_Free((void *) str);
    result = Py_BuildValue("O(Os)O", Py_TYPE(self),
                           repr, ENDIAN_STR(self), dict);
error:
    Py_DECREF(dict);
    Py_XDECREF(repr);
    return result;
}

PyDoc_STRVAR(reduce_doc, "state information for pickling");


static PyObject *
bitarray_reverse(bitarrayobject *self)
{
    PyObject *t;    /* temp bitarray to store lower half of self */
    idx_t i, m;

    if (self->nbits < 2)
        Py_RETURN_NONE;

    t = newbitarrayobject(Py_TYPE(self), self->nbits / 2, self->endian);
    if (t == NULL)
        return NULL;

#define tt  ((bitarrayobject *) t)
    /* copy lower half of array into temporary array */
    memcpy(tt->ob_item, self->ob_item, Py_SIZE(tt));

    m = self->nbits - 1;

    /* reverse the upper half onto the lower half. */
    for (i = 0; i < tt->nbits; i++)
        setbit(self, i, GETBIT(self, m - i));

    /* revert the stored away lower half onto the upper half. */
    for (i = 0; i < tt->nbits; i++)
        setbit(self, m - i, GETBIT(tt, i));
#undef tt
    Py_DECREF(t);
    Py_RETURN_NONE;
}

PyDoc_STRVAR(reverse_doc,
"reverse()\n\
\n\
Reverse the order of bits in the array (in-place).");


static PyObject *
bitarray_fill(bitarrayobject *self)
{
    long p;

    p = setunused(self);
    self->nbits += p;
#ifdef IS_PY3K
    return PyLong_FromLong(p);
#else
    return PyInt_FromLong(p);
#endif
}

PyDoc_STRVAR(fill_doc,
"fill() -> int\n\
\n\
Adds zeros to the end of the bitarray, such that the length of the bitarray\n\
will be a multiple of 8.  Returns the number of bits added (0..7).");


static PyObject *
bitarray_invert(bitarrayobject *self)
{
    invert(self);
    Py_RETURN_NONE;
}

PyDoc_STRVAR(invert_doc,
"invert()\n\
\n\
Invert all bits in the array (in-place),\n\
i.e. convert each 1-bit into a 0-bit and vice versa.");


static PyObject *
bitarray_bytereverse(bitarrayobject *self)
{
    bytereverse(self);
    Py_RETURN_NONE;
}

PyDoc_STRVAR(bytereverse_doc,
"bytereverse()\n\
\n\
For all bytes representing the bitarray, reverse the bit order (in-place).\n\
Note: This method changes the actual machine values representing the\n\
bitarray; it does not change the endianness of the bitarray object.");


static PyObject *
bitarray_setall(bitarrayobject *self, PyObject *v)
{
    long vi;

    vi = PyObject_IsTrue(v);
    if (vi < 0)
        return NULL;

    memset(self->ob_item, vi ? 0xff : 0x00, Py_SIZE(self));
    Py_RETURN_NONE;
}

PyDoc_STRVAR(setall_doc,
"setall(value, /)\n\
\n\
Set all bits in the bitarray to `bool(value)`.");


static PyObject *
bitarray_sort(bitarrayobject *self, PyObject *args, PyObject *kwds)
{
    idx_t n, n0, n1;
    int reverse = 0;
    static char *kwlist[] = {"reverse", NULL};

    if (!PyArg_ParseTupleAndKeywords(args, kwds, "|i:sort", kwlist, &reverse))
        return NULL;

    n = self->nbits;
    n1 = count(self, 1, 0, n);

    if (reverse) {
        setrange(self, 0, n1, 1);
        setrange(self, n1, n, 0);
    }
    else {
        n0 = n - n1;
        setrange(self, 0, n0, 0);
        setrange(self, n0, n, 1);
    }
    Py_RETURN_NONE;
}

PyDoc_STRVAR(sort_doc,
"sort(reverse=False)\n\
\n\
Sort the bits in the array (in-place).");


/* since too many details differ between the Python 2 and 3 implementation
   of this function, we choose to have two separate function implementation,
   even though this means some of the code is duplicated in the two versions
*/
#ifdef IS_PY3K
static PyObject *
bitarray_fromfile(bitarrayobject *self, PyObject *args)
{
    PyObject *f;
    Py_ssize_t newsize, nbytes = -1;
    PyObject *reader, *rargs, *result;
    size_t nread;
    idx_t t, p;

    if (!PyArg_ParseTuple(args, "O|n:fromfile", &f, &nbytes))
        return NULL;

    if (nbytes == 0)
        Py_RETURN_NONE;

    reader = PyObject_GetAttrString(f, "read");
    if (reader == NULL)
    {
        PyErr_SetString(PyExc_TypeError,
                        "first argument must be an open file");
        return NULL;
    }
    rargs = Py_BuildValue("(n)", nbytes);
    if (rargs == NULL) {
        Py_DECREF(reader);
        return NULL;
    }
    result = PyEval_CallObject(reader, rargs);
    if (result != NULL) {
        if (!PyBytes_Check(result)) {
            PyErr_SetString(PyExc_TypeError,
                            "first argument must be an open file");
            Py_DECREF(result);
            Py_DECREF(rargs);
            Py_DECREF(reader);
            return NULL;
        }
        nread = PyBytes_Size(result);

        t = self->nbits;
        p = setunused(self);
        self->nbits += p;

        newsize = Py_SIZE(self) + nread;

        if (resize(self, BITS(newsize)) < 0) {
            Py_DECREF(result);
            Py_DECREF(rargs);
            Py_DECREF(reader);
            return NULL;
        }
        memcpy(self->ob_item + (Py_SIZE(self) - nread),
               PyBytes_AS_STRING(result), nread);

        if (nbytes > 0 && nread < (size_t) nbytes) {
            PyErr_SetString(PyExc_EOFError, "not enough items read");
            return NULL;
        }
        if (delete_n(self, t, p) < 0)
            return NULL;
        Py_DECREF(result);
    }

    Py_DECREF(rargs);
    Py_DECREF(reader);
    Py_RETURN_NONE;
}
#else  /* Python 2 */
static PyObject *
bitarray_fromfile(bitarrayobject *self, PyObject *args)
{
    PyObject *f;
    FILE *fp;
    Py_ssize_t newsize, nbytes = -1;
    size_t nread;
    idx_t t, p;
    long cur;

    if (!PyArg_ParseTuple(args, "O|n:fromfile", &f, &nbytes))
        return NULL;

    fp = PyFile_AsFile(f);
    if (fp == NULL) {
        PyErr_SetString(PyExc_TypeError,
                        "first argument must be an open file");
        return NULL;
    }

    /* find number of bytes till EOF */
    if (nbytes < 0) {
        if ((cur = ftell(fp)) < 0)
            goto EOFerror;

        if (fseek(fp, 0L, SEEK_END) || (nbytes = ftell(fp)) < 0)
            goto EOFerror;

        nbytes -= cur;
        if (fseek(fp, cur, SEEK_SET)) {
        EOFerror:
            PyErr_SetString(PyExc_EOFError, "could not find EOF");
            return NULL;
        }
    }
    if (nbytes == 0)
        Py_RETURN_NONE;

    /* file exists and there are more than zero bytes to read */
    t = self->nbits;
    p = setunused(self);
    self->nbits += p;

    newsize = Py_SIZE(self) + nbytes;
    if (resize(self, BITS(newsize)) < 0)
        return NULL;

    nread = fread(self->ob_item + (Py_SIZE(self) - nbytes), 1, nbytes, fp);
    if (nread < (size_t) nbytes) {
        newsize -= nbytes - nread;
        if (resize(self, BITS(newsize)) < 0)
            return NULL;
        PyErr_SetString(PyExc_EOFError, "not enough items in file");
        return NULL;
    }

    if (delete_n(self, t, p) < 0)
        return NULL;
    Py_RETURN_NONE;
}
#endif

PyDoc_STRVAR(fromfile_doc,
"fromfile(f, n=<till EOF>, /)\n\
\n\
Read n bytes from the file object f and append them to the bitarray\n\
interpreted as machine values.  When n is omitted, as many bytes are\n\
read until EOF is reached.");


/* since too many details differ between the Python 2 and 3 implementation
   of this function, we choose to have two separate function implementation
*/
#ifdef IS_PY3K
static PyObject *
bitarray_tofile(bitarrayobject *self, PyObject *f)
{
    PyObject *writer, *value, *args, *result;

    if (f == NULL) {
        PyErr_SetString(PyExc_TypeError, "writeobject with NULL file");
        return NULL;
    }
    writer = PyObject_GetAttrString(f, "write");
    if (writer == NULL)
        return NULL;
    setunused(self);
    value = PyBytes_FromStringAndSize(self->ob_item, Py_SIZE(self));
    if (value == NULL) {
        Py_DECREF(writer);
        return NULL;
    }
    args = PyTuple_Pack(1, value);
    if (args == NULL) {
        Py_DECREF(value);
        Py_DECREF(writer);
        return NULL;
    }
    result = PyEval_CallObject(writer, args);
    Py_DECREF(args);
    Py_DECREF(value);
    Py_DECREF(writer);
    if (result == NULL)
    {
        PyErr_SetString(PyExc_TypeError, "open file expected");
        return NULL;
    }
    Py_DECREF(result);
    Py_RETURN_NONE;
}
#else  /* Python 2 */
static PyObject *
bitarray_tofile(bitarrayobject *self, PyObject *f)
{
    FILE *fp;

    fp = PyFile_AsFile(f);
    if (fp == NULL) {
        PyErr_SetString(PyExc_TypeError, "open file expected");
        return NULL;
    }
    if (Py_SIZE(self) == 0)
        Py_RETURN_NONE;

    setunused(self);
    if (fwrite(self->ob_item, 1, Py_SIZE(self), fp) !=
        (size_t) Py_SIZE(self))
    {
        PyErr_SetFromErrno(PyExc_IOError);
        clearerr(fp);
        return NULL;
    }
    Py_RETURN_NONE;
}
#endif

PyDoc_STRVAR(tofile_doc,
"tofile(f, /)\n\
\n\
Write all bits (as machine values) to the file object f.\n\
When the length of the bitarray is not a multiple of 8,\n\
the remaining bits (1..7) are set to 0.");


static PyObject *
bitarray_tolist(bitarrayobject *self)
{
    PyObject *list;
    idx_t i;

    list = PyList_New((Py_ssize_t) self->nbits);
    if (list == NULL)
        return NULL;

    for (i = 0; i < self->nbits; i++)
        if (PyList_SetItem(list, (Py_ssize_t) i,
                           PyBool_FromLong(GETBIT(self, i))) < 0)
            return NULL;
    return list;
}

PyDoc_STRVAR(tolist_doc,
"tolist() -> list\n\
\n\
Return an ordinary list with the items in the bitarray.\n\
Note that the list object being created will require 32 or 64 times more\n\
memory than the bitarray object, which may cause a memory error if the\n\
bitarray is very large.\n\
Also note that to extend a bitarray with elements from a list,\n\
use the extend method.");


static PyObject *
bitarray_frombytes(bitarrayobject *self, PyObject *bytes)
{
    idx_t t, p;

    if (!PyBytes_Check(bytes)) {
        PyErr_SetString(PyExc_TypeError, "bytes expected");
        return NULL;
    }

    /* Before we extend the raw bytes with the new data, we need to store
       the current size and pad the last byte, as our bitarray size might
       not be a multiple of 8.  After extending, we remove the padding
       bits again.  The same is done in bitarray_fromfile().
    */
    t = self->nbits;
    p = setunused(self);
    self->nbits += p;

    if (extend_rawbytes(self, bytes) < 0)
        return NULL;
    if (delete_n(self, t, p) < 0)
        return NULL;
    Py_RETURN_NONE;
}

PyDoc_STRVAR(frombytes_doc,
"frombytes(bytes, /)\n\
\n\
Append from a byte string, interpreted as machine values.");


static PyObject *
bitarray_tobytes(bitarrayobject *self)
{
    setunused(self);
    return PyBytes_FromStringAndSize(self->ob_item, Py_SIZE(self));
}

PyDoc_STRVAR(tobytes_doc,
"tobytes() -> bytes\n\
\n\
Return the byte representation of the bitarray.\n\
When the length of the bitarray is not a multiple of 8, the few remaining\n\
bits (1..7) are considered to be 0.");


static PyObject *
bitarray_to01(bitarrayobject *self)
{
#ifdef IS_PY3K
    PyObject *string;
    PyObject *unpacked;

    unpacked = unpack(self, '0', '1');
    if (unpacked == NULL)
        return NULL;
    string = PyUnicode_FromEncodedObject(unpacked, NULL, NULL);
    Py_DECREF(unpacked);
    return string;
#else
    return unpack(self, '0', '1');
#endif
}

PyDoc_STRVAR(to01_doc,
"to01() -> str\n\
\n\
Return a string containing '0's and '1's, representing the bits in the\n\
bitarray object.\n\
Note: To extend a bitarray from a string containing '0's and '1's,\n\
use the extend method.");


static PyObject *
bitarray_unpack(bitarrayobject *self, PyObject *args, PyObject *kwds)
{
    char zero = 0x00, one = 0xff;
    static char *kwlist[] = {"zero", "one", NULL};

    if (!PyArg_ParseTupleAndKeywords(args, kwds, "|cc:unpack", kwlist,
                                     &zero, &one))
        return NULL;

    return unpack(self, zero, one);
}

PyDoc_STRVAR(unpack_doc,
"unpack(zero=b'\\x00', one=b'\\xff') -> bytes\n\
\n\
Return bytes containing one character for each bit in the bitarray,\n\
using the specified mapping.");


static PyObject *
bitarray_pack(bitarrayobject *self, PyObject *bytes)
{
    if (!PyBytes_Check(bytes)) {
        PyErr_SetString(PyExc_TypeError, "bytes expected");
        return NULL;
    }
    if (extend_bytes(self, bytes, BYTES_RAW) < 0)
        return NULL;

    Py_RETURN_NONE;
}

PyDoc_STRVAR(pack_doc,
"pack(bytes, /)\n\
\n\
Extend the bitarray from bytes, where each byte corresponds to a single\n\
bit.  The byte `b'\\x00'` maps to bit 0 and all other characters map to\n\
bit 1.\n\
This method, as well as the unpack method, are meant for efficient\n\
transfer of data between bitarray objects to other python objects\n\
(for example NumPy's ndarray object) which have a different memory view.");

/* -------------------------------------------- term functions -------------------------------------------- */

static PyObject *
bitarray_eval_monic(bitarrayobject *self, PyObject *args, PyObject *kwds)
{
    static char* kwlist[] = {"data", "index", "blocksize", NULL};
    PyObject *x;
    idx_t index=0, blocksize=16, offset=0;

    if (!PyArg_ParseTupleAndKeywords(args, kwds, "|OLL:eval_monic", kwlist, &x, &index, &blocksize)) {
        return NULL;
    }

    if (!bitarray_Check(x)) {
        PyErr_SetString(PyExc_TypeError, "bitarray expected");
        return NULL;
    }

    if (index < 0){
        PyErr_SetString(PyExc_IndexError, "index has to be zero or greater");
        return NULL;
    }

    if (blocksize <= 0){
        PyErr_SetString(PyExc_IndexError, "block size has to be 1 or greater");
        return NULL;
    }

    if (index >= blocksize){
        PyErr_SetString(PyExc_IndexError, "index has to be strictly less than block size");
        return NULL;
    }

    // BEGIN Evaluation core:
    // Resize current bitarray so it can store the evaluation result.
    bitarrayobject * other = (bitarrayobject *) x;
    idx_t new_bit_size = other->nbits / blocksize;
    if (new_bit_size != self->nbits && resize(self, new_bit_size) < 0){
        return NULL;
    }

    setunused(self);

    // Actual term evaluation.
    // Naively it is same as the following: take index-th bit, skip blocksize bits.
    idx_t ctr = 0;
    char acc = 0;               // accumulator - here is the sub-result collected.
    unsigned char sub_ctr = 0;      // counter inside the char, small range
    for(offset = index; offset < other->nbits; offset += blocksize){
        if (GETBIT(other, offset)) {
            acc |= BITMASK(self->endian, sub_ctr);
        }

        ++sub_ctr;
        // Once accumulator is full (or this is the last iteration), flush it to
        // the buffer.
        if (sub_ctr >= 8 || offset + blocksize >= other->nbits){
            self->ob_item[ctr] = acc;
            sub_ctr = 0;
            acc = 0;
            ++ctr;
        }
    }

    // END Evaluation core
    Py_INCREF(self);
    return (PyObject *) self;
}

PyDoc_STRVAR(eval_monic_doc,
"eval_monic(data, index, blocksize)\n\
\n\
Evaluates a monic term on the input data with x_index and the given\n\
blocksize. Equivalent to data[index::blocksize]. The evaluation is performed in-place with minimal \n\
memory reallocations. The result is a bitarray of evaluations of the term.");

static PyObject *
bitarray_fast_copy(bitarrayobject *self, PyObject *obj)
{
    if (!bitarray_Check(obj)) {
        PyErr_SetString(PyExc_TypeError, "bitarray expected");
        return NULL;
    }

    bitarrayobject * other = (bitarrayobject *) obj;
    if (other->endian != self->endian){
        PyErr_SetString(PyExc_ValueError, "The source does not have the same endianity as the destination");
        return NULL;
    }

    if (other->nbits != self->nbits){
        PyErr_SetString(PyExc_ValueError, "The source does not have the same size as the destination");
        return NULL;
    }

    if (other == self || other->ob_item == self->ob_item){
        PyErr_SetString(PyExc_ValueError, "The source and the destination are the same");
        return NULL;
    }

    // Copy itself, very fast.
    setunused(self);
    setunused(other);
    copy_n(self, 0, other, 0, other->nbits);

    Py_INCREF(self);
    return (PyObject *) self;
}

PyDoc_STRVAR(fast_copy_doc,
"fast_copy(other_bitarray)\n\
\n\
Copies the contents of the parameter with memcpy. Has to have same endianness, size, ...");

#define BITWISE_HW_TYPE uint64_t

#define BITWISE_HW_INTERNAL(SELF, OTHER, OP) do {                                                                      \
    Py_ssize_t i = 0, ii = 0;                                                                                          \
    const Py_ssize_t size = Py_SIZE(SELF);                                                                             \
    const BITWISE_HW_TYPE * self_ob_item = (const BITWISE_HW_TYPE *) (SELF)->ob_item;                                  \
    const BITWISE_HW_TYPE * other_ob_item = (const BITWISE_HW_TYPE *) (OTHER)->ob_item;                                \
    BITWISE_HW_TYPE tmp;                                                                                               \
                                                                                                                       \
    for (ii=0; (Py_ssize_t)(i + sizeof(BITWISE_HW_TYPE)) < size; i += (Py_ssize_t)sizeof(BITWISE_HW_TYPE), ++ii) {                               \
        tmp = self_ob_item[ii] OP other_ob_item[ii];                                                                   \
        BITWISE_HW_WP3(tmp, hw);                                                                                       \
    }                                                                                                                  \
                                                                                                                       \
    for (; i < size; ++i) {                                                                                            \
        hw += bitcount_lookup[(unsigned char)((SELF)->ob_item[i] OP (OTHER)->ob_item[i])];                             \
    }                                                                                                                  \
} while(0)

#define BITWISE_FAST_HW_FUNC(OPNAME, OP)                                                                               \
static PyObject * bitwise_fast_hw_ ## OPNAME (bitarrayobject *self, PyObject *obj)                                     \
{                                                                                                                      \
    if (!bitarray_Check(obj)) {                                                                                        \
        PyErr_SetString(PyExc_TypeError, "bitarray expected");                                                         \
        return NULL;                                                                                                   \
    }                                                                                                                  \
                                                                                                                       \
    bitarrayobject * other = (bitarrayobject *) obj;                                                                   \
    if (other->endian != self->endian){                                                                                \
        PyErr_SetString(PyExc_ValueError, "The source does not have the same endianity as the destination");           \
        return NULL;                                                                                                   \
    }                                                                                                                  \
                                                                                                                       \
    if (other->nbits != self->nbits){                                                                                  \
        PyErr_SetString(PyExc_ValueError, "The source does not have the same size as the destination");                \
        return NULL;                                                                                                   \
    }                                                                                                                  \
                                                                                                                       \
    if (other == self || other->ob_item == self->ob_item){                                                             \
        PyErr_SetString(PyExc_ValueError, "The source and the destination are the same");                              \
        return NULL;                                                                                                   \
    }                                                                                                                  \
                                                                                                                       \
    idx_t hw = 0;                                                                                                      \
    setunused(self);                                                                                                   \
    setunused(other);                                                                                                  \
                                                                                                                       \
    BITWISE_HW_INTERNAL(self, other, OP);                                                                              \
    return PyLong_FromLongLong(hw);                                                                                    \
}

BITWISE_FAST_HW_FUNC(and, &);
BITWISE_FAST_HW_FUNC(xor, ^);
BITWISE_FAST_HW_FUNC(or, |);

PyDoc_STRVAR(bitwise_fast_hw_and_doc,
             "fast_hw_and(other_bitarray)\n\
\n\
Performs quick in-memory AND operation on these self and other_bitarray and returns a hamming weight.");

PyDoc_STRVAR(bitwise_fast_hw_or_doc,
             "fast_hw_or(other_bitarray)\n\
\n\
Performs quick in-memory OR operation on these self and other_bitarray and returns a hamming weight.");

PyDoc_STRVAR(bitwise_fast_hw_xor_doc,
             "fast_hw_xor(other_bitarray)\n\
\n\
Performs quick in-memory XOR operation on these self and other_bitarray and returns a hamming weight.");

/* -------------------------------------------- bitarray repr -------------------------------------------- */

static PyObject *
bitarray_repr(bitarrayobject *self)
{
    PyObject *bytes;
    PyObject *unpacked;
#ifdef IS_PY3K
    PyObject *decoded;
#endif

    if (self->nbits == 0) {
        bytes = PyBytes_FromString("bitarray()");
    }
    else {
        bytes = PyBytes_FromString("bitarray(\'");
        unpacked = unpack(self, '0', '1');
        if (unpacked == NULL)
            return NULL;
        PyBytes_ConcatAndDel(&bytes, unpacked);
        PyBytes_ConcatAndDel(&bytes, PyBytes_FromString("\')"));
    }
#ifdef IS_PY3K
    decoded = PyUnicode_FromEncodedObject(bytes, NULL, NULL);
    Py_DECREF(bytes);
    return decoded;
#else
    return bytes;  /* really a string in Python 2 */
#endif
}


static PyObject *
bitarray_insert(bitarrayobject *self, PyObject *args)
{
    idx_t i;
    PyObject *v;

    if (!PyArg_ParseTuple(args, "LO:insert", &i, &v))
        return NULL;

    normalize_index(self->nbits, &i);

    if (insert_n(self, i, 1) < 0)
        return NULL;
    if (set_item(self, i, v) < 0)
        return NULL;
    Py_RETURN_NONE;
}

PyDoc_STRVAR(insert_doc,
"insert(index, value, /)\n\
\n\
Insert `bool(value)` into the bitarray before index.");


static PyObject *
bitarray_pop(bitarrayobject *self, PyObject *args)
{
    idx_t i = -1;
    long vi;

    if (!PyArg_ParseTuple(args, "|L:pop", &i))
        return NULL;

    if (self->nbits == 0) {
        /* special case -- most common failure cause */
        PyErr_SetString(PyExc_IndexError, "pop from empty bitarray");
        return NULL;
    }
    if (i < 0)
        i += self->nbits;

    if (i < 0 || i >= self->nbits) {
        PyErr_SetString(PyExc_IndexError, "pop index out of range");
        return NULL;
    }
    vi = GETBIT(self, i);
    if (delete_n(self, i, 1) < 0)
        return NULL;
    return PyBool_FromLong(vi);
}

PyDoc_STRVAR(pop_doc,
"pop(index=-1, /) -> item\n\
\n\
Return the i-th (default last) element and delete it from the bitarray.\n\
Raises `IndexError` if bitarray is empty or index is out of range.");


static PyObject *
bitarray_remove(bitarrayobject *self, PyObject *v)
{
    idx_t i;
    long vi;

    vi = PyObject_IsTrue(v);
    if (vi < 0)
        return NULL;

    i = findfirst(self, vi, 0, self->nbits);
    if (i < 0) {
        PyErr_SetString(PyExc_ValueError, "remove(x): x not in bitarray");
        return NULL;
    }
    if (delete_n(self, i, 1) < 0)
        return NULL;
    Py_RETURN_NONE;
}

PyDoc_STRVAR(remove_doc,
"remove(value, /)\n\
\n\
Remove the first occurrence of `bool(value)` in the bitarray.\n\
Raises `ValueError` if item is not present.");


/* --------- special methods ----------- */

static PyObject *
bitarray_getitem(bitarrayobject *self, PyObject *a)
{
    PyObject *res;
    idx_t start, stop, step, slicelength, j, i = 0;

    if (IS_INDEX(a)) {
        if (getIndex(a, &i) < 0)
            return NULL;
        if (i < 0)
            i += self->nbits;
        if (i < 0 || i >= self->nbits) {
            PyErr_SetString(PyExc_IndexError, "bitarray index out of range");
            return NULL;
        }
        return PyBool_FromLong(GETBIT(self, i));
    }
    if (PySlice_Check(a)) {
        if (slice_GetIndicesEx((PySliceObject *) a, self->nbits,
                               &start, &stop, &step, &slicelength) < 0) {
            return NULL;
        }
        res = newbitarrayobject(Py_TYPE(self), slicelength, self->endian);
        if (res == NULL)
            return NULL;

        for (i = 0, j = start; i < slicelength; i++, j += step)
            setbit((bitarrayobject *) res, i, GETBIT(self, j));

        return res;
    }
    PyErr_SetString(PyExc_TypeError, "index or slice expected");
    return NULL;
}

/* Sets the elements, specified by slice, in self to the value(s) given by v
   which is either a bitarray or a boolean.
*/
static int
setslice(bitarrayobject *self, PySliceObject *slice, PyObject *v)
{
    idx_t start, stop, step, slicelength, j, i = 0;

    if (slice_GetIndicesEx(slice, self->nbits,
                           &start, &stop, &step, &slicelength) < 0)
        return -1;

    if (bitarray_Check(v)) {
#define vv  ((bitarrayobject *) v)
        if (vv->nbits == slicelength) {
            for (i = 0, j = start; i < slicelength; i++, j += step)
                setbit(self, j, GETBIT(vv, i));
            return 0;
        }
        if (step != 1) {
            char buff[256];
            sprintf(buff, "attempt to assign sequence of size %lld "
                          "to extended slice of size %lld",
                    vv->nbits, (idx_t) slicelength);
            PyErr_SetString(PyExc_ValueError, buff);
            return -1;
        }
        /* make self bigger or smaller */
        if (vv->nbits > slicelength) {
            if (insert_n(self, start, vv->nbits - slicelength) < 0)
                return -1;
        }
        else {
            if (delete_n(self, start, slicelength - vv->nbits) < 0)
                return -1;
        }
        /* copy the new values into self */
        copy_n(self, start, vv, 0, vv->nbits);
#undef vv
        return 0;
    }
    if (IS_INT_OR_BOOL(v)) {
        int vi;

        vi = IntBool_AsInt(v);
        if (vi < 0)
            return -1;
        for (i = 0, j = start; i < slicelength; i++, j += step)
            setbit(self, j, vi);
        return 0;
    }
    PyErr_SetString(PyExc_IndexError,
                    "bitarray or bool expected for slice assignment");
    return -1;
}

static PyObject *
bitarray_setitem(bitarrayobject *self, PyObject *args)
{
    PyObject *a, *v;
    idx_t i = 0;

    if (!PyArg_ParseTuple(args, "OO:__setitem__", &a, &v))
        return NULL;

    if (IS_INDEX(a)) {
        if (getIndex(a, &i) < 0)
            return NULL;
        if (i < 0)
            i += self->nbits;
        if (i < 0 || i >= self->nbits) {
            PyErr_SetString(PyExc_IndexError, "bitarray index out of range");
            return NULL;
        }
        if (set_item(self, i, v) < 0)
            return NULL;
        Py_RETURN_NONE;
    }
    if (PySlice_Check(a)) {
        if (setslice(self, (PySliceObject *) a, v) < 0)
            return NULL;
        Py_RETURN_NONE;
    }
    PyErr_SetString(PyExc_TypeError, "index or slice expected");
    return NULL;
}

static PyObject *
bitarray_delitem(bitarrayobject *self, PyObject *a)
{
    idx_t start, stop, step, slicelength, j, i = 0;

    if (IS_INDEX(a)) {
        if (getIndex(a, &i) < 0)
            return NULL;
        if (i < 0)
            i += self->nbits;
        if (i < 0 || i >= self->nbits) {
            PyErr_SetString(PyExc_IndexError, "bitarray index out of range");
            return NULL;
        }
        if (delete_n(self, i, 1) < 0)
            return NULL;
        Py_RETURN_NONE;
    }
    if (PySlice_Check(a)) {
        if (slice_GetIndicesEx((PySliceObject *) a, self->nbits,
                               &start, &stop, &step, &slicelength) < 0) {
            return NULL;
        }
        if (slicelength == 0)
            Py_RETURN_NONE;

        if (step < 0) {
            stop = start + 1;
            start = stop + step * (slicelength - 1) - 1;
            step = -step;
        }
        if (step == 1) {
            assert(stop - start == slicelength);
            if (delete_n(self, start, slicelength) < 0)
                return NULL;
            Py_RETURN_NONE;
        }
        /* this is the only complicated part when step > 1 */
        for (i = j = start; i < self->nbits; i++)
            if ((i - start) % step != 0 || i >= stop) {
                setbit(self, j, GETBIT(self, i));
                j++;
            }
        if (resize(self, self->nbits - slicelength) < 0)
            return NULL;
        Py_RETURN_NONE;
    }
    PyErr_SetString(PyExc_TypeError, "index or slice expected");
    return NULL;
}

/* ---------- number methods ---------- */

static PyObject *
bitarray_add(bitarrayobject *self, PyObject *other)
{
    PyObject *res;

    res = bitarray_copy(self);
    if (extend_dispatch((bitarrayobject *) res, other) < 0) {
        Py_DECREF(res);
        return NULL;
    }
    return res;
}

static PyObject *
bitarray_iadd(bitarrayobject *self, PyObject *other)
{
    if (extend_dispatch(self, other) < 0)
        return NULL;
    Py_INCREF(self);
    return (PyObject *) self;
}

static PyObject *
bitarray_mul(bitarrayobject *self, PyObject *v)
{
    PyObject *res;
    idx_t vi = 0;

    if (!IS_INDEX(v)) {
        PyErr_SetString(PyExc_TypeError,
                        "integer value expected for bitarray repetition");
        return NULL;
    }
    if (getIndex(v, &vi) < 0)
        return NULL;
    res = bitarray_copy(self);
    if (repeat((bitarrayobject *) res, vi) < 0) {
        Py_DECREF(res);
        return NULL;
    }
    return res;
}

static PyObject *
bitarray_imul(bitarrayobject *self, PyObject *v)
{
    idx_t vi = 0;

    if (!IS_INDEX(v)) {
        PyErr_SetString(PyExc_TypeError,
            "integer value expected for in-place bitarray repetition");
        return NULL;
    }
    if (getIndex(v, &vi) < 0)
        return NULL;
    if (repeat(self, vi) < 0)
        return NULL;
    Py_INCREF(self);
    return (PyObject *) self;
}

static PyObject *
bitarray_cpinvert(bitarrayobject *self)
{
    PyObject *res;

    res = bitarray_copy(self);
    invert((bitarrayobject *) res);
    return res;
}

#if HAS_VECTORS
#define BITWISE_FUNC_INTERNAL(SELF, OTHER, OP, OPEQ) do {   \
    Py_ssize_t i = 0;                                       \
    const Py_ssize_t size = Py_SIZE(SELF);                  \
    char* self_ob_item = (SELF)->ob_item;                   \
    const char* other_ob_item = (OTHER)->ob_item;           \
                                                            \
    for (; (Py_ssize_t)(i + sizeof(vec)) < size; i += sizeof(vec)) {      \
        vector_op(self_ob_item + i, other_ob_item + i, OP); \
    }                                                       \
                                                            \
    for (; i < size; ++i) {                                 \
        self_ob_item[i] OPEQ other_ob_item[i];              \
    }                                                       \
} while(0);
#else
#define BITWISE_FUNC_INTERNAL(SELF, OTHER, OP, OPEQ) do { \
    Py_ssize_t i;                                         \
    const Py_ssize_t size = Py_SIZE(SELF);                \
                                                          \
    for (i = 0; i < size; ++i) {                          \
        (SELF)->ob_item[i] OPEQ (OTHER)->ob_item[i];      \
    }                                                     \
} while(0);
#endif

#define BITWISE_FUNC(oper)  \
static PyObject *                                                   \
bitarray_ ## oper (bitarrayobject *self, PyObject *other)           \
{                                                                   \
    PyObject *res;                                                  \
                                                                    \
    res = bitarray_copy(self);                                      \
    if (bitwise((bitarrayobject *) res, other, OP_ ## oper) < 0) {  \
        Py_DECREF(res);                                             \
        return NULL;                                                \
    }                                                               \
    return res;                                                     \
}

BITWISE_FUNC(and)
BITWISE_FUNC(or)
BITWISE_FUNC(xor)


#define BITWISE_IFUNC(oper)  \
static PyObject *                                            \
bitarray_i ## oper (bitarrayobject *self, PyObject *other)   \
{                                                            \
    if (bitwise(self, other, OP_ ## oper) < 0)               \
        return NULL;                                         \
    Py_INCREF(self);                                         \
    return (PyObject *) self;                                \
}

BITWISE_IFUNC(and)
BITWISE_IFUNC(or)
BITWISE_IFUNC(xor)

/* -------------------------------------------- best terms -------------------------------------------- */

// Heap element
typedef struct {
    int64_t hwdiff;
    int64_t hw;
    uint64_t idx;
} topterm_heap_elem_t;

static int topterm_compare(const void *e1, const void *e2, const void *udata ATTR_UNUSED)
{
    UNUSEDVAR(udata);
    const topterm_heap_elem_t *i1 = e1;
    const topterm_heap_elem_t *i2 = e2;
    if (i2->hwdiff == i1->hwdiff){
        return 0;
    }
    return i1->hwdiff < i2->hwdiff ? 1 : -1;
}

// heap implementation: https://github.com/ph4r05/heap
typedef struct heap_s{
    unsigned int size;  /* size of array */
    unsigned int count; /* items within heap */
    const void *udata;  /* user data */
    int (*cmp) (const void *, const void *, const void *);
    void * array[];
} heap_t;

size_t heap_sizeof(unsigned int size)
{
    return sizeof(heap_t) + size * sizeof(void *);
}

#define HP_CHILD_LEFT(idx) ((idx) * 2 + 1)
#define HP_CHILD_RIGHT(idx) ((idx) * 2 + 2)
#define HP_PARENT(idx) (((idx) - 1) / 2)

void heap_init(heap_t* h, int (*cmp) (const void *,const void *, const void *udata), const void *udata, unsigned int size)
{
    h->cmp = cmp;
    h->udata = udata;
    h->size = size;
    h->count = 0;
}

heap_t *heap_new(int (*cmp) (const void *, const void *, const void *udata), const void *udata, unsigned int size)
{
    heap_t *h = PyMem_Malloc(heap_sizeof(size));

    if (!h)
        return NULL;

    heap_init(h, cmp, udata, size);

    return h;
}

void heap_free(heap_t * h)
{
    PyMem_Free(h);
}

int heap_count(const heap_t * h)
{
    return h->count;
}

/**
 * @return a new heap on success; NULL otherwise */
static heap_t* __ensurecapacity(heap_t * h)
{
    if (h->count < h->size)
        return h;

    h->size *= 2;

    return PyMem_Realloc(h, heap_sizeof(h->size));
}

static void __swap(heap_t * h, const int i1, const int i2)
{
    void *tmp = h->array[i1];
    h->array[i1] = h->array[i2];
    h->array[i2] = tmp;
}

static int __pushup(heap_t * h, unsigned int idx)
{
    /* 0 is the root node */
    while (0 != idx)
    {
        int parent = HP_PARENT(idx);

        /* we are smaller than the parent */
        if (h->cmp(h->array[idx], h->array[parent], h->udata) < 0)
            return -1;
        else
            __swap(h, idx, parent);

        idx = parent;
    }

    return idx;
}

static void __pushdown(heap_t * h, unsigned int idx)
{
    while (1)
    {
        unsigned int childl, childr, child;

        childl = HP_CHILD_LEFT(idx);
        childr = HP_CHILD_RIGHT(idx);

        if (childr >= h->count)
        {
            /* can't pushdown any further */
            if (childl >= h->count)
                return;

            child = childl;
        }
            /* find biggest child */
        else if (h->cmp(h->array[childl], h->array[childr], h->udata) < 0)
            child = childr;
        else
            child = childl;

        /* idx is smaller than child */
        if (h->cmp(h->array[idx], h->array[child], h->udata) < 0)
        {
            __swap(h, idx, child);
            idx = child;
            /* bigger than the biggest child, we stop, we win */
        }
        else
            return;
    }
}

static void __heap_offerx(heap_t * h, void *item)
{
    h->array[h->count] = item;
    __pushup(h, h->count++);
}

int heap_offerx(heap_t * h, void *item)
{
    if (h->count == h->size)
        return -1;
    __heap_offerx(h, item);
    return 0;
}

int heap_offer(heap_t ** h, void *item)
{
    if (NULL == (*h = __ensurecapacity(*h)))
        return -1;

    __heap_offerx(*h, item);
    return 0;
}

/* -------------------------------------------- term generator -------------------------------------------- */
#define MAX_DEG 64
typedef struct {
    int deg;
    int maxterm;
    int cur[MAX_DEG];
} termgen_t;


/**
 * Initializes term generator to a first combination
 */
static void init_termgen(termgen_t * t, int deg, int maxterm){
    int i;
    t->deg = deg;
    t->maxterm = maxterm;
    memset(t->cur, 0, sizeof(int)*MAX_DEG);
    for(i=0; i<deg; i++){
        t->cur[i] = i;
    }
}

/**
 * Moves termgen to a next combination
 */
static int next_termgen(termgen_t * t){
    int j;
    int idx = t->deg - 1;

    if (t->cur[idx] == t->maxterm - 1) {
        do {
            idx -= 1;
        } while (idx >= 0 && t->cur[idx] + 1 == t->cur[idx + 1]);

        if (idx < 0) {
            return 0;
        }

        for (j = idx + 1; j < t->deg; ++j) {
            t->cur[j] = t->cur[idx] + j - idx + 1;
        }
    }

    t->cur[idx]++;
    return 1;
}

#define OP_AND 1
#define OP_XOR 2

/*********************** (Bitarray) Base object *********************/

#define POLY_DYNAMIC 0
#define MAX_POLY_DEG 12
#define MAX_POLY_TERMS 12

typedef struct tpoly {
    Py_ssize_t nterms;
    Py_ssize_t mterm_ord;
    Py_ssize_t maxterm;
#if POLY_DYNAMIC
    Py_ssize_t * sizes;
    Py_ssize_t * poly;        // polynomial, array of term indices
#else
    Py_ssize_t sizes[MAX_POLY_TERMS];
    Py_ssize_t poly[MAX_POLY_DEG * MAX_POLY_TERMS];        // polynomial, array of term indices
#endif
} tpoly;
static void tpoly_destroy_body(tpoly * poly);

typedef struct {
    PyObject_HEAD
    Py_ssize_t maxterm;
    Py_ssize_t base_size;
    char * valids;
    bitarrayobject ** base;

    Py_ssize_t eval_buff_size;
    struct tpoly * eval_buff;
    BITWISE_HW_TYPE * hws_buff;
} tbase;

static PyTypeObject TBase_Type;

#define TBase_Check(op)  PyObject_TypeCheck(op, &TBase_Type)

static void
tbase_destroy(tbase * base, int full_dealloc){
    if (base == NULL){
        return;
    }

    for (idx_t k = 0; base->base != NULL && k < base->maxterm; k++) {
        if (base->base[k] == NULL){
            continue;
        }
        Py_DECREF(base->base[k]);
        base->base[k] = NULL;
    }

    if (base->base != NULL){
        PyMem_Free((void *) base->base);
        base->base = NULL;
    }

    if (base->valids != NULL){
        PyMem_Free((void *) base->valids);
        base->valids = NULL;
    }

    if (base->eval_buff != NULL){
        for(idx_t i = 0; i < base->eval_buff_size; ++i){
            tpoly_destroy_body(&(base->eval_buff[i]));
        }
        PyMem_Free((void *) base->eval_buff);
        base->eval_buff = NULL;
    }

    if (base->hws_buff != NULL){
        PyMem_Free((void *) base->hws_buff);
        base->hws_buff = NULL;
    }

    if (full_dealloc)
        PyMem_Free((void *) base);
}

static int
tbase_get(PyObject * base_arr, tbase ** ibase){
    if (ibase == NULL){
        PyErr_SetString(PyExc_ValueError, "empty base pointer");
        return 0;
    }

    if (!PyList_Check(base_arr)) {
        PyErr_SetString(PyExc_TypeError, "base is expected as a list of bit arrays");
        return 0;
    }

    const int we_alloc = *ibase == NULL;
    if (*ibase == NULL){
        *ibase = PyMem_Malloc((size_t) (sizeof(tbase)));
        if (*ibase == NULL) {
            PyErr_NoMemory();
            return 0;
        }
    }

    tbase *base = *ibase;
    base->maxterm = PyList_Size(base_arr);
    base->base = NULL;
    base->valids = NULL;
    base->eval_buff = NULL;
    base->hws_buff = NULL;
    base->base_size = -1;
    base->eval_buff_size = 0;

    base->base = PyMem_Malloc((size_t) (sizeof(PyObject *) * base->maxterm));
    if (base->base == NULL) {
        PyErr_NoMemory();
        tbase_destroy(base, we_alloc);
        return 0;
    }

    memset(base->base, 0, (sizeof(PyObject *) * base->maxterm));
    base->valids = PyMem_Malloc((size_t) (sizeof(char) * base->maxterm));
    if (base->valids == NULL) {
        PyErr_NoMemory();
        tbase_destroy(base, we_alloc);
        return 0;
    }

    memset(base->valids, 0, (sizeof(char) * base->maxterm));

    int ok = 1;
    for (idx_t k = 0; k < base->maxterm; ++k) {
        PyObject * tmp_obj = PyList_GetItem(base_arr, (Py_ssize_t) k);
        if (tmp_obj == Py_None){
            continue;
        }

        if (!bitarray_Check(tmp_obj)) {
            PyErr_SetString(PyExc_TypeError, "bitarray expected in the base array");
            ok = 0;
            break;
        }

        base->valids[k] = 1;
        base->base[k] = (bitarrayobject *) tmp_obj;
        Py_INCREF(base->base[k]);
        if (base->base_size < 0){
            base->base_size = base->base[k]->nbits;

        } else if (base->base[k]->nbits != base->base_size){
            PyErr_Format(PyExc_ValueError, "Base size has to be the same, idx: %d", (int) k);
            ok = 0;
            break;
        }
    }

    if (!ok){
        tbase_destroy(base, we_alloc);
        return 0;
    } else {
        return 1;
    }
}

static int
tbase_traverse(tbase *base, visitproc visit, void *arg)
{
    for (idx_t k = 0; base->base != NULL && k < base->maxterm; k++) {
        if (base->base[k] == NULL){
            continue;
        }
        Py_VISIT(base->base[k]);
    }
    return 0;
}

static void
tbase_dealloc(tbase *it)
{
    PyObject_GC_UnTrack(it);
    tbase_destroy(it, 0);
    Py_TYPE(it)->tp_free((PyObject *)it);
}

static int
tbase_eval_buff(tbase *base, PyObject * num){
    if (!PyNumber_Check(num)){
        return 1;
    }

    Py_ssize_t k = PyNumber_AsSsize_t(num, NULL);
    if (k <= 0){
        return 1;
    }

    base->eval_buff_size = 0;
    base->eval_buff = PyMem_Malloc((size_t) (sizeof(tpoly) * k));
    if (base->eval_buff == NULL) {
        PyErr_NoMemory();
        return 0;
    }

    base->hws_buff = PyMem_Malloc((size_t) (sizeof(BITWISE_HW_TYPE) * k));
    if (base->hws_buff == NULL) {
        PyMem_Free((void *) base->eval_buff);
        PyErr_NoMemory();
        return 0;
    }

    base->eval_buff_size = k;
    return 1;
}

static PyObject *
tbase_new(PyTypeObject *type, PyObject *args, PyObject *kwds)
{
    PyObject *ibase = NULL;
    PyObject *nbuff = NULL;
    static char *kwlist[] = {"base", "buff_size", NULL};

    if (!PyArg_ParseTupleAndKeywords(args, kwds, "|OO:tbase", kwlist, &ibase, &nbuff))
        return NULL;

    assert(type != NULL && type->tp_alloc != NULL);
    tbase * obj = (tbase *) type->tp_alloc(type, 0);
    if (obj == NULL) {
        PyErr_NoMemory();
        return NULL;
    }

    // https://docs.python.org/3/c-api/gcsupport.html
    // https://github.com/python/cpython/blob/master/Objects/dictobject.c
    // Whole object has to be valid when tracking. Thus untrack, setup, track again.
    _PyObject_GC_UNTRACK(obj);

    if (!tbase_get(ibase, &obj)){
        Py_DECREF(obj);
        return NULL;
    }

    if (!tbase_eval_buff(obj, nbuff)){
        Py_DECREF(obj);
        return NULL;
    }

    PyObject_GC_Track(obj);
    return (PyObject *) obj;
}

/*********************** Eval all terms *********************/

// eval_top_k. We need a simple heap - heap allocated top 128 elements
static PyObject *
eval_all_terms(PyObject *self, PyObject *args, PyObject *kwds)
{
    static char* kwlist[] = {"base", "deg", "topk", "hw_center", NULL};
    PyObject *base_arr;
    idx_t deg=2, topk=128, hw_center=0;
    long k = 0;
    int op_code = OP_AND;
    topterm_heap_elem_t * heap_data = NULL;

    //PySys_WriteStdout("Here! %p ");
    if (!PyArg_ParseTupleAndKeywords(args, kwds, "O|LLL:eval_all_terms", kwlist, &base_arr, &deg, &topk, &hw_center)) {
        return NULL;
    }

    if (deg < 2){
        PyErr_SetString(PyExc_IndexError, "Minimal degree is 2. For 1 use directly hw()");
        return NULL;
    }

    tbase * base = NULL;
    if (!tbase_get(base_arr, &base)){
        return NULL;
    }

    if (deg > base->maxterm){
        PyErr_SetString(PyExc_IndexError, "degree is larger than size of the base");
        tbase_destroy(base, 1);
        return NULL;
    }

    // Allocate memory for the heap objects, actual storage of the heap structs. Heap contains only pointers.
    heap_data = PyMem_Malloc((size_t) (sizeof(topterm_heap_elem_t) * topk));
    if (heap_data == NULL){
        PyErr_NoMemory();
        goto error;
    }

    // Prepare state & working variables
    //const Py_ssize_t size = Py_SIZE(base[0]);
    idx_t hw = 0, comb_idx=-1;

    // Sub result for deg-1
    int last_cached_tmpsub = -1;
    bitarrayobject *tmpsub = (bitarrayobject *) newbitarrayobject(Py_TYPE(base->base[0]), base->base[0]->nbits, base->base[0]->endian);
    if (tmpsub == NULL) {
        goto error;
    }

    // Create the heap
    heap_t *hp = heap_new(topterm_compare, NULL, (unsigned int) topk);

    // Generate all polynomials
    termgen_t termgen;
    init_termgen(&termgen, (int)deg, (int)base->maxterm);
    do {
        hw = 0;
        comb_idx += 1;
        const int idx_first = termgen.cur[0];
        const int idx_plast = termgen.cur[deg-2];
        const int idx_last = termgen.cur[deg-1];

        // Precompute tmpsub up to deg-2.
        // Copy the first basis vector. Then AND or XOR next basis vector from the term.
        if (last_cached_tmpsub != idx_plast){
            memcpy(tmpsub->ob_item, base->base[idx_first]->ob_item, Py_SIZE(base->base[idx_first]));
            for(k=1; k <= deg-2; k++){
                if (op_code == OP_AND) {
                    BITWISE_FUNC_INTERNAL(tmpsub, base->base[termgen.cur[k]], &, &=);
                } else if (op_code == OP_XOR){
                    BITWISE_FUNC_INTERNAL(tmpsub, base->base[termgen.cur[k]], ^, ^=);
                }
            }

            last_cached_tmpsub = idx_plast;
        }

        // Now do just in-memory HW counting.
        if (op_code == OP_AND) {
            BITWISE_HW_INTERNAL(tmpsub, base->base[idx_last], &);
        } else if (op_code == OP_XOR){
            BITWISE_HW_INTERNAL(tmpsub, base->base[idx_last], ^);
        }

        int64_t hw_diff = hw_center > hw ? (hw_center - hw) : (hw - hw_center);

        // Not interesting element (too low & heap is full already) -> continue.
        if (comb_idx >= hp->size && hw_diff <= ((topterm_heap_elem_t * )(hp->array[0]))->hwdiff){
            continue;
        }

        // Interesting element.
        if (comb_idx < hp->size){
            heap_data[comb_idx].hwdiff = hw_diff;
            heap_data[comb_idx].hw = hw;
            heap_data[comb_idx].idx = (uint64_t)comb_idx;
            heap_offerx(hp, &(heap_data[comb_idx]));

        } else {
            topterm_heap_elem_t * const being_replaced = (topterm_heap_elem_t *) hp->array[0];
            being_replaced->hwdiff = hw_diff;
            being_replaced->hw = hw;
            being_replaced->idx = (uint64_t)comb_idx;
            __pushdown(hp, 0);
        }

    } while(next_termgen(&termgen) == 1);

    // Return as an array of tuples. Sorting will be done in the python already.
    PyObject* ret_list = PyList_New((Py_ssize_t)hp->count);
    for(k=0; k < hp->count; k++){
        topterm_heap_elem_t * const cur = (topterm_heap_elem_t *) hp->array[k];
        PyObject* cur_tuple = PyTuple_Pack(3,
                                           PyLong_FromLongLong(cur->hwdiff),
                                           PyLong_FromLongLong(cur->hw),
                                           PyLong_FromLongLong(cur->idx));
        PyList_SetItem(ret_list, (Py_ssize_t)k, cur_tuple);
    }

    heap_free(hp);
    tbase_destroy(base, 1);
    if (heap_data != NULL){
        PyMem_Free((void *) heap_data);
    }

    return ret_list;

    error:
    tbase_destroy(base, 1);
    if (heap_data != NULL){
        PyMem_Free((void *) heap_data);
    }
    return NULL;
}

PyDoc_STRVAR(eval_all_terms_doc,
             "eval_all_terms(base, deg=2, topk=128, hw_center=0) -> list of (hwdiff, hw, idx)\n\
\n\
Evaluates all terms on the given basis");

static void
tpoly_destroy_body(tpoly * poly){
#if POLY_DYNAMIC
    if (poly == NULL){
        return;
    }

    if (poly->sizes){
        PyMem_Free((void *) poly->sizes);
        poly->sizes = NULL;
    }

    if (poly->poly){
        PyMem_Free((void *) poly->poly);
        poly->poly = NULL;
    }
#endif
}

static int
tpoly_get(PyObject * poly_arr, tpoly * poly, int fast){
    Py_ssize_t (* const ph4_size)(PyObject *o) = PyList_Size;
    PyObject * (* const ph4_item)(PyObject *o, Py_ssize_t i) = PyList_GetItem;
    #define PH4_DEC_ITEM(x)

    if (!PyList_Check(poly_arr)) {
        PyErr_SetString(PyExc_TypeError, "poly is expected to be list of lists of integers");
        return 0;
    }

    memset(poly, 0, sizeof(tpoly));
    poly->nterms = ph4_size(poly_arr);

#if POLY_DYNAMIC
    poly->sizes = PyMem_Malloc((size_t) (poly->nterms * sizeof(Py_ssize_t)));
    if (poly->sizes == NULL) {
        tpoly_destroy_body(poly);
        PyErr_NoMemory();
        return NULL;
    }
#endif

    int ok = 1;
    for (idx_t k = 0; k < poly->nterms && ok; ++k) {
        PyObject * tmp_obj = ph4_item(poly_arr, (Py_ssize_t) k);
        if (!fast && !PyList_Check(tmp_obj)) {
            PyErr_Format(PyExc_ValueError, "poly is expected to be list of lists of integers, idx: %d", (int) k);
            ok = 0;

        } else {
            const Py_ssize_t tsize = ph4_size(tmp_obj);
            poly->sizes[k] = tsize;
            poly->mterm_ord = tsize > poly->mterm_ord ? tsize : poly->mterm_ord;
        }
        PH4_DEC_ITEM(tmp_obj);
    }

    if (!ok){
        tpoly_destroy_body(poly);
        return 0;
    }

#if POLY_DYNAMIC
    poly->poly = PyMem_Malloc((size_t) (poly->nterms * poly->mterm_ord * sizeof(Py_ssize_t)));
    if (poly->poly == NULL) {
        tpoly_destroy_body(poly);
        PyErr_NoMemory();
        return NULL;
    }
#endif

    for (idx_t k = 0; k < poly->nterms && ok; ++k) {
        PyObject * tmp_obj = ph4_item(poly_arr, (Py_ssize_t) k);
        const Py_ssize_t tsize = ph4_size(tmp_obj);

        for (idx_t l = 0; l < tsize && ok; ++l) {
            PyObject * to2 = ph4_item(tmp_obj, (Py_ssize_t) l);
            if (!fast && !PyNumber_Check(to2)){
                PyErr_Format(PyExc_ValueError, "poly is expected to be list of lists of integers, idx: %d, %d", (int) k, (int) l);
                ok = 0;

            } else {
                const Py_ssize_t ct = PyNumber_AsSsize_t(to2, NULL);
                poly->poly[k * poly->mterm_ord + l] = ct;
                poly->maxterm = ct > poly->maxterm ? ct : poly->maxterm;
            }
            PH4_DEC_ITEM(to2);
        }
        PH4_DEC_ITEM(tmp_obj);
    }

    if (!ok){
        tpoly_destroy_body(poly);
        return 0;
    } else {
        return 1;
    }
#undef PH4_DEC_ITEM
}

static PyObject *
base_eval_polynomial_hw(tbase *base, PyObject *args, PyObject *kwds) {
    PyObject *poly_arr = NULL;
    PyObject *polys_arr = NULL;
    PyObject *res_arr = NULL;
    if (!PyArg_ParseTuple(args, "|OOO:eval_poly_hw", &poly_arr, &polys_arr, &res_arr)) {
        return NULL;
    }

    Py_ssize_t npolys = 1;
    tpoly bpolys[1];
    BITWISE_HW_TYPE bhws[1] = {0};

    tpoly * polys = (tpoly *) &bpolys;
    BITWISE_HW_TYPE * hws = &bhws[0];
    const int single_poly = poly_arr != NULL && poly_arr != Py_None;

    if (res_arr != NULL && res_arr != Py_None){
        if (!PyList_Check(res_arr)){
            PyErr_SetString(PyExc_ValueError, "res array has to be an array");
            goto error;
        }
    }

    if (single_poly){
        if (!tpoly_get(poly_arr, &polys[0], 1)) {
            goto error;
        }
    } else {
        polys = NULL;
        hws = NULL;
        if (!PyList_Check(polys_arr)){
            PyErr_SetString(PyExc_ValueError, "polys has to be array of polynomials");
            goto error;
        }

        npolys = PyList_Size(polys_arr);
        if (npolys == 0){
            return PyList_New(0);
        }

        if (npolys <= base->eval_buff_size){
            polys = base->eval_buff;
            hws = base->hws_buff;

        } else {
            polys = PyMem_Malloc((size_t) (npolys * sizeof(tpoly)));
            if (polys == NULL) {
                PyErr_NoMemory();
                goto error;
            }

            hws = PyMem_Malloc((size_t) (npolys * sizeof(BITWISE_HW_TYPE)));
            if (hws == NULL) {
                PyErr_NoMemory();
                goto error;
            }
        }

        memset(polys, 0, npolys * sizeof(tpoly));
        memset(hws, 0, npolys * sizeof(BITWISE_HW_TYPE));
        for(Py_ssize_t i = 0; i < npolys; ++i){
            if (!tpoly_get(PyList_GetItem(polys_arr, i), &polys[i], 1)) {
                goto error;
            }
        }
    }


//#define POLY_TERMS(p) (nterms)
//#define POLY_TERM_SIZE(p, k, t) (PyList_GET_SIZE(PyList_GET_ITEM(poly_arr, k)))
//#define POLY_TERM(p, k)  (PyList_GET_ITEM(poly_arr, k))
//#define POLY_TERM_FREE(p, k, t)
//#define POLY_VAR(p, k, l, t) (PyLong_AsSsize_t(PyList_GET_ITEM(t, l)))

#define POLY_TERMS(p) (polys[p].nterms)
#define POLY_TERM_SIZE(p, k, t) (polys[p].sizes[k])
#define POLY_TERM(p, k, name)
#define POLY_TERM_FREE(p, k, t)
#define POLY_IDX(p, k, l) ((k) * polys[p].mterm_ord + (l))
#define POLY_VAR(p, k, l, t) (polys[p].poly[POLY_IDX(p, k, l)])

#define POLY_OBJ(p, k, l, t) (base->base[POLY_VAR(p, k, l, t)])
#define POLY_DATA(p, k, l, t) ((POLY_OBJ(p, k, l, t))->ob_item)

    // BITWISE_HW_TYPE jumps
    Py_ssize_t off = 0;
    Py_ssize_t base_size_lim = 0;

#if HAS_VECTORS
    base_size_lim = base->base_size < (Py_ssize_t)sizeof(vec) ? 0 : (base->base_size >> 3) - ((base->base_size >> 3) % sizeof(vec));
    for(Py_ssize_t ii = 0; off < base_size_lim; ++ii, off += (Py_ssize_t)sizeof(vec)) {  // Base
        for(Py_ssize_t p = 0; p < npolys; ++p) {                         // polys
            vec res = bitv_00;
            for (Py_ssize_t k = 0; k < POLY_TERMS(p); ++k) {             // XOR
                vec subr = bitv_ff;
                POLY_TERM(p, k, term);
                for (Py_ssize_t l = 0; l < POLY_TERM_SIZE(p, k, term); ++l) {  // AND
                    vec ttt;
                    memcpy(&ttt, &(POLY_DATA(p, k, l, term)[off]), sizeof(vec));
                    subr &= ttt;
                }
                res ^= subr;
            }

            BITWISE_HW_TYPE ww = 0;
            memcpy(&ww, &res, 8);
            BITWISE_HW_WP3(ww, hws[p]);
            memcpy(&ww, ((char*)&res)+8, 8);
            BITWISE_HW_WP3(ww, hws[p]);
        }
    }
#endif

    // uint64_t finish
    base_size_lim = (base->base_size >> 3) - ((base->base_size >> 3) % sizeof(BITWISE_HW_TYPE));
    for(Py_ssize_t ii = 0; off < base_size_lim; ++ii, off += (Py_ssize_t)sizeof(BITWISE_HW_TYPE)) {  // Base
        for(Py_ssize_t p = 0; p < npolys; ++p) {                         // polys
            BITWISE_HW_TYPE res = 0;
            for (Py_ssize_t k = 0; k < POLY_TERMS(p); ++k) {             // XOR
                BITWISE_HW_TYPE subr = ~((BITWISE_HW_TYPE) 0);
                POLY_TERM(p, k, term);
                for (Py_ssize_t l = 0; l < POLY_TERM_SIZE(p, k, term); ++l) {  // AND
                    subr &= *(BITWISE_HW_TYPE *) (((char *) POLY_DATA(p, k, l, term)) + off);
                }
                res ^= subr;
            }
            BITWISE_HW_WP3(res, hws[p]);
        }
    }

    // Soft finish, bit-wise
    Py_ssize_t off_bits = off << 3;
    Py_ssize_t rem_bits = base->base_size - (off << 3);
    BITWISE_HW_TYPE res_mask = ((BITWISE_HW_TYPE)1 << (rem_bits)) - 1;
    for(Py_ssize_t p = 0; p < npolys; ++p) {                           // polys
        BITWISE_HW_TYPE res = 0;
        for (Py_ssize_t k = 0; k < POLY_TERMS(p); ++k) {               // XOR
            BITWISE_HW_TYPE subr = ~((BITWISE_HW_TYPE) 0);
            POLY_TERM(p, k, term);
            for (Py_ssize_t l = 0; l < POLY_TERM_SIZE(p, k, term); ++l) {    // AND
                BITWISE_HW_TYPE cur = 0;
                for (Py_ssize_t i = off_bits; i < base->base_size; ++i) {  // Base
                    cur |= (((BITWISE_HW_TYPE) GETBIT(POLY_OBJ(p, k, l, term), i)) << i);
                }
                subr &= cur;
            }
            res ^= subr;
        }
        res &= res_mask;
        BITWISE_HW_WP3(res, hws[p]);
    }

#define DEALLOC() \
    for(Py_ssize_t k = 0; polys != NULL && k < npolys; ++k)         \
        tpoly_destroy_body(&polys[k]);                              \
                                                                    \
    if (!single_poly && npolys > base->eval_buff_size){             \
        if (hws != NULL)                                            \
            PyMem_Free((void *) hws);                               \
        if (polys != NULL)                                          \
            PyMem_Free((void *) polys);                             \
    }

    // Return
    PyObject * ret = !single_poly ? NULL : PyLong_FromLongLong(hws[0]);

    if (single_poly) {
        DEALLOC()
        return ret;

    } else {
        if (res_arr == NULL || res_arr == Py_None){
            ret = PyList_New(npolys);
            if (ret == NULL){
                PyErr_NoMemory();
                goto error;
            }
            Py_INCREF(ret);
        } else {
            ret = res_arr;
            Py_INCREF(ret);
        }

        for(Py_ssize_t p = 0; p < npolys; ++p) {
            if (PyList_SetItem(ret, p, PyLong_FromLongLong(hws[p]))){
                PyErr_SetString(PyExc_ValueError, "Error adding result to the array");
                goto error;
            }
        }

        DEALLOC()
        return ret;
    }

    error:
DEALLOC()
    return NULL;

#undef STEPTYPE
#undef DEALLOC
#undef POLY_OBJ
#undef POLY_DATA
#undef POLY_VAR
#undef POLY_IDX
#undef POLY_TERM
#undef POLY_TERM_FREE
#undef POLY_TERMS
#undef POLY_TERM_SIZE
}

PyDoc_STRVAR(base_eval_polynomial_hw_doc,
             "eval_poly_hw(base, poly) -> hw\n\
\n\
Computes Hamming weight of the polynomial evaluated on the basis");

static PyMethodDef
    tbase_methods[] = {
        {"eval_poly_hw", (PyCFunction) base_eval_polynomial_hw, METH_VARARGS, base_eval_polynomial_hw_doc},
        {NULL,           NULL}  /* sentinel */
};

static PyTypeObject TBase_Type = {
#ifdef IS_PY3K
        PyVarObject_HEAD_INIT(NULL, 0)
#else
        PyObject_HEAD_INIT(NULL)
        0,                                           /* ob_size */
#endif
        "bitarray._tbase",                          /* tp_name */
        sizeof(tbase),                                       /* tp_basicsize */
        0,                                       /* tp_itemsize */
        /* methods */
        (destructor) tbase_dealloc,                          /* tp_dealloc */
        0,                                          /* tp_print */
        0,                                          /* tp_getattr */
        0,                                         /* tp_setattr */
        0,                                       /* tp_compare */
        0,                                        /* tp_repr */
        0,                                        /* tp_as_number */
        0,                                        /* tp_as_sequence */
        0,                                        /* tp_as_mapping */
        0,                                        /* tp_hash */
        0,                                        /* tp_call */
        0,                                        /* tp_str */
        PyObject_GenericGetAttr,                  /* tp_getattro */
        0,                                        /* tp_setattro */
        0,                                        /* tp_as_buffer */
        Py_TPFLAGS_DEFAULT | Py_TPFLAGS_HAVE_GC,  /* tp_flags */
        0,                                        /* tp_doc */
        (traverseproc)tbase_traverse,                     /* tp_traverse */
        0,                                        /* tp_clear  - empty for immutable */
        0,                                  /* tp_richcompare */
        0,                                /* tp_weaklistoffset */
        0,                                        /* tp_iter */
        0,                                        /* tp_iternext */
        tbase_methods,                            /* tp_methods */
        0,                                        /* tp_members */
        0,                                        /* tp_getset */
        0,                                        /* tp_base */
        0,                                        /* tp_dict */
        0,                                        /* tp_descr_get */
        0,                                        /* tp_descr_set */
        0,                                        /* tp_dictoffset */
        0,                                        /* tp_init */
        PyType_GenericAlloc,                     /* tp_alloc */
        tbase_new,                                /* tp_new */
        PyObject_GC_Del
};

/******************* variable length encoding and decoding ***************/

static int
check_codedict(PyObject *codedict)
{
    PyObject *key, *value;
    Py_ssize_t pos = 0;

    if (!PyDict_Check(codedict)) {
        PyErr_SetString(PyExc_TypeError, "dict expected");
        return -1;
    }
    if (PyDict_Size(codedict) == 0) {
        PyErr_SetString(PyExc_ValueError, "prefix code dict empty");
        return -1;
    }
    while (PyDict_Next(codedict, &pos, &key, &value)) {
        if (!bitarray_Check(value)) {
            PyErr_SetString(PyExc_TypeError,
                            "bitarray expected for dict value");
            return -1;
        }
        if (((bitarrayobject *) value)->nbits == 0) {
            PyErr_SetString(PyExc_ValueError, "non-empty bitarray expected");
            return -1;
        }
    }
    return 0;
}

static PyObject *
bitarray_encode(bitarrayobject *self, PyObject *args)
{
    PyObject *codedict, *iterable, *iter, *symbol, *bits;

    if (!PyArg_ParseTuple(args, "OO:encode", &codedict, &iterable))
        return NULL;

    if (check_codedict(codedict) < 0)
        return NULL;

    iter = PyObject_GetIter(iterable);
    if (iter == NULL) {
        PyErr_SetString(PyExc_TypeError, "iterable object expected");
        return NULL;
    }
    /* extend self with the bitarrays from codedict */
    while ((symbol = PyIter_Next(iter)) != NULL) {
        bits = PyDict_GetItem(codedict, symbol);
        Py_DECREF(symbol);
        if (bits == NULL) {
            PyErr_SetString(PyExc_ValueError,
                            "symbol not defined in prefix code");
            goto error;
        }
        if (extend_bitarray(self, (bitarrayobject *) bits) < 0)
            goto error;
    }
    Py_DECREF(iter);
    if (PyErr_Occurred())
        return NULL;
    Py_RETURN_NONE;
error:
    Py_DECREF(iter);
    return NULL;
}

PyDoc_STRVAR(encode_doc,
"encode(code, iterable, /)\n\
\n\
Given a prefix code (a dict mapping symbols to bitarrays),\n\
iterate over the iterable object with symbols, and extend the bitarray\n\
with the corresponding bitarray for each symbols.");


/* Binary tree definition */
typedef struct _bin_node
{
    struct _bin_node *child[2];
    PyObject *symbol;
} binode;


static binode *
new_binode(void)
{
    binode *nd;

    nd = (binode *) PyMem_Malloc(sizeof(binode));
    if (nd == NULL) {
        PyErr_NoMemory();
        return NULL;
    }
    nd->child[0] = NULL;
    nd->child[1] = NULL;
    nd->symbol = NULL;
    return nd;
}

static void
delete_binode_tree(binode *tree)
{
    if (tree == NULL)
        return;

    delete_binode_tree(tree->child[0]);
    delete_binode_tree(tree->child[1]);
    PyMem_Free(tree);
}

static int
insert_symbol(binode *tree, bitarrayobject *ba, PyObject *symbol)
{
    binode *nd = tree, *prev;
    Py_ssize_t i;
    int k;

    for (i = 0; i < ba->nbits; i++) {
        k = GETBIT(ba, i);
        prev = nd;
        nd = nd->child[k];

        /* we cannot have already a symbol when branching to the new leaf */
        if (nd && nd->symbol)
            goto ambiguity;

        if (!nd) {
            nd = new_binode();
            if (nd == NULL)
                return -1;
            prev->child[k] = nd;
        }
    }
    /* the new leaf node cannot already have a symbol or children */
    if (nd->symbol || nd->child[0] || nd->child[1])
        goto ambiguity;

    nd->symbol = symbol;
    return 0;

 ambiguity:
    PyErr_SetString(PyExc_ValueError, "prefix code ambiguous");
    return -1;
}

static binode *
make_tree(PyObject *codedict)
{
    binode *tree;
    PyObject *symbol, *array;
    Py_ssize_t pos = 0;

    tree = new_binode();
    if (tree == NULL)
        return NULL;

    while (PyDict_Next(codedict, &pos, &symbol, &array)) {
        if (insert_symbol(tree, (bitarrayobject *) array, symbol) < 0) {
            delete_binode_tree(tree);
            return NULL;
        }
    }
    return tree;
}

/*
  Traverse tree using the branches corresponding to the bitarray `ba`,
  starting at *indexp.  Return the symbol at the leaf node, or NULL
  when the end of the bitarray has been reached, or on error (in which
  case the appropriate PyErr_SetString is set.
*/
static PyObject *
traverse_tree(binode *tree, bitarrayobject *ba, idx_t *indexp)
{
    binode *nd = tree;
    int k;

    while (*indexp < ba->nbits) {
        k = GETBIT(ba, *indexp);
        (*indexp)++;
        nd = nd->child[k];
        if (nd == NULL) {
            PyErr_SetString(PyExc_ValueError,
                            "prefix code does not match data in bitarray");
            return NULL;
        }
        if (nd->symbol)  /* leaf */
            return nd->symbol;
    }
    if (nd != tree)
        PyErr_SetString(PyExc_ValueError, "decoding not terminated");

    return NULL;
}

static PyObject *
bitarray_decode(bitarrayobject *self, PyObject *codedict)
{
    binode *tree, *nd;
    PyObject *list;
    Py_ssize_t i;
    int k;

    if (check_codedict(codedict) < 0)
        return NULL;

    tree = make_tree(codedict);
    if (tree == NULL || PyErr_Occurred())
        return NULL;

    nd = tree;
    list = PyList_New(0);
    if (list == NULL) {
        delete_binode_tree(tree);
        return NULL;
    }
    /* traverse tree (just like above) */
    for (i = 0; i < self->nbits; i++) {
        k = GETBIT(self, i);
        nd = nd->child[k];
        if (nd == NULL) {
            PyErr_SetString(PyExc_ValueError,
                            "prefix code does not match data in bitarray");
            goto error;
        }
        if (nd->symbol) {  /* leaf */
            if (PyList_Append(list, nd->symbol) < 0)
                goto error;
            nd = tree;
        }
    }
    if (nd != tree) {
        PyErr_SetString(PyExc_ValueError, "decoding not terminated");
        goto error;
    }
    delete_binode_tree(tree);
    return list;

error:
    delete_binode_tree(tree);
    Py_DECREF(list);
    return NULL;
}

PyDoc_STRVAR(decode_doc,
"decode(code, /) -> list\n\
\n\
Given a prefix code (a dict mapping symbols to bitarrays),\n\
decode the content of the bitarray and return it as a list of symbols.");

/*********************** (Bitarray) Decode Iterator *********************/


typedef struct {
    PyObject_HEAD
    bitarrayobject *bao;        /* bitarray we're searching in */
    binode *tree;               /* prefix tree containing symbols */
    idx_t index;                /* current index in bitarray */
} decodeiterobject;

static PyTypeObject DecodeIter_Type;

#define DecodeIter_Check(op)  PyObject_TypeCheck(op, &DecodeIter_Type)



/* create a new initialized bitarray search iterator object */
static PyObject *
bitarray_iterdecode(bitarrayobject *self, PyObject *codedict)
{
    decodeiterobject *it;  /* iterator to be returned */
    binode *tree;

    if (check_codedict(codedict) < 0)
        return NULL;

    tree = make_tree(codedict);
    if (tree == NULL || PyErr_Occurred())
        return NULL;

    it = PyObject_GC_New(decodeiterobject, &DecodeIter_Type);
    if (it == NULL)
        return NULL;

    it->tree = tree;

    Py_INCREF(self);
    it->bao = self;
    it->index = 0;
    PyObject_GC_Track(it);
    return (PyObject *) it;
}

PyDoc_STRVAR(iterdecode_doc,
"iterdecode(code, /) -> iterator\n\
\n\
Given a prefix code (a dict mapping symbols to bitarrays),\n\
decode the content of the bitarray and return an iterator over\n\
the symbols.");

static PyObject *
decodeiter_next(decodeiterobject *it)
{
    PyObject *symbol;

    assert(DecodeIter_Check(it));
    symbol = traverse_tree(it->tree, it->bao, &(it->index));
    if (symbol == NULL)  /* stop iteration OR error occured */
        return NULL;
    Py_INCREF(symbol);
    return symbol;
}

static void
decodeiter_dealloc(decodeiterobject *it)
{
    delete_binode_tree(it->tree);
    PyObject_GC_UnTrack(it);
    Py_XDECREF(it->bao);
    PyObject_GC_Del(it);
}

static int
decodeiter_traverse(decodeiterobject *it, visitproc visit, void *arg)
{
    Py_VISIT(it->bao);
    return 0;
}

static PyTypeObject DecodeIter_Type = {
#ifdef IS_PY3K
    PyVarObject_HEAD_INIT(NULL, 0)
#else
    PyObject_HEAD_INIT(NULL)
    0,                                        /* ob_size */
#endif
    "bitarraydecodeiterator",                 /* tp_name */
    sizeof(decodeiterobject),                 /* tp_basicsize */
    0,                                        /* tp_itemsize */
    /* methods */
    (destructor) decodeiter_dealloc,          /* tp_dealloc */
    0,                                        /* tp_print */
    0,                                        /* tp_getattr */
    0,                                        /* tp_setattr */
    0,                                        /* tp_compare */
    0,                                        /* tp_repr */
    0,                                        /* tp_as_number */
    0,                                        /* tp_as_sequence */
    0,                                        /* tp_as_mapping */
    0,                                        /* tp_hash */
    0,                                        /* tp_call */
    0,                                        /* tp_str */
    PyObject_GenericGetAttr,                  /* tp_getattro */
    0,                                        /* tp_setattro */
    0,                                        /* tp_as_buffer */
    Py_TPFLAGS_DEFAULT | Py_TPFLAGS_HAVE_GC,  /* tp_flags */
    0,                                        /* tp_doc */
    (traverseproc) decodeiter_traverse,       /* tp_traverse */
    0,                                        /* tp_clear */
    0,                                        /* tp_richcompare */
    0,                                        /* tp_weaklistoffset */
    PyObject_SelfIter,                        /* tp_iter */
    (iternextfunc) decodeiter_next,           /* tp_iternext */
    0,                                        /* tp_methods */
};

/*********************** (Bitarray) Search Iterator *********************/

typedef struct {
    PyObject_HEAD
    bitarrayobject *bao;        /* bitarray we're searching in */
    bitarrayobject *xa;         /* bitarray being searched for */
    idx_t p;                    /* current search position */
} searchiterobject;

static PyTypeObject SearchIter_Type;

#define SearchIter_Check(op)  PyObject_TypeCheck(op, &SearchIter_Type)

/* create a new initialized bitarray search iterator object */
static PyObject *
bitarray_itersearch(bitarrayobject *self, PyObject *x)
{
    searchiterobject *it;  /* iterator to be returned */
    bitarrayobject *xa;

    if (!bitarray_Check(x)) {
        PyErr_SetString(PyExc_TypeError, "bitarray expected for itersearch");
        return NULL;
    }
    xa = (bitarrayobject *) x;
    if (xa->nbits == 0) {
        PyErr_SetString(PyExc_ValueError, "can't search for empty bitarray");
        return NULL;
    }

    it = PyObject_GC_New(searchiterobject, &SearchIter_Type);
    if (it == NULL)
        return NULL;

    Py_INCREF(self);
    it->bao = self;
    Py_INCREF(xa);
    it->xa = xa;
    it->p = 0;  /* start search at position 0 */
    PyObject_GC_Track(it);
    return (PyObject *) it;
}

PyDoc_STRVAR(itersearch_doc,
"itersearch(bitarray, /) -> iterator\n\
\n\
Searches for the given a bitarray in self, and return an iterator over\n\
the start positions where bitarray matches self.");

static PyObject *
searchiter_next(searchiterobject *it)
{
    idx_t p;

    assert(SearchIter_Check(it));
    p = search(it->bao, it->xa, it->p);
    if (p < 0)  /* no more positions -- stop iteration */
        return NULL;
    it->p = p + 1;  /* next search position */
    return PyLong_FromLongLong(p);
}

static void
searchiter_dealloc(searchiterobject *it)
{
    PyObject_GC_UnTrack(it);
    Py_XDECREF(it->bao);
    Py_XDECREF(it->xa);
    PyObject_GC_Del(it);
}

static int
searchiter_traverse(searchiterobject *it, visitproc visit, void *arg)
{
    Py_VISIT(it->bao);
    return 0;
}

static PyTypeObject SearchIter_Type = {
#ifdef IS_PY3K
    PyVarObject_HEAD_INIT(NULL, 0)
#else
    PyObject_HEAD_INIT(NULL)
    0,                                        /* ob_size */
#endif
    "bitarraysearchiterator",                 /* tp_name */
    sizeof(searchiterobject),                 /* tp_basicsize */
    0,                                        /* tp_itemsize */
    /* methods */
    (destructor) searchiter_dealloc,          /* tp_dealloc */
    0,                                        /* tp_print */
    0,                                        /* tp_getattr */
    0,                                        /* tp_setattr */
    0,                                        /* tp_compare */
    0,                                        /* tp_repr */
    0,                                        /* tp_as_number */
    0,                                        /* tp_as_sequence */
    0,                                        /* tp_as_mapping */
    0,                                        /* tp_hash */
    0,                                        /* tp_call */
    0,                                        /* tp_str */
    PyObject_GenericGetAttr,                  /* tp_getattro */
    0,                                        /* tp_setattro */
    0,                                        /* tp_as_buffer */
    Py_TPFLAGS_DEFAULT | Py_TPFLAGS_HAVE_GC,  /* tp_flags */
    0,                                        /* tp_doc */
    (traverseproc) searchiter_traverse,       /* tp_traverse */
    0,                                        /* tp_clear */
    0,                                        /* tp_richcompare */
    0,                                        /* tp_weaklistoffset */
    PyObject_SelfIter,                        /* tp_iter */
    (iternextfunc) searchiter_next,           /* tp_iternext */
    0,                                        /* tp_methods */
};

/*************************** Method definitions *************************/

static PyMethodDef
bitarray_methods[] = {
    {"all",          (PyCFunction) bitarray_all,         METH_NOARGS,
     all_doc},
    {"any",          (PyCFunction) bitarray_any,         METH_NOARGS,
     any_doc},
    {"append",       (PyCFunction) bitarray_append,      METH_O,
     append_doc},
    {"buffer_info",  (PyCFunction) bitarray_buffer_info, METH_NOARGS,
     buffer_info_doc},
    {"bytereverse",  (PyCFunction) bitarray_bytereverse, METH_NOARGS,
     bytereverse_doc},
    {"copy",         (PyCFunction) bitarray_copy,        METH_NOARGS,
     copy_doc},
    {"count",        (PyCFunction) bitarray_count,       METH_VARARGS,
     count_doc},
    {"decode",       (PyCFunction) bitarray_decode,      METH_O,
     decode_doc},
    {"iterdecode",   (PyCFunction) bitarray_iterdecode,  METH_O,
     iterdecode_doc},
    {"encode",       (PyCFunction) bitarray_encode,      METH_VARARGS,
     encode_doc},
    {"endian",       (PyCFunction) bitarray_endian,      METH_NOARGS,
     endian_doc},
    {"extend",       (PyCFunction) bitarray_extend,      METH_O,
     extend_doc},
    {"fill",         (PyCFunction) bitarray_fill,        METH_NOARGS,
     fill_doc},
    {"fromfile",     (PyCFunction) bitarray_fromfile,    METH_VARARGS,
     fromfile_doc},
    {"frombytes",    (PyCFunction) bitarray_frombytes,   METH_O,
     frombytes_doc},
    {"index",        (PyCFunction) bitarray_index,       METH_VARARGS,
     index_doc},
    {"insert",       (PyCFunction) bitarray_insert,      METH_VARARGS,
     insert_doc},
    {"invert",       (PyCFunction) bitarray_invert,      METH_NOARGS,
     invert_doc},
    {"length",       (PyCFunction) bitarray_length,      METH_NOARGS,
     length_doc},
    {"pack",         (PyCFunction) bitarray_pack,        METH_O,
     pack_doc},
    {"pop",          (PyCFunction) bitarray_pop,         METH_VARARGS,
     pop_doc},
    {"remove",       (PyCFunction) bitarray_remove,      METH_O,
     remove_doc},
    {"reverse",      (PyCFunction) bitarray_reverse,     METH_NOARGS,
     reverse_doc},
    {"setall",       (PyCFunction) bitarray_setall,      METH_O,
     setall_doc},
    {"search",       (PyCFunction) bitarray_search,      METH_VARARGS,
     search_doc},
    {"itersearch",   (PyCFunction) bitarray_itersearch,  METH_O,
     itersearch_doc},
    {"sort",         (PyCFunction) bitarray_sort,        METH_VARARGS |
                                                         METH_KEYWORDS,
     sort_doc},
    {"tofile",       (PyCFunction) bitarray_tofile,      METH_O,
     tofile_doc},
    {"tolist",       (PyCFunction) bitarray_tolist,      METH_NOARGS,
     tolist_doc},
    {"tobytes",      (PyCFunction) bitarray_tobytes,     METH_NOARGS,
     tobytes_doc},
    {"to01",         (PyCFunction) bitarray_to01,        METH_NOARGS,
     to01_doc},
    {"unpack",       (PyCFunction) bitarray_unpack,      METH_VARARGS |
                                                         METH_KEYWORDS,
     unpack_doc},
    {"eval_monic",   (PyCFunction) bitarray_eval_monic,  METH_VARARGS |
                                                         METH_KEYWORDS,
      eval_monic_doc},
    {"fast_copy",   (PyCFunction) bitarray_fast_copy,    METH_O,
     fast_copy_doc},
    {"fast_hw_and",   (PyCFunction) bitwise_fast_hw_and, METH_O,
      bitwise_fast_hw_and_doc},
    {"fast_hw_or",   (PyCFunction) bitwise_fast_hw_or,   METH_O,
      bitwise_fast_hw_or_doc},
    {"fast_hw_xor",   (PyCFunction) bitwise_fast_hw_xor, METH_O,
      bitwise_fast_hw_xor_doc},

    /* special methods */
    {"__copy__",     (PyCFunction) bitarray_copy,        METH_NOARGS,
     copy_doc},
    {"__deepcopy__", (PyCFunction) bitarray_copy,        METH_O,
     copy_doc},
    {"__len__",      (PyCFunction) bitarray_length,      METH_NOARGS,
     len_doc},
    {"__contains__", (PyCFunction) bitarray_contains,    METH_O,
     contains_doc},
    {"__reduce__",   (PyCFunction) bitarray_reduce,      METH_NOARGS,
     reduce_doc},

    /* slice methods */
    {"__delitem__",  (PyCFunction) bitarray_delitem,     METH_O,       0},
    {"__getitem__",  (PyCFunction) bitarray_getitem,     METH_O,       0},
    {"__setitem__",  (PyCFunction) bitarray_setitem,     METH_VARARGS, 0},

    /* number methods */
    {"__add__",      (PyCFunction) bitarray_add,         METH_O,       0},
    {"__iadd__",     (PyCFunction) bitarray_iadd,        METH_O,       0},
    {"__mul__",      (PyCFunction) bitarray_mul,         METH_O,       0},
    {"__rmul__",     (PyCFunction) bitarray_mul,         METH_O,       0},
    {"__imul__",     (PyCFunction) bitarray_imul,        METH_O,       0},
    {"__and__",      (PyCFunction) bitarray_and,         METH_O,       0},
    {"__or__",       (PyCFunction) bitarray_or,          METH_O,       0},
    {"__xor__",      (PyCFunction) bitarray_xor,         METH_O,       0},
    {"__iand__",     (PyCFunction) bitarray_iand,        METH_O,       0},
    {"__ior__",      (PyCFunction) bitarray_ior,         METH_O,       0},
    {"__ixor__",     (PyCFunction) bitarray_ixor,        METH_O,       0},
    {"__invert__",   (PyCFunction) bitarray_cpinvert,    METH_NOARGS,  0},

    {NULL,           NULL}  /* sentinel */
};


static PyObject *
bitarray_new(PyTypeObject *type, PyObject *args, PyObject *kwds)
{
    PyObject *a;  /* to be returned in some cases */
    PyObject *initial = NULL;
    char *endian_str = NULL;
    int endian;
    static char *kwlist[] = {"initial", "endian", NULL};

    if (!PyArg_ParseTupleAndKeywords(args, kwds,
                        "|Os:bitarray", kwlist, &initial, &endian_str))
        return NULL;

    if (endian_str == NULL) {
        endian = DEFAULT_ENDIAN;  /* use default value */
    }
    else if (strcmp(endian_str, "little") == 0) {
        endian = 0;
    }
    else if (strcmp(endian_str, "big") == 0) {
        endian = 1;
    }
    else {
        PyErr_SetString(PyExc_ValueError,
                        "endian must be 'little' or 'big'");
        return NULL;
    }

    /* no arg or None */
    if (initial == NULL || initial == Py_None)
        return newbitarrayobject(type, 0, endian);

    /* int, long */
    if (IS_INDEX(initial)) {
        idx_t nbits = 0;

        if (getIndex(initial, &nbits) < 0)
            return NULL;
        if (nbits < 0) {
            PyErr_SetString(PyExc_ValueError,
                            "cannot create bitarray with negative length");
            return NULL;
        }
        return newbitarrayobject(type, nbits, endian);
    }

    /* from bitarray itself */
    if (bitarray_Check(initial)) {
#define np  ((bitarrayobject *) initial)
        a = newbitarrayobject(type, np->nbits,
                              endian_str == NULL ? np->endian : endian);
        if (a == NULL)
            return NULL;
        memcpy(((bitarrayobject *) a)->ob_item, np->ob_item, Py_SIZE(np));
#undef np
        return a;
    }

    /* bytes */
    if (PyBytes_Check(initial)) {
        Py_ssize_t strlen;
        char *str;

        strlen = PyBytes_Size(initial);
        if (strlen == 0)        /* empty string */
            return newbitarrayobject(type, 0, endian);

        str = PyBytes_AsString(initial);
        if (0 <= str[0] && str[0] < 8) {
            /* when the first character is smaller than 8, it indicates the
               number of unused bits at the end, and rest of the bytes
               consist of the raw binary data, this is used for pickling */
            if (strlen == 1 && str[0] > 0) {
                PyErr_Format(PyExc_ValueError,
                             "did not expect 0x0%d", (int) str[0]);
                return NULL;
            }
            a = newbitarrayobject(type, BITS(strlen - 1) - ((idx_t) str[0]),
                                  endian);
            if (a == NULL)
                return NULL;
            memcpy(((bitarrayobject *) a)->ob_item, str + 1, strlen - 1);
            return a;
        }
    }

#define CHECK_TYPE(type)  \
    if (Py ## type ## _Check(initial)) {                                  \
        PyErr_SetString(PyExc_TypeError,                                  \
                        "cannot create bitarray from " #type " object");  \
        return NULL;                                                      \
    }
CHECK_TYPE(Float)
CHECK_TYPE(Complex)
#undef CHECK_TYPE

    /* leave remaining type dispatch to the extend method */
    a = newbitarrayobject(type, 0, endian);
    if (a == NULL)
        return NULL;
    if (extend_dispatch((bitarrayobject *) a, initial) < 0) {
        Py_DECREF(a);
        return NULL;
    }
    return a;
}


static PyObject *
richcompare(PyObject *v, PyObject *w, int op)
{
    int cmp, vi, wi;
    idx_t i, vs, ws;

    if (!bitarray_Check(v) || !bitarray_Check(w)) {
        Py_INCREF(Py_NotImplemented);
        return Py_NotImplemented;
    }
#define va  ((bitarrayobject *) v)
#define wa  ((bitarrayobject *) w)
    vs = va->nbits;
    ws = wa->nbits;
    if (vs != ws) {
        /* shortcut for EQ/NE: if sizes differ, the bitarrays differ */
        if (op == Py_EQ)
            Py_RETURN_FALSE;
        if (op == Py_NE)
            Py_RETURN_TRUE;
    }

    /* to avoid uninitialized warning for some compilers */
    vi = wi = 0;
    /* search for the first index where items are different */
    for (i = 0; i < vs && i < ws; i++) {
        vi = GETBIT(va, i);
        wi = GETBIT(wa, i);
        if (vi != wi) {
            /* we have an item that differs -- first, shortcut for EQ/NE */
            if (op == Py_EQ)
                Py_RETURN_FALSE;
            if (op == Py_NE)
                Py_RETURN_TRUE;
            /* compare the final item using the proper operator */
            switch (op) {
            case Py_LT: cmp = vi <  wi; break;
            case Py_LE: cmp = vi <= wi; break;
            case Py_EQ: cmp = vi == wi; break;
            case Py_NE: cmp = vi != wi; break;
            case Py_GT: cmp = vi >  wi; break;
            case Py_GE: cmp = vi >= wi; break;
            default: return NULL;  /* cannot happen */
            }
            return PyBool_FromLong((long) cmp);
        }
    }
#undef va
#undef wa

    /* no more items to compare -- compare sizes */
    switch (op) {
    case Py_LT: cmp = vs <  ws; break;
    case Py_LE: cmp = vs <= ws; break;
    case Py_EQ: cmp = vs == ws; break;
    case Py_NE: cmp = vs != ws; break;
    case Py_GT: cmp = vs >  ws; break;
    case Py_GE: cmp = vs >= ws; break;
    default: return NULL;  /* cannot happen */
    }
    return PyBool_FromLong((long) cmp);
}

/************************** Bitarray Iterator **************************/

typedef struct {
    PyObject_HEAD
    bitarrayobject *bao;        /* bitarray we're iterating over */
    idx_t index;                /* current index in bitarray */
} bitarrayiterobject;

static PyTypeObject BitarrayIter_Type;

#define BitarrayIter_Check(op)  PyObject_TypeCheck(op, &BitarrayIter_Type)

/* create a new initialized bitarray iterator object, this object is
   returned when calling item(a) */
static PyObject *
bitarray_iter(bitarrayobject *self)
{
    bitarrayiterobject *it;

    assert(bitarray_Check(self));
    it = PyObject_GC_New(bitarrayiterobject, &BitarrayIter_Type);
    if (it == NULL)
        return NULL;

    Py_INCREF(self);
    it->bao = self;
    it->index = 0;
    PyObject_GC_Track(it);
    return (PyObject *) it;
}

static PyObject *
bitarrayiter_next(bitarrayiterobject *it)
{
    long vi;

    assert(BitarrayIter_Check(it));
    if (it->index < it->bao->nbits) {
        vi = GETBIT(it->bao, it->index);
        it->index++;
        return PyBool_FromLong(vi);
    }
    return NULL;  /* stop iteration */
}

static void
bitarrayiter_dealloc(bitarrayiterobject *it)
{
    PyObject_GC_UnTrack(it);
    Py_XDECREF(it->bao);
    PyObject_GC_Del(it);
}

static int
bitarrayiter_traverse(bitarrayiterobject *it, visitproc visit, void *arg)
{
    Py_VISIT(it->bao);
    return 0;
}

static PyTypeObject BitarrayIter_Type = {
#ifdef IS_PY3K
    PyVarObject_HEAD_INIT(NULL, 0)
#else
    PyObject_HEAD_INIT(NULL)
    0,                                        /* ob_size */
#endif
    "bitarrayiterator",                       /* tp_name */
    sizeof(bitarrayiterobject),               /* tp_basicsize */
    0,                                        /* tp_itemsize */
    /* methods */
    (destructor) bitarrayiter_dealloc,        /* tp_dealloc */
    0,                                        /* tp_print */
    0,                                        /* tp_getattr */
    0,                                        /* tp_setattr */
    0,                                        /* tp_compare */
    0,                                        /* tp_repr */
    0,                                        /* tp_as_number */
    0,                                        /* tp_as_sequence */
    0,                                        /* tp_as_mapping */
    0,                                        /* tp_hash */
    0,                                        /* tp_call */
    0,                                        /* tp_str */
    PyObject_GenericGetAttr,                  /* tp_getattro */
    0,                                        /* tp_setattro */
    0,                                        /* tp_as_buffer */
    Py_TPFLAGS_DEFAULT | Py_TPFLAGS_HAVE_GC,  /* tp_flags */
    0,                                        /* tp_doc */
    (traverseproc) bitarrayiter_traverse,     /* tp_traverse */
    0,                                        /* tp_clear */
    0,                                        /* tp_richcompare */
    0,                                        /* tp_weaklistoffset */
    PyObject_SelfIter,                        /* tp_iter */
    (iternextfunc) bitarrayiter_next,         /* tp_iternext */
    0,                                        /* tp_methods */
};

/********************* Bitarray Buffer Interface ************************/
#ifdef WITH_BUFFER

#if PY_MAJOR_VERSION == 2       /* old buffer protocol */
static Py_ssize_t
bitarray_buffer_getreadbuf(bitarrayobject *self,
                           Py_ssize_t index, const void **ptr)
{
    if (index != 0) {
        PyErr_SetString(PyExc_SystemError, "accessing non-existent segment");
        return -1;
    }
    *ptr = (void *) self->ob_item;
    return Py_SIZE(self);
}

static Py_ssize_t
bitarray_buffer_getwritebuf(bitarrayobject *self,
                            Py_ssize_t index, const void **ptr)
{
    if (index != 0) {
        PyErr_SetString(PyExc_SystemError, "accessing non-existent segment");
        return -1;
    }
    *ptr = (void *) self->ob_item;
    return Py_SIZE(self);
}

static Py_ssize_t
bitarray_buffer_getsegcount(bitarrayobject *self, Py_ssize_t *lenp)
{
    if (lenp)
        *lenp = Py_SIZE(self);
    return 1;
}

static Py_ssize_t
bitarray_buffer_getcharbuf(bitarrayobject *self,
                           Py_ssize_t index, const char **ptr)
{
    if (index != 0) {
        PyErr_SetString(PyExc_SystemError, "accessing non-existent segment");
        return -1;
    }
    *ptr = self->ob_item;
    return Py_SIZE(self);
}

#endif

static int
bitarray_getbuffer(bitarrayobject *self, Py_buffer *view, int flags)
{
    int ret;
    void *ptr;

    if (view == NULL) {
        self->ob_exports++;
        return 0;
    }
    ptr = (void *) self->ob_item;
    ret = PyBuffer_FillInfo(view, (PyObject *) self, ptr,
                            Py_SIZE(self), 0, flags);
    if (ret >= 0) {
        self->ob_exports++;
    }
    return ret;
}

static void
bitarray_releasebuffer(bitarrayobject *self, Py_buffer *view)
{
    self->ob_exports--;
}

static PyBufferProcs bitarray_as_buffer = {
#if PY_MAJOR_VERSION == 2   /* old buffer protocol */
    (readbufferproc) bitarray_buffer_getreadbuf,
    (writebufferproc) bitarray_buffer_getwritebuf,
    (segcountproc) bitarray_buffer_getsegcount,
    (charbufferproc) bitarray_buffer_getcharbuf,
#endif
    (getbufferproc) bitarray_getbuffer,
    (releasebufferproc) bitarray_releasebuffer,
};

#endif  /* WITH_BUFFER */

/************************** Bitarray Type *******************************/

static PyTypeObject Bitarraytype = {
#ifdef IS_PY3K
    PyVarObject_HEAD_INIT(NULL, 0)
#else
    PyObject_HEAD_INIT(NULL)
    0,                                        /* ob_size */
#endif
    "bitarray._bitarray",                     /* tp_name */
    sizeof(bitarrayobject),                   /* tp_basicsize */
    0,                                        /* tp_itemsize */
    /* methods */
    (destructor) bitarray_dealloc,            /* tp_dealloc */
    0,                                        /* tp_print */
    0,                                        /* tp_getattr */
    0,                                        /* tp_setattr */
    0,                                        /* tp_compare */
    (reprfunc) bitarray_repr,                 /* tp_repr */
    0,                                        /* tp_as_number*/
    0,                                        /* tp_as_sequence */
    0,                                        /* tp_as_mapping */
    PyObject_HashNotImplemented,              /* tp_hash */
    0,                                        /* tp_call */
    0,                                        /* tp_str */
    PyObject_GenericGetAttr,                  /* tp_getattro */
    0,                                        /* tp_setattro */
#ifdef WITH_BUFFER
    &bitarray_as_buffer,                      /* tp_as_buffer */
#else
    0,                                        /* tp_as_buffer */
#endif
    Py_TPFLAGS_DEFAULT | Py_TPFLAGS_BASETYPE | Py_TPFLAGS_HAVE_WEAKREFS
#if defined(WITH_BUFFER) && PY_MAJOR_VERSION == 2
    | Py_TPFLAGS_HAVE_NEWBUFFER
#endif
    ,                                         /* tp_flags */
    0,                                        /* tp_doc */
    0,                                        /* tp_traverse */
    0,                                        /* tp_clear */
    richcompare,                              /* tp_richcompare */
    offsetof(bitarrayobject, weakreflist),    /* tp_weaklistoffset */
    (getiterfunc) bitarray_iter,              /* tp_iter */
    0,                                        /* tp_iternext */
    bitarray_methods,                         /* tp_methods */
    0,                                        /* tp_members */
    0,                                        /* tp_getset */
    0,                                        /* tp_base */
    0,                                        /* tp_dict */
    0,                                        /* tp_descr_get */
    0,                                        /* tp_descr_set */
    0,                                        /* tp_dictoffset */
    0,                                        /* tp_init */
    PyType_GenericAlloc,                      /* tp_alloc */
    bitarray_new,                             /* tp_new */
    PyObject_Del,                             /* tp_free */
};

/*************************** Module functions **********************/

static PyObject *
bitdiff(PyObject *self, PyObject *args)
{
    PyObject *a, *b;
    Py_ssize_t i;
    idx_t res = 0;
    unsigned char c;

    if (!PyArg_ParseTuple(args, "OO:bitdiff", &a, &b))
        return NULL;
    if (!(bitarray_Check(a) && bitarray_Check(b))) {
        PyErr_SetString(PyExc_TypeError, "bitarray object expected");
        return NULL;
    }

#define aa  ((bitarrayobject *) a)
#define bb  ((bitarrayobject *) b)
    if (aa->nbits != bb->nbits) {
        PyErr_SetString(PyExc_ValueError,
                        "bitarrays of equal length expected");
        return NULL;
    }
    setunused(aa);
    setunused(bb);
    for (i = 0; i < Py_SIZE(aa); i++) {
        c = aa->ob_item[i] ^ bb->ob_item[i];
        res += bitcount_lookup[c];
    }
#undef aa
#undef bb
    return PyLong_FromLongLong(res);
}

PyDoc_STRVAR(bitdiff_doc,
"bitdiff(a, b, /) -> int\n\
\n\
Return the difference between two bitarrays a and b.\n\
This is function does the same as (a ^ b).count(), but is more memory\n\
efficient, as no intermediate bitarray object gets created.\n\
Deprecated since version 1.2.0, use `bitarray.util.count_xor()` instead.");


static PyObject *
bits2bytes(PyObject *self, PyObject *v)
{
    idx_t n = 0;

    if (!IS_INDEX(v)) {
        PyErr_SetString(PyExc_TypeError, "integer expected");
        return NULL;
    }
    if (getIndex(v, &n) < 0)
        return NULL;
    if (n < 0) {
        PyErr_SetString(PyExc_ValueError, "non-negative integer expected");
        return NULL;
    }
    return PyLong_FromLongLong(BYTES(n));
}

PyDoc_STRVAR(bits2bytes_doc,
"bits2bytes(n, /) -> int\n\
\n\
Return the number of bytes necessary to store n bits.");


static PyObject *
sysinfo(void)
{
    return Py_BuildValue("iiiiL",
                         (int) sizeof(void *),
                         (int) sizeof(size_t),
                         (int) sizeof(Py_ssize_t),
                         (int) sizeof(idx_t),
                         (idx_t) PY_SSIZE_T_MAX);
}

PyDoc_STRVAR(sysinfo_doc,
"_sysinfo() -> tuple\n\
\n\
tuple(sizeof(void *),\n\
      sizeof(size_t),\n\
      sizeof(Py_ssize_t),\n\
      sizeof(idx_t),\n\
      PY_SSIZE_T_MAX)");

/*
   In retrospect, I wish I had never added any modules functions here.
   These, and possibly many others should be part of a separate utility
   module.  Anyway, at this point (2019) it is too late to remove them,
   so I will just leave them here, but not any new ones.
*/
static PyMethodDef module_functions[] = {
    {"bitdiff",    (PyCFunction) bitdiff,    METH_VARARGS, bitdiff_doc   },
    {"bits2bytes", (PyCFunction) bits2bytes, METH_O,       bits2bytes_doc},
    {"_sysinfo",   (PyCFunction) sysinfo,    METH_NOARGS,  sysinfo_doc   },
    {"eval_all_terms", (PyCFunction) eval_all_terms, METH_VARARGS | METH_KEYWORDS, eval_all_terms_doc },
    {NULL,         NULL}  /* sentinel */
};

/*********************** Install Module **************************/

#ifdef IS_PY3K
static PyModuleDef moduledef = {
    PyModuleDef_HEAD_INIT, "_bitarray", 0, -1, module_functions,
};
PyMODINIT_FUNC
PyInit__bitarray(void)
#else
PyMODINIT_FUNC
init_bitarray(void)
#endif
{
    PyObject *m;

    Py_TYPE(&Bitarraytype) = &PyType_Type;
    Py_TYPE(&SearchIter_Type) = &PyType_Type;
    Py_TYPE(&DecodeIter_Type) = &PyType_Type;
    Py_TYPE(&BitarrayIter_Type) = &PyType_Type;
    Py_TYPE(&TBase_Type) = &PyType_Type;
#ifdef IS_PY3K
    m = PyModule_Create(&moduledef);
    if (m == NULL)
        return NULL;
#else
    m = Py_InitModule3("_bitarray", module_functions, 0);
    if (m == NULL)
        return;
#endif

    Py_INCREF((PyObject *) &Bitarraytype);
    PyModule_AddObject(m, "_bitarray", (PyObject *) &Bitarraytype);

    Py_INCREF((PyObject *) &TBase_Type);
    PyModule_AddObject(m, "_tbase", (PyObject *) &TBase_Type);
#ifdef IS_PY3K
    return m;
#endif
}
