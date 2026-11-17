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


---

# ATUALIZAÇÃO: a tela diz `SWEET EARLY 32`

Lido na tela pelo dono: **aparece `SWEET EARLY 32` e para.**

## O que isso localiza, com precisão

`SWEET MARK(32)` é a **primeira** marca do `kernel_bootstrap`
(`osfmk/kern/startup.c:385`). A ordem no código é:

```c
printf("%s\n", version);
SWEET_MARK(32, "kernel_bootstrap entered");   <-- ULTIMA COISA VISIVEL
scale_setup();
SWEET_MARK(33, "scale_setup done");           <-- nunca imprime
vm_mem_bootstrap();
SWEET_MARK(34, "vm_mem_bootstrap done");
```

Então o kernel chega **ao fim do `arm_vm_init`** (as marcas 1-31 são de lá e
passaram), entra no `kernel_bootstrap`, imprime a 32 — e morre antes da 33.

## O que isso descarta

| hipótese | porque cai |
| --- | --- |
| travamento no pmap/`arm_vm_init` | a marca 32 só roda **depois** do `arm_vm_init` inteiro |
| `sweet_early_map_final` / `assert(*slot == ARM_TTE_EMPTY)` | vem antes da 32; se disparasse, a 32 nunca apareceria |
| framebuffer fora do mapeamento | calculado: a tela ocupa `0x9d400000..0x9dfff400`, o bloco mapeado é `0x9c000000..0x9e000000` — **cabe, com 3 KB de folga** |
| `scale_setup()` | é só aritmética com `max_mem_actual`/`task_max`; não aloca nem acessa hardware |

## O que sobra: a primeira pintura

A marca 32 é a **primeira** chamada de `sweet_early_putc` da vida deste kernel, e
é ela que executa o bloco `if (!early_started)`:

```c
for (y = 0; y < EARLY_BAR_TOP; y++)          // 2152 linhas
    for (x = 0; x < EARLY_WIDTH; x++)        // 1440 colunas
        line[x] = LOGO_BLACK;                // 3,1 M escritas de 32 bits
sweet_early_paint_logo();                    // 207x241 pixels do logo
sweet_early_clear_row();
```

São **~12 MB de escritas** no framebuffer. Se o MMU daquele bloco não estiver com
a atribuição que o código supõe (por exemplo `CACHE_ATTRINDX_WRITECOMB` — memoria
Normal-NC — sobre uma região que o `arm_vm_init` também mapeia como cacheable),
a escrita não falha imediatamente: ela pode **travar o barramento** ou gerar um
fault em background que só aparece no próximo acesso.

## O que fazer agora, e é barato

Duas coisas que não dependem de adivinhar:

1. **Marcar dentro do bloco `!early_started`.** Uma `sweet_early_putc` crua
   (sem passar pela pintura) antes do laço, e outra depois. Se a primeira aparece
   e a segunda não, o travamento é nas 12 MB de escrita. Isso é uma edição pequena
   e o build funciona.

2. **Testar sem a pintura.** Um boot-arg que pula `!early_started` inteiro
   (ex.: `sweetnologo=1`) mostraria se o kernel avança com o console funcionando e
   o logo desligado. Se avançar, é a pintura; se não, é outra coisa e o console
   simplesmente parou de poder escrever.

A (2) é a mais informativa e a mais rápida de escrever.
