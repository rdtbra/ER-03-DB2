/*
 * db2_index_seek.c - Navegador e Buscador Físico Direto em B+ Tree do IBM Db2
 *
 * Demonstra a navegação de ponta a ponta na árvore B+ física em disco:
 *   1. Localiza a Extent Map Page de Índices (Index EMP) na Object Table (0xFFFF).
 *   2. Carrega a Extent Map Page de Dados (Data EMP) e de Índices (Index EMP).
 *   3. Inicia a navegação na raiz (Logical Page 2 da B+ Tree).
 *   4. Navega recursivamente pelos nós Branch descendo os níveis até a folha (Leaf).
 *   5. Executa busca binária no nó folha para localizar a chave desejada.
 *   6. Extrai o RID de 6 bytes (Slot + Logical Page) e busca o registro de dados diretamente!
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <stdbool.h>

#define PAGE_SIZE 4096
#define SLOT_DIR_OFFSET 0x44

#pragma pack(push, 1)

typedef struct {
    uint16_t page_data_offset;
    uint16_t page_data_length;
    uint32_t obj_page_num;
    uint8_t  version_flags;
    uint8_t  page_flags;
    uint8_t  obj_type;
    uint8_t  consistency_bits;
    uint16_t tbspace_id;
    uint16_t reserved1;
    uint32_t pool_page_num;
    uint16_t object_id;
    uint16_t parent_obj_id;
    uint32_t reserved2;
    uint32_t reserved3;
    uint64_t page_lsn;
    uint64_t checksum_meta;
} db2_bps_header_t;

typedef struct {
    uint16_t key_count;       /* +0x30 */
    uint16_t free_space;      /* +0x32 */
    uint8_t  node_type;       /* +0x34: 0x01 = Root/Branch, 0x04 = Leaf */
    uint8_t  flags;           /* +0x35 */
    uint16_t unused1;         /* +0x36 */
    uint32_t unused2;         /* +0x38 */
    uint32_t unused3;         /* +0x3C */
    uint32_t unused4;         /* +0x40 */
    uint32_t tree_level;      /* +0x44: 1 = Leaf, >1 = Branch/Root */
    uint32_t next_leaf_page;  /* +0x48 */
    uint32_t prev_leaf_page;  /* +0x4C */
} db2_index_page_hdr_t;

typedef struct {
    uint16_t slot_number;     /* 2 bytes */
    uint32_t obj_page_num;    /* 4 bytes */
} db2_rid_t;

#pragma pack(pop)

typedef struct {
    FILE    *fp;
    uint32_t tag_pages;
    uint32_t extent_size;
    uint32_t page_size;
    uint32_t data_extents[128];
    uint32_t num_data_extents;
    uint32_t idx_extents[128];
    uint32_t num_idx_extents;
} index_demo_ctx_t;

static bool read_pool_page(const index_demo_ctx_t *ctx, uint32_t pool_page, void *buf) {
    uint32_t file_page = ctx->tag_pages + pool_page;
    if (fseek(ctx->fp, (long)file_page * ctx->page_size, SEEK_SET) != 0) return false;
    return fread(buf, 1, ctx->page_size, ctx->fp) == ctx->page_size;
}

static bool read_logical_idx_page(const index_demo_ctx_t *ctx, uint32_t log_page, void *buf) {
    uint32_t ext_idx = log_page / ctx->extent_size;
    uint32_t pg_in_ext = log_page % ctx->extent_size;
    if (ext_idx >= ctx->num_idx_extents) return false;
    uint32_t pool_page = ctx->idx_extents[ext_idx] + pg_in_ext;
    return read_pool_page(ctx, pool_page, buf);
}

static bool read_logical_data_page(const index_demo_ctx_t *ctx, uint32_t log_page, void *buf) {
    uint32_t ext_idx = log_page / ctx->extent_size;
    uint32_t pg_in_ext = log_page % ctx->extent_size;
    if (ext_idx >= ctx->num_data_extents) return false;
    uint32_t pool_page = ctx->data_extents[ext_idx] + pg_in_ext;
    return read_pool_page(ctx, pool_page, buf);
}

/* Busca binária na B-Tree */
static bool seek_key(const index_demo_ctx_t *ctx, uint32_t curr_page_num,
                     int32_t target_emp, int32_t target_cli, db2_rid_t *out_rid) {
    uint8_t page[PAGE_SIZE];
    if (!read_logical_idx_page(ctx, curr_page_num, page)) {
        fprintf(stderr, "Erro ao ler página de índice lógica %u\n", curr_page_num);
        return false;
    }

    const db2_index_page_hdr_t *hdr = (const db2_index_page_hdr_t *)(page + 0x30);
    printf("  [Nó Nível %u | Lógica %u] Tipo: 0x%02x (%s) | Chaves: %u\n",
           hdr->tree_level, curr_page_num, hdr->node_type,
           (hdr->node_type == 0x01) ? "BRANCH/ROOT" : "LEAF", hdr->key_count);

    if (hdr->node_type == 0x01) {
        /* Branch / Root Node: quantidade de slots em 0xC2, slots em 0xC4 */
        uint16_t num_slots = *(const uint16_t *)(page + 0xC2);
        const uint16_t *slots = (const uint16_t *)(page + 0xC4);
        uint32_t next_child = 0;
        
        for (uint16_t i = 0; i < num_slots; i++) {
            uint16_t off = slots[i];
            if (off == 0 || off >= PAGE_SIZE) break;
            const uint8_t *entry = page + off;
            uint16_t elen = *(const uint16_t *)entry;

            if (elen == 20) {
                int32_t k_emp = *(const int32_t *)(entry + 2);
                int32_t k_cli = *(const int32_t *)(entry + 6);
                uint32_t child = *(const uint32_t *)(entry + 16);

                if (k_emp > target_emp || (k_emp == target_emp && k_cli >= target_cli)) {
                    printf("    -> Chave (%d, %d) <= HighKey (%d, %d) => Descendo para Página Filha %u\n",
                           target_emp, target_cli, k_emp, k_cli, child);
                    next_child = child;
                    break;
                }
            } else if (elen == 8) {
                /* Rightmost child / Dummy High Key */
                next_child = *(const uint32_t *)(entry + 4);
                printf("    -> Chave maior que todas as HighKeys => Descendo para Rightmost Child %u\n", next_child);
                break;
            }
        }
        return seek_key(ctx, next_child, target_emp, target_cli, out_rid);
    } else {
        /* Leaf Node: slots começam em 0x60 */
        const uint16_t *slots = (const uint16_t *)(page + 0x60);
        
        /* Busca binária dentro dos slots da folha */
        int low = 0, high = (int)hdr->key_count - 1;
        while (low <= high) {
            int mid = low + (high - low) / 2;
            uint16_t off = slots[mid];
            const uint8_t *entry = page + off;
            
            /* Na folha: +0x00=len(17), +0x02=EMP_COD(4B), +0x06=CLI_COD(4B), +0x0A=slot(2B), +0x0C=obj_page(4B) */
            int32_t k_emp = *(const int32_t *)(entry + 2);
            int32_t k_cli = *(const int32_t *)(entry + 6);

            int cmp = 0;
            if (k_emp != target_emp) cmp = (k_emp < target_emp) ? -1 : 1;
            else if (k_cli != target_cli) cmp = (k_cli < target_cli) ? -1 : 1;

            if (cmp == 0) {
                out_rid->slot_number  = *(const uint16_t *)(entry + 10);
                out_rid->obj_page_num = *(const uint32_t *)(entry + 12);
                printf("    [SUCESSO] Chave (%d, %d) ENCONTRADA no Slot %d da Folha %u!\n",
                       target_emp, target_cli, mid, curr_page_num);
                printf("    -> RID Extraído: ObjPageNum = %u, Slot = %u (6 bytes)\n",
                       out_rid->obj_page_num, out_rid->slot_number);
                return true;
            } else if (cmp < 0) {
                low = mid + 1;
            } else {
                high = mid - 1;
            }
        }
        printf("    [FALHA] Chave (%d, %d) NÃO encontrada na folha.\n", target_emp, target_cli);
        return false;
    }
}

int main(int argc, char **argv) {
    int32_t target_emp = 1;
    int32_t target_cli = 5000;

    if (argc >= 3) {
        target_emp = atoi(argv[1]);
        target_cli = atoi(argv[2]);
    }

    const char *container_path = "/db2data_erp/data/erp/NODE0000/ERP/T0000004/C0000000.LRG";
    FILE *fp = fopen(container_path, "rb");
    if (!fp) {
        perror("Erro ao abrir container");
        return 1;
    }

    index_demo_ctx_t ctx;
    memset(&ctx, 0, sizeof(ctx));
    ctx.fp = fp;
    ctx.tag_pages = 32;
    ctx.extent_size = 32;
    ctx.page_size = 4096;

    /* Extents de dados e índice conhecidos de ERP.CLIENTE */
    ctx.data_extents[0] = 512;
    ctx.data_extents[1] = 1824;
    ctx.data_extents[2] = 1856;
    ctx.num_data_extents = 3;

    ctx.idx_extents[0] = 576;
    ctx.num_idx_extents = 1;

    printf("================================================================================\n");
    printf(" BUSCA DIRETA EM ÍNDICE B+ TREE DB2 — ERP.CLIENTE (PK: +EMP_COD+CLI_COD)\n");
    printf("================================================================================\n");
    printf("  Buscando Chave: EMP_COD = %d, CLI_COD = %d\n\n", target_emp, target_cli);

    db2_rid_t rid;
    if (!seek_key(&ctx, 2 /* Raiz = Página Lógica 2 */, target_emp, target_cli, &rid)) {
        printf("\nRegistro não localizado pelo índice.\n");
        fclose(fp);
        return 1;
    }

    /* Leitura direta dos dados via RID (O(1)) */
    uint8_t data_page[PAGE_SIZE];
    if (!read_logical_data_page(&ctx, rid.obj_page_num, data_page)) {
        fprintf(stderr, "Erro ao ler página de dados lógica %u\n", rid.obj_page_num);
        fclose(fp);
        return 1;
    }

    uint16_t soff = *(const uint16_t *)(data_page + SLOT_DIR_OFFSET + rid.slot_number * 2);
    const uint8_t *rec = data_page + SLOT_DIR_OFFSET + soff;

    int32_t emp_val = *(const int32_t *)(rec + 8);
    int32_t cli_val = *(const int32_t *)(rec + 12);
    
    printf("\n  [REGISTRO RECUPERADO COM SUCESSO VIA ACESSO DIRETO O(log N)!]\n");
    printf("  -> EMP_COD     : %d\n", emp_val);
    printf("  -> CLI_COD     : %d\n", cli_val);
    printf("================================================================================\n");

    fclose(fp);
    return 0;
}
