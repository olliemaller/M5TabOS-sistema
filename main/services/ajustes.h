// Ajustes do sistema - as PREFERENCIAS DO USUARIO, guardadas na NVS.
//
// A distincao que motivou este modulo: o tab5_perfil_t de cada app diz "que
// hardware EU preciso ligado" - Grove, radio, IMU. Isso e requisito do
// aplicativo. Ja o brilho e outra coisa: e preferencia de quem esta segurando
// o aparelho, e nenhum app tem autoridade sobre ela. Estavam misturados no
// mesmo struct, e o app vencia - errado.
//
// Agora o sistema sobrescreve o brilho do perfil na hora de abrir o app. O
// campo continua existindo no tab5_perfil_t (que precisa seguir autocontido e
// copiavel para outros projetos), mas quem manda aqui e este modulo.
//
// NVS e nao cartao: ajuste tem de funcionar sem cartao nenhum, e ser lido no
// boot antes de qualquer montagem.

#pragma once

#include <stdint.h>
#include <stddef.h>

// Faixa do brilho, no valor do M5GFX (0-255 vira o duty de um PWM de 9 bits
// a 44,1 kHz no GPIO22). O minimo NAO e zero de proposito: com a tela apagada
// voce nao conseguiria enxergar o controle para desfazer. O olho percebe o
// brilho em escala logaritmica (ver a barra de Ajustes, que segue essa curva),
// entao o piso tem de ser baixo - mas nao tanto: com 2 (0,9% de duty) a tela
// ficava acesa ao arrastar a barra e VOLTAVA APAGADA depois do repouso, porque
// o driver do backlight nao parte do zero com um pulso tao curto (ver
// tab5_power_acender). 6 = 1,8% de duty: ainda escuro, mas sempre visivel.
#define AJUSTES_BRILHO_MIN  6
#define AJUSTES_BRILHO_MAX  255

// Carrega da NVS (e inicializa a NVS, se preciso). Chamar antes das tarefas.
void ajustes_iniciar(void);

// Toda mudanca de ajuste vai para a NVS 1 s depois da ultima (na tarefa do
// esp_timer). Antes de cortar a energia, isto antecipa a gravacao pendente.
void ajustes_gravar_agora(void);

uint8_t ajustes_brilho(void);

// Aplica na tela AGORA e grava. Valor fora da faixa e limitado, nao recusado.
void ajustes_definir_brilho(uint8_t brilho);

// O BRILHO NA ESCALA DO OLHO. O valor do PWM e linear na luz, mas o olho nao;
// a "posicao" anda de 0 a AJUSTES_BRILHO_POS_MAX e o PWM sai dela pela curva
// gama 2,2 (a barra de Ajustes anda nela). A porcentagem (0-100, para quem fala
// assim: os scripts Berry) e a mesma posicao em passos de 10. 0 nao apaga a
// tela - vale o AJUSTES_BRILHO_MIN.
#define AJUSTES_BRILHO_POS_MAX 1000
int ajustes_brilho_para_pos(int brilho);   // PWM -> posicao
int ajustes_pos_para_brilho(int pos);      // posicao -> PWM
uint8_t ajustes_brilho_pct(void);
void    ajustes_definir_brilho_pct(uint8_t pct);

// Se o usuario quer o WiFi ligado. Quem age sobre isso e o servico de WiFi;
// aqui so mora a vontade.
bool ajustes_wifi(void);
void ajustes_definir_wifi(bool ligado);

// Manter o 5V do Grove SEMPRE ligado.
//
// Sem isto, um modulo que precisa bootar (o Unit C6L roda o Meshtastic inteiro
// num ESP32-C6) reinicia toda vez que o trilho e cortado, e nunca chega a
// conversar. Com isto ligado ele fica de pe como qualquer periferico.
//
// Custo: o trilho consome mesmo sem ninguem plugado, por isso nasce desligado.
bool ajustes_grove(void);
void ajustes_definir_grove(bool ligado);

// A PORTA USB-A ligada: 5V no conector e o pendrive montado sozinho em /usb
// (services/tab5_usb). Nasce LIGADA - espetar um pendrive e tocar musica
// nao pode depender de achar um ajuste. Sem nada plugado custa quase nada
// (a chave de 5V parada); com um aparelho plugado o sono leve fica suspenso.
bool ajustes_usb_a(void);
void ajustes_definir_usb_a(bool ligado);

// Tempo sem toque ate a tela apagar, em SEGUNDOS.
//
// Guardado em segundos e nao em milissegundos porque cabe num uint8_t da NVS e
// porque e assim que a pessoa pensa no ajuste. O sistema converte.
//
// O minimo de 15 s nao e capricho: abaixo disso a tela apaga no meio de uma
// leitura, e o toque para acordar vira parte do uso normal. O maximo de 60 s
// vem do outro lado - a tela e o maior consumidor do aparelho, e cada segundo
// aceso custa mais que qualquer economia que a gente arrancou do resto.
#define AJUSTES_TELA_MIN_S  15
#define AJUSTES_TELA_MAX_S  60

uint8_t ajustes_tela_s(void);
void    ajustes_definir_tela_s(uint8_t segundos);

// RELOGIO DO PAINEL (DPI/pixel clock), em MHz.
//
// Nasceu: ate aqui so dava pra mudar por 'tela relogio <mhz>'
// no console, e o valor morria no proximo boot (o sistema sempre reaplicava
// 60 fixo). Baixar o relogio corta a leitura continua da PSRAM que o painel
// exige (ver tab5_power.cpp) - a mesma familia de economia do brilho, so que
// mexendo no OUTRO lado da conta (quanto o painel PEDE, nao quanto o sistema
// gasta atendendo). Guardado e nao so em RAM para sobreviver ao reinicio, e
// exposto em Ajustes para quem nao estiver com o cabo serial na mao.
//
// A faixa e a MESMA que tab5_power_relogio_dpi() aceita (24..80) - limitada
// aqui tambem, e nao so la, pelo mesmo motivo do tela_s: um valor gravado por
// engano nao pode virar um painel que nunca mais sobe.
// Teto de 80 (era 100). Acima de 80 o
// painel so gasta banda de PSRAM sem ganho visivel, e o passo de 5 de Ajustes
// (24, 29 ... 79) nunca parava em 80: pulava para 84. Com o teto em 80, a
// limitacao faz o resto - 79+5 vira 80, 25-5 vira 24.
#define AJUSTES_RELOGIO_MIN  24
#define AJUSTES_RELOGIO_MAX  80

uint8_t ajustes_relogio_painel(void);

// SO GRAVA - nao aplica no painel. Quem muda o relogio de verdade e
// tab5_power_relogio_dpi(); o chamador (console, Ajustes) e quem decide a
// ORDEM entre aplicar e gravar, porque so ele sabe se a troca deu certo.
void    ajustes_definir_relogio_painel(uint8_t mhz);

// VOLUME do alto-falante interno (ES8388). Faixa NATIVA de
// M5.Speaker.setVolume() (0..255) - ao contrario do brilho, 0 e um estado
// legitimo (mudo), entao nao ha piso: a diferenca que justifica o piso do
// brilho (uma tela apagada esconde o proprio controle para desfazer) nao
// existe aqui.
//
// APLICADO NA HORA, mesmo padrao de ajustes_definir_brilho() - mas SEM
// custo de hardware nenhum enquanto ninguem toca som: setVolume() so grava
// um numero no objeto Speaker, o ES8388 continua desligado ate o primeiro
// M5.Speaker.tone()/playWav() de algum app (M5Unified inicializa o codec de
// forma preguicosa - ver o aviso grande em app_config.cpp, junto da secao
// VOLUME). Por isso pode ser aplicado aqui, no boot, sem esperar nenhum app
// abrir.
#define AJUSTES_VOLUME_MIN  0
#define AJUSTES_VOLUME_MAX  255

uint8_t ajustes_volume(void);
void    ajustes_definir_volume(uint8_t volume);

// CASA - posicao de referencia para calcular distancia/direcao ate uma
// localizacao recebida (Mesh, app_mesh.cpp). E do usuario e nao do Mesh
// especificamente, pelo mesmo motivo do brilho: nao e requisito de nenhum
// app, e preferencia de quem segura o aparelho - guardada aqui para poder
// servir qualquer app que precise no futuro, nao so o Mesh.
//
// Devolve false enquanto nao configurada (lat=lon=0 e o valor de fabrica, e
// tambem o unico ponto real que ninguem mora - fica seguro usar como "vazio"
// sem precisar de uma flag a parte na NVS).
bool ajustes_casa(float* lat, float* lon);
void ajustes_definir_casa(float lat, float lon);

// AVISO SONORO DA TRILHA: silenciar so a Trilha, com o Mesh ainda tocando.
// Vive aqui (preferencia do usuario) e nao
// em notificacao.h (que so guarda o estado em RAM, ver notif_silenciar()) -
// mesma distincao do resto deste arquivo: o QUE soa e decisao de quem
// segura o aparelho, nao do servico. Default TOCAR (true): o beep de
// notificacao já existia e funcionava; isto e um jeito de desligar, nao de
// ligar. Fica dentro do proprio app Trilha por enquanto (nao em Ajustes) -
// se um dia mais fontes pedirem o mesmo controle, cada uma ganha o par
// analogo (ajustes_beep_mesh() etc.), em vez de um mecanismo generico sem
// um segundo caso real para validar o formato (ver docs/ARQUITETURA.md, 1.1).
bool ajustes_beep_trilha(void);
void ajustes_definir_beep_trilha(bool tocar);

// AVISO SONORO DO MESH - o par analogo previsto no aviso acima, feito no
// mesmo formato. Mesma logica, mesmo
// default (TOCAR).
bool ajustes_beep_mesh(void);
void ajustes_definir_beep_mesh(bool tocar);

// TEMA DE COR DO ESPECTRO do app Musica (0 = arco-iris, o padrao). O app
// sabe quantos temas existem; aqui so se guarda o numero.
uint8_t ajustes_tema_espectro(void);
void    ajustes_definir_tema_espectro(uint8_t tema);

// COR DE DESTAQUE DO TOCADOR do sistema (services/tab5_musica), RGB565.
// 0 = a do sistema (lilas). Quem define e a interface do tocador (o app
// Musica pelo tema, um app Berry pela dele); o servico so publica.
uint16_t ajustes_cor_tocador(void);
void     ajustes_definir_cor_tocador(uint16_t cor565);

// MODO DO TOCADOR (services/tab5_musica): bit 0 = aleatorio, bits 1-2 =
// repetir (0 nao, 1 a fila toda, 2 a mesma musica). Vale ao ligar de novo.
uint8_t ajustes_modo_tocador(void);
void    ajustes_definir_modo_tocador(uint8_t modo);

// CANAL DO ESP-NOW. 0 = o do roteador; 1..13 = canal fixo (o WiFi
// solta o roteador). Era so RAM: todo reinicio voltava para o roteador e o
// Cardputer no canal 1 sumia ate alguem reabrir o app e escolher de novo.
// Quem aplica e o servico ESP-NOW (tab5_espnow) - aqui so mora a escolha.
uint8_t ajustes_espnow_canal(void);
void    ajustes_definir_espnow_canal(uint8_t canal);

// TAMANHO DA LETRA DA CONVERSA DO ESP-NOW: 0 = o padrao, 1..3 = maior (o
// botao "Aa" da barra do app passa de um para o outro). Guardado para nao
// voltar ao pequeno a cada reinicio.
#define AJUSTES_ESPNOW_LETRA_MAX 3
uint8_t ajustes_espnow_letra(void);
void    ajustes_definir_espnow_letra(uint8_t nivel);

// A ULTIMA CARGA CONHECIDA DA BATERIA (0..100; AJUSTES_SOC_NENHUM = nunca
// gravada). Guardada pela guarda da bateria (services/tab5_bateria) a cada 2
// pontos e ao desligar por bateria vazia: um boot COM O CABO nao pode partir
// da tensao, que carregando le muito acima da carga real.
#define AJUSTES_SOC_NENHUM 255

// ECONOMIA MAXIMA EM REPOUSO (services/tab5_wifi_cochilo): com a tela apagada
// e nada precisando da rede, o Wi-Fi desliga e volta ao acender a tela ou por
// 1 min a cada 15. DESLIGADA de fabrica: o padrao e ficar conectado em modo
// economico (P4 dormindo com batida, C6 em modem sleep maximo) - ao acender a
// tela a rede ja esta la.
bool    ajustes_wifi_cochilo(void);
// SONO LEVE DO P4 EM REPOUSO (contrato de economia, kernel/app.h). Ligado de
// fabrica; 'energia sono off' no console desliga (rede de seguranca).
bool    ajustes_sono_leve(void);
void    ajustes_definir_sono_leve(bool ligado);
void    ajustes_definir_wifi_cochilo(bool ligado);
// CAPA DA MUSICA PELA INTERNET (services/tab5_musica): sem capa no arquivo nem
// na pasta, o titulo e o artista da musica que toca vao a busca do iTunes.
// Ligada de fabrica; desligada, nada da biblioteca sai do aparelho.
// O C6 DORMINDO ASSOCIADO NO REPOUSO (hal/tab5_c6_sono): com a tela apagada o
// enlace SDIO desmonta e o C6 entra em light sleep, e o ESP-NOW so ouve com a
// tela acesa. Desligado de fabrica enquanto o consumo na bateria nao for medido.
bool    ajustes_c6_sono(void);
void    ajustes_definir_c6_sono(bool ligado);
bool    ajustes_capa_internet(void);
// Os ajustes de antes do ensaio de energia (services/tab5_ensaio), enquanto
// ele roda: wifi, bluetooth, grove, mesh_internet, mesh_via. nullptr apaga.
#define AJUSTES_ENSAIO_N 5
bool    ajustes_ensaio_originais(uint8_t out[AJUSTES_ENSAIO_N]);
void    ajustes_definir_ensaio_originais(const uint8_t* originais);
void    ajustes_definir_capa_internet(bool ligado);
uint8_t ajustes_bateria_soc(void);
void    ajustes_definir_bateria_soc(uint8_t pct);

// POR ONDE O TAB5 FALA COM A MALHA MESHTASTIC. Serial = o cabo Grove
// (PROTO/UART); Bluetooth = BLE pelo C6 interno, sem fio; Internet = MQTT
// pelo Wi-Fi, sem no nenhum. O PIN e o do pareamento BLE do Meshtastic
// (padrao 123456 nos aparelhos sem tela).
//
// O GROVE TEM PRIORIDADE: quando a sondagem ACHA um C6L no cabo, a via vira
// Serial sozinha (kernel/core.cpp). Depois disso a escolha manual vale ate o
// proximo achado - e a internet tambem pode ser escolhida assim, alem de
// cobrir a falta do no (ajustes_mesh_internet).
#define AJUSTES_MESH_SERIAL     0
#define AJUSTES_MESH_BLUETOOTH  1
#define AJUSTES_MESH_INTERNET   2
uint8_t  ajustes_mesh_via(void);

// MESH PELA INTERNET QUANDO O NO NAO ESTA CONECTADO: com o Wi-Fi conectado e
// sem o C6L (cabo ou Bluetooth), as mensagens chegam pelo servidor MQTT.
// Ligado de fabrica.
bool     ajustes_mesh_internet(void);
void     ajustes_definir_mesh_internet(bool ligado);
void     ajustes_definir_mesh_via(uint8_t via);
uint32_t ajustes_mesh_pin(void);
void     ajustes_definir_mesh_pin(uint32_t pin);
// O no escolhido mostra o PIN na tela (um novo a cada pareamento): o PIN salvo
// nao serve, e o sistema pergunta sempre. Aprendido na primeira recusa do PIN
// salvo; volta a false quando outro no e escolhido.
bool     ajustes_mesh_pin_na_tela(void);
void     ajustes_definir_mesh_pin_na_tela(bool sim);
// Endereco BLE do no escolhido ("" = o primeiro Meshtastic que aparecer).
void     ajustes_mesh_ble_endereco(char out[18]);
void     ajustes_definir_mesh_ble_endereco(const char* end);

// GPS DO TAB5 PARA A MALHA: de quantos em quantos MINUTOS o Tab5 passa a
// posicao do GPS (Grove) adiante. 0 = nunca (padrao: posicao e dado pessoal -
// so sai se o usuario pedir). Com o no do outro lado ela vai SO PARA O NO, que
// a difunde no ritmo e com a precisao dele (ver enviar_posicao em
// tab5_mesh.cpp); pela internet, sem no, o Tab5 difunde ele mesmo. Editado no
// app Mesh, Configurar o no > Localizacao.
uint8_t ajustes_mesh_posicao_min(void);
void    ajustes_definir_mesh_posicao_min(uint8_t minutos);

// BLUETOOTH LIGADO. Via Bluetooth no Mesh liga junto (ver o app Ajustes).
bool ajustes_bluetooth(void);
void ajustes_definir_bluetooth(bool ligado);

// Tela de bloqueio: ao acordar do repouso o aparelho mostra hora e data e pede
// um deslize para cima. Desligada de fabrica.
bool ajustes_bloqueio(void);
void ajustes_definir_bloqueio(bool ligado);

// PENDRIVE AO CONECTAR: ligado num computador pelo USB-C, o Tab5 abre o app
// Pendrive (o cartao SD vira um disco no computador). Desligado de fabrica: o
// pendrive tira o console do cabo, e quem grava o firmware pelo USB-C nao
// quer isso acontecendo sozinho. Com um app aberto nao interrompe - so avisa
// (central de avisos). O app Pendrive abre na mao com o ajuste desligado.
bool ajustes_pendrive(void);
void ajustes_definir_pendrive(bool ligado);

// LIXEIRA: apagar sozinho os itens com mais de N dias (30 = 30 dias). 0 = nunca
// (padrao: o que vai para a lixeira fica ate esvaziar). Quem aplica e o
// tab5_arq_lixeira_manter (services/tab5_arquivos).
uint8_t ajustes_lixeira_dias(void);
void    ajustes_definir_lixeira_dias(uint8_t dias);

// ESCALA DO GIROSCOPIO, por eixo do BMI270 (0 = x, 1 = y, 2 = z). O chip sai
// de fabrica com erro de sensibilidade de alguns por cento, e o rumo do Radar
// herda esse erro a cada volta. Medida no proprio Radar (voltas inteiras contra
// uma referencia); 1,0 = sem correcao. Faixa aceita 0,90..1,10.
float ajustes_giro_escala(int eixo);
void  ajustes_definir_giro_escala(int eixo, float escala);

// ORDEM DOS ICONES NA TELA INICIAL, como os NOMES dos apps separados por
// virgula ("Mesh,Trilha,Radar"). Nome e nao indice: um app novo no firmware
// ou um app removido nao embaralha o resto - quem nao esta na lista entra no
// fim, na ordem de apps.cpp, e nome que nao existe mais e ignorado. Vazio = a
// ordem de apps.cpp. Quem interpreta e o shell (sistema_home_*, app.h).
void ajustes_ordem_apps(char* out, size_t n);
void ajustes_definir_ordem_apps(const char* nomes);   // "" ou nullptr = padrao

// IDIOMA DA INTERFACE (o codigo do .po escolhido). Quem interpreta e
// services/idioma.h - a interface usa idioma_*, nao estes.
const char* ajustes_idioma(void);                 // "pt", "en", "fr"...
void        ajustes_definir_idioma(const char* codigo);

// PAPEL DE PAREDE DO INICIO (home): linhas abstratas finas sobre preto, os
// mesmos desenhos do fundo da tela de bloqueio. SORTEIO (o de fabrica) tira
// um desenho a cada boot e o mantem ate o proximo; 1..5 fixa um; NENHUM e o
// preto de antes.
#define AJUSTES_PAREDE_SORTEIO  0
#define AJUSTES_PAREDE_ESTILOS  5   // 1..5 = fios, ondas, teia, raios, circulos
#define AJUSTES_PAREDE_NENHUM   6

uint8_t ajustes_parede(void);
void    ajustes_definir_parede(uint8_t opcao);

// ---------------------------------------------------------------------------
// PRIMEIRO USO, FUSO E SERVIDOR DA MALHA PELA INTERNET
// ---------------------------------------------------------------------------

// A CONFIGURACAO INICIAL JA FOI FEITA? false so num aparelho que nunca rodou o
// sistema (NVS vazia - inclusive depois de gravar pelo M5Burner, que apaga
// os ajustes): o boot abre o assistente de boas-vindas (kernel/boas_vindas).
// Quem ja tinha QUALQUER ajuste gravado conta como configurado - atualizar o
// firmware nao traz o assistente de volta.
bool ajustes_config_inicial(void);
void ajustes_definir_config_inicial(bool feita);

// FUSO HORARIO, em minutos a partir do UTC (-180 = UTC-3, o padrao). So a
// EXIBICAO usa (tab5_hora_local): o RTC, os logs e os carimbos seguem em UTC.
// Fixo, sem horario de verao - quem tem verao ajusta duas vezes por ano.
#define AJUSTES_FUSO_MIN_MIN  (-12 * 60)
#define AJUSTES_FUSO_MAX_MIN  ( 14 * 60)
int16_t ajustes_fuso_min(void);
void    ajustes_definir_fuso_min(int16_t minutos);

// TIME-ZONE REGION (services/tab5_fuso): 0 = manual, the fixed offset above;
// any other region brings its own offset and daylight-saving rule. Picking a
// region also stores its standard offset in ajustes_fuso_min, so code that
// only reads the offset (the MQTT server guess, Berry's ajustes.fuso) keeps
// a sensible value.
// 12-HOUR CLOCK: times shown as "9:05pm" (tab5_hora_hhmm) instead of "21:05".
bool    ajustes_relogio_12h(void);
void    ajustes_definir_relogio_12h(bool sim);

uint8_t ajustes_fuso_zona(void);
void    ajustes_definir_fuso_zona(uint8_t zona);

// SERVIDORES MQTT DO MODO INTERNET do Mesh (sem no: o Tab5 fala com a malha
// pelo Wi-Fi). E UMA LISTA DA PESSOA, nao uma fixa no codigo: ate
// AJUSTES_MQTT_MAX servidores, cada um com nome, endereco, usuario, senha
// (guardada cifrada, como as do Wi-Fi - chave derivada do chip) e raiz dos
// topicos. Tamanhos: os do MQTT do Meshtastic.
//
// A ESCOLHA: 0 = AUTOMATICO, o comportamento de sempre (o servidor que o no
// ensinou, ou o da comunidade brasileira); 1..n = o item n-1 da lista, que
// vale por cima de tudo. Apagar um item acerta a escolha junto.
//
// De fabrica a lista traz o Mesh Brasil e o publico do Meshtastic na raiz do
// Brasil. Os MODELOS para adicionar e o sentido de cada campo moram em
// services/tab5_mesh_mqtt (a interface usa os mesh_mqtt_*, nao estes).
#define AJUSTES_MQTT_AUTOMATICO  0
#define AJUSTES_MQTT_MAX         8
typedef struct { char nome[24], srv[64], usr[32], pwd[32], raiz[48]; } ajustes_mqtt_srv_t;
uint8_t ajustes_mqtt_servidor(void);
void    ajustes_definir_mqtt_servidor(uint8_t escolha);
int     ajustes_mqtt_n(void);
bool    ajustes_mqtt_ler(int i, ajustes_mqtt_srv_t* out);          // i = 0..n-1
int     ajustes_mqtt_gravar(int i, const ajustes_mqtt_srv_t* s);   // i = -1: novo. Devolve o indice; -1 = cheia
void    ajustes_mqtt_apagar(int i);
