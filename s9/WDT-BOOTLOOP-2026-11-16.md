# Bootloop do build novo: o watchdog que o kernel antigo não tinha

**2026-11-16.** Comparação das strings dos dois kernels — o que boota (12 nov,
extraído do aparelho) e o build novo (16 nov) — isolou a diferença que explica o
loop.

## O achado

Sete strings existem **só** no kernel novo, e todas são do watchdog:

```
SWEET WDT: armed, bark %us bite %us
SWEET WDT: ml_io_map failed, watchdog stays off
SWEET WDT: disabled by boot-arg
sweetwdt
sweet_wdt_arm
sweet_wdt_disarm
sweet_wdt_pet
```

Confirmado por busca literal no kernel que boota: nenhuma delas existe lá. O
watchdog é **novo no 16 nov**.

## Por que isso produz bootloop

`bsd/dev/sweet_wdt.c`:

```c
#define SWEET_WDT_PA        0x17980000UL     /* qcom,wdt@17980000 */
#define SWEET_WDT_EN        0x08
#define SWEET_WDT_BARK      0x10
#define SWEET_WDT_BITE      0x14
#define SWEET_WDT_HZ        32765UL
#define SWEET_WDT_DEFAULT_S 90
```

- `sweet_wdt_arm()` programa BARK em 90 s e BITE em 95 s e liga o periférico.
- `sweet_wdt_pet()` escreve `RST`; é chamado de `rtclock_intr`
  (`osfmk/arm/rtclock.c:307`), no tick de timer de **cada** CPU.

Se o pet não chega — porque o `rtclock_intr` não roda, porque o mapeamento
`ml_io_map` devolveu outro endereço, ou porque o tick para — **o hardware
reinicia o SoC em 95 s**. Isso é um bootloop por construção.

E bate com o que foi medido: o boot com o kernel novo voltou ao TWRP em **45 s**
na segunda tentativa; o kernel de 12 nov levava minutos. Um reset de hardware
explica o tempo curto e o fato de o log **não ser regravado** (o kernel morre
antes de inicializar o UFS).

## O teste

`sweetwdt=0` está implementado no próprio arquivo:

```c
if (PE_parse_boot_argn("sweetwdt", &secs, sizeof(secs))) {
    if (secs == 0) {
        printf("SWEET WDT: disabled by boot-arg\n");
        return;
    }
}
```

Então: kernel de 16 nov + `sweetwdt=0` na cmdline, nada mais mudado. Montado a
partir do `boot.img` que boota, com o payload novo e a cmdline estendida.

| | |
| --- | --- |
| boot.img | `6abf1266e3428db297212ee74cd28b7eead47faa1f7613cd017e79e01fb9e09f` |
| verificação | hash lido do `sda20` bate com o arquivo |
| kernel | 16 nov 19:14:36 (o que entrava em loop) |
| única diferença | `sweetwdt=0` |

## Resultado do teste (2026-11-16, 20:49–21:02)

**O watchdog ERA a causa do loop rápido — mas não é a única coisa quebrada.**

Medido:

| | com `sweetwdt` (padrão 90 s) | com `sweetwdt=0` |
| --- | --- | --- |
| volta ao TWRP | **45 s** | **750 s** (12,5 min), e só porque o dono tirou da mão |
| log regravado | não | **não** |
| BCB de recovery escrito | não | **não** |

Ou seja: desligar o watchdog **eliminou o reset de hardware** — o aparelho ficou
12,5 min sem reiniciar sozinho, contra 45 s antes. Isso confirma o mecanismo.

Mas o kernel novo **continua não chegando ao userspace**:

- `sweetlog` em `sda29` ficou byte-idêntico (`flush #10`, versão de 12 nov) — o
  UFS nunca foi inicializado;
- a partição `misc` foi lida depois: **4096 bytes, todos zero** — o
  `sweet_bcb_request_recovery()` não rodou;
- `pstore` vazio.

Logo o kernel novo morre **antes** de inicializar o UFS, e sem o watchdog ele
simplesmente fica parado em vez de reiniciar (o `sweet_wdt.c` tem um
`for(;;) wfe` justamente como "espera o watchdog morder"; sem watchdog, espera
para sempre).

## Hipótese que sobra, e como testá-la sem chutar

O que roda **antes** do UFS e pode travar: `sweet_early.c` — o logo
(`sweet_early_paint_logo`) e o console inicial escrevem direto no framebuffer.
Isso é o único caminho que roda antes do UFS e que é novo no 16 nov.

**Como saber sem instrumentar:** a tela. Se o logo aparece, o `sweet_early`
rodou e o travamento é depois dele. Se a tela fica preta, o travamento é nele.
Eu não vejo a tela — o dono vê.

## Correção do que eu disse antes

Escrevi acima que "boota e escreve log" seria o resultado bom. Não aconteceu. O
resultado real é o do meio: o watchdog explicava o loop curto, e há **uma segunda
falha** antes do UFS.

## Nota operacional

O disco estava com 162 MB livres e travou a montagem do boot.img. Limpei:
a cópia descartável `s9build/xnu7195` (o original é `s9work/xnu7195-local`) e
`work/iosroot/ios16-root-v2.img` (17 GB, reconstruível por
`ios-s9/tools/mkiosroot.sh` a partir de `work/ipsw`). Ficaram 16 GB livres.
