# Achado central: o HFS do rootfs está inconsistente e se corrompe durante o boot

**2026-11-16, do log do boot com `sweetnull` (`log-sweetnull.txt`, flush #10).**

Isto é a causa que sobrevive a todos os outros testes. Não é persona, não é
commpage, não é gestalt, não é o ADT: **o sistema de arquivos corrompe a cada
boot**, centenas de vezes, e toda escrita que falha é uma escrita que o MCM
precisa.

## A evidência

Contagem no log (`log-sweetnull.txt`):

```
hfs_mark_inconsistent: Runtime corruption detected    98x
hfs_mark_inconsistent: Failed to complete operation   16x
```

E a **primeira** ocorrência é o achado, porque acontece antes de qualquer daemon
de usuário, no próprio mount:

```
EXEC mount_hfs pid=4
GetMasterBlock: Error 16 opening /dev/ufsroot0
hfs: BlockMarkFreeInternal() trying to free unallocated blocks on volume iPhoneOS <2388090, 1>
hfs_mark_inconsistent: Runtime corruption detected on iPhoneOS, fsck will be forced on next mount.
hfs: error truncating data fork!
```

`BlockMarkFreeInternal() trying to free unallocated blocks` significa uma coisa
só: o **bitmap de blocos livres no disco diz que um bloco está livre, e o
alocador está tentando liberá-lo**. O bitmap e as referências estão fora de
sincronia. Isso é estado **na imagem**, não um efeito colateral de algo que o
userspace fez.

## O código que dispara

`bsd/sweet_hfs/core/VolumeAllocation.c`, `BlockMarkFreeInternal`, três `goto
Corruption`, todos com a mesma forma (`:3237`, `:3274`, `:3310`):

```c
if ((do_validate == true) &&
        (*currentWord & SWAP_BE32 (bitMask)) != SWAP_BE32 (bitMask)) {
    goto Corruption;
}
```

e em `Corruption`, com `DEBUG` desligado:

```c
printf ("hfs: BlockMarkFreeInternal() trying to free unallocated blocks ...");
hfs_mark_inconsistent(vcb, HFS_INCONSISTENCY_DETECTED);
err = EIO;
```

`hfs_mark_inconsistent` (`hfs_vfsops.c:4439`) seta `kHFSVolumeInconsistentMask`
no VCB — um **bit persistente**, que só o `fsck_hfs` limpa. Ou seja: a primeira
ocorrência marca o volume como inconsistente e ele fica assim.

## Por que isto explica a parede inteira

O `Bergamota` documentou o mesmo elo para iOS 18 (`NOTES.md:76`): *"a second,
independent cause: MCM's sqlite DB fails on HFS `EINVAL`,
`hfs_mark_inconsistent`"*. A cadeia é:

1. A escrita do MCM (banco de containers) falha com `EIO`/`EINVAL`.
2. O MCM não consegue responder `container_system_path_for_identifier`.
3. `BSSystemContainerForCurrentProcessPathProvider libraryPath` devolve nil.
4. `-[NSURL initFileURLWithPath:]` recebe nil → backboardd aborta.

E a symbolicação exata desse abort está em **três** projetos (`ref/sweet
NOTES.md:88`, `ref/Bergamota NOTES.md:269`, `ref/Bergamota-gpu-eagl
NOTES.md:241`):

```
+[NSURL fileURLWithPath:]
  <- -[BSSystemContainerForCurrentProcessPathProvider libraryPath]
  <- cachesPath
  <- backboardd main+0x1d858   (StartWindowServer, shader cache path)
```

O `sweet` acrescenta a frase que fecha o caso (`NOTES.md:90`): *"never any
IOMobileFramebuffer / display lookup — the exception happens before display
detection"*. **O nosso sintoma é idêntico ao deles, e eles o resolveram sem
patcher o backboardd.**

## O que o `sweet` fez, e o que já temos

`tools/sweet-root-tweaks.py:74-83` cria
`/private/var/containers/{Bundle,Data/System,Shared/SystemGroup,Temp}`. **Nós já
fazemos isso** — `log-sweetnull.txt:215-227` mostra os 13 `SWEET fixup mkdir ...
err=0`. Então esse passo está cumprido e não era o que faltava.

O que pode ser diferente é o **estado do HFS na imagem**. O `sweet` parte de um
rootfs APFS convertido; o nosso é HFS+ montado com o `sweet_hfs`, e o `GetMasterBlock:
Error 16` (`EBUSY`) no log mostra o `mount_hfs` encontrando o dispositivo já
ocupado — vale investigar se a montagem está sendo feita duas vezes ou se o
volume já vem marcado.

## O que isto muda no plano

Antes de qualquer outra coisa: **verificar o rootfs offline**. Se o
`BlockMarkFreeInternal` reclama de blocos não alocados já no mount, o bitmap da
imagem está errado e nenhum patch de userspace resolve — o MCM vai continuar
falhando em escrever.

Ferramenta disponível e já auditada: `s9/tools/s9hfs.py` lê a imagem HFS+
offline, e `work/hfslocal.py` reaproveita o B-tree dele para arquivo local.

## O que NÃO é a causa (descartado com evidência nesta sessão)

| hipótese | prova de descarte |
| --- | --- |
| keybagd escondido pelo `/dev/null` | `sweetnull` estendido: 0 linhas `\|keybagd\|`; ele não escreve nada |
| XPC rejeitado no listener (`mach_port_is_connection_for_service`) | função **inexistente** no 7195 (existe no 8796); e 0 `Bogus check-in` no log |
| `secure-boot-hashes` / ADT | boot testado, negativo; ADT do iOS 16 que funciona é igual |
| commpage multiuser | 0 é o valor normal; o boot iOS 16 que desenha também tem 0 |
