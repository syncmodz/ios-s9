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
