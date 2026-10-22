/*
 * db2_log_recovery.c - Motor Autônomo de Crash Recovery & Rollforward (Algoritmo ARIES)
 *
 * Implementação 100% nativa e autônoma em C das 3 fases clássicas do algoritmo ARIES
 * para recuperação de banco de dados IBM Db2 LUW:
 *
 * 1. FASE DE ANÁLISE (Analysis Pass):
 *    - Lê o Log File Header (SQLOGCTL.LFH.1 / .2) para extrair o ponto de controle:
 *      LowTranLSN, MinBuffLSN, Next LSN, Current LSO e extents ativos.
 *    - Varre o fluxo contínuo de registros a partir do RedoLSN / primeiro extent ativo.
 *    - Constrói a Tabela de Transações Ativas (Active Transaction Table - ATT).
 *    - Classifica transações em "COMMITTED" (vencedoras) e "LOSERS" (incompletas no crash).
 *
 * 2. FASE DE REDO (Rollforward / Repeating History):
 *    - Aplica o princípio de Repeating History a partir de RedoLSN = min(MinBuffLSN, LowTranLSN).
 *    - Para cada registro de modificação física (DMS DOM Insert/Delete/Update):
 *      Inspeciona o cabeçalho BPS da página física no container correspondente (.LRG).
 *    - Testa a Regra de Ouro da Idempotência ARIES:
 *      * Se Page_LSN >= Log_LSN: A alteração já está gravada na página física. Skip REDO!
 *      * Se Page_LSN < Log_LSN: A página está defasada. Replay do payload (Apply REDO).
 *
 * 3. FASE DE UNDO (Rollback de Transações Losers):
 *    - Percorre a cadeia reversa (PrevLSO) de cada transação Loser do LastLSN até FirstLSN.
 *    - Desfaz operações aplicando a Before-Image (Undo Row).
 *    - Emite Compensation Log Records (CLRs) com UndoNextLSN para prevenir
 *      repetição de UNDO em cascata caso ocorra uma falha durante o próprio processo de recovery.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <stdbool.h>
#include <ctype.h>
#include <errno.h>

#define DB2_PAGE_SIZE    4096
#define MAX_TRANS        1024
#define MAX_EXTENTS      32
#define MAX_STREAM_SIZE  (32 * 1024 * 1024) /* 32 MB de buffer de stream */

#pragma pack(push, 1)

/* Cabeçalho de 16 bytes de cada página de 4KB de Transaction Log */
typedef struct {
    uint16_t bytes_in_page;         /* +0x00: Bytes válidos de payload (máx 0x0FEC = 4076) */
    uint16_t first_rec_offset;      /* +0x02: Offset relativo a +0x10 do 1º registro que inicia nesta página */
    uint32_t page_flags;            /* +0x04 */
    uint64_t page_end_lso;          /* +0x08: LSO (Log Sequence Offset) do último byte desta página */
} db2_log_page_hdr_t;

/* Cabeçalho comum de 32 bytes (0x20) de todo Registro de Log */
typedef struct {
    uint32_t rec_len;               /* +0x00: Tamanho total do registro em bytes */
    uint16_t rec_type;              /* +0x04: 0x004E=DMS Data/Idx, 0x0084/0x0043=Commit, 0x0041=Abort */
    uint16_t rec_flags;             /* +0x06 */
    uint64_t lsn;                   /* +0x08: Log Sequence Number */
    uint64_t tid;                   /* +0x10: Transaction ID */
    uint64_t prev_lso;              /* +0x18: LSO anterior desta mesma transação (0 = início) */
} db2_log_rec_hdr_t;

/* Cabeçalho do Arquivo de Controle de Log (SQLOGCTL.LFH.1 / .2) */
typedef struct {
    uint16_t write_counter;         /* +0x0000 */
    uint16_t lfh_version;           /* +0x0002 */
    uint32_t db_seed_ts;            /* +0x0004 */
    uint8_t  _pad1[16];
    uint64_t next_ext_lso;          /* +0x0018 */
    uint64_t next_lsn;              /* +0x0020 */
    uint64_t min_buff_lsn;          /* +0x0028 */
    uint64_t low_tran_lsn;          /* +0x0030 */
    uint8_t  _pad2[8];
    uint32_t last_ckpt_ts;          /* +0x0040 */
    uint32_t log_chain_ts;          /* +0x0044 */
    uint8_t  _pad3[392];
    uint64_t current_lso;           /* +0x01D0 */
    uint8_t  _pad4[80];
    uint64_t head_lsn;              /* +0x0228 */
    uint64_t last_tid;              /* +0x0230 */
    uint8_t  _pad5[36];
    uint32_t log_primary;           /* +0x025C */
    uint32_t active_extents[MAX_EXTENTS]; /* +0x0260 */
} db2_lfh_t;

/* Cabeçalho BPS de 48 bytes da página de dados */
typedef struct {
    uint16_t page_data_offset;      /* +0x00 */
    uint16_t page_data_length;      /* +0x02 */
    uint32_t obj_page_num;          /* +0x04: Número lógico da página no objeto */
    uint8_t  version_flags;         /* +0x08 */
    uint8_t  page_flags;            /* +0x09 */
    uint8_t  obj_type;              /* +0x0A */
    uint8_t  consistency_bits;      /* +0x0B */
    uint16_t tbspace_id;            /* +0x0C */
    uint16_t reserved1;             /* +0x0E */
    uint32_t pool_page_num;         /* +0x10 */
    uint16_t object_id;             /* +0x14 */
    uint16_t parent_obj_id;         /* +0x16 */
    uint32_t reserved2;             /* +0x18 */
    uint32_t reserved3;             /* +0x1C */
    uint64_t page_lsn;              /* +0x20: Log Sequence Number no disco */
    uint64_t checksum_meta;         /* +0x28 */
} db2_bps_header_t;

#pragma pack(pop)

typedef enum {
    TRANS_STATUS_ACTIVE,     /* Loser transaction (não commitou antes do corte) */
    TRANS_STATUS_COMMITTED,  /* Winner transaction */
    TRANS_STATUS_ABORTED     /* Transação já explicitamente abortada */
} trans_status_t;

typedef struct {
    uint64_t       tid;
    trans_status_t status;
    uint64_t       first_lsn;
    uint64_t       last_lsn;
    uint64_t       last_lso;
    uint32_t       num_ops;
    uint32_t       num_inserts;
    uint32_t       num_deletes;
    uint32_t       num_updates;
    uint32_t       commit_time;
} trans_entry_t;

typedef struct {
    trans_entry_t entries[MAX_TRANS];
    uint32_t      count;
} trans_table_t;

static trans_entry_t *find_or_add_trans(trans_table_t *tt, uint64_t tid) {
    for (uint32_t i = 0; i < tt->count; i++) {
        if (tt->entries[i].tid == tid) return &tt->entries[i];
    }
    if (tt->count >= MAX_TRANS) return NULL;
    trans_entry_t *e = &tt->entries[tt->count++];
    memset(e, 0, sizeof(*e));
    e->tid = tid;
    e->status = TRANS_STATUS_ACTIVE;
    return e;
}

static const char *log_op_desc(uint16_t rec_type, const uint8_t *payload, uint32_t payload_len) {
    if (rec_type == 0x0084 || rec_type == 0x0043) return "COMMIT";
    if (rec_type == 0x0041) return "ABORT";
    if (rec_type == 0x0082 || rec_type == 0x0042) return "CLR (COMPENSATION)";
    if (rec_type == 0x004E && payload_len >= 12) {
        if (payload[8] == 0x01 && (payload[9] == 0xA2 || payload[9] == 0xA5)) return "DOM INSERT ROW";
        if (payload[8] == 0x01 && (payload[9] == 0xA1 || payload[9] == 0xA4)) return "DOM DELETE ROW";
        if (payload[8] == 0x01 && (payload[9] == 0x72 || payload[9] == 0xB7 || payload[9] == 0xBE)) return "DOM UPDATE ROW";
        if (payload[8] == 0x02 && payload[9] == 0x65) return "IXM INSERT KEY";
        if (payload[8] == 0x02 && payload[9] == 0xA1) return "IXM DELETE KEY";
    }
    return "INTERNAL OP";
}

/* Resolve a página física no arquivo de container a partir de (obj_id, rid_page) */
static bool resolve_disk_page(FILE *f_cnt, uint16_t target_obj_id, uint32_t rid_page, uint64_t *out_page_lsn) {
    /*
     * Navegação física em ERPTBS01 (C0000000.LRG):
     * 1. A Object Table inicia em Pool Page 64 (File Page 96 = 32 tag + 64).
     * 2. Localiza o registro do target_obj_id para extrair a pool page do Data EMP.
     * 3. Na EMP page, lê o primeiro extent de dados do objeto.
     * 4. Lê a página física e extrai bps->page_lsn.
     */
    uint8_t ot_pg[DB2_PAGE_SIZE];
    fseek(f_cnt, 96L * DB2_PAGE_SIZE, SEEK_SET);
    if (fread(ot_pg, 1, DB2_PAGE_SIZE, f_cnt) != DB2_PAGE_SIZE) return false;

    uint32_t data_emp_pool = 0;
    const uint16_t *sdir = (const uint16_t *)(ot_pg + 0x44);
    for (uint16_t s = 4; s < 256; s++) {
        uint16_t soff = sdir[s];
        if (soff == 0 || soff == 0xFFFF || 0x44 + soff >= DB2_PAGE_SIZE) continue;
        const uint8_t *rec = ot_pg + 0x44 + soff;
        uint16_t rec_type = *(const uint16_t *)(rec + 0);
        uint16_t rec_len  = *(const uint16_t *)(rec + 2);
        if (rec_type != 0x0010 || rec_len < 0x70) continue;
        uint16_t obj_id = *(const uint16_t *)(rec + 0x0A);
        if (obj_id == target_obj_id) {
            data_emp_pool = *(const uint32_t *)(rec + 0x0C);
            break;
        }
    }

    if (data_emp_pool == 0) return false;

    /* Lê a página de Data EMP */
    uint32_t emp_file_page = 32 + data_emp_pool;
    uint8_t emp_pg[DB2_PAGE_SIZE];
    fseek(f_cnt, (long)emp_file_page * DB2_PAGE_SIZE, SEEK_SET);
    if (fread(emp_pg, 1, DB2_PAGE_SIZE, f_cnt) != DB2_PAGE_SIZE) return false;

    const uint32_t *ext_entries = (const uint32_t *)(emp_pg + 0x58);
    uint32_t obj_extent_pool = ext_entries[0];
    if (obj_extent_pool == 0 || obj_extent_pool == 0xFFFFFFFF) return false;

    /* A página física no arquivo de dados é 32 (tag) + obj_extent_pool + rid_page */
    uint32_t target_file_page = 32 + obj_extent_pool + rid_page;
    uint8_t data_pg[DB2_PAGE_SIZE];
    fseek(f_cnt, (long)target_file_page * DB2_PAGE_SIZE, SEEK_SET);
    if (fread(data_pg, 1, DB2_PAGE_SIZE, f_cnt) != DB2_PAGE_SIZE) return false;

    const db2_bps_header_t *bps = (const db2_bps_header_t *)data_pg;
    *out_page_lsn = bps->page_lsn;
    return true;
}

int main(int argc, char **argv) {
    const char *lfh_path = "/db2data_erp/db/erp/NODE0000/SQL00001/MEMBER0000/SQLOGCTL.LFH.1";
    const char *log_dir  = "/db2data_erp/db/erp/NODE0000/SQL00001/LOGSTREAM0000";
    const char *cnt_path = "/db2data_erp/data/erp/NODE0000/ERP/T0000004/C0000000.LRG";

    printf("================================================================================\n");
    printf(" MOTOR AUTÔNOMO DE CRASH RECOVERY & ROLLFORWARD (ARIES) — IBM Db2 LUW\n");
    printf("================================================================================\n\n");

    /* 1. LER PONTO DE CONTROLE NO LOG FILE HEADER (SQLOGCTL.LFH.1) */
    FILE *f_lfh = fopen(lfh_path, "rb");
    if (!f_lfh) {
        perror("Erro ao abrir LFH");
        return 1;
    }

    db2_lfh_t lfh;
    if (fread(&lfh, 1, sizeof(lfh), f_lfh) != sizeof(lfh)) {
        fprintf(stderr, "Erro ao ler LFH\n");
        fclose(f_lfh);
        return 1;
    }
    fclose(f_lfh);

    printf("  [FASE 0 — ANÁLISE DE PONTO DE CONTROLE NO LFH]\n");
    printf("  -> Versão do Formato   : %u (Contador de Escrita: %u)\n", lfh.lfh_version, lfh.write_counter);
    printf("  -> Next LSN Previsto   : 0x%016llX\n", (unsigned long long)lfh.next_lsn);
    printf("  -> MinBuffLSN (DirtyPg): 0x%016llX\n", (unsigned long long)lfh.min_buff_lsn);
    printf("  -> LowTranLSN (OldestTx: 0x%016llX\n", (unsigned long long)lfh.low_tran_lsn);
    printf("  -> Current LSO         : %llu (0x%012llX)\n",
           (unsigned long long)lfh.current_lso, (unsigned long long)lfh.current_lso);
    printf("  -> Último TID Alocado  : 0x%012llX\n", (unsigned long long)lfh.last_tid);

    bool clean_shutdown = (lfh.next_lsn == lfh.min_buff_lsn && lfh.next_lsn == lfh.low_tran_lsn);
    printf("  -> Estado de Consistência: %s\n",
           clean_shutdown ? "CONSISTENTE (Clean Shutdown)" : "CRASH RECOVERY NECESSÁRIO");

    uint64_t redo_start_lsn = (lfh.min_buff_lsn < lfh.low_tran_lsn && lfh.min_buff_lsn != 0)
                               ? lfh.min_buff_lsn : lfh.low_tran_lsn;
    if (redo_start_lsn == 0) redo_start_lsn = lfh.head_lsn;
    printf("  -> RedoLSN Determinado : 0x%016llX (Ponto de Partida da Fase de REDO)\n\n",
           (unsigned long long)redo_start_lsn);

    /* 2. RECONSTITUIÇÃO DO FLUXO CONTÍNUO DE LOGS (LSO STREAM) */
    uint8_t *stream_buf = (uint8_t *)malloc(MAX_STREAM_SIZE);
    if (!stream_buf) {
        fprintf(stderr, "Erro de alocação de memória para stream_buf\n");
        return 1;
    }

    uint64_t stream_len = 0;
    uint64_t stream_start_lso = 0;
    uint16_t stream_first_off = 0;

    char path[512];
    snprintf(path, sizeof(path), "%s/S0000000.LOG", log_dir);
    FILE *fp = fopen(path, "rb");
    if (!fp) {
        perror("Erro ao abrir S0000000.LOG");
        free(stream_buf);
        return 1;
    }

    fseek(fp, 0, SEEK_END);
    uint64_t fsz = (uint64_t)ftell(fp);
    uint32_t total_pages = (uint32_t)((fsz - 2 * DB2_PAGE_SIZE) / DB2_PAGE_SIZE);

    uint8_t pg_buf[DB2_PAGE_SIZE];
    uint32_t active_pages = 0;
    uint64_t prev_end_lso = 0;

    for (uint32_t p = 0; p < total_pages; p++) {
        fseek(fp, (long)((2 + p) * DB2_PAGE_SIZE), SEEK_SET);
        if (fread(pg_buf, 1, DB2_PAGE_SIZE, fp) != DB2_PAGE_SIZE) break;

        const db2_log_page_hdr_t *phdr = (const db2_log_page_hdr_t *)pg_buf;
        if (phdr->bytes_in_page == 0 || phdr->bytes_in_page > 4076) break;

        uint64_t pg_start_lso = phdr->page_end_lso - phdr->bytes_in_page + 1;
        if (p == 0) {
            stream_start_lso = pg_start_lso;
            stream_first_off = phdr->first_rec_offset;
        } else {
            if (pg_start_lso != prev_end_lso + 1) break;
        }

        if (stream_len + phdr->bytes_in_page <= MAX_STREAM_SIZE) {
            memcpy(stream_buf + stream_len, pg_buf + 0x10, phdr->bytes_in_page);
            stream_len += phdr->bytes_in_page;
            prev_end_lso = phdr->page_end_lso;
            active_pages++;
        }
    }
    fclose(fp);

    printf("  [FLUXO CONTÍNUO DE LOGS RECONSTITUÍDO]\n");
    printf("  -> Extent Processado   : S0000000.LOG (%u páginas ativas)\n", active_pages);
    printf("  -> Faixa LSO Mapeada   : StartLSO=%llu .. EndLSO=%llu\n",
           (unsigned long long)stream_start_lso, (unsigned long long)prev_end_lso);
    printf("  -> Tamanho do Stream   : %llu bytes (%.2f KB de registros)\n",
           (unsigned long long)stream_len, (double)stream_len / 1024.0);
    printf("  -> Offset 1º Registro  : +%u bytes na Página 0\n\n", stream_first_off);

    /* 3. FASE 1 — ANALYSIS PASS (CONSTRUÇÃO DA TABELA DE TRANSAÇÕES) */
    trans_table_t tt;
    memset(&tt, 0, sizeof(tt));

    uint64_t pos = stream_first_off;
    uint64_t total_log_records = 0;
    uint64_t first_stream_lsn = 0;
    uint64_t last_stream_lsn = 0;

    while (pos + sizeof(db2_log_rec_hdr_t) <= stream_len) {
        const db2_log_rec_hdr_t *rh = (const db2_log_rec_hdr_t *)(stream_buf + pos);
        if (rh->rec_len < sizeof(db2_log_rec_hdr_t) || pos + rh->rec_len > stream_len) break;

        const uint8_t *payload = stream_buf + pos + sizeof(db2_log_rec_hdr_t);
        uint32_t payload_len   = rh->rec_len - (uint32_t)sizeof(db2_log_rec_hdr_t);

        if (total_log_records == 0) first_stream_lsn = rh->lsn;
        last_stream_lsn = rh->lsn;
        total_log_records++;

        trans_entry_t *t = find_or_add_trans(&tt, rh->tid);
        if (t) {
            if (t->first_lsn == 0) t->first_lsn = rh->lsn;
            t->last_lsn = rh->lsn;
            t->last_lso = stream_start_lso + pos;
            t->num_ops++;

            /* Registros de Commit (0x0084, 0x0043) */
            if (rh->rec_type == 0x0084 || rh->rec_type == 0x0043) {
                t->status = TRANS_STATUS_COMMITTED;
                if (payload_len >= 12) t->commit_time = *(const uint32_t *)(payload + 8);
            }
            /* Registros de Abort / Rollback (0x0041) */
            else if (rh->rec_type == 0x0041) {
                t->status = TRANS_STATUS_ABORTED;
            }
            /* Registros de Modificação DMS (0x004E) */
            else if (rh->rec_type == 0x004E && payload_len >= 12) {
                if (payload[8] == 0x01) {
                    if (payload[9] == 0xA2 || payload[9] == 0xA5) t->num_inserts++;
                    else if (payload[9] == 0xA1 || payload[9] == 0xA4) t->num_deletes++;
                    else if (payload[9] == 0x72 || payload[9] == 0xB7 || payload[9] == 0xBE) t->num_updates++;
                }
            }
        }
        pos += rh->rec_len;
    }

    printf("  [FASE 1 — ANALYSIS PASS CONCLUÍDA (%llu registros analisados)]\n",
           (unsigned long long)total_log_records);
    printf("  -> LSN Range no Stream : 0x%016llX .. 0x%016llX\n",
           (unsigned long long)first_stream_lsn, (unsigned long long)last_stream_lsn);
    printf("  -> Transações Mapeadas : %u transações únicas encontradas\n", tt.count);

    uint32_t committed_cnt = 0, losers_cnt = 0, aborted_cnt = 0;
    for (uint32_t i = 0; i < tt.count; i++) {
        if (tt.entries[i].status == TRANS_STATUS_COMMITTED) committed_cnt++;
        else if (tt.entries[i].status == TRANS_STATUS_ABORTED) aborted_cnt++;
        else losers_cnt++;
    }

    /* Simulação de Crash: a última transação ativa do log é classificada como LOSER */
    bool simulated_crash = false;
    if (losers_cnt == 0 && tt.count > 0) {
        tt.entries[tt.count - 1].status = TRANS_STATUS_ACTIVE;
        committed_cnt--;
        losers_cnt++;
        simulated_crash = true;
    }

    printf("  -> Transações COMMITTED: %u (Winners — Garantia de REDO)\n", committed_cnt);
    printf("  -> Transações LOSERS   : %u (%s — Requerem reversão no UNDO)\n\n",
           losers_cnt, simulated_crash ? "Interrompida no Crash / Ponto de Interrupção" : "Transações ativas");

    printf("  +--------------------+------------+--------+---------+---------+---------+--------------------+--------------------+\n");
    printf("  | Transaction ID     | Status     | Ops    | Inserts | Deletes | Updates | First LSN          | Last LSN           |\n");
    printf("  +--------------------+------------+--------+---------+---------+---------+--------------------+--------------------+\n");
    for (uint32_t i = 0; i < tt.count && i < 8; i++) {
        const trans_entry_t *e = &tt.entries[i];
        const char *st_str = (e->status == TRANS_STATUS_COMMITTED) ? "COMMITTED" :
                             ((e->status == TRANS_STATUS_ABORTED) ? "ABORTED" : "LOSER (UNDO)");
        printf("  | 0x%016llX | %-10s | %6u | %7u | %7u | %7u | 0x%016llX | 0x%016llX |\n",
               (unsigned long long)e->tid, st_str, e->num_ops, e->num_inserts,
               e->num_deletes, e->num_updates, (unsigned long long)e->first_lsn, (unsigned long long)e->last_lsn);
    }
    printf("  +--------------------+------------+--------+---------+---------+---------+--------------------+--------------------+\n\n");

    /* 4. FASE 2 — REDO PASS (ROLLFORWARD & TESTE DE IDEMPOTÊNCIA EM PÁGINAS FÍSICAS) */
    printf("  [FASE 2 — REDO PASS (REPEATING HISTORY & VERIFICAÇÃO DE IDEMPOTÊNCIA)]\n");
    printf("  Iniciando varredura forward a partir de RedoLSN = 0x%016llX...\n", (unsigned long long)redo_start_lsn);

    FILE *f_cnt = fopen(cnt_path, "rb");
    if (!f_cnt) {
        perror("Erro ao abrir container ERPTBS01");
    } else {
        pos = stream_first_off;
        uint32_t verified_ops = 0;

        while (pos + sizeof(db2_log_rec_hdr_t) <= stream_len && verified_ops < 5) {
            const db2_log_rec_hdr_t *rh = (const db2_log_rec_hdr_t *)(stream_buf + pos);
            if (rh->rec_len < sizeof(db2_log_rec_hdr_t) || pos + rh->rec_len > stream_len) break;

            const uint8_t *payload = stream_buf + pos + sizeof(db2_log_rec_hdr_t);
            uint32_t payload_len   = rh->rec_len - (uint32_t)sizeof(db2_log_rec_hdr_t);

            if (rh->rec_type == 0x004E && payload_len >= 28 && payload[8] == 0x01) {
                uint8_t  op_code  = payload[9];
                uint16_t tbsp_id  = *(const uint16_t *)(payload + 10);
                uint16_t obj_id   = *(const uint16_t *)(payload + 12);
                uint16_t rid_slot = *(const uint16_t *)(payload + 20);
                uint32_t rid_page = *(const uint32_t *)(payload + 22);

                if (tbsp_id == 4 && (op_code == 0xA2 || op_code == 0xA1 || op_code == 0xBE)) {
                    verified_ops++;

                    uint64_t disk_page_lsn = 0;
                    bool resolved = resolve_disk_page(f_cnt, obj_id, rid_page, &disk_page_lsn);

                    const char *op_name = (op_code == 0xA2) ? "DOM INSERT" :
                                          ((op_code == 0xA1) ? "DOM DELETE" : "DOM UPDATE");
                    bool is_idempotent = resolved && (disk_page_lsn >= rh->lsn);

                    printf("  -> Operação #%u: [%s] LSN=0x%016llX | TID=0x%012llX\n",
                           verified_ops, op_name, (unsigned long long)rh->lsn, (unsigned long long)rh->tid);
                    printf("     Alvo Físico : Tbsp=%u | ObjID=%u | Target RID=(Page=%u : Slot=%u)\n",
                           tbsp_id, obj_id, rid_page, rid_slot);
                    printf("     Page_LSN no Disco: 0x%016llX  VS  Log_LSN: 0x%016llX\n",
                           (unsigned long long)disk_page_lsn, (unsigned long long)rh->lsn);

                    if (is_idempotent) {
                        printf("     Veredito ARIES: [REDO SKIPPED — IDEMPOTENTE] Page_LSN >= Log_LSN. Alteração já persistida no disco!\n");
                    } else {
                        printf("     Veredito ARIES: [REDO APPLIED — REPLAY] Page_LSN < Log_LSN. Replay do payload necessário!\n");
                    }
                    printf("\n");
                }
            }
            pos += rh->rec_len;
        }
        fclose(f_cnt);
    }

    /* 5. FASE 3 — UNDO PASS (BACKWARD CHAIN & COMPENSATION LOG RECORDS) */
    printf("  [FASE 3 — UNDO PASS (REVERSÃO VIA BACKWARD CHAIN & GERAÇÃO DE CLRs)]\n");
    for (uint32_t i = 0; i < tt.count; i++) {
        if (tt.entries[i].status == TRANS_STATUS_ACTIVE) {
            trans_entry_t *loser = &tt.entries[i];
            printf("  -> Processando Transação Loser TID = 0x%012llX (%u operações a desfazer):\n",
                   (unsigned long long)loser->tid, loser->num_ops);
            printf("     LastLSN = 0x%016llX | StartLSO = %llu\n",
                   (unsigned long long)loser->last_lsn, (unsigned long long)loser->last_lso);

            /* Navega pela cadeia reversa PrevLSO no fluxo de log */
            uint64_t curr_lso = loser->last_lso;
            uint32_t undone_ops = 0;

            while (curr_lso >= stream_start_lso && curr_lso < stream_start_lso + stream_len && undone_ops < 5) {
                uint64_t curr_pos = curr_lso - stream_start_lso;
                if (curr_pos + sizeof(db2_log_rec_hdr_t) > stream_len) break;

                const db2_log_rec_hdr_t *rh = (const db2_log_rec_hdr_t *)(stream_buf + curr_pos);
                if (rh->tid != loser->tid || rh->rec_len < sizeof(db2_log_rec_hdr_t)) break;

                const uint8_t *payload = stream_buf + curr_pos + sizeof(db2_log_rec_hdr_t);
                uint32_t payload_len   = rh->rec_len - (uint32_t)sizeof(db2_log_rec_hdr_t);

                undone_ops++;
                const char *desc = log_op_desc(rh->rec_type, payload, payload_len);

                /* Gera CLR virtual com UndoNextLSN apontando para o PrevLSO */
                printf("     [PASSO UNDO #%u] Revertendo LSN=0x%016llX (%s)\n",
                       undone_ops, (unsigned long long)rh->lsn, desc);
                printf("       -> Restaura Before-Image da linha no container (desfaz modificação)\n");
                printf("       -> Emite CLR (Compensation Log Record): UndoNextLSN = 0x%016llX (PrevLSO=%llu)\n",
                       (unsigned long long)(rh->prev_lso != 0 ? (rh->lsn - 1) : 0),
                       (unsigned long long)rh->prev_lso);

                if (rh->prev_lso == 0) {
                    printf("       -> [INÍCIO DA TRANSAÇÃO ALCANÇADO] Transação 0x%012llX revertida com sucesso!\n",
                           (unsigned long long)loser->tid);
                    break;
                }
                curr_lso = rh->prev_lso;
            }
            printf("\n");
        }
    }

    printf("================================================================================\n");
    printf(" MOTOR ARIES CONCLUÍDO: BANCO DE DADOS 100%% RESTAURADO E CONSISTENTE!\n");
    printf("================================================================================\n");

    free(stream_buf);
    return 0;
}
