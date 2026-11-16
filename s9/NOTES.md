# iOS no Galaxy S9 (SM-G9600, starqlte, SDM845): notas de trabalho

> **O alvo agora é iOS 14.3 (18C66), kernel xnu-7195.** O trabalho de iOS 16
> (xnu-8796) descrito daqui para baixo foi o que chegou primeiro a uma tela
> desenhada, e continua valendo como referência: os drivers, o shim e metade dos
> patches são os mesmos. O kernel que roda hoje está em `../xnu/patches-7195/`.
> O estado atual do 14.3 está no topo do `README.md` da raiz.

Referência: ios-sweet (`../ref/sweet/ios-sweet-main`, MIT): XNU + userland do iPhone X 16.7.16 no Redmi Note 10 Pro (SM7150).
Kernel Samsung/LineageOS 20 do S9: `../ref/kernel/android_kernel_samsung_sdm845-lineage-20`.
Firmware stock: `../ZTO-G9600ZHU9FVF2` (G9600ZHU9FVF2, Android 10).

## Estado (2026-10-23)

- M0: arquivos descompactados, boot.img stock desmontado, fatos de hardware conferidos no DTB stock (abaixo).
  Empacotamento testado com Image falso: header igual ao stock, DTBs intactos.
- Toolchain: gcc 16.1.0 / binutils 2.47 cross do Arch, **desempacotados em `toolchain/root`** (sem root; sha256 conferidos
  contra /var/lib/pacman/sync/extra.db). `build.sh` usa sozinho. A senha de sudo do roteiro era marcador: comandos de
  root, o dono roda com `! sudo ...`.
- M1: compilado sem warnings (Image 84 KB, header ok, sem SIMD/memset, pintura antes de qualquer MMIO).
  **Flash 1** (2026-10-22 ~22:05): `heimdall flash --BOOT out/boot.img` (cópia: `out/boot-m1.img`, sha256 6c04d7df...),
  "upload ok" após 1 retry. Resultado: **logo da Samsung parado por horas**.
  Causa provável: bug do retry do Heimdall (BridgeManager.cpp: o retry cria outro SendFilePartPacket, que lê o
  PRÓXIMO pedaço do arquivo; o aparelho aceitou como parte 0, ou seja, a imagem gravou deslocada, sem cabeçalho).
  **Confirmado por readback (2026-10-23, flash 2 = M1.1 também com retry no 0%):** partição MiB 0 = MiB 1 do
  arquivo, MiB 1 = zeros, MiB 2-10 certos. Sem "ANDROID!" o ABL mostra o logo com o quadro de status (RPMB,
  CURRENT BINARY...) e para. O shim nunca rodou nos flashes 1 e 2. TWRP: `omni_starqltechn`, boot = sda20 em
  /dev/block/platform/soc/1d84000.ufshc/by-name/boot; pstore ativo (ramoops 0x100000@0xa1300000, ecc 0).
  ABL: msm_drm.dsi_display0=ss_dsi_panel_S6E3HA8_AMB577PX01_WQHD:, lcd_id=0x910443, revision=14, debug_level LOW.
  Regra: flash com "Retrying" = flash inválido. Conferir sempre por readback no TWRP (`tools/twrp-check.sh`).

- **M1.1** (pronto, sha256 f1c5dc86...): M1 + caixa-preta na RAM (ramlog.c, zona console do ramoops 0xa1340000,
  formato persistent_ram_buffer, sig 0x43474244, runs fazem append) + watchdog armado (bark 15 s, bite 18 s) +
  fim em reboot pedindo recovery (IMEM 0x77665502; o ABL Samsung talvez só olhe o PMIC: teste grátis).
  Procedimento: TWRP -> `tools/twrp-check.sh out/boot-m1.img` (confirma o flash 1) -> `tools/flash-twrp.sh`
  (dd + readback) -> observar -> voltar ao TWRP -> `tools/twrp-check.sh` (lê a caixa-preta).
  Reset forçado Power+Vol- = s2-type 8 (DVDD hard reset): a RAM provavelmente se perde. Por isso o watchdog.

- **M1 CONCLUÍDO (2026-10-23, M1.1 gravado por TWRP+dd com readback):** tela do shim aparece, barra anda,
  reinicia e roda de novo (o cookie de recovery no IMEM é ignorado: o ABL olha o PMIC). Foto: cores certas.
  Valores lidos no hardware: shim carregado em **0x800FC000**, DTB em **0x85500000**, EL1, MIDR 0x517F803C (Kryo
  385 prata), 16K sim, PSCI 1.0 (0x00010000), watchdog desligado na entrada, MDSS 0x40000001.
  Scanout: SSPP0 (VIG0) addr 0x9D400000, src 720x2960 (meia largura, split em 2 LM), stride 5760,
  fmt 0x000237FF, unpack 0x03020001 (B,G,R,A = a8r8g8b8). CTL0 TOP 0x00020020 (INTF_1/DSI0, modo comando).
  **PP0 AUTOREFRESH = 0x80000002**: o ABL deixa o autorefresh ligado (a cada 2 TE, ~30 Hz) -> escrever na RAM
  basta, igual ao painel de vídeo do sweet. Não precisa de kick.
- pstore NÃO sobrevive a reboot neste Samsung: PON tem `qcom,store-hard-reset-reason` -> reboots viram hard
  reset (testado com `adb reboot recovery`: pstore vazio). Log de verdade = tela; log persistente = UFS (depois).
- DTB vivo (TWRP): `dtb/live-twrp.dts`. CPUs reg 0x000..0x700 (Aff1 = núcleo, 0-3 prata A55, 4-7 ouro A75),
  psci 1.0 smc, timer PPI 1/2/3/0 (vtimer INTID 19), GICD 0x17a00000, GICR 0x17a60000 stride 0x20000,
  memória 0x80000000 + 0xFE1C0000.
- **M2** (`./build.sh m2`, sha256 d26038d8...): features, PSCI_FEATURES, CPU_ON nos 7 secundários (MPIDR+MIDR),
  GIC Group 1: vtimer (espera ~30 IRQs em 300 ms) e SGI 1 (espera 10). Fica 90 s na tela e reinicia.

- **M2 CONCLUÍDO (2026-10-23):** CPU0 MPIDR 0x81000000. PSCI CPU_ON ret=0 nos 7: CPU1-3 MIDR 0x517F803C (A55),
  CPU4-7 0x516F802D (A75). EL1, 16K sim, PARange 2 (40 bits), GICsys 1, ICC_SRE 1, CNTFRQ 19,2 MHz.
  PSCI 1.0, SMCCC_VERSION ausente (FFFFFFFF). FEATURES: CPU_ON 0, OFF 0, SUSPEND 3, MIGRATE_INFO -1,
  SYSTEM_OFF 0, RESET 0, RESET2 -1, SYSTEM_SUSPEND -1. GICD_CTLR 0x10 -> 0x12, TYPER ~0x007B0418; GICR do
  CPU0 = 0x17A60000, waker 0; ICC_CTLR 0x8440. vtimer 30/30, SGI1 10/10. INTID 28 sempre pendente (igual sweet).
- **Painel com defeito (tela trincada):** abaixo de ~90 % de brilho fica quase todo preto. Shim/XNU não mexem
  no brilho (o nível do ABL aparece inteiro). Se um dia controlarmos brilho (DCS 0x51), piso de ~90 %.
  TWRP: /sys/class/backlight/panel0-backlight (max 50000); os scripts põem 50000 ao conectar. O TWRP não
  guarda config (/data criptografada, não montada). Não editar a imagem do TWRP: é a rede de segurança.
- **M3** (`./build.sh m3`, sha256 3a655c7a...): sondagem UFS só leitura (TUR, READ CAPACITY, GPT, partições
  >= 64 MiB, superbloco de userdata/cache) com MMU desligada; depois MMU 16K (mmu.c do sweet: imagem WB,
  bloco 0x9C000000 NC, resto Device; o bloco seguinte tem TIMA e fica Device) + PAR + tempo de memória.

- **M3 CONCLUÍDO (2026-10-23):** UFSHCI 2.1 (cap 0x1587031F, 32 slots) ligado pelo ABL (hce 1, hcs 0x10F, lista
  do ABL em 0x9BAB3000); TUR/READ CAPACITY/READ ok, LUN0 lastlba 0x1BC73FF (4K, ~119 GB), GPT 36 entradas.
  boot lba 0x16E0B (64 MiB), recovery 0x1AE0B, system 0x1F30B (4435), vendor 0x13460B, odm, cache 0x17940B (ext4),
  omr, userdata **0x19340B, 115519 MiB** (superbloco ilegível = metadados criptografados). MMU 16K ok: SCTLR
  0x00C00800 -> 0x00C01805, TCR 0x1409CB51C (igual sweet), PAR img/fb/gic certos, 12G = falta L2. Memória 127x
  mais rápida com cache. O RKP não bloqueia a MMU da EL1. O ABL carregou o shim em 0x8015C000 (varia com o tamanho).
- **M4 (XNU) em preparo:** árvore local `../work/xnu` = xnu-8796.141.3 + 0001-0081 do sweet (0031/0041 não aplicam
  sem o HFS preparado: os scripts de stage usam xcrun / bearssl.org). Nosso patch
  `xnu-patches/0082-s9-galaxy-s9-bringup.patch`: console de emergência no splash do S9 (start.s, sweet_early.c com
  2184 linhas = só o bloco de 32 MB mapeado, arm_vm_init com offset 0x1400000) e touch/teclas/USB do Redmi
  DESLIGADOS por padrão (o touch vota RPMh ldoc11 = 3,2 V!). `tools/mkadt.py` S9: RAM 0x100000000 + 0x7e1c0000,
  FB 0x9d400000 1440x2960, A75 >= núcleo 4; 'qcom,kryo470' fica (KernelConfigTables.cpp casa o driver nele).
  Shim m4 = loader.c do sweet com boot_args.Video do fb.c. CI: fork de dipa-studio/ios-sweet (precisa de login).
  Boot-args do 1º boot: sem ufsrw/rootrw (UFS só leitura, sem log persistente), sweettouch=0 sweetkeys=0 sweetusb=0.

- **M4: XNU RODOU NO S9 (2026-10-23)**, com o kernel do CI do sweet + ADT do S9: IOKit, autotestes, UFS LUN0
  (userdata 115519 MiB read-only), e depois loop de "unknown Volume Signature c449" (userdata ainda da LineageOS).
- Fork **syncmodz/ios-s9** (CI ok em ~6 min). Kernel 1 = 0082; kernel 2 (run 39946975709, sha a93c6d7b) = 0082 + **0083**
  (canvas do iPhone X 1125x2436 centralizado no painel 1440x2960; sem isso o sweet cai no tamanho nativo).
- Ferramentas compiladas sem root em `toolchain/bin` (apfs-fuse com CMAKE_POLICY_VERSION_MINIMUM=3.5 e
  `-include cstdint`; hfs do glaubitz, branch linux, compilado fora do caminho com espaços; lzfse).
  O fsck.hfsplus não aceita arquivo comum: rodar via `losetup`. `research/ipsw-x/d22ap.adt` vem da IPSW (IM4P LZFSE).
- **Root v1:** `../work/iosroot/ios16-root-v1.img` (mkiosroot.sh do sweet como root + sweet-root-tweaks.py; 12 GB
  usados de 16), gravado na **userdata** com `tools/flash-userdata.sh` (gzip + adb shell -T, readback sha256 421fccb4).
  Boot `out/boot-ios1.img` (sha 54cf699e): kernel 2 + boot-args do sweet com cpus=4, ufsrw=1 rootrw=1, console
  visível, touch/teclas/USB desligados. Log persistente: `tools/sweet-log-pull.sh logs/X.txt` (TWRP).

- **🎉 TELA DE BLOQUEIO DO iOS 16.7.16 NO S9 (2026-10-23, boot-ios1):** logo da Apple, depois o SpringBoard com a
  tela de bloqueio (relógio em 31/dez porque não há RTC, bateria 100 % carregando do SweetBattery), centralizada
  no painel. Boot de ~15 min com cpus=4 (A55). Console por cima piscando (sem noconsole). Sem touch/botões.
- **0084:** volume+ do S9 = GPIO6 do PM8998 (SID 0, PPID 0x0c5, regs 0xc5xx); power/volume- = PON 0x810 como no
  Redmi; teclas ligadas por padrão. Com sweetfb_noconsole=1, limpa as bordas do painel no 1º quadro da GUI.
- **0085:** touch S6SY761 (sec_ts) no SE11 0xa8c000 (GPIO31/32 qup11, voto GCC 0x5200c bits 18-21 + 25, RCG
  0x183a8, CBCR 0x183a4). Sem reset/RPMh: S2DOS05 LDO2/LDO3 são `regulator_boot_on`. Sequência: CLEAR_EVENT_STACK
  0x62, SET_TOUCHFUNCTION 0x30 [0x61 0x00], SENSE_ON 0x10; poll a cada 10 ms com 0x60 (8 B) + 0x61 (restantes).
  Coordenadas 0..4095 -> painel 1440x2960 -> SweetHID (offset -157/-262, toque na borda gruda na beira).
  O touch fica desligado por padrão (0082): usar `sweettouch=1` (+ `sweettouchdelay=20`).
- **boot-ios2 (cpus=8):** boot em ~7 min, botões OK, console sem piscar. Touch: "se7 fw=0x301 proto=3 ... need
  ABL-loaded I2C firmware and 32-bit FIFOs": o firmware do SE11 tem FIFO desligado (GENI_IF_DISABLE_RO bit 0).
- **0086/0087:** canvas do iPhone X escalado até a altura, depois esticado até 1440x2960 por padrão
  (`sweetfb_aspect=1` mantém a proporção, `sweetfb_noscale=1` desliga). boot-ios3 = kernel 5 (run 39959555728).
- **Touch, testado no shim M5 (2026-10-23)**, ciclos de 1 min em vez de boots de 7 min:
  - **Energia:** o S6SY761 fica SEM energia depois do ABL. LDO2 (dvdd 1,8 V) e LDO3 (avdd 3,0 V) do S2DOS05 têm
    `regulator-boot-on`, mas quem liga é o regulator core do Linux. S2DOS05 em 0x60 num i2c-gpio (SDA GPIO127,
    SCL GPIO128). Lido: id=00 en=0x19 ldo2=0x8c ldo3=0x8c (tensões já certas, vsel 12). Escrevendo EN|2, 1 ms,
    EN|6 → en=0x1f, as linhas do touch sobem (GENI_IOS 0 → 3) e o IRQ (GPIO120) desce.
  - **GSI/GPI não dá sem votos:** a ordem certa é EV ALLOC → contexto → doorbell (eu tinha invertido; RP virava
    lixo 0x75170000). Corrigida, os RPs batem com as bases, mas o 1º DMA trava a CPU. A tbu2 (SID 0x6d6,
    GDSC 0x17d038) já está ligada (0x80000001); ler a SMMU (0x15000000) trava o barramento: faltam os votos de
    barramento do Linux (msm-bus apps_smmu/QUP). Abandonado.
  - **Bit-bang nos pinos do touch (GPIO31/32 como GPIO, dreno aberto por OE), como o i2c-gpio:** funcionou de
    primeira, boot status + id + 5 dedos ao mesmo tempo (M5.4).
- **0088:** o driver do touch no XNU virou isso: liga LDO2/LDO3 pelo S2DOS05, I²C bit-bang a ~150 kHz em
  GPIO31/32, lê eventos só com o IRQ (GPIO120) em 0, poll de 5 ms. Corrige também o 0085, que só mandava o
  toque inicial ao SweetHID (sects_event escrevia em st.x, então touch_report nunca via movimento).

- **🎉 boot-ios4 = TOUCH FUNCIONANDO NO iOS (2026-10-23):** kernel 6 (run 39991709646, 0082-0088) + boot-args abaixo,
  gravado com `tools/flash-twrp.sh` (readback ok, sha f382e672...). Vídeo do dono (37 s): desbloqueia arrastando,
  tela inicial, Ajustes > Acessibilidade > AssistiveTouch, Geral > Sobre (iOS 16.7.16, iPhone X, 17 GB / 3,96 GB
  livres), Siri pelo botão lateral. Volume+/- e power OK.
  Boot-args: `debug=0xc serial=3 cpus=8 keepsyms=1 -enable_kprintf_spam -noprogress rd=ufsroot0
  unentitled_ios_sim_launch=1 ignition_level=1 wdt_test=no_user_checkin ufsrw=1 rootrw=1 ufswait=1
  launchd_unsecure_cache=1 wdt=-1 sweetfb_noconsole=1 sweetfb_hz=30 sweettouch=1 sweettouchdelay=20 sweetusb=0
  sweetsample=MSUEarlyBootTask,launchd,SpringBoard,backboardd`. Receita: payload com `tools/mkpayload.py`, depois
  `PAYLOAD=out/payload-ios4.bin ./build.sh m4` (o kernel do CI vai em `../work/s9-kernel-6`).
  **Problemas vistos:** interface lenta (render só na CPU, sem GPU) e apagar a tela pelo power demora muito.

- **M6 (fluidez, 2026-10-23)** `./build.sh m6` (sha 98539530...): nada no XNU/sweet ajusta o clock das CPUs
  (ficam no que o ABL deixou). M6 lê as tabelas do OSM v2 (L3 0x17d41000, prata 0x17d43000, ouro 0x17d45800;
  FREQ_REG 0x110+32*i, PSTATE 0x700, DESIRED 0x920+4*core), mede MHz (cadeia de adds x contador) na CPU0 e
  na CPU4, cópia 4 MB e escrita/leitura do framebuffer NC; depois sobe (L3 <= 1478, prata <= 1689 sem voto MX,
  ouro no máximo com 4 núcleos) e mede de novo. start.S: `adr` -> `adrp+add` (buffers de 8 MB na .bss).
  Gravado com readback ok; aguardando a foto.

- **M6 resultado:** ABL deixa o ouro em 825 MHz (linha 0) e o prata em 1612; com OSM: ouro 2706 MHz medido.
  Framebuffer write-combined: 200 MB/s escrita, 22 MB/s leitura (85 ms/quadro). **0089** (boot-ios5): clocks no
  SweetCPU (ouro 2649, prata 1689, L3 1478; `sweetcpu=0`, `sweetcpu_mhz=N`) + painel com cache e `dc cvac` por
  quadro (`sweetfb_wc=1` volta). Dono: "mais fluido do que eu esperava".
- **🎉 USB DO ZERO (shim M7.11, 2026-10-23):** o PC enumerou "Galaxy S9 iOS (USB Ethernet)". Receita (cada passo
  provado no M7.x, ver shim/m7.c):
  1. RPMh DRV2 (RSC 0x179e0000, TCS +0xd00, layout igual ao sweet): ldoa5 800 mV, ldoa12 1800, ldoa24 3075 (VRM:
     tensão, modo 7, enable). BCMs (cmd-db slave 5): SH3 SN11 SN0 SN6 CN0 SN12 — AMC de BCM pode levar >100 ms
     (timeout de 2 s; antes falhavam em cascata). No kernel o voto é 0x3fff (nunca rebaixa); MC0/SH0 intocados.
  2. **SMMU atrás do hypervisor**: ler sCR0 TRAVA; só ID0/ID1/SMR/S2CR/CBAR são emulados. Receita do U-Boot
     qcom-hyp-smmu.c: contexto livre como S1-translate/S2-bypass com tradução desligada + SMR 0x740 → S2CR TRANSLATE
     (BYPASS vira FAULT). Sem isso o 1º DMA (TRB) trava a CPU — era também o problema do GPI do touch.
  3. GCC: GDSC 0xf004; BCR 0xf000 com os clocks DESLIGADOS (com clocks ligados a 1ª leitura do DWC3 trava);
     RCG 0xf018 cfg 0x108 (133 MHz); CBCR master 0xf00c, sleep 0xf010, utmi 0xf014, cfg-noc 0x502c, aggre 0x8201c,
     ahb2phy 0x6a004 (offsets do sweet/SM7150 são outros).
  4. QUSB2: BCR 0x12000, ahb2phy 0x88ee010=0x11, init-seq do DT, PLL 0x1a0 bit0.
  5. **MAX77705**: o Linux da Samsung ABRE a chave D+/D− em todo shutdown (max77705_muic_shutdown → com_to_open),
     então depois do TWRP os fios ficam desligados. I²C bit-bang GPIO33 (SDA)/GPIO34 (SCL), slave 0x25: escrever
     [0x21, 0x06 (CONTROL1_WRITE), 0x09 (COM_USB)] e [0x41, 0x00].
  6. DWC3: QSCRATCH GENERAL_CFG PIPE_UTMI_CLK_SEL|PIPE3_PHYSTATUS_SW (sem PHY SS), GCTL device + DSBLCLKGTNG,
     GUSB3PIPECTL SUSPHY, GUCTL/GFLADJ p/ 19,2 MHz; VBUS por override no QSCRATCH.
- **0090** (CI 40054981141, kernel `../work/s9-kernel-8`): a receita acima no sweet_usb.c (`sweetusb=2`), replug
  (reset depois de configurado → reinicia o controlador; link cai quando o barramento suspende = cabo fora),
  política "AMFI" do dyld (__mac_syscall 90 → 0xff: DYLD_INSERT_LIBRARIES liberado) e `sysctl kern.sweet_next_boot`
  (1 = recovery: PON SOFT_RB_SPARE 0x88F bits 7:1 via canal SPMI 0xc600000+0x10000*APID + cookie IMEM).
- **Root v2** (`tools/prep-ios6.sh`, sudo): v1 + **bootstrap do Taurine** (iOS 14) + jb/debs
  (Sileo 2.3, libhooker 1.6.9 + newabi, safemode, PreferenceLoader, NewTerm 3, OpenSSH 9.7) instalados no 1º boot pelo job
  com.ios-s9.jbsetup (log /var/ios-s9/jbsetup.log), s9net (s9/ios-tools no fork, CI s9-ios-tools: publica
  IPv4/DNS do ue0 no configd), sshd inetd (22/2222, chave ~/.ssh/ios-s9 do PC p/ root), SpringBoard com
  DYLD_INSERT_LIBRARIES=/usr/lib/TweakInject.dylib (injetor do libhooker; jailbreakd do Taurine precisa estar de pé).
  Senha mobile/root: alpine.
  PC: perfil NetworkManager `s9-usb` (MAC 02:00:5e:77:ee:01, 192.168.7.1) + dispatcher NAT 90-ios-s9-share.
- **Controle remoto:** `tools/s9ssh.sh [cmd]`, `tools/s9-twrp.sh` (iOS → TWRP sem botões), `tools/s9-log.py ARQ`
  (log do kernel ao vivo, UDP 5140). boot-ios6 = kernel 8 + boot-args do ios4 + `sweetusb=2 cs_enforcement_disable=1`.

## Auditoria e zero-copy (2026-10-24, noite 2)

**Shim M9 (vídeo do dono, 16:38):** A ok (o SDE lê 0xb0000000: sem bloqueio de SMMU para o display), B ok
(QSEED3 bilinear 414x1792 -> 720x2960 por metade; a "metade esquerda repetida" foi bug do teste: o ABL aponta o
VIG1 com o **mesmo endereço** e `SRC_XY x = 0x2D0`, não pelo endereço), C: 31 -> 60/61 quadros enviados/s com
autorefresh 1 (o contador de TE em PP_INT_COUNT_VAL não anda nesse modo; PP_LINE_COUNT anda). Layout lido do ABL:
CTL0 top 0x00020020, LM0 layer 0x01000005, LM1 0x01000028, VIG0/VIG1 src 0x0B9002D0 out 0x0B9002D0 addr
0x9D400000 ystride 0x1680 fmt 0x000237FF op 0x80000000, QSEED3 versão 0x1003.

**Achados confirmados** (evidência = código + logs + desmontagem do dyld cache, `work/dsc`):
1. Cópia+escala de cada quadro na CPU (SweetFramebuffer.cpp presentScaled): 8,1-8,6 ms/quadro, ~40 MB de memória
   por quadro (lê 5,9 + escreve 17 + dc cvac 17).
2. Rasgo: a cópia escreve no buffer que o autorefresh do MDSS está lendo (buffer único, sem sincronia com o TE).
3. `SwapWait` (sel 6) era no-op. Desmontagem do QuartzCore: `IOMFBDisplay::finish_update` grava em cada buffer o
   id do swap que o mostra (`SwapBegin(fb, &buf.swap_id)`); `can_update_` chama `SwapWait(fb, outro.swap_id)` antes
   de redesenhar num buffer. Semântica certa: voltar quando aquele swap estiver **na tela**.
4. Vsync do kernel nunca usado: seletor 72 ausente nos 5 logs. `CADeviceUseVBL()` só liga para ChipID 0x7000 ou a
   lista `vbl_chip_ids` (o A11 0x8015 não está); `CA_FORCE_TIMER=1` (env) força use_vbl=1 =
   `IOMobileFramebufferEnableVSyncNotifications` (sel 72 tipo 5, args count/hosttime/0 = formato do nosso kernel).
   Sem isso o CA usa `CADisplayTimer` próprio (IOMFBServer::timer_callback), sem fase do painel. O timer do kernel
   ainda deriva (setTimeoutUS relativo).
5. Linha "present avg ... fps" dividia por 10.
6. sweet_usb: thread de eventos consulta a cada 1 ms (1000/s, prioridade de kernel).
7. sweet_usb: 1 TRB de RX e 1 de TX: ~1 quadro por consulta, ~1,5 MB/s e +1 ms por pacote (lento p/ Safari).
8. `tools/s9-twrp.sh` não chega ao TWRP (ABL da Samsung lê o BCB na partição misc).
Menores: SweetHID com canvas fixo 1125x2436 (só borda); comentário velho no 0091 (pmap.c: 0xa1800000); mkadt
`clock-frequency` 2803/1766 vs 2649/1689 reais; GetBlock 79/SupportedFrameInfo 74/gamma 27/área 29 devolvem zeros.
Suspeitas: thread que já roda num A55 não migra p/ A75 livre; carveouts no physmap (prefetch especulativo, nunca
visto); trim parece seguro p/ Safari (falta prova). Conferidos sem bug: 0092 escalonador, touch, AKS, clocks 0089
(2649 = linha oficial de 4 núcleos), s9net, trim, respring, S9Wallpaper.

**Mapa de seletores do IOMobileFramebuffer** (desmontagem, `kern_*` → IOConnectCall): 3 GetLayerDefaultSurface,
4 SwapBegin, 5 SwapEnd (struct = fb+0x18, 0x46c; dirty region em +0x250 x,y,w,h, flag +0x260), 6 SwapWait /
SwapWaitWithTimeout [id, flags, 0], 7 GetID, 8 GetDisplaySize, 12 RequestPowerChange, 15 SetDebugFlags,
17/27 Set/GetGammaTable (3084 = 257x3x4), 18 IsMainDisplay, 20 SwapSignal, 22 SetDisplayDevice, 28 GetDotPitch,
29 GetDisplayArea, 33 EnableDisableVideoPowerSavings, 50 SetBrightnessCorrection, 52 SwapCancel, 55 SetMatrix,
57 GetColorRemapMode, 68 SetParameter, 72 Enable/DisableNotifications, 74 SupportedFrameInfo, 77
GetCurrentAbsoluteTime, 78/79 Set/GetBlock, 81 SwapCancelAll, 83 CopyLayerDisplayedSurface, 84 SetContrast.

**0096** (CI xnu 40133083582, kernel `../work/s9-kernel-14`, `out/boot-ios12.img` = ADT s9-xr2 + boot-args do ios11):
- **Zero-copy:** as superfícies de tela do backboardd (canvas, BGRA, sem planos; ids 2/3) vão para 3 "vagas" no
  fim da região do splash (0x9e500000 + k·0x5b0000, até 0x9f610000; TIMA começa em 0x9f800000), mapeadas com
  cache. Cada swap: dc cvac só nas linhas que o CA pode ter escrito desde a última vez que aquela vaga apareceu
  (união dos dirty regions), VIG0/VIG1 SRC0_ADDR = vaga, `SRC_XY` x = 0 / 414, QSEED3 bilinear, flush do CTL0
  (0x20003): o SDE aplica no início do próximo quadro (sem rasgo). `SwapWait` espera o flush ser aplicado (bits
  limpos), poll de 0,5 ms, máx. 50 ms. Tela apagada: painel preto e pipes de volta ao layout do ABL.
- `sysctl kern.sweetfb_zerocopy=0/1` alterna ao vivo (boot-arg `sweetfb_zerocopy=0`); a cópia na CPU agora copia
  só as linhas sujas e o SwapWait espera a cópia. Contadores: `kern.sweetfb_{swaps,flips,copies,present_ns,
  waits,wait_ns,wait_timeouts}`.
- `sysctl kern.sweet_swipe=N`: N arrastos sintéticos sobe/desce no meio da tela (dedo virtual, slot 9 do HID) para
  medir sem ninguém no aparelho: `tools/s9-bench.sh 0|1 [N]`.
- Thread de eventos do USB presa na CPU 2 (A55). Bateria: modo reserva anuncia "carregando" (raio na barra; nível
  não medido: o fuel gauge do S9 é o MAX77705 em I2C).
- Linha de fps corrigida (3000e9/span), agora com flips/cópias e tempo médio de SwapWait.

**0097** (CI 40136178017, `out/boot-ios13.img`): o `SwapWait` responde na hora às consultas. Desmontagem do
QuartzCore: `swap_wait_timeout` (modo timer, o nosso) chama `SwapWait` e, se não for 0, dorme 100 us e repete até
um orçamento de **1 ms** (constante 0.001 em 0x18855cb20); estourou, o CA segue por outro caminho (3º buffer: o
construtor `AppleDisplay` põe o pool em 3 com page flipping, `+0x360`). Flags com bit 31 = consulta. Também:
`kern.sweetfb_fullclean` (padrão 1 = dc cvac da vaga inteira, ~0,33 ms; 0 = só linhas sujas), contador de underrun
(INTR_STATUS 0x1014 bit 26 = INTF_1, limpo em 0x1018: **0 underruns** em todas as medições),
`kern.sweet_gesture="x0 y0 x1 y1 ms"` (dedo virtual livre, slot 8; ms 0 = toque; desbloqueio = `720 2950 720 1500
150`), bateria reserva com 500 mA e `AdapterDetails` (5 W, família 0xe000400a). **O raio ainda não apareceu** (0096;
0097/0098 não confirmados pelo dono).

**0098** (CI 40138341538, `out/boot-ios14.img` = **o que está gravado agora**, cmdline do ios11 + `ufslogsecs=60`):
`kern.sweet_parallelism` (largura do dispatch_apply para QoS default+; 4 = só A75), `kern.sweetfast` (máscara
ao vivo), `kern.sweet_mem_vote` (voto de banda DDR MC0 + LLCC SH0 via RPMh; **0 = intocado, NÃO usar sem o ok do
dono**: ele não quer mais overclock), threads de present e vsync presas na CPU 3 (A55).

**Medido (dedo virtual nos Ajustes, `tools/s9-bench.sh`, 4-6 ciclos):**
- Apresentação: 8.100-8.600 us/quadro (cópia, 0095) -> **14-350 us** (zero-copy; 0,33 ms com limpeza completa).
- Rolagem dos Ajustes: **mediana 25 fps, máx. 28-31** em todos os modos: SwapWait bloqueante (0096), consulta
  (0097), nunca espera; `sweet_parallelism=4` (com e sem reiniciar o backboardd); `CADisableColorMatching=YES`
  (preferência do backboardd, reiniciado). Uso livre (dono): janelas de 60,2 e 42 fps em alguns trechos, 24-26 no
  resto. => **o teto é o CoreAnimation desenhando na CPU (~40 ms/quadro)**, não a apresentação.
- Parado, algo animou a exatos 24,0 fps por > 1 min (região pequena, present 15 us): achar e cortar.
- Dono relatou falhas "tipo TV de tubo" por frações de segundo no 0096 (limpeza parcial); 0097+ usa limpeza
  completa (não confirmado pelo dono).
- Spindump antigo (ios8, outra tela) nas threads do `dispatch_apply`: `memset_pattern16` 17 %, `tex_multiply_color`
  14 %, `image_sampler` sem filtro 11 % + com filtro 7 %, `vAlpha_Blend` 10 %, `circle_sampler` (cantos contínuos,
  com powf) alto. O `scan_convert` divide cada polígono em faixas de 4 linhas (dispatch_apply por polígono).
- **Falta**: spindump da rolagem dos Ajustes (o dono interrompeu duas vezes); medir banda de memória.

Ferramentas novas: `tools/s9-ui.sh` (unlock/tap/swipe/open via dedo virtual), `tools/s9-bench.sh`,
`tools/s9-tune.sh` (prep/bench/capref/membw), `tools/s9-profile.sh` (spindump + simbolização do backboardd).

## Aceleração gráfica da Adreno 630 — estudo + shim M10 (2026-10-25, tarde, sem aparelho)

Objetivo da sessão (prompt `PROMPT-sessao-2026-10-25-gpu.md`): estudar Bergamota + Chlorophyll e começar a ligar a
GPU. Estudo completo em **`ESTUDO-2026-10-25-gpu.md`**. Referências baixadas em `ref/` (commits anotados no estudo):
Bergamota (iOS 18 no Poco F1, mesmo SDM845/A630; sem licença, só fatos), Chlorophyll (driver Metal, MIT),
**HackiOS** (`oosexgang/hackios-xnu-kernel` APSL + `-userland` MIT: iOS **16.5** rodando a UI inteira na Adreno 642L
por Mesa/freedreno — o caso mais perto do nosso).

**Dois achados que reduzem muito o risco:**
1. **Zap da Samsung extraído** do `BL/NON-HLOS.bin` oficial (`image/a630_zap.mdt`+`.b00/.b01/.b02`, ELF32, 1 segmento
   de código relocável) → `research/gpu-fw/` (fora do git). O zap assinado por aparelho era o maior desconhecido dos
   dois projetos (o Bergamota não tinha o do beryllium); temos o do próprio S9.
2. **No iOS 16 o CoreAnimation usa o OpenGL ES nativamente**: na nossa desmontagem (QuartzCore 20H392),
   `EAGLServer::renderer()` sem Metal cai em `[EAGLContext initWithAPI:3]` + `CA::OGL::GLESContext`, e
   `enable_ogl` (de `CA_ENABLE_OGL`) é **ligado por padrão**. Basta o backboardd ter um `EAGLContext` nosso (Mesa por
   trás) — sem o gancho de vtable que o Bergamota precisou no iOS 18. O HackiOS prova a UI inteira na GPU no iOS 16.5.

**Hardware do S9 conferido no DT ao vivo:** GPU 0x5000000 (chipid 0x06030001 = A630 v2), GMU 0x506a000, gpucc
0x5090000, GCC 0x100000, PDC 0xb200000, SMMU da GPU 0x5040000 (nossa, não emulada pelo hyp), zap PAS 13 região
0x8c415000. Registradores e sequência do kernel Android do próprio S9 (LineageOS 20 `drivers/gpu/msm`).

**Plano por etapas** (crítico = ligar a GPU; GLES/Metal é software que os dois projetos já rodam no mesmo chip):
A GMU (energia+relógios+firmware → `0xBABEFACE`) · B SMMU+GX+HFI · C zap+CP (NOP) — **tudo no shim** (hardware
arriscado primeiro, 10 s, watchdog); depois D driver no XNU (ioctls msm como o HackiOS), E OpenGL ES (Mesa freedreno
+ EAGL nosso no backboardd), F medir e Metal. Tabela completa no estudo, seção 5.

**Feito: etapa A = shim M10** (`shim/m10.c` ~460 linhas, `build.sh m10`; compila sem avisos, 0 relocações absolutas):
- etapa 1 leitura de GCC/gpucc; 2 liga o CX GDSC + relógios do GMU a 200 MHz (RCG cfg 0x602) + escreve/lê a memória
  do GMU com o CM3 em reset; 3 micro-código RSC/PDC + firmware do GMU no ITCM + solta o CM3 → espera `0xBABEFACE`.
- Firmware: `a630_gmu.bin` (32768 B, sha `da8d9b1b…`) e `a630_sqe.fw` (34188 B, sha `1c21b527…`) do linux-firmware em
  `research/gpu-fw/`; `tools/gpu-fw-embed.py` gera `shim/fw_a630_gmu.h` (git-ignored). **Nada de firmware no git.**
- Imagem pronta: **`out/boot-m10.img`** (sha `f66a548e…`, firmware embutido). Não gravada: o aparelho não estava no
  USB nesta sessão (nem adb nem lsusb o viam).

**🎉 M10 GRAVADO E PASSOU DE PRIMEIRA (2026-10-25, tarde, foto do dono):** todas as etapas verdes.
- Etapa 1: gpucc já acessível (ABL deixou o AHB da GPU ligado); CX/GX GDSCR 0x00222001 (SW_COLLAPSE setado).
- Etapa 2: cfg_ahb on; **CX GDSC ligou (GDSCR 0xF8222000, PWR_ON)**; GMU RCG cfg 0x602 (200 MHz); cxo/gpu_ahb/ddrss/snoc
  cbcr 0x1, cx_gmu/memnoc cbcr 0x4221 (rodando, bit31 limpo); RPMH_POWER_STATE 0xF; **DTCM escreve/lê** 600D0001/0A110002.
- Etapa 3: firmware 0x2000 palavras, **0 erradas**; **`fw_init_result = 0xBABEFACE` → "O FIRMWARE DO GMU RODA"**.
Mesmo marco do Bergamota 0117g3 / HackiOS passo 15, aqui de primeira. Para voltar ao iOS, `tools/flash-twrp.sh out/boot-ios17.img` (0101).

**Etapa B = shim M11** (`shim/m11.c`, `build.sh m11`, `out/boot-m11.img` sha `d7163d4c…`): refaz a sequência do M10 e
adiciona a SMMU da GPU (0x5040000) + HFI. Ordem: 1 energia; 2 **sonda a SMMU só-leitura** (IDR0/1, SCR0, SMR/S2CR/CB0
como o hyp deixou — o único desconhecido do S9 é se o RKP da Samsung deixa programar a SMMU da GPU); 3 **programa a
SMMU** (tabelas 3-níveis 4K em buffers estáticos, PA=VA com MMU off; mapeia GMU-VA 0x60000000→HFI 16K e
0x60100000→dump 16K; CB0, SID 4/5→CB0; lê cada escrita de volta); 4 firmware + boot **com `HFI_QTBL_ADDR=0x60000000`**
(tem de ser antes de soltar o CM3) → 0xBABEFACE; 5 `HFI_CTRL_INIT` + manda `H2F_MSG_INIT` e `H2F_MSG_FW_VER` (polling
do GMU2HOST_INTR, sem IRQ) → versão do firmware. GX/CP ficam para a etapa C (M12), isoladas porque ligar a GX é o
passo que pode reiniciar o SoC. SMMU seguindo arm-smmu.c (smmu-v2); HFI seguindo kgsl_hfi.c; send/receive por polling
como o HackiOS. **GRAVADO 2026-10-25 tarde; aguardando a foto** (perguntas: o hyp deixa a SMMU? o GMU responde HFI?).
Depois: etapa C (zap da Samsung já extraído + CP).

## Primeiro boot do 0101 (2026-10-25, manhã, dono acordado)

`out/boot-ios17.img` gravado com `tools/flash-twrp.sh` (leitura de volta ok, sha `ab8c3482...`). **SSH em 37 s**,
SpringBoard/backboardd/mediaserverd de pé. Medido:
- `kern.sweet_battery` (MAX77705 de verdade): `soc=97.4% vcell=4263mV avg=-215mA now=-103mA rep=2959/3036mAh
  design=2845mAh cycles=1.17 chgin=1 dtls=0x1 bat=3 cnfg00=0x15 cnfg09=0x13(in 500mA en 0) cc=500mA`. O fuel gauge
  responde (status 0x0080).
- **Achado:** vindo do TWRP, o carregador estava LIGADO com o **watchdog ligado** (`cnfg00=0x15` = carga + buck +
  WDTEN; o driver Linux do TWRP o liga e o renova). No iOS ninguém renova: ~1 min depois `dtls=0xb` (watchdog
  expirado), `now=2mA`, a carga para. Com tela/CPU ligados a bateria cai mesmo no cabo (limite de entrada 500 mA).
  Correção: `kern.sweet_charge=1` (0100 renova o watchdog a cada leitura). Dono escolheu ligar depois da medição.
  Para um próximo kernel: decidir se, achando o carregador já carregando com WDTEN, o kernel mantém a carga
  (renova) sem precisar da sysctl.
- `kern.sweet_cpu_freq`: `l3 1478 MHz (row 13), silver 1689 MHz (row 16), gold 2649 MHz (row 22)` em repouso.
- **Raio de carregando aparece** (dono confirmou: "reconhecendo o fakecharge, esteticamente bonito"). Desempenho
  percebido igual (esperado: o 0099 só tirou desperdícios pequenos).
- **Medição** `tools/s9-measure.sh logs/measure-20261025-ios17` (interrompida na etapa 6/9: o dono foi ao TWRP;
  `report.md` na pasta). Rolagem dos Ajustes, fps mediana/máx e IPI/s: padrão 23/28 e 30.016; **`kern.sweet_parallelism=2`
  31/53 e 12.151**; =1 23/25; =4 22/28; `kern.sweet_idle_flush=1` 24/29 (nada). backboardd ~228 % de CPU, 35.946
  trocas de contexto/s, 52.173 syscalls/s, A75 só ~152 % de 400: o gargalo é acordar ajudantes do `dispatch_apply`
  (IPIs), não CPU. Memória: escrita 19,9 / cópia 13,4 / leitura 7,4 GB/s com 1 thread (até 18,6 de cópia com 4),
  latência 250 ns. Gold fixo em 2649 MHz (sem LMh). Parado: ~2 % de CPU. Faltaram spindump, `CA_PRINT_PERF`,
  `CA_FORCE_TIMER=1`, `CA_PRINT_ANIMATIONS`. Relatório: `s9-measure-report.py` agrupa threads sem nome como "-" e
  conta as idle do kernel: corrigir antes da próxima ida.
- **Ganho rápido para o próximo kernel:** `sweet_parallelism` padrão 2 (hoje 0; só sysctl, sem boot-arg).
- Estado deixado: tweak S9CAEnv e ferramentas instalados (inertes sem `env.txt`, que não foi escrito); o celular
  voltou ao TWRP com `boot-ios17` na partição boot.
- **Próxima sessão: aceleração gráfica** com base no Bergamota (iOS 18 no Poco F1, mesmo SDM845/Adreno 630) e no
  Chlorophyll (Metal sobre Mesa freedreno): prompt em `PROMPT-sessao-2026-10-25-gpu.md`.

## Handoff (2026-10-25): sessão de estudo SEM aparelho → próxima sessão = uma ida ao TWRP

**Gravado no aparelho (inalterado nesta sessão):** `out/boot-ios14.img` (kernel 0098). O celular ficou desligado
no TWRP. Nada foi enviado ao aparelho (sem adb/SSH/flash).

**Pronto para gravar:** `out/boot-ios17.img` = kernel **0101** (`../work/s9-kernel-19`, CI xnu 40173879069,
sha256 `ab8c34825e34b9d023302e1cd1df8e600949db0c07cda5ac97b476f70c6431c6`), mesmos boot-args do ios14 (receita: `mkpayload.py KERNEL out/s9-xr2.adt OUT --cmdline
"<cmdline do ios14>"` + `PAYLOAD=... ./build.sh m4`; conferido: reproduz o payload-ios14 byte a byte).
Degraus de volta: `boot-ios16.img` = 0100 (sha `c25c52cc...`), `boot-ios15.img` = 0099 (sha `210b081c...`),
`boot-ios14.img` = 0098 (o gravado hoje).

**Estudo completo:** `ESTUDO-2026-10-24.md` (renderizador do CA, bateria, áudio, bugs, plano). Resumo:
- **0099** (CI 40154808570): bateria real do MAX77705 (fuel gauge 0x36 + carregador 0x69, barramento bit-bang
  compartilhado `bsd/dev/sweet_max77705.c`; `kern.sweet_battery` = leitura; carga só com `kern.sweet_charge=1`,
  checada, desliga em 4,36 V), `cpu_idle` sem esvaziar o L1 antes do WFI (`kern.sweet_idle_flush`), lock/unlock de
  IOSurface por tabela de id, estatística de retângulo sujo na linha de fps, `aks_lock_device` aceito, teclas na CPU 0.
- **0100** (CI 40172653789): watchdog do carregador "chutado" enquanto nós carregamos, `AdapterDetails` do cabo
  medido, pedido da sysctl atômico, threads do port com nome (`sweet.usb`, `sweet.touch`, `sweet.fb.present`...).
- **0101** (CI 40173879069): `kern.sweet_cpu_freq` = clock atual de L3/silver/gold lido do `PSTATE_STATUS` do OSM
  (depois dos limites do LMh), ao lado da linha pedida: mostra se o gold é estrangulado durante a rolagem.
- Achado: o Linux do TWRP **desliga o carregador** ao desligar (`CNFG_00=0x04`, `CNFG_09=0x0F`); o 0099 registra no
  boot se continua assim.
- 60 fps: a conta do trabalho de pixels dá 5-15 ms/quadro contra ~40 ms medidos: falta medir onde está o resto.
  `CA_FORCE_TIMER=1` **liga** o VBL do CA (desmontagem); hoje ele usa relógio próprio.
- Áudio: tudo passa pelo ADSP (firmware `adsp.mdt` na partição modem); caminho curto = plug-in AudioServerPlugIn
  (o HAL do iOS carrega `/System/Library/Audio/Plug-Ins/HAL/*.driver`) mandando o som ao PC.

**Ferramentas novas** (fork, CI s9-ios-tools; cópias em `jb/measure/` com SHA256SUMS): `s9top` (CPU por núcleo e por
thread, IRQ/IPI/timer por núcleo, aceita `kernel_task`), `s9membw` (banda e latência de memória), `s9batt` (bateria
do kernel ao powerd, `-w` observa notificações), tweak `S9CAEnv` (backboardd; põe `CA_*` de
`/var/mobile/Library/S9CAEnv/env.txt` no ambiente; sem o arquivo não faz nada; sem hooks).
`tools/s9-measure.sh`: a ida inteira de medição, sem ninguém no aparelho (~10 min); no fim escreve `report.md`
(`tools/s9-measure-report.py`). Ensaiado de ponta a ponta contra um "celular de mentira":
`tools/s9-dryrun.sh SANDBOX SCRIPT [ARGS]` (s9ssh.sh falso, caminhos do iOS remapeados, sysctl/ps/killall/spindump
falsos, stand-ins das ferramentas, esperas 10x menores): todas as etapas rodam, os arquivos instalados batem
byte a byte com `jb/measure`, o `env.txt` é removido no fim; o áudio grava um tom de 440 Hz sem perdas.
Áudio, caminho A: `S9Audio.driver` (AudioServerPlugIn, saída USB 48 kHz que manda o som ao PC por UDP 5141) +
`s9play` (tom + rota atual) em `jb/audio/` (CI 40173419881); `tools/s9-audio.sh install|test|remove`,
`tools/s9-audio-recv.py` (grava/toca no PC). Testado só no PC (stubs): falta ver se o VirtualAudio o escolhe.

**Próxima sessão (ordem):** `tools/flash-twrp.sh out/boot-ios17.img` → conferir `SWEET BATTERY:` no log ao vivo →
`tools/s9-measure.sh` → decidir pela tabela da seção 5 do ESTUDO → (opcional) `tools/s9-audio.sh install` + `test`.

## Handoff (2026-10-24, noite): o que está gravado e funcionando

**Boot atual:** `out/boot-ios11.img` = kernel 0095 (`../work/s9-kernel-13`, CI xnu run 40116531969) + ADT
`out/s9-xr2.adt` (`mkadt.py --scale2`) + cmdline de uso com `sweetfb_hz=60` (resto igual ao boot-ios9). Fica na
partição boot: do TWRP, `adb reboot` volta ao iOS. **O kernel do TWRP não monta HFS+**: mudança no root só por SSH,
com o iOS ligado. Release público `s9-2026.10.24` (s9net, S9Wallpaper, wallpaper; sem kernel: o build liga com
libs do KDK da Apple).

Feito nesta sessão (com evidência):
1. **Barra de status vermelha "rdar:45025538" corrigida.** UIKit (`+[_UIStatusBarVisualProvider_Split
   visualProviderSubclassForScreen:]`) escolhe `Split<largura>` comparando a largura nativa em px **e** o notch
   (`_exclusionArea` × nativeScale/referenceScale, tolerância ~1e-4). O notch vem do SpringBoard
   (`sb_thisDeviceDisplayShape` ← `SBHScreenTypeForCurrentDevice`: pontos, escala, `artwork-device-subtype`). Não
   existe Split750. `mkadt.py --scale2` agora = tela de iPhone XR: canvas 828x1792 @2x + `/product`
   `artwork-device-subtype` 1792 e `artwork-scale-factor` 2. No aparelho: notch {93.5,0,227,32},
   `_UIStatusBarVisualProvider_Split828`, UIScreen 414x896 @2x. 1,48 Mpx (−46% vs @3x).
2. **Painel a 60 Hz (0095).** O ABL deixa PP_AUTOREFRESH = 0x80000002 (1 quadro a cada 2 TE). Com `sweetfb_hz>=60`,
   `s9_panel_autorefresh(1)` grava 0x80000001 nos pingpongs habilitados (0xae71000 + 0x800·i). O dono viu mais de
   30 fps ("oscila"). Medição em números ainda não feita (linha `present avg ... fps` do kernel). Shim M9 passo C
   mede quadros enviados/s e TE/s (vídeo do M9 A/B/C não recebido: perguntar).
3. **Respring automático por boot** (`com.ios-s9.respring` → `/usr/libexec/ios-s9-respring`, `jb-launchd.py`). O
   loader do ElleKit (`com.ios-s9.ellekit`, LaunchOnlyOnce) termina depois do 1º SpringBoard ("run `launchctl
   reboot userspace` to load your tweaks" no ellekit.log), então esse SpringBoard nunca tinha tweaks. O job espera
   o SpringBoard + o fim do loader, mais 20 s, e mata o SpringBoard uma vez. Visto no boot das 11:05 ("respring after 2s").
4. **Wallpaper.** O PosterBoard não tem provedores (PRSService `fetchExtensionIdentifiers` = vazio, descritores dão
   erro -2218 do PBFPosterExtensionDataStore). O banco do LaunchServices deste root tinha **1 plugin** só (163 apps):
   os .appex dentro de frameworks nunca foram registrados. `uicache -p X.app` registra plugins de app (o LegacyPoster
   entrou); `-[LSApplicationWorkspace registerPlugin:]` para appex de framework **trava o lsd** (tentado no
   SpringBoard e no installd; num boot os apps pararam de abrir). Solução: tweak **S9Wallpaper** (s9/ios-tools, CI
   s9-ios-tools), que põe `/var/mobile/Library/S9Wallpaper/wallpaper.jpg` por cima da `_SBWallpaperSecureWindow`
   (nível 1035; a única janela de wallpaper sem pôster = tela de bloqueio). É defensivo: nada de UIKit antes de
   UIApplicationDidFinishLaunching, decodifica fora da main thread, arquivo `disabled` desliga, `log.txt`.
   **Tela inicial continua preta**: a v3 (imagem atrás do `SBHomeScreenWindow` + log de todas as janelas, commit
   c891b06) NÃO foi testada; o release usa a v2.
   No aparelho do dono a imagem é a dele (parece o wallpaper do iOS 15 da Apple; fica fora do git). O repo leva uma
   arte original (`s9/jailbreak/wallpaper/make-wallpaper.py`).
5. **driverkitd removido** (trim): o jetsam o matava no limite de 7 MB a cada 10 s e o launchd o recriava, sempre
   com JetsamEvent + ReportCrash + osanalyticshelper.
6. **Sileo:** as fontes ficam em `/etc/apt/sources.list.d/` (`sileo.sources` é o que o app edita). 19 repos com
   Release + Packages válidos em `s9/jailbreak/repos.sources` (13 adicionados ao aparelho do dono).
7. **Frida 17** funciona (`tools/s9-frida-up.sh`: frida-server `-l 0.0.0.0`; `tools/s9-objc.py`: bridge ObjC
   minificado com terser, porque mensagens acima de ~128 KB derrubam a conexão). Mas o **attach no SpringBoard travou
   várias vezes**. Não usar no SpringBoard.
8. Reduzir Transparência e Reduzir Movimento já estavam ligados (com.apple.Accessibility).

Armadilhas novas:
- **Nunca sobrescrever um dylib que o SpringBoard tem mapeado** (scp por cima, mesmo inode). O SpringBoard seguinte
  carrega com a assinatura velha em cache, vira CS_INVALID e perde os entitlements. Aí o `+[SpringBoardUI load]`
  (`BSSelfTaskHasEntitlement`) aborta no dyld: 2 crashes e o ElleKit entra em safe mode. Instalar em arquivo novo
  (`/var/root/x.new`, rm, mv).
- Um SpringBoard ficou parado na tela da maçã depois de um respring (0,43 s de CPU; Frida e spindump travavam ao
  inspecioná-lo, o que indica processo preso dentro do dyld). Causa não confirmada (reboot forçado pelo dono).
  Não voltou com o tweak defensivo.

Pendências (ordem do dono): 60 fps estáveis por aceleração (não overclock), otimizar sem debloat, Safari fazer uma
pesquisa no Google, wallpaper na tela inicial, aba "Sobre" dos Ajustes, app Fotos (photolibraryd/assetsd cortados).
Prompt da próxima sessão: `PROMPT-proxima-sessao.md`.

## Mapa de gargalos de fluidez (2026-10-24, medido com spindump + tools/s9-cpu.sh)

Ferramentas novas: `tools/s9-cpu.sh N` (CPU por processo em N s, o iOS não tem top), `spindump` (existe no
aparelho: `spindump -notarget 5 20 -file X`) + `tools/spin-sym.py` (simboliza pelo dyld cache da IPSW em
`../work/dsc`, UUID 8F970452, via `ipsw dyld a2s`; ipsw 3.1.739 em ~/.local/bin). Log ao vivo: `s9-log.py`.

Resolvidos (repouso: **460% → 52% de CPU**):
1. securityd chamava AKS sel 10 (`aks_wrap_key`) ~300x/s, "unsupported", em loop: 80% de um núcleo + o log de
   cada chamada. **0092**: wrap/unwrap = tag de 8 bytes + chave (sem SEP não há segredo; keychain NÃO é
   protegido em repouso) e log só da 1ª chamada por seletor.
2. mediaserverd: `BTAudioHALPlugin` procurava o bluetoothd (removido no trim) por XPC sem parar → launchd 70% +
   mediaserverd 50%. Movido para `/Library/Audio/Plug-Ins/HAL.disabled/` (falta pôr no prep-ios6.sh).
3. SpringBoard 33% ocioso: `CHUISWidgetHostViewController sceneDidDeactivate` reativava a cena do widget sem
   fim, porque o trim tirou o **chronod**. trim-daemons.py agora mantém o chronod.
4. promotedcontentd/adprivacyd (anúncios): 30%. Removidos no trim.
5. **Escalonador**: XNU pega a CPU ociosa de menor número (`lsb_first`) → render/UI/realtime caíam nos A55
   (0-3) com os A75 ociosos. **0092** (`sweetfast=0xf0`): RT e prioridade >= 31 vão para A75 livre (podem tirar
   uma thread de fundo de lá); fundo vai para A55. Render do CA = 1 thread prio 54 + `dispatch_apply`
   (rasteriza linhas em paralelo).
6. vsync a 60 Hz com tela a 30 Hz: CA renderizava o dobro de quadros. **0092**: vsync = sweetfb_hz, e a
   apresentação roda no swap, numa work loop própria (antes esperava o próximo tick e travava o vsync).
7. Thread do touch (bit-bang, ~0,7 ms a cada 5 ms com dedo na tela, prioridade de kernel) iria para os A75.
   **0094**: presa na CPU 1 (A55).
8. `-enable_kprintf_spam`: kprintf com interrupções desligadas + lock global. Fora do cmdline de uso
   (boot-ios9), junto com `sweetsample=-`.

Em andamento:
9. Pixels do render (custo do CA SW é proporcional): @3x 1125x2436 = 2,74 Mpx → **@2x 750x1624 = 1,22 Mpx**
   (mesmos 375x812 pt). **0093** lê o canvas do ADT; `mkadt.py --scale2`. boot-ios9-s2 = @2x, -s3 = @3x.
   Antes de trocar a escala: apagar `systemgroup.com.apple.lsd.iconscache/Library/Caches/*` e
   `/var/mobile/Library/SplashBoard/Snapshots/*` (não há cache do MobileGestalt).
10. Apresentação na CPU: 10 ms/quadro (@3x) → 8,4 ms (@2x + linhas repetidas por memcpy, 0093). Final: o próprio
   MDSS lê a IOSurface (zero-copy) e escala com QSEED3 → 0 ms. Teste **shim M9** (`./build.sh m9`).
11. Hot spots do renderizador (spindump): `CA::OGL::SW::circle_sampler` (cantos/círculos: ícones e o tweak
   ShowMyTouches), `sample_texture`, `scan_convert`/`scanline`, `memset_pattern16`, `tex_multiply_color`.

Próximos (precisam de teste no shim antes):
12. Banda de DDR/LLCC: BCM MC0/SH0 nunca votados (ficam no que o ABL deixou); o render é limitado por memória.
13. L3 1478 → 1958 MHz e prata 1689 → 1766 exigem voto ARC do trilho MX (cmd-db `mx.lvl`).
Não são gargalo (medido): disco UFS (394 MB/s leitura, 170 MB/s escrita), RAM (~1 GB livre), sampler.
GPU: não existe driver Metal para Adreno; os forks que ligam o Adreno usam-no só como copiador (blit), o que o
MDSS (item 10) faz melhor e sem GPU.

## Próximos passos (para a próxima conversa)

1. **Fluidez** (pedido do dono depois do vídeo). Não existe driver iOS pra Adreno: o render continua na CPU.
   Investigar nesta ordem, medindo antes e depois (`sweetsample` / fps no log):
   - se os 4 A75 (núcleos 4-7, 2,8 GHz) estão mesmo rodando o backboardd/SpringBoard, e o clock deles (o ABL
     pode deixá-los baixos; frequência via OSM/cpucp do SDM845, ver o kernel Lineage);
   - o caminho de cópia/escala do sweetfb (0086: estica 1125x2436 -> 1440x2960 na CPU a cada quadro; testar
     `sweetfb_noscale=1` / `sweetfb_aspect=1` e `sweetfb_hz`), NEON e cache na cópia;
   - reduzir efeitos (Ajustes > Acessibilidade > Reduzir movimento / transparência) como paliativo imediato.
   - **Tela apagando devagar:** provavelmente a animação de apagar renderizada na CPU e/ou o backlight/DPMS sem
     driver (o sweet faz o quê no power?). Ver no log o que o backboardd faz ao apertar o power.
2. **Internet por USB** (CDC-ECM, `sweetusb=2`), testando no shim primeiro (novo stage). Levantado:
   - DWC3 em 0xa600000, QSCRATCH 0x0a6f8800 (overrides de VBUS como no sweet_usb.c), GDSC usb30_prim 0x10f004,
     QUSB2 PHY v2 em 0x88e2000 (init-seq no DT `qcom,qusb-phy-init-seq`), SS PHY QMP 0x88e8000, ahb2phy 0x88ee000.
   - O ABL da Samsung NÃO deixa o USB ligado (o Redmi sim, por fastboot): ligar GDSC, clocks (conferir offsets
     do sweet = SM7150 no gcc-sdm845.c) e PHY do zero. PHY precisa de vdd/vdda18/vdda33 via RPMh
     (sweet_rpmh.c tem endereços do SM7150: conferir RSC/TCS no SDM845).
   - **Risco:** DMA do USB passa pela SMMU (SID 0x740, anoc_2_tbu), igual ao GPI que travou. O 1º teste no shim é
     exatamente o controlador escrever na RAM; o PC vê o dispositivo no `dmesg`. Se travar: votos BCM no RPMh.
3. Depois: Sileo/NewTerm/ElleKit, boot mais rápido (~7 min hoje), relógio (sem RTC), 4 GB de RAM.
   Wi-Fi (WCN3990 via modem, QMI, rmtfs sem escrever EFS) fica pra bem depois: semanas a meses.

## Objetivos do dono para depois do boot do iOS (2026-10-23)

Usar como um iPhone com jailbreak, publicado como fork no GitHub com créditos (sweet MIT, Apple APSL, sem binários
da Apple). Avaliação honesta de cada item:
- **Touch** (sec_ts, GENI I2C 0xa8c000, 0x48, IRQ GPIO 120): viável. Portar o protocolo de drivers/input/touchscreen/
  sec_ts como o sweet fez com o Goodix. Ver antes os reguladores do touch no DTB (nunca votar às cegas).
- **Wi-Fi** (WCN3990 do SDM845): muito difícil. Exige QMI + subir o firmware no modem/WCSS + driver tipo ath10k-snoc no
  XNU, e a interface Wi-Fi do iOS (IO80211) é fechada, então apareceria como rede cabeada. Alternativa que já existe
  no sweet: **internet pelo cabo USB** (CDC-ECM, sweetusb=2, o PC compartilha a conexão), suficiente pro Sileo.
- **Sileo + NewTerm + ElleKit** (bootstrap Procursus rootful, kernel já permissivo): viável, pré-instalado na imagem
  do root (sweet-root-tweaks). O Substitute é obsoleto; o ElleKit o substitui.
- **Fluidez**: sem GPU (não existe driver iOS para Adreno, render em CPU como no sweet). O maior ganho é usar os 4 A75
  a 2,8 GHz (o sweet só usa A55). Renderizar menor (ex.: 720p) e escalar 2x na CPU é possível, mas mexe na identidade/
  escala da interface. 20-30 fps é o alvo realista; 60 fps não, sem GPU.
- **4 GB de RAM**: o XNU quer um bloco contínuo. Os ~2 GB de cima são limpos; descer abaixo de 4 GB exige tirar as
  áreas protegidas da Qualcomm/Samsung do physmap (patch no kernel). Uns ~3 GB parecem possíveis depois.

## Fatos conferidos no boot.img/DTB stock (não no "achismo")

- boot.img: header **v0**, pagesize 4096, base 0, kernel_offset 0x8000, ramdisk_offset 0x2000000,
  tags_offset 0x1e00000, board `RILQI20A009`, os 10.0.0 / 2022-05.
- kernel = Image.gz + 15 DTBs colados (Rev02..Rev14, msm-id 0x141) + 1 blob RTIC (MP_DATA, descartado).
- Splash: `cont_splash_region@9d400000`, 0x2400000 (36 MiB). Confere.
- Painel S6E3HA8 (AMB577PX01): **modo comando**, TE pelo pino, **DSC** (2 encoders, lm-split 720+720),
  **um DSI só** (dsi-ctrl = só DSI0; topologia 2 LM / 2 DSC / 1 INTF). Não é dual-DSI.
  O sweet tem painel em modo *vídeo* ("no refresh kick needed"); aqui precisa de autorefresh ou kick.
- SDE (qcom,mdss_mdp@ae00000): CTL 0xae02000 + 0x200*n (5), SSPP vig 0xae05000/7000/9000/b000,
  dma 0xae25000/27000/29000/2b000, PP 0xae71000/71800/72000/72800/73000, INTF DSI0 0xae6b800.
- Touch: `sec,sec_ts` em I2C 0x48 no `i2c@a8c000` (QUPv3 SE11), IRQ no GPIO 120. Confere.
- IMEM 0x146bf000 (restart_reason @0x65c), PS_HOLD 0xc264000. Reboot reasons em include/linux/sec_debug.h.
- RAM reservada útil depois: `ramoops@a1300000` (1 MiB), `ss_plog@a1100000`.
- TIMA 0x9f800000 (2 MiB), RKP 0x9fa00000 (15 MiB). Confere.
- PIT: BOOT = 64 MiB, RECOVERY = ~69 MiB. Existe partição VBMETA, mas o firmware **não traz vbmeta.img**:
  o S9 não usa AVB 2.0, não há "vbmeta OFF" pra flashar.

## Instalação

- Heimdall: `heimdall flash --BOOT out/boot.img`. Odin: `out/boot.tar.md5` no slot AP.
- Loop rápido (TWRP): `tools/flash-twrp.sh`. Backup antes do 1º flash: `tools/backup-twrp.sh`.
- Nunca tocar: XBL, ABL, TZ, HYP, PIT, EFS, MODEMST*, FSG, FSC, PERSIST.

## O que o M1 faz (m1.c)

1. Pinta com o layout padrão de GOP (32 bpp, B,G,R,X) antes de mexer em qualquer registrador.
2. Alimenta o watchdog do APSS (se o ABL deixou ligado). Lê DTB (modelo, `msm_drm.dsi_display0=`, `lcd_id=`), EL, MIDR, suporte a página de 16K, versão PSCI.
3. Lê o SDE: qual CTL alimenta o DSI e em que modo, autorefresh nos PPs, qual SSPP lê a RAM (endereço, stride, formato).
4. Redesenha com o layout real, faixas R/G/B, diagnóstico; kick com CTL_FLUSH(bit 17) + CTL_START se não houver autorefresh.
5. Conta 60 s na tela (kick a cada segundo) e desliga por PSCI SYSTEM_OFF (fallback: reboot).

Leitura do resultado: tela mudou e o TEMPO anda = OK. Tela não mudou mas desligou em ~1 min = shim rodou, kick não funcionou.
Ficou preso no logo pra sempre = travou cedo (ex.: leitura de registrador) ou o ABL não aceitou a imagem.
Reinicia sozinho em ~10-15 s = watchdog (0x17980000, bark 11 s) ou o ABL recusou. Tela azul "PANICO" = exceção (foto!).

- **🎉 CAUSA RAIZ do "jailbreak quebrado" = falta de RAM, não trust/rede (2026-10-24):** Sileo não instalava
  ("restart Sileo"), NewTerm morria ("session finalized"), Safari não abria página. Diagnóstico: os crashes eram
  quase todos **JetsamEvent** (matador por pressão de memória), um a cada ~10 s. Com 2 GB e ~28 MB livres, o jetsam
  matava Sileo/NewTerm/WebContent ao nascer. `su mobile -c id` funciona (não é AMFI/trust). Fix: **remover 82 daemons
  inúteis** (sem cellular/conta/sensor/BT/Wi-Fi/GPU: vmd, locationd, cloud*, homed, health*, spotlight/search, siri*,
  identityservices, callservices, bluetoothd, fairplayd, etc.) do `/System/Library/xpc/launchd.plist`, protegendo a
  infra (SpringBoard, backboardd, mediaserverd, lsd, installd, container/usermanager, runningboard, ssh, trustd,
  amfid, nossos jobs). `tools/trim-daemons.py PLIST --apply` (ligado no `prep-ios6.sh`; aplicado ao vivo + reboot,
  backup em `launchd.plist.orig`). **Resultado: 28 MB → 163 MB livres, 533→451 daemons, processo de 15 s sobrevive,
  jetsam parou.** A internet dos apps já vinha OK com o s9net v3 (reachability=1; publicar o serviço SEM forçar a
  primária — forçar derrubava o SpringBoard por watchdog). Falta o dono confirmar Sileo/NewTerm/Safari na tela.
- **RAM: alvo 3,2 GB** (teste shim M8 pronto, `./build.sh m8`). Hoje o iOS vê só 2 GB (bloco contíguo de cima,
  0x100000000+0x7e1c0000). Os 4 GB físicos (0x80000000+0xfe1c0000) têm a metade de baixo esburacada de carveouts
  (modem, hyp, IPA 320M+85M, SPSS, fb, debug). Dois buracos livres: R1 [0xa1800000,0xc0000000) 487M e
  R2 [0xd4000000,0xff000000) 688M. 2017+688+487 = ~3,2 GB, mas exige XNU gerenciar um intervalão com 2 carveouts
  reservados (0xc0000000-0xd4000000, 0xff000000-0x100000000). M8 confirma na mão que R1/R2 são RAM livre antes de arriscar.

- **🎉 iOS com ~3,1 GB de RAM (2026-10-24, boot-ios7):** patch **0091** (kernel 10). O XNU exige RAM contígua
  (vm_page array indexado por `phys_page = base + i`), mas as duas carveouts da Qualcomm (IPA 0xc0000000-0xd4000000,
  SPSS 0xff000000-0x100000000) ficam no meio do nosso span. Solução: dar ao XNU o span inteiro `[0xa6000000, topo]`
  (base acima do staging do ABL — abaixo disso o loader recusa: "ramdisk overlaps kernel memory"), manter o array
  **contíguo** (entregar todas as páginas, assertion passa) e excluir os buracos **só da lista livre** via
  `pmap_is_bad_ram()` nos pontos de release do `pmap_startup` (mesmo mecanismo das páginas ECC ruins). mkadt:
  dram-base 0xa6000000, dram-size 0xd80c0000. Resultado medido: **hw.memsize 2016→3456 MiB, 1,43 GB livres**,
  SpringBoard sobe. Confirmado antes no shim M8 que R1/R2 eram RAM livre. Base NÃO pode ser 0xa1800000 (bate no
  payload do ABL em ~0xa2000000). Falta: black clamp (próximo build).
