# Kernel 7195 novo: build configurado, flashado, verificado

**2026-11-16.** O dono pediu o build do 7195 configurado e o kernel novo no
aparelho. Feito, e o comando está em `s9/BUILD-7195-2026-11-16.md`.

## O que entrou

| | |
| --- | --- |
| árvore | `/Users/ios/s9work/xnu7195-local` (a certa, ver o doc de build) |
| kernel | `Darwin Kernel Version 20.2.0: seg 16 nov 2026 19:14:36 -03; ios:xnu7195-local/...` |
| boot.img | `out/boot.img`, sha256 `c9b0abd4b9d5fd0c537ccbc5189da274b806059529bfc655820a380887eb1241`, 23.265.280 bytes |
| flash | feito com `tools/flash-twrp.sh`, **sem reboot** |
| verificação | leitura nova do `sda20` sobre a região escrita: hash idêntico |

O aparelho **não foi bootado**. O dono pediu o kernel no aparelho; o boot fica
para quando ele mandar.

## O que este kernel tem de novo em relação ao de 12 nov

Os fontes mais recentes da árvore são de 15 nov, e são esses que entram agora:

- `osfmk/arm64/sweet_early.c` (21:55) + `osfmk/arm64/sweet_logo.h` (21:48) —
  `sweet_early_paint_logo()` desenha um logo no framebuffer **antes** do
  userspace existir, com `logo_palette` vindo de `mklogo_color.py`.
- `iokit/Kernel/arm/SweetFramebuffer.cpp` (21:26)
- `osfmk/kern/printf.c` (21:24) — o caminho de panic no log persistente
- `osfmk/arm/pmap.c` (20:34)
- `bsd/vfs/vfs_syscalls.c`, `bsd/kern/sys_persona.c`, `osfmk/bank/bank.c`,
  `bsd/kern/kern_persona.c`, `bsd/sys/persona.h` (madrugada de 15 nov)

## O que isso permite testar

**O logo é visível.** Se `sweet_early_paint_logo()` funcionar, aparece uma
imagem na tela antes de qualquer daemon — e isso separa "a tela não funciona"
de "o SpringBoard não desenha", que até agora estavam juntos. É a primeira
medida de display que não depende do userspace.

## O que o flash NÃO muda

O backboardd continua morrendo na mesma exceção, e ela está no log do boot
anterior (`log-sbh-boot1.txt:672`):

```
*** Terminating app due to uncaught exception 'NSInvalidArgumentException',
    reason: '*** -[NSURL initFileURLWithPath:]: nil string parameter'
```

O frame `0x1022b2c48` do stack **não mapeia** nos segmentos do
`/usr/libexec/backboardd` do rootfs (`__TEXT` vm `0x100000000`, `__LINKEDIT`
até `0x10011a620`) — ou seja, o endereço é de outra imagem carregada (o stack
mistura frames de `CoreFoundation`, `libobjc` e do próprio app). Resolver isso
exige o relatório `.ips` que o `ReportCrash` (pid 81, `EXIT status=0` no log)
já escreveu, ou um `atos` sobre o backboardd com o slide do processo — não dá
para deduzir só do log do kernel.

## Reverter

O boot anterior está em `backups/boot-sda20-20261116.img` (sha256
`af1b7037…`), íntegro. `dd` de volta no `sda20` restaura o estado.
