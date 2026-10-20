# ER-03-DB2 — Engenharia Reversa do Servidor IBM Db2 11.5 (Linux x86_64)

Repositório de estudo prático e engenharia reversa das estruturas internas do **IBM Db2 v11.5.0.0**, incluindo decodificação direta em C (sem uso de bibliotecas ou motor SQL do Db2) de:

* **Arquivos de Controle de Tablespaces (`SQLSPCS.1` / `SQLSPCS.2`):** Slots fixos de 256 KB (`0x40000`), flags de gerenciamento DMS / Automatic Storage e detecção de tablespaces removidas (`DROP TABLESPACE`).
* **Containers DMS e Automatic Storage (`.LRG`, `.USR`, `.CAT`, `.dat`):**
  * *Container Tag* (`DB2CONT` no offset `0x200`).
  * *Space Map Pages (SMP)* (`Object ID = 0xFFFE`, `Type = 0x0E`), bitmap de alocação de 4 bits por *extent* e *High Water Mark (HWM)*.
  * *Object Table* (`Object ID = 0xFFFF`, `Type = 0x00`) e *Extent Map Pages (EMP)* (`Type = 0x40` Dados / `0x41` Índices).
* **Páginas de Dados (`Type = 0x00`) e Registros `FIXEDVAR`:**
  * Cabeçalhos *BPS Page Header* (48B), *Data Page Header* (20B) e *Slot Directory* (`Offset Real = 0x44 + slot_offset`).
  * Reconstrução automática do schema físico da tabela a partir do **Slot 3** (*Table Description Record*) da Página 0.
  * Decodificação de registros `FIXEDVAR` (`SMALLINT`, `INTEGER`, `BIGINT`, `CHAR`, `VARCHAR`, `DATE` em Packed BCD, `TIMESTAMP` em Packed BCD, `BLOB`/`CLOB`, além de registros de *Overflow Pointer* `0x0042` e *Overflow Target* `0x00C4`).
* **Objetos de Índice B+Tree (`Type = 0x01`):**
  * *Index SMP* (`ObjPg 0`), *MetaIndex* (`ObjPg 1`), *Index Root Vector* (`"RT10"`), *Info Page* (`ObjPg 3`), descida multinível pela árvore B+ (*Root $\rightarrow$ Internal $\rightarrow$ Leaf*) e travessia da cadeia duplamente encadeada de páginas folha (*Leaf Chain*) extraindo chaves e ponteiros `RID(Page:Slot)`.

## Estrutura do Repositório

* [`DOCUMENTACAO_ENGENHARIA_REVERSA_DB2.md`](DOCUMENTACAO_ENGENHARIA_REVERSA_DB2.md): Documentação técnica detalhada de toda a investigação e mapeamento de bytes/offsets.
* [`db2_inspector.c`](db2_inspector.c): Ferramenta em C para inspeção direta de containers, páginas de dados, árvores B+ de índices e arquivos `SQLSPCS.1`.
* [`Makefile`](Makefile): Script de compilação (`gcc -O2 -Wall -Wextra -std=c11`).

## Compilação e Uso

```bash
make

# 1. Inspecionar o arquivo binário de controle de tablespaces (SQLSPCS.1)
./db2_inspector --sqlspcs /db2data_erp/db/erp/NODE0000/SQL00001/SQLSPCS.1

# 2. Inspecionar um container completo (Tag, SMP, Object Table, Tabelas e Índices B+Tree)
./db2_inspector /db2data_erp/data/erp/NODE0000/ERP/T0000004/C0000000.LRG

# 3. Inspecionar um Object ID específico (ex: 6 = ERP.ITEM, exibindo as primeiras 10 linhas/chaves)
./db2_inspector /db2data_erp/data/erp/NODE0000/ERP/T0000004/C0000000.LRG 6 10

# 4. Inspecionar tabelas do catálogo do sistema diretamente no catalog.dat (ex: 5 = SYSIBM.SYSTABLES)
./db2_inspector /db2data_erp/data/catalog.dat 5 10
```
