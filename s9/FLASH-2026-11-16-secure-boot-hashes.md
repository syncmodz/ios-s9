# Flash do ADT com `secure-boot-hashes` — 2026-11-16

**Feito no aparelho.** O dono autorizou flash e análise, pediu para não dar boot
naquele momento, e depois autorizou o boot — que **reprovou a hipótese**. O resultado
está em `s9/SBH-TEST-2026-11-16-negative.md`; este arquivo é o registro do flash.

## O que estava no aparelho

Extraído do backup do `sda20` antes de tocar em nada
(`backups/boot-sda20-20261116.img`, SHA256 `af1b70376ce01f8bd2c31804de4c6eb9685334a81b8f5917fedd75a3e1e026c3`,
67.108.864 bytes, conferido contra o `sha256sum` do próprio aparelho).

```
kern: Darwin Kernel Version 20.2.0: qui 12 nov 2026 14:04:37 -03
      ios:xnu7195-local/BUILD/obj/DEVELOPMENT_ARM64_SWEET
```

**Este era o kernel 7195 que não existia em nenhuma árvore local.** Não está no
macOS nem no volume Linux (`S9-iOS14-Development`) — só o 8796. Estava apenas no
aparelho. Payload extraído para `backups/payload-7195-current.bin`.

O ADT dentro dele **não tinha** `secure-boot-hashes`: era exatamente o bug que
`s9/COMMPAGE-MULTIUSER-2026-11-16.md` descreve.

## O que foi gravado

`out/boot.img`, SHA256 **`45591498e6dd3a6af5f2fb2cd48e13a36fb57b5b6b7380366c72ca5ce0a94f5d`**,
23.248.896 bytes.

Construído trocando **apenas o ADT** no payload extraído do aparelho — o kernel
7195 não foi recompilado nem alterado:

| | antes | depois |
| --- | --- | --- |
| kernel | 7195 (12 nov, 14:04:37) | **idêntico** |
| `entry` | `0xfffffff0072d8488` | idêntico |
| `dram-base` | `0xa6000000` | idêntico |
| `dram-size` | `0xd80c0000` | idêntico |
| cmdline | 8 cpus, rd=ufsroot0, … | idêntica |
| ADT | 12.792 B, **sem** `secure-boot-hashes` | 12.980 B, **com** |

O shim M4 foi recompilado (90.112 → 81.920 bytes de `.text`); as strings são
idênticas byte a byte, é o mesmo programa, só outro gcc. Os 15 DTBs no blob são
idênticos (11.167.815 bytes, mesmo SHA256).

## Verificação

Três conferências independentes, todas em cima do que o aparelho devolveu:

1. `tools/flash-twrp.sh` — dd + readback próprio: hash bate.
2. Leitura nova do `sda20` com `dd if=/tmp/vb.img bs=4096 count=5676 | sha256sum`
   → `45591498…`, **igual ao arquivo**.
3. ADT relido do aparelho: `secure-boot-hashes` presente, `boot-manifest-hash` de
   48 bytes (`5cbe3601…`), e a propriedade plana continua lá (XNU precisa dela).

**A cauda da partição não é zero** e isso é esperado: `sda20` tem 64 MiB e o novo
boot tem 23 MB, então sobram 43,8 MB do boot antigo (4.080.858 bytes não-zero,
exatamente o mesmo número do arquivo antigo). O shim lê o payload pelo tamanho
declarado no header, não até o fim da partição — ver `shim/loader.c:93`
(`p->version != 1 || (p->adt_off & 7) || (rd_start & 7)`) e o `adt_size` do
header. Não é corrupção.

## Estado e o que falta

- **O aparelho foi bootado neste estado e o teste falhou** (ver
  `SBH-TEST-2026-11-16-negative.md`); depois o boot original foi restaurado e
  verificado.
- O boot antigo está intacto em `backups/boot-sda20-20261116.img` (+ `.sha256`) —
  restaurar é `dd` desse arquivo de volta.
- **O que este flash testa:** se o `MSUEarlyBootTask` para de logar
  `Failed to query gestalt for the BootManifestHash` e passa a imprimir
  `Content from the ramdisk will be present at`. Uma linha no log decide.
- **O que ele não testa:** se o `containermanagerd` passa a receber containers.
  Isso depende de `MGGetBoolAnswer("3kmXfug8VcxLI5yEmsqQKw")`, que é outra chave
  do gestalt.
- O `tools/bootimg-check.py` **aborta** neste par de imagens (`BadGzipFile`, magic
  `SW`) porque o ramdisk é o payload `SWEETXNU` cru, não gzip. Acontece igual com
  o boot que já estava no aparelho, então é limitação do script, não do build.
  Vale corrigir antes da próxima.
