# O commpage multiuser, a fundo

**2026-11-16.** O que a palavra `_COMM_PAGE_MULTIUSER_CONFIG` faz neste port, dos dois
lados. Tudo verificado em fonte ou binário; onde é inferência, diz.

---

## 1. O caminho no kernel: existe, e ninguém o escreve

`osfmk/arm/cpu_capabilities.h`:

```
162  #define _COMM_PAGE64_BASE_ADDRESS   (0x0000000FFFFFC000ULL)   /* In TTBR0 */
170  #define _COMM_PAGE_RW_OFFSET        (0)
358  #define _COMM_PAGE_MULTIUSER_CONFIG (_COMM_PAGE_START_ADDRESS+0x108)  // uint32_t
115  #define kIsMultiUserDevice          0x80000000   // this device is in multiuser mode
119  #define kMultiUserCurrentUserMask   0x3fffffff   // the current user UID
```

Então a palavra mora em `0xFFFFFC000 + 0x108 = 0xFFFFFC108`, e é **uma palavra só**:
bit 31 = "é multiusuário", bits 0-29 = o uid atual.

Escritores no kernel — o `grep` sobre a árvore inteira dá **um**:

```
osfmk/kern/host.c:1399        commpage_update_multiuser_config(multiuser_config);
osfmk/arm/commpage/commpage.c:762   commpage_update_multiuser_config(uint32_t) { ... }
```

e `host_set_multiuser_config_flags` (`osfmk/kern/host.c:1387`) é o único chamador dela.
A cadeia é `host_set_multiuser_config_flags` → `commpage_update_multiuser_config` →
`*(uint32_t *)(_COMM_PAGE_MULTIUSER_CONFIG + _COMM_PAGE_RW_OFFSET)`.

**O lado da inicialização importa mais.** `pmap_create_commpages`
(`osfmk/arm/pmap/pmap.c:10840`) aloca as páginas com `memset(..., 0, PAGE_SIZE)`, e
`commpage_populate` (`osfmk/arm/commpage/commpage.c:124`) preenche assinatura, versão,
cache line, cpus, memória, cpufamily, timestamps — e **não** escreve
`MULTIUSER_CONFIG`. Confirmei lendo o corpo inteiro.

Conclusão firme: **a palavra é 0 no boot e continua 0 até alguém chamar
`host_set_multiuser_config_flags`.** Não é um valor "não suportado" — é um zero legítimo,
que significa "não é multiusuário, usuário atual desconhecido".

---

## 2. Quem no userspace pode escrevê-la

`host_set_multiuser_config_flags` exige `host_priv_t`, ou seja a porta privilegiada do
host — só root, e só depois de `host_get_special_port(4)`.

No rootfs 18C66, quem importa o símbolo:

| binário | importa set? |
| --- | --- |
| `usr/libexec/keybagd` | **sim** (`_host_set_multiuser_config_flags` em `nm -u`) |
| `sbin/launchd` | não (só `host_get_...`) |
| `usr/libexec/containermanagerd` | não (só `host_get_...`) |

Um único binário. E o caminho que chega lá, no keybagd (`0x10001b640`), é o **ramo
multiusuário**: o `main` só o chama depois de decidir que o dispositivo já é multiusuário.
Num aparelho single-user ele nunca roda — o que é consistente com o valor 0 do §1.

**Ou seja: neste port, e provavelmente num iPhone real também, a palavra é 0. Zero não é
o bug.**

---

## 3. Os dois leitores, e o que eles fazem com 0

### 3.1 `containermanagerd` — `isMultiUser` (`0x100013420`)

Li a função inteira. Ela é uma cascata, e a ordem importa:

```
0x100013438  isMultiUser = 1                              (provisório)
0x10001344c  isMultiUser = (UMUserManager class != nil)   (se a classe existe = 1)
0x100013454  se nil -> sai com 0
0x100013468  isMultiUser = os_feature_enabled("containermanagerd","data_separation")
0x100013470  se o flag == 0 -> sai com 0
0x100013480  host_get_multiuser_config_flags(&flags)
0x100013498  isMultiUser = (kr != 0) | (!(flags >> 31))   <-- veja abaixo
0x10001349c  se kr != 0 -> vai para o gestalt
0x1000134a0  tbnz w8,#0x1f -> se bit 31 SET -> 0x1000134cc (isMultiUser = 0)
0x1000134a4  MGGetBoolAnswer("3kmXfug8VcxLI5yEmsqQKw")
0x1000134b4  isMultiUser = !answer
0x1000134b8  se answer == 0 -> os_variant_uses_ephemeral_storage(), isMultiUser = !that
```

Três coisas que só aparecem lendo o código:

1. **O bit 31 "setado" significa *desligar*.** `tbnz w8, #0x1f` salta para o ramo que
   põe `isMultiUser = 0`. O nome `kIsMultiUserDevice` sugere o contrário; aqui o teste
   está invertido em relação ao nome. Seja qual for a leitura correta, **com a palavra a
   0 o bit 31 é 0 e o fluxo cai no gestalt** — que é o caminho mais longo.
2. **`0x100013498` é um cálculo estranho e vale registrar como suspeito:**
   `strb w9` grava `(kr != 0) | ((~flags) >> 31)`. Como `~flags >> 31` é 1 quando o bit
   31 é **0**, a flag fica **1** justamente no caso não-multiusuário. Escrevi isso três
   vezes e dá o mesmo; não vou afirmar o que o autor queria, mas registro que a
   expressão não diz "é multiusuário".
3. **O primeiro gate nem é o commpage: é o feature flag.** `__os_feature_enabled_impl`
   lê `/System/Library/FeatureFlags/Domain/containermanagerd.plist`, que **existe** no
   rootfs 18C66 e diz:

   ```json
   { "data_separation" => { "Enabled" => true } }
   ```

   Então esse gate passa. Verificado no arquivo, não inferido.

### 3.2 O segundo critério: `MGGetBoolAnswer("3kmXfug8VcxLI5yEmsqQKw")`

A chave é uma das ~200 do bloco de strings do `libMobileGestalt.dylib`
(`0x18f37656b`, índice 33 do bloco começando em `0x18f37626b` — calculado a partir do
passo de 23 bytes por chave). O mesmo literal aparece em `MobileActivation`,
`AppAttestInternal`, `DeviceIdentity` e `StorageSettings`, mas **só** o
`containermanagerd` a usa para decidir multiusuário.

O valor dela **não está em nenhuma tabela do binário** — vem do cache do gestalt:

```
/private/var/containers/Shared/SystemGroup/systemgroup.com.apple.mobilegestaltcache/Library/Caches/com.apple.MobileGestalt.plist
```

e essa string está em `libMobileGestalt.dylib` (`0x18f3728xx`), junto de
`CacheVersion` / `CacheData` / `CacheExtra` / `CacheUUID` e
`Cache file did not exist. Rebuilding.`

**E aqui está o elo que vale:** esse diretório **não existe no rootfs 18C66**. Verifiquei:
`private/var/containers` não está na imagem. Ele é criado no boot pelo `sweet_fixup`
(`bsd_vfs_vfs_syscalls_c.diff`) e o `log33.txt` mostra `mkdir … err=17` em toda a árvore
`SystemGroup/…/mobilegestaltcache/Library/Caches` — criado, **mas vazio**. Não há
`com.apple.MobileGestalt.plist`.

Isso não é exclusivo do 14.3: o `log33.txt` linha 169 mostra o próprio sistema reclamando,

```
copy_path_for_booted_os_data: Failed to query gestalt for the BootManifestHash
main: Unable to get path for booted OS data
```

do `MSUEarlyBootTask`. Ele chama `MGCopyAnswer("BootManifestHash")`, não obtém nada e
segue. **O gestalt está degradado neste port, e há prova em log.**

Vale notar que o `containermanagerd` só chega ao `MGGetBoolAnswer` porque o commpage
está a 0 (§3.1, ramo `0x1000134a4`). Num dispositivo cujo gestalt funciona, a resposta
para "é multiusuário?" seria `false` e o resultado final também seria não-multiusuário —
**então o caminho pelo gestalt provavelmente também termina em single-user.** Não tenho
como afirmar sem medir, mas a hipótese "o commpage a 0 é a causa" ficou bem mais fraca:
o valor 0 é o normal, e o que o código faz com ele é cair num segundo critério que
também aponta para single-user.

---

## 4. O que mudou na minha leitura, honestamente

Na sessão anterior escrevi que o commpage a 0 "explicaria de uma vez" o `SYSTEM_PROXY`,
as personas 100/200 e a decisão do MCM. Investigando a fundo, **três dessas três coisas
não se sustentam**:

- **A palavra a 0 é o valor normal**, não um defeito do port (§1). O kernel nunca a
  inicializa em nenhuma plataforma; quem a escreve é o keybagd, no ramo multiusuário,
  que num aparelho single-user não roda (§2).
- **A decisão de `isMultiUser` cai no gestalt quando a palavra é 0** (§3.1), e o
  gestalt, mesmo funcionando, responderia não-multiusuário.
- **E o mais importante:** no iOS 16, que chega ao SpringBoard, o commpage também é 0 —
  a mesma árvore de kernel o produz — e lá o `SYSTEM_PROXY` e a ausência de 100/200
  **também acontecem** (o log de `evidence-20261029` mostra `UMD_INIT: FAILED to Set
  Foreground ID + multiuser in commpage successfully`). Ou seja, esse sintoma **não
  distingue** o boot que funciona do que não funciona. É um beco.

Isso é o resultado útil da investigação: **o commpage não é a parede.** A diferença entre
o boot de 16 que desenha e o de 14.3 que não desenha está em outro lugar.

---

## 5. O que ainda vale, e o achado que saiu desta investigação

O commpage sai da lista de suspeitos (§4). Mas ao comparar os dois logs lado a lado
apareceu **uma diferença real e concreta** entre o boot que desenha e o que não desenha,
e ela é sobre o gestalt:

**iOS 16.7.16 (`s9port/logs/evidence-20261029`, chega ao SpringBoard):**

```
MSUEarlyBootTask: MSUEarlyBootTask running
main: Content from the ramdisk will be present at /private/var/MobileSoftwareUpdate//5cbe36…-MSUData if it exists
MSUEarlyBootTask: I have nothing to do. Goodbye!!
```

**iOS 14.3 (`~/s9work/logs/log33.txt`, não desenha):**

```
main: MSUEarlyBootTask running
SWEET Sandbox: MSUEarlyBootTask op 2
copy_path_for_booted_os_data: Failed to query gestalt for the BootManifestHash
main: Unable to get path for booted OS data
```

O `MSUEarlyBootTask` do 14.3 **chama o gestalt e falha**; o do 16 **não chama** — ele
imprime o caminho do MSUData direto, sem consultar o gestalt. Contagem: `Failed to query
gestalt` aparece **1×** no log do 14.3 e **0×** no do 16.

Isso não prova que o gestalt seja a parede, mas é a primeira diferença *observada* entre
os dois boots que toca o caminho do `containermanagerd` (§3.2: ele consulta o gestalt
quando o commpage é 0). E é grátis de investigar: são logs que já temos.

**Próximo passo, sem aparelho:** descobrir por que o `MGCopyAnswer("BootManifestHash")`
do 14.3 falha e o do 16 nem é tentado. Duas hipóteses a separar:
(a) o `MSUEarlyBootTask` do 14.3 é uma versão diferente que consulta o gestalt onde o do
16 não consultava — comparar os dois binários responde; (b) o gestalt do 14.3 está
quebrado e o do 16 funcionava — aí o `MobileGestaltHelper` do 16 (que roda, `sel 8` e
`sel 28`) é a diferença.

**Estado das duas:** resolvidas nesta sessão, extraindo o binário do iOS 16 do
`work/iosroot/ios16-root-v2.img` (HFSX, lido com `work/hfslocal.py`, que reaproveita o
B-tree do `s9hfs.py` trocando a camada adb por um arquivo local).

**Hipótese (a) está morta.** Os dois `MSUEarlyBootTask` têm o **mesmo** código de gestalt:
ambos importam `_MGCopyAnswer`, ambos têm as strings `Failed to query gestalt for the
BootManifestHash`, `Unable to get path for booted OS data` e o formato `%s/%s-MSUData`.
Não é uma build diferente que pergunta onde a outra não perguntava.

**Hipótese (b) investigada em 2026-11-16 e DERRUBADA em teste com o aparelho.**

Registrei aqui, antes do teste, que a causa seria o `libMobileGestalt` lendo
`IODeviceTree:/chosen/secure-boot-hashes` e que o `mkadt.py` não criava esse nó.
Implementei o nó, flashee, bootei, e **a mensagem não mudou**:

```
copy_path_for_booted_os_data: Failed to query gestalt for the BootManifestHash
main: Unable to get path for booted OS data
```

A razão é simples e está nas próprias imagens: **o boot do iOS 16 que chega ao
SpringBoard tem exatamente o mesmo ADT** — só a propriedade plana, sem o nó, e o
**mesmo hash** de 48 bytes (`5cbe36011d7d8315…`). O `mkadt.py` já produzia uma
árvore estruturalmente igual a uma que funciona. Acrescentar o nó não podia mudar
nada, e não mudou.

O erro de método: a string `IODeviceTree:/chosen/secure-boot-hashes` **existe** no
`libMobileGestalt` do 18C66, e eu tratei a existência do caminho como prova de
obrigatoriedade. Não é. O binário do 20H392 (que não tenho) chega à mesma resposta
sem ele.

O texto abaixo fica como registro do raciocínio que falhou, não como conclusão.
O resultado completo e a análise do que sobrou estão em
`s9/SBH-TEST-2026-11-16-negative.md`.

---

### O raciocínio original (incorreto, mantido por transparência)

A prova que eu achei que tinha era a mensagem de sucesso: A prova
está na mensagem de sucesso:

| | `Content from the ramdisk will be present at` |
| --- | --- |
| iOS 16 (`evidence-20261029`) | **1×** — imprimiu o caminho, logo o gestalt respondeu |
| iOS 14.3 (`log33.txt`) | **0×** — nunca chegou lá |

O mesmo binário, o mesmo código, resultados opostos. Seguindo a cadeia:

`libMobileGestalt.dylib` (`0x18f37cc98`) mostra de onde a chave sai:

```
IODeviceTree:/defaults
IODeviceTree:/chosen/secure-boot-hashes   <-- aqui
sfr-manifest-hash
crypto-hash-method
boot-manifest-hash
Unable to copy the bootManifestHash
```

O gestalt não lê a propriedade `boot-manifest-hash` solta: ele lê o dicionário
**`secure-boot-hashes` sob `/chosen`** e tira a chave de dentro. E os dois ADTs diferem
exatamente aí:

```
work/ipsw/d22ap.adt   (Apple, da IPSW)   tem "chosen" e tem "secure-boot-hashes"
s9port/out/s9.adt     (gerado pelo port) tem "chosen", NAO tem "secure-boot-hashes"
```

O `mkadt.py` até cria uma propriedade `boot-manifest-hash`
(`tools/mkadt.py:57`, `BOOT_MANIFEST_HASH = sha384(b'sweet boot manifest')`) — e esse
valor sintético **chega a ser usado**, está no path do preboot no `log33.txt:127`. Mas
ele fica numa propriedade plana que o gestalt não consulta. O gestalt quer o nó
`/chosen/secure-boot-hashes`, e esse o `mkadt.py` não cria.

**Então:** o `MobileGestaltHelper` responde nil para `BootManifestHash`, o
`MSUEarlyBootTask` falha, e o cache do gestalt
(`…/systemgroup.com.apple.mobilegestaltcache/Library/Caches/com.apple.MobileGestalt.plist`,
que também não existe na imagem) nunca é populado.

**Ação proposta na época (executada e reprovada):** emitir `/chosen` com um filho
`secure-boot-hashes`. Foi feito (`4d144b0`), flashado e bootado — não mudou nada. O nó
ficou no `mkadt.py` porque é inofensivo, com comentário dizendo que não é um fix.

O teste do `sweetnull=...,keybagd` continua valendo, mas por outro motivo: ver o que o
keybagd faz depois de `setupUserSessionStart` — se constrói o cache do gestalt ou morre
antes.

Não fiz nenhuma alteração de código nesta sessão. As conclusões acima são leitura de
fonte e de binário. A que muda uma decisão é a do §4: **o commpage sai da lista de
suspeitos**, e o gestalt entra.
