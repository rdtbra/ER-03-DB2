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
8. [Ferramenta Desenvolvida em C (`db2_inspector.c`)](#8-ferramenta-desenvolvida-em-c-db2_inspectorc)
9. [Próxima Etapa: Transaction Logs (`LOGSTREAM0000` e `SQLOGCTL.LFH`)](#9-próxima-etapa-transaction-logs-logstream0000-e-sqlogctllfh)

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
