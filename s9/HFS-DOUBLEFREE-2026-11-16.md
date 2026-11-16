# Causa A resolvida: não é corrupção de disco. É um duplo-free.

**2026-11-16.** Verificado offline contra a imagem, não inferido.

## A prova

Imagem: `/Volumes/Linux/@home/caelestia/S9-iOS14-Development/build/ios14-flash-ready/ios14-rootfs.hfs`
(11 GB, HFS+, `blockSize=4096 totalBlocks=2621440 freeBlocks=233445`).

O bitmap de alocação lido linha a linha bate **exatamente** com o cabeçalho:
233.445 bits livres, na convenção HFS+ (`1 = usado`). Conferido:

```
totalBlocks=2621440  bits=1: 2387995  bits=0: 233445
freeBlocks no header: 233445
-> 1=usado?  SIM
```

**A imagem não está corrompida.**

## O que o kernel reclama

`log-sweetnull.txt`, 98 ocorrências, sempre com a mesma forma:

```
hfs: BlockMarkFreeInternal() trying to free unallocated blocks on volume iPhoneOS <2388090, 1>
```

Blocos reclamados: 2388090, 2388171, 2388177, 2388229, 2388259, 2388267,
2388763, 2388764, 2396487, 2396529, 2396530, 2396531.

Estado de cada um **no bitmap da imagem**:

| bloco | bit | significado |
| --- | --- | --- |
| 2388090 | 0 | LIVRE |
| 2388763 | 0 | LIVRE |
| 2396530 | 0 | LIVRE |
| … (todos os 12) | 0 | LIVRE |

Ou seja: **o kernel está tentando liberar blocos que já estão livres.** É um
double free, não um bitmap inconsistente.

## Por que isto importa tanto

`VolumeAllocation.c`, `BlockMarkFreeInternal`, os três `goto Corruption`
(`:3237`, `:3274`, `:3310`):

```c
if ((do_validate == true) &&
        (*currentWord & SWAP_BE32 (bitMask)) != SWAP_BE32 (bitMask)) {
    goto Corruption;
}
```

A validação pergunta "este bloco está marcado como alocado?" — e a resposta
legítima para um free normal é **sim**. Quando a resposta é não, o HFS conclui
corrupção e chama `hfs_mark_inconsistent(vcb, HFS_INCONSISTENCY_DETECTED)`, que
seta `kHFSVolumeInconsistentMask` no VCB — **bit persistente, só o `fsck_hfs`
limpa** (`hfs_vfsops.c:4439`).

Uma única ocorrência envenena o volume para o resto do boot. Daí as 98.

## O que isso muda

A causa A **não** é "a imagem está ruim, refaça o rootfs". O Bergamota dizia
*"re-flash before every test boot"* porque via o mesmo sintoma e assumiu que era
a imagem; a medição mostra que **a imagem está boa** e que a culpa é de algo no
caminho de free do `sweet_hfs` — o HFS+ de fonte aberta que este port compila no
lugar do driver da Apple.

Candidatos concretos, em ordem:

1. **`hfs_truncate` / liberação de extents**: o log mostra `hfs: error truncating
   data fork!` logo depois da primeira ocorrência (linha 122), e `BlockMarkFreeInternal`
   é chamado a partir de `hfs_free_extents` (`VolumeAllocation.c:1620`).
2. **A extensão do bitmap**: `allocationFile` = 1 extent, blocos 1..80. Se o
   código de free calcular o índice de bit com o bloco errado, o double-free cai
   sempre nos números altos — e todos os 12 estão entre 2.388.090 e 2.396.531,
   perto do fim do volume.
3. **`GetMasterBlock: Error 16` (`EBUSY`)** imediatamente antes: o `mount_hfs`
   do userspace não conseguiu abrir `/dev/ufsroot0` e caiu no caminho alternativo.
   Vale saber se o mount está acontecendo duas vezes.

## Como verificar o candidato 2 (barato, sem aparelho)

`BlockMarkFreeInternal` recebe `startingBlock_in` e `numBlocks_in`. Todos os
números reclamados vêm do intervalo alto do volume. Se o índice de bit estiver
sendo calculado como `startingBlock - algumOffset`, os blocos do começo do volume
iriam parar no fim. Um `printf` temporário no `goto Corruption` imprimindo
`startingBlock_in`, `numBlocks_in` e `blockNum` mostraria isso direto.

Isso exige reconstruir o kernel — que agora **funciona** (`s9/BUILD-7195-2026-11-16.md`),
mas ainda não boota com as fontes atuais (`s9/BOOTLOOP-2026-11-16-new-kernel.md`).
Logo: primeiro resolver por que o build atual não boota; só depois instrumentar.


---

## Confirmado no aparelho, não só na imagem

O bitmap foi lido **do próprio `sda29`** (80 blocos a partir do bloco 1) e comparado
com o cabeçalho do volume no aparelho:

```
header do aparelho : freeBlocks = 230182
bitmap do aparelho : bits=1 (usados) = 2391258, livres = 230182   -> bate
bloco 2388090 usado = 0
bloco 2388763 usado = 0
bloco 2396530 usado = 0
```

(A imagem de referência em `ios14-flash-ready/` tem 233.445 livres; o aparelho tem
230.182. A diferença de 3.263 blocos ≈ 13,4 MB é o que o boot escreve. Normal.)

Então o diagnóstico se mantém com a fonte real: **o kernel está liberando blocos
que o bitmap diz que já estão livres**, e o `BlockMarkFreeInternal` corretamente
chama isso de corrupção — o erro é de quem pediu o free, não do bitmap.

## O que se sabe do "quem pediu"

A cadeia, lida no código:

```
hfs_remove_orphans            hfs_vfsutils.c:1739
  -> TruncateFileC            FileExtentMapping.c:1251
    -> DeallocateFork         FileExtentMapping.c:583
      -> ReleaseExtents       FileExtentMapping.c:466
        -> BlockDeallocate    VolumeAllocation.c:1577
          -> BlockMarkFreeInternal(vcb, firstBlock, numBlocks, true)   linha 1625
```

O `true` no último argumento é o `do_validate` — é o que faz a validação que
dispara. E `BlockDeallocate` já tem uma checagem própria **só sob `ALLOC_DEBUG`**
(`VolumeAllocation.c:1601-1609`), que estava desligada:

```c
if (ALLOC_DEBUG) {
    if (firstBlock >= hfsmp->totalBlocks) panic(...);
    if ((firstBlock + numBlocks) >= hfsmp->totalBlocks) panic(...);
}
```

O log mostra `hfs: Removed 1 orphaned / unlinked files and 17 directories` — ou
seja, o caminho é exatamente o `hfs_remove_orphans`, e o `hfs: error truncating
data fork!` que vem em seguida confirma que o truncate falhou.

## Descartado por medição

| hipótese | prova |
| --- | --- |
| bitmap da imagem corrompido | 233.445 na imagem, 230.182 no aparelho, ambos batem com o header |
| `vcbVBMIOSize` errado | `MAXPHYS` = 64K (arm/param.h:68) > blockSize 4096, então `min()` dá 4096 — correto |
| HFS modificado por nós | `VolumeAllocation.c` é **idêntico** ao do port do OnePlus 2 (`refs/veerhooda-xnu/bsd/op2_hfs/`) |
| blocos com dados | os reclamados são todos zero; o último bloco usado real (2387993) tem XML |

## Próximo passo, agora com alvo preciso

`hfs_remove_orphans` está liberando extents de arquivos órfãos cujos blocos já
foram liberados antes. Duas leituras possíveis, ambas verificáveis no código:

1. O extent record lido do B-tree de extents contém blocos que já foram liberados
   por uma passada anterior (o loop de `TruncateFileC` chama `TruncateFileC` de
   novo em `fsize > 0`).
2. `DeallocateFork` libera os extents do catálogo **e** depois `TruncateExtents`
   libera os do B-tree de extents, e o mesmo bloco aparece nas duas fontes.

O lugar exato para um `printf` (quando houver kernel que boote) é
`VolumeAllocation.c:1625`, imprimindo `firstBlock`, `numBlocks` e o `fileID`
do chamador — que já está disponível na pilha do `ReleaseExtents`.
