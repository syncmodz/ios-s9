# O `secure-boot-hashes` NÃO era a causa — boot testado, hipótese descartada

**2026-11-16, com o aparelho.** O dono autorizou boot. O aparelho foi flashado com
o ADT que tem `/chosen/secure-boot-hashes`, **bootou**, e o resultado foi **negativo**.
Revertido e verificado.

## O teste

Boot completo (946 linhas no log persistente, flush #9, kernel
`xnu7195-local`, 12 nov 14:04:37). A linha decisiva, **idêntica à de antes**:

```
copy_path_for_booted_os_data: Failed to query gestalt for the BootManifestHash
main: Unable to get path for booted OS data
```

O boot gravado foi confirmado antes de reiniciar (`sha256` da região escrita =
`45591498…`) e o ADT relido do aparelho tinha o nó, com hash de 48 bytes.

## Por que a hipótese estava errada — a prova

**O boot do iOS 16 que funciona tem exatamente o mesmo ADT: só a propriedade plana,
sem o nó.**

```
ios16 (funciona)   secure-boot-hashes=False  flat=True  filhos=['memory-map','asmb']
ios14 (nao)        secure-boot-hashes=False  flat=True  filhos=['memory-map','asmb']
```

E o **hash é idêntico** nos dois (`5cbe36011d7d8315…`, 48 bytes). O `mkadt.py` gerava
um ADT estruturalmente igual ao que funciona no 16. Acrescentar o nó não podia
mudar nada, e não mudou.

Onde eu errei: vi a string `IODeviceTree:/chosen/secure-boot-hashes` no
`libMobileGestalt` do 14.3 e assumi que o caminho era obrigatório. É um caminho que
**existe** no código, mas o do 16 chega ao mesmo resultado com a propriedade plana —
provavelmente porque o `libMobileGestalt` do 20H392 é outro binário. Não tenho esse
cache para comparar; o do 14.3 é o único aqui.

## A diferença real que sobrou

Comparando os dois logs, uma coisa distingue os boots e não tem nada a ver com o ADT:

| | `MobileGestaltHelper` executa? |
| --- | --- |
| iOS 16 (funciona) | **sim** — `exec MobileGestaltHelper`, e `SWEET FB: … sel 8 / sel 28` |
| iOS 14.3 (não) | **não** — zero ocorrências em 946 linhas |

O job `com.apple.mobilegestalt.xpc` (`/usr/libexec/MobileGestaltHelper`, user
`mobile`, **sem** `RunAtLoad` — é on-demand por XPC) nunca é disparado no 14.3.

Mas o `libMobileGestalt` do 14.3 **não tem** a string `com.apple.mobilegestalt.xpc`
nem `MobileGestaltHelper` — verificado no binário. Ou seja, ele não chama o helper;
lê o IORegistry direto. Então "o helper não roda" pode ser consequência, não causa.

**Isto é onde a investigação para, e o próximo passo é medição, não leitura:** se o
gestalt do 14.3 lê o IORegistry direto, a pergunta é o que o
`IORegistryEntry::fromPath("/chosen/secure-boot-hashes")` devolve em runtime. Uma
sonda de uma linha em `IOGetBootManifestHash` responde — mas **exige recompilar o
kernel 7195**, e essa árvore não está pronta: `sources/apple-xnu-7195` não tem
`config/MASTER.arm64.sweet` e `build/xnu7195-port` nunca teve `BUILD/`.

## Estado do aparelho

**Restaurado.** O `sda20` foi regravado com o backup e verificado:
`sha256` = `af1b70376ce01f8bd2c31804de4c6eb9685334a81b8f5917fedd75a3e1e026c3`,
idêntico ao original. O boot no aparelho é o de antes deste experimento.

## O que fica

- O `mkadt.py` **mantém** o nó `secure-boot-hashes`? **Não justifica** — a mudança
  foi feita com base numa hipótese que o teste derrubou. Está no commit `4d144b0`.
  Não reverto agora porque o nó é inofensivo (o kernel ignora o que não procura e o
  caminho existe no código), mas **não é um fix** e o comentário no arquivo diz que
  era. Corrigir o comentário é devido.
- A pergunta aberta que vale: por que o gestalt do 14.3 falha onde o do 16 não, com
  o mesmo device tree e o mesmo hash. É userspace, não ADT.
