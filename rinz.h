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

/* ═══════════════════════════════════════════════════════════════
 * 定数
 * ═══════════════════════════════════════════════════════════════*/

#define RINZ_OK             0
#define RINZ_ERROR         -1
#define RINZ_DATA_ERROR    -2
#define RINZ_BUF_ERROR     -3

#define RINZ_MAX_WBITS     15
#define RINZ_WINDOW_SIZE   (1 << RINZ_MAX_WBITS)  /* 32KB */

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
    while (bs->bit_count < n && bs->src_pos < bs->src_size) {
        bs->bit_buf |= (uint32_t)bs->src[bs->src_pos++] << bs->bit_count;
        bs->bit_count += 8;
    }
    return bs->bit_buf & ((1u << n) - 1);
}

static inline void rinz_bs_skip(RinzBitStream* bs, int n) {
    bs->bit_buf >>= n;
    bs->bit_count -= n;
}

static inline uint32_t rinz_bs_read(RinzBitStream* bs, int n) {
    uint32_t val = rinz_bs_peek(bs, n);
    rinz_bs_skip(bs, n);
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
    /* カウント初期化 */
    for (int i = 0; i < RINZ_MAX_BITS; i++) h->counts[i] = 0;
    
    /* 各ビット長のコード数をカウント */
    for (int i = 0; i < n; i++) {
        if (lens[i] > 0 && lens[i] < RINZ_MAX_BITS) {
            h->counts[lens[i]]++;
        }
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
        code = (code << 1) | rinz_bs_read(bs, 1);
        
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

static uint8_t g_rinz_fixed_lit_lens[288];
static uint8_t g_rinz_fixed_dist_lens[32];
static RinzHuffTable g_rinz_fixed_lit;
static RinzHuffTable g_rinz_fixed_dist;
static int g_rinz_fixed_init = 0;

static inline void rinz_init_fixed_tables(void) {
    if (g_rinz_fixed_init) return;
    
    /* リテラル/長さ (RFC 1951) */
    for (int i = 0; i < 144; i++) g_rinz_fixed_lit_lens[i] = 8;
    for (int i = 144; i < 256; i++) g_rinz_fixed_lit_lens[i] = 9;
    for (int i = 256; i < 280; i++) g_rinz_fixed_lit_lens[i] = 7;
    for (int i = 280; i < 288; i++) g_rinz_fixed_lit_lens[i] = 8;
    
    /* 距離 */
    for (int i = 0; i < 32; i++) g_rinz_fixed_dist_lens[i] = 5;
    
    rinz_huff_build(&g_rinz_fixed_lit, g_rinz_fixed_lit_lens, 288);
    rinz_huff_build(&g_rinz_fixed_dist, g_rinz_fixed_dist_lens, 32);
    
    g_rinz_fixed_init = 1;
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
} RinzOutput;

/* 非圧縮ブロック */
static inline int rinz_inflate_stored(RinzBitStream* bs, RinzOutput* out) {
    rinz_bs_align(bs);
    
    if (bs->src_pos + 4 > bs->src_size) return RINZ_DATA_ERROR;
    
    uint16_t len = bs->src[bs->src_pos] | (bs->src[bs->src_pos + 1] << 8);
    uint16_t nlen = bs->src[bs->src_pos + 2] | (bs->src[bs->src_pos + 3] << 8);
    bs->src_pos += 4;
    
    if ((uint16_t)~nlen != len) return RINZ_DATA_ERROR;
    
    if (bs->src_pos + len > bs->src_size) return RINZ_DATA_ERROR;
    if (out->out_pos + len > out->out_size) return RINZ_BUF_ERROR;
    
    for (uint16_t i = 0; i < len; i++) {
        out->out[out->out_pos++] = bs->src[bs->src_pos++];
    }
    
    return RINZ_OK;
}

/* 圧縮ブロック展開 */
static inline int rinz_inflate_block(RinzBitStream* bs, RinzOutput* out,
                                      RinzHuffTable* lit, RinzHuffTable* dist) {
    while (1) {
        int sym = rinz_huff_decode(lit, bs);
        if (sym < 0) return RINZ_DATA_ERROR;
        
        if (sym < 256) {
            /* リテラル */
            if (out->out_pos >= out->out_size) return RINZ_BUF_ERROR;
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
                length += rinz_bs_read(bs, g_rinz_len_extra[len_idx]);
            }
            
            int dist_sym = rinz_huff_decode(dist, bs);
            if (dist_sym < 0 || dist_sym >= 30) return RINZ_DATA_ERROR;
            
            int distance = g_rinz_dist_base[dist_sym];
            if (g_rinz_dist_extra[dist_sym] > 0) {
                distance += rinz_bs_read(bs, g_rinz_dist_extra[dist_sym]);
            }
            
            if (distance > (int)out->out_pos) return RINZ_DATA_ERROR;
            if (out->out_pos + length > out->out_size) return RINZ_BUF_ERROR;
            
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
    int hlit = rinz_bs_read(bs, 5) + 257;
    int hdist = rinz_bs_read(bs, 5) + 1;
    int hclen = rinz_bs_read(bs, 4) + 4;
    
    if (hlit > 286 || hdist > 30) return RINZ_DATA_ERROR;
    
    /* コード長のコード長を読む */
    uint8_t clen_lens[19] = {0};
    for (int i = 0; i < hclen; i++) {
        clen_lens[g_rinz_clen_order[i]] = rinz_bs_read(bs, 3);
    }
    
    RinzHuffTable clen_huff;
    rinz_huff_build(&clen_huff, clen_lens, 19);
    
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
            int rep = rinz_bs_read(bs, 2) + 3;
            uint8_t prev = (idx > 0) ? all_lens[idx - 1] : 0;
            while (rep-- > 0 && idx < total) {
                all_lens[idx++] = prev;
            }
        } else if (sym == 17) {
            /* 0を3-10回繰り返す */
            int rep = rinz_bs_read(bs, 3) + 3;
            while (rep-- > 0 && idx < total) {
                all_lens[idx++] = 0;
            }
        } else { /* sym == 18 */
            /* 0を11-138回繰り返す */
            int rep = rinz_bs_read(bs, 7) + 11;
            while (rep-- > 0 && idx < total) {
                all_lens[idx++] = 0;
            }
        }
    }
    
    rinz_huff_build(lit, all_lens, hlit);
    rinz_huff_build(dist, all_lens + hlit, hdist);
    
    return RINZ_OK;
}

/* ═══════════════════════════════════════════════════════════════
 * 公開API
 * ═══════════════════════════════════════════════════════════════*/

/*
 * raw deflate展開 (zlibヘッダーなし)
 */
static inline int rinz_inflate_raw(const uint8_t* src, size_t src_size,
                                    uint8_t* dst, size_t dst_size,
                                    size_t* out_size) {
    rinz_init_fixed_tables();
    
    RinzBitStream bs;
    rinz_bs_init(&bs, src, src_size);
    
    RinzOutput out;
    out.out = dst;
    out.out_size = dst_size;
    out.out_pos = 0;
    
    int final_block = 0;
    
    while (!final_block) {
        final_block = rinz_bs_read(&bs, 1);
        int btype = rinz_bs_read(&bs, 2);
        
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

/*
 * zlib形式展開 (2バイトヘッダー付き)
 */
static inline int rinz_inflate(const uint8_t* src, size_t src_size,
                                uint8_t* dst, size_t dst_size,
                                size_t* out_size) {
    if (src_size < 2) return RINZ_DATA_ERROR;
    
    /* zlibヘッダー確認 */
    uint8_t cmf = src[0];
    uint8_t flg = src[1];
    
    if ((cmf & 0x0F) != 8) return RINZ_DATA_ERROR;  /* deflate以外 */
    if ((cmf * 256 + flg) % 31 != 0) return RINZ_DATA_ERROR;  /* チェック失敗 */
    
    /* ヘッダーをスキップ */
    size_t offset = 2;
    if (flg & 0x20) {
        /* FDICT (プリセット辞書) - スキップ */
        offset += 4;
    }
    
    if (offset >= src_size) return RINZ_DATA_ERROR;
    
    return rinz_inflate_raw(src + offset, src_size - offset - 4, dst, dst_size, out_size);
}

/*
 * 展開後サイズ推定（PNG用）
 * 実際のサイズは展開しないとわからないが、目安を返す
 */
static inline size_t rinz_inflate_bound(size_t src_size) {
    /* 最悪ケース: 非圧縮で少し増える */
    return src_size * 1024;  /* 大きめに見積もる */
}

#endif /* RINZ_H */
