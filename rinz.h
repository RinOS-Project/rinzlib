/*
 * RinOS zlib ✿
 * 軽量deflate/inflate実装
 * 
 * RFC 1950 (zlib), RFC 1951 (deflate) 準拠
 */

#ifndef RINZ_H
#define RINZ_H

#include <stdint.h>
#include <stddef.h>

#include "rinz_checksum.h"

/* ═══════════════════════════════════════════════════════════════
 * 定数
 * ═══════════════════════════════════════════════════════════════*/

#define RINZ_OK             0
#define RINZ_ERROR         -1
#define RINZ_DATA_ERROR    -2
#define RINZ_BUF_ERROR     -3
#define RINZ_LIMIT_ERROR   -4

#define RINZ_MAX_WBITS     15
#define RINZ_WINDOW_SIZE   (1 << RINZ_MAX_WBITS)  /* 32KB */
#define RINZ_LIMIT_UNBOUNDED ((size_t)-1)

/* Explicit caller-owned resource policy for untrusted DEFLATE input.  The
 * input limit covers the bytes passed to the selected API (including the
 * zlib wrapper for rinz_inflate); output and block limits are enforced while
 * decoding. */
typedef struct {
    size_t max_input_bytes;
    size_t max_output_bytes;
    size_t max_blocks;
} RinzInflateLimits;

/* ═══════════════════════════════════════════════════════════════
 * ビットストリーム
 * ═══════════════════════════════════════════════════════════════*/

typedef struct {
    const uint8_t* src;
    size_t         src_size;
    size_t         src_pos;
    uint32_t       bit_buf;
    int            bit_count;
} RinzBitStream;

static inline void rinz_bs_init(RinzBitStream* bs, const uint8_t* data, size_t size) {
    bs->src = data;
    bs->src_size = size;
    bs->src_pos = 0;
    bs->bit_buf = 0;
    bs->bit_count = 0;
}

static inline int rinz_bs_eof(RinzBitStream* bs) {
    return bs->src_pos >= bs->src_size && bs->bit_count == 0;
}

static inline uint32_t rinz_bs_peek(RinzBitStream* bs, int n) {
    if (bs == NULL || n <= 0 || n > 24) return 0u;
    while (bs->bit_count < n && bs->src_pos < bs->src_size) {
        bs->bit_buf |= (uint32_t)bs->src[bs->src_pos++] << bs->bit_count;
        bs->bit_count += 8;
    }
    if (bs->bit_count < n) return 0u;
    return bs->bit_buf & ((1u << n) - 1);
}

static inline void rinz_bs_skip(RinzBitStream* bs, int n) {
    if (bs == NULL || n <= 0 || n > bs->bit_count) {
        if (bs != NULL) {
            bs->bit_buf = 0u;
            bs->bit_count = 0;
        }
        return;
    }
    bs->bit_buf >>= n;
    bs->bit_count -= n;
}

static inline int rinz_bs_read_checked(RinzBitStream* bs, int n,
                                       uint32_t* value) {
    if (bs == NULL || value == NULL || n <= 0 || n > 24) return 0;
    while (bs->bit_count < n && bs->src_pos < bs->src_size) {
        bs->bit_buf |= (uint32_t)bs->src[bs->src_pos++] << bs->bit_count;
        bs->bit_count += 8;
    }
    if (bs->bit_count < n) {
        *value = 0u;
        return 0;
    }
    *value = bs->bit_buf & ((1u << n) - 1u);
    bs->bit_buf >>= n;
    bs->bit_count -= n;
    return 1;
}

static inline uint32_t rinz_bs_read(RinzBitStream* bs, int n) {
    uint32_t val = 0u;
    (void)rinz_bs_read_checked(bs, n, &val);
    return val;
}

static inline void rinz_bs_align(RinzBitStream* bs) {
    bs->bit_buf = 0;
    bs->bit_count = 0;
}

/* ═══════════════════════════════════════════════════════════════
 * ハフマンテーブル
 * ═══════════════════════════════════════════════════════════════*/

#define RINZ_MAX_CODES     320
#define RINZ_MAX_BITS      16

typedef struct {
    uint16_t counts[RINZ_MAX_BITS];   /* 各ビット長のコード数 */
    uint16_t symbols[RINZ_MAX_CODES]; /* ソート済みシンボル */
    uint16_t first[RINZ_MAX_BITS];    /* 各ビット長の最初のコード値 */
    uint16_t index[RINZ_MAX_BITS];    /* symbols配列へのインデックス */
} RinzHuffTable;

/* ハフマンテーブル構築 */
static inline int rinz_huff_build(RinzHuffTable* h, const uint8_t* lens, int n) {
    if (h == NULL || lens == NULL || n < 0 || n > RINZ_MAX_CODES)
        return RINZ_ERROR;
    /* カウント初期化 */
    for (int i = 0; i < RINZ_MAX_BITS; i++) h->counts[i] = 0;
    
    /* 各ビット長のコード数をカウント */
    for (int i = 0; i < n; i++) {
        if (lens[i] >= RINZ_MAX_BITS) return RINZ_DATA_ERROR;
        if (lens[i] > 0) {
            h->counts[lens[i]]++;
        }
    }

    /* Reject an oversubscribed canonical tree.  Incomplete trees are
     * permitted by DEFLATE for the distance alphabet, but a code may never
     * consume more leaves than the prefix space provides. */
    int left = 1;
    for (int bits = 1; bits < RINZ_MAX_BITS; ++bits) {
        left = (left << 1) - h->counts[bits];
        if (left < 0) return RINZ_DATA_ERROR;
    }
    
    /* first[]とindex[]を計算 */
    uint16_t code = 0;
    uint16_t idx = 0;
    for (int bits = 1; bits < RINZ_MAX_BITS; bits++) {
        code = (code + h->counts[bits - 1]) << 1;
        h->first[bits] = code;
        h->index[bits] = idx;
        idx += h->counts[bits];
    }
    
    /* シンボルをソート */
    uint16_t offsets[RINZ_MAX_BITS];
    for (int i = 0; i < RINZ_MAX_BITS; i++) offsets[i] = h->index[i];
    
    for (int i = 0; i < n; i++) {
        if (lens[i] > 0 && lens[i] < RINZ_MAX_BITS) {
            h->symbols[offsets[lens[i]]++] = i;
        }
    }
    
    return RINZ_OK;
}

/* ハフマンデコード */
static inline int rinz_huff_decode(RinzHuffTable* h, RinzBitStream* bs) {
    uint32_t code = 0;

    for (int bits = 1; bits < RINZ_MAX_BITS; bits++) {
        uint32_t bit = 0u;
        if (!rinz_bs_read_checked(bs, 1, &bit)) return -1;
        code = (code << 1) | bit;
        
        if (h->counts[bits] > 0) {
            int first = h->first[bits];
            if ((int)code >= first && (int)code < first + h->counts[bits]) {
                return h->symbols[h->index[bits] + code - first];
            }
        }
    }
    
    return -1;  /* デコードエラー */
}

/* ═══════════════════════════════════════════════════════════════
 * 固定ハフマンテーブル
 * ═══════════════════════════════════════════════════════════════*/

static RinzHuffTable g_rinz_fixed_lit;
static RinzHuffTable g_rinz_fixed_dist;
/* 0 = uninitialized, 1 = one caller is initializing, 2 = ready. */
static int g_rinz_fixed_init_state = 0;

static inline void rinz_init_fixed_tables(void) {
    int expected = 0;
    if (__atomic_load_n(&g_rinz_fixed_init_state, __ATOMIC_ACQUIRE) == 2)
        return;
    if (!__atomic_compare_exchange_n(&g_rinz_fixed_init_state, &expected, 1,
                                    0, __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE)) {
        while (__atomic_load_n(&g_rinz_fixed_init_state, __ATOMIC_ACQUIRE) != 2) {
        }
        return;
    }

    uint8_t fixed_lit_lens[288];
    uint8_t fixed_dist_lens[32];
    
    /* リテラル/長さ (RFC 1951) */
    for (int i = 0; i < 144; i++) fixed_lit_lens[i] = 8;
    for (int i = 144; i < 256; i++) fixed_lit_lens[i] = 9;
    for (int i = 256; i < 280; i++) fixed_lit_lens[i] = 7;
    for (int i = 280; i < 288; i++) fixed_lit_lens[i] = 8;
    
    /* 距離 */
    for (int i = 0; i < 32; i++) fixed_dist_lens[i] = 5;
    
    rinz_huff_build(&g_rinz_fixed_lit, fixed_lit_lens, 288);
    rinz_huff_build(&g_rinz_fixed_dist, fixed_dist_lens, 32);
    
    __atomic_store_n(&g_rinz_fixed_init_state, 2, __ATOMIC_RELEASE);
}

/* ═══════════════════════════════════════════════════════════════
 * 長さ・距離テーブル
 * ═══════════════════════════════════════════════════════════════*/

static const uint16_t g_rinz_len_base[29] = {
    3,4,5,6,7,8,9,10,11,13,15,17,19,23,27,31,
    35,43,51,59,67,83,99,115,131,163,195,227,258
};

static const uint8_t g_rinz_len_extra[29] = {
    0,0,0,0,0,0,0,0,1,1,1,1,2,2,2,2,
    3,3,3,3,4,4,4,4,5,5,5,5,0
};

static const uint16_t g_rinz_dist_base[30] = {
    1,2,3,4,5,7,9,13,17,25,33,49,65,97,129,193,
    257,385,513,769,1025,1537,2049,3073,4097,6145,
    8193,12289,16385,24577
};

static const uint8_t g_rinz_dist_extra[30] = {
    0,0,0,0,1,1,2,2,3,3,4,4,5,5,6,6,
    7,7,8,8,9,9,10,10,11,11,12,12,13,13
};

/* コード長のコード順序 */
static const uint8_t g_rinz_clen_order[19] = {
    16,17,18,0,8,7,9,6,10,5,11,4,12,3,13,2,14,1,15
};

/* ═══════════════════════════════════════════════════════════════
 * Inflate (展開)
 * ═══════════════════════════════════════════════════════════════*/

typedef struct {
    uint8_t* out;
    size_t   out_size;
    size_t   out_pos;
    size_t   max_output_size;
} RinzOutput;

static inline int rinz_output_reserve(RinzOutput* out, size_t amount) {
    if (out == NULL || out->out_pos > out->out_size ||
        amount > out->out_size - out->out_pos) return RINZ_BUF_ERROR;
    if (out->out_pos > out->max_output_size ||
        amount > out->max_output_size - out->out_pos) return RINZ_LIMIT_ERROR;
    if (amount != 0u && out->out == NULL) return RINZ_BUF_ERROR;
    return RINZ_OK;
}

/* 非圧縮ブロック */
static inline int rinz_inflate_stored(RinzBitStream* bs, RinzOutput* out) {
    int reserve_result;
    if (bs == NULL || out == NULL || bs->src_pos > bs->src_size ||
        bs->src_size - bs->src_pos < 4u) return RINZ_DATA_ERROR;
    rinz_bs_align(bs);
    
    uint16_t len = bs->src[bs->src_pos] | (bs->src[bs->src_pos + 1] << 8);
    uint16_t nlen = bs->src[bs->src_pos + 2] | (bs->src[bs->src_pos + 3] << 8);
    bs->src_pos += 4;
    
    if ((uint16_t)~nlen != len) return RINZ_DATA_ERROR;
    
    if ((size_t)len > bs->src_size - bs->src_pos)
        return RINZ_DATA_ERROR;
    reserve_result = rinz_output_reserve(out, (size_t)len);
    if (reserve_result != RINZ_OK) return reserve_result;
    
    for (uint16_t i = 0; i < len; i++) {
        out->out[out->out_pos++] = bs->src[bs->src_pos++];
    }
    
    return RINZ_OK;
}

/* 圧縮ブロック展開 */
static inline int rinz_inflate_block(RinzBitStream* bs, RinzOutput* out,
                                      RinzHuffTable* lit, RinzHuffTable* dist) {
    int reserve_result;
    while (1) {
        int sym = rinz_huff_decode(lit, bs);
        if (sym < 0) return RINZ_DATA_ERROR;
        
        if (sym < 256) {
            /* リテラル */
            reserve_result = rinz_output_reserve(out, 1u);
            if (reserve_result != RINZ_OK) return reserve_result;
            out->out[out->out_pos++] = (uint8_t)sym;
        } else if (sym == 256) {
            /* ブロック終端 */
            break;
        } else {
            /* 長さ・距離ペア */
            int len_idx = sym - 257;
            if (len_idx < 0 || len_idx >= 29) return RINZ_DATA_ERROR;
            
            int length = g_rinz_len_base[len_idx];
            if (g_rinz_len_extra[len_idx] > 0) {
                uint32_t extra = 0u;
                if (!rinz_bs_read_checked(bs, g_rinz_len_extra[len_idx],
                                          &extra)) return RINZ_DATA_ERROR;
                length += (int)extra;
            }
            
            int dist_sym = rinz_huff_decode(dist, bs);
            if (dist_sym < 0 || dist_sym >= 30) return RINZ_DATA_ERROR;
            
            int distance = g_rinz_dist_base[dist_sym];
            if (g_rinz_dist_extra[dist_sym] > 0) {
                uint32_t extra = 0u;
                if (!rinz_bs_read_checked(bs, g_rinz_dist_extra[dist_sym],
                                          &extra)) return RINZ_DATA_ERROR;
                distance += (int)extra;
            }
            
            if (distance > (int)out->out_pos) return RINZ_DATA_ERROR;
            reserve_result = rinz_output_reserve(out, (size_t)length);
            if (reserve_result != RINZ_OK) return reserve_result;
            
            /* バックリファレンスコピー */
            for (int i = 0; i < length; i++) {
                out->out[out->out_pos] = out->out[out->out_pos - distance];
                out->out_pos++;
            }
        }
    }
    
    return RINZ_OK;
}

/* 動的ハフマンテーブル読み込み */
static inline int rinz_inflate_dynamic(RinzBitStream* bs, 
                                        RinzHuffTable* lit, RinzHuffTable* dist) {
    uint32_t value = 0u;
    int hlit;
    int hdist;
    int hclen;
    if (!rinz_bs_read_checked(bs, 5, &value)) return RINZ_DATA_ERROR;
    hlit = (int)value + 257;
    if (!rinz_bs_read_checked(bs, 5, &value)) return RINZ_DATA_ERROR;
    hdist = (int)value + 1;
    if (!rinz_bs_read_checked(bs, 4, &value)) return RINZ_DATA_ERROR;
    hclen = (int)value + 4;

    if (hlit > 286 || hdist > 32) return RINZ_DATA_ERROR;
    
    /* コード長のコード長を読む */
    uint8_t clen_lens[19] = {0};
    for (int i = 0; i < hclen; i++) {
        if (!rinz_bs_read_checked(bs, 3, &value)) return RINZ_DATA_ERROR;
        clen_lens[g_rinz_clen_order[i]] = (uint8_t)value;
    }
    
    RinzHuffTable clen_huff;
    if (rinz_huff_build(&clen_huff, clen_lens, 19) != RINZ_OK)
        return RINZ_DATA_ERROR;
    
    /* リテラル/長さ + 距離のコード長を読む */
    uint8_t all_lens[320] = {0};
    int idx = 0;
    int total = hlit + hdist;
    
    while (idx < total) {
        int sym = rinz_huff_decode(&clen_huff, bs);
        if (sym < 0) return RINZ_DATA_ERROR;
        
        if (sym < 16) {
            all_lens[idx++] = sym;
        } else if (sym == 16) {
            /* 直前の値を3-6回繰り返す */
            int rep;
            uint8_t prev;
            if (idx == 0 || !rinz_bs_read_checked(bs, 2, &value))
                return RINZ_DATA_ERROR;
            rep = (int)value + 3;
            prev = all_lens[idx - 1];
            if (rep > total - idx) return RINZ_DATA_ERROR;
            while (rep-- > 0) {
                all_lens[idx++] = prev;
            }
        } else if (sym == 17) {
            /* 0を3-10回繰り返す */
            int rep;
            if (!rinz_bs_read_checked(bs, 3, &value)) return RINZ_DATA_ERROR;
            rep = (int)value + 3;
            if (rep > total - idx) return RINZ_DATA_ERROR;
            while (rep-- > 0) {
                all_lens[idx++] = 0;
            }
        } else if (sym == 18) {
            /* 0を11-138回繰り返す */
            int rep;
            if (!rinz_bs_read_checked(bs, 7, &value)) return RINZ_DATA_ERROR;
            rep = (int)value + 11;
            if (rep > total - idx) return RINZ_DATA_ERROR;
            while (rep-- > 0) {
                all_lens[idx++] = 0;
            }
        } else {
            return RINZ_DATA_ERROR;
        }
    }
    
    if (rinz_huff_build(lit, all_lens, hlit) != RINZ_OK ||
        rinz_huff_build(dist, all_lens + hlit, hdist) != RINZ_OK)
        return RINZ_DATA_ERROR;
    
    return RINZ_OK;
}

/* ═══════════════════════════════════════════════════════════════
 * 公開API
 * ═══════════════════════════════════════════════════════════════*/

/*
 * raw deflate展開 (zlibヘッダーなし)
 */
static inline void rinz_clear_output(uint8_t* dst, size_t dst_size) {
    size_t index;
    if (dst == NULL) return;
    for (index = 0u; index < dst_size; ++index) dst[index] = 0u;
}

static inline int rinz_inflate_raw_impl(const uint8_t* src, size_t src_size,
                                         uint8_t* dst, size_t dst_size,
                                         size_t* out_size,
                                         const RinzInflateLimits* limits) {
    if ((src == NULL && src_size != 0u) ||
        (dst == NULL && dst_size != 0u) || limits == NULL) return RINZ_ERROR;
    if (src_size > limits->max_input_bytes) return RINZ_LIMIT_ERROR;
    rinz_init_fixed_tables();
    
    RinzBitStream bs;
    rinz_bs_init(&bs, src, src_size);
    
    RinzOutput out;
    out.out = dst;
    out.out_size = dst_size;
    out.out_pos = 0;
    out.max_output_size = limits->max_output_bytes;
    
    int final_block = 0;
    size_t block_count = 0u;
    
    while (!final_block) {
        uint32_t header = 0u;
        uint32_t block_type = 0u;
        if (limits->max_blocks != RINZ_LIMIT_UNBOUNDED &&
            block_count >= limits->max_blocks) return RINZ_LIMIT_ERROR;
        ++block_count;
        if (!rinz_bs_read_checked(&bs, 1, &header) ||
            !rinz_bs_read_checked(&bs, 2, &block_type)) return RINZ_DATA_ERROR;
        final_block = (int)header;
        int btype = (int)block_type;
        
        int ret;
        
        if (btype == 0) {
            /* 非圧縮 */
            ret = rinz_inflate_stored(&bs, &out);
        } else if (btype == 1) {
            /* 固定ハフマン */
            ret = rinz_inflate_block(&bs, &out, &g_rinz_fixed_lit, &g_rinz_fixed_dist);
        } else if (btype == 2) {
            /* 動的ハフマン */
            RinzHuffTable lit, dist;
            ret = rinz_inflate_dynamic(&bs, &lit, &dist);
            if (ret != RINZ_OK) return ret;
            ret = rinz_inflate_block(&bs, &out, &lit, &dist);
        } else {
            return RINZ_DATA_ERROR;  /* 無効なブロックタイプ */
        }
        
        if (ret != RINZ_OK) return ret;
    }
    
    if (out_size) *out_size = out.out_pos;
    return RINZ_OK;
}

static inline int rinz_inflate_raw_limited(const uint8_t* src, size_t src_size,
                                           uint8_t* dst, size_t dst_size,
                                           size_t* out_size,
                                           const RinzInflateLimits* limits) {
    int result;
    if (out_size != NULL) *out_size = 0u;
    if ((src == NULL && src_size != 0u) ||
        (dst == NULL && dst_size != 0u) || limits == NULL) {
        rinz_clear_output(dst, dst_size);
        return RINZ_ERROR;
    }
    result = rinz_inflate_raw_impl(src, src_size, dst, dst_size, out_size,
                                   limits);
    if (result != RINZ_OK) {
        if (out_size != NULL) *out_size = 0u;
        rinz_clear_output(dst, dst_size);
    }
    return result;
}

static inline int rinz_inflate_raw(const uint8_t* src, size_t src_size,
                                    uint8_t* dst, size_t dst_size,
                                    size_t* out_size) {
    const RinzInflateLimits limits = {
        RINZ_LIMIT_UNBOUNDED,
        RINZ_LIMIT_UNBOUNDED,
        RINZ_LIMIT_UNBOUNDED
    };
    return rinz_inflate_raw_limited(src, src_size, dst, dst_size, out_size,
                                    &limits);
}

/*
 * zlib形式展開 (2バイトヘッダー付き)
 */
static inline int rinz_inflate_limited(const uint8_t* src, size_t src_size,
                                       uint8_t* dst, size_t dst_size,
                                       size_t* out_size,
                                       const RinzInflateLimits* limits) {
    size_t offset = 2u;
    size_t payload_size;
    size_t decoded_size = 0u;
    uint32_t expected_adler;
    int result;
    if (out_size != NULL) *out_size = 0u;
    if (src == NULL || src_size < 6u ||
        (dst == NULL && dst_size != 0u) || limits == NULL) {
        rinz_clear_output(dst, dst_size);
        return RINZ_ERROR;
    }
    if (src_size > limits->max_input_bytes) {
        rinz_clear_output(dst, dst_size);
        return RINZ_LIMIT_ERROR;
    }
    
    /* zlibヘッダー確認 */
    uint8_t cmf = src[0];
    uint8_t flg = src[1];
    
    if ((cmf & 0x0F) != 8) {
        rinz_clear_output(dst, dst_size);
        return RINZ_DATA_ERROR;
    }  /* deflate以外 */
    if ((cmf * 256 + flg) % 31 != 0) {
        rinz_clear_output(dst, dst_size);
        return RINZ_DATA_ERROR;
    }  /* チェック失敗 */
    
    /* ヘッダーをスキップ */
    if (flg & 0x20) {
        /* A preset dictionary is not part of this allocation-free API. */
        rinz_clear_output(dst, dst_size);
        return RINZ_DATA_ERROR;
    }
    if (src_size - offset < 4u) {
        rinz_clear_output(dst, dst_size);
        return RINZ_DATA_ERROR;
    }
    payload_size = src_size - offset - 4u;
    result = rinz_inflate_raw_impl(src + offset, payload_size, dst, dst_size,
                                   &decoded_size, limits);
    if (result != RINZ_OK) {
        if (out_size != NULL) *out_size = 0u;
        rinz_clear_output(dst, dst_size);
        return result;
    }
    expected_adler = ((uint32_t)src[src_size - 4u] << 24u) |
                     ((uint32_t)src[src_size - 3u] << 16u) |
                     ((uint32_t)src[src_size - 2u] << 8u) |
                     (uint32_t)src[src_size - 1u];
    if (rinz_adler32(dst, decoded_size) != expected_adler) {
        rinz_clear_output(dst, dst_size);
        return RINZ_DATA_ERROR;
    }
    if (out_size != NULL) *out_size = decoded_size;
    return RINZ_OK;
}

static inline int rinz_inflate(const uint8_t* src, size_t src_size,
                                uint8_t* dst, size_t dst_size,
                                size_t* out_size) {
    const RinzInflateLimits limits = {
        RINZ_LIMIT_UNBOUNDED,
        RINZ_LIMIT_UNBOUNDED,
        RINZ_LIMIT_UNBOUNDED
    };
    return rinz_inflate_limited(src, src_size, dst, dst_size, out_size,
                                &limits);
}

/*
 * 展開後サイズ推定（PNG用）
 * 実際のサイズは展開しないとわからないが、目安を返す
 */
static inline size_t rinz_inflate_bound(size_t src_size) {
    /* 最悪ケース: 非圧縮で少し増える */
    return src_size > (size_t)-1 / 1024u ? (size_t)-1 : src_size * 1024u;
}

#endif /* RINZ_H */
