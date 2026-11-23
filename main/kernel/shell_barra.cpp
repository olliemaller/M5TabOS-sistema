// SHELL - a barra: icone de bateria, o desenho da barra de status (Wi-Fi,
// Bluetooth, ESP-NOW, recado, pastilha da musica), a hora, a faixa de baixo da
// home (temperatura, servicos GROVE/CARTAO) e o botao da navegacao. Partido de
// shell.cpp; o que as partes compartilham esta em shell_interno.hpp.

#include "tela_virtual.hpp"
#include "ajustes.h"
#include "idioma.h"
#include "tab5_ble.hpp"
#include "tab5_mesh_ble.hpp"
#include "tab5_wifi.hpp"
#include "tab5_espnow.hpp"
#include "tab5_tela.hpp"
#include "tab5_grove.hpp"
#include "tab5_sensores.hpp"
#include "notificacao.h"
#include "midia.h"
#include "tab5_mesh.hpp"
#include "tab5_gps.hpp"
#include "tab5_sd.hpp"
#include "tab5_ui.hpp"
#include "tab5_tema.hpp"
#include "tab5_hora.hpp"
#include <time.h>
#include <string.h>
#include <stdio.h>
#include "shell_interno.hpp"

// ---------------------------------------------------------------------------
// Icone de bateria
// ---------------------------------------------------------------------------

// Tres estados de alimentacao, nao dois. O erro anterior era detectar "carregando"
// so pela corrente positiva: a 100% o carregador para, a corrente vai a zero e o
// raio sumia, mesmo com o cabo na tomada.
//
// O discriminador e simples e confiavel: o sistema consome mais de 100mA. Se
// essa corrente NAO esta saindo da bateria, ela so pode estar vindo do USB.
typedef enum {
  EN_BATERIA,      // descarregando: corrente claramente negativa
  EN_CARREGANDO,   // corrente entrando na bateria
  EN_TOMADA        // alimentado por fora, bateria sem trocar carga (cheia)
} estado_energia_t;

static estado_energia_t estado_energia(const tab5_medida_t& m)
{
  if (m.mA < -20.0f) { return EN_BATERIA;    }
  if (m.mA >  20.0f) { return EN_CARREGANDO; }
  return EN_TOMADA;
}

// Raio de carregamento, desenhado com contorno para ficar legivel tanto sobre a
// parte cheia quanto sobre a parte vazia da bateria.
static void desenhar_raio(lgfx::LovyanGFX& d, int cx, int cy, uint16_t cor)
{
  d.fillTriangle(cx + 2, cy - 10, cx - 7, cy + 2, cx + 1, cy + 2, cor);
  d.fillTriangle(cx - 2, cy + 10, cx + 7, cy - 2, cx - 1, cy - 2, cor);
}

// Plugue, para quando esta na tomada com a bateria ja cheia - o mesmo que o
// celular mostra quando termina de carregar e voce nao tirou o cabo.
//
// Dimensoes apertadas de proposito: ele desenha DENTRO da bateria, que tem 30px
// de altura e 3px de borda de cada lado. Sobram 24px, e este plugue ocupa 19.
static void desenhar_tomada(lgfx::LovyanGFX& d, int cx, int cy, uint16_t cor)
{
  d.fillRect(cx - 5, cy - 9, 2, 5, cor);           // pino esquerdo
  d.fillRect(cx + 3, cy - 9, 2, 5, cor);           // pino direito
  d.fillRoundRect(cx - 7, cy - 4, 14, 9, 2, cor);  // corpo
  d.fillRect(cx - 1, cy + 5, 2, 5, cor);           // cabo
}

// Bateria estilo celular: contorno arredondado, polo, preenchimento proporcional
// e o simbolo central quando esta carregando ou na tomada.
static void desenhar_bateria(lgfx::LovyanGFX& d, int x, int y, int pct, estado_energia_t est)
{
  const int L = 58;              // largura do corpo
  const int A = 30;              // altura do corpo
  const int borda = 3;

  const int p = (pct < 0) ? 0 : (pct > 100 ? 100 : pct);

  // Verde com carregador ou carga boa; amarelo abaixo de 50%; vermelho abaixo de 20%.
  const uint16_t cor = (est != EN_BATERIA) ? (uint16_t)TEMA_SUCESSO
                     : (p < 20)            ? (uint16_t)TEMA_PERIGO
                     : (p < 50)            ? (uint16_t)TEMA_ALERTA
                                           : (uint16_t)TEMA_SUCESSO;

  // Corpo: interior apagado e depois preenchido ate a fracao da carga.
  // TEMA_FUNDO: este fillRect limpa a area do icone E pinta a parte VAZIA da
  // bateria, que tem de ter a mesma cor da barra.
  d.fillRect(x, y, L + 6, A, TEMA_FUNDO);                           // limpa a area toda
  d.drawRoundRect(x, y, L, A, 6, TEMA_TEXTO_2);
  d.drawRoundRect(x + 1, y + 1, L - 2, A - 2, 5, TEMA_TEXTO_2);     // 2px de moldura
  d.fillRect(x + L + 1, y + A / 2 - 6, 5, 12, TEMA_TEXTO_2);        // polo positivo

  const int util = L - 2 * borda;
  const int cheio = (util * p) / 100;
  if (cheio > 0) {
    d.fillRoundRect(x + borda, y + borda, cheio, A - 2 * borda, 3, cor);
  }

  // Simbolo central. Mesma tecnica nos dois casos: contorno na cor do fundo
  // deslocado de 1px e figura clara por cima, para ficar legivel tanto sobre a
  // parte cheia quanto sobre a parte vazia da bateria.
  if (est == EN_CARREGANDO) {
    desenhar_raio(d, x + L / 2 + 1, y + A / 2 + 1, TEMA_FUNDO);
    desenhar_raio(d, x + L / 2,     y + A / 2,     TEMA_TEXTO);
  } else if (est == EN_TOMADA) {
    desenhar_tomada(d, x + L / 2 + 1, y + A / 2 + 1, TEMA_FUNDO);
    desenhar_tomada(d, x + L / 2,     y + A / 2,     TEMA_TEXTO);
  }

  // Se o modelo nao souber estimar (pct < 0), marca com um tracinho.
  if (pct < 0) {
    d.drawFastHLine(x + borda + 4, y + A / 2, util - 8, TEMA_TEXTO_2);
  }
}

// ---------------------------------------------------------------------------
// Desenho
// ---------------------------------------------------------------------------
// Icone de WiFi: quatro arcos concentricos, aceso conforme o sinal.
//
// Simbolo em vez de texto porque a barra e lida de relance: forma e cor chegam
// antes da leitura. O nome da rede continua disponivel no app Ajustes, que e
// onde voce vai quando quer detalhe.
//
// Faixas de RSSI (dBm), as usuais de WiFi:
//   >= -55 excelente (4) | >= -65 bom (3) | >= -75 ok (2) | >= -85 fraco (1)
// O X FICA AO LADO, E NAO POR CIMA.
//
// A versao anterior riscava o proprio leque com dois tracos grossos. Ficou
// legivel e ficou horrivel: o X brigava com os arcos pelo mesmo espaco e o
// simbolo virava um borrao. A convencao boa mantem o
// leque intacto e poe o X SOLTO no canto de cima, a direita.
//
// Para isso o leque anda um pouco para a esquerda e encolhe, abrindo o canto.
// O icone inteiro passa a ocupar ~46px de largura em vez de ~40.
// Termometro de ~14x31 (era 12x26, +20%junto da troca de
// fonte), desenhado a partir do centro do bulbo. Bulbo, haste e a coluna
// colorida dentro dela - o mesmo desenho serve para qualquer cor, entao a
// temperatura pinta o liquido e o vidro fica sempre claro.
//
// ICONE VETORIAL, NAO ACOMPANHA FONTE SOZINHO - por isso os literais abaixo
// sao os originais * 1.2 (arredondados), e nao uma conta em tempo de execucao:
// e um desenho fixo, chamado de um unico lugar, sem motivo para reprocessar
// escala a cada frame.
// A COR DA TEMPERATURA, continua: azul frio -> verde -> amarelo -> laranja ->
// vermelho. Antes eram tres faixas (azul ate 55, laranja, vermelho a 70) e o
// termometro passava a maior parte do dia no mesmo azul - nao contava nada.
// Os degraus seguem o que este chip faz de verdade: ~40 parado, ~50 com a tela
// e o radio ocupados, 55+ trabalhando, 70+ quente.
static uint16_t cor_da_temp(float c)
{
  static const struct { float t; uint8_t r, g, b; } P[] = {
    // Os mesmos tons do tema (INFO, SUCESSO, ALERTA, APP_LARANJA, PERIGO): o
    // gradiente precisa dos canais RGB para interpolar, entao aqui vao os
    // valores 24 bits e nao as constantes ja reduzidas para 565.
    { 35.0f,  0x39, 0xB8, 0xE6 },   // azul    (TEMA_INFO)
    { 45.0f,  0x34, 0xC7, 0x7B },   // verde   (TEMA_SUCESSO)
    { 55.0f,  0xF5, 0xB8, 0x3D },   // amarelo (TEMA_ALERTA)
    { 63.0f,  0xF2, 0x7B, 0x3D },   // laranja (TEMA_APP_LARANJA)
    { 70.0f,  0xFF, 0x5C, 0x5C },   // vermelho (TEMA_PERIGO)
  };
  const int n = sizeof(P) / sizeof(P[0]);
  int r = P[0].r, g = P[0].g, b = P[0].b;
  if (c >= P[n - 1].t) { r = P[n - 1].r; g = P[n - 1].g; b = P[n - 1].b; }
  else {
    for (int i = 0; i + 1 < n; ++i) {
      if (c >= P[i].t && c < P[i + 1].t) {
        const float k = (c - P[i].t) / (P[i + 1].t - P[i].t);
        r = P[i].r + (int)((P[i + 1].r - P[i].r) * k);
        g = P[i].g + (int)((P[i + 1].g - P[i].g) * k);
        b = P[i].b + (int)((P[i + 1].b - P[i].b) * k);
        break;
      }
    }
  }
  return (uint16_t)(((r & 0xF8) << 8) | ((g & 0xFC) << 3) | (b >> 3));
}

// A COLUNA SOBE COM A TEMPERATURA (20 a 80 C, a faixa do sensor) e o liquido
// toma a cor de cor_da_temp(). Sem leitura: vidro vazio, bulbo cinza.
// Ocupa de cy-20 a cy+12.
static void desenhar_termometro(lgfx::LovyanGFX& d, int cx, int cy, bool tem, float c,
                                uint16_t fundo)
{
  const uint16_t vidro   = TEMA_TEXTO_2;
  const uint16_t liquido = tem ? cor_da_temp(c) : (uint16_t)TEMA_TEXTO_3;

  // Haste: contorno claro com o miolo na cor do fundo, para o liquido ter onde
  // aparecer sem o vidro sumir.
  d.fillRoundRect(cx - 5, cy - 20, 10, 24, 5, vidro);
  d.fillRoundRect(cx - 2, cy - 18, 5, 22, 2, fundo);
  if (tem) {
    float f = (c - 20.0f) / 60.0f;
    f = f < 0.0f ? 0.0f : (f > 1.0f ? 1.0f : f);
    const int h = 2 + (int)(f * 20.0f + 0.5f);        // 2..22 px de coluna
    d.fillRect(cx - 2, cy + 4 - h, 5, h, liquido);
  }

  d.fillCircle(cx, cy + 5, 7, vidro);
  d.fillCircle(cx, cy + 5, 5, liquido);               // bulbo
}

static void desenhar_wifi(lgfx::LovyanGFX& d, int cx, int cy, uint16_t cor,
                          int barras, bool cortado)
{
  // Leque deslocado: o centro dele nao e mais o centro do icone.
  cx -= 7;

  // Desenhado como arcos: o ponto na base e mais tres coroas. fillArc cobre de
  // 225 a 315 graus, que e o leque de 90 graus apontando para cima.
  // Apagado em TEMA_TEXTO_3 (o "desativado" do tema): sobre o fundo escuro o
  // leque "vazio" nao chama mais atencao que o cheio.
  const uint16_t off = TEMA_TEXTO_3;
  d.fillCircle(cx, cy + 9, 3, barras >= 1 ? cor : off);
  for (int n = 1; n <= 3; ++n)
  {
    const int r = 5 + n * 5;
    d.fillArc(cx, cy + 9, r, r - 2, 225, 315,
              (barras >= n + 1) ? cor : off);
  }

  if (cortado)
  {
    // POR CIMA da ponta do leque, comendo um pedaco dele - como no simbolo
    // usual de "sem conexao". Nao e decoracao: o X sobreposto diz que aquele leque esta
    // interrompido, enquanto um X ao lado dizia so "tem um X aqui tambem".
    //
    // 3px de espessura. A versao anterior tinha 4 mais um contorno preto de 6,
    // e o conjunto ficava mais pesado que o proprio leque que devia negar.
    const int ex = cx + 16;
    const int ey = cy - 4;
    const int b  = 6;
    tab5_linha_grossa(d, ex - b, ey - b, ex + b, ey + b, 3, TEMA_PERIGO);
    tab5_linha_grossa(d, ex + b, ey - b, ex - b, ey + b, 3, TEMA_PERIGO);
  }
}

// MODO ESP-NOW. Com o canal fixo, o WiFi solta o roteador DE
// PROPOSITO - o radio esta de pe, conversando por ESP-NOW. O leque cinza com X
// dizia "sem conexao", que e mentira: ha conexao, so que outra. Neste modo a
// barra mostra o simbolo do ESP-NOW (o mesmo do icone do app: ponto de origem
// e tres ondas saindo) - ciano no ar, cinza com o radio parado.
static void desenhar_espnow(lgfx::LovyanGFX& d, int cx, int cy, bool no_ar)
{
  const uint16_t cor = no_ar ? (uint16_t)TEMA_INFO : (uint16_t)TEMA_TEXTO_3;
  const int ox = cx - 9, oy = cy + 9;   // o ponto de origem, embaixo a esquerda
  d.fillCircle(ox, oy, 3, cor);
  for (int n = 1; n <= 3; ++n) {
    const int r = 3 + n * 6;
    d.fillArc(ox, oy, r, r - 2, 270, 360, cor);
  }
}

// BLUETOOTH NA BARRA: a runa, a esquerda do WiFi, so enquanto o
// Bluetooth esta ligado. Cor: destaque = conectado a um no (Mesh BLE); texto
// = ligado; cinza apagado = subindo; vermelho = falhou.
static int bt_visual(void)
{
  const tab5_ble_estado_t e = tab5_ble_estado();
  if (e == TAB5_BLE_DESLIGADO) { return 0; }
  if (e == TAB5_BLE_FALHOU)    { return 3; }
  if (e == TAB5_BLE_SUBINDO || e == TAB5_BLE_DESLIGANDO) { return 1; }
  return mesh_ble_conectado() ? 4 : 2;
}

static void desenhar_bt(lgfx::LovyanGFX& d, int cx, int cy, int v)
{
  const uint16_t cor = (v == 4) ? (uint16_t)TEMA_DESTAQUE : (v == 3) ? (uint16_t)TEMA_PERIGO
                     : (v == 2) ? (uint16_t)TEMA_TEXTO    : (uint16_t)TEMA_TEXTO_3;
  const int t = cy - 12, b = cy + 12, dx = 7, q = 6;
  for (int k = 0; k <= 1; ++k) {
    d.drawLine(cx + k, t, cx + k, b, cor);
    d.drawLine(cx + k, t, cx + dx + k, t + q, cor);
    d.drawLine(cx + dx + k, t + q, cx - dx + k, b - q, cor);
    d.drawLine(cx + k, b, cx + dx + k, b - q, cor);
    d.drawLine(cx + dx + k, b - q, cx - dx + k, t + q, cor);
  }
}

// O que a barra MOSTRA do radio: o estado do WiFi, ou o modo ESP-NOW (100 no
// ar, 101 parado). Um numero so para o desenho e para o barra_mudou - os dois
// tem que concordar, senao trocar de modo nao repinta.
static int radio_visual(void)
{
  if (tab5_espnow_canal_fixo()) { return tab5_espnow_pronto() ? 100 : 101; }
  return (int)tab5_wifi_estado();
}

// Ultimo estado desenhado na barra, para nao repintar quando nada mudou.
static int  s_barra_temp = -999;  // ultimo grau INTEIRO desenhado (so na tela de bloqueio)
static char s_barra_hora[8] = ""; // a hora desenhada no canto (fora do bloqueio)
static bool s_barra_bloq = false; // o canto foi pintado como tela de bloqueio
static int  s_barra_pct  = -999;
static int  s_barra_wifi = -1;    // estado de WiFi com que a barra foi pintada
static int  s_barra_bt   = -1;    // estado de Bluetooth com que a barra foi pintada
static uint32_t s_barra_notif = 0;   // versao de notificacao ja desenhada
static estado_energia_t s_barra_estado = EN_BATERIA;
static const char* s_barra_nome = nullptr;
static uint32_t s_barra_midia = 0;   // versao de midia (kernel/midia.h) ja desenhada
static uint32_t s_barra_idioma = 0;  // versao do idioma (services/idioma.h) ja desenhada

// RECADO (sistema_recado, app.h): a frase e ate quando ela fica no lugar do
// nome do app. s_barra_recado e a que esta desenhada ("" = o nome).
#define RECADO_MS 2000
static char     s_recado[32]      = "";
static uint32_t s_recado_ate      = 0;
static char     s_barra_recado[32] = "";

static const char* recado_agora(void)
{
  return (s_recado[0] && (int32_t)(s_recado_ate - tab5_agora_ms()) > 0) ? s_recado : "";
}

void sistema_recado(const char* texto)
{
  snprintf(s_recado, sizeof(s_recado), "%s", texto ? texto : "");
  s_recado_ate = tab5_agora_ms() + RECADO_MS;
  sistema_acordar_interface();
}

bool recado_pendente(void) { return strcmp(recado_agora(), s_barra_recado) != 0; }

// Roboto suave na altura nativa mais proxima de 'px' (so a barra usa tamanhos
// que o tema nao nomeia: a maquete pede 21-26 px).
static void fonte_barra(lgfx::LovyanGFX& d, bool negrito, int px)
{
  d.setFont(tab5_fonte_suave(negrito, px));
  d.setTextSize(1);
}

// A PASTILHA DA MUSICA, ancorada a direita em 'dir'. Na cor de destaque do
// tema (fundo suave, acento forte), para nao se confundir com nenhuma
// gravidade de aviso: nota (tocando) ou pausa, e o titulo cortado ao que
// couber. Devolve a largura usada (0 = nao coube).
static const int MIDIA_PAST_MAX = 320;
static int desenhar_pastilha_midia(lgfx::LovyanGFX& d, int dir, int max_l, const midia_info_t& mi)
{
  const int alt = ALTURA_BARRA - 24, cy = ALTURA_BARRA / 2;   // 40 px, como as pilulas de aviso
  const int ICONE = 22, PAD = 14;
  if (max_l < alt) { return 0; }
  char t[MIDIA_TITULO];
  snprintf(t, sizeof(t), "%s", mi.titulo);
  tab5_texto(d, 3);
  tab5_fonte_corpo_negrito(d);
  const int cabe_txt = max_l - (PAD + ICONE + 10 + PAD);
  if (cabe_txt >= 40) { tab5_cortar_com_reticencias(d, t, sizeof(t), cabe_txt); }
  else                { t[0] = '\0'; }
  const int larg = t[0] ? PAD + ICONE + 10 + (int)d.textWidth(t) + PAD : alt;
  const int x = dir - larg;
  d.fillRoundRect(x, (ALTURA_BARRA - alt) / 2, larg, alt, alt / 2, TEMA_DESTAQUE_SUAVE);
  const int ix = t[0] ? x + PAD : x + (alt - ICONE) / 2;
  if (mi.tocando) {
    // Colcheia: cabeca, haste e bandeirola.
    d.fillEllipse(ix + 7, cy + 7, 7, 5, TEMA_DESTAQUE);
    d.fillRect(ix + 12, cy - 11, 3, 18, TEMA_DESTAQUE);
    d.fillTriangle(ix + 12, cy - 11, ix + 22, cy - 4, ix + 12, cy - 3, TEMA_DESTAQUE);
  } else {
    d.fillRect(ix + 4,  cy - 9, 5, 18, TEMA_DESTAQUE);
    d.fillRect(ix + 14, cy - 9, 5, 18, TEMA_DESTAQUE);
  }
  if (t[0]) {
    d.setTextDatum(middle_left);
    d.setTextColor(TEMA_TEXTO, TEMA_DESTAQUE_SUAVE);
    d.drawString(t, ix + ICONE + 10, cy);
  }
  tab5_fonte_corpo(d);
  return larg;
}
// A COR DE CADA GRAVIDADE - uma so, para a barra, a central e o bloqueio
// falarem a mesma lingua.
uint16_t cor_do_nivel(notif_nivel_t n)
{
  return (n >= NOTIF_URGENTE) ? (uint16_t)TEMA_PERIGO
       : (n == NOTIF_ATENCAO) ? (uint16_t)TEMA_ALERTA
       : (n == NOTIF_INFO)    ? (uint16_t)TEMA_INFO
                              : (uint16_t)TEMA_TEXTO_2;   // ESTADO: atividade, nao novidade
}

// completo=true pinta o FUNDO da barra antes dos elementos. E preciso sempre
// que a tela tenha sido apagada (abertura, volta do repouso, troca de app),
// senao sobra o preto do framebuffer e a barra parece ter sumido - foi o que
// aconteceu quando o fillRect foi removido achando que era so o flash.
//
// completo=false e a atualizacao periodica: os elementos se sobrescrevem
// sozinhos (texto com cor de fundo, icones limpando o proprio retangulo) e
// pintar o fundo inteiro aqui seria justamente o flash que a gente tirou.
void desenhar_barra_status(lgfx::LovyanGFX& d, bool completo)
{
  const int L = d.width();
  char buf[32];

  // Le do CACHE. A leitura de verdade acontece uma vez por segundo no loop da
  // tarefa; ir ao I2C aqui de novo seria uma transacao a mais por redesenho da
  // barra, sem nenhum dado novo.
  tab5_medida_t m = tab5_power_ultima();
  const estado_energia_t est = estado_energia(m);

  // EM FAIXAS. 1280x64 em deitado sao ~164KB escritos de uma vez, e a barra e
  // repintada em toda troca de app - o mesmo tipo de rajada que produzia a
  // piscada, so que menor. tab5_tela_limpar respira entre as faixas.
  if (completo) {
    tab5_tela_limpar(0, 0, L, ALTURA_BARRA, TEMA_FUNDO);
    // Os blocos da esquerda, meio e direita limpam os proprios retangulos logo
    // abaixo, todos em TEMA_FUNDO e todos dentro deste. Marcados, eles somem.
    tab5_tela_marcar_limpo(0, 0, L, ALTURA_BARRA, TEMA_FUNDO);
  }

  // Fora do 'completo', NAO apagamos a barra inteira.
  //
  // O fillRect de largura total era um flash visivel a cada repintura - discreto
  // enquanto o meio da barra era texto, evidente depois que virou um icone com
  // arcos. Cada elemento agora se vira: o texto escreve com cor de fundo (que
  // sobrescreve em vez de limpar) e os dois icones limpam so o proprio retangulo.
  //
  // Preco disso: todo texto daqui PRECISA ter largura fixa. Um nome de app mais
  // curto que o anterior deixaria rabo do antigo na tela.
  // ---- ESQUERDA: a hora, e o app logo em seguida ----------------------
  //
  // Os dois juntos de proposito. Sao as duas coisas que voce le sem procurar -
  // que horas sao e onde estou - e o MEIO fica inteiro para os avisos, que e
  // conteudo que aparece e some. A hora ficava na faixa de baixo e a
  // temperatura do P4 aqui; trocaram de lugar: a hora e o que se consulta o
  // tempo todo, e o canto de cima e onde todo celular a poe.
  //
  // NA TELA DE BLOQUEIO, NAO: ela ja tem a hora grande no meio, e a hora
  // repetida aqui em cima seria a mesma informacao duas vezes. Ali o canto
  // continua com o P4 e a temperatura, como era.
  //
  // ESQ_FIM e a fronteira, e ela existe porque o bloco do meio ja comeu o nome
  // do app uma vez: os dois limpavam retangulos que se cruzavam, e quem
  // desenhasse depois vencia. Agora cada lado sabe onde para.
  static const int ROT_X          = 16;
  static const int GAP_ROT_TERMO  = 14;  // espaco entre "P4" e o termometro
  static const int GAP_TERMO_TEMP = 15;  // espaco entre o termometro (+20%) e o numero
  // O NOME DO APP COLA NA HORA, com largura MAXIMA. Ele ficava num x fixo
  // (168) e ia ate onde o texto fosse: um nome comprido ("Configuracoes",
  // um app Berry) passava da fronteira e a pastilha de avisos desenhava por
  // cima. Agora o nome comeca logo depois da hora, e o que passar de
  // APP_MAX_L vira "..." - o resto da barra e dos avisos e da musica.
  static const int GAP_ESQ_APP = 22;    // da hora (ou da temperatura) ao nome
  static const int APP_MAX_L   = 190;   // o nome do app nunca passa disto
  static const int ESQ_MAX     = 420;   // o canto esquerdo nunca passa disto (o que se limpa)

  tab5_tela_limpar(0, 4, ESQ_MAX, ALTURA_BARRA - 8, TEMA_FUNDO);
  int esq_x = ROT_X;   // onde acabou o que foi desenhado a esquerda do nome
  if (!s_bloqueado) {
    hora_agora(buf, sizeof(buf));
    fonte_barra(d, true, 26);   // Roboto medio/negrito, como a hora da maquete
    d.setTextDatum(middle_left);
    d.setTextColor(TEMA_TEXTO, TEMA_FUNDO);
    d.drawString(buf, ROT_X, ALTURA_BARRA / 2);
    esq_x = ROT_X + (int)d.textWidth(buf);
    tab5_fonte_corpo(d);
  } else {
    // Ordem de leitura: "P4", o termometro, o numero. O rotulo diz DE QUEM e a
    // medida, o simbolo diz o QUE e, o numero diz QUANTO.
    float c = 0.0f;
    const bool tem = tab5_sensores_temp(&c);
    tab5_texto(d, 3.0f);   // +20%
    d.setTextDatum(middle_left);
    d.setTextColor(TEMA_TEXTO_2, TEMA_FUNDO);
    d.drawString("P4", ROT_X, ALTURA_BARRA / 2);
    // Medido, nao fixo: a largura de "P4" depende da fonte do momento.
    const int termo_x = ROT_X + (int)d.textWidth("P4") + GAP_ROT_TERMO;
    desenhar_termometro(d, termo_x, ALTURA_BARRA / 2, tem, c, TEMA_FUNDO);
    if (tem) { snprintf(buf, sizeof(buf), "%.0f°C", (double)c); }
    else     { snprintf(buf, sizeof(buf), "--"); }
    d.drawString(buf, termo_x + GAP_TERMO_TEMP, ALTURA_BARRA / 2);
    esq_x = termo_x + GAP_TERMO_TEMP + (int)d.textWidth(buf);
  }

  // Nome do app em BRANCO e mais encorpado que o resto: e a resposta para "onde
  // eu estou", que se le antes de qualquer outra coisa na barra. E o unico
  // TITULO que aparece em toda tela do sistema, entao e o unico texto da
  // barra que troca para a Orbitron_Light (ver tab5_fonte_titulo em
  // tab5_ui.hpp) - o resto da barra (P4, wifi, bateria) sao leituras
  // curtas, nao titulo, e ficam na fonte de corpo.
  //
  // TAMANHO NATIVO (1) e a base ideal para uma fonte bitmap como a Orbitron -
  // escalar por uma fracao descarta/duplica linha de pixel de um jeito
  // irregular e engrossa o traco (foi por isso que "CONFIG" na barra parecia
  // uma fonte DIFERENTE da amostra da galeria em Ajustes). Mas 1.0 ficou
  // grande demais aqui na praticafoi reduzir uns 20%,
  // entao 0.8 (ainda bem mais limpo que uma fracao tipo 0.6, so um pouco
  // aquem do nativo).
  //
  // O negrito duplo-com-deslocamento que existia aqui era remendo para a
  // Font0 nao ter peso - a Orbitron_Light ja e uma fonte "leve" (light) de
  // proposito grafico, entao NAO desenhamos duas vezes: a segunda copia
  // deslocada 1px ficaria borrada, nao "em negrito", numa fonte com traco
  // fino e regular como esta.
  // Roboto negrito nativa (ver tab5_titulo em tab5_ui.hpp)
  // - era a FreeMonoBold a 1,04x, a ultima escala fracionaria da barra.
  tab5_titulo(d, 28);
  d.setTextDatum(middle_left);
  d.setTextColor(TEMA_TEXTO, TEMA_FUNDO);
  const char* recado = recado_agora();
  snprintf(s_barra_recado, sizeof(s_barra_recado), "%s", recado);
  const int app_x = esq_x + GAP_ESQ_APP;
  int ESQ_FIM;
  if (recado[0]) {
    // O RECADO e uma pilula (superficie alta com borda fina) no lugar do nome,
    // e nao texto solto: destaca-se do nome do app sem precisar de cor propria.
    // O texto encolhe para a pilula inteira caber em APP_MAX_L.
    const int PAD = 14, ALT = ALTURA_BARRA - 24;
    snprintf(buf, sizeof(buf), "%s", recado);
    tab5_cortar_com_reticencias(d, buf, sizeof(buf), APP_MAX_L - 2 * PAD);
    const int larg = (int)d.textWidth(buf) + 2 * PAD;
    const int py = (ALTURA_BARRA - ALT) / 2;
    d.fillRoundRect(app_x, py, larg, ALT, ALT / 2, TEMA_SUPERFICIE_ALTA);
    d.drawRoundRect(app_x, py, larg, ALT, ALT / 2, TEMA_LINHA);
    d.setTextColor(TEMA_TEXTO, TEMA_SUPERFICIE_ALTA);
    d.drawString(buf, app_x + PAD, ALTURA_BARRA / 2);
    ESQ_FIM = app_x + larg;
  } else {
    snprintf(buf, sizeof(buf), "%s", s_app ? tr(s_app->nome) : tr("Início"));
    tab5_cortar_com_reticencias(d, buf, sizeof(buf), APP_MAX_L);
    d.drawString(buf, app_x, ALTURA_BARRA / 2);
    ESQ_FIM = app_x + (int)d.textWidth(buf);
  }
  tab5_fonte_corpo(d);

  // Estado do WiFi. Tudo daqui sai de cache: tab5_wifi_* nunca vai ao C6, quem
  // conversa pelo SDIO e a tarefa do servico, uma vez a cada 5s.
  const tab5_wifi_estado_t ew = tab5_wifi_estado();

  uint16_t cor_wifi = TEMA_TEXTO_3;   // desligado
  int      barras   = 0;
  bool     cortado  = true;

  if (ew == TAB5_WIFI_CONECTADO)
  {
    cor_wifi = TEMA_TEXTO;
    cortado  = false;
    const int r = (int)tab5_wifi_rssi();
    // Sem leitura de sinal, mostramos o icone CHEIO em vez de vazio: estamos
    // conectados de fato (temos IP), e desenhar zero barras diria ao olho que a
    // conexao esta ruim - trocaria uma informacao ausente por uma errada.
    barras = (r == TAB5_WIFI_RSSI_DESCONHECIDO) ? 4
           : (r >= -55) ? 4 : (r >= -65) ? 3 : (r >= -75) ? 2 : 1;
  }
  else if (ew == TAB5_WIFI_SUBINDO || ew == TAB5_WIFI_CONECTANDO)
  {
    // Amarelo e SEM o X: esta tentando, ainda nao falhou. Mostrar o X aqui
    // faria a tela dizer "sem conexao" durante uma conexao em andamento.
    cor_wifi = TEMA_ALERTA;
    barras   = 0;
    cortado  = false;
  }
  else if (ew == TAB5_WIFI_FALHOU || ew == TAB5_WIFI_SEM_CREDENCIAL
        || ew == TAB5_WIFI_C6_MUDO)
  {
    // C6_MUDO tem que estar AQUI e em Ajustes: nao esta desligado, esta
    // QUEBRADO, e a diferenca decide se voce vai ao Ajustes religar (inutil) ou
    // atras do gravador. Todo estado novo do enum precisa ser revisitado nos
    // dois lugares.
    cor_wifi = TEMA_PERIGO;
  }

  // ---- DIREITA: aviso, wifi, bateria ----------------------------------
  //
  // Tudo agrupado no mesmo canto de proposito. O wifi ficava no MEIO da barra,
  // longe da bateria, e os dois sao a mesma pergunta - "como esta o aparelho
  // agora". Separados, o olho tinha que varrer a barra inteira para juntar.
  //
  // As posicoes saem da direita para a esquerda, ancoradas no icone da
  // bateria: assim mexer numa largura nao desalinha as outras.
  const int bx      = L - 16 - 64;        // icone da bateria
  const int pct_dir = bx - 12;            // borda direita do "100%"
  // 82: o suficiente para o braco direito do X (que chega a cx+22) nao encostar
  // no numero da porcentagem, e nao mais que isso - a 96 o icone ficava solto
  // no meio do caminho, longe do grupo a que pertence.
  const int wifi_cx = pct_dir - 82;       // centro do icone de wifi

  // O MEIO E SO DOS AVISOS, e ele comeca depois de ESQ_FIM. Antes o retangulo
  // do aviso era ancorado no wifi e crescia 150px para a esquerda, entrando por
  // cima do nome do app - era isso que comia o nome.
  const int bt_v     = bt_visual();
  const int bt_cx    = wifi_cx - 46;
  const int meio_ini = ESQ_FIM + 8;
  int meio_fim = bt_v ? (bt_cx - 18) : (wifi_cx - 30);

  notif_item_t lista[NOTIF_MAX_FONTES];
  int n_avisos = notif_listar(lista, NOTIF_MAX_FONTES);
  // TOCANDO AGORA (kernel/midia.h): pastilha propria, a direita do meio, e os
  // avisos no que sobra - musica tocando nao esconde mensagem nova, nem o
  // contrario. A pastilha cinza da parte de fundo do tocador sai da lista:
  // a da musica ja diz que ele esta rodando, e com mais informacao.
  midia_info_t mi;
  const bool tem_midia = midia_ler(&mi);
  if (tem_midia && mi.fonte) {
    int k = 0;
    for (int i = 0; i < n_avisos; ++i) {
      if (strcmp(lista[i].nome, mi.fonte) != 0) { lista[k++] = lista[i]; }
    }
    n_avisos = k;
  }
  tab5_tela_limpar(meio_ini, 4, meio_fim - meio_ini, ALTURA_BARRA - 8, TEMA_FUNDO);
  if (tem_midia) {
    const int vao = meio_fim - meio_ini;
    const int max_l = n_avisos ? (vao / 2 < MIDIA_PAST_MAX ? vao / 2 : MIDIA_PAST_MAX)
                               : (vao < MIDIA_PAST_MAX ? vao : MIDIA_PAST_MAX);
    const int l = desenhar_pastilha_midia(d, meio_fim, max_l, mi);
    meio_fim -= l + 12;
  }
  s_barra_midia = midia_versao();
  if (n_avisos > 0) {
    // OS AVISOS NA BARRA SAO PILULAS QUE DIVIDEM O MEIO: cada app com
    // novidade ganha uma pilula com o icone mini e o resumo ao lado ("Fulano:
    // oi" no Mesh), da direita para a esquerda (o mais grave primeiro, a ordem
    // do notif_listar). Ate 3; com mais, as 3 primeiras e um "+N" do lado. O
    // espaco e repartido em partes iguais: um aviso so pode ser largo, tres
    // dividem; sem espaco para texto a pilula fica so com o icone. O texto
    // inteiro continua na central (tocar a barra).
    const int MAX_P = 3, ALT = ALTURA_BARRA - 24, PY = (ALTURA_BARRA - ALT) / 2, GAP = 8;
    const int PONTO = 10, PAD = 14, PIL_MAX = 300, TXT_MIN = 48;   // medidas da maquete
    const int mostra = n_avisos < MAX_P ? n_avisos : MAX_P;
    int dir = meio_fim;
    if (n_avisos > mostra) {
      char mais[8];
      snprintf(mais, sizeof(mais), "+%d", n_avisos - mostra);
      fonte_barra(d, false, 22);
      const int lm = (int)d.textWidth(mais) + 2 * PAD;
      d.fillRoundRect(dir - lm, PY, lm, ALT, ALT / 2, TEMA_SUPERFICIE);
      d.setTextDatum(middle_center);
      d.setTextColor(TEMA_TEXTO_2, TEMA_SUPERFICIE);
      d.drawString(mais, dir - lm / 2, ALTURA_BARRA / 2);
      dir -= lm + GAP;
    }
    int larg = (dir - meio_ini - (mostra - 1) * GAP) / (mostra > 0 ? mostra : 1);
    if (larg > PIL_MAX) { larg = PIL_MAX; }
    const bool com_txt = larg >= PAD + PONTO + 8 + TXT_MIN + PAD;
    if (!com_txt) { larg = ALT; }   // sem texto: so o pontinho, numa pilula redonda
    fonte_barra(d, false, 22);
    for (int k = 0; k < mostra && dir - larg >= meio_ini; ++k) {
      const notif_item_t& a = lista[k];
      // Pilula na superficie do tema; de ATENCAO para cima ganha borda de 2 px
      // na cor do nivel (a mesma do pontinho), para o grave saltar aos olhos.
      const uint16_t cor_nivel = cor_do_nivel(a.nivel);
      const int x = dir - larg;
      d.fillRoundRect(x, PY, larg, ALT, ALT / 2, TEMA_SUPERFICIE);
      if (a.nivel >= NOTIF_ATENCAO) {
        d.drawRoundRect(x, PY, larg, ALT, ALT / 2, cor_nivel);
        d.drawRoundRect(x + 1, PY + 1, larg - 2, ALT - 2, ALT / 2 - 1, cor_nivel);
      }
      const int px = com_txt ? x + PAD + PONTO / 2 : x + larg / 2;
      d.fillCircle(px, ALTURA_BARRA / 2, PONTO / 2, cor_nivel);
      if (com_txt) {
        char t[NOTIF_RESUMO];
        snprintf(t, sizeof(t), "%s", a.resumo[0] ? a.resumo : tr(a.nome));
        const int tx = x + PAD + PONTO + 8;
        tab5_cortar_com_reticencias(d, t, sizeof(t), x + larg - PAD - tx);
        d.setTextDatum(middle_left);
        d.setTextColor(TEMA_TEXTO, TEMA_SUPERFICIE);
        d.drawString(t, tx, ALTURA_BARRA / 2 + 1);
      }
      dir = x - GAP;
    }
    tab5_fonte_corpo(d);
  }

  // 56 de largura, e nao 32: o icone cresceu para caber o X ao lado. Um
  // retangulo curto deixaria meio X na tela quando a conexao voltasse.
  tab5_tela_limpar(wifi_cx - 26, 4, 60, ALTURA_BARRA - 8, TEMA_FUNDO);
  if (bt_v) {
    tab5_tela_limpar(bt_cx - 14, 4, 28, ALTURA_BARRA - 8, TEMA_FUNDO);
    desenhar_bt(d, bt_cx, ALTURA_BARRA / 2, bt_v);
  }
  s_barra_bt = bt_v;

  // A conta do centro, porque "-1" solto viraria numero magico em duas semanas:
  // o desenho poe o ponto da base em cy+9 e o arco maior sobe 20px a partir
  // dali, entao o icone ocupa de (cy-11) a (cy+12) - centro em cy+0.5. Para
  // esse centro cair no meio da barra, cy = ALTURA_BARRA/2 - 1.
  // (Antes estava (ALTURA_BARRA-18)/2, que deixava o icone 8px acima do centro.)
  const int rv = radio_visual();
  if (rv >= 100) { desenhar_espnow(d, wifi_cx, ALTURA_BARRA / 2 - 1, rv == 100); }
  else           { desenhar_wifi(d, wifi_cx, ALTURA_BARRA / 2 - 1, cor_wifi, barras, cortado); }

  // Bateria a direita: so o icone e a porcentagem. Tensao, corrente e potencia
  // vivem no app Energia - a barra nao e lugar de numero de diagnostico.
  desenhar_bateria(d, bx, (ALTURA_BARRA - 30) / 2, m.pct, est);

  // TAMANHO PROPRIO, explicito - antes herdava o que sobrava do P4 ou do
  // aviso (dependendo se havia aviso na passada), um acoplamento fragil que
  // ninguem tinha escolhido.
  fonte_barra(d, false, 24);
  d.setTextDatum(middle_right);
  d.setTextColor(TEMA_TEXTO, TEMA_FUNDO);
  if (m.pct < 0) { snprintf(buf, sizeof(buf), "--%%"); }
  else           { snprintf(buf, sizeof(buf), "%3d%%", m.pct); }
  d.drawString(buf, pct_dir, ALTURA_BARRA / 2);

  { float c = 0.0f; s_barra_temp = tab5_sensores_temp(&c) ? (int)(c + 0.5f) : -999; }
  hora_agora(s_barra_hora, sizeof(s_barra_hora));
  s_barra_bloq   = s_bloqueado;
  s_barra_pct    = m.pct;
  s_barra_wifi   = rv;
  s_barra_notif  = notif_versao();
  s_barra_estado = est;
  s_barra_nome   = s_app ? s_app->nome : nullptr;
  s_barra_idioma = idioma_versao();
}

// Verdadeiro se algo visivel na barra mudou desde o ultimo desenho dela.
bool barra_mudou(void)
{
  tab5_medida_t m = tab5_power_ultima();     // idem: cache, nao I2C
  const char* nome = s_app ? s_app->nome : nullptr;
  // GRAU INTEIRO, e nao o float: a decimal muda quase toda leitura e a barra
  // seria repintada 1x por segundo para sempre - justamente o gasto que a
  // funcao existe para evitar.
  // A temperatura so conta na tela de bloqueio (e o unico lugar em que ela
  // esta nesta barra); fora dela, o que vira sozinho e a hora.
  float c = 0.0f;
  const int temp = tab5_sensores_temp(&c) ? (int)(c + 0.5f) : -999;
  char hora[8];
  hora_agora(hora, sizeof(hora));

  return (m.pct != s_barra_pct)
      || (s_bloqueado != s_barra_bloq)
      || (s_bloqueado ? (temp != s_barra_temp) : (strcmp(hora, s_barra_hora) != 0))
      || (estado_energia(m) != s_barra_estado)
      || (nome != s_barra_nome)
      // Sem esta linha o icone de WiFi so mudava quando a BATERIA mudava:
      // conectar ou cair nao repintava nada, e a barra mentia por minutos.
      || (radio_visual() != s_barra_wifi)
      || (bt_visual() != s_barra_bt)
      // Aviso novo tem que aparecer na hora, nao no proximo 1% de bateria. A
      // VERSAO muda so quando algo mudou de verdade - um servico que republica
      // o mesmo estado nao faz a barra repintar.
      || (notif_versao() != s_barra_notif)
      || (midia_versao() != s_barra_midia)
      // Trocar o idioma muda o nome do app e o "Início" sem mudar mais nada.
      || (idioma_versao() != s_barra_idioma)
      || recado_pendente();
}

// Ultima linha de servicos desenhada. Existe pelo mesmo motivo do
// barra_mudou(): a faixa e redesenhada so em repintura COMPLETA, e o conteudo
// dela muda sozinho - o Grove troca de modulo, o cartao entra, o radio cala.
// Sem comparar, a linha envelheceria na tela ate alguem abrir e fechar um app.
static char s_nav_txt[128] = "";
static char s_nav_temp[16] = "";  // a temperatura que a faixa mostra (com app aberto)

// A HORA DA BARRA DE CIMA (e da tela de bloqueio). "--:--" enquanto o relogio nao foi
// acertado (sem RTC valido nem rede) - melhor que uma hora de 1970.
void hora_agora(char* out, size_t n)
{
  const uint32_t t = tab5_hora_epoch();
  if (!t) { snprintf(out, n, "--:--"); return; }
  struct tm lt;
  tab5_hora_local(t, &lt);
  tab5_hora_hhmm(&lt, out, n);
}

// A TEMPERATURA DO P4 NA FAIXA DE BAIXO (a hora subiu para a barra de cima).
//
// O mesmo desenho de coluna que GROVE e CARTAO usam - rubrica pequena em cima,
// valor embaixo - com o termometro a esquerda, na altura das duas linhas:
//
//   |  P4 · méd 41°
//   o  47°C
//
// A rubrica diz DE QUEM e a medida e traz a MEDIA desde o boot
// (tab5_sensores_temp_media): um numero sozinho nao diz se 47 e muito ou e o
// normal deste aparelho; ao lado da media, diz. O termometro sobe e muda de
// cor com a leitura (cor_da_temp), entao da para ler o estado sem ler o numero.

// Chave do que esta desenhado: so repinta quando o grau INTEIRO (ou a media)
// muda - a decimal mudaria quase toda leitura.
static void temp_chave(char* out, size_t n)
{
  float c = 0.0f, m = 0.0f;
  const int ci = tab5_sensores_temp(&c) ? (int)(c + 0.5f) : -999;
  const int mi = tab5_sensores_temp_media(&m) ? (int)(m + 0.5f) : -999;
  snprintf(out, n, "%d/%d", ci, mi);
}

static const int TEMP_TERMO_L = 26;   // termometro (14) e o respiro ate o texto

// Monta os dois textos e devolve a largura do bloco inteiro, para quem quiser
// centra-lo. Deixa a fonte no corpo.
static int temp_textos(lgfx::LovyanGFX& d, char* rot, size_t nr, char* val, size_t nv,
                       bool* tem, float* c)
{
  float m = 0.0f;
  *tem = tab5_sensores_temp(c);
  if (tab5_sensores_temp_media(&m)) { snprintf(rot, nr, tr("P4 · méd %.0f°"), (double)m); }
  else                              { snprintf(rot, nr, "P4"); }
  if (*tem) { snprintf(val, nv, "%.0f°C", (double)*c); }
  else      { snprintf(val, nv, "--"); }
  tab5_fonte_corpo(d);
  tab5_texto(d, 1.95f);
  const int lr = (int)d.textWidth(rot);
  tab5_texto(d, 3.45f);
  const int lv = (int)d.textWidth(val);
  return TEMP_TERMO_L + (lr > lv ? lr : lv);
}

// Desenha o bloco com a borda esquerda em x0. Nao limpa nada: quem chama ja
// limpou a faixa (ou a parte dela que e sua).
static void desenhar_temp_nav(lgfx::LovyanGFX& d, int x0)
{
  const int y = d.height() - ALTURA_NAV;
  char rot[32], val[16];
  bool tem; float c = 0.0f;
  temp_textos(d, rot, sizeof(rot), val, sizeof(val), &tem, &c);

  // O termometro ocupa de cy-20 a cy+12: cy = meio da faixa + 4 o centra.
  desenhar_termometro(d, x0 + 7, y + ALTURA_NAV / 2 + 4, tem, c, TEMA_SUPERFICIE);

  const int tx = x0 + TEMP_TERMO_L;
  d.setTextDatum(top_left);
  tab5_texto(d, 1.95f);   // o mesmo par de tamanhos da rubrica/valor de GROVE e CARTAO
  d.setTextColor(TEMA_TEXTO_2, TEMA_SUPERFICIE);
  const int rot_y = y + 8;
  d.drawString(rot, tx, rot_y);
  const int alt_rot = d.fontHeight();
  tab5_texto(d, 3.45f);
  d.setTextColor(TEMA_TEXTO, TEMA_SUPERFICIE);
  d.drawString(val, tx, rot_y + alt_rot + 4);
  tab5_fonte_corpo(d);
}

// Com um app aberto a temperatura fica a ESQUERDA da pilula - so ela e
// repintada quando o grau vira, nunca a pilula.
static void desenhar_temp_nav_app(lgfx::LovyanGFX& d)
{
  const int y = d.height() - ALTURA_NAV;
  int l = d.width() / 2 - NAV_BTN_L / 2 - 24;   // ate perto da pilula, sem encostar
  if (l > 240) { l = 240; }
  tab5_tela_limpar(16, y + 1, l, ALTURA_NAV - 1, TEMA_SUPERFICIE);
  desenhar_temp_nav(d, 24);
}

// Monta os dois campos. TUDO DE CACHE: nenhuma sondagem, nenhuma montagem de
// cartao, nada que va ao I2C. Esta funcao roda no tique periodico, e o caminho
// de desenho nao e lugar de ir ao hardware.
static void montar_servicos(char* esq, size_t te, char* dir, size_t td)
{
  if (!ajustes_grove()) {
    snprintf(esq, te, "%s", tr("desligado"));
  } else if (tab5_mesh_servico_ativo()) {
    // Com o servico de pe, o que interessa nao e "tem um C6L ali" e sim se ele
    // esta respondendo e quantos nos alcanca.
    snprintf(esq, te, tr("Meshtastic - %d nós, %s"),
             tab5_mesh_num_nos(), tr(tab5_mesh_no_ar() ? "no ar" : "mudo"));
  } else if (tab5_gps_servico_ativo()) {
    // Mesma ideia do Mesh acima: o que importa e se ha FIX de verdade, nao so
    // "tem um GPS ali". qualidade==0 e "recebo o modulo mas ainda sem fix" -
    // acontece nos primeiros segundos, ou a ceu fechado.
    const nmea_posicao_t p = tab5_gps_posicao();
    if (p.valida && p.qualidade > 0) {
      snprintf(esq, te, tr("GPS - fix (%u sats)"), (unsigned)p.sats_usados);
    } else {
      snprintf(esq, te, "%s", tr("GPS - sem fix"));
    }
  } else {
    snprintf(esq, te, "%s", tr(grove_nome(tab5_grove_tipo())));
  }

  snprintf(dir, td, "%s", tr(tab5_sd_descricao()));
}

// DUAS COLUNAS COM RUBRICA, e nao uma linha corrida.
//
// A linha corrida saia da area em portrait - 90 caracteres nao cabem em 720px -
// e mesmo cabendo era um paragrafo onde deveria haver um painel. Com rubrica
// pequena em cima e valor embaixo, o olho acha o campo sem ler o texto todo.
static void desenhar_servicos(lgfx::LovyanGFX& d, const char* esq, const char* dir)
{
  const int L = d.width();
  const int y = d.height() - ALTURA_NAV;

  // A MAIOR DAS QUE SOBRARAM. 1280x95 sao ~243KB numa
  // rajada, e ela e repintada a cada mudanca da linha de servicos.
  tab5_tela_limpar(0, y + 1, L, ALTURA_NAV - 1, TEMA_SUPERFICIE);

  // Teto e divisor: e o que transforma dois textos soltos num painel. O divisor
  // vai so na altura do conteudo, sem encostar nas bordas - encostando, ele
  // viraria uma parede e cortaria a faixa em duas telas.
  // TRES COLUNAS: GROVE | P4 | CARTAO. A do meio e estreita e
  // so tem a temperatura; as dos lados dividem o resto.
  const int meio_l = 210;
  const int lado_l = (L - meio_l) / 2;
  d.drawFastHLine(0, y, L, TEMA_LINHA);
  d.drawFastVLine(lado_l, y + 16, ALTURA_NAV - 32, TEMA_LINHA);
  d.drawFastVLine(lado_l + meio_l, y + 16, ALTURA_NAV - 32, TEMA_LINHA);

  {
    char rot[32], val[16];
    bool tem; float c = 0.0f;
    const int bl = temp_textos(d, rot, sizeof(rot), val, sizeof(val), &tem, &c);
    desenhar_temp_nav(d, L / 2 - bl / 2);
  }

  struct { const char* rot; const char* val; int cx; } col[2] = {
    { tr("GROVE"),  esq, lado_l / 2 },
    { tr("CARTÃO"), dir, L - lado_l / 2 },
  };

  for (int i = 0; i < 2; ++i) {
    d.setTextDatum(top_center);

    // POSICAO DO ROTULO E DO VALOR MEDIDA DE VERDADE (d.fontHeight()), nao
    // mais "y+22"/"y+44" fixos (cortavam a parte de baixo do texto): os offsets antigos foram calculados a
    // olho para o tamanho de ANTES do aumento, e nao acompanharam quando o
    // rotulo/valor cresceram (+30%/+15%). Medir a altura
    // real de CADA tamanho antes de posicionar o proximo elimina esse
    // descompasso de vez - cresce o texto, cresce o espaco reservado, junto.
    tab5_texto(d, 1.95f);  // +30%; +50%
    d.setTextColor(TEMA_TEXTO_2, TEMA_SUPERFICIE);
    const int rot_y = y + 8;   // faixa de 72 px: rotulo + valor + respiro cabem com 8 em cima
    d.drawString(col[i].rot, col[i].cx, rot_y);
    const int alt_rot = d.fontHeight();

    tab5_texto(d, 3.45f);  // +15%; +50%
    d.setTextColor(TEMA_TEXTO, TEMA_SUPERFICIE);
    // Corta no que cabe na METADE da tela, e nao no tamanho do buffer: o texto
    // e centrado, entao um valor comprido sangraria para os dois lados. Corta
    // caractere por caractere medindo de verdade (d.textWidth) - a conta por
    // "12px por caractere" era da Font0 (largura fixa) e nao vale para a Font4
    // (proporcional).
    char cabe[40];
    snprintf(cabe, sizeof(cabe), "%s", col[i].val);
    tab5_cortar_ao_caber(d, cabe, lado_l - 32);   // por caractere: "nós" tem acento
    d.drawString(cabe, col[i].cx, rot_y + alt_rot + 4);
  }
}

// Chamada pelo loop principal (core.cpp) no mesmo tique da barra de status -
// so redesenha se o TEXTO mudou. Comparar strings de 90 bytes de dois em dois
// segundos e mais barato que pintar a faixa a toa. So faz sentido na HOME
// (s_app==NULL): com um app aberto a faixa mostra o botao, nao servicos.
void atualizar_linha_servicos(void)
{
  if (s_bloqueado) { return; }   // a faixa de baixo e preta enquanto bloqueado
  char temp[16];
  temp_chave(temp, sizeof(temp));
  if (s_app) {
    // Com app aberto so a temperatura muda sozinha nesta faixa.
    if (strcmp(temp, s_nav_temp) != 0) {
      snprintf(s_nav_temp, sizeof(s_nav_temp), "%s", temp);
      tela_virtual().startWrite();
      desenhar_temp_nav_app(tela_virtual());
      tela_virtual().endWrite();
    }
    return;
  }
  char e[64], v[64], junto[128];
  montar_servicos(e, sizeof(e), v, sizeof(v));
  snprintf(junto, sizeof(junto), "%s|%s|%s", e, v, temp);
  if (strcmp(junto, s_nav_txt) != 0) {
    snprintf(s_nav_txt, sizeof(s_nav_txt), "%s", junto);
    tela_virtual().startWrite();
    desenhar_servicos(tela_virtual(), e, v);
    tela_virtual().endWrite();
  }
  // O cartao do relogio da home vai no mesmo tique: ele mesmo compara o que
  // mostra (hora, dia, Wi-Fi, Mesh) e so repinta quando algo mudou.
  home_cartao_atualizar();
}

// O QUE O BOTAO DA FAIXA FAZ, e o desenho segue. Hoje ele so tem um trabalho
// - fechar o app e voltar a tela inicial - mas o simbolo sai daqui, e nao de
// um texto cravado no desenho: no dia em que o botao fizer outra coisa (um app
// com niveis voltando um nivel, por exemplo), muda esta funcao e o tratamento
// do toque, e o desenho acompanha sozinho.
typedef enum {
  NAV_INICIO,   // circulo: a tela inicial (o "home" do Android)
  NAV_VOLTAR,   // triangulo para a esquerda: um passo para tras
} nav_acao_t;

static nav_acao_t nav_acao(void)
{
  return NAV_INICIO;
}

// Os simbolos da navegacao, em traco de 2 px. A casinha e a da maquete (viewBox
// 24x24 em 26 px); o "voltar" continua o triangulo.
static const int NAV_ICONE_L = 26;   // lado do desenho da casinha

static void desenhar_icone_nav(lgfx::LovyanGFX& d, int cx, int cy, nav_acao_t acao,
                               uint16_t tinta, uint16_t fundo)
{
  if (acao == NAV_VOLTAR) {
    d.fillTriangle(cx - 13, cy, cx + 10, cy - 14, cx + 10, cy + 14, tinta);
    d.fillTriangle(cx - 7, cy, cx + 7, cy - 9, cx + 7, cy + 9, fundo);
    return;
  }
  // Casinha: telhado, paredes, chao e porta (pontos da maquete, escala 26/24).
  const float k = NAV_ICONE_L / 24.0f;
  const float ox = cx - NAV_ICONE_L / 2.0f, oy = cy - NAV_ICONE_L / 2.0f;
  auto traco = [&](float x0, float y0, float x1, float y1) {
    tab5_linha_grossa(d, ox + x0 * k, oy + y0 * k, ox + x1 * k, oy + y1 * k, 1.0f, tinta);
  };
  traco(3, 11, 12, 4);  traco(12, 4, 21, 11);     // telhado
  traco(4, 11, 4, 21);  traco(20, 11, 20, 21);    // paredes
  traco(4, 21, 9, 21);  traco(15, 21, 20, 21);    // chao, aberto na porta
  traco(9, 21, 9, 15);  traco(15, 21, 15, 15);    // laterais da porta
  traco(9, 15, 15, 15);                       // verga da porta
}

void desenhar_barra_nav(lgfx::LovyanGFX& d)
{
  const int L = d.width();
  const int y = d.height() - ALTURA_NAV;

  tab5_tela_limpar(0, y, L, ALTURA_NAV, TEMA_SUPERFICIE);
  // MARCA. O desenhar_servicos logo abaixo pede a mesma limpeza - e pede sem
  // condicao, como deve. Sem esta marca a faixa era limpa DUAS vezes na home:
  // 276KB onde 138 bastavam, na transicao mais pesada que existe.
  tab5_tela_marcar_limpo(0, y, L, ALTURA_NAV, TEMA_SUPERFICIE);
  d.drawFastHLine(0, y, L, TEMA_LINHA);

  // NA HOME, A FAIXA CONTA O QUE ESTA LIGADO NO APARELHO.
  //
  // Sem app aberto nao ha "INICIO" para desenhar e a faixa ficava vazia. A
  // barra de CIMA ja responde "como esta o aparelho" - bateria, temperatura,
  // wifi. O que ela nao responde e "o que esta ligado nele", e essa e a
  // pergunta que custou tempo nas ultimas sessoes: o Grove trocou de modulo e
  // ninguem avisou, o cartao dizia "sem cartao" com cartao dentro.
  //
  // Tudo daqui sai de CACHE - nenhuma sondagem, nenhuma montagem de cartao, nada
  // que va ao I2C. A faixa e desenhada junto do resto da home, e o caminho de
  // desenho nao e lugar de ir ao hardware.
  if (!s_app) {
    char e[64], v[64], temp[16];
    montar_servicos(e, sizeof(e), v, sizeof(v));
    temp_chave(temp, sizeof(temp));
    snprintf(s_nav_txt, sizeof(s_nav_txt), "%s|%s|%s", e, v, temp);
    desenhar_servicos(d, e, v);
    return;
  }

  if (s_app) {
    // Travado: desenhado apagado (tudo em TEMA_TEXTO_3, sem a pilula alta), para
    // o olho saber que nao adianta tocar.
    const uint16_t fundo = s_nav_travada ? (uint16_t)TEMA_SUPERFICIE : (uint16_t)TEMA_SUPERFICIE_ALTA;
    const uint16_t tinta = s_nav_travada ? (uint16_t)TEMA_TEXTO_3    : (uint16_t)TEMA_TEXTO;
    // Pilula totalmente redonda (raio = altura/2). A altura e a do retangulo de
    // toque (NAV_BTN_A): o desenho nao pode passar da area que responde.
    const int bx = L / 2 - NAV_BTN_L / 2, by = y + NAV_BTN_Y0;
    d.fillRoundRect(bx, by, NAV_BTN_L, NAV_BTN_A, NAV_BTN_A / 2, fundo);
    // Casinha + "Início" juntos, o conjunto centrado na pilula.
    const int GAP_ICONE = 10;
    d.setFont(tab5_fonte_suave(true, 26));
    d.setTextSize(1);
    const char* rotulo = tr("Início");
    const int conj = NAV_ICONE_L + GAP_ICONE + (int)d.textWidth(rotulo);
    const int cy = by + NAV_BTN_A / 2;
    const int x0 = L / 2 - conj / 2;
    desenhar_icone_nav(d, x0 + NAV_ICONE_L / 2, cy, nav_acao(), tinta, fundo);
    d.setTextDatum(middle_left);
    d.setTextColor(tinta, fundo);
    d.drawString(rotulo, x0 + NAV_ICONE_L + GAP_ICONE, cy);
    tab5_fonte_corpo(d);
    temp_chave(s_nav_temp, sizeof(s_nav_temp));
    desenhar_temp_nav_app(d);
  }
}
