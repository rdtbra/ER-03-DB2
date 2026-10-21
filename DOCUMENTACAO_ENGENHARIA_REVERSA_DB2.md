# Documentação de Engenharia Reversa — IBM Db2 v11.5.0.0 (Linux x86_64)

> **Objetivo:** Mapeamento arquitetural e engenharia reversa completa das estruturas físicas em disco e em memória do servidor IBM Db2 instalado nesta máquina (binários, instância, banco de dados `ERP`, arquivos de controle, *tablespaces*, *containers*, páginas de dados, árvores B+ de índices e *transaction logs*), acompanhado do desenvolvimento da ferramenta em C [`db2_inspector.c`](file:///desenv/db2/db2_inspector.c) que lê e decodifica diretamente os arquivos binários do Db2 sem usar bibliotecas ou SQL do SGBD.

---

## Sumário

1. [Instalação dos Binários e Registro Global](#1-instalação-dos-binários-e-registro-global)
2. [A Instância `erp` e Arquitetura de Processos/Threads (EDUs)](#2-a-instância-erp-e-arquitetura-de-processosthreads-edus)
3. [Topologia Física do Banco `ERP` (`DBPATH` vs. `STORAGE PATH`)](#3-topologia-física-do-banco-erp-dbpath-vs-storage-path)
4. [Engenharia Reversa do Arquivo de Controle de Tablespaces (`SQLSPCS.1`)](#4-engenharia-reversa-do-arquivo-de-controle-de-tablespaces-sqlspcs1)
5. [Anatomia Física dos Containers DMS / Automatic Storage](#5-anatomia-física-dos-containers-dms--automatic-storage)
6. [Anatomia de Páginas de Dados (`Type = 0x00`) e Registros `FIXEDVAR`](#6-anatomia-de-páginas-de-dados-type--0x00-e-registros-fixedvar)
7. [Anatomia do Objeto de Índice B+Tree (`Type = 0x01`)](#7-anatomia-do-objeto-de-índice-btree-type--0x01)
8. [Engenharia Reversa de Transaction Logs (`SQLOGCTL.LFH` e `S000000X.LOG`)](#8-engenharia-reversa-de-transaction-logs-sqlogctllfh-e-s000000xlog)
9. [Ferramenta Completa Desenvolvida em C (`db2_inspector.c`)](#9-ferramenta-completa-desenvolvida-em-c-db2_inspectorc)
10. [Leitor Direto de Tabelas por Nome (`db2_select.c` — `<BANCO> <SCHEMA> <TABELA>`)](#10-leitor-direto-de-tabelas-por-nome-db2_selectc--banco-schema-tabela)
11. [Engenharia Reversa do Checksum de Página (`sqlbCalculatePageCheckSum`) e Patch Cirúrgico no Catálogo (`db2_rename_idx.c`)](#11-engenharia-reversa-do-checksum-de-página-sqlbcalculatepagechecksum-e-patch-cirúrgico-no-catálogo-db2_rename_idxc)
12. [Engenharia Reversa das Operações `INSERT`, `DELETE`, `UPDATE` e `RE-INSERT` (`libdb2e.so.1` + Transaction Log)](#12-engenharia-reversa-das-operações-insert-delete-update-e-re-insert-libdb2eso1--transaction-log)
13. [Topologia Multi-Container e Algoritmo de Striping (Round-Robin de Extents)](#13-topologia-multi-container-e-algoritmo-de-striping-round-robin-de-extents)
14. [Anatomia e Parametrização Universal de Páginas de 8KB, 16KB e 32KB](#14-anatomia-e-parametrização-universal-de-páginas-de-8kb-16kb-e-32kb)
15. [Engenharia Reversa da Compressão de Linhas (Static & Adaptive Row Compression)](#15-engenharia-reversa-da-compressão-de-linhas-static--adaptive-row-compression)

---

## 1. Instalação dos Binários e Registro Global

* **Versão instalada:** IBM Db2 v11.5.0.0 (64 bits, `SQL11050`, Build `s1906101300`, Fix Pack `0`)
* **Diretório de instalação dos binários:** `/opt/ibm/db2/V11.5`
* **Utilitários globais em `/usr/local/bin`:** `db2ls`, `db2greg`
* **Registro Global (`/var/db2/global.reg` via `db2greg -dump`):**
  ```text
  S,TSA,4.1.0.4,/opt/IBM/tsamp,DG_NOT_ALLOWED,DB2_INSTALLED,0,0,-,1692555071,0
  S,RSCT,3.2.3.2,/usr/sbin/rsct,DG_NOT_ALLOWED,DB2_INSTALLED,0,0,-,1692555071,0
  S,DB2,11.5.0.0,/opt/ibm/db2/V11.5,,,0,0,,1692555135,0
  V,DB2GPRF,DB2SYSTEM,vm2074-vmwarenet,/opt/ibm/db2/V11.5,
  I,DB2,11.5.0.0,erp,/home/erp/sqllib,,1,0,/opt/ibm/db2/V11.5,,
  ```

---

## 2. A Instância `erp` e Arquitetura de Processos/Threads (EDUs)

* **Usuário / Grupo SO:** `erp` / `db2`
* **Diretório da instância (`INSTHOME/sqllib`):** `/home/erp/sqllib`
* **Arquivos críticos da instância:**
  * [`/home/erp/sqllib/db2systm`](file:///home/erp/sqllib/db2systm) (28.672 bytes): Arquivo binário de configuração da instância (**DBM CFG** — `db2 get dbm cfg`).
  * [`/home/erp/sqllib/db2nodes.cfg`](file:///home/erp/sqllib/db2nodes.cfg): Topologia de nós (`0 vm2074-vmwarenet 0` — partição única, Member 0).
  * [`/home/erp/sqllib/profile.env`](file:///home/erp/sqllib/profile.env): Registro de variáveis da instância (`DB2ENVLIST=INSTHOME`).
  * [`/home/erp/sqllib/sqldbdir/sqldbdir`](file:///home/erp/sqllib/sqldbdir/sqldbdir): **System Database Directory** (catálogo de bancos da instância).
  * `/home/erp/sqllib/db2dump/`: Diretório de logs de diagnóstico (`db2diag.log`, dumps FODC, relatórios `db2dart`).

### Processos e Threads (*Engine Dispatchable Units* — EDUs)
Quando a instância é iniciada (`db2start`) e o banco `ERP` está ativo (`db2pd -edus`):
* **`db2fmcd`:** Fault Monitor Coordinator Daemon (`/opt/ibm/db2/V11.5/bin/db2fmcd`, rodando como `root`).
* **`db2wdog`:** Watchdog supervisor da instância.
* **`db2sysc`:** Processo principal do motor Db2. Todas as tarefas internas do banco rodam como **threads de kernel (EDUs)** dentro do processo `db2sysc`:
  * `db2ipccm` / `db2tcpcm`: Listeners de comunicação IPC (memória compartilhada local) e TCP/IP.
  * `db2agent (ERP)`: Thread agente que atende conexões de aplicações e executa planos SQL.
  * `db2pfchr (ERP)` (12 threads): *Prefetchers* assíncronos que leem extents dos containers de dados/índices antecipadamente para o bufferpool.
  * `db2pclnr (ERP)` (4 threads): *Page Cleaners* assíncronos que descarregam páginas sujas (*dirty pages*) do bufferpool para os containers.
  * `db2loggr (ERP)` / `db2loggw (ERP)` / `db2lfr.0 (ERP)`: Gerenciador de log, gravador de log (*Log Writer* que esvazia o *Log Buffer* nos arquivos `.LOG`) e leitor de arquivos de log.
  * `db2dlock (ERP)`: Detector de *deadlocks*.
  * `db2stmm (ERP)`: *Self-Tuning Memory Manager*.

---

## 3. Topologia Física do Banco `ERP` (`DBPATH` vs. `STORAGE PATH`)

O banco `ERP` foi criado a partir dos scripts em `/home/erp/dba/erp` ([`cr_erp.sql`](file:///home/erp/dba/erp/cr_erp.sql)) separando o caminho de metadados/controle (`DBPATH`) do caminho de armazenamento de dados (`STORAGE PATH`):

* **Metadados de Controle e Logs (`DBPATH`):** `/db2data_erp/db/erp/NODE0000/SQL00001/`
* **Containers de Dados (`STORAGE PATH`):** `/db2data_erp/data/`

### Arquivos de Controle em `/db2data_erp/db/erp/NODE0000/SQL00001/`

| Arquivo | Tamanho | Função Físico-Estrutural |
| :--- | :---: | :--- |
| `SQLDBCONF` | 16.384 B | **Database Configuration (DB CFG):** Parâmetros do banco (`LOGFILSIZ=1024`, `DFT_EXTENT_SZ=32`, codepage `1208` UTF-8, etc.). |
| `SQLSPCS.1` / `.2` | 1.310.720 B | **Tablespace Control File (duplicado):** 5 slots de 256 KB (`0x40000`) descrevendo as tablespaces e caminhos de seus containers. |
| `SQLSGF.1` / `.2` | 135.168 B | **Storage Group Control File (duplicado):** Assinatura `DBSTOGR`. Define o grupo `IBMSTOGROUP` (`SGID=0`) apontando para `/db2data_erp/data`. |
| `SQLOGCTL.GLFH.1` / `.2` | 24.576 B | **Global Log File Header (duplicado):** Controle global de cadeia de logs e LSNs de recuperação. |
| `MEMBER0000/SQLOGCTL.LFH.1` / `.2` | 24.576 B | **Member Log File Header (duplicado):** Ponteiro para o log ativo (`S0000006.LOG`), `MinBuffLSN` e `LowTranLSN` para *Crash Recovery*. |
| `MEMBER0000/SQLOGMIR.LFH` | 8.192 B | Controle de espelhamento do LFH. |
| `MEMBER0000/SQLBP.1` / `.2` | 512 B | **Bufferpool Control File (duplicado):** Define o bufferpool inicial (`IBMDEFAULTBP`) necessário para montar o catálogo no *startup*. |
| `MEMBER0000/SQLINSLK` | 9 B | **Instance Lock:** Trava física de posse do banco pela instância ativa. |
| `db2rhist.asc` / `.bak` | 6.833 B | **Recovery History File:** Histórico cronológico de DDL de tablespaces, backups, restores e loads. |
| `LOGSTREAM0000/S0000000.LOG` .. `S0000012.LOG` | 4.202.496 B (cada) | **13 arquivos de Transaction Log** (1024 páginas de 4 KB = 4 MB de dados + 8 KB de cabeçalho `IBMLOG`). |

---

## 4. Engenharia Reversa do Arquivo de Controle de Tablespaces (`SQLSPCS.1`)

O arquivo `SQLSPCS.1` (`1.310.720` bytes) é um array de structs de **262.144 bytes (`0x40000` = 256 KB)** indexado pelo `Tablespace ID`:

| Offset no Slot (`0x40000`) | Tipo | Descrição |
| :--- | :--- | :--- |
| `+0x0000` | `uint32_t` | Versão do registro (`9`) |
| `+0x0004` | `uint32_t` | Checksum CRC32 do slot |
| `+0x0008` | `int16_t` | **Tablespace ID** (`0..N` quando ativa; **`-1` / `0xFFFF` quando removida via `DROP TABLESPACE`**) |
| `+0x000A` | `char[128]` | Nome da tablespace |
| `+0x008C` | `uint16_t` | Flags: `0x2102` (DMS Regular), `0x6112` (DMS System Temp), `0x3122` (DMS Large Automatic Storage) |
| `+0x0098` | `uint32_t` | `ExtentSize` (em páginas) |
| `+0x009C` | `uint32_t` | `PrefetchSize` (em páginas) |
| `+0x03E0` | `uint64_t` | `Creation LSN` |
| `+0x0430` | `uint32_t` | `TotalPages` da tablespace |
| `+0x0434` | `uint32_t` | `UseablePages` da tablespace |
| `+0x0448` | `uint32_t` | Quantidade de containers (`num_containers`) |
| `+0x0458` | `uint32_t` | `Bufferpool ID` (`1`) |
| `+0x045C` | `uint32_t` | `PageSize` em bytes (`4096`) |
| `+0x04A4` | `uint32_t` | `InitSize` em páginas (`8192` = 32 MB) |
| `+0x04AC` | `uint32_t` | `MaxSize` em páginas (`26.214.400` = 100 GB na `ERPTBS01`, ou `0xFFFFFFFF` = Ilimitado) |
| `+0x04F2` | `int16_t` | `Storage Group ID` (`0` = `IBMSTOGROUP`) |
| `+0xC000` | `struct[]` | **Descritores de Containers:** `+0x00` Tipo (`6`=File), `+0x04` `TotalPages`, `+0x08` `UseablePages`, `+0x30` `PathID` (`-1` ou `0`), `+0x34` `char[256]` caminho do arquivo |

### As 5 entradas descobertas no `SQLSPCS.1` do banco `ERP`:
1. **Slot #0 (`0x000000`) — `SYSCATSPACE` (`ID=0`, Ativa):** DMS Regular, Extent 32, `59.136` páginas totais (`59.104` usáveis), container explícito `/db2data_erp/data/catalog.dat`.
2. **Slot #1 (`0x040000`) — `TEMPSPACE1` (`ID=1`, Ativa):** DMS System Temp, Extent 32, `10.000` páginas totais (`9.952` usáveis), container explícito `/db2data_erp/data/temp.dat`.
3. **Slot #2 (`0x080000`) — `USERSPACE1` (`ID=-1 / 0xFFFF`, DROPPED):** Criada originalmente com Automatic Storage em `/db2data_erp/data/erp/NODE0000/ERP/T0000002/C0000000.LRG` e posteriormente removida com `DROP TABLESPACE USERSPACE1`. O Db2 marcou `tbsp_id = -1` e zerou `TotalPages`/`UseablePages` no cabeçalho do slot, preservando o registro histórico no offset `+0xC000`.
4. **Slot #3 (`0x0C0000`) — `SYSTOOLSPACE` (`ID=3`, Ativa):** DMS Large (AutoStorage), Extent **4**, `8.192` páginas totais (`8.188` usáveis), container `/db2data_erp/data/erp/NODE0000/ERP/T0000003/C0000000.LRG`.
5. **Slot #4 (`0x100000`) — `ERPTBS01` (`ID=4`, Ativa):** DMS Large (AutoStorage), Extent **32**, `8.192` páginas totais (`8.160` usáveis), container `/db2data_erp/data/erp/NODE0000/ERP/T0000004/C0000000.LRG`.

---

## 5. Anatomia Física dos Containers DMS / Automatic Storage

Dentro de qualquer arquivo de container (`.LRG`, `.dat`):

1. **Extent 0 Físico do Arquivo (`File Pages 0 .. tag_pages-1`) — Container Tag (`DB2CONT`):**
   * O primeiro extent inteiro do arquivo (`32` páginas = 128 KB na `ERPTBS01` e `SYSCATSPACE`; `4` páginas = 16 KB na `SYSTOOLSPACE`) é reservado para o **Container Tag**, reduzindo `UseablePages = TotalPages - ExtentSize`.
   * No offset **`0x0200`** (512 bytes) encontra-se a assinatura `"DB2CONT\0"`, seguida do `Tablespace ID` em `0x0210` (`uint32_t`), nome do banco (`"ERP     "` em `0x0226`), instância (`"erp"` em `0x022F`) e caminho de controle (`"/db2data_erp/db/erp/NODE0000/SQL00001/"` em `0x0242`).
2. **Pool Page 0 (`File Page = tag_pages`) — Meta-SMP:**
   * Possui `object_id = 0xFFFE`, `obj_type = 0x0E`.
   * No offset `+0x34` contém um `uint32_t` apontando para a `Pool Page` da **Space Map Page (SMP) ativa** (que coincide com `1 * extent_size`).
3. **Pool Page `extent_size` (ex.: `Pool Page 32`) — Space Map Page (SMP) Ativa:**
   * `+0x38`: `uint32_t last_object_id` (último ID de objeto alocado na tablespace).
   * `+0x3C`: `uint32_t hwm_extents` (**High Water Mark** em extents).
   * `+0x58`: Array de palavras `uint32_t` onde **cada *nibble* de 4 bits (do MSB para o LSB)** codifica o estado de 1 extent da tablespace:
     * `0x0`: Extent livre (`FREE`)
     * `0x1`: Extent alocado (`USED`)
     * `0x2` / `0x3`: Extent pendente de liberação (`PENDING FREE`)
4. **Pool Page 1 (`File Page = tag_pages + 1`) — EMP da Object Table (`Object ID = 0xFFFF`, `Type = 0x40`):**
   * Fica dentro do próprio Extent 0 da tablespace (logo após a `Pool Page 0`) e funciona como a página **EMP (*Extent Map Page*)** da *Object Table*.
   * Em `+0x3C` informa quantos extents a *Object Table* possui e em `+0x58` lista as `Pool Pages` iniciais de cada extent da *Object Table* (sempre iniciando na `Pool Page 64` em tablespaces de extent 32).
5. **Object Table (`Object ID = 0xFFFF`, `Type = 0x00`, iniciando na `Pool Page 64`):**
   * É uma tabela interna de páginas de dados cujos registros (`rec_type = 0x0010`, `rec_len = 0x0098 = 152` bytes) mapeiam cada `Object ID` criado na tablespace para suas páginas de mapa de extents (**EMP**):
     * `rec + 0x08`: `uint16_t tbsp_id`
     * `rec + 0x0A`: `uint16_t object_id`
     * `rec + 0x0C`: `uint32_t data_emp_pool_page` (página EMP `Type = 0x40` que lista os extents de dados da tabela)
     * `rec + 0x30`: `uint32_t index_emp_pool_page` (página EMP `Type = 0x41` que lista os extents de índice da tabela)
     * `rec + 0x60`: `uint32_t lob_emp_pool_page` (página EMP para LOBs, se houver)
6. **Páginas EMP (`Type = 0x40` para Dados, `Type = 0x41` para Índices):**
   * `+0x3C`: `uint32_t num_extents` alocados para aquele segmento do objeto.
   * `+0x58`: `uint32_t extent_pool_pages[num_extents]` — array ordenado contendo a `Pool Page` inicial de cada extent lógico `0 .. num_extents-1` do objeto.

---

## 6. Anatomia de Páginas de Dados (`Type = 0x00`) e Registros `FIXEDVAR`

### Estrutura da Página de 4.096 Bytes
* **`0x00..0x2F` (48 bytes) — BPS Page Header (`db2_bps_header_t`):**
  * `+0x00`: `uint16_t page_data_offset = 48 (0x30)`
  * `+0x02`: `uint16_t page_data_length = 4048 (0x0FD0)`
  * `+0x04`: `uint32_t obj_page_num` (página lógica `0..N-1` dentro da tabela)
  * `+0x0A`: `uint8_t obj_type` (`0x00` = Data, `0x01` = Index, `0x0E` = SMP, `0x40` = Data EMP, `0x41` = Index EMP)
  * `+0x0C`: `uint16_t tbspace_id`
  * `+0x10`: `uint32_t pool_page_num` (`File Page = Pool Page + tag_pages`)
  * `+0x14`: `uint16_t object_id`
  * `+0x20`: `uint64_t page_lsn` (**Log Sequence Number** da última atualização da página — controle WAL)
* **`0x30..0x43` (20 bytes) — Data Page Header (`db2_data_page_hdr_t`):**
  * `+0x30`: `uint16_t slot_count` (quantidade de entradas no diretório de slots)
  * `+0x32`: `uint16_t total_free_space`
  * `+0x38`: `uint16_t max_record_size`
  * `+0x3A`: `uint16_t free_space_offset`
* **`0x44..` (a partir do byte 68) — Slot Directory (`uint16_t slots[slot_count]`):**
  * Cada entrada `s` guarda um `uint16_t slot_offset` **relativo ao fim do Data Page Header (`0x44 = 68`)**.
  * Endereço físico exato do registro na página de 4096 bytes:
    $$\text{Offset na Página} = \mathtt{0x44} + \mathtt{slots}[s]$$
  * Slots excluídos são marcados com `0xFFFF` (ou `0x0000`).

### Os 4 Slots de Metadados na Página 0 de toda Tabela
* **Slot 0 (`0x0FE0` no disco):** *Data Object Header Control Record* (32 bytes, `rec_type = 0x0001`). O `uint32_t` em `+4` informa o **total exato de páginas usadas pelo objeto de dados** (`1` em `EMPRESA`, `1` em `FILIAL`, `981` em `ITEM`, `86` em `CLIENTE`, `61` em `SYSTABLES`, `368` em `SYSCOLUMNS`).
* **Slot 1 (`0x0BF4` no disco):** *Free Space Control Record (FSCR)* (1.004 bytes, `rec_type = 0x0001`). Array de 500 `uint16_t` indicando o espaço livre aproximado nas 500 páginas seguintes (repete-se no Slot 0 das páginas múltiplas de 500).
* **Slot 2 (`0x0BA8` no disco):** *Table Directory Record (TDIR)* (76 bytes, `rec_type = 0x0010`). Aponta para `MetaIndex Root Page = 1`.
* **Slot 3:** *Table Description Record* (`rec_type = 0x0000`, `rec_flags = 0x0000`). Contém `uint16_t num_cols` no offset `+6`, seguido de `num_cols` descritores de **8 bytes por coluna** (`db2_col_desc_t`):
  * `+0x00` (`uint16_t col_type`): `0x0000`=`SMALLINT`, `0x0001`=`INTEGER`, `0x0003`=`REAL`, `0x0004`=`DOUBLE`, `0x0005`=`BIGINT`, `0x0100`=`CHAR`, `0x0101`=`VARCHAR`, `0x0105`=`DATE`, `0x0107`=`TIMESTAMP`, `0x0108`=`BLOB`, `0x0109`=`CLOB`.
  * `+0x02` (`uint16_t col_len`): Tamanho em bytes.
  * `+0x04` (`uint16_t null_flags`): `0x0001` = **NULLABLE** (acrescenta 1 byte de indicador de nulidade após os bytes fixos da coluna: `0x00` = valor presente, `0x01` = `NULL`); `0x0002` = **NOT NULL**.
  * `+0x06` (`uint16_t fixed_offset`): Posição da coluna na parte fixa do registro (a partir de `rec + 8`).

### Formato dos Registros de Dados (`FIXEDVAR`)
* **Cabeçalho de 8 bytes (`rec + 0..7`):**
  * `+0x00`: `uint16_t rec_type` — os 3 bits inferiores (`rec_type & 0x07`) definem o tipo físico:
    * `0x00`: Registro normal de dados (`FIXEDVAR`; o bit `0x10` é ligado quando há múltiplos descritores LOB não-nulos, como nas Views em `SYSIBM.SYSTABLES`).
    * `0x02` (ex.: `0x0042`, 12 bytes): **Overflow Pointer Record** (*stub* deixado no `RID` original contendo o `RID(Page, Slot)` de destino quando um `UPDATE` moveu a linha de página).
    * `0x04` (ex.: `0x00C4`): **Overflow Target Data Record** (o registro real de dados gravado na nova página).
  * `+0x02`: `uint16_t rec_len` (tamanho total do registro em bytes).
  * `+0x04`: `uint16_t rec_flags` (`0x0001` para linhas de dados de usuário).
  * `+0x06`: `uint16_t fixed_part_len` (tamanho da seção fixa).
* **Codificação dos tipos de dados a partir de `fixed_base = rec + 8`:**
  * `SMALLINT` / `INTEGER` / `BIGINT`: Inteiros de 2, 4 e 8 bytes em *little-endian*.
  * `CHAR(n)`: String fixa de `n` bytes preenchida com espaços (`0x20`).
  * `VARCHAR(n)`: Na parte fixa (`fixed_offset`), armazena 4 bytes: `uint16_t var_offset` (relativo a `rec + 8`) e `uint16_t var_len`. Os caracteres reais ficam em `rec + 8 + var_offset`.
  * `DATE`: 4 bytes em **Packed BCD** (`YYYYMMDD`, ex.: `20 24 09 22` = `2024-09-22`).
  * `TIMESTAMP`: 10 bytes em **Packed BCD** (`YYYYMMDDHHMMSSuuuuuu`, ex.: `20 23 08 20 16 06 39 00 93 43` = `2023-08-20 16:06:39.009343`).

---

## 7. Anatomia do Objeto de Índice B+Tree (`Type = 0x01`)

Todos os índices de uma tabela residem no mesmo Objeto de Índice (`Type = 0x01`), cujos extents são listados pelo `Index EMP` (`Type = 0x41`):

1. **Página Lógica 0 (`ObjPg 0`) — Index SMP (*Free Page Map*):**
   * `+0x34`: `Page Type = 0x01000210`.
   * `+0x38`: `uint32_t allocated_pages` | `+0x3C`: `uint32_t used_pages`.
   * `+0x80`: Array de 1 byte por página lógica do índice (`0x10` = página em uso `U`, `0x00` = livre `.`).
2. **Página Lógica 1 (`ObjPg 1`) — MetaIndex Root Page:**
   * Árvore B+ interna que mapeia `(KeyPart1 = 0, KeyPart2 = IndexID)` para a `ObjPg` raiz daquele índice (ex.: `IndexID = 1` $\rightarrow$ `Root ObjPg = 2`).
3. **Cabeçalho de Nó B+Tree (`+0x30..+0x53`, 36 bytes — `db2_idx_node_hdr_t`):**
   * `+0x30`: `uint16_t slot_count` | `+0x32`: `uint16_t total_free_space`.
   * `+0x34`: `uint32_t page_state` (`0x01` = Root, `0x04` = Leaf, `0x40000000` = Large RID de 6 bytes).
   * `+0x40`: `uint32_t index_root_page` (`1` = MetaIndex, `2` = Índice 1).
   * `+0x44`: `uint32_t tree_level` (`1` = Nível Folha; `2`, `3`, ... = Nós Internos).
   * **`+0x48`: `uint32_t next_node`** (próxima página na lista encadeada de folhas).
   * **`+0x4C`: `uint32_t prev_node`** (página anterior na lista encadeada de folhas).
4. **Index Root Vector (`"RT10"` em `+0x56`):**
   * Presente no nó raiz de cada índice, com comprimento `rv_len = *(uint16_t *)(page + 0x54)`.
   * Define `min_key_size` (`+0x5C`), `key_length` (`+0x5E`), `info_page_id` (`+0x60`, ex.: `ObjPg 3`), `num_key_parts` (`+0x9C`) e os descritores de 12 bytes de cada coluna da chave em `+0xA0` (`db2_idx_keypart_t`).
5. **Collated Key Directory (em `dir_base = 0x54 + rv_len` na raiz, ou `0x54` nos demais nós):**
   * `dir_base + 0x06`: `uint16_t num_keys`.
   * `dir_base + 0x0C`: `uint16_t key_offsets[num_keys]` — diferente das páginas de dados, **cada offset aponta diretamente a partir do byte `0` da página de 4096 bytes (`page + key_offsets[k]`)**.
6. **Registros de Chave Folha vs. Nó Interno:**
   * **Nó Folha (`tree_level == 1`):** `[2B rec_len] + [Bytes das Colunas da Chave] + [6B Large RID: 2B uint16_t Slot + 4B uint32_t Page] + [1B ridFlags]`.
   * **Nó Interno (`tree_level > 1`):** `[2B rec_len] + [Bytes da Hi Key] + [6B 0xFFFFFFFFFFFF] + [4B uint32_t Child_ObjPg]`. O último registro do nó interno é uma **Dummy Hi Key** de 8 bytes (`08 00 01 00 <4B Child_ObjPg>`).
7. **Info Page (`ObjPg 3`):**
   * Armazena o registro de estatísticas físicas do índice (`FullKeyCard` em `irec + 10` e `LeafPages` em `irec + 18`).

---

## 8. Engenharia Reversa de Transaction Logs (`SQLOGCTL.LFH` e `S000000X.LOG`)

### 8.1. Log File Header (`SQLOGCTL.LFH.1` / `SQLOGCTL.LFH.2` — 12 KB)
O Db2 mantém duas cópias espelhadas de 12 KB (`SQLOGCTL.LFH.1` e `SQLOGCTL.LFH.2`) em `MEMBER0000/`, alternando a gravação a cada checkpoint (controlado pelo campo `uint16_t update_counter` em `+0x00`):

| Offset | Tamanho | Tipo C | Descrição | Valor em `ERP` (`LFH.1`) |
| :--- | :--- | :--- | :--- | :--- |
| `0x0000` | 2 B | `uint16_t` | Contador de atualização (par/ímpar entre `.1` e `.2`) | `126` (`0x007E`) |
| `0x0002` | 2 B | `uint16_t` | Versão do formato do LFH | `14` (`0x000E`) |
| `0x000C` | 4 B | `uint32_t` | `Database Seed` (Epoch de criação do banco) | `0x64E2643E` |
| `0x0010` | 4 B | `uint32_t` | Timestamp da cadeia de logs atual | `0x64E2646D` |
| `0x0014` | 4 B | `uint32_t` | Timestamp do último Checkpoint | `0x66F13D31` |
| `0x0034` | 8 B | `uint64_t` | `Current LSO` (Log Sequence Offset atual) | `79320530` (`0x04BA55D2`) |
| `0x003C` | 8 B | `uint64_t` | `Next Extent StartLSO` | `82400417` (`0x04E954A1`) |
| `0x0044` | 8 B | `uint64_t` | `Next LSN` (Próximo Log Sequence Number) | `0x000000000007D405` |
| `0x0088` | 8 B | `uint64_t` | `MinBuffLSN` (Página suja mais antiga nos Bufferpools) | `0x000000000007D405` |
| `0x0090` | 8 B | `uint64_t` | `LowTranLSN` (Transação ativa mais antiga) | `0x000000000007D405` |
| `0x00E8` | 8 B | `uint64_t` | `Last Allocated TID` (Último Transaction ID) | `0x000000001FF7` (`8183`) |
| `0x01C0` | 52 B | `uint32_t[13]` | Vetor circular dos extents de log ativos | `0, 1, 2, ..., 12` |
| `0x0228` | 8 B | `uint64_t` | `Head LSN` (Início do log ativo para Crash Recovery) | `0x000000000007D405` |
| `0x025C` | 4 B | `uint32_t` | `LOGPRIMARY` (Quantidade de arquivos primários) | `13` (`S0000000..S0000012.LOG`) |

> **Invariante de Consistência (Clean Shutdown):** Quando `NextLSN == MinBuffLSN == LowTranLSN == HeadLSN` (`0x000000000007D405`), não há páginas sujas pendentes de flush nem transações em aberto, indicando banco perfeitamente consistente em disco.

---

### 8.2. Anatomia Física dos Arquivos de Log (`S000000X.LOG` — 4.202.496 bytes)
Cada arquivo `S000000X.LOG` possui **1026 páginas de 4096 bytes**:
1. **Páginas Físicas 0 e 1 (`0x0000..0x1FFF` — 8 KB):** Dois cabeçalhos espelhados de extent com a assinatura `"IBMLOG  "` em `+0x08`, contendo:
   * `+0x14` (`uint32_t`): Número do Extent (`0` a `12`).
   * `+0x18` (`uint32_t`): Quantidade de páginas de dados de log (`1024` páginas = `4096 KB`).
   * `+0x20` (`uint32_t`): Timestamp da cadeia de log (`0x64E26469`).
   * `+0x24` (`uint32_t`): `Database Seed` (`0x64E2643E`).
2. **Páginas de Dados de Log (`0x2000` em diante — 1024 páginas de 4096 bytes):**
   Cada página de log de 4096 bytes possui:
   * **Header de 16 bytes (`0x00..0x0F`):**
     * `+0x00` (`uint16_t bytes_in_page`): Bytes úteis gravados na página (máximo `4076 = 0x0FEC`).
     * `+0x02` (`uint16_t first_rec_offset`): Offset do primeiro registro completo que se inicia nesta página (ou `0xFFFF` se um registro grande atravessa a página inteira).
     * `+0x04` (`uint16_t page_flags`): Flags da página de log.
     * `+0x08` (`uint64_t page_end_lso`): Último **LSO (Log Sequence Offset)** contido nesta página.
   * **Payload de 4076 bytes (`0x10..0xFFB`):** Fatia contígua do fluxo de bytes LSO.
   * **Trailer de 4 bytes (`0xFFC..0xFFF`):** Checksum de paridade da página de log.

> **Algoritmo de Remontagem do Fluxo LSO (`LSO Stream Reassembly`):**
> O `start_lso` de uma página de log é dado por `page_end_lso - bytes_in_page + 1`. Páginas consecutivas pertencem à mesma geração do log circular se, e somente se, `start_lso(p) == page_end_lso(p - 1) + 1`. Ao remover os 16 bytes de cabeçalho e os 4 bytes de rodapé de cada página de 4096 bytes e concatenar os blocos de `4076` bytes, reconstrói-se o **fluxo contínuo de bytes (LSO Stream)** onde os registros de transação estão empacotados sem lacunas (inclusive registros que cruzam fronteiras de páginas de 4 KB).

---

### 8.3. Estrutura do Registro de Transação (32B Header + Payload)
Dentro do fluxo LSO remontado, cada registro possui um cabeçalho fixo de **32 bytes** (`db2_log_rec_hdr_t`):

| Offset | Tamanho | Tipo C | Campo | Descrição |
| :--- | :--- | :--- | :--- | :--- |
| `+0x00` | 4 B | `uint32_t` | `rec_len` | Tamanho total do registro em bytes (Header 32B + Payload) |
| `+0x04` | 2 B | `uint16_t` | `rec_type` | `0x004E` = UNDO/REDO Data/Index, `0x0052` = Internal/Append Page, `0x0084`/`0x0043` = Commit, `0x0082`/`0x0042` = Compensation (CLR) |
| `+0x06` | 2 B | `uint16_t` | `rec_flags` | Flags de encadeamento/compensação |
| `+0x08` | 8 B | `uint64_t` | `lsn` | **LSN (Log Sequence Number)** — o mesmo número gravado em `+0x20` (`Page LSN`) das páginas de dados/índices! |
| `+0x10` | 8 B | `uint64_t` | `tid` | **TID (Transaction ID)** de 6 bytes (`0x00000000157E`, etc.) |
| `+0x18` | 8 B | `uint64_t` | `prev_lso` | Ponteiro reverso (`LSO`) para o registro anterior da mesma transação (`TID`), usado para `ROLLBACK` |

Principais subtipos decodificados no payload (`offset +32` do registro):
* **`DOM INSERT ROW` (`rec_type=0x004E`, `comp=0x05`, `sub=0x01`, `op=0xA2`):**
  * `payload+10` (`uint16_t`): `Tablespace ID` (`4` = `ERPTBS01`)
  * `payload+12` (`uint16_t`): `Object ID` (`6` = `ERP.ITEM`, `7` = `ERP.CLIENTE`)
  * `payload+16` (`uint16_t`): `Row Length` (`76` B para `ITEM`, `66` B para `CLIENTE`)
  * `payload+20` (`uint16_t`): `Target RID Slot`
  * `payload+22` (`uint32_t`): `Target RID Page`
  * `payload+28` (`row_len` B): Imagem binária completa da linha inserida (`FIXEDVAR` header + colunas fixas + colunas variáveis)!
* **`IXM INSERT KEY` (`rec_type=0x004E`, `comp=0x05`, `sub=0x02`, `op=0x65`):**
  * `payload+10` (`uint16_t`): `Tablespace ID`
  * `payload+12` (`uint16_t`): `Index Object ID`
  * `payload+18` (`uint16_t`): `Index ID` (`1`)
  * `payload+20` (`uint32_t`): `Root Page` (`2`)
  * `payload+24` (`uint32_t`): `Target Leaf Page`
  * `payload+40` (`uint16_t`): `Target RID Slot`
  * `payload+42` (`uint32_t`): `Target RID Page`
  * `payload+54` (`uint16_t`): `Key Length`
  * `payload+56`: Bytes exatos da chave indexada em Little-Endian.
* **`DOM APPEND PAGE / UPDATE PAGE 0 SLOT 0` (`rec_type=0x0052`, `comp=0x05`, `sub=0x01`, `op=0x85`):**
  * Emitido sempre que uma nova página de dados é anexada ao final da tabela, atualizando o contador `PageCount` no **Slot 0 da Página 0** da tabela e carimbando o `Page LSN` da Página 0.

---

### 8.4. Prova de Correlação Direta: `Page LSN` na Tabela $\leftrightarrow$ Registro no `S0000003.LOG`
Ao inspecionar a **Página 0** das tabelas `ERP.ITEM` (`ObjID=6`, `PageCount=981` páginas `0..980`) e `ERP.CLIENTE` (`ObjID=7`, `PageCount=86` páginas `0..85`) no container `C0000000.LRG`, observamos os seguintes `Page LSN` no cabeçalho BPS (`+0x20`):
* `ERP.ITEM` (Página 0): **`Page LSN = 0x000000000006302B`**
* `ERP.CLIENTE` (Página 0): **`Page LSN = 0x0000000000065CB9`**

Buscando esses exatos LSNs dentro de `S0000003.LOG` com `./db2_inspector --log`:
1. **`LSN = 0x000000000006302B` (`LogPg=66+0x4A3` em `S0000003.LOG`):**
   * Imediatamente após o `DOM INSERT` (`0x63029`) e `IXM INSERT` (`0x6302A`) do `Item 49952` no último slot da **Página 979 (`Slot 50`)**, o registro `0x6302B` executa `DMS DOM APPEND PAGE (Op=0x85)` em `Tablespace=4 | ObjectID=6 | Param1=980`, anexando a **Página 980** (a 981ª e última página de `ERP.ITEM`) e atualizando o Slot 0 da **Página 0**! No registro seguinte (`0x6302C`), o `Item 49953` é inserido em `Page=980 : Slot=0`.
2. **`LSN = 0x0000000000065CB9` (`LogPg=386+0x530` em `S0000003.LOG`):**
   * Imediatamente após o `DOM INSERT` (`0x65CB7`) e `IXM INSERT` (`0x65CB8`) do `Cliente 4997` no último slot da **Página 84 (`Slot 58`)**, o registro `0x65CB9` executa `DMS DOM APPEND PAGE (Op=0x85)` em `Tablespace=4 | ObjectID=7 | Param1=85`, anexando a **Página 85** (a 86ª e última página de `ERP.CLIENTE`) e atualizando o Slot 0 da **Página 0**! No registro seguinte (`0x65CBA`), o `Cliente 4998` é inserido em `Page=85 : Slot=0`.

---

## 9. Ferramenta Completa Desenvolvida em C (`db2_inspector.c`)

* **Código-fonte:** [`/desenv/db2/db2_inspector.c`](file:///desenv/db2/db2_inspector.c)
* **Build:** [`/desenv/db2/Makefile`](file:///desenv/db2/Makefile) (`make` $\rightarrow$ gera `/desenv/db2/db2_inspector`)

### Comandos de uso e validações realizadas:
```bash
# 1. Decodificar o arquivo de controle SQLSPCS.1 (mostra as 5 tablespaces, incluindo USERSPACE1 deletada)
./db2_inspector --sqlspcs /db2data_erp/db/erp/NODE0000/SQL00001/SQLSPCS.1

# 2. Decodificar o cabeçalho de controle de Transaction Log (SQLOGCTL.LFH.1)
./db2_inspector --lfh /db2data_erp/db/erp/NODE0000/SQL00001/MEMBER0000/SQLOGCTL.LFH.1

# 3. Decodificar registros de transação (DOM INSERT + IXM INSERT) em S0000002.LOG
./db2_inspector --log /db2data_erp/db/erp/NODE0000/SQL00001/LOGSTREAM0000/S0000002.LOG 0 6

# 4. Rastrear no S0000003.LOG os exatos LSNs gravados na Página 0 de ERP.ITEM (0x6302B) e ERP.CLIENTE (0x65CB9)
./db2_inspector --log /db2data_erp/db/erp/NODE0000/SQL00001/LOGSTREAM0000/S0000003.LOG 0x63029 4
./db2_inspector --log /db2data_erp/db/erp/NODE0000/SQL00001/LOGSTREAM0000/S0000003.LOG 0x65CB7 4

# 5. Inspecionar toda a tablespace ERPTBS01 (Tag, SMP, Object Table, Dados e Índices das 4 tabelas)
./db2_inspector /db2data_erp/data/erp/NODE0000/ERP/T0000004/C0000000.LRG

# 6. Inspecionar a tabela ERP.ITEM (ObjID 6: 31 extents de dados, 981 págs, 50.000 linhas + B+Tree de 3 níveis, 265 folhas, 50.000 chaves)
./db2_inspector /db2data_erp/data/erp/NODE0000/ERP/T0000004/C0000000.LRG 6 10

# 7. Inspecionar diretamente o catálogo SYSIBM.SYSTABLES (ObjID 5: 61 págs, 437 linhas) e SYSIBM.SYSCOLUMNS (ObjID 6: 368 págs, 6.247 linhas)
./db2_inspector /db2data_erp/data/catalog.dat 5 10
./db2_inspector /db2data_erp/data/catalog.dat 6 10
```

---

## 10. Leitor Direto de Tabelas por Nome (`db2_select.c` — `<BANCO> <SCHEMA> <TABELA>`)

* **Código-fonte:** [`/desenv/db2/db2_select.c`](file:///desenv/db2/db2_select.c)
* **Binário:** `/desenv/db2/db2_select`

Para permitir consultar os dados de qualquer tabela informando apenas `<BANCO> <SCHEMA> <TABELA>` (sem conectar ao motor Db2), decodificamos mais dois arquivos binários de controle:
1. **System & Local Database Directory (`sqldbdir/sqldbdir` — 1.512 bytes):**
   * Assinatura `"DB2 DIRECTORY"` em `0x0000`, `uint32_t num_entries` em `+0x0014`, offset do primeiro registro em `+0x0034` (`0x00D0`, tamanho `0x0518` por entrada).
   * Em cada entrada (`0x00D0`):
     * `+0x00` (`char[9]`): `DB Alias` (`"ERP     "`)
     * `+0x09` (`char[9]`): `DB Name` (`"ERP     "`)
     * `+0x13` (`char[9]`): Subdiretório físico do banco no Local DB Directory (`"SQL00001"`)
     * `+0x26` (`char[215]`): Caminho base da instância/banco (`"/db2data_erp/db/erp"`)
2. **Automatic Storage Group Control File (`SQLSGF.1` — 135.168 bytes):**
   * Assinatura `"DBSTOGR\0"` em `0x0000`.
   * Descritor do grupo `IBMSTOGROUP` em `0x1008`, contendo o **Storage Path** em `0x1114` (`"/db2data_erp/data"`).

### Cadeia de Resolução Automática em 5 Passos (`db2_select`):
1. Resolve `<BANCO>` (`ERP`) lendo `/home/erp/sqllib/sqldbdir/sqldbdir` $\rightarrow$ `/db2data_erp/db/erp/NODE0000/sqldbdir/sqldbdir` $\rightarrow$ `/db2data_erp/db/erp/NODE0000/SQL00001`.
2. Lê `SQLSGF.1` (`/db2data_erp/data`) e `SQLSPCS.1` (Slot 0 = `SYSCATSPACE` em `/db2data_erp/data/catalog.dat`).
3. Varre `SYSIBM.SYSTABLES` (`ObjID=5` em `catalog.dat`) comparando `Col 0 (NAME) == <TABELA>` e `Col 1 (CREATOR) == <SCHEMA>`, extraindo `Col 8 (FID / ObjectID)` e `Col 9 (TID / TablespaceID)`.
4. Varre `SYSIBM.SYSCOLUMNS` (`ObjID=6` em `catalog.dat`) comparando `Col 1 (TBNAME) == <TABELA>` e `Col 2 (TBCREATOR) == <SCHEMA>`, extraindo `NAME` (`Col 0`), `COLTYPE` (`Col 4`), `LENGTH` (`Col 8`), `SCALE` (`Col 9`) e `COLNO` (`Col 10`).
5. Resolve o container da `TablespaceID` em `SQLSPCS.1` (ex: `/db2data_erp/data/erp/NODE0000/ERP/T0000004/C0000000.LRG`), localiza o `Data EMP` do `ObjectID` na `Object Table (0xFFFF)` e imprime todas as linhas formatadas com os nomes reais das colunas!

### Exemplos de Uso:
```bash
# Listar todas as tabelas de um schema no banco
./db2_select ERP ERP

# Consultar todos os registros de uma tabela
./db2_select ERP ERP EMPRESA
./db2_select ERP ERP FILIAL

# Consultar com limite de linhas
./db2_select ERP ERP CLIENTE 15
./db2_select ERP ERP ITEM 15

# Consultar qualquer tabela do catálogo interno SYSIBM
./db2_select ERP SYSIBM SYSTABLESPACES
./db2_select ERP SYSIBM SYSBUFFERPOOLS
```

---

## 11. Engenharia Reversa do Checksum de Página (`sqlbCalculatePageCheckSum`) e Patch Cirúrgico no Catálogo (`db2_rename_idx.c`)

* **Código-fonte:** [`/desenv/db2/db2_rename_idx.c`](file:///desenv/db2/db2_rename_idx.c)
* **Binário:** `/desenv/db2/db2_rename_idx`

### 11.1 Por que o Db2 bloqueia `RENAME INDEX SYSIBM.SQL...` (`SQL0607N`)?
Na tabela `ERP.EMPRESA`, a Primary Key foi criada sem nome explícito de índice, fazendo o Db2 gerar o índice `SYSIBM.SQL230820161855040` (`USER_DEFINED = 0`, `SYSTEM_REQUIRED = 1`). Ao tentar executar `RENAME INDEX SYSIBM.SQL230820161855040 TO PK_EMP` via SQL, o Db2 recusa a operação com:
```text
SQL0607N  "RENAME" is not defined for system objects.  SQLSTATE=42832
```

### 11.2 Engenharia Reversa do Checksum de Página de 64 bits (`libdb2e.so.1`)
Qualquer alteração binária direta nas páginas de 4096 bytes de `/db2data_erp/data/catalog.dat` é rejeitada pelo Buffer Pool Services (`sqlb_verify_page` $\rightarrow$ `ZRC=0x86020019=SQLB_CSUM "Bad Page, Checksum Error"`) se o campo `uint64_t checksum_meta` no offset `+0x28` do cabeçalho `BPS Page Header` não bater exatamente com o conteúdo da página.

Desmontando as funções `_Z25sqlbCalculatePageCheckSumP9SQLB_PAGEi` (`0x0281a060`), `_Z16sqlb_verify_pageP9SQLB_PAGEP16SQLB_OBJECT_DESCjjP12SQLB_GLOBALSPm` (`0x028121f0`) e suas variantes SIMD (`_Z18ossFletcher64_sse2PKjm`, `_Z19sqlbFletcher64_avx2PKjm`, `_Z21sqlbFletcher64_avx512PKjm`) em `/opt/ibm/db2/V11.5/lib64/libdb2e.so.1`, extraímos o algoritmo exato (**Fletcher-64 modificado**):

1. Copiam-se os 4096 bytes da página para um vetor `uint32_t words[1024]`.
2. Se o bit `0x10` de `page[0x0B]` (`cbits`) estiver ativo, força-se temporariamente `words[0x1C / 4] = 0xFFFFFFFFU` (`diskPageID`).
3. Zera-se o próprio campo de checksum de 64 bits no offset `+0x28`: `words[10] = 0; words[11] = 0;`.
4. Acumulam-se as 1024 palavras `uint32_t` little-endian em dois acumuladores de 64 bits inicializados com `0xFFFFFFFFULL`:
   ```c
   uint64_t s1 = 0xFFFFFFFFULL;
   uint64_t s2 = 0xFFFFFFFFULL;
   for (int i = 0; i < 1024; i++) {
       s1 += words[i];
       s2 += s1;
   }
   ```
5. Reduz-se `s1` (soma em complemento de um) duas vezes para 32 bits e duas vezes para 16 bits:
   ```c
   s1 = (s1 & 0xFFFFFFFFULL) + (s1 >> 32);
   s1 = (s1 & 0xFFFFFFFFULL) + (s1 >> 32);
   s1 = (s1 & 0xFFFFULL) + (s1 >> 16);
   s1 = (s1 & 0xFFFFULL) + (s1 >> 16);
   ```
6. Reduz-se `s2` duas vezes para 48 bits:
   ```c
   s2 = (s2 & 0xFFFFFFFFFFFFULL) + (s2 >> 48);
   s2 = (s2 & 0xFFFFFFFFFFFFULL) + (s2 >> 48);
   ```
7. O checksum final de 64 bits gravado em `+0x28` combina `s2` nos 48 bits superiores e `s1` nos 16 bits inferiores:
   ```c
   uint64_t checksum = ((s2 & 0xFFFFFFFFFFFFULL) << 16) | (s1 & 0xFFFFULL);
   ```

### 11.3 Páginas Modificadas no Patch Cirúrgico (`SYSIBM.SQL230820161855040` $\rightarrow$ `ERP.PK_EMP`)
O utilitário [`db2_rename_idx.c`](file:///desenv/db2/db2_rename_idx.c) atualiza cirurgicamente **12 páginas** de `/db2data_erp/data/catalog.dat` e recalcula o Fletcher-64 de cada página BPS:
1. **`SYSIBM.SYSINDEXES` (`ObjID=7`) — Linha de Dados (`FilePg=20426`, `Slot=2`, `Off=0xAF1`):**
   * Altera `NAME` (`Col 0`) de `"SQL230820161855040"` (18B) para `"PK_EMP"` (6B) e `CREATOR` (`Col 1`) de `"SYSIBM  "` para `"ERP     "`.
   * Recalcula os offsets de todas as colunas `VARCHAR` subsequentes (`TBNAME`, `TBCREATOR`, `COLNAMES`, `PAGE_FETCH_PAIRS`, `DEFINER`, `AVGPARTITION_PAGE_FETCH_PAIRS`), encolhe `rec_len` de `387` para `375` bytes e incrementa `total_free_space` (`+0x32`) em `+12` bytes (`Checksum: 0x2062052A8D45D5DC -> 0x1F4537246E24D2DC`).
2. **Índice B+Tree `INDINDEXES01` (`+CREATOR+NAME`, `ObjID=7`):**
   * Remove a chave antiga `('SYSIBM  ', 'SQL230820161855040') -> RID(42:2)` da página folha `FilePg=1300` (`k[47]`) e insere em ordem lexicográfica a nova chave de 25 bytes `('ERP     ', 'PK_EMP') -> RID(42:2)` na página folha `FilePg=1290` (`k[1]`, entre `PK_CLIENTE` e `PK_FILIAL`).
3. **`SYSIBM.SYSINDEXCOLUSE` (`ObjID=9`) — Linha de Dados (`FilePg=881`, `Slot=15`) + B+Tree `INDINDEXCOLUSE01`:**
   * Altera `INDSCHEMA` para `"ERP     "` e `INDNAME` para `"PK_EMP"` na linha de dados (`FilePg=881`), remove a chave antiga da folha `FilePg=12340` (`k[31]`) e insere a nova chave `('ERP     ', 'PK_EMP', 'EMP_COD', 1) -> RID(17:15)` na folha `FilePg=12323` (`k[2]`).
4. **`SYSIBM.SYSINDEXAUTH` (`ObjID=20`) — Linha de Dados (`FilePg=3136`, `Slot=7`) + B+Trees (`FilePg=3202`, `FilePg=3203`):**
   * Atualiza `NAME` e `CREATOR` na linha de dados e reordena o diretório de slots das páginas folha `FilePg=3202` e `FilePg=3203` (`k[3] -> k[1]`).
5. **`SYSIBM.SYSCONSTDEP` (`ObjID=263`) — Linha de Dados (`FilePg=4896`, `Slot=7`) + B+Tree (`FilePg=4962`):**
   * Atualiza a dependência da Primary Key (`CONSTNAME = SQL230820161855060`) para apontar para `BSCHEMA = 'ERP     '` e `BNAME = 'PK_EMP'` na linha de dados e reordena a chave na folha `FilePg=4962` (`k[3] -> k[1]`).
6. **`SYSIBM.SYSTABLES` `PACKED_DESC` BLOB (`ObjID=5`, `FilePg=1157`):**
   * Atualiza o descritor binário compilado da tabela `ERP.EMPRESA` na página LOB `FilePg=1157`: ajusta o offset final do nome do índice em `+0xA3A` (`0x037A -> 0x036E`), altera o schema do índice em `+0xA6D` (`"SYSIBM  " -> "ERP     "`) e grava `"PK_EMP+EMP_COD"` em `+0xB50`.

### 11.4 Validação no Motor Real do IBM Db2 (`db2start`)
Após iniciar a instância (`db2start`), o próprio motor SQL do Db2 validou todos os checksums Fletcher-64 e passou a exibir e utilizar oficialmente o índice `ERP.PK_EMP`:
```text
INDSCHEMA  INDNAME              TABSCHEMA  TABNAME      COLNAMES             UNIQUERULE
---------- -------------------- ---------- ------------ -------------------- ----------
ERP        PK_CLIENTE           ERP        CLIENTE      +EMP_COD+CLI_COD     P         
ERP        PK_EMP               ERP        EMPRESA      +EMP_COD             P         
ERP        PK_FILIAL            ERP        FILIAL       +EMP_COD+FIL_COD     P         
ERP        PK_ITEM              ERP        ITEM         +EMP_COD+ITE_COD     P         
```

---

## 12. Engenharia Reversa das Operações `INSERT`, `DELETE`, `UPDATE` e `RE-INSERT` (`libdb2e.so.1` + Transaction Log)

Por meio do disassembly de `/opt/ibm/db2/V11.5/lib64/libdb2e.so.1` combinado com o monitoramento ao vivo das páginas físicas (`FilePg=1952` Dados e `FilePg=226` Índice B+Tree) e do arquivo de log ativo (`S0000000.LOG`), mapeamos o fluxo completo de execução das operações DML e a estrutura exata dos registros de Write-Ahead Logging (WAL).

### 12.1 Anatomia do Cabeçalho Comum de Log (`SQLP_LRH` — 40 bytes em FormatVer=14)
Todo registro gravado pela função `_Z11sqlpWriteLRP8sqeAgentmmmmmP14SQLP_LREC_PARTmP9SQLP_LSN8Pm` (`0x09587120`) possui um cabeçalho DPS de **40 bytes (`0x00..0x27`)**, seguido imediatamente em **`+0x28`** pelo cabeçalho do componente (`SQLD_LRH` para tabelas ou `SQLI_LRH` para índices):

| Offset | Tipo C | Campo | Descrição |
| :--- | :--- | :--- | :--- |
| `+0x00` | `uint32_t` | `rec_len` | Tamanho total do registro de log em bytes |
| `+0x04` | `uint16_t` | `rec_type` | `0x004E` (`'N'` Undo/Redo), `0x0052` (`'R'` Redo-Only / Internal), `0x0042` (`'B'` CLR Undo), `0x0084` (Commit), `0x0041` (Abort) |
| `+0x06` | `uint16_t` | `rec_flags` | Flags do registro DPS (`0x0000` normal, `0x2000` fim de transação) |
| `+0x08` | `uint64_t` | `lsn` | `SQLP_LSN8` (Log Sequence Number de 64 bits atribuído ao registro e gravado em `page + 0x20`) |
| `+0x10` | `uint8_t[6]`| `tid` | Transaction ID de 6 bytes (ex: `0D 20 00 00 00 00` = `0x00000000200D`) + 2B padding |
| `+0x18` | `uint64_t` | `prev_lso` | Log Space Offset (`LSO`) do registro anterior da mesma transação (0 no 1º registro da transação) |
| `+0x20` | `uint64_t` | `savepoint` | Identificador interno de Savepoint da instrução SQL (`sqlpSpReq`) |
| **`+0x28`** | `uint8_t` | `comp_id` | **`0x01` = DOM (Data Object Manager)** \| **`0x02` = IXM (Index Manager)** |
| **`+0x29`** | `uint8_t` | `func_id` | Opcode da operação: `0xA2` (DOM Insert), `0xA1` (DOM Delete), `0x72`/`0xB7`/`0xBE` (DOM Update), `0x65` (IXM Insert Key), `0x8D` (IXM Pseudo-Delete Key), `0x8C` (IXM Cleanup Pseudo-Deleted Keys) |

---

### 12.2 Como o Db2 Executa o `INSERT`
1. **Cadeia de Funções em `libdb2e.so.1`:**
   * `sqlriisr(sqlrr_cb*)` (`0x0a13a120`) $\rightarrow$ `sqlrinsr` (`0x0a088c10`) $\rightarrow$ `sqldRowInsert` (`0x02acd690`):
     * Abre um Savepoint de instrução (`sqlpSpReq` em `0x095391c0`).
     * Serializa o vetor `SQLD_VALUE**` no formato binário de disco `FIXEDVAR` (`sqldFastFormatFixedVar` em `0x02ada4d0` / `sqldFullFormatFixedVar` em `0x029b63f0`).
     * Invoca `sqldInsertRow` (`0x02a1cdb0`), que localiza uma página com espaço livre via FSCR (*Free Space Control Record*, `sqldFindPageFromTargetFSCR` em `0x02a13e80`) e chama `sqldAddRow` (`0x02a1f700`).
     * `sqldAddRow` fixa a página no Bufferpool (`sqlbfix` em `0x02601890`), aloca um slot no *Slot Directory* (reutilizando o primeiro slot com `0xFFFF` ou expandindo `slot_count`), copia a linha para o heap da página, chama **`sqldWriteInsertLR` (`0x02a20f40`)** para gravar o log WAL e atualiza `page_lsn` (`+0x20`).
     * Em seguida, `sqldRowInsert` chama `sqldKeyInsert` (`0x02ad6650`) $\rightarrow$ `sqliaddk` (`0x0ae3a7c0`), que desce pela B+Tree (`sqlischa` em `0x0af18820`), insere a chave ordenada na página folha (`sqliiadd` em `0x0ae3c340`) e grava o log de índice via **`sqlilidx` (`0x0ad69350`)**.
2. **Como o `INSERT` é Escrito no Transaction Log (`sqldWriteInsertLR` em `0x02a20f40`):**
   * Grava `comp_id = 0x01` (`movb $0x1, 0x80(%rsp)`) e `func_id = 0xA2` (`movb $0xa2, 0x81(%rsp)` — ou `0xA5` se a página estava vazia).
   * No cabeçalho `SQLD_LRH` (`+0x28..+0x3F`), grava `pool_id` (`+0x2A`), `obj_id` (`+0x2C`), `rec_len` (`+0x30`), `free_space_after` (`+0x32`), o **RID de 6 bytes (`Slot` em `+0x34`, `Page` em `+0x36`)** e o `record_offset` (`+0x3A`).
   * **Otimização de 4 bytes (`lea -0x4(%r14), %rcx` em `0x2a2128e`):** Como os 4 primeiros bytes do registro `FIXEDVAR` (`rec_type` e `rec_len`) já estão codificados no cabeçalho `SQLD_LRH`, `sqldWriteInsertLR` omite esses 4 bytes e grava em `+0x40` apenas os **`rec_len - 4` bytes** restantes da imagem Redo da linha!
   * Logo após, `sqlilidx` grava um segundo registro (`comp_id = 0x02`, `func_id = 0x65`), contendo `RootPg`, `LeafPg`, o `RID` alvo (`flags = 0x02`) e os bytes da chave inserida.

---

### 12.3 Como o Db2 Executa o `DELETE`
1. **Cadeia de Funções em `libdb2e.so.1`:**
   * `sqlridel(sqlrr_cb*)` (`0x0a139350`) $\rightarrow$ `sqldRowDelete` (`0x02ab9ca0`) $\rightarrow$ `sqldDeleteRow` (`0x02a2ca20`):
     * Adquire lock exclusivo `X` na linha (`sqldLockForWrite` em `0x029ceef0`) e abre Savepoint (`sqlpSpReq`).
     * Fixa a página (`sqldFixPageForDelete` em `0x02a2ec30`), posiciona no slot (`sqldPositionOnRow` em `0x02a33020`) e **copia a linha inteira para uma Work Area em memória** (`_intel_fast_memcpy` em `0x02a2d426`) antes de remover o registro da página.
     * Chama `sqldRemoveRow` (`0x02a2fb20`) $\rightarrow$ **`sqldLogAndDeleteRecord` (`0x02a30c20`)** $\rightarrow$ **`sqldWriteDeleteLR` (`0x02a31210`)**.
     * Por fim, usa a cópia em memória da linha excluída para extrair as chaves de índice (`sqldBuildDKEYFromData` em `0x02994d40`) e chama `sqldKeyDelete` (`0x02abb030`) $\rightarrow$ `sqlidelk` (`0x0ae80490`) $\rightarrow$ `sqliCursorDel` (`0x0af39b30`) $\rightarrow$ **`procLeaf2Del` (`0x0af2d6e0`)**.
2. **O Que `sqldLogAndDeleteRecord` (`0x02a30c20`) Faz na Página de Dados (`FilePg=1952`):**
   * **Não zera os bytes do registro no heap da página!** Os bytes antigos permanecem intocados no espaço livre da página até serem sobrescritos.
   * Mantém `slot_count` (`+0x30`) inalterado para jamais deslocar os `RIDs` das demais linhas, e grava **`0xFFFF` (`-1`)** na entrada do *Slot Directory* (`mov $0xffffffff, %esi; mov %si, 0x14(%r12,%r10,2)` em `0x2a30e07`, ou seta o bit 15 `|= 0x8000` em modo de pseudo-deleção de linha).
   * Reserva os bytes liberados nos contadores de espaço excluído da página (`+0x34` = `unreserved_deleted_space` e `+0x36` = `tx_deleted_space`, ambos recebendo `0x003E = 62`) e carimba os **6 bytes do ID de Transação/Savepoint (`+0x3C..+0x41`)** no cabeçalho da página (`0x2a30ef4..0x2a30f06`), impedindo que outra transação concorrente consuma esse espaço antes do `COMMIT` da transação que fez o `DELETE`!
3. **O Que `procLeaf2Del` (`0x0af2d6e0`) Faz na Folha B+Tree (`FilePg=226`) — *Pseudo-Delete*:**
   * Em índices Type-2 do Db2, o `DELETE` **não remove fisicamente a chave da página folha B+Tree**!
   * Ele apenas liga o bit `0x01` no byte de flags do `RID` dentro da chave na folha (alterando `flags` de **`0x02` para `0x03`** em `+0x2F5`), incrementa o contador de chaves pseudo-deletadas da folha (`+0x0B0`: `00 00 -> 01 00`) e liga o bit `0x20` no estado da página folha (`+0x036`: `10 41 -> 20 41`).
4. **Como o `DELETE` é Escrito no Transaction Log (`sqldWriteDeleteLR` em `0x02a31210` e `sqlilkey` em `0x0aec43f0`):**
   * Na tabela: `sqldWriteDeleteLR` grava `comp_id = 0x01`, `func_id = 0xA1` (`movb $0xa1, 0x29(%rax)` em `0x2a31312`), o `RID(Page:Slot)` e, em `+0x40`, grava a **Before-Image completa (`rec_len - 4` bytes) da linha excluída**! Isso garante que, em caso de `ROLLBACK`, `sqldLogAndUndoDelete` (`0x02a58630`) tenha todos os bytes para restaurar a linha no mesmo `RID`.
   * No índice: `sqlilkey` grava `comp_id = 0x02`, `func_id = 0x8D` (`IXM_PSEUDO_DELETE_KEY`), registrando `LeafPg`, `KeySlot`, o `RID` e o valor da chave.

---

### 12.4 Como o Db2 Executa o `INSERT` Subsequente (Reuso de Slot e Limpeza de Pseudo-Deleted Keys)
Quando um novo `INSERT` é executado após o `DELETE` (já comitado):
1. **Na Página de Dados (`sqldAddRow` / `sqldReclaimPseudoDeletedSpaceRuntime` em `0x02a398c0`):**
   * Verifica que o carimbo de transação em `+0x3C..+0x41` (`00 00 00 00 2B CC`) pertence a uma transação já comitada (`sqlpGetCommitLSN`), zera os contadores de espaço bloqueado (`+0x34`, `+0x36` e `+0x3C..+0x41`), varre o *Slot Directory* (`+0x44`), encontra o **`Slot 7 == 0xFFFF`** deixado pelo `DELETE` e **reutiliza exatamente o `RID(Page=0 : Slot=7)`**, sobrescrevendo o endereço `0x0A80` (`Off=0x0A3C`)!
2. **Na Página Folha B+Tree e no Transaction Log (`sqliTryToCleanup` + Encadeamento `PrevLSO`):**
   * Caso a chave inserida seja diferente (ex: `EMP_COD = 5`), a chave antiga `EMP_COD = 4` permanece temporariamente com `flags = 0x03` ou é limpa por `sqliCleanupKeysOnLeaf` (`0x0af2e5e0`), que grava no log um registro especial **`Type = 0x0052` (Redo-Only) com `comp_id = 0x02, func_id = 0x8C` (`IXM_CLEANUP_PSEUDO_DELETED_KEYS`)**.
   * **Detalhe Arquitetural Notável na Cadeia `PrevLSO`:** No log da transação `TID=0x2010`, o `Rec #38` (`DOM INSERT 0xA2`, `LSO=103273464`) é seguido pelo `Rec #39` (`IXM CLEANUP 0x8C`, `Type=0x0052`, `LSO=103273586`, `PrevLSO=103273464`) e pelo `Rec #40` (`IXM INSERT 0x65`, `Type=0x004E`, `LSO=103273670`). Porém, o `PrevLSO` do `Rec #40` aponta para **`103273464` (`Rec #38`)**, **pulando o `Rec #39`**! Isso ocorre porque a limpeza física de uma chave pseudo-deletada de outra transação já comitada é uma operação *Redo-Only* (`0x0052`) que jamais deve ser desfeita caso a transação atual sofra `ROLLBACK`.
   * Caso a chave re-inserida seja idêntica à chave pseudo-deletada (`EMP_COD = 4` no mesmo `RID(0:7)`), o Db2 simplesmente reverte o byte de flag na folha B+Tree de **`0x03` $\rightarrow$ `0x02`** e decrementa o contador de pseudo-deletados de `1` para `0`!

---

### 12.5 Como o Db2 Executa o `UPDATE` (`sqldReplaceRow` em `0x02a713e0`)
Durante a investigação também desmontamos as 3 funções de log de `UPDATE` chamadas por `sqldReplaceRow`:
* **`sqldWriteXOROnlyLR` (`0x02a73120` — `comp_id = 0x01, func_id = 0x72`):** Calcula um `XOR` bit a bit (`old_bytes ^ new_bytes`) da fatia alterada da linha (`xor (%r11,%rdi,8), %rax` em `0x2a73459`), pois o mesmo vetor XOR serve tanto para **Redo** (`old ^ xor = new`) quanto para **Undo** (`new ^ xor = old`)!
* **`sqldWriteXORFullOffsetLR` (`0x02a75420` — `comp_id = 0x01, func_id = 0xB7`):** Variante XOR com offset completo.
* **`sqldWriteFullBeforeDeltaUpdLR` (`0x02a74640` — `comp_id = 0x01, func_id = 0xBE`):** Grava a Before-Image completa de `rec_len - 4` bytes (para Undo) seguida apenas da fatia Delta modificada (`DeltaOff` em `+0x42`, `DeltaLen` em `+0x44`) para Redo (exatamente como capturado no `Rec #33`, `LSN=0x7D453` em `S0000000.LOG`).

---

## 13. Topologia Multi-Container e Algoritmo de Striping (Round-Robin de Extents)

Quando uma *tablespace* DMS ou Automatic Storage possui múltiplos arquivos de container (por exemplo, ao adicionar múltiplos *storage paths* a um *Storage Group* com `CREATE STOGROUP MULTISG ON '/path1', '/path2'`), o Db2 divide a capacidade em **Stripe Sets** e distribui os extents físicos em regime de **Round-Robin** estrito entre os containers do conjunto.

### 13.1 Mapeamento Estrutural em `SQLSPCS.1` / `SQLSPCS.2`
No cabeçalho do slot de 256 KB de cada tablespace em `SQLSPCS.1`:
* **`+0x0448` (`uint32_t num_containers`):** Quantidade total de containers configurados na tablespace.
* **`+0x0450` (`uint32_t total_cont_bytes`):** Tamanho total somado da área de descritores de container em `+0xC000`.
* **Sequência de Containers a partir de `+0xC000`:**
  Os containers são gravados consecutivamente sem padding fixo entre eles. Cada descritor possui:
  * `+0x00`: `uint32_t cont_type` (`6` = File DMS)
  * `+0x04`: `uint32_t total_pages` (ex: `4096`)
  * `+0x08`: `uint32_t useable_pages` (ex: `4064 = total_pages - extent_size`)
  * `+0x30`: `int32_t path_id` (ex: `1024`, `1025`)
  * `+0x34`: String ASCII com o caminho do container (`c_path`) terminada em `\0`.
  * **Offset do Próximo Container:**
    $$\mathtt{NextContainerOffset} = \mathtt{CurrentOffset} + \mathtt{0x34} + \text{strlen}(c\_path) + 1$$

---

### 13.2 Disassembly das Rotinas Internas de Striping (`libdb2e.so.1`)
A tradução de uma página lógica da tablespace (`Pool Page`) para seu container físico e número de página dentro do arquivo (`File Page`) é executada por:
* `sqlbMapPoolPG2Container` (`0x025f6bf0`)
* `sqlbDMSMapPool2Container` (`0x028ab0f0`)
* `sqlb_locate_extent_in_current_map` (`0x02831870`)

No disassembly de `sqlb_locate_extent_in_current_map`:
```asm
   0x28ab257: idiv   %r13                  # pool_page / extent_size (quociente em %eax, resto em %edx)
   ...
   0x2831986: divl   0x2c(%rbp)            # extent_in_range / num_containers (0x2c)
   0x283198c: mov    0x30(%rbp,%rdx,4),%ebp# container_id = range->containers[resto]
   0x28319c5: mov    %eax,(%r14)           # extent_in_container = quociente
   ...
   0x28ab288: imul   %r13,%rbx             # extent_in_container * extent_size
   0x28ab293: add    %rbx,%r8              # + page_in_extent
   0x28ab2a4: add    %rsi,%r8              # + tag_pages
```

### 13.3 Fórmulas Matemáticas Universais de Striping
Dado `pool_page`, `extent_size`, `num_containers` e `tag_pages`:
$$\mathtt{pool\_extent} = \mathtt{pool\_page} \mathbin{/} \mathtt{extent\_size}$$
$$\mathtt{page\_in\_extent} = \mathtt{pool\_page} \pmod{\mathtt{extent\_size}}$$
$$\mathtt{container\_idx} = \mathtt{pool\_extent} \pmod{\mathtt{num\_containers}}$$
$$\mathtt{extent\_in\_container} = \mathtt{pool\_extent} \mathbin{/} \mathtt{num\_containers}$$
$$\mathtt{file\_page} = \mathtt{tag\_pages} + (\mathtt{extent\_in\_container} \times \mathtt{extent\_size}) + \mathtt{page\_in\_extent}$$

**Fórmula Inversa (Dada uma `File Page` em um container `c`):**
$$\mathtt{extent\_in\_container} = (\mathtt{file\_page} - \mathtt{tag\_pages}) \mathbin{/} \mathtt{extent\_size}$$
$$\mathtt{page\_in\_extent} = (\mathtt{file\_page} - \mathtt{tag\_pages}) \pmod{\mathtt{extent\_size}}$$
$$\mathtt{pool\_extent} = (\mathtt{extent\_in\_container} \times \mathtt{num\_containers}) + c$$
$$\mathtt{pool\_page} = (\mathtt{pool\_extent} \times \mathtt{extent\_size}) + \mathtt{page\_in\_extent}$$

### 13.4 Validação Empírica no Banco `ERP` (`TBS_MULTI` / `ERP.TAB_STRIPE`)
Criamos uma tablespace `TBS_MULTI` (`ExtentSize = 32`) com 2 containers:
* Container 0: `/db2data_erp/stogroup1/.../T0000007/C0000000.LRG`
* Container 1: `/db2data_erp/stogroup2/.../T0000007/C0000001.LRG`

Varredura dos Extents Físicos e BPS Headers:
| Pool Extent | Pool Page | Objeto / Função | Tipo Físico | Container Alvo | File Extent | File Page |
| :---: | :---: | :--- | :---: | :---: | :---: | :---: |
| **0** | `0` | `0xFFFE` (SMP Principal) | `0x0E` | **Container 0** | Extent 1 | 32 |
| **1** | `32` | `0xFFFE` (SMP Extent 1) | `0x0E` | **Container 1** | Extent 1 | 32 |
| **2** | `64` | `0xFFFF` (Object Table) | `0x00` | **Container 0** | Extent 2 | 64 |
| **3** | `96` | `0x0004` (Table EMP Dados) | `0x40` | **Container 1** | Extent 2 | 64 |
| **4** | `128` | `0x0004` (Dados: Páginas 0..31) | `0x00` | **Container 0** | Extent 3 | 96 |
| **5** | `160` | `0x0004` (Table EMP Índice) | `0x41` | **Container 1** | Extent 3 | 96 |
| **6** | `192` | `0x0004` (Índice B+Tree) | `0x01` | **Container 0** | Extent 4 | 128 |
| **7** | `224` | `0x0004` (Dados: Páginas 32..63)| `0x00` | **Container 1** | Extent 4 | 128 |

O leitor standalone [`db2_select.c`](file:///desenv/db2/db2_select.c) foi atualizado com essa máquina de mapeamento e leu com perfeição todos os **250 registros** da tabela `ERP.TAB_STRIPE`, intercalando as leituras entre ambos os containers transparentemente!

---

## 14. Anatomia e Parametrização Universal de Páginas de 8KB, 16KB e 32KB

Validamos o comportamento interno do Db2 ao utilizar *Bufferpools* e *Tablespaces* configuradas com tamanhos de página não-padrão de **8.192 bytes (8KB)**, **16.384 bytes (16KB)** e **32.768 bytes (32KB)**.

### 14.1 BPS Header Universal (48 bytes em todas as páginas)
Independentemente do *PageSize*, o cabeçalho BPS mantém tamanho fixo de 48 bytes (`0x30`):
* `+0x00` (`uint16_t page_data_offset`): Permanece fixo em **`48 (0x0030)`**.
* `+0x02` (`uint16_t page_data_length`): Informa exatamente **`PageSize - 48`**:
  * 4KB: `4048 (0x0FD0)`
  * 8KB: `8144 (0x1FD0)`
  * 16KB: `16336 (0x3FD0)`
  * 32KB: `32720 (0x7FD0)`

> **Detecção Automática:** Ao inspecionar qualquer arquivo binário de container, o tamanho real da página pode ser obtido instantaneamente lendo os primeiros 4 bytes da Pool Page 0:
> $$\mathtt{PageSize} = \mathtt{page\_data\_length} + 48$$

---

### 14.2 Data Page Header (20 bytes) e Slot Directory
* O cabeçalho da página de dados mantém os mesmos 20 bytes (`0x30..0x43`).
* O **Slot Directory** inicia sempre no byte fixo **`0x44` (68 bytes)** a partir do início da página.
* Cada slot armazena um `uint16_t` com o offset relativo a `0x44`:
  $$\mathtt{OffsetNaPagina} = \mathtt{0x44} + \mathtt{slot\_offset}$$

---

### 14.3 Disposição dos Metadados na Página 0
* **Slot 0 (Control Header de 32 bytes):** Fica sempre alinhado encostado no final da página:
  $$\mathtt{OffsetSlot0} = \mathtt{PageSize} - 32$$
  * Em 4KB: `0x0FE0` (4064)
  * Em 8KB: `0x1FE0` (8160)
  * Em 16KB: `0x3FE0` (16352)
  * Em 32KB: `0x7FE0` (32736)
* **Slot 1 (FSCR — Free Space Control Record):** Permanece fixo com **1.004 bytes** (500 entradas de `uint16_t` controlando o espaço das 500 páginas lógicas seguintes).
* **Slot 2 (TDIR):** Permanece com **76 bytes**.
* **Slot 3 (Table Description):** Contém os mesmos descritores de coluna de 8 bytes cada.
* **Slot 4+ (Registros FIXEDVAR):** Alocados dinamicamente no espaço livre ampliado da página (permitindo linhas de até ~32 KB sem necessidade imediata de ponteiro de *overflow*).

---

### 14.4 Testes e Validação Completa
Criamos e validamos tabelas reais em cada tamanho de página:
* `ERP.TAB8K` em `TBS8K` (8KB) $\rightarrow$ 2 registros lidos diretamente via [`db2_select`](file:///desenv/db2/db2_select).
* `ERP.TAB16K` em `TBS16K` (16KB) $\rightarrow$ 1 registro com string longa de 8KB lido diretamente.
* `ERP.TAB32K` em `TBS32K` (32KB) $\rightarrow$ 1 registro gigante com string de 20KB e timestamp lido diretamente.
* `ERP.TAB_STRIPE` em `TBS_MULTI` (Multi-Container Striping) $\rightarrow$ 250 registros distribuídos em 2 containers lidos com 100% de sucesso.

---

## 15. Engenharia Reversa da Compressão de Linhas (Static & Adaptive Row Compression)

A compressão de tabelas no IBM Db2 (`COMPRESS YES STATIC` ou `COMPRESS YES ADAPTIVE`) baseia-se na substituição de padrões repetidos por tokens curtos de bits, apoiados por um dicionário de substituição construído offline (via `REORG TABLE ... RESETDICTIONARY`) ou online (*Automatic Dictionary Creation*).

### 15.1 Detecção Físico-Binária da Linha Comprimida (`rflags & 0x0080`)
No cabeçalho de 8 bytes de qualquer registro `FIXEDVAR`:
* Linha padrão não-comprimida: `rflags = 0x0001` (bit 0 ligado).
* **Linha comprimida com Dicionário:** **`rflags = 0x0081` (o bit `0x0080` indica compressão ativa)**.
* Ao encontrar o bit `0x0080`, o payload do registro **não segue** a disposição posicional de colunas descrita na *Table Description* (Slot 3), mas sim uma sequência contínua de tokens codificados pelo dicionário.
* **Ganho real comprovado:** Na tabela `ERP.TAB_COMP_STATIC` com 2.500 linhas, o tamanho médio foi reduzido de **~160 bytes para 29 bytes**, permitindo acomodar **136 registros comprimidos por página de 4KB** (contra apenas ~25 registros no modo tradicional).

---

### 15.2 Topologia de Armazenamento do Dicionário no Objeto da Tabela
O Db2 armazena o dicionário de compressão global diretamente dentro do segmento de dados (`Type = 0x00`) da própria tabela, antes das linhas de dados:
1. **Página 0, Slot 4 — `HEADREC` (`rec_flags = 0x0040`):**
   * Contém os metadados do dicionário: identificadores da árvore de prefixos (`SP Dictionary Tree Node`), tamanho total e raízes de expansão.
2. **Páginas 1 a 14, Slot 0 — `NEXTREC` (`rec_flags = 0x0040`):**
   * Cada página possui **apenas 1 registro (`SlotCnt = 1`)** com `rec_len = 4005` bytes contendo os blocos de continuação do dicionário.
3. **Página 15 — Término do Dicionário e Início dos Dados:**
   * `Página 15, Slot 0`: Bloco final do dicionário (`rec_flags = 0x0040`, `len = 1680` bytes).
   * `Página 15, Slot 1..75`: **Início das linhas comprimidas de usuários (`rec_flags = 0x0081`)**.
   * `Páginas 16..33`: Páginas 100% preenchidas com linhas comprimidas (136 slots por página).

---

### 15.3 Disassembly do Algoritmo de Descompressão (`PCAndSPExpandRec` em `libdb2e.so.1`)
O processo de descompressão é executado pela cadeia de funções:
* `sqldExpandFullRec` (`0x02b6b330`)
* `sqldExpandRec` (`0x02b6a9b0`)
* `PCAndSPExpandRec` (`0x02d53ed0`)
* `PCExpandRec` (`0x02d53b40`)

#### Estrutura da Tabela do Dicionário de Expansão (`SPBINEXPDICT` — 32 KB)
O dicionário é montado em memória num bloco de **32.768 bytes (`0x8000`)**, dividido em **2.048 entradas de 16 bytes**:
* Cada entrada possui 16 bytes: `uint64_t w0` e `uint64_t w1`.
* O **tamanho do símbolo descomprimido (`sym_len`)** está codificado no **byte mais significativo de `w0`**:
  $$\mathtt{sym\_len} = w_0 \gg 56$$
* Os bytes descomprimidos daquele símbolo ocupam `entry[0 .. sym_len-1]`.

#### Decodificação dos Tokens de 12 bits no Stream Comprimido
No disassembly de `PCAndSPExpandRec`:
```asm
   0x2d53f0c: movzbl (%rbx,%r8,1),%r14d       # Lê byte de controle
   0x2d53f11: inc    %rbx
   0x2d53f14: cmp    $0xf0,%r14
   ...
   0x2d54231: rol    $0x7,%r13w               # Token 1: rotaciona 7 bits para a esquerda
   0x2d54236: rol    $0xb,%dx                 # Token 2: rotaciona 11 bits para a esquerda
   0x2d54241: and    $0x7ff8,%r14d            # Máscara alinhada a 8 bytes dentro dos 32 KB
   0x2d54248: and    $0x7ff8,%edx
   0x2d5424e: mov    (%r14,%rsi,1),%r15       # R15 = w0 da entrada no dicionário
   0x2d54252: mov    0x8(%r14,%rsi,1),%r14    # R14 = w1 da entrada no dicionário
   0x2d54260: mov    %r15,(%rdi)              # Grava no buffer de destino
   0x2d54273: shr    $0x38,%r15               # Extrai sym_len (w0 >> 56)
   0x2d54277: add    %r15,%rdi                # Avança ponteiro de saída pelo sym_len
   0x2d5428c: add    $0x3,%rbx                # Consumiu 3 bytes do stream (2 tokens de 12b = 24b)
```

1. **Tokens Empacotados em 24 bits (3 bytes $\rightarrow$ 2 tokens de 12 bits):**
   * $\mathtt{offset1} = ((b_0 \mid (b_1 \ll 8)) \text{ rol } 7) \ \& \ \mathtt{0x7FF8}$
   * $\mathtt{offset2} = ((b_1 \mid (b_2 \ll 8)) \text{ rol } 11) \ \& \ \mathtt{0x7FF8}$
2. **Tokens Literais e Padrões Locais (`r14 < 0xF0`):**
   * Copia repetições literais de bytes que não foram indexadas no dicionário global.
3. **Padrões de Escape (`r14 == 0xFF`):**
   * Indica sequência estendida com offsets de 2 bytes adicionais.

---

### 15.4 Integração no Leitor Standalone (`db2_select`)
Identificamos a necessidade de proteger o leitor contra decodificação errônea de linhas comprimidas: ao encontrar `rflags & 0x0080`, o leitor reconhece que os bytes pertencem ao stream LZ/SP comprimido, evitando a leitura de lixo binário e preparando a base para a expansão direta via dicionário `HEADREC`/`NEXTREC`.
