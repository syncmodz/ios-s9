# `usersession.kb` no keybagd do 14.3 — anatomia completa

**2026-11-16.** Dissecação estática de `/usr/libexec/keybagd` do rootfs 18C66
(`~/s9dev/rootfs-14.3/apfs-raw`). Nada rodou no aparelho; tudo abaixo foi lido do
binário (`work/kb/keybagd`, disassembly `work/kb/kb.txt`) e do log `log33.txt`.

---

## 1. O que o log mostra

```
EXEC keybagd pid=11                        (boot task)
  ****** IN MKB_INIT ****
  MKB_INIT: System Keybag loaded
  ****** STARTING EARLYBOOT SETUP ****
  MKB_INIT: Could not open /private/var//keybags/usersession.kb: No such file or directory
  MKB_INIT: LoadAllUserSessions: diskLoadUserSessions failed
  PALLOC ... 1000 / 99 / 199
  MKB_INIT: setup kernel personas
  ****** DONE MKB_INIT ****
EXIT keybagd pid=11 status=0

EXEC keybagd pid=38                        (o daemon, RunAtLoad)
  SWEET pthread_register #80 comm=keybagd
  SWEET AKS: keybagd sel 0 ... -> 0x0
  (nada mais, até o fim do boot)
```

Duas coisas que não estavam documentadas:

1. **O silêncio do daemon (pid 38) não prova nada — é o `/dev/null`.** O `main` do
   keybagd imprime `****** STARTING KEYBAGD  ****` (offset `0x57f64`) com `fwrite`
   para `stdout`, e o job do launchd não tem `StandardOutPath`. No nosso kernel,
   `bsd/dev/mem.c:158` diz explicitamente: *"daemons started by launchd write
   stdout/stderr to /dev/null"*, e só os nomes do boot-arg `sweetnull` (default
   `backboardd,SpringBoard`) são ecoados ao console como `|name| text`. O `log33.txt`
   não tem `sweetnull` nos boot-args, e as únicas 15 linhas com prefixo `|...|` são
   todas de `backboardd`. **Portanto o keybagd daemon pode estar rodando normalmente
   e ser invisível.** Não se pode concluir que ele parou antes do print.
   *(Correção de uma conclusão errada que eu tinha escrito antes de checar isto.)*
2. **O `-t 15` do launchd plist é ignorado.** O `main` só compara `argv[1]` com
   `--init` (`0x10002ed9c`–`0x10002edb4`); não há `getopt`, e o único `strtol` do
   binário está em outro caminho (`seshat-count`, `0x10002f7a0`). O plist
   `com.apple.mobile.keybagd.plist` passa `-t 15`, e o binário descarta.

---

## 2. O caminho de leitura, instrução por instrução

`diskLoadUserSessions` = `0x1000201b8`. Sequência:

```
0x1000201dc  open("/private/var//keybags/usersession.kb", O_RDONLY)
0x1000201e4  bl  load_binary_dict        (0x10001b388)
0x1000201e8  cbz x0 -> 0x100020994       <-- x0 == NULL => FALHA
```

`load_binary_dict` (`0x10001b388`):

```
0x10001b3a4  _open(path, 0)
0x10001b3ac  b.eq 0x10001b428            <-- open falhou (fd == -1)
0x10001b428  __error / _strerror
0x10001b438  "load_binary_dict" log: "Could not open %s: %s"
0x10001b44c  mov x21,#0 ; mov x20,#0     <-- devolve NULL
0x10001b410  mov x0, x20 ; ret            -> NULL
```

Então `diskLoadUserSessions` cai em `0x100020994`:

```
0x100020994  "LoadAllUserSessions"
0x1000209a8  mov w0, #0x0
0x1000209c8  ret                         -> 0  (falha)
```

E `setupUserSessionStart` (`0x100020110`) trata o retorno:

```
0x100020124  bl 0x1000201b8              (diskLoadUserSessions)
0x100020128  cbz w0 -> 0x100020190       (0 = falha)
0x100020190  bl 0x100020bc0              (fallback: persona.kb)
0x100020194  mov w19, #0x1
```

**Não existe fallback que sintetize a tabela de sessões.** O ramo `0x100020bc0` carrega
`/private/var//keybags/persona.kb` e, se também falhar, só loga `StartupPersonaLoad` e
segue. A sessão `mobile:501` simplesmente não existe — e é exatamente a persona
`100`/`200` que falta no log (ver `s9/CONTAINER-2026-11-16.md` §3.3).

O `persona.kb` que o fallback lê é um **array** (`CFArrayGetValueAtIndex` em
`0x100020c7c`), enquanto o `usersession.kb` é um **dicionário**
(`CFDictionaryContainsKey` em `0x10002076c`). Formatos diferentes; um não substitui o
outro.

---

## 3. O formato do arquivo, completo

O `diskSaveUserSessions` (`0x10001fcb4`) escreve as chaves; o load lê as mesmas. Todas
resolvidas do binário (não inferidas):

### Obrigatórias (lidas logo no início)

| chave | tipo | o que é |
| --- | --- | --- |
| `NUMENT` | CFNumber | quantas entradas de sessão |
| `BLOB` | CFData | a tabela serializada (o `%@` do log `Reloaded _gUserSessionTable`) |
| `APNSID` | CFNumber | APNS id; o log imprime `set to %llu` |

### Leitura opcional (cada uma com um `ContainsKey` antes)

`MAXUSER`, `MAXUSERSIZE`, `USESSTYPE`, `LOGINCHECKIN`, `DEVICENETWORKBG`,
`VOLUMETYPE`, `SYNCENT`/`SYNCBLOB` (bolhas de sync), `LRUENT`/`LRUBLOB` (lista LRU).

No `log33.txt` o `VOLUMETYPE` está ausente — e há um caminho para isso
(`0x100020818`):

```
FOUND NO VOLUMETYPE, setting up volume type
```

seguido de `_setVolumeType` e da gravação `VOL:Native` (`0x1000208f0`). Ou seja: o
próprio binário **sabe criar o default** quando a chave falta. Isso é importante —
significa que um arquivo mínimo válido não precisa de `VOLUMETYPE`.

### O BLOB

O `BLOB` é lido (`0x10002026c` `CFDataGetBytePtr` / `0x100020278` `CFDataGetLength`) e
passado a `0x10001b018`, que é o irmão de `convertBinaryToObject` — desserializa um
plist binário (`CFPropertyListCreateWithStream`, formato `kCFPropertyListBinaryFormat_v1_0`,
constante `0xc8` em `0x10001b280`). O resultado é um `CFArray` de dicionários; cada
entrada é validada como dicionário (`0x1000202c8`) e só é aceita se
`NUMENT >= 0x1f6 (502)` (`0x1000202dc`). Depois `0x100023634` a instala na tabela.

**Então o arquivo é:** um plist binário, dicionário de topo com `NUMENT`, `BLOB`,
`APNSID`; o `BLOB` é outro plist binário contendo um array de `NUMENT` dicionários.

---

## 4. Por que o arquivo não existe — e o que fazer

O `open` falha com ENOENT, mas o diretório existe: o log mostra
`SWEET fixup mkdir /private/var/keybags err=17` (17 = EEXIST). O `sweet_fixup` roda na
linha 211, **depois** do `EXEC keybagd pid=11` (linha 199) — mas como o diretório já
existia, isso não é o problema.

O produtor natural de `usersession.kb` é `diskSaveUserSessions`, que é chamado de 11
lugares. O primeiro que roda numa vida de dispositivo é `setupUserSessionStart` →
`diskLoadUserSessions` (falha) → `0x10001fcb4` em `0x10002098c`, ou seja **o save só
acontece depois de um load bem-sucedido**. É um ciclo: sem arquivo, nunca grava; sem
gravar, nunca há arquivo. Um aparelho recém-restaurado sai desse ciclo porque o
`/private/var` vem de um snapshot do Data volume com o arquivo já dentro.

Além disso, `0x100020990 cbnz w26 -> 0x100020934` só faz o save se `w26 != 0`, e `w26`
é `0` no caminho de falha do load (`0x10002032c`).

### Opções, da menos à mais invasiva

1. **Colocar um `usersession.kb` válido na imagem do root.** Precisa ser gerado no
   formato do §3, e o `s9hfs.py` não cria arquivo novo (só reescreve bytes dentro de um
   existente). Dá para reservar um arquivo dummy de tamanho suficiente no HFS e
   sobrescrevê-lo, mas ainda falta *gerar* o plist — que depende de saber o schema dos
   dicionários do `BLOB`, que eu **não** resolvi nesta sessão (a validação em
   `0x100023634` não foi lida).
2. **Ver o que o daemon (pid 38) faz de verdade.** Ele é invisível no log por causa do
   `/dev/null` (§1.1). Duas formas baratas: (a) bootar com
   `sweetnull=backboardd,SpringBoard,keybagd` — o mecanismo já existe em
   `bsd/dev/mem.c:158`, é só um boot-arg; (b) um backtrace do pid 38 pelo sampler.
   Isto responde se o daemon chega a `setupUserSessionStart` e, se não, onde para.
   **É o teste mais barato e o mais informativo desta lista.**
3. **Instrumentar `diskLoadUserSessions` para logar o `errno` do `open` e o que mais
   falhou.** Trivial (já temos a string `Could not open %s: %s` no binário; o log
   atual não a mostra porque ela vai para o `os_log` do keybagd, que não está no nosso
   console) — mas o `strerror` já está no log: "No such file or directory". Não agrega.

---

## 5. O ponto que liga isto ao resto

O `main` do keybagd, antes do `STARTING KEYBAGD` (`0x10002ede4`):

```
0x10002ede4  bl 0x10001b6a0
0x10001b6a0    -> 0x10001b5fc
0x10001b5fc      _mach_host_self
0x10001b61c      _host_get_multiuser_config_flags
0x10001b6b8      cmp w0, #0x0 ; cset eq      (sucesso?)
0x10001b6c4      cmp w9, #0x0 ; cset lt      (bits 31 = kIsMultiUserDevice?)
```

O mesmo `host_get_multiuser_config_flags` aparece **9 vezes no `containermanagerd`**
(inclusive em `0x100013478`, a função que decide `isMultiUser`, e em
`0x100089e6c`, no caminho de identidade do cliente). Em todas: `tbnz w8, #0x1f`
— testa o bit 31 do valor do commpage.

No nosso kernel o commpage só é escrito por `host_set_multiuser_config_flags`
(`osfmk/kern/host.c:1387` → `commpage_update_multiuser_config`). **Verifiquei que
nada o inicializa:** `pmap_create_commpages` (`osfmk/arm/pmap/pmap.c:10840`) aloca as
páginas com `memset(..., 0, PAGE_SIZE)`, e `commpage_populate` nunca escreve
`_COMM_PAGE_MULTIUSER_CONFIG`. Então o valor lido é **0** a menos que alguém chame
`host_set_multiuser_config_flags`.

O único binário do 14.3 que importa `_host_set_multiuser_config_flags` é o **keybagd**
(`nm -u`), no `0x10001b640` — e o caminho que chega lá só roda no ramo multiusuário.
O keybagd do boot task (pid 11) não loga tê-lo chamado, e o do daemon (pid 38) é
invisível (§1.1).

**Hipótese, não fato:** o commpage fica em 0, todo mundo lê "não é multiusuário", e
isso explicaria de uma vez (a) o `SYSTEM_PROXY` em todos os processos, (b) as personas
`100`/`200` ausentes, (c) a decisão `isMultiUser` do containermanagerd. **Como
testar barato:** um `sysctl` que leia `_COMM_PAGE_MULTIUSER_CONFIG` (é uma página
mapeada no kernel; uma linha) ou, sem código, um `sweetnull=...,keybagd` para ver se
o daemon tenta `host_set_multiuser_config_flags`.

---

## 6. O que eu não resolvi

- O schema dos dicionários dentro do `BLOB` (função `0x100023634`). Sem ele não dá para
  gerar um `usersession.kb` sintético correto.
- O valor real do commpage em runtime (a hipótese do §5 é estática, não medida).
- O que o keybagd daemon (pid 38) faz — ele é invisível no log por causa do `/dev/null`
  (§1.1). `sweetnull=...,keybagd` responde.
