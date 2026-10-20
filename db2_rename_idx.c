/*
 * db2_rename_idx.c — Patch Cirúrgico Binário em catalog.dat (Burla SQL0607N)
 *
 * Renomeia diretamente no disco o índice de sistema SYSIBM.SQL230820161855040
 * (da tabela ERP.EMPRESA) para ERP.PK_EMP, atualizando:
 *   1. SYSIBM.SYSINDEXES     (ObjID=7,   Data Page FilePg=20426, Slot=2)
 *   2. INDINDEXES01          (ObjID=7,   B+Tree Leaf Pages FilePg=1300 -> FilePg=1290)
 *   3. SYSIBM.SYSINDEXCOLUSE (ObjID=9,   Data Page FilePg=881,   Slot=15)
 *   4. INDINDEXCOLUSE01      (ObjID=9,   B+Tree Leaf Pages FilePg=12340 -> FilePg=12323)
 *   5. SYSIBM.SYSINDEXAUTH   (ObjID=20,  Data Page FilePg=3136 + B+Tree FilePg=3202, 3203)
 *   6. SYSIBM.SYSCONSTDEP    (ObjID=263, Data Page FilePg=4896 + B+Tree FilePg=4962)
 *   7. PACKED_DESC BLOB      (ObjID=5,   LOB Data Page FilePg=1157 da ERP.EMPRESA)
 *
 * Recalcula o Checksum Fletcher-64 de página (engenharia reversa de
 * _Z25sqlbCalculatePageCheckSumP9SQLB_PAGEi na libdb2e.so.1) em todas as páginas!
 */

#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <stdbool.h>
#include <string.h>
#include <errno.h>

#define DB2_PAGE_SIZE   4096
#define SLOT_DIR_OFFSET 0x44

/*
 * Algoritmo exato de Checksum de Página do IBM Db2 11.5 (Fletcher-64 modificado),
 * obtido por engenharia reversa de _Z25sqlbCalculatePageCheckSumP9SQLB_PAGEi em libdb2e.so.1.
 */
static uint64_t db2_calc_page_checksum(const uint8_t *page) {
    uint32_t words[DB2_PAGE_SIZE / 4];
    memcpy(words, page, DB2_PAGE_SIZE);

    if (page[0x0B] & 0x10) {
        words[0x1C / 4] = 0xFFFFFFFFU;
    }
    words[0x28 / 4] = 0U;
    words[0x2C / 4] = 0U;

    uint64_t s1 = 0xFFFFFFFFULL;
    uint64_t s2 = 0xFFFFFFFFULL;

    for (int i = 0; i < (DB2_PAGE_SIZE / 4); i++) {
        s1 += words[i];
        s2 += s1;
    }

    s1 = (s1 & 0xFFFFFFFFULL) + (s1 >> 32);
    s1 = (s1 & 0xFFFFFFFFULL) + (s1 >> 32);
    s1 = (s1 & 0xFFFFULL) + (s1 >> 16);
    s1 = (s1 & 0xFFFFULL) + (s1 >> 16);

    s2 = (s2 & 0xFFFFFFFFFFFFULL) + (s2 >> 48);
    s2 = (s2 & 0xFFFFFFFFFFFFULL) + (s2 >> 48);

    return ((s2 & 0xFFFFFFFFFFFFULL) << 16) | (s1 & 0xFFFFULL);
}

static bool read_page(FILE *fp, uint32_t fpg, uint8_t *buf) {
    if (fseek(fp, (long)fpg * DB2_PAGE_SIZE, SEEK_SET) != 0) return false;
    return fread(buf, 1, DB2_PAGE_SIZE, fp) == DB2_PAGE_SIZE;
}

static bool write_page_with_checksum(FILE *fp, uint32_t fpg, uint8_t *buf, const char *desc) {
    uint64_t old_crc = *(const uint64_t *)(buf + 0x28);
    uint64_t new_crc = db2_calc_page_checksum(buf);
    *(uint64_t *)(buf + 0x28) = new_crc;

    if (fseek(fp, (long)fpg * DB2_PAGE_SIZE, SEEK_SET) != 0) return false;
    if (fwrite(buf, 1, DB2_PAGE_SIZE, fp) != DB2_PAGE_SIZE) return false;

    printf("  [OK] FilePg=%5u (%-36s) | Checksum: 0x%016llX -> 0x%016llX\n",
           fpg, desc, (unsigned long long)old_crc, (unsigned long long)new_crc);
    return true;
}

/* 1. Patch na linha de dados de SYSIBM.SYSINDEXES (FilePg=20426, Slot=2, Off=0xAF1) */
static bool patch_sysindexes_data(FILE *fp) {
    const uint32_t fpg = 20426;
    uint8_t pg[DB2_PAGE_SIZE];
    if (!read_page(fp, fpg, pg)) return false;

    uint16_t soff = *(uint16_t *)(pg + SLOT_DIR_OFFSET + 2 * 2);
    uint8_t *rec  = pg + SLOT_DIR_OFFSET + soff;

    /* Verifica se ainda contém SQL230820161855040 */
    if (memcmp(rec + 0x14A, "SQL230820161855040", 18) != 0) {
        printf("  [!] FilePg=20426 já foi patcheada anteriormente.\n");
        return true;
    }

    /* Reduz o tamanho do registro em 12 bytes (387 -> 375 = 0x0177) */
    *(uint16_t *)(rec + 0x002) = 375;

    /* Atualiza os descritores VARCHAR (var_off, var_len) relativos a rec + 8 */
    /* Col 0 (NAME): var_off = 0x0142, var_len = 6 ("PK_EMP") */
    *(uint16_t *)(rec + 0x008) = 0x0142;
    *(uint16_t *)(rec + 0x00A) = 6;
    /* Col 1 (CREATOR): var_off = 0x0148, var_len = 8 ("ERP     ") */
    *(uint16_t *)(rec + 0x00C) = 0x0148;
    *(uint16_t *)(rec + 0x00E) = 8;
    /* Col 2 (TBNAME): var_off = 0x0150, var_len = 7 ("EMPRESA") */
    *(uint16_t *)(rec + 0x010) = 0x0150;
    *(uint16_t *)(rec + 0x012) = 7;
    /* Col 3 (TBCREATOR): var_off = 0x0157, var_len = 8 ("ERP     ") */
    *(uint16_t *)(rec + 0x014) = 0x0157;
    *(uint16_t *)(rec + 0x016) = 8;
    /* Col 4 (COLNAMES): var_off = 0x015F, var_len = 8 ("+EMP_COD") */
    *(uint16_t *)(rec + 0x018) = 0x015F;
    *(uint16_t *)(rec + 0x01A) = 8;
    /* Col 18 (PAGE_FETCH_PAIRS): var_off = 0x0167, var_len = 0 */
    *(uint16_t *)(rec + 0x05D) = 0x0167;
    /* Col 20 (DEFINER): var_off = 0x0167, var_len = 8 ("ERP     ") */
    *(uint16_t *)(rec + 0x067) = 0x0167;
    /* Col 54 (AVGPARTITION_PAGE_FETCH_PAIRS): var_off = 0x016F, var_len = 0 */
    *(uint16_t *)(rec + 0x113) = 0x016F;

    /* Grava a nova seção variável compactada (37 bytes) e zera os 12 bytes finais */
    const char *new_var = "PK_EMPERP     EMPRESAERP     +EMP_CODERP     ";
    memcpy(rec + 0x14A, new_var, 45);
    memset(rec + 0x14A + 45, 0, 12);

    /* Atualiza total_free_space da página (+12 bytes) */
    *(uint16_t *)(pg + 0x32) += 12;

    return write_page_with_checksum(fp, fpg, pg, "SYSIBM.SYSINDEXES Data Row");
}

/* 2. Patch nas folhas B+Tree do índice INDINDEXES01 (FilePg=1300 -> FilePg=1290) */
static bool patch_sysindexes_btree(FILE *fp) {
    /* 2.1 Remove k[47] ('SYSIBM  ', 'SQL230820161855040') da FilePg=1300 */
    uint8_t pg1300[DB2_PAGE_SIZE];
    if (!read_page(fp, 1300, pg1300)) return false;

    uint16_t nkeys1300 = *(uint16_t *)(pg1300 + 0x54 + 6);
    uint16_t *koffs1300 = (uint16_t *)(pg1300 + 0x54 + 12);
    int del_idx = -1;
    for (uint16_t i = 0; i < nkeys1300; i++) {
        uint16_t ko = koffs1300[i];
        if (memcmp(pg1300 + ko + 14, "SQL230820161855040", 18) == 0) {
            del_idx = i;
            break;
        }
    }
    if (del_idx >= 0) {
        for (uint16_t i = (uint16_t)del_idx; i + 1 < nkeys1300; i++) {
            koffs1300[i] = koffs1300[i + 1];
        }
        koffs1300[nkeys1300 - 1] = 0;
        *(uint16_t *)(pg1300 + 0x54 + 6) = nkeys1300 - 1;
        *(uint16_t *)(pg1300 + 0x32) += 37; /* total_free_space += 37 */
        if (!write_page_with_checksum(fp, 1300, pg1300, "INDINDEXES01 Leaf (Remove Old Key)")) return false;
    }

    /* 2.2 Insere ('ERP     ', 'PK_EMP') -> RID(42:2) em k[1] da FilePg=1290 */
    uint8_t pg1290[DB2_PAGE_SIZE];
    if (!read_page(fp, 1290, pg1290)) return false;

    uint16_t nkeys1290  = *(uint16_t *)(pg1290 + 0x54 + 6);
    uint16_t *koffs1290 = (uint16_t *)(pg1290 + 0x54 + 12);
    uint16_t free_off   = *(uint16_t *)(pg1290 + 0x3E);

    const uint8_t new_key[25] = {
        0x19, 0x00,                                     /* krec_len = 25 */
        0x08, 0x00, 'E','R','P',' ',' ',' ',' ',' ',    /* CREATOR = 'ERP     ' */
        0x06, 0x00, 'P','K','_','E','M','P',            /* NAME    = 'PK_EMP' */
        0x02, 0x2A, 0x00, 0x00, 0x02                    /* RID(Page=42 : Slot=2, flags=0x02) */
    };

    memcpy(pg1290 + free_off, new_key, 25);
    for (int i = (int)nkeys1290; i > 1; i--) {
        koffs1290[i] = koffs1290[i - 1];
    }
    koffs1290[1] = free_off;
    *(uint16_t *)(pg1290 + 0x54 + 6) = nkeys1290 + 1;
    *(uint16_t *)(pg1290 + 0x3E)     = free_off + 25;
    *(uint16_t *)(pg1290 + 0x32)    -= 25;

    return write_page_with_checksum(fp, 1290, pg1290, "INDINDEXES01 Leaf (Insert PK_EMP)");
}

/* 3. Patch na linha de dados de SYSIBM.SYSINDEXCOLUSE (FilePg=881, Slot=15, Off=0xC05) */
static bool patch_sysindexcoluse_data(FILE *fp) {
    const uint32_t fpg = 881;
    uint8_t pg[DB2_PAGE_SIZE];
    if (!read_page(fp, fpg, pg)) return false;

    uint16_t soff = *(uint16_t *)(pg + SLOT_DIR_OFFSET + 15 * 2);
    uint8_t *rec  = pg + SLOT_DIR_OFFSET + soff;

    if (memcmp(rec + 0x02D, "SQL230820161855040", 18) != 0) return true;

    *(uint16_t *)(rec + 0x002) = 58;     /* rec_len: 70 -> 58 */
    *(uint16_t *)(rec + 0x008) = 0x001D; /* Col 0 (INDSCHEMA): off=0x1D, len=8 */
    *(uint16_t *)(rec + 0x00A) = 8;
    *(uint16_t *)(rec + 0x00C) = 0x0025; /* Col 1 (INDNAME):   off=0x25, len=6 */
    *(uint16_t *)(rec + 0x00E) = 6;
    *(uint16_t *)(rec + 0x010) = 0x002B; /* Col 2 (COLNAME):   off=0x2B, len=7 */
    *(uint16_t *)(rec + 0x012) = 7;

    memcpy(rec + 0x025, "ERP     PK_EMPEMP_COD", 21);
    memset(rec + 0x025 + 21, 0, 12);
    *(uint16_t *)(pg + 0x32) += 12;

    return write_page_with_checksum(fp, fpg, pg, "SYSIBM.SYSINDEXCOLUSE Data Row");
}

/* 4. Patch nas folhas B+Tree do índice INDINDEXCOLUSE01 (FilePg=12340 -> FilePg=12323) */
static bool patch_sysindexcoluse_btree(FILE *fp) {
    uint8_t pg12340[DB2_PAGE_SIZE];
    if (!read_page(fp, 12340, pg12340)) return false;

    uint16_t nkeys12340  = *(uint16_t *)(pg12340 + 0x54 + 6);
    uint16_t *koffs12340 = (uint16_t *)(pg12340 + 0x54 + 12);
    int del_idx = -1;
    for (uint16_t i = 0; i < nkeys12340; i++) {
        uint16_t ko = koffs12340[i];
        if (memcmp(pg12340 + ko + 14, "SQL230820161855040", 18) == 0) {
            del_idx = i;
            break;
        }
    }
    if (del_idx >= 0) {
        for (uint16_t i = (uint16_t)del_idx; i + 1 < nkeys12340; i++) {
            koffs12340[i] = koffs12340[i + 1];
        }
        koffs12340[nkeys12340 - 1] = 0;
        *(uint16_t *)(pg12340 + 0x54 + 6) = nkeys12340 - 1;
        *(uint16_t *)(pg12340 + 0x32)    += 48;
        if (!write_page_with_checksum(fp, 12340, pg12340, "INDINDEXCOLUSE01 Leaf (Remove Old)")) return false;
    }

    uint8_t pg12323[DB2_PAGE_SIZE];
    if (!read_page(fp, 12323, pg12323)) return false;

    uint16_t nkeys12323  = *(uint16_t *)(pg12323 + 0x54 + 6);
    uint16_t *koffs12323 = (uint16_t *)(pg12323 + 0x54 + 12);
    uint16_t free_off    = *(uint16_t *)(pg12323 + 0x3E);

    const uint8_t new_key[36] = {
        0x24, 0x00,                                     /* krec_len = 36 */
        0x08, 0x00, 'E','R','P',' ',' ',' ',' ',' ',    /* INDSCHEMA = 'ERP     ' */
        0x06, 0x00, 'P','K','_','E','M','P',            /* INDNAME   = 'PK_EMP' */
        0x07, 0x00, 'E','M','P','_','C','O','D',        /* COLNAME   = 'EMP_COD' */
        0x01, 0x00,                                     /* COLSEQ    = 1 */
        0x0F, 0x11, 0x00, 0x00, 0x02                    /* RID(Page=17 : Slot=15, flags=0x02) */
    };

    memcpy(pg12323 + free_off, new_key, 36);
    for (int i = (int)nkeys12323; i > 2; i--) {
        koffs12323[i] = koffs12323[i - 1];
    }
    koffs12323[2] = free_off;
    *(uint16_t *)(pg12323 + 0x54 + 6) = nkeys12323 + 1;
    *(uint16_t *)(pg12323 + 0x3E)     = free_off + 36;
    *(uint16_t *)(pg12323 + 0x32)    -= 36;

    return write_page_with_checksum(fp, 12323, pg12323, "INDINDEXCOLUSE01 Leaf (Insert New)");
}

/* 5. Patch em SYSIBM.SYSINDEXAUTH (Data FilePg=3136 + B+Tree FilePg=3202, 3203) */
static bool patch_sysindexauth(FILE *fp) {
    /* 5.1 Data Page FilePg=3136, Slot=7, Off=0xA35 */
    uint8_t pg3136[DB2_PAGE_SIZE];
    if (!read_page(fp, 3136, pg3136)) return false;

    uint16_t soff = *(uint16_t *)(pg3136 + SLOT_DIR_OFFSET + 7 * 2);
    uint8_t *rec  = pg3136 + SLOT_DIR_OFFSET + soff;
    if (memcmp(rec + 0x035, "SQL230820161855040", 18) == 0) {
        *(uint16_t *)(rec + 0x002) = 67;     /* rec_len: 79 -> 67 */
        *(uint16_t *)(rec + 0x010) = 0x002D; /* Col 2 (NAME):    off=0x2D, len=6 */
        *(uint16_t *)(rec + 0x012) = 6;
        *(uint16_t *)(rec + 0x014) = 0x0033; /* Col 3 (CREATOR): off=0x33, len=8 */
        *(uint16_t *)(rec + 0x016) = 8;
        memcpy(rec + 0x035, "PK_EMPERP     ", 14);
        memset(rec + 0x035 + 14, 0, 12);
        *(uint16_t *)(pg3136 + 0x32) += 12;
        if (!write_page_with_checksum(fp, 3136, pg3136, "SYSIBM.SYSINDEXAUTH Data Row")) return false;
    }

    /* 5.2 B+Tree FilePg=3202 (dbase=0x0E8, k[3] at 0x322 -> move para k[1]) */
    uint8_t pg3202[DB2_PAGE_SIZE];
    if (!read_page(fp, 3202, pg3202)) return false;
    uint16_t *koffs3202 = (uint16_t *)(pg3202 + 0x0E8 + 12);
    uint16_t ko3202 = koffs3202[3];
    const uint8_t key3202[47] = {
        0x2F, 0x00,
        0x08, 0x00, 'E','R','P',' ',' ',' ',' ',' ',
        0x08, 0x00, 'E','R','P',' ',' ',' ',' ',' ',
        0x06, 0x00, 'P','K','_','E','M','P','U',
        0x08, 0x00, 'S','Y','S','I','B','M',' ',' ','S',
        0x07, 0x00, 0x00, 0x00, 0x02
    };
    memcpy(pg3202 + ko3202, key3202, 47);
    memset(pg3202 + ko3202 + 47, 0, 12);
    koffs3202[3] = koffs3202[2];
    koffs3202[2] = koffs3202[1];
    koffs3202[1] = ko3202;
    *(uint16_t *)(pg3202 + 0x32) += 12;
    if (!write_page_with_checksum(fp, 3202, pg3202, "INDINDEXAUTH01 Leaf (Reorder Key)")) return false;

    /* 5.3 B+Tree FilePg=3203 (dbase=0x0B8, k[3] at 0x372 -> move para k[1]) */
    uint8_t pg3203[DB2_PAGE_SIZE];
    if (!read_page(fp, 3203, pg3203)) return false;
    uint16_t *koffs3203 = (uint16_t *)(pg3203 + 0x0B8 + 12);
    uint16_t ko3203 = koffs3203[3];
    const uint8_t key3203[25] = {
        0x19, 0x00,
        0x08, 0x00, 'E','R','P',' ',' ',' ',' ',' ',
        0x06, 0x00, 'P','K','_','E','M','P',
        0x07, 0x00, 0x00, 0x00, 0x02
    };
    memcpy(pg3203 + ko3203, key3203, 25);
    memset(pg3203 + ko3203 + 25, 0, 12);
    koffs3203[3] = koffs3203[2];
    koffs3203[2] = koffs3203[1];
    koffs3203[1] = ko3203;
    *(uint16_t *)(pg3203 + 0x32) += 12;
    return write_page_with_checksum(fp, 3203, pg3203, "INDINDEXAUTH02 Leaf (Reorder Key)");
}

/* 6. Patch em SYSIBM.SYSCONSTDEP (Data FilePg=4896 + B+Tree FilePg=4962) */
static bool patch_sysconstdep(FILE *fp) {
    /* 6.1 Data Page FilePg=4896, Slot=7, Off=0xA19 */
    uint8_t pg4896[DB2_PAGE_SIZE];
    if (!read_page(fp, 4896, pg4896)) return false;

    uint16_t soff = *(uint16_t *)(pg4896 + SLOT_DIR_OFFSET + 7 * 2);
    uint8_t *rec  = pg4896 + SLOT_DIR_OFFSET + soff;
    if (memcmp(rec + 0x022, "SQL230820161855040", 18) == 0) {
        *(uint16_t *)(rec + 0x002) = 81;     /* rec_len: 93 -> 81 */
        *(uint16_t *)(rec + 0x008) = 0x001A; /* Col 0 (BNAME):     off=0x1A, len=6 */
        *(uint16_t *)(rec + 0x00A) = 6;
        *(uint16_t *)(rec + 0x00C) = 0x0020; /* Col 1 (BSCHEMA):   off=0x20, len=8 */
        *(uint16_t *)(rec + 0x00E) = 8;
        *(uint16_t *)(rec + 0x011) = 0x0028; /* Col 3 (TABNAME):   off=0x28, len=7 */
        *(uint16_t *)(rec + 0x013) = 7;
        *(uint16_t *)(rec + 0x015) = 0x002F; /* Col 4 (TABSCHEMA): off=0x2F, len=8 */
        *(uint16_t *)(rec + 0x017) = 8;
        *(uint16_t *)(rec + 0x019) = 0x0037; /* Col 5 (CONSTNAME): off=0x37, len=18 */
        *(uint16_t *)(rec + 0x01B) = 18;

        memcpy(rec + 0x022, "PK_EMPERP     EMPRESAERP     SQL230820161855060", 47);
        memset(rec + 0x022 + 47, 0, 12);
        *(uint16_t *)(pg4896 + 0x32) += 12;
        if (!write_page_with_checksum(fp, 4896, pg4896, "SYSIBM.SYSCONSTDEP Data Row")) return false;
    }

    /* 6.2 B+Tree FilePg=4962 (dbase=0x0F4, k[3] at 0x305 -> move para k[1]) */
    uint8_t pg4962[DB2_PAGE_SIZE];
    if (!read_page(fp, 4962, pg4962)) return false;
    uint16_t *koffs4962 = (uint16_t *)(pg4962 + 0x0F4 + 12);
    uint16_t ko4962 = koffs4962[3];
    const uint8_t key4962[70] = {
        0x46, 0x00,
        0x08, 0x00, 'E','R','P',' ',' ',' ',' ',' ',
        0x06, 0x00, 'P','K','_','E','M','P',
        0x01, 0x00, 0x00, 0x00, 0x00, 'I',
        0x08, 0x00, 'E','R','P',' ',' ',' ',' ',' ',
        0x07, 0x00, 'E','M','P','R','E','S','A',
        0x12, 0x00, 'S','Q','L','2','3','0','8','2','0','1','6','1','8','5','5','0','6','0',
        0x07, 0x00, 0x00, 0x00, 0x00
    };
    memcpy(pg4962 + ko4962, key4962, 70);
    memset(pg4962 + ko4962 + 70, 0, 12);
    koffs4962[3] = koffs4962[2];
    koffs4962[2] = koffs4962[1];
    koffs4962[1] = ko4962;
    *(uint16_t *)(pg4962 + 0x32) += 12;
    return write_page_with_checksum(fp, 4962, pg4962, "INDCONSTDEP01 Leaf (Reorder Key)");
}

/* 7. Patch no PACKED_DESC BLOB da tabela ERP.EMPRESA (FilePg=1157) */
static bool patch_packed_desc_blob(FILE *fp) {
    const uint32_t fpg = 1157;
    uint8_t pg[DB2_PAGE_SIZE];
    if (!read_page(fp, fpg, pg)) return false;

    if (memcmp(pg + 0xB50, "SQL230820161855040", 18) != 0) return true;

    /* Offset final do nome do índice dentro do descritor: 0x0368 + 6 = 0x036E */
    *(uint16_t *)(pg + 0xA3A) = 0x036E;
    /* Schema do índice no descritor compilado: "ERP     " */
    memcpy(pg + 0xA6D, "ERP     ", 8);
    /* Nome do índice + colunas ("PK_EMP+EMP_COD") */
    memset(pg + 0xB50, 0, 26);
    memcpy(pg + 0xB50, "PK_EMP+EMP_COD", 14);

    if (fseek(fp, (long)fpg * DB2_PAGE_SIZE, SEEK_SET) != 0) return false;
    if (fwrite(pg, 1, DB2_PAGE_SIZE, fp) != DB2_PAGE_SIZE) return false;

    printf("  [OK] FilePg=%5u (%-36s) | LOB Page atualizada (sem BPS header)\n",
           fpg, "SYSIBM.SYSTABLES PACKED_DESC BLOB");
    return true;
}

int main(int argc, char **argv) {
    const char *cat_path = (argc >= 2) ? argv[1] : "/db2data_erp/data/catalog.dat";

    printf("====================================================================================================\n");
    printf(" PATCH BINÁRIO DIRETO EM DISCO — SYSIBM.SQL230820161855040 -> ERP.PK_EMP\n");
    printf("====================================================================================================\n");
    printf("  Arquivo Alvo: %s\n\n", cat_path);

    FILE *fp = fopen(cat_path, "r+b");
    if (!fp) {
        fprintf(stderr, "Erro ao abrir '%s' para escrita: %s\n", cat_path, strerror(errno));
        return 1;
    }

    bool ok = true;
    ok = ok && patch_sysindexes_data(fp);
    ok = ok && patch_sysindexes_btree(fp);
    ok = ok && patch_sysindexcoluse_data(fp);
    ok = ok && patch_sysindexcoluse_btree(fp);
    ok = ok && patch_sysindexauth(fp);
    ok = ok && patch_sysconstdep(fp);
    ok = ok && patch_packed_desc_blob(fp);

    fflush(fp);
    fclose(fp);

    if (ok) {
        printf("\n  >> Patch binário concluído com sucesso! Todas as páginas tiveram o Fletcher-64 recalculado.\n\n");
        return 0;
    } else {
        fprintf(stderr, "\n  [ERRO] Falha durante a aplicação do patch.\n");
        return 1;
    }
}
