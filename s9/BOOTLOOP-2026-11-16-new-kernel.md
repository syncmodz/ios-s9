# Kernel novo = bootloop. Restaurado. O que aprendi e o que não sei

**2026-11-16, ~19:39–19:50.** O dono autorizou o boot. O kernel de 16 nov entrou em
**bootloop**: reiniciava sozinho a cada volta, e ele voltava ao TWRP na mão.

## Estado do aparelho agora

**Restaurado e verificado.** `sda20` = `af1b70376ce01f8bd2c31804de4c6eb9685334a81b8f5917fedd75a3e1e026c3`
— o boot que bootava (12 nov). A partição `misc` foi lida: **todos os 4096 bytes
zerados**, nenhum `boot-recovery` gravado. Nada pendente.

## O que aconteceu, com evidência

Duas tentativas, e a diferença entre elas é o dado:

| boot image | resultado | log do kernel |
| --- | --- | --- |
| shim recompilado + kernel 16 nov | volta ao TWRP em **7 min** | **não regravado** |
| **shim de 12 nov** (do backup) + kernel 16 nov | volta ao TWRP em **45 s** | **não regravado** |

Nos dois casos o `sweetlog` em `sda29` continuou **byte-idêntico** ao de 12 nov
(`flush #9, total 63280 bytes`, versão `qui 12 nov 14:04:37`). Ou seja: o kernel
novo **não chega a inicializar o UFS**, então não escreve log — e não há panic
persistido. Sem log, o bootloop não deixa rastro; só a tela, que eu não vejo.

O `sweet_early_panic` (`osfmk/arm64/sweet_early.c:363`) tem um caminho que chama
`sweet_log_flush_panic()` e depois `sweet_bcb_request_recovery()`. Se o panic
acontece antes do UFS estar utilizável, o flush falha e nada é gravado — coerente
com o observado. E o BCB **não** ficou gravado, então esse caminho não chegou ao
fim, ou `sweetbcb` (TUNABLE, default 1) estava desligado.

## O que isto invalida da minha conclusão anterior

Eu disse que `/Users/ios/s9work/xnu7195-local` era a árvore que produziu o kernel
do aparelho, porque o string de versão dela diz `ios:xnu7195-local/...`. **Isso
não se sustenta:** não há um único `.o` nesta árvore datado de 12 nov — os 844
objetos são todos de 15/16 nov. Uma árvore que buildou em 12 nov teria objetos
dessa data. O nome no caminho é igual, mas o conteúdo não é o que gerou aquele
binário.

Consequência prática: **este kernel não é "o de 12 nov com mais commits".** É um
build de fontes que eu não tenho como comparar com o que funcionava. O bootloop
pode estar em qualquer uma das mudanças de 15 nov (pmap, printf, sweet_early,
vfs, personas) ou na combinação com o shim.

## O que faria um build confiável

Buildar é barato agora (`s9/BUILD-7195-2026-11-16.md`, ~7 min). Falta o que
importa: **saber se um kernel recém-buildado das fontes antigas boota**. Se ele
bootar, o harness está certo e o problema é uma mudança de 15 nov — bisseccionável
por arquivo. Se ele **não** bootar, o problema é o meu build (toolchain/flags) e
nenhuma conclusão sobre os fontes vale.

Esse é o único teste que separa as duas coisas, e ele custa um flash e um boot.

## Não repetir

- Não dizer "está tudo pronto para o boot" quando o binário nunca foi executado.
  Eu verifiquei estrutura de payload e coerência de ADT — nenhuma das duas prevê se
  o código roda.
- O `shim` difere entre os dois boot.img (90.112 → 81.920 bytes de `.text`, mesmas
  strings, gcc diferente). A primeira tentativa misturou as duas variáveis; a
  segunda isolou o shim e o bootloop continuou, então **o shim não era a causa**.
