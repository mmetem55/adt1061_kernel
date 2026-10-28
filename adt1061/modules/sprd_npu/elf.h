/*
 * Copyright (c) 2012 by Tensilica Inc. ALL RIGHTS RESERVED.
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to deal
 * in the Software without restriction, including without limitation the rights
 * to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 * copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 * AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
 * THE SOFTWARE.
 */

/* Low-level ELF data structures and other definitions. */

#ifndef _ELF_H_
#define _ELF_H_

/*
 * This file used to redefine the entire ELF32 spec by hand (all the
 * Elf32_xxx types and structs, the ELF32_ST_xxx and ELF32_R_xxx macros,
 * and every ET, SHT, STB, STT, PT and DT constant) under its own
 * include guard, completely independent of the kernel own linux/elf.h.
 * Since both define the exact same standard names, including both in
 * one translation unit (which happens here, since common.c and
 * pi_library_load.c also pull in linux/module.h, which pulls in
 * linux/elf.h) is a hard conflict: this file object-like macros used
 * to get substituted into the kernel header own typedef lines,
 * mangling them, and the struct typedefs collided outright as
 * duplicate, differently shaped definitions.
 *
 * Fix: just use the kernel own linux/elf.h for all of the standard
 * ELF32 pieces (it is byte for byte the same spec) and keep only what
 * is genuinely Xtensa specific and not part of the generic kernel
 * header below: EM_XTENSA and EM_XTENSA_DEPRECATED, SHT_XTENSA_AUTOTIE,
 * and the xtensa_relocations enum and its R_XTENSA constants.
 */
#include <linux/elf.h>

/* Not part of <linux/elf.h> (the kernel just uses plain "unsigned char" in
 * e_ident[]/st_info/st_other); kept as its own name, so it can't collide. */
typedef unsigned char Elf32_Byte;

#ifndef EM_XTENSA
#define EM_XTENSA      94
#endif
#ifndef EM_XTENSA_DEPRECATED
#define EM_XTENSA_DEPRECATED  0xabc7
#endif

#define SHT_XTENSA_AUTOTIE 0x7f000000  /* AutoTIE analysis information. */

// List of xtensa relocations. See bintutils/bfd/relocs.c for an
// explanation of each of these.

typedef enum xtensa_relocations {
  R_XTENSA_NONE = 0,
  R_XTENSA_32 = 1,
  R_XTENSA_RTLD = 2,
  R_XTENSA_GLOB_DAT = 3,
  R_XTENSA_JMP_SLOT = 4,
  R_XTENSA_RELATIVE = 5,
  R_XTENSA_PLT = 6,
  R_XTENSA_OP0 = 8,
  R_XTENSA_OP1 = 9,
  R_XTENSA_OP2 = 10, 
  R_XTENSA_ASM_EXPAND = 11,
  R_XTENSA_ASM_SIMPLIFY = 12,
  R_XTENSA_GNU_VTINHERIT = 15,
  R_XTENSA_GNU_VTENTRY = 16,
  R_XTENSA_DIFF8 = 17,
  R_XTENSA_DIFF16 = 18,
  R_XTENSA_DIFF32 = 19,
  R_XTENSA_SLOT0_OP = 20,
  R_XTENSA_SLOT1_OP = 21,
  R_XTENSA_SLOT2_OP = 22,
  R_XTENSA_SLOT3_OP = 23,
  R_XTENSA_SLOT4_OP = 24,
  R_XTENSA_SLOT5_OP = 25,
  R_XTENSA_SLOT6_OP = 26,
  R_XTENSA_SLOT7_OP = 27,
  R_XTENSA_SLOT8_OP = 28,
  R_XTENSA_SLOT9_OP = 29,
  R_XTENSA_SLOT10_OP = 30,
  R_XTENSA_SLOT11_OP = 31,
  R_XTENSA_SLOT12_OP = 32,
  R_XTENSA_SLOT13_OP = 33,
  R_XTENSA_SLOT14_OP = 34,
  R_XTENSA_SLOT0_ALT = 35,
  R_XTENSA_SLOT1_ALT = 36,
  R_XTENSA_SLOT2_ALT = 37,
  R_XTENSA_SLOT3_ALT = 38,
  R_XTENSA_SLOT4_ALT = 39,
  R_XTENSA_SLOT5_ALT = 40,
  R_XTENSA_SLOT6_ALT = 41,
  R_XTENSA_SLOT7_ALT = 42,
  R_XTENSA_SLOT8_ALT = 43,
  R_XTENSA_SLOT9_ALT = 44,
  R_XTENSA_SLOT10_ALT = 45,
  R_XTENSA_SLOT11_ALT = 46,
  R_XTENSA_SLOT12_ALT = 47,
  R_XTENSA_SLOT13_ALT = 48,
  R_XTENSA_SLOT14_ALT = 49
} xtensa_relocations;

#endif /* ! _ELF_H_ */
