# Boot com `sweetnull`: o keybagd não está silencioso, ele está mudo

**2026-11-16, 20:05.** O kernel que já boota (12 nov), com um único boot-arg
diferente. Sem kernel novo, sem risco de regressão — `entry`, segmentos e kernel
são byte-idênticos ao backup; só a cmdline mudou (conferido: 72 bytes, todos em
`0x48..0x2a8`).

```
sweetnull=backboardd,SpringBoard,keybagd,containermanagerd,usermanagerd
```

## Resultado: NEGATIVO, e isso é informação

`flush #10`, log novo (64.263 bytes), boot completo até `Early boot complete`
(linha 357), backboardd morre na **mesma** exceção (linha 670).

Contagem de linhas com prefixo `|nome|` no console:

| nome | linhas |
| --- | --- |
| backboardd | 15 |
| **keybagd** | **0** |
| **containermanager** | **0** |
| SpringBoard | 0 |

**O keybagd daemon (pid 38) não escreve uma única linha em stdout.** O
`sweetnull` foi parseado e chegou ao kernel (está no `boot-args` da linha 133),
então o mecanismo de tee funcionou para o backboardd — mas não havia nada para
tecer vindo do keybagd.

### O que isso mata

`USERSESSION-KB-2026-11-16.md` §1.1 e §6 tratavam o silêncio do keybagd como
"invisibilidade por `/dev/null`" e listavam `sweetnull=...,keybagd` como *"o teste
mais barato e o mais informativo desta lista"*. **Está resolvido, e o resultado é
que não há nada escondido.** O daemon não loga porque não chega a logar — não
porque o log esteja sendo descartado.

Isso aponta para o próximo lugar concreto: o keybagd (pid 38) faz `AKS sel 0` e
para. Ou ele morre sem sinal (o log não mostra `EXIT keybagd pid=38`), ou está
bloqueado esperando algo. A pergunta deixa de ser "o que ele escreveria" e passa a
ser "onde ele está parado".

### Ruído que não é regressão

`SEGV launchd` aparece 8× neste log — e 8× no anterior, com os mesmos `pc`. É o
`debug=0xc` interceptando falhas normais do dyld durante o carregamento, não um
defeito. Verificado por contagem nos dois arquivos.

## O que este teste não mostrou

O `containermanagerd` também não apareceu, mas por um motivo técnico que eu já
sabia e não corrigi a tempo: o matching do `sweetnull` é por `p_comm`, limitado a
`MAXCOMLEN=16` (`bsd/sys/param.h:95`), então `containermanagerd` (17 chars) nunca
casa — o nome certo é `containermanager`. A versão corrigida está montada em
`/tmp/payload-sweetnull2.bin` e não foi usada neste boot.

## Próximo passo, com o que este log abriu

O keybagd não é silencioso — é mudo. Então:

1. `sweetnull` corrigido (`containermanager`) para ver o lado do MCM.
2. Para o keybagd, o caminho não é console: é ver **onde ele para**. Ele roda como
   processo; o `sweetsample` (`sweetsample=-` está desligado no boot-args) é a
   ferramenta que já existe para amostrar pilha de processo.
