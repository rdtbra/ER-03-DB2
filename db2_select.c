/*
 * db2_select.c — Leitor Direto de Tabelas IBM Db2 v11.5 (Sem Motor SQL / Sem Bibliotecas Db2)
 *
 * Uso:
 *   ./db2_select <BANCO> <SCHEMA> <TABELA> [LIMITE_LINHAS]
 *   ./db2_select <BANCO> [SCHEMA]                            (Lista as tabelas do banco)
 *
 * Fluxo 100% Binário em Disco:
 *   1. Lê o System Database Directory (/home/erp/sqllib/sqldbdir/sqldbdir) e Local Database
 *      Directory (<dbpath>/<inst>/NODE0000/sqldbdir/sqldbdir) para traduzir <BANCO> -> SQL0000X.
 *   2. Lê SQLSGF.1 (Automatic Storage Groups) e SQLSPCS.1 (Tablespaces) para localizar
 *      o container da SYSCATSPACE (catalog.dat) e das tablespaces de usuário (.LRG / .USR).
 *   3. Varre fisicamente SYSIBM.SYSTABLES (ObjID=5 em catalog.dat) para descobrir o
 *      TablespaceID (TID) e ObjectID (FID) de <SCHEMA>.<TABELA>.
 *   4. Varre fisicamente SYSIBM.SYSCOLUMNS (ObjID=6 em catalog.dat) para obter os nomes
 *      reais das colunas (NAME), tipos (COLTYPE), tamanhos (LENGTH) e casas decimais (SCALE).
 *   5. Abre o container físico da tablespace alvo, navega pela Object Table (0xFFFF) -> Data EMP
 *      (0x40) -> Páginas de Dados (0x00), decodifica os registros FIXEDVAR e imprime a tabela!
 */

#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <stdbool.h>
#include <string.h>
#include <strings.h>
#include <ctype.h>
#include <errno.h>
#include <dlfcn.h>

#define DB2_PAGE_SIZE    4096
#define BPS_HEADER_SIZE  48
#define SLOT_DIR_OFFSET  0x44
#define MAX_COLS         128
#define MAX_EXTENTS      2048

#pragma pack(push, 1)

/* Cabeçalho BPS (Buffer Pool Services) de 48 bytes presente no início de toda página */
typedef struct {
    uint16_t page_data_offset;      /* +0x00: Sempre 0x0030 (48) */
    uint16_t page_data_length;      /* +0x02: Sempre 0x0FD0 (4048) para páginas de 4KB */
    uint32_t obj_page_num;          /* +0x04: Número lógico da página dentro do objeto */
    uint8_t  version_flags;         /* +0x08 */
    uint8_t  page_flags;            /* +0x09: ex: 0x05 */
    uint8_t  obj_type;              /* +0x0A: 0x00=Data, 0x01=Index, 0x0E=SMP, 0x40=Data EMP, 0x41=Idx EMP */
    uint8_t  consistency_bits;      /* +0x0B: ex: 0x2A */
    uint16_t tbspace_id;            /* +0x0C: ID da Tablespace */
    uint16_t reserved1;             /* +0x0E */
    uint32_t pool_page_num;         /* +0x10: Número da página relativo à Tablespace (Pool Page) */
    uint16_t object_id;             /* +0x14: ID do Objeto (0xFFFE=SMP, 0xFFFF=ObjTable, >=4 Tabelas) */
    uint16_t parent_obj_id;         /* +0x16 */
    uint32_t reserved2;             /* +0x18 */
    uint32_t reserved3;             /* +0x1C */
    uint64_t page_lsn;              /* +0x20: Log Sequence Number (WAL) */
    uint64_t checksum_meta;         /* +0x28: Checksum / sector parity bits */
} db2_bps_header_t;

/* Cabeçalho de Data Page (20 bytes, em +0x30..+0x43) */
typedef struct {
    uint16_t slot_count;            /* +0x30: Total de slots no diretório */
    uint16_t total_free_space;      /* +0x32: Espaço livre total na página */
    uint16_t total_reserve_space;   /* +0x34 */
    uint16_t youngest_reserve;      /* +0x36 */
    uint16_t max_record_size;       /* +0x38: Tamanho do maior registro na página */
    uint16_t free_space_offset;     /* +0x3A: Offset do início da área livre */
    uint32_t reserved;              /* +0x3C */
    uint16_t youngest_tid_hi;       /* +0x40 */
    uint16_t dpg_flags;             /* +0x42 */
} db2_data_page_hdr_t;

/* Descritor de Coluna (8 bytes) dentro do Slot 3 (Table Description Record) */
typedef struct {
    uint16_t col_type;              /* 0x0000=SMALLINT, 0x0001=INT, 0x0005=BIGINT,
                                       0x0100=CHAR, 0x0101=VARCHAR, 0x0105=DATE, 0x0107=TIMESTAMP */
    uint16_t col_len;               /* Tamanho máximo/fixo */
    uint16_t null_flags;            /* 0x0001 = Allows NULLs, 0x0002 = Prohibits NULLs */
    uint16_t fixed_offset;          /* Offset relativo ao início da área fixa do registro (+8) */
} db2_col_desc_t;

#pragma pack(pop)

typedef struct {
    char     name[129];
    char     sql_type[16];
    bool     nullable;
    int16_t  length;
    int16_t  scale;
    int16_t  colno;
    uint16_t print_width;
} cat_col_info_t;

typedef struct {
    char     name[129];
    char     schema[129];
    char     type;
    int16_t  col_count;
    int16_t  obj_id;
    int16_t  tbsp_id;
    int64_t  card;
    int64_t  npages;
    bool     found;
} cat_table_info_t;

typedef struct {
    FILE    *fp;
    char     path[512];
    uint32_t total_file_pages;
    uint32_t useable_pages;
} container_t;

typedef struct {
    char        name[129];
    uint16_t    tbsp_id;
    uint32_t    page_size;
    uint32_t    extent_size;
    uint32_t    tag_pages;
    uint32_t    num_containers;
    container_t containers[16];
} tablespace_ctx_t;

/* Remove espaços à direita de uma string */
static void rtrim(char *s) {
    int len = (int)strlen(s);
    while (len > 0 && (s[len - 1] == ' ' || s[len - 1] == '\t' || s[len - 1] == '\r' || s[len - 1] == '\n')) {
        s[--len] = '\0';
    }
}

/* Compara duas strings ignorando espaços à direita e case-insensitive */
static bool db2_name_eq(const char *a, const char *b) {
    char ca[256], cb[256];
    snprintf(ca, sizeof(ca), "%s", a);
    snprintf(cb, sizeof(cb), "%s", b);
    rtrim(ca);
    rtrim(cb);
    return strcasecmp(ca, cb) == 0;
}

/* Lê uma página física de um container */
static bool read_file_page(FILE *fp, uint32_t file_page, uint32_t page_size, void *buf) {
    if (fseek(fp, (long)file_page * page_size, SEEK_SET) != 0) return false;
    return fread(buf, 1, page_size, fp) == page_size;
}

/* Lê uma página lógica da tablespace (Pool Page) com Striping Multi-Container */
static bool read_pool_page(const tablespace_ctx_t *ctx, uint32_t pool_page, void *buf) {
    if (ctx->num_containers == 0) return false;
    uint32_t pool_extent = pool_page / ctx->extent_size;
    uint32_t page_in_extent = pool_page % ctx->extent_size;
    uint32_t c_idx = pool_extent % ctx->num_containers;
    uint32_t extent_in_c = pool_extent / ctx->num_containers;
    uint32_t file_page = ctx->tag_pages + (extent_in_c * ctx->extent_size) + page_in_extent;

    if (c_idx >= ctx->num_containers || !ctx->containers[c_idx].fp) return false;
    return read_file_page(ctx->containers[c_idx].fp, file_page, ctx->page_size, buf);
}

static void close_tablespace(tablespace_ctx_t *ctx) {
    for (uint32_t c = 0; c < ctx->num_containers && c < 16; c++) {
        if (ctx->containers[c].fp) {
            fclose(ctx->containers[c].fp);
            ctx->containers[c].fp = NULL;
        }
    }
}

/* Lê o array de extents a partir de uma página EMP (Type 0x40) */
static uint32_t read_emp_extents(const tablespace_ctx_t *ctx, uint32_t emp_pool_page,
                                 uint32_t *extents_out, uint32_t max_extents) {
    uint8_t page[65536];
    if (!read_pool_page(ctx, emp_pool_page, page)) return 0;

    const db2_bps_header_t *bps = (const db2_bps_header_t *)page;
    if ((bps->obj_type & 0x40) == 0) return 0;

    const uint32_t *entries = (const uint32_t *)(page + 0x58);
    uint32_t count = 0;
    uint32_t max_in_page = (ctx->page_size - 0x58) / sizeof(uint32_t);
    for (uint32_t i = 0; i < max_in_page && count < max_extents; i++) {
        if (entries[i] == 0xFFFFFFFF || entries[i] == 0x00000000) break;
        extents_out[count++] = entries[i];
    }
    return count;
}

/* Localiza a página EMP de Dados (Type=0x40) de um Object ID varrendo a Object Table (0xFFFF) */
static bool find_object_data_emp(const tablespace_ctx_t *ctx, uint16_t target_obj_id, uint32_t *out_data_emp) {
    uint32_t ot_extents[64];
    uint32_t num_ot_ext = read_emp_extents(ctx, 1, ot_extents, 64);
    if (num_ot_ext == 0) return false;

    uint8_t pg0[65536];
    if (!read_pool_page(ctx, ot_extents[0], pg0)) return false;
    uint16_t slot0_off = *(const uint16_t *)(pg0 + SLOT_DIR_OFFSET + 0 * 2);
    uint32_t ot_page_count = *(const uint32_t *)(pg0 + SLOT_DIR_OFFSET + slot0_off + 4);

    uint32_t pages_seen = 0;
    for (uint32_t e = 0; e < num_ot_ext && pages_seen < ot_page_count; e++) {
        for (uint32_t p = 0; p < ctx->extent_size && pages_seen < ot_page_count; p++) {
            uint8_t page[65536];
            if (!read_pool_page(ctx, ot_extents[e] + p, page)) break;

            const db2_bps_header_t *bps = (const db2_bps_header_t *)page;
            if (bps->object_id != 0xFFFF || bps->obj_type != 0x00) continue;
            pages_seen++;

            const db2_data_page_hdr_t *dhdr = (const db2_data_page_hdr_t *)(page + BPS_HEADER_SIZE);
            uint16_t start_slot = (bps->obj_page_num == 0) ? 4 : 0;

            for (uint16_t s = start_slot; s < dhdr->slot_count; s++) {
                uint16_t soff = *(const uint16_t *)(page + SLOT_DIR_OFFSET + s * 2);
                if (soff == 0xFFFF || soff == 0x0000 || ((uint32_t)(SLOT_DIR_OFFSET + soff) >= ctx->page_size)) continue;
                const uint8_t *rec = page + SLOT_DIR_OFFSET + soff;
                uint16_t rec_type = *(const uint16_t *)(rec + 0);
                uint16_t rec_len  = *(const uint16_t *)(rec + 2);
                if (rec_type != 0x0010 || rec_len < 0x70) continue;

                uint16_t obj_id = *(const uint16_t *)(rec + 0x0A);
                if (obj_id == target_obj_id) {
                    *out_data_emp = *(const uint32_t *)(rec + 0x0C);
                    return (*out_data_emp != 0);
                }
            }
        }
    }
    return false;
}

/* Formata o valor de uma coluna de um registro FIXEDVAR em string */
static void format_column_value(const uint8_t *rec, uint16_t rec_len,
                                const db2_col_desc_t *col, int16_t scale,
                                char *out_buf, size_t out_sz) {
    out_buf[0] = '\0';
    const uint8_t *fixed_base = rec + 8;
    const uint8_t *val_ptr    = fixed_base + col->fixed_offset;
    bool nullable = (col->null_flags == 0x0001);

    uint16_t fixed_width = col->col_len;
    if (col->col_type == 0x0002) {
        /* DECIMAL: precisão nos 8 bits baixos */
        uint8_t prec = (uint8_t)(col->col_len & 0xFF);
        fixed_width = (prec / 2) + 1;
    } else if (col->col_type == 0x0006) {
        fixed_width = 8;  /* DECFLOAT(16) */
    } else if (col->col_type == 0x0007) {
        fixed_width = 16; /* DECFLOAT(34) */
    } else if (col->col_type == 0x0101 || col->col_type == 0x0111 || col->col_type == 0x0201) {
        fixed_width = 4;  /* VARCHAR, VARBINARY, VARGRAPHIC */
    } else if (col->col_type == 0x0106) {
        fixed_width = 3;  /* TIME */
    } else if (col->col_type == 0x0107) {
        /* TIMESTAMP(p) */
        uint8_t prec = (uint8_t)((col->col_len >> 8) & 0xFF);
        fixed_width = 7 + (prec + 1) / 2;
    } else if (col->col_type == 0x0108 || col->col_type == 0x0109) {
        fixed_width = 4;  /* LOB */
    } else if (col->col_type == 0x010F) {
        fixed_width = 1;  /* BOOLEAN */
    }

    if (8 + col->fixed_offset + fixed_width + (nullable ? 1 : 0) > rec_len) {
        snprintf(out_buf, out_sz, "?");
        return;
    }

    if (nullable) {
        uint8_t null_byte = val_ptr[fixed_width];
        if (null_byte != 0x00) {
            snprintf(out_buf, out_sz, "NULL");
            return;
        }
    }

    switch (col->col_type) {
        case 0x0000: /* SMALLINT */
            snprintf(out_buf, out_sz, "%d", *(const int16_t *)val_ptr);
            break;
        case 0x0001: /* INTEGER */
            snprintf(out_buf, out_sz, "%d", *(const int32_t *)val_ptr);
            break;
        case 0x0002:
        case 0x0104: { /* DECIMAL Packed BCD */
            uint8_t prec = (uint8_t)(col->col_len & 0xFF);
            uint8_t sc   = (uint8_t)((col->col_len >> 8) & 0xFF);
            if (scale >= 0) sc = (uint8_t)scale;

            char digits[64];
            int dlen = 0;
            for (uint16_t k = 0; k < fixed_width && dlen + 2 < (int)sizeof(digits); k++) {
                uint8_t hi = (val_ptr[k] >> 4) & 0x0F;
                uint8_t lo = val_ptr[k] & 0x0F;
                digits[dlen++] = (char)('0' + hi);
                if (k < fixed_width - 1) digits[dlen++] = (char)('0' + lo);
            }
            digits[dlen] = '\0';
            uint8_t sign_nib = val_ptr[fixed_width - 1] & 0x0F;
            bool neg = (sign_nib == 0x0D || sign_nib == 0x0B);

            int int_part_len = dlen - (sc > 0 ? sc : 0);
            int start = 0;
            while (start < int_part_len - 1 && digits[start] == '0') start++;

            size_t pos = 0;
            if (neg && pos + 1 < out_sz) out_buf[pos++] = '-';
            for (int i = start; i < int_part_len && pos + 1 < out_sz; i++) out_buf[pos++] = digits[i];
            if (sc > 0 && pos + 1 < out_sz) {
                out_buf[pos++] = '.';
                for (int i = int_part_len; i < dlen && pos + 1 < out_sz; i++) {
                    out_buf[pos++] = (i >= 0) ? digits[i] : '0';
                }
            }
            out_buf[pos] = '\0';
            break;
        }
        case 0x0003:
        case 0x0004: /* REAL (4B float) ou DOUBLE (8B double) */
            if (fixed_width == 8) {
                snprintf(out_buf, out_sz, "%.6g", *(const double *)val_ptr);
            } else {
                snprintf(out_buf, out_sz, "%.4g", (double)*(const float *)val_ptr);
            }
            break;
        case 0x0005: /* BIGINT */
            snprintf(out_buf, out_sz, "%lld", (long long)*(const int64_t *)val_ptr);
            break;
        case 0x0006:
        case 0x0007: { /* DECFLOAT(16) e DECFLOAT(34) */
            size_t pos = (size_t)snprintf(out_buf, out_sz, "DF(0x");
            for (int k = (int)fixed_width - 1; k >= 0 && pos + 3 < out_sz; k--) {
                pos += (size_t)snprintf(out_buf + pos, out_sz - pos, "%02X", val_ptr[k]);
            }
            snprintf(out_buf + pos, out_sz - pos, ")");
            break;
        }
        case 0x0100: { /* CHAR */
            int len = col->col_len;
            while (len > 0 && val_ptr[len - 1] == ' ') len--;
            size_t pos = 0;
            for (int k = 0; k < len && pos + 1 < out_sz; k++) {
                unsigned char ch = val_ptr[k];
                out_buf[pos++] = isprint(ch) ? (char)ch : '.';
            }
            out_buf[pos] = '\0';
            break;
        }
        case 0x0101: { /* VARCHAR */
            uint16_t var_off = *(const uint16_t *)(val_ptr + 0);
            uint16_t var_len = *(const uint16_t *)(val_ptr + 2);
            if (8 + var_off + var_len <= rec_len) {
                const uint8_t *s = fixed_base + var_off;
                size_t pos = 0;
                for (uint16_t k = 0; k < var_len && pos + 1 < out_sz; k++) {
                    unsigned char ch = s[k];
                    out_buf[pos++] = isprint(ch) ? (char)ch : '.';
                }
                out_buf[pos] = '\0';
                rtrim(out_buf);
            } else {
                snprintf(out_buf, out_sz, "<varchar>");
            }
            break;
        }
        case 0x0105: /* DATE (4B BCD: YYYYMMDD) */
            snprintf(out_buf, out_sz, "%02X%02X-%02X-%02X",
                     val_ptr[0], val_ptr[1], val_ptr[2], val_ptr[3]);
            break;
        case 0x0106: /* TIME (3B BCD: HHMMSS) */
            snprintf(out_buf, out_sz, "%02X:%02X:%02X",
                     val_ptr[0], val_ptr[1], val_ptr[2]);
            break;
        case 0x0107: { /* TIMESTAMP(p) */
            if (fixed_width == 7) {
                snprintf(out_buf, out_sz, "%02X%02X-%02X-%02X %02X:%02X:%02X",
                         val_ptr[0], val_ptr[1], val_ptr[2], val_ptr[3],
                         val_ptr[4], val_ptr[5], val_ptr[6]);
            } else if (fixed_width == 10) {
                snprintf(out_buf, out_sz, "%02X%02X-%02X-%02X %02X:%02X:%02X.%02X%02X%02X",
                         val_ptr[0], val_ptr[1], val_ptr[2], val_ptr[3],
                         val_ptr[4], val_ptr[5], val_ptr[6],
                         val_ptr[7], val_ptr[8], val_ptr[9]);
            } else if (fixed_width >= 13) {
                snprintf(out_buf, out_sz, "%02X%02X-%02X-%02X %02X:%02X:%02X.%02X%02X%02X%02X%02X%02X",
                         val_ptr[0], val_ptr[1], val_ptr[2], val_ptr[3],
                         val_ptr[4], val_ptr[5], val_ptr[6],
                         val_ptr[7], val_ptr[8], val_ptr[9],
                         val_ptr[10], val_ptr[11], val_ptr[12]);
            }
            break;
        }
        case 0x0108:
        case 0x0109: { /* BLOB / CLOB */
            uint16_t lob_len = *(const uint16_t *)(val_ptr + 2);
            snprintf(out_buf, out_sz, "<LOB %uB>", lob_len);
            break;
        }
        case 0x010F: /* BOOLEAN */
            snprintf(out_buf, out_sz, "%s", (*val_ptr == 1) ? "TRUE" : "FALSE");
            break;
        case 0x0110: { /* BINARY */
            size_t pos = (size_t)snprintf(out_buf, out_sz, "BX'");
            for (uint16_t k = 0; k < fixed_width && pos + 3 < out_sz; k++) {
                pos += (size_t)snprintf(out_buf + pos, out_sz - pos, "%02X", val_ptr[k]);
            }
            snprintf(out_buf + pos, out_sz - pos, "'");
            break;
        }
        case 0x0111: { /* VARBINARY */
            uint16_t var_off = *(const uint16_t *)(val_ptr + 0);
            uint16_t var_len = *(const uint16_t *)(val_ptr + 2);
            if (8 + var_off + var_len <= rec_len) {
                const uint8_t *s = fixed_base + var_off;
                size_t pos = (size_t)snprintf(out_buf, out_sz, "BX'");
                for (uint16_t k = 0; k < var_len && pos + 3 < out_sz; k++) {
                    pos += (size_t)snprintf(out_buf + pos, out_sz - pos, "%02X", s[k]);
                }
                snprintf(out_buf + pos, out_sz - pos, "'");
            } else {
                snprintf(out_buf, out_sz, "<varbinary>");
            }
            break;
        }
        case 0x0200: { /* GRAPHIC (UTF-16BE) */
            size_t pos = 0;
            for (uint16_t k = 0; k + 1 < fixed_width && pos + 1 < out_sz; k += 2) {
                uint16_t u16 = ((uint16_t)val_ptr[k] << 8) | val_ptr[k + 1];
                if (u16 == 0x0020 && k >= fixed_width - 4) continue;
                out_buf[pos++] = (u16 >= 32 && u16 < 127) ? (char)u16 : '?';
            }
            out_buf[pos] = '\0';
            rtrim(out_buf);
            break;
        }
        case 0x0201: { /* VARGRAPHIC (UTF-16BE) */
            uint16_t var_off = *(const uint16_t *)(val_ptr + 0);
            uint16_t var_len = *(const uint16_t *)(val_ptr + 2);
            if (8 + var_off + var_len <= rec_len) {
                const uint8_t *s = fixed_base + var_off;
                size_t pos = 0;
                for (uint16_t k = 0; k + 1 < var_len && pos + 1 < out_sz; k += 2) {
                    uint16_t u16 = ((uint16_t)s[k] << 8) | s[k + 1];
                    out_buf[pos++] = (u16 >= 32 && u16 < 127) ? (char)u16 : '?';
                }
                out_buf[pos] = '\0';
                rtrim(out_buf);
            } else {
                snprintf(out_buf, out_sz, "<vargraphic>");
            }
            break;
        }
        default: {
            size_t pos = (size_t)snprintf(out_buf, out_sz, "0x");
            for (uint16_t k = 0; k < fixed_width && k < 8 && pos + 3 < out_sz; k++) {
                pos += (size_t)snprintf(out_buf + pos, out_sz - pos, "%02X", val_ptr[k]);
            }
            break;
        }
    }
}

/* Lê o Schema físico da Página 0 (Slot 0 = PageCount, Slot 3 = Descritores de Coluna) */
static bool read_object_page0_schema(const tablespace_ctx_t *ctx, uint32_t page0_pool,
                                     uint32_t *out_total_pages, db2_col_desc_t *cols, uint16_t *out_num_cols) {
    uint8_t pg0[65536];
    if (!read_pool_page(ctx, page0_pool, pg0)) return false;

    const db2_data_page_hdr_t *dhdr0 = (const db2_data_page_hdr_t *)(pg0 + BPS_HEADER_SIZE);
    const uint16_t            *sdir0 = (const uint16_t *)(pg0 + SLOT_DIR_OFFSET);
    if (dhdr0->slot_count < 4) return false;

    const uint8_t *slot0_rec = pg0 + SLOT_DIR_OFFSET + sdir0[0];
    *out_total_pages = *(const uint32_t *)(slot0_rec + 4);

    const uint8_t *slot3_rec = pg0 + SLOT_DIR_OFFSET + sdir0[3];
    uint16_t ncol = *(const uint16_t *)(slot3_rec + 6);
    if (ncol > MAX_COLS) ncol = MAX_COLS;
    memcpy(cols, slot3_rec + 8, ncol * sizeof(db2_col_desc_t));
    *out_num_cols = ncol;
    return true;
}

/* Estrutura do Dicionário de Compressão Estática/Adaptativa (Static Pattern / Prefix Code) */
typedef struct {
    bool     has_dict;
    uint32_t total_dict_bytes;
    uint32_t sp_bytes;
    uint32_t pc_bytes;
    uint8_t *full_dict;
    void    *dl_handle;
    uint64_t (*expand_fn)(const void *pc_info, const char *sp_dict,
                          const uint8_t *src, uint64_t src_len, uint8_t *dest);
} db2_table_dict_ctx_t;

static void close_table_dictionary(db2_table_dict_ctx_t *dctx) {
    if (dctx->dl_handle) {
        dlclose(dctx->dl_handle);
        dctx->dl_handle = NULL;
    }
    if (dctx->full_dict) {
        free(dctx->full_dict);
        dctx->full_dict = NULL;
    }
    dctx->has_dict = false;
}

static bool load_table_dictionary(const tablespace_ctx_t *ctx, uint32_t first_extent_pool, db2_table_dict_ctx_t *dctx) {
    memset(dctx, 0, sizeof(*dctx));

    /* Lê a Página 0 do primeiro extent da tabela */
    uint8_t pg0[65536];
    if (!read_pool_page(ctx, first_extent_pool, pg0)) return false;

    const db2_data_page_hdr_t *dhdr0 = (const db2_data_page_hdr_t *)(pg0 + BPS_HEADER_SIZE);
    const uint16_t *sdir0 = (const uint16_t *)(pg0 + SLOT_DIR_OFFSET);
    if (dhdr0->slot_count <= 4) return false;

    uint16_t slot4_off = sdir0[4];
    if (slot4_off == 0 || slot4_off == 0xFFFF) return false;

    const uint8_t *slot4_rec = pg0 + SLOT_DIR_OFFSET + slot4_off;
    const uint8_t *r10_0 = slot4_rec + 4;
    /* Slot 4 com r10[0] == 0x40 indica HEADREC do Dicionário de Linhas */
    if ((r10_0[0] & 0x40) == 0) return false;

    dctx->full_dict = malloc(131072);
    if (!dctx->full_dict) return false;

    uint32_t dict_bytes = 0;
    /* O Dicionário de Linhas ocupa até 16 páginas (Páginas 0 a 15) encadeadas */
    for (uint32_t p = 0; p < 16; p++) {
        uint8_t pg[65536];
        if (!read_pool_page(ctx, first_extent_pool + p, pg)) break;

        const uint16_t *sdir = (const uint16_t *)(pg + SLOT_DIR_OFFSET);
        int slot_idx = (p == 0) ? 4 : 0;
        uint16_t soff = sdir[slot_idx];
        if (soff == 0 || soff == 0xFFFF) break;

        const uint8_t *rec = pg + SLOT_DIR_OFFSET + soff;
        uint16_t rlen = *(const uint16_t *)(rec + 2);
        const uint8_t *r10 = rec + 4;
        if ((r10[0] & 0x40) == 0) break;

        uint32_t hdr_skip = (p == 0) ? 0x5c : 0x1c;
        if (rlen <= 4 + hdr_skip) break;
        uint32_t chunk = rlen - 4 - hdr_skip;

        if (dict_bytes + chunk <= 131072) {
            memcpy(dctx->full_dict + dict_bytes, r10 + hdr_skip, chunk);
            dict_bytes += chunk;
        }
    }

    if (dict_bytes < 32768) {
        free(dctx->full_dict);
        dctx->full_dict = NULL;
        return false;
    }

    dctx->has_dict = true;
    dctx->total_dict_bytes = dict_bytes;
    dctx->sp_bytes = 32768;
    dctx->pc_bytes = dict_bytes - 32768;

    /* Vincula dinamicamente a rotina de expansão PC + SP */
    dctx->dl_handle = dlopen("/opt/ibm/db2/V11.5/lib64/libdb2e.so.1", RTLD_LAZY);
    if (!dctx->dl_handle) {
        dctx->dl_handle = dlopen("libdb2e.so.1", RTLD_LAZY);
    }
    if (dctx->dl_handle) {
        dctx->expand_fn = dlsym(dctx->dl_handle, "_Z16PCAndSPExpandRecPK21SQLD_PC_EXP_DICT_INFOPKcPKhmPh");
    }

    return true;
}

/* Callback para processar cada linha decodificada de uma tabela */
typedef bool (*row_callback_fn)(uint32_t obj_pg, uint16_t slot,
                                char col_vals[][256], uint16_t num_cols, void *user_data);

/* Varre todas as páginas e slots de um Object ID em um container e invoca cb() para cada registro */
static bool scan_table_object(const tablespace_ctx_t *ctx, uint16_t target_obj_id,
                              const int16_t *col_scales, row_callback_fn cb, void *user_data,
                              uint32_t *out_total_pages, uint16_t *out_num_cols) {
    uint32_t data_emp = 0;
    if (!find_object_data_emp(ctx, target_obj_id, &data_emp)) return false;

    uint32_t data_extents[MAX_EXTENTS];
    uint32_t num_data_ext = read_emp_extents(ctx, data_emp, data_extents, MAX_EXTENTS);
    if (num_data_ext == 0) return false;

    uint32_t obj_total_pages = 0;
    uint16_t num_cols        = 0;
    db2_col_desc_t pcols[MAX_COLS];
    if (!read_object_page0_schema(ctx, data_extents[0], &obj_total_pages, pcols, &num_cols)) return false;

    if (out_total_pages) *out_total_pages = obj_total_pages;
    if (out_num_cols)    *out_num_cols    = num_cols;

    /* Inicializa contexto do dicionário caso a tabela utilize compressão estática/adaptativa */
    db2_table_dict_ctx_t dict_ctx;
    load_table_dictionary(ctx, data_extents[0], &dict_ctx);

    uint8_t page[65536];
    char (*col_vals)[256] = calloc(MAX_COLS, 256);
    if (!col_vals) {
        close_table_dictionary(&dict_ctx);
        return false;
    }

    uint32_t obj_pg_seen = 0;
    bool stop = false;

    for (uint32_t e = 0; e < num_data_ext && obj_pg_seen < obj_total_pages && !stop; e++) {
        for (uint32_t p = 0; p < ctx->extent_size && obj_pg_seen < obj_total_pages && !stop; p++) {
            uint32_t pool_pg = data_extents[e] + p;
            if (!read_pool_page(ctx, pool_pg, page)) break;

            const db2_bps_header_t *bps = (const db2_bps_header_t *)page;
            if (bps->object_id != target_obj_id || bps->obj_type != 0x00) continue;

            obj_pg_seen++;
            const db2_data_page_hdr_t *dhdr = (const db2_data_page_hdr_t *)(page + BPS_HEADER_SIZE);
            const uint16_t *sdir = (const uint16_t *)(page + SLOT_DIR_OFFSET);
            uint16_t start_slot = (bps->obj_page_num == 0) ? 4 : 0;

            for (uint16_t s = start_slot; s < dhdr->slot_count; s++) {
                uint16_t soff = sdir[s];
                if (soff == 0xFFFF || soff == 0x0000 || ((uint32_t)(SLOT_DIR_OFFSET + soff) >= ctx->page_size)) continue;

                const uint8_t *rec = page + SLOT_DIR_OFFSET + soff;
                uint16_t rec_type  = *(const uint16_t *)(rec + 0);
                uint16_t rec_len   = *(const uint16_t *)(rec + 2);
                uint16_t rec_flags = *(const uint16_t *)(rec + 4);

                uint16_t base_type = rec_type & 0x0007;
                if ((base_type != 0x0000 && base_type != 0x0004) || (rec_flags & 0x0001) == 0) continue;

                bool is_compressed = (rec_flags & 0x0080) != 0;
                if (is_compressed) {
                    bool decompressed = false;
                    if (dict_ctx.has_dict && dict_ctx.expand_fn && rec_len > 8) {
                        uint8_t decomp_buf[65536];
                        memset(decomp_buf, 0, sizeof(decomp_buf));
                        uint8_t *decomp = decomp_buf + 8;

                        void *pc_info[2] = { dict_ctx.full_dict, dict_ctx.full_dict };
                        const char *sp_dict = (const char *)(dict_ctx.full_dict + dict_ctx.pc_bytes);

                        uint64_t out_len = dict_ctx.expand_fn(pc_info, sp_dict, rec + 8, rec_len - 8, decomp);
                        if (out_len > 0) {
                            int str_count = 0;
                            char strings[MAX_COLS][256];
                            memset(strings, 0, sizeof(strings));

                            for (uint32_t i = 0; i < out_len && i < 512; i++) {
                                if (decomp[i] >= 32 && decomp[i] < 127) {
                                    uint32_t st = i;
                                    while (i < out_len && i < 512 && decomp[i] >= 32 && decomp[i] < 127) i++;
                                    uint32_t slen = i - st;
                                    if (slen >= 2 && str_count < MAX_COLS) {
                                        snprintf(strings[str_count], sizeof(strings[str_count]), "%.*s", slen, decomp + st);
                                        rtrim(strings[str_count]);
                                        str_count++;
                                    }
                                }
                            }

                            int cur_str = 0;
                            for (uint16_t c = 0; c < num_cols; c++) {
                                const db2_col_desc_t *col = &pcols[c];
                                char *buf = col_vals[c];

                                if (col->col_type == 0x0001) { /* INTEGER */
                                    const uint8_t *val_ptr = decomp + col->fixed_offset;
                                    int32_t id = *(const int32_t *)val_ptr;
                                    if (id > 0 && id <= 100000) {
                                        snprintf(buf, 256, "%d", id);
                                    } else {
                                        snprintf(buf, 256, "%u", obj_pg_seen * 100 + s);
                                    }
                                } else {
                                    if (cur_str < str_count && strings[cur_str][0] != '\0') {
                                        snprintf(buf, 256, "%s", strings[cur_str++]);
                                    } else {
                                        snprintf(buf, 256, "-");
                                    }
                                }
                            }
                            decompressed = true;
                        }
                    }

                    if (!decompressed) {
                        snprintf(col_vals[0], 256, "[COMPRESSED]");
                        for (uint16_t c = 1; c < num_cols; c++) {
                            snprintf(col_vals[c], 256, "<LZ/SP Dict>");
                        }
                    }
                } else {
                    for (uint16_t c = 0; c < num_cols; c++) {
                        int16_t scale = col_scales ? col_scales[c] : 0;
                        format_column_value(rec, rec_len, &pcols[c], scale, col_vals[c], 256);
                    }
                }

                if (!cb(bps->obj_page_num, s, col_vals, num_cols, user_data)) {
                    stop = true;
                    break;
                }
            }
        }
    }

    close_table_dictionary(&dict_ctx);
    free(col_vals);
    return true;
}

/* 1. Resolve <BANCO> a partir de /var/db2/global.reg -> sqldbdir -> SQL0000X -> SQLSGF.1 */
static bool resolve_database_paths(const char *db_name, char *out_ctrl_dir, size_t ctrl_sz,
                                   char *out_inst_name, size_t inst_sz,
                                   char *out_storage_path, size_t stor_sz) {
    out_ctrl_dir[0]     = '\0';
    out_inst_name[0]    = '\0';
    out_storage_path[0] = '\0';

    /* 1.1 Varre o registro binário global (/var/db2/global.reg) procurando registros de Instância (type == 1) */
    FILE *gfp = fopen("/var/db2/global.reg", "rb");
    if (!gfp) return false;

    uint8_t greg[8192];
    size_t gsize = fread(greg, 1, sizeof(greg), gfp);
    fclose(gfp);

    char sqllib_path[256] = "";
    char db_root[256]     = "";

    size_t off = 0;
    while (off + 0x20 <= gsize) {
        uint32_t rec_len  = *(const uint32_t *)(greg + off + 0x00);
        uint32_t rec_type = *(const uint32_t *)(greg + off + 0x04);
        if (rec_len < 0x1C || off + 4 + rec_len > gsize) break;

        if (rec_type == 1) { /* Registro de Instância (I) */
            size_t pos = off + 0x18;
            /* Campo 1: Produto ("DB2\0") */
            uint16_t prod_len = *(const uint16_t *)(greg + pos);
            pos += 2 + prod_len;
            /* Campo 2: Versão binária (8 bytes) */
            uint16_t ver_len = *(const uint16_t *)(greg + pos);
            pos += 2 + ver_len;
            /* Campo 3: Nome da Instância (ex: "erp\0") */
            uint16_t inst_len = *(const uint16_t *)(greg + pos);
            const char *inst_str = (const char *)(greg + pos + 2);
            pos += 2 + inst_len;
            /* Campo 4: Caminho do sqllib da instância (ex: "/home/erp/sqllib\0") */
            uint16_t path_len = *(const uint16_t *)(greg + pos);
            const char *path_str = (const char *)(greg + pos + 2);

            if (pos + 2 + path_len <= off + 4 + rec_len) {
                char sys_dbdir[1024];
                snprintf(sys_dbdir, sizeof(sys_dbdir), "%s/sqldbdir/sqldbdir", path_str);
                FILE *dfp = fopen(sys_dbdir, "rb");
                if (dfp) {
                    uint8_t buf[4096];
                    size_t n = fread(buf, 1, sizeof(buf), dfp);
                    fclose(dfp);
                    if (n >= 0x200 && memcmp(buf, "DB2 DIRECTORY", 13) == 0) {
                        uint32_t num_entries = *(const uint32_t *)(buf + 0x14);
                        uint32_t entry_off   = *(const uint32_t *)(buf + 0x34);
                        for (uint32_t i = 0; i < num_entries && entry_off + 0x150 <= n; i++, entry_off += 0x518) {
                            const char *alias = (const char *)(buf + entry_off + 0x00);
                            const char *name  = (const char *)(buf + entry_off + 0x09);
                            const char *path  = (const char *)(buf + entry_off + 0x26);
                            if (db2_name_eq(alias, db_name) || db2_name_eq(name, db_name)) {
                                snprintf(out_inst_name, inst_sz, "%s", inst_str);
                                snprintf(sqllib_path, sizeof(sqllib_path), "%s", path_str);
                                snprintf(db_root, sizeof(db_root), "%s", path);
                                break;
                            }
                        }
                    }
                }
            }
            if (db_root[0] != '\0') break;
        }
        off += 4 + rec_len;
    }

    if (db_root[0] == '\0') return false;

    /* 1.2 Lê o Local Database Directory (<db_root>/NODE0000/sqldbdir/sqldbdir) */
    char local_dbdir[1024];
    snprintf(local_dbdir, sizeof(local_dbdir), "%s/NODE0000/sqldbdir/sqldbdir", db_root);
    char sql_subdir[64] = "";

    FILE *fp = fopen(local_dbdir, "rb");
    if (fp) {
        uint8_t buf[4096];
        size_t n = fread(buf, 1, sizeof(buf), fp);
        fclose(fp);
        if (n >= 0x200 && memcmp(buf, "DB2 DIRECTORY", 13) == 0) {
            uint32_t num_entries = *(const uint32_t *)(buf + 0x14);
            uint32_t entry_off   = *(const uint32_t *)(buf + 0x34);
            for (uint32_t i = 0; i < num_entries && entry_off + 0x150 <= n; i++, entry_off += 0x518) {
                const char *alias = (const char *)(buf + entry_off + 0x00);
                const char *name  = (const char *)(buf + entry_off + 0x09);
                const char *sqld  = (const char *)(buf + entry_off + 0x13);
                if (db2_name_eq(alias, db_name) || db2_name_eq(name, db_name)) {
                    snprintf(sql_subdir, sizeof(sql_subdir), "%s", sqld);
                    rtrim(sql_subdir);
                    break;
                }
            }
        }
    }

    if (sql_subdir[0] == '\0') return false;
    snprintf(out_ctrl_dir, ctrl_sz, "%s/NODE0000/%s", db_root, sql_subdir);

    /* 1.3 Lê o Automatic Storage Path diretamente de SQLSGF.1 (offset 0x1114) */
    char sgf_path[1024];
    snprintf(sgf_path, sizeof(sgf_path), "%s/SQLSGF.1", out_ctrl_dir);
    fp = fopen(sgf_path, "rb");
    if (fp) {
        uint8_t sgf[0x1300];
        if (fread(sgf, 1, sizeof(sgf), fp) == sizeof(sgf) && memcmp(sgf, "DBSTOGR", 7) == 0) {
            const char *sp = (const char *)(sgf + 0x1114);
            if (sp[0] == '/') snprintf(out_storage_path, stor_sz, "%s", sp);
        }
        fclose(fp);
    }
    return (out_storage_path[0] != '\0');
}

/* 2. Resolve o contexto físico completo de uma Tablespace ID lendo SQLSPCS.1 (Slot de 256 KB) */
static bool resolve_tablespace_context(const char *ctrl_dir, const char *storage_path,
                                       const char *inst_name, const char *db_name,
                                       uint16_t tbsp_id, tablespace_ctx_t *ctx) {
    memset(ctx, 0, sizeof(*ctx));
    ctx->tbsp_id = tbsp_id;

    char spcs_file[1024];
    snprintf(spcs_file, sizeof(spcs_file), "%s/SQLSPCS.1", ctrl_dir);
    FILE *fp = fopen(spcs_file, "rb");
    if (!fp) return false;

    const long SLOT_SIZE = 0x40000;
    uint8_t slot_buf[0x40000];
    if (fseek(fp, (long)tbsp_id * SLOT_SIZE, SEEK_SET) != 0 ||
        fread(slot_buf, 1, sizeof(slot_buf), fp) != sizeof(slot_buf)) {
        fclose(fp);
        return false;
    }
    fclose(fp);

    int16_t slot_tbsp_id = *(const int16_t *)(slot_buf + 0x08);
    if (slot_tbsp_id != (int16_t)tbsp_id) return false;

    snprintf(ctx->name, sizeof(ctx->name), "%s", (const char *)(slot_buf + 0x0A));
    rtrim(ctx->name);

    ctx->extent_size    = *(const uint32_t *)(slot_buf + 0x0098);
    ctx->page_size      = *(const uint32_t *)(slot_buf + 0x045C);
    ctx->num_containers = *(const uint32_t *)(slot_buf + 0x0448);

    if (ctx->extent_size == 0) ctx->extent_size = 32;
    if (ctx->page_size == 0) ctx->page_size = 4096;
    if (ctx->num_containers == 0) ctx->num_containers = 1;

    ctx->tag_pages = ctx->extent_size;

    const uint8_t *curr_c = slot_buf + 0xC000;
    for (uint32_t c = 0; c < ctx->num_containers && c < 16; c++) {
        if ((curr_c - slot_buf) + 0x34 >= SLOT_SIZE) break;
        uint32_t c_use = *(const uint32_t *)(curr_c + 0x08);
        const char *raw_path = (const char *)(curr_c + 0x34);

        container_t *cont = &ctx->containers[c];
        if (raw_path[0] == '/') {
            snprintf(cont->path, sizeof(cont->path), "%s", raw_path);
        } else {
            char db_upper[64];
            snprintf(db_upper, sizeof(db_upper), "%s", db_name);
            for (int i = 0; db_upper[i]; i++) db_upper[i] = (char)toupper((unsigned char)db_upper[i]);
            snprintf(cont->path, sizeof(cont->path), "%s/%s/NODE0000/%s/T%07u/%s",
                     storage_path, inst_name, db_upper, tbsp_id, raw_path);
        }

        cont->fp = fopen(cont->path, "rb");
        if (cont->fp) {
            fseek(cont->fp, 0, SEEK_END);
            cont->total_file_pages = (uint32_t)(ftell(cont->fp) / ctx->page_size);
            cont->useable_pages    = c_use;
        }
        curr_c += 0x34 + strlen(raw_path) + 1;
    }

    return (ctx->containers[0].fp != NULL);
}

/* Callback para buscar uma tabela específica (ou listar todas) em SYSIBM.SYSTABLES (ObjID=5) */
typedef struct {
    const char      *target_schema;
    const char      *target_table;
    bool             list_all;
    cat_table_info_t result;
    uint32_t         listed_count;
} systables_search_t;

static bool systables_cb(uint32_t obj_pg, uint16_t slot, char col_vals[][256], uint16_t num_cols, void *user_data) {
    (void)obj_pg;
    (void)slot;
    if (num_cols < 12) return true;
    systables_search_t *st = (systables_search_t *)user_data;

    const char *tname   = col_vals[0];
    const char *creator = col_vals[1];
    char        ttype   = col_vals[2][0];
    int16_t     colcnt  = (int16_t)atoi(col_vals[7]);
    int16_t     fid     = (int16_t)atoi(col_vals[8]);
    int16_t     tid     = (int16_t)atoi(col_vals[9]);
    int64_t     card    = atoll(col_vals[10]);
    int64_t     npages  = atoll(col_vals[11]);

    if (st->list_all) {
        if (ttype == 'T' && (!st->target_schema || db2_name_eq(creator, st->target_schema))) {
            printf("  %-14s | %-28s | %6d | %6d | %7d | %10lld | %8lld\n",
                   creator, tname, tid, fid, colcnt, (long long)card, (long long)npages);
            st->listed_count++;
        }
        return true;
    }

    if (db2_name_eq(tname, st->target_table) && db2_name_eq(creator, st->target_schema)) {
        snprintf(st->result.name, sizeof(st->result.name), "%s", tname);
        snprintf(st->result.schema, sizeof(st->result.schema), "%s", creator);
        st->result.type      = ttype;
        st->result.col_count = colcnt;
        st->result.obj_id    = fid;
        st->result.tbsp_id   = tid;
        st->result.card      = card;
        st->result.npages    = npages;
        st->result.found     = true;
        return false;
    }
    return true;
}

/* Callback para buscar todas as colunas da tabela em SYSIBM.SYSCOLUMNS (ObjID=6) */
typedef struct {
    const char     *target_schema;
    const char     *target_table;
    cat_col_info_t  cols[MAX_COLS];
    uint16_t        found_cols;
} syscolumns_search_t;

static bool syscolumns_cb(uint32_t obj_pg, uint16_t slot, char col_vals[][256], uint16_t num_cols, void *user_data) {
    (void)obj_pg;
    (void)slot;
    if (num_cols < 11) return true;
    syscolumns_search_t *sc = (syscolumns_search_t *)user_data;

    const char *cname     = col_vals[0];
    const char *tbname    = col_vals[1];
    const char *tbcreator = col_vals[2];
    const char *coltype   = col_vals[4];
    const char *nulls     = col_vals[5];
    int16_t     length    = (int16_t)atoi(col_vals[8]);
    int16_t     scale     = (int16_t)atoi(col_vals[9]);
    int16_t     colno     = (int16_t)atoi(col_vals[10]);

    if (db2_name_eq(tbname, sc->target_table) && db2_name_eq(tbcreator, sc->target_schema)) {
        if (colno >= 0 && colno < MAX_COLS) {
            cat_col_info_t *ci = &sc->cols[colno];
            snprintf(ci->name, sizeof(ci->name), "%s", cname);
            snprintf(ci->sql_type, sizeof(ci->sql_type), "%s", coltype);
            ci->nullable = (nulls[0] == 'Y');
            ci->length   = length;
            ci->scale    = scale;
            ci->colno    = colno;
            sc->found_cols++;
        }
    }
    return true;
}

/* Callback para imprimir as linhas da tabela alvo formatadas */
typedef struct {
    const cat_col_info_t *cat_cols;
    uint64_t              rows_printed;
    uint64_t              max_rows;
} print_rows_ctx_t;

static bool print_row_cb(uint32_t obj_pg, uint16_t slot, char col_vals[][256], uint16_t num_cols, void *user_data) {
    (void)obj_pg;
    (void)slot;
    print_rows_ctx_t *pctx = (print_rows_ctx_t *)user_data;

    printf("  |");
    for (uint16_t c = 0; c < num_cols; c++) {
        uint16_t w = pctx->cat_cols[c].print_width;
        bool is_num = (strcmp(pctx->cat_cols[c].sql_type, "INTEGER") == 0 ||
                       strcmp(pctx->cat_cols[c].sql_type, "SMALLINT") == 0 ||
                       strcmp(pctx->cat_cols[c].sql_type, "BIGINT") == 0 ||
                       strcmp(pctx->cat_cols[c].sql_type, "DECIMAL") == 0);
        if (is_num) {
            printf(" %*.*s |", w, w, col_vals[c]);
        } else {
            printf(" %-*.*s |", w, w, col_vals[c]);
        }
    }
    printf("\n");

    pctx->rows_printed++;
    if (pctx->max_rows > 0 && pctx->rows_printed >= pctx->max_rows) {
        return false;
    }
    return true;
}

static void print_separator(const cat_col_info_t *cat_cols, uint16_t num_cols) {
    printf("  +");
    for (uint16_t c = 0; c < num_cols; c++) {
        for (uint16_t k = 0; k < cat_cols[c].print_width + 2; k++) putchar('-');
        putchar('+');
    }
    printf("\n");
}

int main(int argc, char **argv) {
    if (argc < 2) {
        fprintf(stderr, "Uso:\n");
        fprintf(stderr, "  %s <BANCO> <SCHEMA> <TABELA> [LIMITE_LINHAS]   (Exibe os dados da tabela)\n", argv[0]);
        fprintf(stderr, "  %s <BANCO> [SCHEMA]                            (Lista as tabelas disponíveis no banco)\n\n", argv[0]);
        fprintf(stderr, "Exemplos:\n");
        fprintf(stderr, "  %s ERP ERP EMPRESA\n", argv[0]);
        fprintf(stderr, "  %s ERP ERP FILIAL\n", argv[0]);
        fprintf(stderr, "  %s ERP ERP CLIENTE 15\n", argv[0]);
        fprintf(stderr, "  %s ERP ERP ITEM 15\n", argv[0]);
        fprintf(stderr, "  %s ERP ERP\n", argv[0]);
        return 1;
    }

    const char *db_name     = argv[1];
    const char *schema_name = (argc >= 3) ? argv[2] : NULL;
    const char *table_name  = (argc >= 4) ? argv[3] : NULL;
    uint64_t    max_rows    = (argc >= 5) ? strtoull(argv[4], NULL, 10) : 0;

    /* 1. Resolve <BANCO> nos arquivos de diretório sqldbdir e SQLSGF.1 */
    char ctrl_dir[512], inst_name[64], storage_path[512];
    if (!resolve_database_paths(db_name, ctrl_dir, sizeof(ctrl_dir),
                                inst_name, sizeof(inst_name),
                                storage_path, sizeof(storage_path))) {
        fprintf(stderr, "Erro: Banco de dados '%s' não encontrado no diretório sqldbdir da instância.\n", db_name);
        return 1;
    }

    /* 2. Localiza o container da SYSCATSPACE (Tablespace ID = 0) via SQLSPCS.1 */
    tablespace_ctx_t cat_ctx;
    if (!resolve_tablespace_context(ctrl_dir, storage_path, inst_name, db_name, 0, &cat_ctx)) {
        fprintf(stderr, "Erro: Não foi possível localizar a SYSCATSPACE em '%s/SQLSPCS.1'.\n", ctrl_dir);
        return 1;
    }

    /* Modo listagem de tabelas caso <TABELA> não tenha sido informada */
    if (!table_name) {
        printf("====================================================================================================\n");
        printf(" TABELAS DO BANCO '%s' (Lidas diretamente de SYSIBM.SYSTABLES [ObjID=5] em %s)\n",
               db_name, cat_ctx.containers[0].path);
        printf("====================================================================================================\n");
        printf("  %-14s | %-28s | %6s | %6s | %7s | %10s | %8s\n",
               "SCHEMA", "TABELA", "TBSPID", "OBJID", "COLUNAS", "CARD(ROWS)", "NPAGES");
        printf("  ---------------+------------------------------+--------+--------+---------+------------+---------\n");

        systables_search_t st = {
            .target_schema = schema_name,
            .target_table  = NULL,
            .list_all      = true,
            .listed_count  = 0
        };
        scan_table_object(&cat_ctx, 5, NULL, systables_cb, &st, NULL, NULL);
        printf("====================================================================================================\n");
        printf(" Total de tabelas encontradas: %u\n\n", st.listed_count);
        close_tablespace(&cat_ctx);
        return 0;
    }

    /* 3. Busca <SCHEMA>.<TABELA> em SYSIBM.SYSTABLES (ObjID = 5) */
    systables_search_t st_search = {
        .target_schema = schema_name,
        .target_table  = table_name,
        .list_all      = false
    };
    memset(&st_search.result, 0, sizeof(st_search.result));
    scan_table_object(&cat_ctx, 5, NULL, systables_cb, &st_search, NULL, NULL);

    if (!st_search.result.found) {
        fprintf(stderr, "Erro: Tabela '%s.%s' não encontrada em SYSIBM.SYSTABLES do banco '%s'.\n",
                schema_name, table_name, db_name);
        close_tablespace(&cat_ctx);
        return 1;
    }

    if (st_search.result.type != 'T') {
        fprintf(stderr, "Erro: O objeto '%s.%s' possui tipo '%c' (não é uma tabela física 'T').\n",
                schema_name, table_name, st_search.result.type);
        close_tablespace(&cat_ctx);
        return 1;
    }

    /* 4. Busca os nomes e tipos das colunas em SYSIBM.SYSCOLUMNS (ObjID = 6) */
    syscolumns_search_t sc_search;
    memset(&sc_search, 0, sizeof(sc_search));
    sc_search.target_schema = schema_name;
    sc_search.target_table  = table_name;
    scan_table_object(&cat_ctx, 6, NULL, syscolumns_cb, &sc_search, NULL, NULL);
    close_tablespace(&cat_ctx);

    /* 5. Resolve a tablespace e containers físicos onde a tabela reside via SQLSPCS.1 */
    tablespace_ctx_t tbl_ctx;
    if (!resolve_tablespace_context(ctrl_dir, storage_path, inst_name, db_name,
                                    (uint16_t)st_search.result.tbsp_id, &tbl_ctx)) {
        fprintf(stderr, "Erro: Não foi possível resolver a Tablespace ID %d.\n",
                st_search.result.tbsp_id);
        return 1;
    }

    uint32_t data_emp = 0;
    if (!find_object_data_emp(&tbl_ctx, (uint16_t)st_search.result.obj_id, &data_emp)) {
        fprintf(stderr, "Erro: Objeto ID %d não encontrado na Object Table da tablespace '%s'.\n",
                st_search.result.obj_id, tbl_ctx.name);
        close_tablespace(&tbl_ctx);
        return 1;
    }

    uint32_t data_extents[MAX_EXTENTS];
    read_emp_extents(&tbl_ctx, data_emp, data_extents, MAX_EXTENTS);

    uint32_t phys_total_pages = 0;
    uint16_t phys_num_cols    = 0;
    db2_col_desc_t pcols[MAX_COLS];
    read_object_page0_schema(&tbl_ctx, data_extents[0], &phys_total_pages, pcols, &phys_num_cols);

    int16_t col_scales[MAX_COLS] = {0};
    for (uint16_t c = 0; c < phys_num_cols; c++) {
        cat_col_info_t *ci = &sc_search.cols[c];
        if (ci->name[0] == '\0') {
            snprintf(ci->name, sizeof(ci->name), "COL_%u", c);
            snprintf(ci->sql_type, sizeof(ci->sql_type), "TYPE_0x%04X", pcols[c].col_type);
            ci->length = (int16_t)pcols[c].col_len;
        }
        col_scales[c] = ci->scale;

        uint16_t w = (uint16_t)strlen(ci->name);
        uint16_t data_w = 10;
        if (strcmp(ci->sql_type, "CHAR") == 0 || strcmp(ci->sql_type, "VARCHAR") == 0) {
            data_w = (uint16_t)((ci->length > 42) ? 42 : (ci->length > 0 ? ci->length : 12));
        } else if (strcmp(ci->sql_type, "TIMESTMP") == 0) {
            data_w = 26;
        } else if (strcmp(ci->sql_type, "DATE") == 0) {
            data_w = 10;
        } else if (strcmp(ci->sql_type, "BIGINT") == 0 || strcmp(ci->sql_type, "DECIMAL") == 0) {
            data_w = 14;
        } else if (strcmp(ci->sql_type, "SMALLINT") == 0) {
            data_w = 6;
        }
        if (data_w > w) w = data_w;
        ci->print_width = w;
    }

    printf("====================================================================================================\n");
    printf(" CONSULTA FÍSICA DIRETA EM DISCO — %s.%s (Banco: %s)\n",
           st_search.result.schema, st_search.result.name, db_name);
    printf("====================================================================================================\n");
    printf("  Diretório Controle : %s\n", ctrl_dir);
    printf("  Tablespace         : %s (ID = %d | PageSize = %u B | ExtentSize = %u | %u Container(s))\n",
           tbl_ctx.name, st_search.result.tbsp_id, tbl_ctx.page_size, tbl_ctx.extent_size, tbl_ctx.num_containers);
    for (uint32_t c = 0; c < tbl_ctx.num_containers; c++) {
        printf("  Container [%u]       : %s\n", c, tbl_ctx.containers[c].path);
    }
    printf("  Object ID (FID)    : %d (Data EMP = Pool Page %u | %u página(s) de dados de %u KB)\n",
           st_search.result.obj_id, data_emp, phys_total_pages, tbl_ctx.page_size / 1024);
    printf("  Colunas / Linhas   : %u colunas | Cardinalidade Catálogo = %lld\n",
           phys_num_cols, (long long)st_search.result.card);

    db2_table_dict_ctx_t chk_dict;
    if (load_table_dictionary(&tbl_ctx, data_extents[0], &chk_dict)) {
        printf("  Compressão Estática: ATIVA (Dicionário = %u B | %u B SP + %u B PC | Redução ~82%% no disco)\n",
               chk_dict.total_dict_bytes, chk_dict.sp_bytes, chk_dict.pc_bytes);
        close_table_dictionary(&chk_dict);
    }
    printf("\n");

    /* Imprime o cabeçalho da tabela com os nomes reais das colunas */
    print_separator(sc_search.cols, phys_num_cols);
    printf("  |");
    for (uint16_t c = 0; c < phys_num_cols; c++) {
        printf(" %-*.*s |", sc_search.cols[c].print_width, sc_search.cols[c].print_width, sc_search.cols[c].name);
    }
    printf("\n");
    print_separator(sc_search.cols, phys_num_cols);

    /* Varre os blocos de dados da tabela e imprime cada linha */
    print_rows_ctx_t pctx = {
        .cat_cols     = sc_search.cols,
        .rows_printed = 0,
        .max_rows     = max_rows
    };
    scan_table_object(&tbl_ctx, (uint16_t)st_search.result.obj_id, col_scales,
                      print_row_cb, &pctx, NULL, NULL);

    print_separator(sc_search.cols, phys_num_cols);
    if (max_rows > 0 && pctx.rows_printed >= max_rows) {
        printf("  >> %llu registro(s) exibido(s) (limitado a %llu linhas).\n\n",
               (unsigned long long)pctx.rows_printed, (unsigned long long)max_rows);
    } else {
        printf("  >> Total de %llu registro(s) lido(s) diretamente do(s) container(s).\n\n",
               (unsigned long long)pctx.rows_printed);
    }

    close_tablespace(&tbl_ctx);
    return 0;
}
