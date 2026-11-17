# Estado do bootloop do kernel novo — o que sei e o que falta

## O que você viu (informação nova e decisiva)

- aparece o **shim**
- aparece o **console**
- **menos de 10 linhas**
- crasha e reinicia
- (e o retorno ao TWRP foi manual, pelos botões)

Isso muda o diagnóstico: o kernel **roda**. Ele não é rejeitado pelo bootloader.
Ele morre cedo, dentro do bootstrap.

## Onde, exatamente

O kernel que funciona imprime, em ordem:

```
1   Darwin Kernel Version ...
2   pmap_startup() init/release time
3   pmap_startup() delayed init/release
4   vm_page_bootstrap: 218374 free pages
5   "vm_compressor_mode" is 4
6   zone_init: submap[0] ...
7   zone_init: submap[1] ...
8   zone_init: submap[2] ...
9   zone leak detection enabled
10  oslog_init completed, 16 chunks, 8 io pages
11  standard timeslicing quantum is 10000 us
12  standard background quantum is 2500 us
13  WQ[wql_init] ...
14  WQ[wqp_init] ...
15  mig_table_max_displ = 53 ...
```

Se o novo morre com **menos de 10 linhas**, ele pára entre a 1 e a 10 — antes de
`oslog_init`. Isso é a fase de pmap/vm, antes de qualquer driver.

## O que é novo e roda nessa janela

Comparando as strings dos dois kernels (12 nov × 16 nov), o que existe **só** no
novo e pode executar antes de `oslog_init`:

| item | onde | o que faz |
| --- | --- | --- |
| `sweet_wdt_arm` | `osfmk/kern/startup.c:784` | arma o watchdog (bark 90 s / bite 95 s) |
| `commpage_update_multiuser_config(501)` | `startup.c:806` | escreve a palavra multiuser na commpage |
| `SWEET_MARK(32..35)` | `startup.c:385-427` | pinta a barra no framebuffer + crumb |

O watchdog é o que explica o **reset**: com `sweetwdt=0` o aparelho ficou 12,5 min
sem reiniciar, contra 45 s. Isso está provado.

Mas com o watchdog desligado ele **não avançou** — ficou parado. Então há um
travamento separado, antes do watchdog morder.

## Duas incoerências que encontrei no código

1. `sweet_early_mark()` escreve na "crumb page" em `0x9d000000` e o comentário diz
   *"see s9port/shim/crumbs.h for the layout"*. **Esse arquivo não existe**, e o
   shim não lê `0x9d000000` nem `0x53574300`. O mecanismo de crumb não funciona.

2. `sweet_wdt.c` termina em `for (;;) wfe;` com o comentário *"the watchdog
   bites"*. Com `sweetwdt=0` ele espera **para sempre**. Isso é exatamente o que
   se viu (12,5 min parado).

## O que falta para fechar

Uma leitura de tela: **quantas linhas aparecem, e qual é a última**. O console do
`sweet_early` imprime `SWEET EARLY NNN: <label>` a cada marca. A última linha
visível diz o ponto exato.

Sem isso eu só tenho hipóteses. Com isso eu sei qual função instrumentar.
