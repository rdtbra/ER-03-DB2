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
* **Controle de Logs e Transaction Logs (`SQLOGCTL.LFH.1` e `S000000X.LOG`):**
  * Decodificação do *Log File Header* (`SQLOGCTL.LFH.1`/`.2`), verificando `NextLSN`, `MinBuffLSN`, `LowTranLSN`, `CurrentLSO`, `LastTID` e a cadeia circular de arquivos `LOGPRIMARY`.
  * Remontagem do fluxo contínuo de bytes (*LSO Stream*) a partir das páginas de log de 4 KB (`IBMLOG`, 16B header + 4076B payload + 4B checksum), decodificando registros `DOM INSERT`, `IXM INSERT`, `DOM APPEND PAGE` (`Op=0x85`), `COMMIT` e correlacionando o `Page LSN` das páginas de dados diretamente ao registro no arquivo `.LOG`.

## Estrutura do Repositório

* [`DOCUMENTACAO_ENGENHARIA_REVERSA_DB2.md`](DOCUMENTACAO_ENGENHARIA_REVERSA_DB2.md): Documentação técnica detalhada de toda a investigação e mapeamento de bytes/offsets.
* [`db2_select.c`](db2_select.c): **Leitor Direto de Tabelas (`<BANCO> <SCHEMA> <TABELA>`)** — resolve o banco em `sqldbdir`, os storage paths em `SQLSGF.1`, as tablespaces em `SQLSPCS.1`, o catálogo em `SYSIBM.SYSTABLES` (`ObjID=5`) e `SYSIBM.SYSCOLUMNS` (`ObjID=6`), e lista os registros diretamente do container físico sem usar o motor SQL do Db2.
* [`db2_inspector.c`](db2_inspector.c): Ferramenta em C para inspeção estrutural profunda de containers, páginas de dados, árvores B+ de índices, arquivos `SQLSPCS.1`, `SQLOGCTL.LFH.1` e Transaction Logs (`S000000X.LOG`).
* [`Makefile`](Makefile): Script de compilação (`gcc -O2 -Wall -Wextra -std=c11`).

## Compilação e Uso

```bash
make

# A. Consultar os dados de qualquer tabela passando <BANCO> <SCHEMA> <TABELA> [LIMITE]
./db2_select ERP ERP                              # Lista todas as tabelas do schema ERP no banco ERP
./db2_select ERP ERP EMPRESA                      # Lista todos os dados da tabela ERP.EMPRESA
./db2_select ERP ERP FILIAL                       # Lista todos os dados da tabela ERP.FILIAL
./db2_select ERP ERP CLIENTE 15                   # Lista as primeiras 15 linhas de ERP.CLIENTE
./db2_select ERP ERP ITEM 15                      # Lista as primeiras 15 linhas de ERP.ITEM
./db2_select ERP SYSIBM SYSTABLESPACES            # Lista qualquer tabela do catálogo SYSIBM

# B. Inspecionar arquivos de controle e estruturas internas de baixo nível (db2_inspector)
./db2_inspector --sqlspcs /db2data_erp/db/erp/NODE0000/SQL00001/SQLSPCS.1
./db2_inspector --lfh /db2data_erp/db/erp/NODE0000/SQL00001/MEMBER0000/SQLOGCTL.LFH.1
./db2_inspector --log /db2data_erp/db/erp/NODE0000/SQL00001/LOGSTREAM0000/S0000002.LOG 0 6
./db2_inspector --log /db2data_erp/db/erp/NODE0000/SQL00001/LOGSTREAM0000/S0000003.LOG 0x63029 4
./db2_inspector /db2data_erp/data/erp/NODE0000/ERP/T0000004/C0000000.LRG
./db2_inspector /db2data_erp/data/erp/NODE0000/ERP/T0000004/C0000000.LRG 6 10
./db2_inspector /db2data_erp/data/catalog.dat 5 10
```
