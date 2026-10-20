/*
 * db2_inspector.c - Engenharia Reversa de Containers, Índices B+Tree e SQLSPCS do IBM Db2 11.5
 *
 * Lê diretamente do disco arquivos de container DMS / Automatic Storage
 * (.LRG, .USR, .CAT, .dat) e arquivos de controle de tablespaces (SQLSPCS.1/.2)
 * sem utilizar bibliotecas ou conexão SQL do Db2.
 *
 * Estruturas decodificadas:
 *  1. Container Tag (DB2CONT no offset 0x200 do Extent 0 do arquivo)
 *  2. Space Map Pages (SMP - Object ID 0xFFFE, Type 0x0E) e High Water Mark
 *  3. Mapa físico de Extents alocados (estilo db2dart /DHWM)
 *  4. EMP da Object Table (Pool Page 1) e Object Table (Object ID 0xFFFF)
 *  5. Extent Map Pages (EMP - Type 0x40 Data / 0x41 Index) de cada objeto
 *  6. Cabeçalhos BPS (48B), Data Page Header (20B), Slot Directory
 *  7. Metadados da Página 0 (Slot 0 Header, Slot 1 FSCR, Slot 2 TDIR, Slot 3 Colunas)
 *  8. Registros físicos de dados (FIXEDVAR) com decodificação automática de tipos
 *  9. Objetos de Índice (Type 0x01): Index SMP (Pg 0), MetaIndex (Pg 1),
 *     Index Root Vector ("RT10"), travessia completa da árvore B+ (Root -> Internal -> Leaf)
 *     e decodificação de chaves + ponteiros RID(Page:Slot) e Info Page (Pg 3)
 * 10. Arquivo de controle SQLSPCS.1/.2 (blocos de 256 KB por tablespace, inclusive
 *     tablespaces removidas via DROP TABLESPACE como a USERSPACE1)
 */

#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <stdbool.h>
#include <string.h>
#include <ctype.h>
#include <errno.h>

#define DB2_PAGE_SIZE       4096
#define BPS_HEADER_SIZE     48      /* 0x30 bytes */
#define DATA_PG_HDR_SIZE    20      /* 0x14 bytes */
#define SLOT_DIR_OFFSET     68      /* 0x44 = 48 + 20 (base para todos os slot offsets de Data Pages) */
#define SQLSPCS_SLOT_SIZE   0x40000 /* 262.144 bytes (256 KB) por tablespace no SQLSPCS.1 */

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
    uint16_t col_type;              /* Tipo interno: 0x0000=SMALLINT, 0x0001=INT, 0x0005=BIGINT,
                                       0x0100=CHAR, 0x0101=VARCHAR, 0x0105=DATE, 0x0107=TIMESTAMP */
    uint16_t col_len;               /* Tamanho máximo/fixo (ou precisão/escala em TIMESTAMP/DECIMAL) */
    uint16_t null_flags;            /* 0x0001 = Allows NULLs, 0x0002 = Prohibits NULLs */
    uint16_t fixed_offset;          /* Offset relativo ao início da área fixa do registro (+8) */
} db2_col_desc_t;

/* Cabeçalho de Nó de Índice B+Tree (36 bytes, em +0x30..+0x53) */
typedef struct {
    uint16_t slot_count;            /* +0x30: Total de slots no diretório de chaves */
    uint16_t total_free_space;      /* +0x32: Espaço livre total no nó */
    uint32_t page_state;            /* +0x34: Flags: 0x01=Root, 0x04=Leaf, 0x08=RootVec,
                                              0x100000=DummyHiKey, 0x40000000=LargeRID(6B) */
    uint32_t reserved1;             /* +0x38 */
    uint16_t max_free_space;        /* +0x3C: Espaço máximo utilizável */
    uint16_t free_space_offset;     /* +0x3E: Offset do espaço livre */
    uint32_t index_root_page;       /* +0x40: Página raiz do índice ao qual este nó pertence */
    uint32_t tree_level;            /* +0x44: Nível na árvore B+ (1 = Folha/Leaf, >1 = Interno/Root) */
    uint32_t next_node;             /* +0x48: Próxima página na cadeia de folhas (0 = Fim) */
    uint32_t prev_node;             /* +0x4C: Página anterior na cadeia de folhas (0 = Início) */
    uint32_t reserved2;             /* +0x50 */
} db2_idx_node_hdr_t;

/* Descritor de coluna de chave (Key Part) dentro do Index Root Vector ("RT10") - 12 bytes */
typedef struct {
    uint16_t dms_field_id;          /* +0x00: ID da coluna na tabela (0-based, ou 0xFFFF no MetaIndex) */
    uint16_t order_flags;           /* +0x02: 0 = Ascending */
    uint16_t codepage;              /* +0x04 */
    uint16_t col_type;              /* +0x06: Tipo de dado (mesma tabela de tipos do Slot 3) */
    uint16_t col_len;               /* +0x08: Tamanho em bytes da chave */
    uint16_t null_flags;            /* +0x0A: 0x0002 = Prohibits NULLs, 0x0001 = Allows NULLs */
} db2_idx_keypart_t;

#pragma pack(pop)

typedef struct {
    FILE    *fp;
    char     filepath[512];
    uint64_t file_size;
    uint32_t total_file_pages;
    uint32_t tag_pages;             /* Tamanho do Container Tag em páginas (= extent_size) */
    uint32_t extent_size;           /* Páginas por extent */
    uint32_t useable_pages;         /* total_file_pages - tag_pages */
    uint16_t tbspace_id;
} db2_container_t;

/* Lê uma página física do arquivo (File Page) */
static bool read_file_page(db2_container_t *ctx, uint32_t file_page, uint8_t *buf) {
    uint64_t offset = (uint64_t)file_page * DB2_PAGE_SIZE;
    if (offset + DB2_PAGE_SIZE > ctx->file_size) {
        return false;
    }
    if (fseek(ctx->fp, (long)offset, SEEK_SET) != 0) {
        return false;
    }
    return fread(buf, 1, DB2_PAGE_SIZE, ctx->fp) == DB2_PAGE_SIZE;
}

/* Lê uma Pool Page (página lógica da tablespace: File Page = Pool Page + tag_pages) */
static bool read_pool_page(db2_container_t *ctx, uint32_t pool_page, uint8_t *buf) {
    return read_file_page(ctx, pool_page + ctx->tag_pages, buf);
}

/* Lê uma página lógica (0-based) de um Objeto (Dados ou Índice) usando a lista de Extents de seu EMP */
static bool read_object_logical_page(db2_container_t *ctx, const uint32_t *extents,
                                     uint32_t num_extents, uint32_t obj_page_num,
                                     uint8_t *buf, uint32_t *out_pool_pg) {
    uint32_t ext_idx = obj_page_num / ctx->extent_size;
    uint32_t pg_off  = obj_page_num % ctx->extent_size;
    if (ext_idx >= num_extents) return false;
    uint32_t pool_pg = extents[ext_idx] + pg_off;
    if (out_pool_pg) *out_pool_pg = pool_pg;
    return read_pool_page(ctx, pool_pg, buf);
}

static const char *obj_type_name(uint8_t obj_type, uint16_t obj_id) {
    if (obj_id == 0xFFFE && obj_type == 0x0E) return "SMP (Space Map Page)";
    if (obj_id == 0xFFFF && obj_type == 0x40) return "Object Table EMP";
    if (obj_id == 0xFFFF && obj_type == 0x00) return "Object Table Data";
    switch (obj_type) {
        case 0x00: return "Table Data";
        case 0x01: return "Index";
        case 0x02: return "Long Field";
        case 0x03: return "LOB Data";
        case 0x04: return "LOB Allocation";
        case 0x0E: return "SMP";
        case 0x40: return "Data EMP (Extent Map)";
        case 0x41: return "Index EMP (Extent Map)";
        case 0x42: return "Long EMP";
        case 0x43: return "LOB EMP";
        case 0x44: return "LOB Alloc EMP";
        default:   return "Unknown/Internal";
    }
}

static const char *col_type_name(uint16_t col_type) {
    switch (col_type) {
        case 0x0000: return "SMALLINT";
        case 0x0001: return "INTEGER";
        case 0x0002: return "DECIMAL";
        case 0x0003: return "REAL/FLOAT32";
        case 0x0004: return "DOUBLE/FLOAT64";
        case 0x0005: return "BIGINT";
        case 0x0100: return "CHAR";
        case 0x0101: return "VARCHAR";
        case 0x0102: return "LONG VARCHAR";
        case 0x0105: return "DATE";
        case 0x0106: return "TIME";
        case 0x0107: return "TIMESTAMP";
        case 0x0108: return "BLOB";
        case 0x0109: return "CLOB";
        default:     return "OTHER";
    }
}

/* Abre o container, lê o Container Tag (0x200) e detecta automaticamente o Extent Size */
static bool open_container(db2_container_t *ctx, const char *path) {
    memset(ctx, 0, sizeof(*ctx));
    snprintf(ctx->filepath, sizeof(ctx->filepath), "%s", path);

    ctx->fp = fopen(path, "rb");
    if (!ctx->fp) {
        fprintf(stderr, "Erro ao abrir '%s': %s\n", path, strerror(errno));
        return false;
    }

    fseek(ctx->fp, 0, SEEK_END);
    ctx->file_size = (uint64_t)ftell(ctx->fp);
    ctx->total_file_pages = (uint32_t)(ctx->file_size / DB2_PAGE_SIZE);

    /* Detecta onde começa a Pool Page 0 (varrendo páginas 1..256) */
    uint8_t page[DB2_PAGE_SIZE];
    for (uint32_t p = 1; p <= 256 && p < ctx->total_file_pages; p++) {
        if (!read_file_page(ctx, p, page)) break;
        const db2_bps_header_t *bps = (const db2_bps_header_t *)page;
        if (bps->page_data_offset == BPS_HEADER_SIZE &&
            bps->page_data_length == (DB2_PAGE_SIZE - BPS_HEADER_SIZE) &&
            bps->pool_page_num == 0 &&
            bps->object_id == 0xFFFE &&
            bps->obj_type == 0x0E) {
            ctx->tag_pages     = p;
            ctx->extent_size   = *(const uint32_t *)(page + 0x34);
            if (ctx->extent_size == 0) ctx->extent_size = p;
            ctx->tbspace_id    = bps->tbspace_id;
            ctx->useable_pages = ctx->total_file_pages - ctx->tag_pages;
            return true;
        }
    }

    fprintf(stderr, "Não foi possível localizar a Pool Page 0 (SMP) em '%s'.\n", path);
    fclose(ctx->fp);
    ctx->fp = NULL;
    return false;
}

/* 1. Exibe informações do Container Tag (Offset 0x200) */
static void dump_container_tag(db2_container_t *ctx) {
    uint8_t page0[DB2_PAGE_SIZE];
    if (!read_file_page(ctx, 0, page0)) return;

    const uint8_t *tag = page0 + 0x200;
    char magic[9] = {0};
    memcpy(magic, tag, 8);

    uint32_t tag_tbsp_id   = *(const uint32_t *)(tag + 0x10);
    char     dbname[9]     = {0};
    char     instname[16]  = {0};
    char     dbpath[128]   = {0};

    memcpy(dbname,   tag + 0x26, 8);
    memcpy(instname, tag + 0x2F, 15);
    memcpy(dbpath,   tag + 0x42, 120);

    printf("================================================================================\n");
    printf(" 1. CONTAINER TAG & GEOMETRIA FÍSICA (%s)\n", ctx->filepath);
    printf("================================================================================\n");
    printf("  Assinatura (0x200)   : \"%s\"\n", magic);
    printf("  Banco / Instância    : %s / %s\n", dbname, instname);
    printf("  Caminho de Controle  : %s\n", dbpath);
    printf("  Tablespace ID        : %u (Tag: %u)\n", ctx->tbspace_id, tag_tbsp_id);
    printf("  Tamanho do Arquivo   : %lu bytes (%.2f MB)\n",
           (unsigned long)ctx->file_size, (double)ctx->file_size / (1024.0 * 1024.0));
    printf("  Tamanho da Página    : %u bytes\n", DB2_PAGE_SIZE);
    printf("  Tamanho do Extent    : %u páginas (%u KB)\n",
           ctx->extent_size, (ctx->extent_size * DB2_PAGE_SIZE) / 1024);
    printf("  Container Tag (Ext 0): %u páginas (%u KB reservados no início do arquivo)\n",
           ctx->tag_pages, (ctx->tag_pages * DB2_PAGE_SIZE) / 1024);
    printf("  Total de Páginas     : %u (Usáveis na Tablespace: %u = %u Extents)\n\n",
           ctx->total_file_pages, ctx->useable_pages, ctx->useable_pages / ctx->extent_size);
}

/* 2. Decodifica a Space Map Page (SMP) e o mapa de Extents (estilo db2dart /DHWM) */
static void dump_smp_and_extents(db2_container_t *ctx) {
    uint8_t meta_smp[DB2_PAGE_SIZE];
    uint8_t smp_page[DB2_PAGE_SIZE];

    if (!read_pool_page(ctx, 0, meta_smp)) return;
    uint32_t active_smp_pool_pg = *(const uint32_t *)(meta_smp + 0x34);
    if (!read_pool_page(ctx, active_smp_pool_pg, smp_page)) return;

    uint32_t last_obj_id  = *(const uint32_t *)(smp_page + 0x38);
    uint32_t hwm_extents  = *(const uint32_t *)(smp_page + 0x3C);
    uint32_t hwm_pages    = hwm_extents * ctx->extent_size;

    printf("================================================================================\n");
    printf(" 2. SPACE MAP PAGE (SMP) & MAPA DE ALOCAÇÃO DE EXTENTS\n");
    printf("================================================================================\n");
    printf("  Meta-SMP             : Pool Page 0 (File Page %u, Offset 0x%06X)\n",
           ctx->tag_pages, ctx->tag_pages * DB2_PAGE_SIZE);
    printf("  SMP Ativa            : Pool Page %u (File Page %u, Offset 0x%06X)\n",
           active_smp_pool_pg, active_smp_pool_pg + ctx->tag_pages,
           (active_smp_pool_pg + ctx->tag_pages) * DB2_PAGE_SIZE);
    printf("  Último Object ID     : %u\n", last_obj_id);
    printf("  High Water Mark (HWM): %u extents (%u páginas = %.2f MB)\n\n",
           hwm_extents, hwm_pages, (double)(hwm_pages * DB2_PAGE_SIZE) / (1024.0 * 1024.0));

    printf("  [Ext#] PoolPg..Fim   FilePg   SMP_Bit  ObjID   TipoHex  Descrição do Extent\n");
    printf("  ------ ------------- -------- -------- ------- -------- ------------------------\n");

    const uint32_t *bitmap_words = (const uint32_t *)(smp_page + 0x58);
    uint32_t max_ext = hwm_extents;
    if (max_ext > 80) max_ext = 80;

    for (uint32_t ext = 0; ext < max_ext; ext++) {
        uint32_t word = bitmap_words[ext / 8];
        uint8_t  nibble = (word >> ((7 - (ext % 8)) * 4)) & 0x0F;

        uint32_t pool_pg = ext * ctx->extent_size;
        uint32_t file_pg = pool_pg + ctx->tag_pages;

        uint8_t ext_first_pg[DB2_PAGE_SIZE];
        if (!read_pool_page(ctx, pool_pg, ext_first_pg)) break;
        const db2_bps_header_t *bps = (const db2_bps_header_t *)ext_first_pg;

        printf("  [%04u] %6u..%-6u %6u   0x%x(%s) %5u   0x%02x%s   %s\n",
               ext,
               pool_pg, pool_pg + ctx->extent_size - 1,
               file_pg,
               nibble, (nibble == 1) ? "USE" : (nibble == 0 ? "FRE" : "PND"),
               bps->object_id,
               bps->obj_type,
               (bps->obj_page_num == 0) ? "*" : " ",
               obj_type_name(bps->obj_type, bps->object_id));
    }
    if (hwm_extents > max_ext) {
        printf("  ... (%u extents adicionais omitidos na listagem)\n", hwm_extents - max_ext);
    }
    printf("\n");
}

/* Lê o array de extents de uma página EMP (Extent Map Page) */
static uint32_t read_emp_extents(db2_container_t *ctx, uint32_t emp_pool_page,
                                 uint32_t *out_extents, uint32_t max_extents) {
    if (emp_pool_page == 0) return 0;
    uint8_t emp_buf[DB2_PAGE_SIZE];
    if (!read_pool_page(ctx, emp_pool_page, emp_buf)) return 0;

    const db2_bps_header_t *bps = (const db2_bps_header_t *)emp_buf;
    if ((bps->obj_type & 0x40) == 0) return 0;

    uint32_t num_ext = *(const uint32_t *)(emp_buf + 0x3C);
    if (num_ext > max_extents) num_ext = max_extents;

    const uint32_t *arr = (const uint32_t *)(emp_buf + 0x58);
    for (uint32_t i = 0; i < num_ext; i++) {
        out_extents[i] = arr[i];
    }
    return num_ext;
}

/* Busca na Object Table (0xFFFF) os EMPs de Dados e Índice para um Object ID */
static bool find_object_emps(db2_container_t *ctx, uint16_t target_obj_id,
                             uint32_t *out_data_emp, uint32_t *out_idx_emp) {
    *out_data_emp = 0;
    *out_idx_emp  = 0;

    uint32_t ot_extents[64];
    uint32_t num_ot_ext = read_emp_extents(ctx, 1, ot_extents, 64);
    if (num_ot_ext == 0) return false;

    uint8_t ot_pg0[DB2_PAGE_SIZE];
    if (!read_pool_page(ctx, ot_extents[0], ot_pg0)) return false;
    uint16_t ot_s0 = *(const uint16_t *)(ot_pg0 + SLOT_DIR_OFFSET);
    uint32_t ot_page_count = *(const uint32_t *)(ot_pg0 + SLOT_DIR_OFFSET + ot_s0 + 4);

    uint32_t pages_seen = 0;
    for (uint32_t e = 0; e < num_ot_ext && pages_seen < ot_page_count; e++) {
        for (uint32_t p = 0; p < ctx->extent_size && pages_seen < ot_page_count; p++) {
            uint8_t page[DB2_PAGE_SIZE];
            if (!read_pool_page(ctx, ot_extents[e] + p, page)) break;
            const db2_bps_header_t *bps = (const db2_bps_header_t *)page;
            if (bps->object_id != 0xFFFF || bps->obj_type != 0x00) continue;
            pages_seen++;

            const db2_data_page_hdr_t *dhdr = (const db2_data_page_hdr_t *)(page + BPS_HEADER_SIZE);
            uint16_t start_slot = (bps->obj_page_num == 0) ? 4 : 0;
            for (uint16_t s = start_slot; s < dhdr->slot_count; s++) {
                uint16_t soff = *(const uint16_t *)(page + SLOT_DIR_OFFSET + s * 2);
                if (soff == 0xFFFF || soff == 0x0000) continue;
                const uint8_t *rec = page + SLOT_DIR_OFFSET + soff;
                if (*(const uint16_t *)rec != 0x0010) continue;
                uint16_t obj_id = *(const uint16_t *)(rec + 0x0A);
                if (obj_id == target_obj_id) {
                    *out_data_emp = *(const uint32_t *)(rec + 0x0C);
                    *out_idx_emp  = *(const uint32_t *)(rec + 0x30);
                    return true;
                }
            }
        }
    }
    return false;
}

/* 3. Decodifica a Object Table (Object ID 0xFFFF) via Pool Page 1 */
static void dump_object_table(db2_container_t *ctx) {
    uint32_t ot_extents[64];
    uint32_t num_ot_ext = read_emp_extents(ctx, 1, ot_extents, 64);
    if (num_ot_ext == 0) {
        printf("  [!] Pool Page 1 não contém um EMP válido para a Object Table.\n\n");
        return;
    }

    printf("================================================================================\n");
    printf(" 3. OBJECT TABLE (Object ID 0xFFFF — Catálogo Interno da Tablespace)\n");
    printf("================================================================================\n");
    printf("  EMP da Object Table  : Pool Page 1 -> %u extent(s), iniciando na Pool Page %u\n\n",
           num_ot_ext, ot_extents[0]);
    printf("  ObjID  TbspID  Data_EMP(PoolPg)  Data_Extents  Index_EMP(PoolPg)  Idx_Extents  LOB_EMP\n");
    printf("  -----  ------  ----------------  ------------  -----------------  -----------  -------\n");

    uint8_t pg0[DB2_PAGE_SIZE];
    if (!read_pool_page(ctx, ot_extents[0], pg0)) return;
    uint16_t slot0_off = *(const uint16_t *)(pg0 + SLOT_DIR_OFFSET + 0 * 2);
    uint32_t ot_page_count = *(const uint32_t *)(pg0 + SLOT_DIR_OFFSET + slot0_off + 4);

    uint32_t pages_seen = 0;
    for (uint32_t e = 0; e < num_ot_ext && pages_seen < ot_page_count; e++) {
        for (uint32_t p = 0; p < ctx->extent_size && pages_seen < ot_page_count; p++) {
            uint32_t cur_pool_pg = ot_extents[e] + p;
            uint8_t page[DB2_PAGE_SIZE];
            if (!read_pool_page(ctx, cur_pool_pg, page)) break;

            const db2_bps_header_t *bps = (const db2_bps_header_t *)page;
            if (bps->object_id != 0xFFFF || bps->obj_type != 0x00) continue;
            pages_seen++;

            const db2_data_page_hdr_t *dhdr = (const db2_data_page_hdr_t *)(page + BPS_HEADER_SIZE);
            uint16_t start_slot = (bps->obj_page_num == 0) ? 4 : 0;

            for (uint16_t s = start_slot; s < dhdr->slot_count; s++) {
                uint16_t soff = *(const uint16_t *)(page + SLOT_DIR_OFFSET + s * 2);
                if (soff == 0xFFFF || soff == 0x0000) continue;
                const uint8_t *rec = page + SLOT_DIR_OFFSET + soff;
                uint16_t rec_type = *(const uint16_t *)(rec + 0);
                uint16_t rec_len  = *(const uint16_t *)(rec + 2);
                if (rec_type != 0x0010 || rec_len < 0x70) continue;

                uint16_t tbsp_id     = *(const uint16_t *)(rec + 0x08);
                uint16_t obj_id      = *(const uint16_t *)(rec + 0x0A);
                uint32_t data_emp_pg = *(const uint32_t *)(rec + 0x0C);
                uint32_t idx_emp_pg  = *(const uint32_t *)(rec + 0x30);
                uint32_t lob_emp_pg  = *(const uint32_t *)(rec + 0x60);

                if (obj_id == 0 && data_emp_pg == 0 && idx_emp_pg == 0) continue;

                uint32_t tmp_ext[512];
                uint32_t d_ext = read_emp_extents(ctx, data_emp_pg, tmp_ext, 512);
                uint32_t i_ext = read_emp_extents(ctx, idx_emp_pg,  tmp_ext, 512);

                printf("  %5u  %6u  %16u  %12u  %17u  %11u  %7u\n",
                       obj_id, tbsp_id, data_emp_pg, d_ext, idx_emp_pg, i_ext, lob_emp_pg);
            }
        }
    }
    printf("\n");
}

/* Formata e imprime o valor de uma coluna a partir de um registro FIXEDVAR */
static void print_column_value(const uint8_t *rec, uint16_t rec_len,
                               const db2_col_desc_t *col) {
    const uint8_t *fixed_base = rec + 8;
    const uint8_t *val_ptr    = fixed_base + col->fixed_offset;
    bool nullable = (col->null_flags == 0x0001);

    uint16_t fixed_width = col->col_len;
    if (col->col_type == 0x0101) fixed_width = 4;
    else if (col->col_type == 0x0107) fixed_width = 10;
    else if (col->col_type == 0x0108 || col->col_type == 0x0109) fixed_width = 4;

    if (nullable) {
        uint8_t null_byte = val_ptr[fixed_width];
        if (null_byte != 0x00) {
            printf("NULL");
            return;
        }
    }

    switch (col->col_type) {
        case 0x0000: {
            int16_t v = *(const int16_t *)val_ptr;
            printf("%d", v);
            break;
        }
        case 0x0001: {
            int32_t v = *(const int32_t *)val_ptr;
            printf("%d", v);
            break;
        }
        case 0x0005: {
            int64_t v = *(const int64_t *)val_ptr;
            printf("%lld", (long long)v);
            break;
        }
        case 0x0100: {
            int len = col->col_len;
            while (len > 0 && val_ptr[len - 1] == ' ') len--;
            printf("'%.*s'", len, (const char *)val_ptr);
            break;
        }
        case 0x0101: {
            uint16_t var_off = *(const uint16_t *)(val_ptr + 0);
            uint16_t var_len = *(const uint16_t *)(val_ptr + 2);
            if (8 + var_off + var_len <= rec_len) {
                printf("'%.*s'", (int)var_len, (const char *)(fixed_base + var_off));
            } else {
                printf("<varchar off=%u len=%u>", var_off, var_len);
            }
            break;
        }
        case 0x0105: {
            printf("%02X%02X-%02X-%02X", val_ptr[0], val_ptr[1], val_ptr[2], val_ptr[3]);
            break;
        }
        case 0x0107: {
            printf("%02X%02X-%02X-%02X %02X:%02X:%02X.%02X%02X%02X",
                   val_ptr[0], val_ptr[1], val_ptr[2], val_ptr[3],
                   val_ptr[4], val_ptr[5], val_ptr[6],
                   val_ptr[7], val_ptr[8], val_ptr[9]);
            break;
        }
        case 0x0108:
        case 0x0109: {
            uint16_t lob_off = *(const uint16_t *)(val_ptr + 0);
            uint16_t lob_len = *(const uint16_t *)(val_ptr + 2);
            printf("<LOB desc_off=%u desc_len=%u>", lob_off, lob_len);
            break;
        }
        default:
            printf("<hex:");
            for (int i = 0; i < fixed_width && i < 8; i++) printf("%02X", val_ptr[i]);
            printf(">");
            break;
    }
}

/* Imprime uma chave de índice B+Tree com base nos descritores de Key Parts */
static uint16_t print_index_key_parts(const uint8_t *key_bytes, uint16_t num_parts,
                                      const db2_idx_keypart_t *parts) {
    uint16_t off = 0;
    for (uint16_t i = 0; i < num_parts; i++) {
        if (i > 0) printf(", ");
        const uint8_t *p = key_bytes + off;
        uint16_t len = parts[i].col_len;
        switch (parts[i].col_type) {
            case 0x0000:
                printf("%d", *(const int16_t *)p);
                break;
            case 0x0001:
                printf("%d", *(const int32_t *)p);
                break;
            case 0x0005:
                printf("%lld", (long long)*(const int64_t *)p);
                break;
            case 0x0100: {
                int slen = len;
                while (slen > 0 && p[slen - 1] == ' ') slen--;
                printf("'%.*s'", slen, (const char *)p);
                break;
            }
            default:
                printf("0x");
                for (uint16_t b = 0; b < len; b++) printf("%02X", p[b]);
                break;
        }
        off += len;
    }
    return off;
}

/* 5. Inspeciona o Objeto de Índice B+Tree (Type = 0x01) associado a um Object ID */
static void inspect_index_object(db2_container_t *ctx, uint16_t target_obj_id,
                                 uint32_t idx_emp_pg, uint32_t max_keys_to_print) {
    if (idx_emp_pg == 0) return;

    uint32_t idx_extents[512];
    uint32_t num_idx_ext = read_emp_extents(ctx, idx_emp_pg, idx_extents, 512);
    if (num_idx_ext == 0) return;

    /* 5.1 Lê a Página 0 do Objeto de Índice (Free Page Map / Index SMP) */
    uint8_t page0[DB2_PAGE_SIZE];
    uint32_t pg0_pool = 0;
    if (!read_object_logical_page(ctx, idx_extents, num_idx_ext, 0, page0, &pg0_pool)) return;

    uint32_t idx_alloc_pages = *(const uint32_t *)(page0 + 0x38);
    uint32_t idx_used_pages  = *(const uint32_t *)(page0 + 0x3C);
    uint64_t idx_pg0_lsn     = *(const uint64_t *)(page0 + 0x48);

    printf("  ==============================================================================\n");
    printf("   5. OBJETO DE ÍNDICE B+TREE (Object ID %u, Type 0x01)\n", target_obj_id);
    printf("  ==============================================================================\n");
    printf("  Index EMP Pool Page  : %u (File Page %u) -> %u Extent(s) de Índice\n",
           idx_emp_pg, idx_emp_pg + ctx->tag_pages, num_idx_ext);
    printf("  Index SMP (ObjPg 0)  : Pool Page %u (File Page %u) | LSN: 0x%016llX\n",
           pg0_pool, pg0_pool + ctx->tag_pages, (unsigned long long)idx_pg0_lsn);
    printf("  Páginas no Índice    : %u alocadas / %u em uso (Mapa ObjPg 1..%u: ",
           idx_alloc_pages, idx_used_pages, idx_alloc_pages > 32 ? 32 : idx_alloc_pages);
    for (uint32_t i = 0; i < idx_alloc_pages && i < 32; i++) {
        uint8_t st = page0[0x80 + i];
        putchar((st == 0x10) ? 'U' : (st == 0x00 ? '.' : '?'));
    }
    if (idx_alloc_pages > 32) printf("...");
    printf(")\n\n");

    /* 5.2 Lê a Página 1 do Objeto de Índice (MetaIndex Root Page) */
    uint8_t meta_pg[DB2_PAGE_SIZE];
    uint32_t meta_pool = 0;
    if (!read_object_logical_page(ctx, idx_extents, num_idx_ext, 1, meta_pg, &meta_pool)) return;

    uint16_t m_rv_len   = (memcmp(meta_pg + 0x56, "RT10", 4) == 0) ? *(const uint16_t *)(meta_pg + 0x54) : 0;
    uint16_t m_dir_base = 0x54 + m_rv_len;
    uint16_t m_num_keys = *(const uint16_t *)(meta_pg + m_dir_base + 0x06);
    const uint16_t *m_koffs = (const uint16_t *)(meta_pg + m_dir_base + 0x0C);

    printf("  --- MetaIndex (ObjPg 1, PoolPg %u): %u Índice(s) registrado(s) ---\n",
           meta_pool, m_num_keys);

    for (uint16_t idx_i = 0; idx_i < m_num_keys; idx_i++) {
        uint16_t koff = m_koffs[idx_i];
        const uint8_t *mrec = meta_pg + koff;
        int16_t  meta_kp1   = *(const int16_t *)(mrec + 2);
        int16_t  index_id   = *(const int16_t *)(mrec + 4);
        uint16_t root_obj_pg = *(const uint16_t *)(mrec + 6);

        printf("    [MetaKey %u] (KeyPart1=%d, IndexID=%d) ==> Root Index Page = ObjPg %u\n\n",
               idx_i, meta_kp1, index_id, root_obj_pg);

        /* 5.3 Lê a Página Raiz do Índice (ex: ObjPg 2) */
        uint8_t root_pg[DB2_PAGE_SIZE];
        uint32_t root_pool = 0;
        if (!read_object_logical_page(ctx, idx_extents, num_idx_ext, root_obj_pg, root_pg, &root_pool)) {
            continue;
        }

        const db2_idx_node_hdr_t *rhdr = (const db2_idx_node_hdr_t *)(root_pg + BPS_HEADER_SIZE);
        bool is_large_rid  = (rhdr->page_state & 0x40000000) != 0;
        bool is_leaf_root  = (rhdr->page_state & 0x04) != 0;
        bool has_root_vec  = (memcmp(root_pg + 0x56, "RT10", 4) == 0);

        uint16_t rv_len       = has_root_vec ? *(const uint16_t *)(root_pg + 0x54) : 0;
        uint16_t min_key_sz   = has_root_vec ? *(const uint16_t *)(root_pg + 0x5C) : 0;
        uint16_t key_len      = has_root_vec ? *(const uint16_t *)(root_pg + 0x5E) : 0;
        uint32_t info_pg_id   = has_root_vec ? *(const uint32_t *)(root_pg + 0x60) : 0;
        uint16_t leaf_pctfree = has_root_vec ? *(const uint16_t *)(root_pg + 0x66) : 0;
        uint16_t num_kparts   = has_root_vec ? *(const uint16_t *)(root_pg + 0x9C) : 0;
        uint16_t num_uparts   = has_root_vec ? *(const uint16_t *)(root_pg + 0x9E) : 0;
        const db2_idx_keypart_t *kparts = (const db2_idx_keypart_t *)(root_pg + 0xA0);

        uint16_t r_dir_base = 0x54 + rv_len;
        uint16_t r_num_keys = *(const uint16_t *)(root_pg + r_dir_base + 0x06);
        const uint16_t *r_koffs = (const uint16_t *)(root_pg + r_dir_base + 0x0C);

        printf("  --- Árvore B+ do Índice ID %d (Root=ObjPg %u, PoolPg %u, FilePg %u) ---\n",
               index_id, root_obj_pg, root_pool, root_pool + ctx->tag_pages);
        printf("  Tipo do Nó Raiz      : %s | %s (State=0x%08X)\n",
               is_leaf_root ? "Root/Leaf Node (1 nível)" : "Root Internal Node (Multi-nível)",
               is_large_rid ? "Large RID (6B)" : "Standard RID (4B)",
               rhdr->page_state);
        printf("  Altura da Árvore B+  : %u nível(is) | Slots=%u | FreeSpace=%u | Keys na Raiz=%u\n",
               rhdr->tree_level, rhdr->slot_count, rhdr->total_free_space, r_num_keys);
        printf("  Index Root Vector    : Assinatura=\"%.4s\" | KeyLen=%uB | MinKeyRec=%uB | PctFree=%u%% | InfoPage=ObjPg %u\n",
               (const char *)(root_pg + 0x56), key_len, min_key_sz, leaf_pctfree, info_pg_id);

        /* Lê a Info Page (se existir, ex: ObjPg 3) para exibir estatísticas físicas do índice */
        if (info_pg_id > 0) {
            uint8_t info_pg[DB2_PAGE_SIZE];
            uint32_t info_pool = 0;
            if (read_object_logical_page(ctx, idx_extents, num_idx_ext, info_pg_id, info_pg, &info_pool)) {
                uint16_t i_koff = *(const uint16_t *)(info_pg + 0x54 + 0x0C);
                if (i_koff > 0 && i_koff + 24 < DB2_PAGE_SIZE) {
                    const uint8_t *irec = info_pg + i_koff;
                    uint64_t full_key_card = *(const uint64_t *)(irec + 10);
                    uint32_t num_leaf_pgs  = *(const uint32_t *)(irec + 18);
                    printf("  Estatísticas (InfoPg): FullKeyCard=%llu | LeafPages=%u (em ObjPg %u / PoolPg %u)\n",
                           (unsigned long long)full_key_card, num_leaf_pgs, info_pg_id, info_pool);
                }
            }
        }

        printf("  Colunas da Chave (%u) : ", num_kparts);
        for (uint16_t kp = 0; kp < num_kparts; kp++) {
            if (kp > 0) printf(" + ");
            printf("[Part %u: ColIdx=%u %s(%uB) %s]",
                   kp + 1, kparts[kp].dms_field_id,
                   col_type_name(kparts[kp].col_type), kparts[kp].col_len,
                   (kparts[kp].null_flags == 0x0002) ? "NOT NULL" : "NULLABLE");
        }
        printf(" (UniqParts=%u)\n", num_uparts);

        /* Se a raiz for um Nó Interno (Tree Level > 1), exibe as Hi Keys da raiz e desce até a primeira folha */
        uint32_t cur_obj_pg  = root_obj_pg;
        uint32_t cur_level   = rhdr->tree_level;
        uint8_t  cur_pg[DB2_PAGE_SIZE];
        memcpy(cur_pg, root_pg, DB2_PAGE_SIZE);

        if (cur_level > 1) {
            printf("\n  [Nó Raiz Interno — Level %u (ObjPg %u)]:\n", cur_level, cur_obj_pg);
            for (uint16_t k = 0; k < r_num_keys && k < 10; k++) {
                const uint8_t *krec = root_pg + r_koffs[k];
                uint16_t krec_len   = *(const uint16_t *)krec;
                uint32_t child_pg   = *(const uint32_t *)(krec + krec_len - 4);
                if (krec_len == 8) {
                    printf("    Dummy Hi Key %u [Off=0x%03X] -> Child Pointer => ObjPg %u\n",
                           k, r_koffs[k], child_pg);
                } else {
                    printf("    Hi Key %u       [Off=0x%03X] Key=(", k, r_koffs[k]);
                    print_index_key_parts(krec + 2, num_kparts, kparts);
                    printf(") -> Child Pointer => ObjPg %u\n", child_pg);
                }
            }

            /* Desce pelo primeiro ponteiro filho de cada nível até atingir o nível folha (Level 1) */
            printf("  Descendo pela B+Tree : ObjPg %u (L%u)", cur_obj_pg, cur_level);
            while (cur_level > 1) {
                uint16_t dir_off = 0x54 + ((memcmp(cur_pg + 0x56, "RT10", 4) == 0) ? *(const uint16_t *)(cur_pg + 0x54) : 0);
                uint16_t koff0   = *(const uint16_t *)(cur_pg + dir_off + 0x0C);
                const uint8_t *krec0 = cur_pg + koff0;
                uint16_t klen0   = *(const uint16_t *)krec0;
                uint32_t child_pg = *(const uint32_t *)(krec0 + klen0 - 4);

                uint32_t child_pool = 0;
                if (!read_object_logical_page(ctx, idx_extents, num_idx_ext, child_pg, cur_pg, &child_pool)) {
                    break;
                }
                const db2_idx_node_hdr_t *chdr = (const db2_idx_node_hdr_t *)(cur_pg + BPS_HEADER_SIZE);
                cur_obj_pg = child_pg;
                cur_level  = chdr->tree_level;
                printf(" --> ObjPg %u [PoolPg %u] (L%u)", cur_obj_pg, child_pool, cur_level);
            }
            printf(" (Primeira Folha!)\n");
        }

        /* 5.4 Percorre a cadeia duplamente encadeada de Páginas Folha (Leaf Chain: prev_node <-> next_node) */
        printf("\n  --- Chaves Físicas nas Páginas Folha (Primeiras %u chaves) ---\n", max_keys_to_print);
        uint64_t total_leaf_keys  = 0;
        uint32_t total_leaf_pages = 0;
        uint32_t leaf_obj_pg      = cur_obj_pg;

        while (leaf_obj_pg != 0 && total_leaf_pages < 10000) {
            uint8_t leaf_pg[DB2_PAGE_SIZE];
            uint32_t leaf_pool = 0;
            if (!read_object_logical_page(ctx, idx_extents, num_idx_ext, leaf_obj_pg, leaf_pg, &leaf_pool)) {
                break;
            }
            const db2_idx_node_hdr_t *lhdr = (const db2_idx_node_hdr_t *)(leaf_pg + BPS_HEADER_SIZE);
            uint16_t l_rv_len   = (memcmp(leaf_pg + 0x56, "RT10", 4) == 0) ? *(const uint16_t *)(leaf_pg + 0x54) : 0;
            uint16_t l_dir_base = 0x54 + l_rv_len;
            uint16_t l_num_keys = *(const uint16_t *)(leaf_pg + l_dir_base + 0x06);
            const uint16_t *l_koffs = (const uint16_t *)(leaf_pg + l_dir_base + 0x0C);

            total_leaf_pages++;

            for (uint16_t k = 0; k < l_num_keys; k++) {
                uint16_t koff = l_koffs[k];
                const uint8_t *krec = leaf_pg + koff;
                uint16_t krec_len   = *(const uint16_t *)krec;

                total_leaf_keys++;
                if (total_leaf_keys <= max_keys_to_print) {
                    printf("    Key #%llu [LeafObjPg=%3u PoolPg=%4u Off=0x%03X Len=%2u]: Chave=(",
                           (unsigned long long)total_leaf_keys, leaf_obj_pg, leaf_pool, koff, krec_len);
                    uint16_t key_bytes_used = print_index_key_parts(krec + 2, num_kparts, kparts);
                    const uint8_t *rid_ptr = krec + 2 + key_bytes_used;

                    if (is_large_rid) {
                        uint16_t rid_slot  = *(const uint16_t *)(rid_ptr + 0);
                        uint32_t rid_page  = *(const uint32_t *)(rid_ptr + 2);
                        uint8_t  rid_flags = *(const uint8_t  *)(rid_ptr + 6);
                        printf(") ==> Table RID(Page=%u : Slot=%u) [flags=0x%02X]\n",
                               rid_page, rid_slot, rid_flags);
                    } else {
                        uint16_t rid_slot  = *(const uint16_t *)(rid_ptr + 0);
                        uint16_t rid_page  = *(const uint16_t *)(rid_ptr + 2);
                        uint8_t  rid_flags = *(const uint8_t  *)(rid_ptr + 4);
                        printf(") ==> Table RID(Page=%u : Slot=%u) [flags=0x%02X]\n",
                               rid_page, rid_slot, rid_flags);
                    }
                }
            }

            leaf_obj_pg = lhdr->next_node;
        }

        printf("\n  >> Travessia da Leaf Chain concluída: %u página(s) folha | %llu chave(s) de índice lidas!\n\n",
               total_leaf_pages, (unsigned long long)total_leaf_keys);
    }
}

/* 4. Inspeciona um Objeto de Dados específico (Schema físico Slot 3 + Registros + Índice B+Tree) */
static void inspect_data_object(db2_container_t *ctx, uint16_t target_obj_id, uint32_t max_rows_to_print) {
    uint32_t data_emp_pg = 0;
    uint32_t idx_emp_pg  = 0;

    if (!find_object_emps(ctx, target_obj_id, &data_emp_pg, &idx_emp_pg) || data_emp_pg == 0) {
        printf("  [!] Objeto ID %u não encontrado ou sem Data EMP neste container.\n\n", target_obj_id);
        return;
    }

    uint32_t data_extents[1024];
    uint32_t num_data_ext = read_emp_extents(ctx, data_emp_pg, data_extents, 1024);
    if (num_data_ext == 0) return;

    uint8_t page0[DB2_PAGE_SIZE];
    if (!read_pool_page(ctx, data_extents[0], page0)) return;

    const db2_bps_header_t      *bps0  = (const db2_bps_header_t *)page0;
    const db2_data_page_hdr_t   *dhdr0 = (const db2_data_page_hdr_t *)(page0 + BPS_HEADER_SIZE);
    const uint16_t              *sdir0 = (const uint16_t *)(page0 + SLOT_DIR_OFFSET);

    const uint8_t *slot0_rec = page0 + SLOT_DIR_OFFSET + sdir0[0];
    uint32_t obj_total_pages = *(const uint32_t *)(slot0_rec + 4);
    uint64_t obj_create_lsn  = *(const uint64_t *)(slot0_rec + 8);

    const uint8_t *slot3_rec = page0 + SLOT_DIR_OFFSET + sdir0[3];
    uint16_t num_cols        = *(const uint16_t *)(slot3_rec + 6);
    const db2_col_desc_t *cols = (const db2_col_desc_t *)(slot3_rec + 8);

    printf("================================================================================\n");
    printf(" 4. INSPEÇÃO PROFUNDA DO OBJETO ID %u (Tablespace %u)\n", target_obj_id, ctx->tbspace_id);
    printf("================================================================================\n");
    printf("  Data EMP Pool Page   : %u (File Page %u) -> %u Extent(s) alocados\n",
           data_emp_pg, data_emp_pg + ctx->tag_pages, num_data_ext);
    printf("  Index EMP Pool Page  : %u\n", idx_emp_pg);
    printf("  Lista de Extents     : ");
    for (uint32_t i = 0; i < num_data_ext && i < 12; i++) {
        printf("[Ext %u: PoolPg %u] ", i, data_extents[i]);
    }
    if (num_data_ext > 12) printf("... (+%u extents)", num_data_ext - 12);
    printf("\n");
    printf("  Página 0 do Objeto   : Pool Page %u (File Page %u, Offset 0x%06X)\n",
           bps0->pool_page_num, bps0->pool_page_num + ctx->tag_pages,
           (bps0->pool_page_num + ctx->tag_pages) * DB2_PAGE_SIZE);
    printf("  Page LSN (Página 0)  : 0x%016llX (Create LSN: 0x%016llX)\n",
           (unsigned long long)bps0->page_lsn, (unsigned long long)obj_create_lsn);
    printf("  Páginas Usadas (Slot0): %u página(s) de dados\n", obj_total_pages);
    printf("  Header Página 0      : SlotCount=%u, FreeSpace=%u, FreeSpaceOffset=%u, MaxRecSize=%u\n\n",
           dhdr0->slot_count, dhdr0->total_free_space, dhdr0->free_space_offset, dhdr0->max_record_size);

    printf("  --- Descritor Físico de Colunas (Slot 3 em Page0+0x%03X, %u colunas) ---\n",
           SLOT_DIR_OFFSET + sdir0[3], num_cols);
    printf("  Col#  TipoHex  Tipo_SQL       Tam/Max  Null?     FixedOffset\n");
    printf("  ----  -------  -------------  -------  --------  -----------\n");
    for (uint16_t c = 0; c < num_cols; c++) {
        printf("  %4u  0x%04X   %-13s  %7u  %-8s  +%u bytes\n",
               c + 1,
               cols[c].col_type,
               col_type_name(cols[c].col_type),
               cols[c].col_len,
               (cols[c].null_flags == 0x0001) ? "NULLABLE" : "NOT NULL",
               cols[c].fixed_offset);
    }
    printf("\n");

    printf("  --- Registros Físicos de Dados (Primeiros %u registros) ---\n", max_rows_to_print);
    uint64_t total_rows = 0;
    uint32_t obj_pg_seen = 0;

    for (uint32_t e = 0; e < num_data_ext && obj_pg_seen < obj_total_pages; e++) {
        for (uint32_t p = 0; p < ctx->extent_size && obj_pg_seen < obj_total_pages; p++) {
            uint32_t pool_pg = data_extents[e] + p;
            uint8_t page[DB2_PAGE_SIZE];
            if (!read_pool_page(ctx, pool_pg, page)) break;

            const db2_bps_header_t *bps = (const db2_bps_header_t *)page;
            if (bps->object_id != target_obj_id || bps->obj_type != 0x00) continue;

            obj_pg_seen++;
            const db2_data_page_hdr_t *dhdr = (const db2_data_page_hdr_t *)(page + BPS_HEADER_SIZE);
            const uint16_t *sdir = (const uint16_t *)(page + SLOT_DIR_OFFSET);

            uint16_t start_slot = (bps->obj_page_num == 0) ? 4 : 0;

            for (uint16_t s = start_slot; s < dhdr->slot_count; s++) {
                uint16_t soff = sdir[s];
                if (soff == 0xFFFF || soff == 0x0000 || (SLOT_DIR_OFFSET + soff >= DB2_PAGE_SIZE)) {
                    continue;
                }
                const uint8_t *rec = page + SLOT_DIR_OFFSET + soff;
                uint16_t rec_type  = *(const uint16_t *)(rec + 0);
                uint16_t rec_len   = *(const uint16_t *)(rec + 2);
                uint16_t rec_flags = *(const uint16_t *)(rec + 4);

                /*
                 * Os 3 bits menos significativos de rec_type (rec_type & 0x07) indicam o tipo base:
                 *  - 0x00 : Normal Data Record (FIXEDVAR; bit 0x10 indica presença de múltiplos LOBs)
                 *  - 0x04 : Overflow Target Data Record (ex: 0x00C4 quando um UPDATE moveu a linha)
                 *  - 0x01 : Internal Control Record (Header / FSCR)
                 *  - 0x02 : Overflow Pointer Record (12B apontando para a nova página/slot)
                 */
                uint16_t base_type = rec_type & 0x0007;
                if ((base_type != 0x0000 && base_type != 0x0004) || (rec_flags & 0x0001) == 0) continue;

                total_rows++;
                if (total_rows <= max_rows_to_print) {
                    printf("  RID(Pg=%4u:Slot=%2u) [FilePg=%4u Off=0x%03X Len=%3u]: ( ",
                           bps->obj_page_num, s, pool_pg + ctx->tag_pages,
                           SLOT_DIR_OFFSET + soff, rec_len);
                    for (uint16_t c = 0; c < num_cols && c < 12; c++) {
                        if (c > 0) printf(" | ");
                        print_column_value(rec, rec_len, &cols[c]);
                    }
                    if (num_cols > 12) printf(" | ... +%u cols", num_cols - 12);
                    printf(" )\n");
                }
            }
        }
    }
    printf("\n  >> Total de páginas de dados lidas: %u | Total de registros (CARD) encontrados no disco: %llu\n\n",
           obj_pg_seen, (unsigned long long)total_rows);

    /* Se o objeto tiver um Index EMP associado, inspeciona também sua árvore B+ de índices! */
    if (idx_emp_pg != 0) {
        inspect_index_object(ctx, target_obj_id, idx_emp_pg, max_rows_to_print);
    }
}

/* 6. Decodifica o arquivo binário de controle de Tablespaces (SQLSPCS.1 / SQLSPCS.2) */
static int inspect_sqlspcs_file(const char *path) {
    FILE *fp = fopen(path, "rb");
    if (!fp) {
        fprintf(stderr, "Erro ao abrir '%s': %s\n", path, strerror(errno));
        return 1;
    }

    fseek(fp, 0, SEEK_END);
    uint64_t fsize = (uint64_t)ftell(fp);
    uint32_t num_slots = (uint32_t)(fsize / SQLSPCS_SLOT_SIZE);

    printf("================================================================================\n");
    printf(" ENGENHARIA REVERSA DO ARQUIVO DE CONTROLE DE TABLESPACES (%s)\n", path);
    printf("================================================================================\n");
    printf("  Tamanho do Arquivo   : %lu bytes (%.2f MB)\n",
           (unsigned long)fsize, (double)fsize / (1024.0 * 1024.0));
    printf("  Tamanho do Slot Fixo : %u bytes (0x%X = 256 KB por Tablespace)\n",
           SQLSPCS_SLOT_SIZE, SQLSPCS_SLOT_SIZE);
    printf("  Total de Slots       : %u slots (0..%u)\n\n", num_slots, num_slots - 1);

    uint8_t *slot_buf = (uint8_t *)malloc(SQLSPCS_SLOT_SIZE);
    if (!slot_buf) {
        fclose(fp);
        return 1;
    }

    for (uint32_t i = 0; i < num_slots; i++) {
        fseek(fp, (long)(i * SQLSPCS_SLOT_SIZE), SEEK_SET);
        if (fread(slot_buf, 1, SQLSPCS_SLOT_SIZE, fp) != SQLSPCS_SLOT_SIZE) break;

        uint32_t version       = *(const uint32_t *)(slot_buf + 0x0000);
        uint32_t checksum      = *(const uint32_t *)(slot_buf + 0x0004);
        int16_t  tbsp_id       = *(const int16_t  *)(slot_buf + 0x0008);
        const char *tbsp_name  = (const char *)(slot_buf + 0x000A);
        uint16_t tbsp_flags    = *(const uint16_t *)(slot_buf + 0x008C);
        uint32_t extent_size   = *(const uint32_t *)(slot_buf + 0x0098);
        uint32_t prefetch_size = *(const uint32_t *)(slot_buf + 0x009C);
        uint64_t create_lsn    = *(const uint64_t *)(slot_buf + 0x03E0);
        uint32_t total_pages   = *(const uint32_t *)(slot_buf + 0x0430);
        uint32_t useable_pages = *(const uint32_t *)(slot_buf + 0x0434);
        uint32_t num_conts     = *(const uint32_t *)(slot_buf + 0x0448);
        uint32_t bp_id         = *(const uint32_t *)(slot_buf + 0x0458);
        uint32_t page_size     = *(const uint32_t *)(slot_buf + 0x045C);
        uint32_t init_pgs      = *(const uint32_t *)(slot_buf + 0x04A4);
        uint32_t max_pgs       = *(const uint32_t *)(slot_buf + 0x04AC);
        int16_t  sgid          = *(const int16_t  *)(slot_buf + 0x04F2);

        /* Container 0 no offset +0xC000 do slot */
        const uint8_t *c0      = slot_buf + 0xC000;
        uint32_t c0_type       = *(const uint32_t *)(c0 + 0x00);
        uint32_t c0_total_pgs  = *(const uint32_t *)(c0 + 0x04);
        uint32_t c0_use_pgs    = *(const uint32_t *)(c0 + 0x08);
        int32_t  c0_path_id    = *(const int32_t  *)(c0 + 0x30);
        const char *c0_path    = (const char *)(c0 + 0x34);

        const char *type_desc = "DMS Regular";
        if (tbsp_flags == 0x6112) type_desc = "DMS System Temporary";
        else if (tbsp_flags == 0x3122) type_desc = "DMS Large (Automatic Storage)";

        printf("  ------------------------------------------------------------------------------\n");
        printf("  [Slot #%u — Offset 0x%06X] Nome: %-15s | Status: %s\n",
               i, i * SQLSPCS_SLOT_SIZE, tbsp_name,
               (tbsp_id < 0) ? "DROPPED (ID=0xFFFF / -1)!" : "ATIVA");
        printf("  ------------------------------------------------------------------------------\n");
        printf("    Tablespace ID      : %d (Version=%u, Checksum=0x%08X)\n", tbsp_id, version, checksum);
        printf("    Tipo / Flags       : 0x%04X (%s)\n", tbsp_flags, type_desc);
        printf("    Page / Extent / Prf: PageSize=%u B | ExtentSize=%u pgs | Prefetch=%u pgs\n",
               page_size, extent_size, prefetch_size);
        printf("    Páginas (Tot/Usáv) : %u páginas totais / %u páginas usáveis\n",
               total_pages, useable_pages);
        printf("    Bufferpool / SGID  : Bufferpool ID=%u | StorageGroup ID=%d (%s)\n",
               bp_id, sgid, (sgid >= 0) ? "IBMSTOGROUP (AutoStorage)" : "Explicit DMS (Sem StoGroup)");
        printf("    AutoResize (Págs)  : InitPages=%u (%.1f MB) | MaxPages=%s",
               init_pgs, (double)init_pgs * page_size / (1024.0 * 1024.0),
               (max_pgs == 0xFFFFFFFF) ? "NONE (Ilimitado)\n" : "");
        if (max_pgs != 0xFFFFFFFF) {
            printf("%u (%.2f GB)\n", max_pgs, (double)max_pgs * page_size / (1024.0 * 1024.0 * 1024.0));
        }
        printf("    Creation LSN       : 0x%016llX\n", (unsigned long long)create_lsn);
        printf("    Containers (%u)     : [Container #0 em +0xC000: Tipo=%u, TotalPgs=%u, UseablePgs=%u, PathID=%d]\n",
               num_conts, c0_type, c0_total_pgs, c0_use_pgs, c0_path_id);
        printf("                         Caminho: %s\n\n", c0_path);
    }

    free(slot_buf);
    fclose(fp);
    return 0;
}

int main(int argc, char **argv) {
    if (argc < 2) {
        fprintf(stderr, "Uso:\n");
        fprintf(stderr, "  1) Inspecionar Container (Tag, SMP, Object Table, Dados e Índices B+Tree):\n");
        fprintf(stderr, "     %s <caminho_do_container> [object_id] [max_rows]\n", argv[0]);
        fprintf(stderr, "  2) Inspecionar Arquivo de Controle de Tablespaces (SQLSPCS.1 / SQLSPCS.2):\n");
        fprintf(stderr, "     %s --sqlspcs <caminho_do_SQLSPCS>\n", argv[0]);
        return 1;
    }

    if (strcmp(argv[1], "--sqlspcs") == 0) {
        const char *spcs_path = (argc >= 3)
            ? argv[2]
            : "/db2data_erp/db/erp/NODE0000/SQL00001/SQLSPCS.1";
        return inspect_sqlspcs_file(spcs_path);
    }

    db2_container_t ctx;
    if (!open_container(&ctx, argv[1])) {
        return 1;
    }

    uint16_t target_obj = (argc >= 3) ? (uint16_t)atoi(argv[2]) : 0;
    uint32_t max_rows   = (argc >= 4) ? (uint32_t)atoi(argv[3]) : 5;

    dump_container_tag(&ctx);
    dump_smp_and_extents(&ctx);
    dump_object_table(&ctx);

    if (target_obj > 0) {
        inspect_data_object(&ctx, target_obj, max_rows);
    } else if (ctx.tbspace_id == 4) {
        inspect_data_object(&ctx, 4, max_rows);
        inspect_data_object(&ctx, 5, max_rows);
        inspect_data_object(&ctx, 6, max_rows);
        inspect_data_object(&ctx, 7, max_rows);
    }

    fclose(ctx.fp);
    return 0;
}
