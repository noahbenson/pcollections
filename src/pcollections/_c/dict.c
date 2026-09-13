///////////////////////////////////////////////////////////////////////////////
// _C/dict.c
// Implementation of the persistent and transient dictionary types using the
// Array Mapped Trie (AMT) and Fixed Arity Trie (FAT) types.


//=============================================================================
// Initialization.

#include <Python.h>
#include <stdatomic.h>
#include <string.h>
#include "uintbits.h"
#include "trie.h"
#include "atm.h"
#include "fat.h"

#ifdef __cplusplus
#  define EXTC extern "C"
#else
#  define EXTC
#endif


//=============================================================================
// 
