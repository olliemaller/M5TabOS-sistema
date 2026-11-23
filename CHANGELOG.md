# Changes

The format follows [Keep a Changelog](https://keepachangelog.com/en/1.1.0/),
and versions follow [SemVer](https://semver.org/). The firmware version lives
in `version.txt`. Which number to bump, and how to release a version
(`ferramentas/lancar.sh`), is in `docs/VERSIONAMENTO.md`. Entries up to 0.9.1
come from upstream and are in Portuguese.

## [Unreleased]

### Added
- **English by default**: a new Tab5 (or one whose language was never chosen)
  starts in English. Portuguese stays in Settings > Language. About 160 more
  interface, notification, error and console texts now go through `tr()` and
  have English in `idiomas/en.po` (SSH, flash-drive mode, USB-A drive, file
  copy errors, Meshtastic Bluetooth pairing, battery, Berry errors, Mesh
  delivery errors and more); developer diagnostics on the console are in
  English.
- **English web pages**: the PC portal, Terminal, Berry editor, Camera and
  Mesh pages, and the new-script template.
- **Time-zone regions with automatic daylight saving** (`services/tab5_fuso`):
  Settings > Date and time and the welcome assistant pick a region
  (Melbourne/Sydney/Canberra/Hobart, Adelaide, Brisbane, Darwin, Perth,
  Auckland, Tokyo, India, Kathmandu, Europe, London, the US zones and more),
  and the clock changes for summer time by itself. Times of old messages use
  the offset in force at that moment. The manual fixed offset stays as
  region 0.
- **12-hour clock** option (Settings > Date and time): `9:05pm` in the status
  bar, home, lock screen, chats, Power graphs and Berry's `sistema.hora_de`.
- Console command **`timezone`** (`fuso`): `timezone list`, `timezone <n>`,
  `timezone manual <minutes>`, `timezone 12h|24h`.
- Berry: `ajustes.relogio_12h()`; `ajustes.fuso()` now returns the offset in
  force (daylight saving included).
- **Screenshot from the PC**: the portal Overview has a "Screenshot of the
  Tab5" button (`/api/tela.bmp`, streamed, no 1.8 MB buffer).
- `display photo` (`tela foto`) without a name saves a dated file instead of
  overwriting `tela.bmp`.
- Meshtastic internet mode, **Automatic** server: picks the server and topic
  root from the time zone (Australia/New Zealand `msh/ANZ`, Europe
  `msh/EU_868`, North America `msh/US`, Japan, India; Brazil keeps Mesh
  Brasil) instead of always Mesh Brasil, and shows which in its name.
- README and all the reference docs in English (API, ARQUITETURA, BERRY,
  BERRY_API, COMPOSITOR, ESCREVENDO_APPS, MESHTASTIC, RADIO_C6, TERCEIROS,
  USB_A_E_FONE, VARIANTES, VERSIONAMENTO).
- Code comments, serial-log messages (`ESP_LOGx`), console diagnostics, the
  system monitor's events and the build scripts' comments and messages in
  English. Identifiers, the Berry API, settings keys and SD paths are unchanged.
- Berry example apps: English comments, on-screen text in the interface
  language (`idioma.escolher`).

### Fixed
- Wi-Fi: a 32-character SSID or a 64-digit hex key was cut by one character
  before going to the C6 (`strncpy` with `- 1` on fields that need no
  terminator), so those networks never connected.
- Mesh: truncated protobuf fields (telemetry, positions, node info) could
  store uninitialised stack values; they now read as 0.
- Mini-player: a track title longer than the buffer counted as "changed" on
  every update, redrawing the notification centre endlessly while it played.
- Languages: a `.po` without a `Language:` header leaked its memory; date
  formats longer than 95 characters after the placeholder were cut.
- GPS: the last field of an NMEA sentence without a `*hh` checksum came back
  empty.
- USB-A: a malformed product string descriptor (length under 2) is ignored.

## [0.9.1] - 2026-11-15

### Adicionado
- **Animação de boot** (`kernel/boot_anim`): no lugar do "M5Tab OS"
  parado, uma teia viva - sete nós fixos da malha, com halo, e 60 pontos de
  fundo que passeiam se ligando e se soltando deles; dos nós saem caracteres
  que voam até o centro, o nome "M5tabOS" é decifrado letra por letra (com
  cursor de terminal) e aparece "[ conectado ]". ~1,9 s, sorteada a cada boot,
  ~30 quadros/s pelo compositor; um toque pula. Só ao ligar (não ao acordar),
  e a chave `TAB5_MOSTRAR_ANIM_BOOT` (kernel/internal.hpp) a desliga no
  desenvolvimento. A teia é pintada em faixas de 32 linhas na RAM interna e
  cada faixa vai para a tela pelo DMA2D (`comp_copiar_faixa`): escrever as
  linhas direto na PSRAM disputava a memória com o painel e, num boot a
  frio, fez a tela piscar azul. O nome fica junto com a teia (sem caixa
  por trás). Se a tela falhar um quadro durante a animação, o log avisa.

## [0.9.0] - 2026-11-15

### Adicionado
- **Repaginação visual com tema central**: `main/tab5_tema.hpp` define as
  cores por papel (fundo, superfície, linha, três níveis de texto, um só
  destaque, estados e 10 cores de app), raios, espaços, letras e tamanho de
  toque; os escuros já estão em RGB565 (o cinza de 24 bits saía esverdeado).
  Apps Berry usam as mesmas constantes pelo módulo `tema`. O app que já está
  no tema marca `app_t.fundo_tema` e tem a área limpa na cor do tema
  (`tab5_celula` e `tab5_escrever` pegam o fundo certo sozinhos). Barra,
  navegação (Início em pílula), home com cartão do relógio (hora, data, Wi-Fi,
  Mesh), central de avisos, bloqueio, teclado virtual e os apps do sistema
  foram para o tema novo. `docs/ESCREVENDO_APPS.md` (seção 2.5) ensina a usar.
- **App Vídeo**: toca MJPEG (JPEG por hardware) e MP4 do celular com H.264 e
  som AAC (`.mp4`, `.m4v`, `.mov`), com o som pela Música; até 1080p cabe na
  PSRAM e a imagem é reduzida até 1280x720 pelo PPA. O vídeo nunca atrasa do
  som: quadros B são pulados e, se preciso, salta ao próximo quadro-chave. A
  Música também toca `.m4a`. Novo serviço de imagem sem limite de tamanho,
  com GIF animado.
- **Decodificador H.264** (`services/tab5_h264`): perfis Main e High, CABAC e
  quadros B, conferido bit a bit com o ffmpeg (`testes/rodar_h264.sh`, com
  modo de fuzz). Medido: 360p ~11-14 quadros/s, 480p ~5 quadros/s. A
  bancada `video h264` fica no console.
- **Ajustes em grupos**: as páginas ficam em cartões por grupo (Conexões,
  Tela e som, Sistema), como na maquete. Brilho e volume entram na própria
  lista e o dedo que cai no trilho prende o controle sem rolar a lista. Na
  página USB há "apagar itens com mais de 30 dias" da lixeira (desligado de
  fábrica).
- **Lixeira no Arquivos**: cada disco tem uma lixeira que mostra origem, data
  e tamanho; toque restaura ou exclui e toque longo escolhe vários. Restaurar
  volta para a pasta de origem (recriada se faltar, com nome livre). O aviso
  de lixeira cheia usa 8% do disco (256 MB a 10 GB). No console, `arquivos
  lixeira <raiz>` só lista e há restaurar, excluir, esvaziar e antigos.
- **ESP-NOW mais completo**: `espnow msgs` mostra os bytes recebidos em
  hexa e texto, `espnow para <MAC>` manda direto e `espnow todos` varre os
  canais 1-13 (solta o roteador e volta). No Berry, `enviar_para` e
  `enviar_todos`. Escolher uma rede tira o ESP-NOW do canal fixo.
- **Berry: módulos novos e um arquivo por módulo**: os módulos saíram de
  `berry_apps.cpp` (2847 -> 855 linhas, só o motor) para
  `main/services/berry/`. Novos: `rede` (baixa sem travar o script, em tarefa
  própria) e `ajustes` (brilho, volume e leituras). Aviso com nível e
  `mesh.enviar` (só mensagem direta, 1 a cada 10 s; recusa nó 0, difusão e
  texto vazio).
- **Música sem engasgo ao sair e voltar ao app**: a tarefa que entrega o som
  ao alto-falante (M5Unified) agora tem prioridade acima da interface e ~85 ms
  de folga no DMA (eram ~43 ms, prioridade 2) - medido, abrir um app deixava
  o áudio ~350 ms sem CPU. E a capa decodificada fica guardada enquanto a
  música toca: reabrir o app não decodifica o JPEG de novo.
- **`help` no jeito do Linux, `help sistema` para o resto**: o `help` mostra
  os comandos de sempre com o nome do Linux (ls, cd, cat, cp, ps/top, free,
  ifconfig, nslookup, nc, curl, wget, ssh...) e aponta para `help sistema`
  (tela, cartão, rede, Mesh, energia... agrupados como antes), `help app`,
  `help <comando>` e `help tudo` (`help all`).
- **Comandos em inglês, além do português**: cada comando e subcomando tem o
  nome em inglês, e os dois valem sempre (`estado`/`status`,
  `energia travas`/`power locks`, `wifi varrer`/`wifi scan`,
  `mesh enviar para`/`mesh send to`). O subcomando só é trocado onde o uso do
  comando o conhece (posição 1 ou 2), nunca num nome de arquivo ou de rede. O
  que aparece segue o idioma do sistema: em português o `help` e as sugestões
  mostram só os nomes em português; em inglês, só os em inglês.
- **Console e Terminal inteiros em inglês** com o sistema em inglês: a ajuda
  de cada comando (resumo, formas, explicação e exemplos) e as respostas dos
  comandos passam pelo `tr()` e estão no `en.po` (~880 textos). Os
  subcomandos em inglês valem até a 4ª palavra (`music playlist rock create`,
  `c6 sleep auto off`).
- **O que segura o repouso, na tela**: o app Tarefas ganhou a linha "EM USO"
  e a Energia a linha "Segura o repouso" (sob os cartões), com cada recurso
  em uso e quem o declarou - "câmera (navegador), rede (servidor web)". Antes
  só no console (`energia quem`) e no Berry (`sistema.recursos()`).
- **A câmera do app aberto pausa com a tela apagada, pelo sistema**
  (`tab5_camera_pausar`/`despausar`, chamados pelo kernel no repouso): o
  preview para de escrever na tela, o uso do app é solto e volta ao acordar
  com o mesmo destino. O app Câmera e os apps Berry perderam os ganchos
  `ao_repouso`/`ao_retomar` de câmera; o navegador (outro consumidor) segue
  filmando. Uma foto em captura nessa hora volta como falha (`ok = false`).
- **Câmera no navegador** (`services/tab5_camera_web`, `/camera/` no portal):
  a câmera do Tab5 ao vivo no PC (640×480 ou 960×720) e a foto em resolução
  cheia (1200×1600) para baixar. Quadro a quadro com o codificador JPEG do
  chip - o servidor segue atendendo os outros apps entre um quadro e outro;
  a câmera liga no primeiro quadro e desliga 5 s depois do último. ~8 q/s
  medidos do PC (espera 16 ms, JPEG 9 ms, envio 41 ms por quadro).
- **"OK para MQTT" do nó** (app Mesh, Conexões e internet: "minhas mensagens
  no MQTT"; `Config.lora.config_ok_to_mqtt`): sem ela, gateway de servidor
  público descarta o que o nó manda. O `mesh` do console mostra o valor, e o
  log registra o carimbo de cada mensagem recebida ("texto de X: OK para MQTT
  sim/NAO") - foi o que provou por que as mensagens de outro rádio não
  chegavam ao servidor.
- **Portal no navegador** (`services/tab5_web_portal`): `http://<ip>/` é a
  área de trabalho do Tab5 no PC - visão geral (bateria, rede, memória,
  tempo ligado) e todos os apps que têm página no PC (`app_t.no_pc`), abertos
  ou não; o app aparece ali dentro, sem sair do portal, e o fechado abre com
  um clique. O painel "No PC" do aparelho mostra esse endereço curto.
- **Terminal inteiro no navegador** (`services/tab5_terminal_web`, antes
  `tab5_ssh_web`): os comandos do Tab5 na mesma sessão da tela, e o SSH
  quando `ssh usuario@host` abre. Comando `web` no console (`web ligar`,
  `web desligar`) e `ssh fechar`.
- **Apps no navegador do PC** (serviço `services/tab5_web`): um servidor web
  só, com uma pasta por app e a mesma senha para todos - `/editor/` (os
  scripts Berry com cores, Verificar com a linha do erro, Rodar no Tab5 e
  gravação automática), `/mesh/` (canais e conversas, mensagens com o estado
  de entrega, a aba Nós com a ficha de cada nó, favorito e mapa, a saúde da
  malha) e `/terminal/` (abaixo); a raiz é o portal. Só liga quando a
  pessoa pede no app (painel "No PC" do sistema, `kernel/painel_web`), só da
  rede local, senha curta na tela, Host e origem conferidos, WebSocket para o
  terminal. Roda de fundo (aparece no Tarefas e na barra); cada app fecha a
  sua parte depois de 10 min sem uso.
- **SSH de verdade, compartilhado** (`services/tab5_ssh` + `components/libvterm`):
  a sessão é um terminal xterm-256color (nano, top, vim) que mora no sistema;
  a tela do Terminal e a página `/terminal/` (xterm.js da flash, funciona
  com o PC sem internet) são só espectadores - começa num e segue no outro. O
  tamanho é o de quem digitou por último; impressão digital e senha pedidas
  por qualquer um dos dois; fileira Esc/Tab/Ctrl/setas.
- **Teclado virtual como no Android e no iPhone**: a tecla de ação segue o
  campo (↵ no editor, nas Notas e na conversa; OK nos ajustes; BUSCAR na
  busca), acentos no toque longo da letra e pontuação no do ponto, espaço
  duplo vira ". ". A conversa do Mesh e do ESP-NOW tem várias linhas (envia
  pelo botão da barra; Shift+Enter no físico), guardadas no histórico.
- **App Terminal**: os comandos do sistema na tela, com o teclado físico ou o
  virtual - `help`, `ls`, `cd`, `cat`, `cp`, `mv`, `rm`, `mkdir`, `df`,
  `ping`, `wifi`, `ble`, `berry`, `mem`/`free`, `tarefas`/`ps`, `mesh`... os
  mesmos do console serial (uma tabela só). Setas trazem o que foi digitado
  antes, Ctrl+C para o comando, Ctrl+L limpa, Ctrl+V cola. O comando roda em
  segundo plano: fechar o app não para; só o comando que passa de 3 s ganha
  a pastilha "Terminal" (com o comando) e o X da central, e o curto não pisca
  na barra. Botão Parar na linha de comando. Voltar mostra a tela como estava.
  Motor em `services/tab5_terminal` (linhas na PSRAM só depois da primeira
  abertura; nenhuma tarefa parada esperando).
- **Comandos de rede** (serviço `services/tab5_rede`, no console e no
  Terminal): `ip` (`ifconfig`), `dns` (`nslookup`), `arp` e `arp varrer` (os
  aparelhos da rede local com o MAC), `traceroute` (`tracert`), `tcp` (`nc`:
  a porta está aberta?), `http` (`curl`/`wget`: HTTP e HTTPS com certificado
  conferido, redirecionamentos, `-I` e `-n`) e `busca` (resumo da Wikipedia
  em português, ou o DuckDuckGo; sem conta nem chave). Os longos param no
  Ctrl+C. Com a tela apagada, o comando do Terminal pede 200 MHz de piso
  (sem isso o HTTPS estourava o prazo a 40 MHz).
- **SSH (cliente)** (`services/tab5_ssh`, libssh2 1.11 com o mbedTLS do
  sistema em `components/libssh2`): `ssh usuario@host <comando>` roda lá e
  mostra a saída e o código de saída; `ssh usuario@host` abre um shell linha
  a linha. Senha por "password" ou "keyboard-interactive" (sem eco na serial,
  mascarada no Terminal), impressão digital SHA256 da chave do servidor, Ctrl+C
  desconecta. Alocações na PSRAM; o aperto de mão usa ~3 KB de pilha. Ainda
  não: chave privada, lista de servidores conhecidos e tela cheia.
- **Mesh: tela e MQTT do nó** (Configuração): "tela do nó apaga em" (o
  `screen_on_secs` do C6L, 15 s a 15 min) em Avisos e tela; "mandar ao MQTT
  o que recebe" (o uplink do canal principal) em Conexões. Sem o uplink, o nó
  só leva ao MQTT o que ele mesmo manda.
- **Notas no jeito dos apps de notas**: a lista mostra o título em negrito,
  a data e uma prévia do texto (a segunda linha); o editor mostra o título no
  alto enquanto se digita ("Nova nota" vazia).
- **Baixar arquivos**: `wget <url>` grava na pasta atual com o nome do fim
  da URL; `http -o <arquivo> <url>` grava onde pedir. Nova escrita do motor
  de arquivos (`tab5_arq_novo_*`): permissão da pasta, nome repetido vira
  "x (2)", e o que não terminou bem é apagado (nunca fica arquivo pela metade).
- **Memória**: mais ~5,7 KB de estáticos da RAM interna para a PSRAM (ordem
  dos ícones, central de avisos, texto do bloqueio, diagnóstico das passadas,
  cópia das redes do nó e o corpo da cobrinha).
- **Avisos na barra de cima como ícones**: o ícone pequeno de cada app com
  novidade (o mesmo da tela inicial), com um ponto quando o aviso pede
  atenção, e "+N" quando não cabem. O texto ficou só na central (tocar a
  barra), no lugar da pastilha grande com o resumo.
- **Arquivos**: itens mais altos (88 px, o conteúdo no meio) e a linha entre
  eles num cinza suave.
- **Proteções contra estouro de buffer e corrupção de memória**: canário na
  pilha (`-fstack-protector`: estouro de vetor local derruba na volta da
  função, com o nome dela) e envenenamento leve do heap (marca antes e depois
  de cada bloco). `mem integridade` confere o heap inteiro. Custo medido: +37 KB
  de código e ~9 KB da RAM interna livre (202 → 193 KB).
- **Editor (Notas): copiar só uma parte**: o toque longo abre Palavra (a sob o
  dedo), Linha (o parágrafo), Tudo e Colar.
- **Emojis no teclado virtual**: uma página com 36, os desenhos leves do
  sistema (os mesmos do Mesh); nas páginas de símbolos a tecla da esquerda
  circula `=\<` → 😀 → `?123`.
- **SSH: servidores conhecidos**: a chave de cada servidor é guardada na
  primeira conexão (`/sdcard/apps/SSH/known_hosts`); se mudar, o ssh recusa
  antes de pedir a senha. `ssh esquecer <host>` apaga a guardada.
- **Editor de Berry no aparelho** (app Editor, variantes com Berry): os
  scripts de `apps/Berry` e "+ Novo" (nasce com um modelo). O editor do
  sistema ganhou o MODO CÓDIGO: fonte monoespaçada, número de linha, cores do
  Berry (palavras-chave, textos, números, comentários, chamadas), Enter mantém
  o recuo, Tab = 2 espaços. Verificar compila sem rodar e marca em vermelho a
  linha do erro; Rodar salva, verifica e abre o app; Ctrl+S / Ctrl+R no
  teclado físico; voltar ou fechar salva. `berry verificar <arquivo>` no
  console e no Terminal (`berry_verificar`, no serviço do Berry).
- **Energia dentro do Ajustes**: a linha "Energia" (com a bateria e o consumo
  de agora) abre o app Energia, que saiu da tela inicial. Novo campo
  `app_t.fora_da_home` para qualquer app que more dentro de outro.
- **Abertura animada**: tocar num ícone faz o app crescer a partir do lugar
  dele, na cor dele escurecida (~0,15 s, até 85% da área - a abertura normal
  completa).
- **Comando que pergunta** (`console_ler_linha`): um comando pode pedir uma
  linha a quem digita - a serial lê a própria porta, o Terminal mostra a
  pergunta na barra e a linha vai para o comando em vez de virar comando novo.
- **Console com sessão e pasta atual**: a serial e o Terminal têm cada um a
  sua pasta (`cd`, `pwd`) e rodam ao mesmo tempo (um ping longo na tela não
  trava a serial); todo comando de arquivo aceita caminho relativo a ela,
  com `.` e `..`. Novo grupo SHELL no `help`. O `print` do Berry sai onde
  o comando foi digitado. O `ping` para no Ctrl+C.
- **Idiomas em arquivo `.po` (gettext)**: a interface inteira dos apps
  nativos (Ajustes, Arquivos, Mesh, ESP-NOW, Música, Energia, Tarefas, Câmera,
  IMU, Exemplo, Notas) e do sistema passa por `tr()`, e o inglês está 100%
  traduzido (833 textos). Cada idioma é um arquivo: `idiomas/en.po` vai
  embutido; um `fr.po` (ou outro) em `/sdcard/apps/Sistema/idiomas/` aparece
  em Ajustes > Idioma no próximo boot, sem compilar. `ferramentas/idiomas.py`
  junta os textos do código (`M5tabOS.pot`) e atualiza os `.po` sem perder
  traduções. A data por extenso também vem do `.po`.
- **Ajustes em pé**: brilho e volume numa coluna estreita à esquerda, em
  barras verticais; a lista de opções à direita.
- **Menos código repetido**: o Arquivos usa o toque longo do sistema
  (`kernel/toque_longo.hpp`) em vez da cópia dele; `tab5_escrever` (texto numa
  altura exata, com corte) substitui as funções `texto()` do Ajustes e da
  Música; `tab5_duracao` ("2 h 05 min") substitui as três cópias do Ajustes,
  do Mesh e da Energia.
- **Notas**: Copiar tudo e Colar saem do alto e aparecem no toque longo, no
  lugar do dedo (menu do `kernel/editor`).
- **App Notas**: lista das notas (a mais nova primeiro) com "+ Nova"; tocar
  abre, segurar escolhe e mostra Copiar e Apagar. O título é a primeira
  linha. Cada nota é um `.txt` em `/sdcard/apps/Notas` (`services/notas`), e o
  cartão só é tocado pelas portas do `tab5_sd` e no worker do sistema. O
  editor de várias linhas é do sistema (`kernel/editor`).
- **Porta `tab5_sd_apagar`**: apagar um arquivo da pasta do app pela mesma
  trava de gravar (recusa com o cartão emprestado).
- **`tab5_utf8_anterior/seguinte/corte`** (`tab5_unicode.hpp`): uma cópia só
  do andar e cortar em UTF-8 (conversa, editor, notas, transferência).
- **Área de transferência** (`services/transferencia`): um texto de até 4 KB,
  na PSRAM só depois da primeira cópia. Segurar um balão no Mesh ou no
  ESP-NOW copia a mensagem; segurar a barra de escrita (ou Ctrl+V) cola. Nas
  Notas: Copiar e Colar no alto do editor. Módulo Berry `transferencia`.
  A confirmação ("Copiado", "Colado") aparece por ~2 s na barra de status, no
  lugar do nome do app (`sistema_recado`).
- **Inglês na interface**: Ajustes > Idioma (e o primeiro passo das
  boas-vindas) escolhe entre Português e English, e vale na hora. Nesta
  primeira fase estão traduzidos a barra de status, a tela inicial, a tela de
  bloqueio, a central de avisos, as boas-vindas e a lista do Ajustes; o que
  ainda não foi traduzido continua em português. Serviço novo
  `services/idioma` (`tr("texto em português")`, tabela em `idioma_en.cpp`).
- **Módulo Berry `idioma`**: `atual()`, `nome()`, `tr(texto)` e
  `escolher(pt, en)` para os apps Berry seguirem o idioma escolhido.
- **`gravar.sh` sem compilar à toa**: sem fonte novo desde o último build,
  grava o binário que já existe (`COMPILAR=1` força o build).

### Mudado
- **Energia: C6 dorme também com o Wi-Fi desligado**: com o rádio parado
  (cochilo) o repouso sem Wi-Fi caiu de 20,6 para 12,7 mA. Medido: `c6 laco
  200` sem falha e ensaio na bateria sem reinício (563 desmontagens). O
  controlador SDMMC, dividido entre o cartão e o C6, ganhou uma trava só.
  Com o Grove desligado a vigia não acorda mais o P4 a cada 10 s.
- **Repouso com teto de clock fixo em 180 MHz**: reconfigurar o gerenciador
  de energia com o sono leve ativo travava o Tab5 quando um recurso (um
  download) chegava com a tela já apagada. O teto é decidido uma vez na
  entrada do repouso; ocioso o DFS desce a 40 MHz e as travas de recursos
  seguem proibindo o sono.
- **Cabo no PC segura o sono leve** (`USJ_NO_AUTO_LS_ON_CONNECTION`): com a
  bateria cheia o repouso dormia e o USB do console travava o Tab5.
- **Rede mais rápida**: a janela TCP passou de 4 para 8 pacotes e a leitura
  é em pedaços de 16 KB (um `write` por pedaço no cartão): 77 -> ~167 KB/s,
  mantendo ~102 KB de RAM interna mínima (16 pacotes davam 334 KB/s mas
  levavam o mínimo a ~80 KB).
- **`esp_hosted` 3.0.10** no P4 e no C6 (host e coprocessador conferidos).
- **Memória: maior bloco da SRAM de 51 para 115 KB**: funções de tarefa do
  FreeRTOS e do ringbuf na flash (-21 KB de IRAM), `console_*.cpp` com
  `.bss` na PSRAM (-8 KB) e sem a reserva interna de 32 KB (nunca usada).
  Livre 169 -> 201 KB, mínimo 122 -> 185 KB. O comando `tarefas` mostra onde
  mora cada pilha. O driver do IMU (FIFO do BMI270) foi para o HAL, e
  Farejo e CSI não acordam mais em fatias de 100 ms. Mesh-BLE e GPS não
  despertam à toa.
- **Motor saindo dos apps para `services/`**: MQTT do Mesh, leitor de
  linhas e texto, GNSS (dias, pontos, satélites), registro do dia e
  hash FNV-1a ficam em serviços; Ajustes, Mesh, Arquivos e outros só chamam.
  Arquivos e a capa da Música leem pelo serviço de volumes, sem `fopen`.
- **Relógio do painel só nas frequências que o chip gera** (80/60/48/40/30/
  24 MHz): pedir 65 dava 60 e a tela mostrava 65. O ajuste mostra o valor
  aplicado; o padrão é 80, o do M5GFX para o ILI9881C.
- **Home mais rápida**: o papel de parede fica guardado com app aberto e no
  repouso (a volta à home caiu de 145 para 86 ms).
- **Código partido em arquivos menores**: console, Mesh (serviço e app),
  `core/shell/app_config`, Música e animações do bloqueio, sem mudar o
  comportamento. Funções que nenhuma variante chama foram removidas.
- **Suspensão decidida pelo sistema** (`kernel/recursos`): o serviço dono do
  hardware declara o recurso que usa (câmera, som, rede, rádio, serial do
  Grove, USB-A, cartão, tomada, tela acesa) e uma tabela única diz o que o
  repouso preserva. Sete mecanismos soltos viraram um; `piso_repouso_mhz`
  saiu dos apps e das partes de fundo. `energia quem` no console e
  `sistema.recursos()` no Berry mostram quem usa o quê. O piso de 200 MHz do
  Terminal nunca tinha funcionado (o P4 só aceita 40/90/180/360): os pedidos
  agora são arredondados para uma frequência válida.
- **Câmera (`hal/tab5_camera`)**: ligar não escolhe mais resolução - o
  preview tem o tamanho do destino e cada quadro pedido o seu
  (`tab5_camera_pedir_quadro`, que substitui a API de foto e atende dois
  pedidos ao mesmo tempo). App Câmera, apps Berry, console e navegador usam a
  câmera juntos sem um recusar o outro. Quadro "como o sensor vê" (sem girar):
  girar no PPA custa ~117 ms por quadro.
- **Ajustes › Acesso pelo PC**: o servidor web num lugar só - endereço,
  senha, cada app com abrir/fechar e "Desligar tudo".
- **SSH é um comando, não um botão**: o app Terminal perdeu o botão "SSH" e a
  tela de conectar; `ssh usuario@host` abre a sessão e a tela troca sozinha
  para ela, voltando aos comandos (com o motivo na barra) quando ela termina.
  No navegador, a mesma regra. O comando roda numa parte de fundo que o
  kernel liga de qualquer tarefa (`sistema_fundo_pedir`).
- **Editor (modo código)**: a coluna dos números de linha tem a largura dos
  dígitos e encosta na borda, com um fio até o texto (antes, um vão fixo à
  esquerda do número).
- **Energia** (`docs/ENERGIA_2026-10-29.md`): o giroscópio só fica ligado
  enquanto alguém o lê (`tab5_imu_giroscopio`); monitor, cartão e ESP-NOW
  seguem o ritmo do repouso em vez de acordar todo segundo; a vigia do
  pendrive espaça para 5 s sem cabo; `sistema.repouso()` no Berry. A Música
  roda numa prioridade acima da interface e da câmera (`fundo_def_t.prioridade`).
- **Editor de Berry**: limite de 32 KB por script, e o que passa disso não abre
  (antes era cortado em 16 KB sem aviso e o corte era gravado); tamanhos em KB.
- **Energia** (o app, dentro do Ajustes) com o VOLTAR no topo.
- **Análise geral do sistema** (`docs/ANALISE_2026-10-28.md`): regra de
  serviço x app, duplicação, memória, tarefas e exposição à rede. Neste lote:
  a capa da Música passa pelo serviço de rede (sem cliente HTTP próprio); um
  formatador de tamanho só (`tab5_bytes_txt`); o cabeçalho do BMP no Arquivos
  é lido no worker (montar o pendrive na interface congelava a tela); a
  sessão do pendrive usa uma vaga fixa do pool em vez de criar 6 KB de SRAM.
- **Abertura de app sem animação** (o zoom a partir do ícone foi tirado).
- **Lógica mais simples por dentro (sem mudar o que se vê)**:
  - Ajustes viram uma tabela (chave, variável, faixa). Toda gravação na NVS
    vai por um caminho só, adiado 1 s, na tarefa do `esp_timer`. Saem as cinco
    bandeiras "pendente" e a lista à parte para quem tem pilha na PSRAM.
    `ajustes_gravar_agora()` antecipa a gravação no desligamento por bateria.
  - Travas: `Trava` (`tab5_mutex.hpp`) cria o mutex antes do `app_main`.
    Sai a criação "na primeira vez" com seção crítica, e sai também o
    `tab5_arquivos_iniciar()`.
  - Wi-Fi: um `s_modo` (parado, procurar, só rádio, conectar) substitui as
    bandeiras `s_descendo`, `s_varrendo` e `s_so_radio`, que eram ligadas em
    volta das chamadas. É o que o evento de queda consulta.
  - Vigia do Grove sai do laço da interface para `kernel/grove.cpp` (um
    timer e a sondagem no pool). O cálculo de quanto o laço dorme conta cada
    prazo uma vez só.
  - Contadores incrementados por mais de uma tarefa ou numa interrupção
    passam de `volatile` para `std::atomic`.
- **Teclado físico automático**: sai o ajuste "Ativar/Desativar" do Ajustes >
  Teclado (a página só mostra o estado). O sistema acha o teclado sozinho:
  com a tela acesa procura a cada 2 s, e um teclado encaixado com a tela
  apagada é achado na primeira tecla (pelo INT, G50), que acende a tela.
- **Com o teclado físico encaixado, sem teclado da tela**: qualquer campo
  (busca de nós do Mesh, senha do Wi-Fi, PIN, renomear no Arquivos, playlist
  na Música, apps Berry) recebe as teclas do físico e o teclado virtual não
  aparece nem trava a navegação. Encaixar ou tirar com o campo aberto troca na
  hora.
- **Com o teclado físico encaixado, tudo deitado**: a tela inicial (6 ícones
  por linha) e todos os apps ficam em paisagem, com o teclado embaixo; tirar
  o teclado volta cada um ao que era. A Câmera fica sempre em pé (o preview só
  existe assim) - nos apps Berry, `# orientacao: fixo` faz o mesmo.
- **Apps deitados**: Ajustes vira lista à esquerda (com "Brilho e volume" no
  topo) e a página ao lado, rolando sem levar a lista; a Música (Tocando) fica
  em duas colunas, a capa ou o espectro de um lado e os controles do outro; o
  IMU põe o nível ao lado das barras; Energia e Tarefas rolam a tela inteira
  (no Tarefas, todas as tarefas aparecem). Novo campo `altura_agora` no
  `app_t`: a altura de rolagem decidida pela área de agora.

### Corrigido
- **Piscada azul da tela resolvida**: a interrupção do DMA que religa o
  framebuffer atrasava e o painel passava um quadro sem imagem. Agora toda
  ISR do DMA nasce no nível 3 e no núcleo 1. Medido: segurar o núcleo 0 por
  20 ms não atrasa mais quadro. A home sem papel de parede é limpa já na cor
  do tema. Apps não pintam mais sobre as barras (recorte na área do app).
- **Toque mudo se recupera sozinho**: o GT911 que parava de responder
  deixava a tela em repouso sem acordar. O vigia o reinicia pelo TP_RST (tela
  acesa em 3 s, em repouso em 10 s, no máximo 1 vez a cada 30 s). Falha de
  leitura I2C devolve 0 pontos (antes repetia o último toque, um dedo
  fantasma).
- **Relógio pelo RTC**: o sono leve mede pelo RC interno (~5% de erro) e o
  RTC (cristal) corrige o relógio a cada 60 s (5 s de diferença em 100 s
  em repouso). NTP e Mesh só valem frescos (30 s); faixa plausível
  2025-2040 e gravação no RTC conferida (acabou o ano 2049 nos logs).
- **Pendrive**: escrita juntada em 64 KB, leitura antecipada e recuperação
  do cartão no HAL (arquivo grande travava o SDMMC e o Wi-Fi). Recusa na
  hora quando um app segura o cartão, com o motivo no log e no console.
- **"Desligado sozinho"**: o IDF usava semihosting a cada troca de tarefa
  (pânico na IDLE0), então `FREERTOS_WATCHPOINT_END_OF_STACK` foi desligado;
  `PANIC_SILENT_REBOOT` evita que o relatório na USB, sem ninguém lendo,
  segure o aparelho por horas (o core dump continua).
- **Download travando com a tela apagando**: a transferência agora declara
  REDE e PROCESSAMENTO; downloads de 4 MB e 16 MB terminam.
- **Desmontagem do C6 derrubava o sistema** (~1 a cada 100): a leitura SDIO
  estaciona antes do `esp_hosted_deinit` e o Wi-Fi é rearmado na remontagem.
- **Berry com prazo preciso**: o prazo é conferido a cada 8192 instruções
  (um fundo de 508 ms rodava 54 s antes do `tempo_esgotado`), e
  `berry verificar` lê o arquivo inteiro (acusava erro falso).
- **IMU, câmera e Grove**: IMU marca dado inválido se o chip não responde
  e se recupera; `tarefa_app_encerrar` não libera recursos de tarefa
  abandonada; Mesh solta o Grove se a UART falha; pilhas do Terminal (8 KB)
  e da sonda do Grove (6 KB) aumentadas; GPS segura o sono leve.
- **Música**: pior bloco de entrega 396 -> 46 ms. Vídeo: o relógio é
  contínuo entre as posições da Música (com som dava 1 quadro/s) e o próximo
  quadro é decodificado adiantado (3 buffers).
- **Travamento ao acordar a tela com a câmera em uso** (a do navegador
  derrubava o Tab5 sempre): o `esp_lcd_panel_del` do ESP-IDF apaga o canal do
  DMA da tela sem pará-lo, e com a câmera mantendo o DMA de pé a tela que
  acorda reusava o canal ainda rodando. `hal/tab5_dma.cpp` para o canal antes
  de apagar (`--wrap`), para todo usuário do DMA. Análise da suspensão e o
  modelo de recursos em `docs/SUSPENSAO_2026-11-05.md`.
- **Ajustes com as páginas trocadas** (tocar em Data e hora abria o Idioma, e
  assim até o fim da lista): o desenho e o toque das páginas eram tabelas à
  parte, e a linha da Energia só entrou na da lista. Agora é uma tabela só,
  com uma linha por página, e a compilação confere o tamanho.
- **Enter (e às vezes uma letra) que não entrava no editor das Notas e do
  Berry**: `tab5_utf8_corte` olhava um byte além do texto (lixo da pilha) e
  descartava a tecla quando ele parecia continuação de UTF-8. Ao ler uma nota,
  o mesmo erro podia cortar o último caractere. Detalhes em
  `docs/TECLADO_2026-11-04.md`.
- **Toque depois de uma pausa digitando**: com o teclado aberto, o toque é
  lido a cada 100 ms no máximo; antes, o driver do GT911 descartava o
  relatório do dedo descendo depois de 128 ms sem leitura.
- **O cochilo do Wi-Fi derrubava o servidor web** (3 min de tela apagada):
  o servidor pede a rede (`tab5_wifi_precisar`).
- **Estouro da pilha do servidor web** com o Mesh no navegador (Breakpoint no
  snprintf): os textos foram para a PSRAM, sem `%f`, pilha de 8 KB.
- **"sem cartão" na primeira lista do Editor** depois do boot (a lista não
  montava o cartão).
- **A música engasgava com a câmera aberta**: o decodificador ficava sem vez.
- **Brilho no mínimo que acordava forte**: a partida a frio do backlight
  (0 → 28 → alvo) escrevia direto no painel; agora termina pelo mesmo caminho
  da barra de Ajustes, um instante depois.
- **Toque que não acendia a tela bloqueada** (visto em 26/10, com teclado e
  C6L encaixados; a tecla acendia): em repouso, com o pino do GT911 preso no
  nível ativo, a interrupção do toque nunca voltava a ser ligada. Agora a
  próxima volta força a leitura, que solta o pino.
- **IMU deitado**: o nível gira junto com a tela (antes a bolha andava 90°
  fora do lado inclinado); as barras seguem mostrando os eixos do chip.
- **Música**: pausado, o tocador se encerra em 5 min (eram 15); sem nada para
  tocar (a lista acabou, parou), em 30 s - a pastilha não fica na barra.
- **O toque que abre um app virava toque nele**: o GT911 ainda diz
  "pressionado" por duas leituras depois da soltura, no lugar do ícone; o
  Ajustes recebia isso como arraste na coluna da esquerda e mexia no volume.
  Vale para todo app: o dedo que abriu o app é ignorado até sair da tela (a
  mesma regra do dedo que acende a tela).
- **Arquivos**: tocar num `.be` abre o script como texto (antes já instalava);
  instalar é o botão "Instalar" no alto. O botão da lixeira na linha do disco
  ficou discreto ("Lixeira", cinza); a confirmação diz quantos itens somem de
  vez e o "Esvaziar" só aceita toque 0,7 s depois de aparecer.
- **Energia deitada**: o gráfico 1 parava no meio (um ponto por coluna de
  pixel, num vetor da largura em pé, 720); agora vai até a borda (1280).
- **Tarefas deitado**: duas colunas - a tabela à esquerda com a altura toda;
  eventos, gráfico e memória à direita (fonte fixa a 1,6x), sem rolar a tela.
- **Mesh, Wi-Fi do nó**: a lista de redes guardadas mostrava só as que cabiam;
  agora é a lista do sistema e rola.
- **Tela de bloqueio**: a faixa da navegação ficava preta; a animação e o
  "Deslize para cima" ocupam a tela inteira abaixo da barra.
- **Procurar redes com o Wi-Fi conectado** (`wifi redes`, no console e no
  Terminal): derrubava a rede (desconectava de propósito antes de varrer) e,
  no Terminal, não mostrava nada (a lista era impressa pela tarefa do Wi-Fi).
  Agora é a mesma procura de Ajustes > Wi-Fi, que não solta a rede, e a lista
  sai na tarefa de quem digitou, com `*` nas redes guardadas. A varredura
  duplicada do console saiu do serviço de Wi-Fi.
- **Lista de redes vistas sem trava** (W2 da análise de 22/10): a procura
  montava a lista no lugar enquanto a tela de Ajustes lia; agora monta à parte
  e troca sob a trava das redes.
- **Música ao voltar para retrato**: a capa nascia do tamanho do quadrado
  deitado e era esticada em pé; agora, ao girar, ela é refeita do tamanho
  novo, e o espectro, a lista e o miniplayer recomeçam na geometria nova.
- **Azimutal e Radar ficam sempre em pé** (nem o teclado deita): o céu do
  Azimutal passava da navegação deitado, e o radar é desenhado com a frente
  do aparelho no alto da tela.
- **Parte de fundo com aviso adiado** (`fundo_def_t.aviso_ms`): a pastilha só
  aparece se o fundo passar do prazo - para trabalho curto e frequente.
- **Pool de tarefas**: a última vaga passou de 4 para 6 KB (+2 KB). Com o C6L
  no Grove, o Mesh fica com a vaga de 6 KB o tempo todo, e quem pedia 6 KB
  caía numa tarefa criada e destruída a cada vez (fragmenta a RAM interna).
- **Memória**: a pilha da interface ganhou 2 KB (8 → 10 KB; medido no
  aparelho: pico de ~4 KB, sobram ~6 KB); a cópia do
  despejo de configuração do Mesh (~7,5 KB, só do console) saiu da RAM
  interna para a PSRAM.
- **Ajustes deitado, novo arraste**: a página pulava de volta para cima no
  toque seguinte (a âncora do dedo não era solta quando ele saía fora da
  coluna da página); agora cada coluna solta a sua.
- **Ajustes deitado, a coluna da página**: toda página rola do mesmo jeito,
  com a barrinha das listas. No Wi-Fi e no Bluetooth a lista de redes e
  aparelhos começava abaixo do fim da tela (invisível) e a página não rolava;
  agora ela entra inteira na página, e a altura é refeita quando a lista muda.
- **App Tarefas deitado pintava por cima das barras**: a lista
  (`kernel/lista.cpp`) trocava o recorte do sistema pelo dela; com a tela
  inteira rolando, a tabela subia por cima da barra de status e da lateral.
  Agora a lista desenha só dentro do recorte que recebe (vale para todas as
  listas).
- **Teclado segurando o aparelho acordado no repouso**: o INT do teclado
  (G50) era armado para tirar o chip do sono leve mesmo sem teclado lido. Com
  o teclado encaixado e o ajuste antigo desligado, uma tecla deixava o pino
  em baixo para sempre e todo sono leve terminava na hora. Agora o G50 só
  acorda o chip enquanto está em cima.
- **Troca de rede Wi-Fi contada como falha**: a queda da rede antiga chegava
  depois da troca e virava "tentativa", com uma reconexão 1 s depois por cima
  da conexão nova. A queda de outra rede que não a alvo é ignorada.
- **Pânico raro no boot (heap da PSRAM)**: em cerca de 1 a cada 65 boots o
  cabeçalho do heap da PSRAM aparecia estragado e o `M5.begin()` caía ao
  alocar o quadro da tela. A causa continua em aberto; agora o boot confere o
  cabeçalho antes do `M5.begin()` e, se estiver estragado, reinicia limpo
  (`esp_restart`, até 3 vezes seguidas) em vez de mostrar o pânico. O log do
  cartão registra quando isso acontece.
  A conferência agora também acontece **dentro** do `M5.begin()`: o estrago
  aparece entre o primeiro e o segundo quadro de 1,8 MB que a tela reserva,
  e toda reserva grande na PSRAM confere o cabeçalho antes (com o mesmo
  reinício limpo).

### Diagnóstico
- Console: `toque reiniciar` e `toque regs` (raio-x do GT911), `tela
  atrasar <ms> [crítica] [núcleo] [vezes]` e `tela irqs`, `wifi
  ligar/desligar`, `c6 laço N`. A tela e o monitor mostram o PC do último
  quadro atrasado; `estado` mostra tela bloqueada.
- O boot grava o motivo cru de reinício do chip e o travamento diz se o
  core dump é deste firmware. A sentinela do heap arma o watchpoint direto
  no hardware (o boot entrava em laço de pânico depois do OpenOCD).
- A sentinela do heap da PSRAM (`main/tab5_heap_psram.cpp`, junto com a
  guarda que saiu do `main.cpp`) agora vigia `heap_data` e `lock` nos **dois
  núcleos**, armada logo depois de o IDF montar o heap (antes dos construtores
  estáticos). Se uma instrução estragar o cabeçalho, o pânico "Breakpoint"
  traz o PC dela no core dump.
- Flagrante do cabeçalho: estragado, o console mostra a linha de cache dele
  como a CPU a vê e de novo depois de invalidá-la (o que a PSRAM guarda).
  Console `mem estresse [n] [linha]` repete o mecanismo (linha da PSRAM +
  1,8 MB zerados a cada volta) para medir.

## [0.8.0] - 2026-10-21

### Adicionado
- **Remover app Berry pela tela inicial**: segurar o ícone e arrastar até a
  faixa "Remover" no topo (só aparece para apps Berry); depois de confirmar,
  o `.be` e os dados do app vão para a lixeira do cartão. O app Arquivos
  também relê os apps ao apagar, renomear ou copiar - o ícone aparece ou some
  na hora. Novos: `sistema_apps_de_fora_removivel/remover` (o ponto de
  extensão dos apps de fora) e `tab5_sd_pasta_caminho` (o caminho sem criar).
- **O firmware traz um app Berry de exemplo**: o `Ola.be`, refeito como um
  passeio por abas (hora e rede, botões/bip/aviso/dado salvo, acelerômetro,
  desenho com o dedo, discos), vai embutido e é gravado em `apps/Berry/` no
  primeiro boot com o cartão.
- **exFAT no pendrive e no cartão**: pendrives de 64 GB para cima e cartões
  SDXC vêm de fábrica em exFAT e agora montam sem reformatar. O componente
  `fatfs` do IDF foi trazido para `components/fatfs` com `FF_FS_EXFAT 1` (no
  IDF ele é fixo em 0); ver `components/fatfs/ORIGEM.txt`. Continuam fora:
  NTFS, os formatos do Mac (APFS/HFS+) e tabela de partição GPT.
- **Módulo Berry `arquivos`**: o script enxerga o cartão inteiro e o
  pendrive (`volumes`, `lista`), mas só altera a própria pasta
  (`apps/Berry/<app>/`): de fora, ele só **copia para dentro** (`copiar`, em
  segundo plano, com `estado`/`cancelar` e `# avisos: arquivos`); dentro, lê,
  apaga (para a lixeira), renomeia e cria pasta. Exemplo:
  `berry_exemplos/Importar.be`. `cartao.lista` passou a usar o serviço.
- **App Arquivos refeito** em cima do motor de arquivos: a raiz mostra os
  **discos** (Cartão SD e, plugado, o Pendrive) com espaço livre, lixeira
  (**Esvaziar**) e **Ejetar**; dentro deles, qualquer pasta - inclusive a
  Música e o pendrive inteiro, que antes não apareciam. **Toque longo
  seleciona** (depois o toque marca/desmarca, **Todos**), com **Copiar,
  Recortar, Apagar** (para a lixeira, com confirmação) e **Renomear**;
  **Colar aqui / Mover aqui** em qualquer pasta gravável; **+ Pasta**.
  **Abrir com** pelo tipo: música toca no tocador do sistema e abre a Música,
  `.be` instala o app Berry (variante berry), imagem (BMP, **JPEG e PNG**
  agora) e texto abrem no próprio app. **Para Música**: numa pasta com
  músicas fora da pasta de música (o pendrive), copia a pasta inteira - ou,
  na raiz do disco, as músicas soltas - para a Música do cartão. O progresso
  da cópia aparece no rodapé (com Cancelar) e na barra; sem cartão o app
  abre assim mesmo (o pendrive pode estar lá). `sistema_abrir_app(nome)`
  (kernel) abre outro app pelo nome.
- **Motor de arquivos do sistema** (`services/tab5_arquivos`) - a base do
  app Arquivos novo, que vai mostrar o cartão inteiro e o pendrive. Lista
  qualquer pasta dos dois discos, cria pasta, renomeia, apaga **para a
  lixeira** do disco (`.Lixeira`, esvaziada à parte) e **copia ou move em
  segundo plano**: blocos de 64 KB, progresso na barra (`Copiando 45%`),
  continua com o app fechado e a tela apagada, confere o espaço antes, nome
  repetido vira `x (2).mp3`, e nunca deixa arquivo pela metade (pendrive
  arrancado, cancelado ou disco cheio). Área de transferência do sistema
  (Copiar/Recortar/Colar). `/sdcard/apps` é só leitura (menos `apps/Berry`);
  as senhas do sistema e os nomes com `.` não aparecem. Console: `arquivos`.
- **Ejetar o pendrive** (`usb ejetar`, `tab5_volume_ejetar`): espera quem
  usa soltar, desmonta e avisa "Pode tirar o pendrive". O aviso do pendrive
  na barra agora abre o **Arquivos** (antes, a Música).
- **O C6 dorme com o Wi-Fi associado** (firmware `m5tabos-c6-5`, por OTA:
  `receber Sistema tab5_c6.bin` + `c6 gravar sim`). Com a tela apagada o
  enlace SDIO desmonta e o C6 entra em light sleep: medido, 71 s dormindo de
  ~80 s, volta em 0,4-0,6 s sem resetar o C6 (associação, IP e conexões
  continuam). O C6 responde o ARP do P4 e descarta broadcast; volta sozinho
  com tráfego para o P4 (pulso no G14), e o P4 o chama pela linha CMD do SDIO
  quando transmite ou faz RPC. Modo automático `c6 sono auto ligar` (ajuste
  `c6_sono`), **desligado de fábrica** até a medida na bateria; com ele o
  ESP-NOW só ouve com a tela acesa e o Bluetooth precisa estar desligado.
  Console: `c6 dormir|acordar|sono|travas|janela`. Ver docs/RADIO_C6.md.
- Achado no caminho: com o ESP-NOW iniciado o rádio do C6 **nunca** dormia
  entre beacons (a janela padrão do IDF é "sempre ouvindo") - o modem sleep
  máximo do repouso não tinha efeito. `C6_AJ_ESPNOW_JANELA` ajusta a janela.
- **Pendrive no USB-A.** A porta USB-A virou host (o controlador de alta
  velocidade do P4): espetou um pendrive FAT/FAT32, ele monta sozinho em
  `/usb`, aparece na barra ("Pendrive <nome>", que abre a Música) e no topo
  da lista de Pastas do app **Música** - toca direto dele, com as pastas
  dele, sem copiar. Tirar no meio para a música e volta a lista à raiz.
  Novo serviço `services/tab5_usb` (ajuste `usb_a`, nasce ligado: os 5V do
  conector entram no perfil de energia) e `services/tab5_volumes` (cartão e
  pendrive por caminho absoluto, `AVISO_VOLUMES` quando um entra ou sai).
  exFAT e NTFS não montam (aviso "use FAT32").
- Música: caminhos absolutos (`/usb/...`) valem em todo o serviço - fila,
  playlists e Favoritas misturam cartão e pendrive; a capa de uma música do
  pendrive fica guardada no cartão. Tocando só do pendrive, o cartão fica
  livre (pode ir para o computador sem parar o som).
- Berry: `musica.pendrive()`; `musica.lista("/usb")`/`tocar("/usb", i)`;
  `# avisos: volumes`.
- Console: `usb` (o aparelho, a velocidade negociada, o pendrive),
  `usb ler <arquivo>` (mede KB/s), `usb espaco`, `usb ligar|desligar`.
- **Música: tela nova.** Duas telas no app: **Tocando** (a capa grande, o
  título, o artista na cor de destaque, o coração, a linha do tempo - tocar
  no tempo da direita mostra quanto falta -, aleatório, anterior,
  tocar/pausar, próxima, repetir, volume, pôr numa playlist e parar; tocar na
  capa troca para o espectro, e o fundo da tela pega a cor da capa) e
  **Biblioteca** (abas Pastas e Playlists, a duração de cada música na coluna
  da direita, o "+" que põe numa playlist e o miniplayer embaixo).
- **Música: playlists** (`.m3u` em `Music/Playlists`, o formato que qualquer
  tocador do computador abre): criar (teclado virtual ou físico), pôr e
  tirar músicas, tocar, tocar aleatório, apagar. O coração da tela Tocando
  põe/tira da playlist **Favoritas**.
- **Música: aleatório e repetir** (não / a fila toda / a mesma música), do
  serviço: valem para qualquer fila e ficam guardados. Desligar o aleatório
  no meio não corta o som; repetir tudo com aleatório embaralha de novo a
  cada volta.
- **Música: capa do álbum.** O serviço procura a imagem da etiqueta ID3, uma
  cover/folder/capa.jpg na pasta e, com o Wi-Fi conectado, a busca do iTunes
  por artista e título (só com artista, e só se o artista devolvido bater).
  Fica em `Music/.capas/`: a internet é consultada uma vez por música.
- **Música: título e artista do nome do arquivo** quando não há etiqueta:
  "Artista - Título (Official Video) [HD].mp3" vira "Título" de "Artista".
- **Música: duração sem tocar** (`tab5_musica_duracao`: cabeçalho Xing/VBRI
  ou bitrate e tamanho; WAV pelo cabeçalho), lembrada pelo serviço.
- Berry: `musica.aleatorio`, `repetir`, `playlists`, `playlist`,
  `playlist_criar/apagar/por/tirar/tocar`, `capa`, `duracao`; `lista()` e
  `estado()` trazem o `caminho` (e `album`, `playlist`, `aleatorio`,
  `repetir`).
- Console: `musica aleatorio|repetir|playlists|playlist|capa|duracao` e
  `travamento`.
- **Core dump na flash** (partição `coredump`, 1 MB no fim): um panic ou
  watchdog deixa o retrato gravado, e o boot seguinte anota no log do cartão
  a tarefa, o PC, o RA e os endereços da pilha (`travamento` no console
  mostra de novo). Um travamento longe do computador deixa de ser só "o log
  acabou ali".
- **Guarda da bateria** (`services/tab5_bateria`, todas as variantes): aviso
  na central em 20% (atenção), 10% e 5% (urgente; tocar abre o Energia) e,
  em 0% com a tensão confirmando por 30 s, o sistema grava o que está
  pendente no cartão e **desliga** - antes ia até o brownout (3,29 V por
  célula, descarga profunda) e entrava em laço de reinício. Ligar com a
  bateria vazia desliga de novo. Cada desligamento fica em
  `Sistema/desligou_bateria.csv`; o `energia` do console mostra a guarda.
- `energia ensaio comecar [min]` no console: na bateria e com a tela apagada,
  desliga um serviço por vez (Mesh pela internet, 5V do Grove, Bluetooth,
  Wi-Fi) e mede quanto cada um custa; no fim devolve os ajustes. Resultado em
  `energia ensaio` e `Sistema/ensaio_energia.csv`.
- **Mesh: busca na página de nós.** No topo da tabela, uma caixa de pesquisa
  filtra os nós a cada letra pelo nome longo, curto ou `!id` (sem diferença
  de maiúsculas nem de acento: "jose" acha "José"), com a contagem "12 de 87"
  e o "x" para limpar. Toque abre o teclado virtual; o teclado físico escreve
  direto. O filtro é do serviço (`tab5_mesh_num_nos_busca`/`tab5_mesh_no_busca`).
- **Mesh: servidores MQTT da pessoa**, no lugar da lista fixa no código.
  Em Ajustes > Malha pela internet e em Mesh > Nó > Internet do Tab5: até 8
  servidores salvos (nome, endereço, usuário, senha cifrada, raiz), cada um
  com EDITAR/APAGAR, e "+ adicionar" com modelos (Mesh Brasil, Meshtastic
  Brasil/Europa/Austrália, em branco). A escolha antiga e o "servidor
  próprio" passam sozinhos para a lista no primeiro boot.

- Console: `mesh` mostra o MQTT do nó (ligado, proxy, servidor, raiz), o
  sobe/desce e o tipo de chave de cada canal e o Wi-Fi do nó; `mesh no`
  despeja a configuração crua do nó campo a campo.

### Removido
- **Mesh: a rede americana** (o segundo cliente MQTT em `msh/US`, a conversa
  "LongFast EUA", os botões BRASIL/EUA e o ajuste `mesh_eua`). Os nós dos EUA
  guardados na NodeDB do cartão não voltam a ser carregados.

### Mudado
- **A variante m5burner agora é uma cópia da berry**: apps do sistema +
  Berry, com o `Ola.be` de exemplo. Gravada pelo M5Burner, começa com os
  ajustes de fábrica.
- **Câmera, Trilha, Azimutal e Exemplo não abrem mais pasta sozinhos**: o
  "próximo número de foto/trilha", a lista de dias do Azimutal, o histórico
  da Trilha e a contagem do Exemplo passam pelo serviço de arquivos
  (`tab5_arq_maior_numero`, `tab5_arq_percorrer` - sem limite de itens, sem
  ler tamanho). Mesmo comportamento; agora o que o gravador do cartão ainda
  segura também conta.
- **Segurança: senhas com chave sorteada.** As senhas do `wifi.txt` e dos
  servidores MQTT eram cifradas com uma chave tirada do MAC do chip - que não
  é segredo (o USB entrega o MAC como número de série) e, mesmo sem ele, cabe
  em 2^24 tentativas: quem tivesse só o cartão lia a senha. Agora a chave são
  16 bytes sorteados no primeiro boot e guardados na NVS (`seg/chave`); o que
  estava na cifra antiga é regravado na nova sozinho no boot seguinte.
- **Segurança: Berry não abre arquivo.** O `open()`, o `load("arquivo")` e o
  `save()` do Berry abriam qualquer caminho - o cartão inteiro e o pendrive,
  por cima da caixa de areia do módulo `cartao`. Agora só o sistema abre
  arquivo, ao carregar o `.be`; o script usa o módulo `cartao`.
- **Capa pela internet virou opção** (Ajustes > Wi-Fi, "Capas de música";
  console `musica capa internet ligar|desligar`). Ligada de fábrica; desligada,
  título e artista não saem do aparelho.
- O log do cartão não guarda mais o texto das mensagens ESP-NOW nem as
  coordenadas de casa.
- **Ligar um serviço, um lugar só:** `sistema_wifi`, `sistema_grove`,
  `sistema_teclado`, `sistema_mesh_via` e `sistema_mesh_reiniciar` (app.h),
  no contrato de `sistema_bluetooth` - gravam a escolha e aplicam. O app
  Ajustes, o app Mesh e o ensaio de energia faziam os passos e as
  dependências (a via Bluetooth do Mesh liga o Bluetooth) cada um por conta
  própria; o reinício do Mesh estava copiado em dois apps.
- **USB-A no repouso:** com a tela apagada e ninguém lendo do pendrive (a
  música tocando dele segura), os 5V do conector caem - o pendrive parado não
  segura mais o sono leve a noite toda. Ao acender, voltam e o pendrive monta
  de novo (~1 s). Um aparelho que não é disco (fone carregando) fica com
  energia. O conector não tem pino de "algo espetado": quem vê é o próprio
  controlador USB, e só com os 5V ligados.
- `tab5_passou_s` (services/tab5_hora.hpp) no lugar de três cópias iguais.
- **Nomes de arquivo em UTF-8** (`CONFIG_FATFS_API_ENCODING_UTF_8`). Antes o
  FAT entregava CP437, e nome com letra que o CP437 não tem ("Canção.mp3")
  aparecia e abria pelo nome curto (`CANCAO~1.MP3`). Playlists gravadas antes
  (caminhos em CP437) continuam valendo: são convertidas ao ler.
- Música: o app não exige mais o cartão (`exige_sd = false`) - com o cartão
  no computador ainda toca o pendrive do USB-A.
- Avisos: até 12 fontes (eram 8 e já não cabiam todas).
- **O P4 dorme em repouso** (light sleep entre eventos: tickless ligado no
  sdkconfig, sono só com a tela desmontada). O toque (G23) e o teclado (G50)
  viram despertar por nível no sono e voltam a borda ao acordar. Antes, em
  repouso, o P4 ficava 97,8% ocioso acordado a 40 MHz. `energia sono [on|off]`
  no console (ajuste na NVS, ligado de fábrica).
  **Com cabo ligado não dorme** (corrente acima de −8 mA = carregando ou
  cheia na tomada): dormindo, o USB não respondia e o Mac ficava sem a porta
  serial até tocar a tela. E se o cabo chega com o chip dormindo, o console
  se reanuncia ao computador (o D+ cai 150 ms e volta), sem precisar recolocar
  o cabo.
- **Contrato de economia** (`kernel/app.h`): `sistema_economia()`,
  `sistema_ritmo_ms(acordado, repouso)` e `sistema_economia_segurar()`. Em
  repouso o kernel mede a cada 5 s (era 1), confere o beep a cada 5 s e o cabo
  do Grove a cada 10; guarda, cochilo e ensaio contam o tempo real. O Wi-Fi
  conectado segura o sono. Doc em `docs/ESCREVENDO_APPS.md` (seção 4).
- **Wi-Fi com potência variável, como nos celulares** (nunca desconecta por
  padrão):
  - tela acesa **com tráfego**: rádio sem power save (máxima capacidade);
    2 s sem pacote, volta ao modem sleep mínimo. Quem conta é o transporte do
    esp_hosted (`CONFIG_ESP_HOSTED_HOST_PKT_STATS`); a tarefa do Wi-Fi olha a
    cada 250 ms com a tela acesa (RSSI segue a cada 5 s);
  - tela apagada: **conectado econômico** - modem sleep máximo (listen 10) e
    o P4 dormindo com batida de 100 ms; depois de 30 min, batida de 300 ms
    ("doze"). Medido com o Mesh pela internet: 57 mA (era 72), sem perder
    pacote (0,167/s dormindo contra 0,164/s acordado);
  - **economia máxima** (opção em Ajustes > Wi-Fi, desligada de fábrica): com
    a tela apagada há 3 min e nada precisando da rede, o Wi-Fi desliga - ~20
    mA - e volta ao acender ou 1 min a cada 15. Com o Mesh pela internet ativo
    não desliga (o MQTT não guarda nada: uma noite de mensagens perdidas no
    teste).
  Vale com o C6 de fábrica (só `esp_wifi_set_ps` de sempre).
- **Wi-Fi em repouso gasta menos**: o ensaio mediu 74 mA com a tela apagada,
  28 deles do Wi-Fi conectado (Mesh internet, Grove sem nada e Bluetooth
  desligado: ~0). Com a tela apagada o C6 passa a modem sleep máximo
  (acorda a cada 10 beacons, ~1 s) e volta ao mínimo ao acender; o ESP-NOW
  conversando ainda mantém o receptor ligado.
- **C6 com clock variável** (firmware do C6, `CONFIG_PM_ENABLE` + DFS 40-160
  MHz, sem light sleep por causa do SDIO escravo): o piso com o Wi-Fi
  desligado caiu de 44,8 para 37,8 mA. Com o Wi-Fi conectado o ganho ainda
  não apareceu (72 mA); o `wifi` do console agora mostra o modo de economia
  que o C6 diz estar usando, para achar o porquê.

### Corrigido
- **Reinício por watchdog de interrupção (INT_WDT)** - provavelmente os de
  17/10 com a Música tocando: com o clock variando de 40 a 360 MHz, cada
  troca que cruza os 200 MHz da PSRAM faz o IDF parar o outro núcleo para
  trocar a temporização dela; se o outro núcleo estava numa interrupção
  também trocando a frequência, os dois se esperavam para sempre (os dois
  núcleos em `do_switch` no core dump). Agora a faixa do clock automático
  nunca cruza a velocidade da PSRAM (`faixa_segura`, `hal/tab5_power.cpp`):
  acima dela o clock fica fixo no teto (com a tela acesa, 360 MHz); o
  repouso a 40 MHz não muda. Custo de bateria com a tela acesa ainda não
  medido.
- **Pânico no boot dentro do `M5.begin()`** (falha ao alocar o quadro da
  tela, `Load/Store access fault` em `tlsf_*`), de vez em quando e mais ao
  reiniciar pelo cabo: o heap da PSRAM começava colado na última variável
  estática dela, na mesma linha de cache, e se estragava. A taxa dependia só
  do leiaute do binário (de 0 a 65% dos boots). Agora uma guarda de 128 bytes
  alinhada (seção `noinit` da PSRAM, `main/main.cpp`) faz o heap começar numa
  linha de cache só dele: 0 travamentos em 36 boots no build que mais travava.
  **Mitigação, não a causa:** um build seguinte voltou a cair todo boot com a
  guarda no lugar (o cabeçalho do heap com a vtable de `FonteRapida`, da
  PSRAM estática), e sumiu ao mudar só o código na flash. Causa em aberto;
  um watchpoint no cabeçalho do heap (`main/main.cpp`) fica de sentinela.
- **Wi-Fi: senha estragada por um firmware antigo.** Uma versão anterior à
  chave sorteada lê o formato novo (`k2…`) como texto puro, corta em 64
  caracteres e regrava o `wifi.txt` - o roteador passa a recusar (reason
  15). O sistema agora reconhece isso e recupera a senha quando ela tem até
  15 caracteres (o byte cortado sai por tentativa); maior, a rede sai do
  arquivo e o log pede para digitar de novo.
- **Berry: três buracos da VM fechados** (docs/ANALISE_2026-10-21.md, B1-B3).
  (1) Um `try ... except ..` num laço engolia o `tempo_esgotado` e o laço
  seguia para sempre, com a tela parada esperando: o erro de tempo agora
  atravessa qualquer `try` e só para no sistema. (2) Um script conseguia ler e
  escrever qualquer endereço da memória (`introspect.toptr` + `bytes(ptr, n)`,
  ou trocando o `.len` de um `bytes`): o módulo `introspect` saiu, o `bytes`
  mapeado é recusado e atributo que começa com `.` não aceita atribuição
  (patches marcados "M5tabOS" no Berry, lista em `berry/ORIGEM.txt`).
  (3) `input()` travava a tarefa do Berry esperando o console: agora devolve
  `""` na hora.
- Ajuste pedido de uma tarefa com a pilha na PSRAM (um app Berry) não se
  perde mais: ia para o log como erro e voltava ao valor antigo no boot
  seguinte; agora segue pelo caminho adiado da NVS.
- Ensaio de energia interrompido (reinício, bateria no fim) deixava Wi-Fi,
  Bluetooth, Grove e Mesh desligados de vez: os originais ficam na NVS e
  voltam no boot seguinte.
- USB-A: a contagem de quem segura o pendrive era zerada à força quando ele
  saía com alguém lendo - o soltar atrasado desse descontava a referência do
  pendrive seguinte. E o descontar ficou atômico.
- **Bateria parada em 92-99% com ela cheia.** O carregador (IP2326) encerra
  a carga em ~200-220 mA, a 8,36 V, e corta de uma vez; o estimador só
  reconhecia "cheia" abaixo de 100 mA e depois não deixava o número subir sem
  carga. Agora a queda da cauda da carga para zero, com o cabo ainda ligado,
  é o fim de carga (100%). Conferido nas 5 cargas do energia.csv de 17/10.
- **Música com a tela apagada tocava a 40 MHz** quando começava com a tela
  já apagada (console, Berry, central): o piso de 180 MHz da parte de fundo
  só valia no repouso seguinte, e o MP3 levava ~390 ms para decodificar
  418 ms de som - a fila do alto-falante secava a cada bloco. Agora o teto
  sobe na hora (`tab5_power_repouso_subir_teto`).
- **Toque que acorda: interrupção sem fim.** O pino do toque virava
  interrupção por nível antes de a ISR saber que estava armada; com o nível
  já ativo (dedo na tela enquanto ela apagava) ela disparava sem se desligar,
  sem fim, no núcleo da interface. Armado primeiro, e um disparo à toa é
  religado no laço do repouso (antes o toque seguinte só acordava no prazo
  do laço). Suspeito de um dos dois reinícios por watchdog com música
  tocando - sem backtrace não dá para cravar; o core dump acima é para isso.
- **Prioridade da tela no AXI** deixou de ser reescrita a cada acordar com
  os registradores já certos (com música, o barramento está ocupado com o
  cartão e o alto-falante).
- **Mesh pela internet sem nó: todo Tab5 tinha o mesmo ID.** Sem nunca ter
  visto um nó, o número ficava 0: o cliente MQTT entrava como
  `tab5-00000000` e as mensagens saíam de `!00000000` - todo firmware do
  M5Burner usado só pela internet se derrubava no servidor e parecia um só.
  Agora o número vem do MAC do Tab5 (a mesma conta do Meshtastic), até um nó
  de verdade ensinar o dele.
- **Mesh pelo Grove perdia mensagens com a tela apagada.** O sono leve do P4
  (novo nesta versão) desligava a recepção da UART entre eventos: o que o nó
  mandava nesse intervalo sumia - mensagens da internet que nunca chegavam e
  quadros cortados virando nós de número absurdo na NodeDB. Com o nó no
  Grove, o serviço segura o sono leve (`ECONOMIA_SEGURA_MESH`).
- **Watchdog da interface a cada 10 s em repouso**: a espera de 5 s do ritmo
  de economia batia no prazo de 5 s do watchdog. Prazo agora de 10 s, e a
  espera da interface nunca passa de 2 s abaixo dele.
- **% da bateria no boot com o cabo**: ligar ou gravar com o carregador ligado
  partia da tensão, que carregando lê muito acima da carga real (46% de
  verdade aparecia como 100%), e a contagem levava horas para corrigir. Agora
  o reinício pela USB continua da RTC (antes só reinício por software), a
  curva não veta um valor menor quando carregando, e a carga fica guardada na
  NVS (a cada 2 pontos e ao desligar) para o boot com cabo depois de desligado.
- App **Música** (do sistema, em todas as variantes): toca MP3 e WAV da pasta
  de música do cartão SD, com subpastas. Vale qualquer pasta na raiz cujo
  nome comece por "music", sem diferença de acento ou maiúscula (`Music`,
  `Música`, `Musicas`); com mais de uma, a que tem mais coisa. Sem nenhuma,
  o app cria `Music`. Tocar uma música toca a pasta dela em
  ordem; tocar/pausar, anterior/próxima, parar, linha de progresso que busca
  e volume (o mesmo de Ajustes). Continua tocando com o app fechado (parte de
  fundo); título e artista da etiqueta ID3 quando houver. MP3 pelo minimp3
  (CC0, componente `components/minimp3`), WAV PCM 8/16 bits. Para ao
  emprestar o cartão ao computador.
- **Tocador do sistema** (`services/tab5_musica`): a música é serviço, e o
  app Música é só a interface. Comando `musica` no console (estado, `lista`,
  `tocar <n> [subpasta]`, pausar/próxima/anterior/parar; mostra se a fila do
  alto-falante secou - o que se ouve como engasgo).
- Berry: módulo **`musica`** - o mesmo tocador do sistema para scripts
  (pasta, lista, tocar, controles, estado, espectro, volume, cor), e o
  exemplo `Tocador.be`, um player completo em ~200 linhas sem nada de áudio.
  `tela.cortar(txt, largura [, tam])` corta texto longo com "...".
- Central de avisos em três faixas: os **avisos** (o que não cabe rola,
  arrastando), **Em segundo plano** (um cartão por parte de fundo, com o X
  que para) e o **miniplayer** (com o X dele). O X vira "Parar?" no primeiro
  toque e para no segundo. Contrato novo em `kernel/fundo.h`: toda parte de
  fundo pode ser parada pelo usuário, e o app salva o que precisa no
  `ao_parar()`. A parada pedida pela central ou pelo console não congela
  mais a tela esperando o loop sair.
- Música: pausada por 15 minutos, o tocador se encerra sozinho.
- Música: **espectro** no alto do painel - 32 barras em segmentos (graves à
  esquerda) que sobem com a música, com o pico caindo devagar. FFT de 1024
  pontos na parte de fundo, só com a janela aberta e a tela acesa (~10% de
  um núcleo; fechado, nada); cada quadro leva a hora em que sai no
  alto-falante, então a barra bate com o que se ouve.
  Cinco temas de cor (arco-íris, o padrão; lilás; fogo; oceano; neon):
  tocar no espectro troca, e a escolha fica guardada nos ajustes. A cor de
  destaque do tocador segue o tema (coral, lilás, laranja, ciano, rosa): o
  painel, a música marcada na lista, a pastilha da barra e o miniplayer
  (`midia_definir_cor` em `kernel/midia.h`).
- Console: `deslizar x0 y0 x1 y1` (arraste simulado: desbloqueia, rola e
  fecha a central), `app exemplo fundo` (liga e desliga o cronômetro em
  fundo) e `despejar -b` (arquivo binário em base64; o `ferramentas/baixar.py`
  já o usa para .bmp/.png/.bin e espera o boot assentar antes de pedir).
- Console: o nome do app ignora acento (`abrir musica` acha "Música" - a
  serial só passa ASCII). O log de montagem do cartão diz a
  frequência e a largura do barramento negociadas.
- Barra: pastilha **tocando agora** (lilás, nota + título) ao lado dos
  avisos - a música não esconde aviso nem o contrário. A central de avisos
  (tocar a barra) ganhou o **miniplayer** no rodapé: título, anterior,
  tocar/pausar, próxima e a linha de progresso.
- Kernel: som em fluxo (`sistema_som_fluxo_*`, para tocar em blocos de
  qualquer tarefa sem quebrar a regra do I2C na interface) e
  `kernel/midia.h` (o que está tocando, para o sistema mostrar e controlar).
- Berry: **parte de fundo** - `sistema.fundo(true [, ms])` faz o sistema
  chamar `fundo()` do script com a janela fechada (pastilha na barra, como a
  Trilha e o Azimutal). Mesma máquina da janela; roda na tarefa do Berry de
  sempre, sem tarefa nova. Exemplo `Registro.be` (bateria, corrente e GPS
  gravados no cartão em segundo plano).
- Berry: orçamento de memória (4 MB para todas as máquinas juntas: passou,
  `memory_error` no script), `sistema.memoria()` e `berry mem` no console.
- Berry: módulo `gnss` (os satélites do GPS: `gnss.satelites()`, a hora
  UTC do receptor) e o exemplo `Azimutal.be` - céu com rastros, lista de
  satélites e gravação em segundo plano no mesmo CSV do Azimutal em C.
- Console: `abrir` aceita nome com espaço (`abrir Camera Berry`).
- ESP-NOW: botão **Aa** na barra aumenta a letra da conversa (4 tamanhos,
  guardado nos ajustes); o componente de conversa ganhou `conversa_escala`.
  O `EspNow.be` ganhou o mesmo botão.
- Berry: módulo `camera` (preview ao vivo direto na tela, foto 1200×1600 na
  pasta do app) e o exemplo `Camera.be`; `mesh` lê as mensagens recentes e os
  nós (`mesh.msg`, `mesh.no`...); cabeçalho `# orientacao:` (em pé, deitado
  ou girar). Ver `docs/BERRY.md`.
- App Mesh, página Nó: grupo **Internet do Tab5 (MQTT)**, que funciona sem
  nó nenhum conectado. Liga e desliga a internet quando faltar o nó e escolhe
  o servidor MQTT, inclusive um servidor próprio (endereço, usuário, senha e
  raiz). É o mesmo ajuste de Ajustes > Malha pela internet. Vale para todas as
  variantes.
- Variantes: o mesmo código gera mais de um firmware (`docs/VARIANTES.md`). A
  **completa** (padrão, `./gravar.sh`) e a **berry** (`./gravar.sh berry`),
  com apps em Berry instalados copiando um `.be` para o cartão, sem compilar
  (`docs/BERRY.md`). O núcleo é um só: as variantes só mudam o que entra no
  binário. `ferramentas/compilar_tudo.sh` compila todas; a versão gravada diz
  qual é (`0.8.0-berry`).
- Apps de fora do binário: `sistema_apps_de_fora_*()` em `kernel/app.h`, o
  ponto por onde os apps do cartão entram (a variante berry usa).
- Variante berry: a API dos apps Berry (tela, sistema, teclado virtual do
  sistema, cartão, ESP-NOW, Mesh, camadas, `sistema.acumular`, avisos), o
  verificador `testes/checar_be.sh` (compila e roda o script no Mac), os
  exemplos `berry_exemplos/` (Mostruário, ESP-NOW, Bench) e o comando
  `berry` do console (`berry tempos` mede um app por dentro).
- Versionamento: `ferramentas/lancar.sh patch|minor|major` lança uma versão
  (version.txt, seção no CHANGELOG, commit e tag), com as regras em
  `docs/VERSIONAMENTO.md`. O número no binário diz de onde o build saiu
  (`0.7.0+66.16a4d6d` = 66 commits depois da 0.7.0), e o build para se o
  `version.txt` ficar abaixo da última versão lançada.
- Mesh > Nó > Localização: compartilhar a localização liga e desliga (é a
  precisão da posição no canal principal do nó: não compartilhar, exata ou
  aproximada de ~90 m a ~6 km), enviar também ao se mover (smart broadcast),
  GPS do próprio nó (ligado/desligado; "a placa não tem GPS" quando não tem),
  e o GPS do Tab5 para o nó, junto da posição fixa e do intervalo de envio.
- Modo pendrive: o cartão SD aparece no computador como um disco USB pelo
  USB-C, em segundo plano — o Tab5 segue usável. Liga e devolve em
  Ajustes > USB; enquanto durar, a barra mostra "Cartão no computador".
  Ejetar no computador devolve o cartão sozinho; devolver no Tab5 sem o
  computador ter ejetado pede confirmação. Durante o pendrive o cabo não é
  console nem gravação, a CPU fica no clock máximo e, com o disco montado, a
  tela não apaga.
- Diário de gravações na flash interna (partição `reserva`, 2 MB): com o
  cartão no computador, o que o sistema grava (log, conversas, energia,
  nós do Mesh, redes Wi-Fi) vai para o diário e chega ao cartão na volta, na
  ordem em que aconteceu — inclusive depois de um reinício no meio. Se o
  computador mudou um arquivo que o Tab5 também reescreveu, o do computador
  fica e o do Tab5 vai para `<nome>.tab5`. A barra avisa quando o diário
  passa de 90%; a página USB mostra o que se perdeu. **Exige regravar com
  `./gravar.sh`** (tabela de partições nova; os ajustes ficam).
- Ajustes > USB: ligar o pendrive sozinho ao conectar num computador
  (desligado de fábrica; com um app aberto ou a tela bloqueada, só avisa).
- `tab5_sd_acrescentar()` e `tab5_sd_substituir()` — as portas de gravação
  do sistema —, `tab5_sd_emprestar()`/`tab5_sd_devolver()`
  (`hal/tab5_sd.hpp`), `app_t.exige_sd`, `sistema_manter_acordado()`,
  `sistema_bloqueado()` e `sistema_app_exige_sd()` (`kernel/app.h`).
- Central de avisos: tocar a barra de status abre uma lista com todos os
  apps que estão avisando (nome, resumo, contagem, cor da gravidade); tocar
  uma linha abre o app. Fecha tocando fora, na barra, com Esc ou deslizando
  para cima. A barra ganha um ponto colorido por app além do destaque, e a
  tela de bloqueio lista até 4 apps em vez de um só.
- `notif_listar()` e `notif_associar_app()` em `services/notificacao.h`.
- Mesh: rede americana em paralelo. Um segundo cliente MQTT
  (`mqtt.meshtastic.org`, raiz `msh/US`, LongFast de fábrica) roda junto do
  enlace de sempre — no modo internet, com o nó no cabo/Bluetooth e com o
  proxy. As mensagens vão para a conversa "LongFast EUA", embaixo de um traço
  "EUA" na coluna de conversas; os nós ficam marcados com a rede e a página
  de nós ganha os botões BRASIL / EUA, com o liga/desliga da rede americana
  (desligada de fábrica). Os EUA têm teto de 96 nós na tabela, não apitam
  nem entram na barra de status, e nó americano não tem CONVERSAR (só canal,
  sem PKI).
- Conversas: a hora de cada mensagem embaixo do balão, do lado de quem falou
  (Mesh e ESP-NOW).

### Mudado
- Barra: o nome do app começa logo depois da hora e tem largura máxima
  (corta com "..."). Um nome comprido passava da fronteira e a pastilha de
  avisos desenhava por cima; agora o resto da barra fica para os avisos.
- Berry: os arquivos dos apps (`cartao`, fotos da `camera`) moram em
  `apps/Berry/<nome do app>/`, ao lado dos `.be` - antes ficavam em
  `apps/<nome do app>/`, misturados com as pastas dos apps do sistema.
- App Câmera: o preview anda no ritmo da câmera. O relógio de 100 ms só vale
  enquanto há foto em curso (antes metia passadas sem quadro novo no meio das
  que tinham), e o botão só é repintado quando muda.
- Berry: o estado de cada chamada virou um contexto por trabalho (não mais
  variáveis soltas), e o `.bss` do Berry (~11 KB) saiu da RAM interna para a
  PSRAM. A tarefa do Berry sobe para a prioridade de quem espera na fila.
- A hora subiu para o canto esquerdo da barra de cima (menos na tela de
  bloqueio, que já mostra a hora grande); a temperatura do P4 desceu para a
  faixa de baixo, com a média desde o boot. O termômetro enche e muda de cor
  (azul, verde, amarelo, laranja, vermelho) conforme a temperatura.
- O botão da faixa de baixo troca a palavra "INÍCIO" por um círculo, o
  símbolo de tela inicial.
- Mesh > Nó em grupos: a página abre numa lista curta (Nome e papel,
  Localização, Rádio LoRa, Conexões e internet, Avisos e telemetria,
  Manutenção), cada grupo com um resumo; tocar abre as opções dele juntas e
  VOLTAR sobe. O pendente vale para a página toda — mudar coisas em grupos
  diferentes ainda grava numa transação só (o nó reinicia uma vez).
- O GPS do Tab5 com o nó do outro lado vai **só para o nó**, como faz o app
  do celular: o nó a difunde no ritmo e com a precisão dele. Antes ia em
  difusão de 3 saltos e o nó repetia a mesma posição no ritmo dele — duas
  vezes o tempo de ar. Com posição fixa ou o GPS do nó ligado, o Tab5 não
  manda nada (o nó ignoraria ou atropelaria). Pela internet, sem nó, o Tab5
  respeita a precisão do canal que o nó deixou (0 = não manda).
- "Posição do GPS para a malha" saiu de Ajustes > Mesh: agora fica só em
  Mesh > Nó > Localização (dois lugares para a mesma coisa).
- Pôr ou tirar a posição fixa pede ao nó só a configuração de volta
  (want_config 69420), não a NodeDB inteira; firmware antigo manda tudo,
  como antes.
- Desligar o MQTT do nó leva o proxy (internet do Tab5) junto — o espelho
  da regra que já existia ao ligar o proxy.
- Arquivos e Câmera não abrem com o cartão no computador: o sistema mostra o
  motivo no lugar do app e abre sozinho quando o cartão volta
  (`app_t.exige_sd`), e o pendrive não começa com um deles aberto.
- Toda gravação de dados passa pelas portas do sistema (`hal/tab5_sd.hpp`);
  `tab5_sd_abrir()` fica para ler e para arquivo novo grande. Os serviços que
  reescrevem um arquivo inteiro (redes Wi-Fi, redes do nó, conversas
  apagadas, NodeDB) só o fazem depois de tê-lo lido — sem cartão, a mudança
  vale até reiniciar em vez de apagar o que estava guardado.
- Trocar um arquivo inteiro sobrevive a queda de energia no meio: quem for
  ler completa a troca interrompida (o conteúdo novo estava no `.tmp`).
- `sistema_manter_acordado()` acende a tela se o pedido chegar com ela
  apagada (o computador montando o disco com o Tab5 em repouso).
- A navegação travada (`sistema_travar_navegacao`) passa a valer também para
  a barra de status (não abre a central de avisos) e para pedidos de fora de
  trocar de app (`sistema_simular_abrir`, console).
- Emoji guardados com paleta (1 byte por pixel + as cores de cada um) em vez
  de RGB565 cru: de ~1,49MB para ~0,91MB de flash, com a mesma imagem pixel a
  pixel. O fundo transparente passa a ser o índice 0 da paleta, não mais a
  cor magenta.
- Emoji em texto menor que 24px encolhe para a altura da linha, em vez de
  invadir as linhas vizinhas.
- Teclado virtual no padrão do celular. Shift vale para UMA letra e solta
  sozinho; dois toques seguidos travam (caps lock), e a seta mostra o estado
  (contorno / cheia / cheia com barra). Na conversa a maiúscula arma sozinha
  no começo do texto e depois de ". ", "! " e "? " (senha, PIN e nome não).
  Shift e apagar foram para as pontas da fileira do "zxcv", como no Android,
  e no lugar do apagar entrou a tecla "?123": duas páginas de símbolos com
  parênteses, chaves, colchetes, `< > \ | ~ ^` e os acentos do português
  (á à â ã é ê í ó ô õ ú ç, maiúsculos com o shift). Espaço ou acento numa
  página de símbolos volta às letras. Vírgula e ponto não viram mais "?" e
  "!" com o shift, e a fileira dos números fica sempre números.
- Teclado virtual: um `tecvirt_editar()` só para os campos simples (senha do
  Wi-Fi, nome e Wi-Fi do nó) no lugar de três cópias do "põe no fim / tira o
  último"; o apagar deles tira o caractere UTF-8 inteiro, não um byte.
- Um codec Protobuf só (`services/mesh_pb.hpp`) no lugar de três cópias
  (`tab5_mesh`, `tab5_mesh_admin`, `tab5_mesh_mqtt`). A escrita confere o
  limite do buffer: o que não cabe é recusado, em vez de montado com
  ponteiro solto e bytes mágicos. Texto, admin e posição saem pelo mesmo
  envelope; o teste no Mac (`testes/rodar_mesh_pb.sh`, com ASan/UBSan)
  confere que os bytes são idênticos aos de antes.
- Parada de tarefa de serviço num mecanismo só (`kernel/parada.hpp`): GPS,
  Mesh e o enlace Bluetooth do Mesh esperam a saída num semáforo em vez de
  perguntar a cada 10 ms. No GPS as bandeiras nem eram `volatile`.
- Ouvinte de evento (`hal/tab5_ouvinte.hpp`) para a varredura BLE, o farejo
  e o CSI: remover o ouvinte espera a chamada em voo. O Radar deixa de
  dormir 40 ms "para dar tempo" ao fechar.
- Troca de relógio do painel pedida pelo console: espera num semáforo em vez
  de perguntar a cada 10 ms.
- "Relógio acertado?" num lugar só (`tab5_hora_epoch()`), no lugar de doze
  cópias do mesmo número com quatro nomes.
- Ponte MQTT: os quadros para o serviço são montados direto no quadro
  estático da fila (a tarefa do cliente dos EUA tem 3,5 KB de pilha); o
  ToRadio é lido por uma função só nas duas saídas (Brasil e EUA).
- RAM interna: as pilhas das 6 tarefas do esp_hosted (`sdio_*`, `eh_rpc_*`)
  foram para a PSRAM (`CONFIG_ESP_HOSTED_DFLT_TASK_FROM_SPIRAM`). Eram 24 KB
  presos para ~8 KB de uso medido. Com WiFi + MQTT no ar, a RAM livre foi de
  202 para 227 KB, o mínimo de 157 para 182 KB e o maior bloco de 77 para
  102 KB. Com a tela acesa, a foto do monitor e os underruns não mudaram
  (~2,3 ms no pior caso, zero piscadas). Em repouso, durante varredura BLE,
  o pior caso subiu de ~25 para ~50 ms, mas o painel está desligado.
- `mem extrato` ganhou a coluna "mínimo" (o pico temporário dentro de cada
  etapa, que o delta esconde) e `tarefas` mostra a duração da foto do
  monitor (última e pior).
- Mesh: a cópia da NodeDB do Tab5 (31 KB de PSRAM) só existe enquanto grava
  ou lê o `nodedb.bin`; os nomes dos nós dos EUA não vão para o `nos.txt`; o
  cliente MQTT dos EUA usa buffers de 768/512 bytes e pilha de 3,5 KB (RAM
  interna).
- Mesh: emprestar a internet do Tab5 ao nó (MQTT Client Proxy do
  Meshtastic), na página Nó do app Mesh, linha "internet do Tab5 (proxy)",
  junto do "MQTT do nó" e com as mesmas regras da página: o toque deixa
  pendente, GRAVAR NO NÓ grava (ligar o proxy leva o MQTT junto, na mesma
  transação) e a linha mostra o que o nó confirmou, com o estado da ponte ao
  lado. O nó publica e assina pelo enlace com o Tab5 (Bluetooth ou cabo) e o
  Tab5 leva ao servidor pelo Wi-Fi: o nó vira gateway completo sem ligar o
  Wi-Fi dele, e o Grove fica livre para o GPS. O Tab5 só carrega bytes - quem
  cifra é o nó, então a mensagem direta com PKI também passa - e usa o
  servidor configurado no nó.
- Radar enxerga WiFi: o farejo do C6 entra como segunda fonte de alvos ao lado
  do BLE (roteadores, celulares procurando rede, aparelhos conectados), no
  mesmo ajuste de cosseno, na mesma busca quente/frio e com filtro próprio
  (TODOS / SÓ TAGS / SÓ WiFi). O farejo fica no canal em que o rádio já está.
- Radar mostra movimento pelo CSI: o serviço `tab5_csi` mede quanto o canal
  entre o roteador e o Tab5 varia (parado / leve / forte), aprende o "parado"
  de cada lugar e suspende a medida enquanto o próprio Tab5 se mexe.
- Serviço `tab5_csi`, dono único da mensagem de CSI (o peer_data guarda um
  ouvinte por mensagem): pede ao C6 só o CSI do roteador, pinga o roteador a
  20 Hz como fonte e custa zero parado. O `radio csi` do console passa por ele
  e mostra a medida de movimento.
- Firmware do C6 (contrato v3): a gravação passa a levar de uma vez tudo que
  depende do rádio, cada recurso ligado sob pedido do P4 e dormente até lá —
  ESP-NOW (texto, dois dialetos) e canal, farejador promiscuo em lotes, tubo
  cru de ESP-NOW, INFO, mais CSI, FTM e 802.15.4. Como regravar o C6 é físico
  (Downloader), as próximas evoluções acontecem do lado do P4 sem reabrir o
  aparelho. Cada capacidade anuncia um bit em `caps` (só acende se o módulo foi
  compilado), e o P4 esconde o que a gravação não trouxe.
- Serviço `tab5_farejo` e comando `farejo` no console: o C6 ouve em modo
  promiscuo, resume por MAC e manda em lotes; o P4 guarda a tabela só enquanto
  ligado (na PSRAM, numa vaga do pool) e o C6 desliga sozinho se o P4 parar de
  renovar. `farejo info` mostra as capacidades de rádio do C6 gravado.
- Console serial reorganizado: os comandos moram numa tabela única, e o
  `help` é gerado dela, em sete grupos (Sistema, Tela, Cartão, Rede,
  Meshtastic, Bancada, Testes), com todas as formas de cada comando e
  exemplos. `help <comando>` explica para que serve; `help tudo` mostra tudo. Comando errado sugere os
  parecidos.
- Console: `estado` (antigo `servicos`, agora com versão, tempo ligado e RAM);
  `wifi` sozinho mostra o estado, mais `wifi redes`, `wifi gravar`,
  `wifi conectar` e `wifi esquecer`; `log` aceita nomes de nível
  (`log eh_sdio debug`) e mostra o nível atual; `ler <arq> [linhas]`;
  caminhos do cartão relativos a `/sdcard`; `mesh nos` e `mesh enviar`;
  `grove cru <baud>`; `tela prioridade alta|padrao`; `mem tudo`.
  Os nomes antigos (`servicos`, `redes`, `nos`, `enviar`, `grove <baud>`,
  `tela on|off`) continuam aceitos.
- Mesh: escolha entre três vias em Ajustes > Meshtastic (Cabo Grove,
  Bluetooth, Internet). O Grove tem prioridade: quando a sondagem acha o C6L no
  cabo, o Mesh passa para ele sozinho, de qualquer via; depois disso a escolha
  manual vale até o próximo achado.
- Mesh, página Nós: todos os canais ativos do nó no topo, como botões (o que
  não está na coluna aparece com "+" e volta ao ser tocado); tocar num nó abre
  o cartão dele - id, MAC (quando o nó envia), modelo, SNR, saltos, bateria e
  tensão, visto, posição com distância e rumo - com CONVERSAR e VOLTAR.
- Mesh: página "Nó" (engrenagem no trilho) para configurar o nó Meshtastic
  pelo Tab5, sem PC nem celular: nomes, papel, saltos, potência, intervalos de
  posição/apresentação/telemetria, MQTT e Bluetooth do nó. As mudanças ficam
  pendentes (amarelo) e "GRAVAR NO NÓ" manda tudo numa transação
  (`begin/commit_edit_settings`) - o nó grava e reinicia uma vez, e a tela
  mostra o que ele confirmou no dump seguinte. Ações imediatas: posição fixa
  pelo GPS do Tab5, acertar a hora do nó, reiniciar (com confirmação). Mostra
  também firmware, placa, região/modem, nós guardados (rádio · Tab5),
  reinícios e o último aviso do firmware (`ClientNotification`). O som das
  notificações saiu da página Malha para esta.
- Mesh: `services/tab5_mesh_admin` - set_config/set_module_config reenviam a
  seção crua do dump com os campos novos no fim (o protobuf fica com a última
  ocorrência), sem modelar o protobuf inteiro. Seções com senha ou chave
  (network, power, security) e o módulo serial nunca são reenviados.
- Mesh: NodeDB do Tab5 no cartão (`nodedb.bin`, PSRAM, 256 nós), carregada ao
  subir o serviço e gravada a cada 10 min e ao parar: o Tab5 lembra mais nós
  do que o teto do rádio (100 no C6L até a 2.7).
- `ferramentas/teste_mesh_admin.py`: decodifica com os protobufs oficiais os
  bytes que o M5tabOS monta (admin, texto, canais).
- Mesh: favoritos (estrela). No cartão do nó, FAVORITAR marca; a tabela mostra
  a estrela e põe os favoritos logo abaixo do nosso nó, na ordem em que foram
  marcados, sempre presentes e nunca despejados. São os favoritos do próprio
  Meshtastic: a marca vai ao nó (AdminMessage `set_favorite_node` /
  `remove_favorite_node`, admin local) e aparece no cliente web; o
  `is_favorite` do dump atualiza a lista daqui. Sem o rádio (modo internet) a
  mudança fica pendente e vai no próximo dump. Lista guardada na NVS.
- Mesh: marca cinza de "ouvida pela malha" na mensagem direta - o vizinho
  repetiu, o destinatário ainda não confirmou. O visto verde fica para o ACK do
  próprio destinatário.
- Home em páginas: com mais apps do que cabem na tela (12 hoje), arrastar para o
  lado troca de página e bolinhas embaixo mostram a atual. `TAB5_HOME_LINHAS_MAX`
  (kernel/internal.hpp) limita as linhas por página.
- Tela de bloqueio (Ajustes > Tela, desligada de fábrica): ao acordar mostra
  hora, data e o aviso em destaque; deslizar para cima desbloqueia e volta ao
  que estava aberto. Enquanto bloqueado, nada chega ao app por baixo.
- Mesh pela internet (MQTT) quando o nó não está conectado: com Wi-Fi e sem o
  C6L, as mensagens de canal chegam e saem pelo servidor MQTT. Sem servidor
  aprendido do nó, usa o do Mesh Brasil (`platform.meshbrasil.com`, raiz
  `meshdev`, canal LongFast com a chave padrão); a região do rádio não entra
  no tópico. Assina também as sub-raízes (`meshdev/<país>/<região>/...`) e os relatórios
  de mapa, que viram nome e posição de nós. Canais, chaves e servidor são
  aprendidos do nó quando ele está conectado.
- Ajustes > Meshtastic: botão LIGAR/DESLIGAR da internet e linha de estado
  (servidor e raiz em uso). O nó no Grove tem prioridade: com ele, a internet
  nunca roda, mesmo com o Wi-Fi ligado.
- `ferramentas/teste_mqtt_vivo.py`: escuta o servidor real e decifra com o
  mesmo algoritmo, para conferir tópico e criptografia fora do aparelho.
- Ajustes > Wi-Fi: lista das redes em volta, senha digitada na tela, várias
  redes guardadas (cifradas) e conexão automática na conhecida mais forte.
- Ajustes > Bluetooth: tocar num nó Meshtastic conecta o Mesh a ele, e a
  página mostra o estado da conexão; nós reconhecidos também pelo nome.
- Hora pela internet (NTP) quando o Wi-Fi está conectado. Ordem de confiança:
  internet, GPS (só com fix), Mesh (só tráfego ao vivo); uma fonte pior não
  desfaz o acerto de uma melhor.
- Ajustes > Bluetooth reconhece rastreadores (rede Find My/AirTag, Tile,
  SmartTag, Google) e beacons (iBeacon, Eddystone), e mostra a distância
  estimada pelo sinal. Ajustes > Wi-Fi mostra a distância estimada de cada rede.
- Log: a hora acertada aparece no horário local, com o fuso, e diz em palavras
  quanto o relógio estava errado ("o relógio estava 1 s atrasado"). Antes saía
  em UTC ("18:19:01Z") com "relogio mudou +1s".
- Log: mensagens de boot e de ajustes reescritas para serem lidas sem o fonte -
  ligado/desligado no lugar de "=0/=1", o perfil de energia lista só o que
  está ligado, endereços de memória e nomes internos (peer_data, AXI, fb0,
  app_main) trocados pelo que importa para quem lê.
- Teclado virtual é um componente do sistema (`tecvirt_t`): conversa, senha do
  Wi-Fi e PIN usam o mesmo, com retorno da tecla e vários dedos. Desceu até a
  barra de baixo (sem a faixa morta; a navegação travada entrega o toque na
  fileira de comando), a letra sai uma amostra antes, e no Ajustes cada letra
  repinta só o campo em vez da página e do teclado inteiros.
- Toque por nível recebe a mesma correção vertical do clique (10 px).
- RAM interna: ~80 KB de variáveis estáticas dos apps e serviços (monitor,
  Trilha, Radar, MQTT, Mesh, ESP-NOW, tabelas de fonte) e o `.bss` do lwIP
  saem da SRAM para a PSRAM via `main/linker.lf`. O buffer do log (12 KB), o
  vetor de linhas da segunda página da tela e o retrato de tarefas do monitor
  também.
- Pilhas redimensionadas pelo pico medido em `pilhas-*.csv`: console 12→8 KB,
  WiFi e ESP-NOW 8→6 KB, MQTT 6→4 KB. A do `sys_evt` sobe de 2,3 para 3 KB
  (chegava a 600 bytes livres).
- mbedTLS aloca na PSRAM (`CONFIG_MBEDTLS_EXTERNAL_MEM_ALLOC`): uma sessão TLS
  não tira mais ~40 KB da interna de uma vez.
- Console: `mem` mostra o extrato da RAM interna (quanto cada etapa do boot e
  cada rádio consumiu) e o mapa de fragmentação por região do heap.
- Ajustes > Meshtastic: a assinatura de repintura vira um hash da página
  inteira; os estados longos (internet, proxy) no fim passavam do limite de
  comparação e a tela não acompanhava a mudança.
- Energia reformulado no molde das telas de bateria dos celulares. No topo, a
  porcentagem grande com o estado (na bateria / carregando / na tomada) e a
  estimativa pela média dos últimos 30 min ("dura ~6 h 30 min neste ritmo",
  "cheia em ~1 h 20 min"). Abaixo, um índice de seis cartões (bateria,
  corrente, potência, tensão, temperatura e gasto das 24 h); tocar num cartão
  escolhe o gráfico. Saem o título, a linha do registro (só aparece se houver
  falha de gravação), os MHz da CPU e a legenda de sinal.
- Energia, gráfico de nível: eixo com números redondos em cada linha da grade
  (antes só o mínimo e o máximo medidos, no canto), área preenchida, horas de
  carga tingidas de verde atrás da curva e um vértice por coluna de pixel (a
  média suaviza o ruído da corrente). Na corrente o preenchimento vai até o
  zero: verde acima, laranja abaixo. Tocar põe um cursor com a hora e o valor.
  O período 24 H é uma janela móvel (da hora cheia de 23 h atrás até agora,
  lendo os arquivos de ontem e de hoje) no lugar do HOJE de 0 a 24 h, que
  ficava vazio logo depois da meia-noite.
- Energia, gráfico novo de gasto por hora: barras de mAh nas últimas 24 h,
  separando tela acesa de repouso, com uma faixa verde nas horas em que houve
  carga e o mesmo eixo de tempo do gráfico de nível. Tocar numa barra mostra o
  detalhe da hora; sem seleção, os totais das 24 h.

### Corrigido
- Gravar nos ajustes a partir de um app Berry (pilha na PSRAM) derrubava o
  aparelho (assert do cache da flash). Os ajustes chamáveis de qualquer
  tarefa gravam pelo caminho adiado, e uma gravação direta de pilha na PSRAM
  é recusada com log em vez de travar.
- Depois do modo pendrive a serial do USB-C não voltava (nem tirando e pondo o
  cabo; só reiniciando): o controlador do console agora é reiniciado e o
  seletor de PHY volta ao estado do boot.
- Apps do cartão copiados pelo modo pendrive só apareciam depois de
  reiniciar: a pasta é relida quando o cartão volta.
- Cartão: "caronas esgotadas" no boot. Cinco serviços gravam pela tarefa do
  cartão e só cabiam quatro; o último (as redes Wi-Fi do nó do Mesh, na
  ordem de hoje) nunca gravava.
- Mesh: mensagem direta para um nó desligado, com o C6L na internet, ganhava
  o check cinza ("outro nó ouviu") e nunca mostrava o X. Era a confirmação de
  que o pacote chegou ao servidor MQTT, tomada por confirmação de rádio.
- Wi-Fi não voltava sozinho ao chegar em casa: depois de desistir longe do
  roteador, a queda gerada pela própria parada do rádio trocava o estado
  FALHOU por DESLIGADO, e a retomada automática (que só olhava FALHOU) nunca
  mais procurava — só a procura de Ajustes > Wi-Fi reconectava. Agora a
  queda não apaga o motivo, a retomada age sempre que o Wi-Fi está ligado em
  Ajustes e sem rede, e acender a tela procura na hora. Um "conectando" sem
  resposta do C6 por 60 s também passa a desistir (e a retomada assume), e o
  `varrer` do console devolve a conexão no fim.
- CSI e Farejo: ligar logo depois de desligar podia criar duas tarefas
  (CSI) ou deixar o farejo desligado e sem tabela depois de "ligar" ter
  respondido sim. Os dois passam a usar a parada do kernel (`parada.hpp`).
- Mesh: canais e NodeDB não são mais gravados no laço do rádio (um cartão
  lento fazia perder quadros do nó); vão de carona na tarefa do cartão.
- Partes de fundo (Trilha, Azimutal): parar espera a saída num semáforo, sem
  o laço de 10 ms na interface; a máquina de estados própria deu lugar à
  mesma parada dos serviços.
- Energia e desempenho: a interface acorda por notificação de tarefa (não
  mais por fila); o teclado só é lido pelo I2C quando o pino de interrupção
  avisa (depois de provar que avisa); o GPS dorme na fila de eventos da UART;
  o ESP-NOW dorme 30 s sem enlace com o C6; Ajustes redesenha no ritmo de
  cada página; o papel de parede da home fica pronto enquanto se está nela.
- Memória: o Bluetooth não tem mais tarefa permanente (−4 KB de RAM
  interna); ligar/desligar usa uma vaga do pool.
- `tab5_dentro()` e `tab5_agora_ms()` no lugar das cópias nos apps.
- `sdkconfig.defaults`: `CONFIG_FREERTOS_TASK_NOTIFICATION_ARRAY_ENTRIES=2`
  (num sdkconfig antigo o código funciona igual; para ganhar a separação,
  `idf.py menuconfig` ou apagar o `sdkconfig`).
- Mesh: parar o serviço apagava o mutex principal enquanto a barra da home
  e o app Mesh podiam estar segurando ou esperando por ele (toda troca de
  via e toda re-sondagem do Grove). Agora o mutex nasce uma vez e fica.
- Wi-Fi: a reconexão depois de uma queda dormia 1 s DENTRO da tarefa de
  eventos do sistema, congelando todos os eventos (IP, esp_hosted). A
  espera foi para a tarefa do Wi-Fi.
- Wi-Fi: ESP-NOW, CSI e Farejo alteravam a mesma máscara de "receptor
  ligado" sem operação atômica e podiam apagar o pedido um do outro.
- Texto cortado para caber agora corta por caractere (UTF-8) em todo o
  sistema: uma função só (`tab5_cortar_ao_caber`) no lugar de uma dúzia de
  cópias — três cortavam por byte e desenhavam lixo em palavras com acento
  (barra de status, faixa da home, nomes no Arquivos).
- Mutex criado sob demanda (`tab5_mutex_travar`) não tem mais corrida na
  primeira chamada; GPS, IMU, MQTT e Azimutal passam a usar ele.
- Pedidos do console para abrir/fechar app esperam o app terminar de abrir
  (antes chamavam `ao_fechar` de um app que nunca abriu).
- Energia: com a tela apagada, o IMU não é mais lido por I2C a cada segundo,
  o monitor amostra a cada 10 s e não espera quadro de painel desmontado, a
  tabela de travas de PM sai só no primeiro repouso, e a potência é
  calculada (V×I) em vez de lida do INA226.
- Ajustes: arrastar brilho/volume grava na flash só o valor final (1 s depois),
  e não a cada quadro do arraste.
- Memória: a "lixeira" de desenho (1,8 MB de PSRAM) é solta quando o painel
  volta; histórico do monitor e buffers de ordem da home saíram da RAM
  interna.
- Pendrive, cabo puxado sem ejetar: a sessão segue (o computador pode ter
  algo do cartão em cache), mas com o barramento parado por mais de 2 s a
  tela volta a apagar e a CPU sai do clock máximo — antes ficavam assim para
  sempre, na bateria. Ligado sem computador no cabo, o clock máximo sai
  depois de 10 s; volta quando o computador configurar o disco.
- Pendrive, ao devolver: se a tarefa do USB não parasse em 2 s, o cartão
  voltava ao Tab5 com ela ainda podendo estar no meio de uma leitura ou
  escrita (memória liberada em uso). Agora a volta espera ela parar.
- Pendrive: "devolver" pedido enquanto ele ainda preparava era descartado
  em silêncio; agora fica guardado e a sessão devolve assim que o USB sobe.
- A foto do cartão no empréstimo (48 KB de PSRAM) é solta quando o cartão
  volta, ou quando o empréstimo desiste — ficava presa até o reinício.
- Diário: aplicar um registro não põe mais 512 B de nomes na pilha de quem
  lê o cartão (os nomes vão no mesmo bloco da PSRAM que o registro).
- Vigia do cabo: com "ligar sozinho" desligado (o de fábrica) olha a cada
  1 s em vez de 250 ms.
- Wi-Fi volta sozinho para uma rede conhecida. Depois de 5 tentativas sem
  sucesso (fora de alcance, roteador reiniciando) o rádio desistia e nunca
  mais tentava, e a procura pela tela via a rede conhecida e desligava o
  rádio de novo. Agora, com o Wi-Fi ligado em Ajustes, ele procura de novo
  sozinho (30 s, 1, 2, 4 e depois a cada 5 min) e conecta na conhecida mais
  forte que aparecer; a procura pela tela também conecta.
- Mesh: se a tarefa do serviço não pudesse ser criada, a UART e o Grove
  ficavam com o Mesh até reiniciar. Agora são devolvidos.
- Ponte MQTT: os clientes do Brasil e dos EUA decifravam e montavam pacotes
  nos MESMOS buffers estáticos, cada um na sua tarefa, ao mesmo tempo — um
  podia sobrescrever o pacote do outro no meio. Cada cliente tem o seu par.
- Ponte MQTT: um quadro que não coubesse no buffer podia sair cortado em
  alguns caminhos (o tamanho era calculado mesmo com a escrita falhando).
- Aviso sumindo da barra sem abrir o app: com a tela bloqueada (ou acordando
  do repouso direto no bloqueio) o kernel entregava os avisos ao app que
  estava por trás, e o ESP-NOW marcava as mensagens como lidas sem ninguém
  ver. App encoberto não recebe mais aviso, toque nem tecla.
- Mesh: parar e subir o serviço (troca de via, Wi-Fi caindo no modo
  internet, Bluetooth caindo, C6L achado no cabo) zerava as não lidas e
  apagava o aviso. As não lidas de cada conversa ficam guardadas até serem
  lidas, e registrar a fonte de novo não zera mais o estado dela.
- Mensagens vindas da ponte do Telegram (e de qualquer celular) apareciam em
  quadrados: todo caractere fora da fonte que não é emoji vira o retângulo
  da M5GFX. O texto agora passa por `tab5_unicode` antes de desenhar — letra
  "estilizada" (𝗻𝗲𝗴𝗿𝗶𝘁𝗼, 𝓈𝒸𝓇𝒾𝓅𝓉) vira letra, bandeira vira "BR", tom de
  pele e caracteres invisíveis somem, acento combinado se junta, cirílico e
  grego são transliterados, e o que sobra vira "?". Testado no Mac:
  `testes/rodar_unicode.sh`.
- Mensagem com quebra de linha era cortada no histórico do cartão (uma
  mensagem por linha): a quebra vira espaço no recebimento.
- Mesh: enviar duas mensagens seguidas congelava a tela até 2,1 s (o limite
  de texto do nó era esperado na tarefa de interface). O texto vai para uma
  fila e a tarefa do serviço respeita o limite; a recusa que já dá para saber
  (Bluetooth fora, internet com direta, EUA desconectado) continua na hora.
- Mesh pelo cabo: a leitura da serial esperava 200 ms por quadro mesmo com
  ele já chegado (pedia 256 bytes). Acorda no primeiro byte e lê o resto.
- Mesh pelo Bluetooth: a leitura parava a cada 64 mensagens e só voltava no
  próximo aviso do nó ou em 5 s — o dump de uma malha grande andava aos
  soluços. Continua na volta seguinte; e o serviço esvazia o que chegou em vez
  de um quadro por volta.
- Mesh pela internet: fila esvaziada de 16 em 16 (era 1 por volta), descida
  do proxy de 8 em 8 (era 4), cópias do mesmo pacote de vários gateways
  descartadas antes de decifrar, e o aviso de fila cheia diz qual fila, uma
  vez a cada 10 s.
- Mesh: a tabela de nós era reordenada a cada linha desenhada quando chegava
  tráfego; agora no máximo a cada 250 ms, e o aviso de "nós mudaram" para a
  tela a no máximo 4/s (1/s vindo dos EUA).
- Mesh: anel de mensagens já vistas de 64 para 256 (a cópia atrasada pela
  ponte não entra de novo com o tráfego dos EUA passando junto).
- Mesh, página Nó: mudar o MQTT ou o proxy do nó pelo Bluetooth não gravava
  nem reiniciava o nó. O firmware desliga o Bluetooth assim que recebe a
  seção MQTT, mesmo no meio da transação, e o commit nunca chegava - a
  transação ficava aberta (o firmware não a expira) e o nó sem Bluetooth até
  ser reiniciado na mão. Pelo Bluetooth a seção MQTT agora vai sozinha, fora
  da transação: o nó valida, grava e reinicia por conta própria. Com outras
  mudanças pendentes junto, a página pede para gravá-las antes. Pelo cabo
  nada muda. A página também mostra o motivo quando uma gravação é recusada.
- Mesh e ESP-NOW abriam deitados com o aparelho em pé. Na abertura, uma
  leitura do IMU zerada ou velha (o cache é de 1 s; no modo de economia do
  BMI270 o primeiro dado demora) caía na zona morta, e o padrão era deitado.
  Agora a abertura lê o IMU até ter uma gravidade plausível, decide pelo eixo
  que domina (sem a histerese, que só faz sentido com o app já aberto) e, sem
  decisão clara, abre na posição em que a tela já está. Com o app aberto, o
  limite para girar desceu de 30° para ~24° de inclinação: segurando o
  aparelho recostado, um app aberto deitado não se corrigia.
- CSI recusado pelo C6 com `ESP_FAIL`: o esp_hosted inicia o Wi-Fi do C6 com a
  config que o P4 manda, e o P4 mandava `csi_enable = 0`; o driver confere
  esse campo antes de tudo ("CSI not enabled in menuconfig!"). O P4 agora
  inicia o Wi-Fi com o CSI habilitado — não exige regravar o C6. O C6 liga o
  CSI por um caminho só e passa a aceitar também HE SU (802.11ax). Detalhes e
  prova em [docs/RADIO_C6.md](docs/RADIO_C6.md).
- 802.15.4 deixava o Wi-Fi associado mas sem passar dados, por duas causas.
  Ligado: faltava a coexistência Wi-Fi/802.15.4 (`esp_coex_wifi_i154_enable`),
  como fazem os exemplos oficiais de Thread e Zigbee no C6. Ao desligar: o
  `esp_ieee802154_disable` corta o clock do MAC no meio de uma recepção; o C6
  passa a desligar por `esp_ieee802154_sleep` (o caminho do OpenThread). O
  `radio 154` pinga o roteador antes, durante e depois.
- CSI vinha raro e cortado: o beacon de 2,4 GHz (11b) não gera CSI, então o
  `radio csi` pinga o roteador a 10 Hz como fonte, como o esp-csi; o record
  deixou de ser cortado em 256 B (teto de 1024) e passou a trazer o formato
  do quadro e a flag de primeira palavra inválida do hardware do C6.
- FTM: o "status 255" era o C6 recusando iniciar a sessão. O motivo agora vem
  no relatório: `ESP_ERR_NOT_SUPPORTED`, porque o ESP-IDF desliga o FTM
  initiator nos ESP32-C6 v0.0/v0.1 (errata do silício). O C6 só anuncia a
  capacidade FTM quando a revisão permite, e o `farejo info` mostra a revisão.
- CSI: o limite de taxa anda por cadência, e não descarta mais quadros que
  chegam com atraso variável.
- C6: a saída para o P4 virou um ring buffer de tamanho variável (records de
  7 B a ~1 KB sem cópia na pilha da tarefa de Wi-Fi), e o INFO mostra as
  mensagens descartadas e o estado do 802.15.4.
- Console: `espnow enviar` mandava só a primeira palavra da mensagem.
- Console: listagens longas (`mesh` com centenas de nós vindos pelo MQTT,
  `ble`, `lista`, `cartao`) prendiam o núcleo 0 por mais de 5 s e disparavam
  o watchdog da IDLE0. Agora cedem a CPU a cada 8 itens.
- Mesh, páginas "Nó" e Nós: rastros no topo e na base da lista ao rolar.
  Linha que ainda não tinha aparecido tinha assinatura vazia e "mudava" em
  toda passada; como a marca de redesenho é um intervalo, cada amostra do
  arraste redesenhava todas as linhas visíveis por cima do que a rolagem
  tinha acabado de deslocar. Agora só linha já desenhada é comparada, e cada
  linha da página "Nó" pinta o próprio fundo.
- Console `tela rolar cpu|dma`: compara a rolagem por DMA (da página da
  frente) com a cópia pela CPU, sem regravar.
- Mesh: dois textos com menos de 2 s de intervalo - o firmware descarta o
  segundo (rate limit da porta TEXT no PhoneAPI; na 2.7 sem erro, a mensagem
  virava "sem resposta"). O envio agora espera o que falta.
- Mesh: cabeçalho e corpo do quadro serial iam em duas escritas, e a tarefa
  do Mesh (batimento) e a interface (texto, favorito, admin) escrevem na UART;
  um quadro podia cair no meio do outro e os dois se perdiam. Agora um quadro
  por vez (trava de transmissão).
- Mesh: a configuração do canal 0 (o primário, LongFast) era ignorada e canal
  desabilitado era lido como ativo. `index` e `role` do Channel são proto3 e
  somem do fio quando valem zero; o leitor partia de -1 e PRIMARY. Um LongFast
  apagado não tinha como ser reaberto e canal desligado continuava na coluna.
- Mesh: canais desabilitados no nó saem da coluna (o histórico fica no cartão)
  e a configuração dos canais fica guardada na NVS, valendo também sem o nó.
- Mesh: apresentação (NODEINFO_APP) e posição (POSITION_APP) de nós ao vivo
  passaram a ser lidas. Antes nome, modelo, MAC e posição só vinham no dump de
  configuração: nó que aparecia depois ficava pelo id em hexa.
- Mesh: o nome mostrado segue curto → longo → id; o id só aparece quando não
  há nome legível. A coluna de conversas mostra o nome longo numa 2ª linha.
- Mesh: a coluna de conversas tem ordem explícita (canais, depois diretas da
  mais recente) e o app segue a conversa aberta pela chave. Uma conversa nova
  numa vaga anterior deslocava os índices e a tela mostrava e enviava para a
  conversa vizinha.
- Mesh: texto longo não saía. Acima de 233 bytes o nó descartava sem
  resposta, e entre ~220 e 233 recusava com TOO_LARGE (o pacote inteiro não
  passa de 255 bytes: cabeçalho, bitfield e, na direta, +12 da cifra PKI). O
  teto agora é 200 bytes, o mesmo dos clientes oficiais; a barra de escrita
  para nele e o envio corta na fronteira de caractere.
- Mesh: mensagem que só aparecia na tela quando chegava a seguinte. O gravador
  do cartão troca de buffer antes de escrever, e quem pedia para ler nesse
  intervalo via "nada pendente" e lia sem a linha; `tab5_sd_gravar_pendentes`
  agora espera a gravação em andamento. E o contador de mensagens só sobe
  depois de a linha estar no gravador.
- Mesh: mensagem própria reentregue durante o dump virava "sem resposta" e
  sobrescrevia no cartão o desfecho verdadeiro.
- Mesh: o anel de ids vistos não é mais zerado ao trocar de via - a mesma
  mensagem pelo rádio e pela ponte MQTT entrava duas vezes.
- Mesh, Malha: uso do canal, airtime e uptime mostram "-" sem telemetria do
  nó (era "0"), nós online contados localmente sem LocalStats, enlace com a
  via, e o nosso nó pelo nome.
- Mesh: tabela de nós com o nosso no topo (em branco), depois os ouvidos pelo
  ar com SNR (em amarelo) FIXOS na ordem em que apareceram - não descem quando
  outro nó fala -, depois o resto (MQTT em azul, sem SNR em cinza) do ouvido
  mais recentemente ao mais antigo. A tabela cheia despeja primeiro os não
  fixos. Linhas de 44 px com um fio de pouco contraste entre elas.
- Mesh: nó de rádio que não aparecia na tabela com o MQTT ligado no C6L. A
  tabela tinha 32 vagas e o downlink do MQTT as girava em segundos; o despejo
  olhava só o avistamento local, e os nós do dump (avistamento 0) giravam
  numa vaga só. Agora: 256 vagas na PSRAM, cache de nomes de 1024, despejo
  pela idade completa e primeiro os da internet, e o lugar fixo (ouvido pelo
  ar) dado na chegada do pacote - protege do despejo mesmo com a tela fechada.
- Mesh: botões de canal na mesma fonte da tabela, com o número do canal
  ("ch0 LongFast") e o rótulo "canais".
- Mesh pela internet: mensagem direta saía cifrada com a chave do canal e
  aparecia para todos no LongFast; agora é recusada (falta a chave pública).
- Mesh pela internet: `via_mqtt` e `hop_start` eram gravados nos campos 13 e 14
  do pacote em vez de 14 e 15.
- Teclado virtual: cada dedo tem a sua tecla e a letra sai na soltura
  daquele dedo - digitando rápido, a letra anterior se perdia quando o dedo
  seguinte descia antes de o anterior subir. Sem o intervalo mínimo de 80 ms
  entre letras (comia a segunda letra de "ss").
- Barra de escrita: mostra as duas últimas linhas do rascunho (da terceira em
  diante o texto não aparecia), com cursor no fim; a quebra de linha usa a
  largura real (o limite de 64 bytes cortava a linha no meio e o fim sumia).
- Brilho mínimo mais baixo (0,9% do PWM, era 4%) e barra de brilho na escala
  do olho: a metade de baixo da barra agora tem degraus finos. Novo comando de
  console `brilho <1-255> [hz]` para testar o backlight sem gravar.
- Mensagem enviada ficava no dia errado: o relógio era acertado pelo
  last_heard dos nós (hora do passado). Agora só tráfego ao vivo acerta o
  relógio, e mensagens carimbadas antes de um acerto grande são corrigidas.
- nos.txt crescia com os mesmos nós a cada boot em que o cartão não montava a
  tempo; agora só grava depois de ler, e é compactado quando tem sobra.
- Câmera com "zoom": o modo 1280x720 do sensor é uma janela no centro (80% x
  60% do campo). O sensor passou para 1600x1200; a foto sai com o quadro
  inteiro (1200x1600) e o preview não estica mais a imagem.
- O Bluetooth ligava em todo boot com o Mesh configurado para ele, mesmo
  desligado em Ajustes. Agora só o botão do Bluetooth liga o rádio.
- IMU que falhava no boot ficava fora até reiniciar; o sistema tenta de novo.
- Azimutal: durante o início a frio do GPS o mapa ficava vazio por minutos
  (satélites ouvidos ainda sem elevação/azimute eram descartados). Agora mostra
  quantos estão sendo ouvidos e uma barra de sinal por satélite.
- Azimutal não liga mais o 5V do Grove por conta própria (só energizava um
  módulo que o sistema não detectava): quem liga é Ajustes > Grove 5V, e o app
  avisa quando está desligado.
- Tocar num aparelho Bluetooth não dava retorno nenhum.
- Emojis não apareciam no texto desenhado pelo compositor (o gancho de emoji
  só existia no display físico) e tinham as cores trocadas (ordem de bytes do
  RGB565). Agora saem em tamanho inteiro, sem ampliação fracionária.
- Teclado virtual: fonte suave ajustada à largura de cada tecla (o ENVIAR não
  cabia com a tela em pé); setas no lugar de "^" e "<-".
- PIN do Meshtastic: o número mostrado pelo nó não fica mais salvo como PIN
  fixo; um PIN salvo recusado volta ao padrão e o sistema passa a perguntar.

### Alterado
- Texto suave sem fundo (quase todo texto do sistema) ~2,3x mais rápido: a
  letra é misturada direto na página de trás, só nos pixels de tinta.


## [0.7.0] - 2026-09-25

Primeira versão pública.

### Sistema
- Núcleo com laço de interface, compositor de página dupla, perfis de energia
  por app, repouso da tela e partes de fundo.
- Componentes: lista rolável, conversa (balões, teclado virtual), botões,
  fontes suaves Roboto com acentos.
- Gravador do cartão único para todos os apps, log diário e trabalhador para
  leituras lentas fora do toque.
- Ajustes organizado em páginas.

### Rádio
- Wi-Fi e Bluetooth LE pelo ESP32-C6 (esp_hosted; NimBLE no P4).
- ESP-NOW com firmware próprio no C6; o canal escolhido fica salvo.
- Meshtastic pelo cabo Grove ou por Bluetooth, com envio opcional da posição
  do GPS.

### Segurança
- Senha do Wi-Fi cifrada no cartão com uma chave derivada de cada aparelho.

### Aplicativos
- Mesh, ESP-NOW, Trilha, Azimutal, Câmera, Arquivos, Energia, Tarefas, IMU,
  Ajustes, CreazySnake e Exemplo.
