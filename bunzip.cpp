#include "bunzip.hpp"
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <setjmp.h>
#include <vector>

/* Constants for huffman coding */
#define MAX_GROUPS          6
#define GROUP_SIZE          50
#define MAX_HUFCODE_BITS    20
#define MAX_SYMBOLS         258
#define SYMBOL_RUNA         0
#define SYMBOL_RUNB         1

/* Status return values */
#define RETVAL_OK                       0
#define RETVAL_LAST_BLOCK               (-1)
#define RETVAL_NOT_BZIP_DATA            (-2)
#define RETVAL_UNEXPECTED_INPUT_EOF     (-3)
#define RETVAL_UNEXPECTED_OUTPUT_EOF    (-4)
#define RETVAL_DATA_ERROR               (-5)
#define RETVAL_OUT_OF_MEMORY            (-6)
#define RETVAL_OBSOLETE_INPUT           (-7)

#define IOBUF_SIZE          4096

static const int BZ2_rNums[512] = { 
   619, 720, 127, 481, 931, 816, 813, 233, 566, 247, 
   985, 724, 205, 454, 863, 491, 741, 242, 949, 214, 
   733, 859, 335, 708, 621, 574, 73, 654, 730, 472, 
   419, 436, 278, 496, 867, 210, 399, 680, 480, 51, 
   878, 465, 811, 169, 869, 675, 611, 697, 867, 561, 
   862, 687, 507, 283, 482, 129, 807, 591, 733, 623, 
   150, 238, 59, 379, 684, 877, 625, 169, 643, 105, 
   170, 607, 520, 932, 727, 476, 693, 425, 174, 647, 
   73, 122, 335, 530, 442, 853, 695, 249, 445, 515, 
   909, 545, 703, 919, 874, 474, 882, 500, 594, 612, 
   641, 801, 220, 162, 819, 984, 589, 513, 495, 799, 
   161, 604, 958, 533, 221, 400, 386, 867, 600, 782, 
   382, 596, 414, 171, 516, 375, 682, 485, 911, 276, 
   98, 553, 163, 354, 666, 933, 424, 341, 533, 870, 
   227, 730, 475, 186, 263, 647, 537, 686, 600, 224, 
   469, 68, 770, 919, 190, 373, 294, 822, 808, 206, 
   184, 943, 795, 384, 383, 461, 404, 758, 839, 887, 
   715, 67, 618, 276, 204, 918, 873, 777, 604, 560, 
   951, 160, 578, 722, 79, 804, 96, 409, 713, 940, 
   652, 934, 970, 447, 318, 353, 859, 672, 112, 785, 
   645, 863, 803, 350, 139, 93, 354, 99, 820, 908, 
   609, 772, 154, 274, 580, 184, 79, 626, 630, 742, 
   653, 282, 762, 623, 680, 81, 927, 626, 789, 125, 
   411, 521, 938, 300, 821, 78, 343, 175, 128, 250, 
   170, 774, 972, 275, 999, 639, 495, 78, 352, 126, 
   857, 956, 358, 619, 580, 124, 737, 594, 701, 612, 
   669, 112, 134, 694, 363, 992, 809, 743, 168, 974, 
   944, 375, 748, 52, 600, 747, 642, 182, 862, 81, 
   344, 805, 988, 739, 511, 655, 814, 334, 249, 515, 
   897, 955, 664, 981, 649, 113, 974, 459, 893, 228, 
   433, 837, 553, 268, 926, 240, 102, 654, 459, 51, 
   686, 754, 806, 760, 493, 403, 415, 394, 687, 700, 
   946, 670, 656, 610, 738, 392, 760, 799, 887, 653, 
   978, 321, 576, 617, 626, 502, 894, 679, 243, 440, 
   680, 879, 194, 572, 640, 724, 926, 56, 204, 700, 
   707, 151, 457, 449, 797, 195, 791, 558, 945, 679, 
   297, 59, 87, 824, 713, 663, 412, 693, 342, 606, 
   134, 108, 571, 364, 631, 212, 174, 643, 304, 329, 
   343, 97, 430, 751, 497, 314, 983, 374, 822, 928, 
   140, 206, 73, 263, 980, 736, 876, 478, 430, 305, 
   170, 514, 364, 692, 829, 82, 855, 953, 676, 246, 
   369, 970, 294, 750, 807, 827, 150, 790, 288, 923, 
   804, 378, 215, 828, 592, 281, 565, 555, 710, 82, 
   896, 831, 547, 261, 524, 462, 293, 465, 502, 56, 
   661, 821, 976, 991, 658, 869, 905, 758, 745, 193, 
   768, 550, 608, 933, 378, 286, 215, 979, 792, 961, 
   61, 688, 793, 644, 986, 403, 106, 366, 905, 644, 
   372, 567, 466, 434, 645, 210, 389, 550, 919, 135, 
   780, 773, 635, 389, 707, 100, 626, 958, 165, 504, 
   920, 176, 193, 713, 857, 265, 203, 50, 668, 108, 
   645, 990, 626, 197, 510, 357, 358, 850, 858, 364, 
   936, 638
};

struct group_data {
    int limit[MAX_HUFCODE_BITS], base[MAX_HUFCODE_BITS], permute[MAX_SYMBOLS];
    char minLen, maxLen;
};

struct bunzip_data {
    jmp_buf jmpbuf;
    const unsigned char *inbuf;
    int inbufCount, inbufPos;
    unsigned int inbufBitCount, inbufBits;

    char outbuf[IOBUF_SIZE];
    int outbufPos;

    std::vector<uint8_t>* vec_out;

    unsigned int crc32Table[256], headerCRC, dataCRC, totalCRC;
    unsigned int *dbuf, dbufSize;
    int writePos, writeRun, writeCount, writeCurrent;

    int blockRandomised;
    int rNToGo, rTPos;

    unsigned char selectors[32768];
    struct group_data groups[MAX_GROUPS];
};

static unsigned int get_bits(bunzip_data *bd, char bits_wanted) {
    unsigned int bits = 0;
    while (bd->inbufBitCount < (unsigned int)bits_wanted) {
        if (bd->inbufPos == bd->inbufCount) {
            longjmp(bd->jmpbuf, RETVAL_UNEXPECTED_INPUT_EOF);
        }
        if (bd->inbufBitCount >= 24) {
            bits = bd->inbufBits & ((1 << bd->inbufBitCount) - 1);
            bits_wanted -= bd->inbufBitCount;
            bits <<= bits_wanted;
            bd->inbufBitCount = 0;
        }
        bd->inbufBits = (bd->inbufBits << 8) | bd->inbuf[bd->inbufPos++];
        bd->inbufBitCount += 8;
    }
    bd->inbufBitCount -= bits_wanted;
    bits |= (bd->inbufBits >> bd->inbufBitCount) & ((1 << bits_wanted) - 1);
    return bits;
}

static int get_next_block(bunzip_data *bd) {
    struct group_data *hufGroup;
    int dbufCount, groupCount, *base, *limit, selector,
        i, j, k, t, runPos, symCount, symTotal, nSelectors,
        byteCount[256];
    unsigned char uc, symToByte[256], mtfSymbol[256], *selectors;
    unsigned int *dbuf = bd->dbuf, origPtr;

    for (i = 0; i < 6; i++) mtfSymbol[i] = (unsigned char)get_bits(bd, 8);
    mtfSymbol[6] = 0;
    bd->headerCRC = get_bits(bd, 32);

    if (std::memcmp(mtfSymbol, "\x17\x72\x45\x38\x50\x90", 6) == 0)
        return RETVAL_LAST_BLOCK;
    if (std::memcmp(mtfSymbol, "\x31\x41\x59\x26\x53\x59", 6) != 0)
        return RETVAL_NOT_BZIP_DATA;

    bd->blockRandomised = (int)get_bits(bd, 1);
    bd->rNToGo = 0;
    bd->rTPos = 0;

    origPtr = get_bits(bd, 24);

    t = get_bits(bd, 16);
    symTotal = 0;
    for (i = 0; i < 16; i++) {
        if (t & (1 << (15 - i))) {
            k = get_bits(bd, 16);
            for (j = 0; j < 16; j++)
                if (k & (1 << (15 - j))) symToByte[symTotal++] = (16 * i) + j;
        }
    }

    groupCount = get_bits(bd, 3);
    if (groupCount < 2 || groupCount > MAX_GROUPS) return RETVAL_DATA_ERROR;

    if (!(nSelectors = get_bits(bd, 15))) return RETVAL_DATA_ERROR;
    selectors = bd->selectors;
    for (i = 0; i < groupCount; i++) mtfSymbol[i] = i;
    for (i = 0; i < nSelectors; i++) {
        for (j = 0; get_bits(bd, 1); j++) if (j >= groupCount) return RETVAL_DATA_ERROR;
        uc = mtfSymbol[j];
        std::memmove(mtfSymbol + 1, mtfSymbol, j);
        mtfSymbol[0] = selectors[i] = uc;
    }

    symCount = symTotal + 2;
    for (j = 0; j < groupCount; j++) {
        unsigned char length[MAX_SYMBOLS], temp[MAX_HUFCODE_BITS + 1];
        int minLen, maxLen, pp;

        t = get_bits(bd, 5);
        for (i = 0; i < symCount; i++) {
            for (;;) {
                if (t < 1 || t > MAX_HUFCODE_BITS) return RETVAL_DATA_ERROR;
                if (!get_bits(bd, 1)) break;
                if (!get_bits(bd, 1)) t++;
                else t--;
            }
            length[i] = t;
        }

        minLen = maxLen = length[0];
        for (i = 1; i < symCount; i++) {
            if (length[i] > maxLen) maxLen = length[i];
            else if (length[i] < minLen) minLen = length[i];
        }

        hufGroup = bd->groups + j;
        hufGroup->minLen = minLen;
        hufGroup->maxLen = maxLen;
        base = hufGroup->base - 1;
        limit = hufGroup->limit - 1;

        pp = 0;
        for (i = minLen; i <= maxLen; i++)
            for (t = 0; t < symCount; t++)
                if (length[t] == i) hufGroup->permute[pp++] = t;

        for (i = minLen; i <= maxLen; i++) temp[i] = limit[i] = 0;
        for (i = 0; i < symCount; i++) temp[length[i]]++;

        pp = t = 0;
        for (i = minLen; i < maxLen; i++) {
            pp += temp[i];
            limit[i] = pp - 1;
            pp <<= 1;
            base[i + 1] = pp - (t += temp[i]);
        }
        limit[maxLen] = pp + temp[maxLen] - 1;
        base[minLen] = 0;
    }

    std::memset(byteCount, 0, 256 * sizeof(int));
    for (i = 0; i < 256; i++) mtfSymbol[i] = (unsigned char)i;

    runPos = dbufCount = symCount = selector = 0;
    for (;;) {
        if (!(symCount--)) {
            symCount = GROUP_SIZE - 1;
            if (selector >= nSelectors) return RETVAL_DATA_ERROR;
            hufGroup = bd->groups + selectors[selector++];
            base = hufGroup->base - 1;
            limit = hufGroup->limit - 1;
        }

        i = hufGroup->minLen;
        j = get_bits(bd, i);
        for (;;) {
            if (i > hufGroup->maxLen) return RETVAL_DATA_ERROR;
            if (j <= limit[i]) break;
            i++;
            j = (j << 1) | get_bits(bd, 1);
        }

        j -= base[i];
        if (j < 0 || j >= MAX_SYMBOLS) return RETVAL_DATA_ERROR;
        int nextSym = hufGroup->permute[j];

        if (nextSym == SYMBOL_RUNA || nextSym == SYMBOL_RUNB) {
            if (!runPos) {
                runPos = 1;
                t = 0;
            }
            if (nextSym == SYMBOL_RUNA) t += runPos;
            else t += 2 * runPos;
            runPos <<= 1;
            continue;
        }

        if (runPos) {
            runPos = 0;
            if ((unsigned int)(dbufCount + t) >= bd->dbufSize) return RETVAL_DATA_ERROR;
            uc = symToByte[mtfSymbol[0]];
            byteCount[uc] += t;
            while (t--) dbuf[dbufCount++] = uc;
        }

        if (nextSym > symTotal) break;

        if ((unsigned int)dbufCount >= bd->dbufSize) return RETVAL_DATA_ERROR;
        i = nextSym - 1;
        uc = mtfSymbol[i];
        std::memmove(mtfSymbol + 1, mtfSymbol, i);
        mtfSymbol[0] = uc;
        uc = symToByte[uc];

        byteCount[uc]++;
        dbuf[dbufCount++] = (unsigned int)uc;
    }

    if (origPtr >= (unsigned int)dbufCount) return RETVAL_DATA_ERROR;

    j = 0;
    for (i = 0; i < 256; i++) {
        k = j + byteCount[i];
        byteCount[i] = j;
        j = k;
    }

    for (i = 0; i < dbufCount; i++) {
        uc = (unsigned char)(dbuf[i] & 0xff);
        dbuf[byteCount[uc]] |= (i << 8);
        byteCount[uc]++;
    }

    bd->dataCRC = 0xffffffffL;
    if (dbufCount) {
        bd->writePos = dbuf[origPtr];
        bd->writeCurrent = (unsigned char)(bd->writePos & 0xff);
        bd->writePos >>= 8;
        bd->writeRun = -1;
        if (bd->blockRandomised) {
            if (bd->rNToGo == 0) {
                bd->rNToGo = BZ2_rNums[bd->rTPos++];
                if (bd->rTPos == 512) bd->rTPos = 0;
            }
            bd->rNToGo--;
            bd->writeCurrent ^= ((bd->rNToGo == 1) ? 1 : 0);
        }
    }
    bd->writeCount = dbufCount;

    return RETVAL_OK;
}

static void flush_bunzip_outbuf(bunzip_data *bd) {
    if (bd->outbufPos && bd->vec_out) {
        bd->vec_out->insert(bd->vec_out->end(), (uint8_t*)bd->outbuf, (uint8_t*)bd->outbuf + bd->outbufPos);
        bd->outbufPos = 0;
    }
}

static int decompress_blocks(bunzip_data *bd) {
    unsigned int *dbuf = bd->dbuf;
    int count, pos, current, run, copies, outbyte, previous;

    for (;;) {
        if (!bd->writeCount) {
            int i = get_next_block(bd);
            if (i) {
                if (i == RETVAL_LAST_BLOCK) return RETVAL_OK;
                return i;
            }
        }
        count = bd->writeCount;
        pos = bd->writePos;
        current = bd->writeCurrent;
        run = bd->writeRun;

        while (count) {
            count--;
            previous = current;
            pos = dbuf[pos];
            current = pos & 0xff;
            pos >>= 8;
            if (bd->blockRandomised) {
                if (bd->rNToGo == 0) {
                    bd->rNToGo = BZ2_rNums[bd->rTPos++];
                    if (bd->rTPos == 512) bd->rTPos = 0;
                }
                bd->rNToGo--;
                current ^= ((bd->rNToGo == 1) ? 1 : 0);
            }
            if (run++ == 3) {
                copies = current;
                outbyte = previous;
                current = -1;
            } else {
                copies = 1;
                outbyte = current;
            }
            while (copies--) {
                if (bd->outbufPos == IOBUF_SIZE) flush_bunzip_outbuf(bd);
                bd->outbuf[bd->outbufPos++] = outbyte;
                bd->dataCRC = (bd->dataCRC << 8) ^ bd->crc32Table[(bd->dataCRC >> 24) ^ (unsigned char)outbyte];
            }
            if (current != previous) run = 0;
        }
        bd->dataCRC = ~(bd->dataCRC);
        bd->totalCRC = ((bd->totalCRC << 1) | (bd->totalCRC >> 31)) ^ bd->dataCRC;
        bd->writeCount = 0;
    }
}

namespace gm14 {

bool bz2_decompress(const uint8_t* in_data, size_t in_len, std::vector<uint8_t>& out_data, size_t expected_len) {
    if (!in_data || in_len < 4) return false;
    bunzip_data bd;
    std::memset(&bd, 0, sizeof(bd));
    bd.inbuf = in_data;
    bd.inbufCount = (int)in_len;
    bd.vec_out = &out_data;
    if (expected_len > 0) out_data.reserve(expected_len);

    for (unsigned int i = 0; i < 256; i++) {
        unsigned int c = i << 24;
        for (int j = 8; j; j--)
            c = c & 0x80000000 ? (c << 1) ^ 0x04c11db7 : (c << 1);
        bd.crc32Table[i] = c;
    }

    int err = setjmp(bd.jmpbuf);
    if (err) {
        if (bd.dbuf) std::free(bd.dbuf);
        return false;
    }

    for (int i = 0; i < 3; i++) {
        if (get_bits(&bd, 8) != (unsigned int)"BZh"[i]) return false;
    }
    int blk = get_bits(&bd, 8);
    if (blk < '1' || blk > '9') return false;
    bd.dbufSize = 100000 * (blk - '0');
    bd.dbuf = (unsigned int *)std::malloc(bd.dbufSize * sizeof(unsigned int));
    if (!bd.dbuf) return false;

    int res = decompress_blocks(&bd);
    flush_bunzip_outbuf(&bd);
    std::free(bd.dbuf);

    return (res == RETVAL_OK);
}

} // namespace gm14
