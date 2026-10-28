/*
 * ec.h - optional companion header for EC sources.
 *
 * Nothing here is needed by generated code; it just gives you the shorthand
 * typedefs used in the examples plus plain-C spellings of the two EC
 * extensions, handy when a file has to compile with a bare C compiler.
 */
#ifndef EC_H
#define EC_H

typedef unsigned char      U8;
typedef unsigned short     U16;
typedef unsigned int       U32;
typedef unsigned long long ULL;

/* x@N written out longhand */
#define BIT(x, n)       (((x) >> (n)) & 1)
#define BITSET(x, n)    ((x) |= (1ULL << (n)))
#define BITCLEAR(x, n)  ((x) &= ~(1ULL << (n)))
#define BITFLIP(x, n)   ((x) ^= (1ULL << (n)))
#define BITPUT(x, n, v) ((x) = ((x) & ~(1ULL << (n))) | ((ULL)!!(v) << (n)))

#endif /* EC_H */
