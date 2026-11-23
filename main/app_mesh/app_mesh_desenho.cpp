// APP Mesh - o DESENHO: glifos das colunas, a marca do Meshtastic, o trilho, a
// coluna de conversas, a grade e a tabela de nos, a busca, o detalhe do no e a
// malha. Partido de app_mesh.cpp (o ciclo do app); o que as partes compartilham
// esta em app_mesh_interno.hpp.

#include "esp_attr.h"   // EXT_RAM_BSS_ATTR - buffers grandes na PSRAM
#include "idioma.h"
#include "tab5_tela.hpp"
#include "tab5_ui.hpp"
#include "ajustes.h"
#include "tab5_gps.hpp"
#include <stdio.h>
#include <string.h>
#include <math.h>
#include "app_mesh_interno.hpp"
#include "tab5_tema.hpp"

// ---------------------------------------------------------------------------
// Desenho
// ---------------------------------------------------------------------------
// "caixa" e o nome que este app ja usava para tab5_botao() (ver tab5_ui.hpp) -
// mantido aqui como apelido fino para nao renomear cada chamada. Ate
// isto era outra copia local do mesmo botao que existia em tres
// apps a mais.
static void caixa(lgfx::LovyanGFX& d, int x, int y, int l, int a,
                  const char* txt, uint16_t fundo, uint16_t tinta, float tam)
{
  tab5_botao(d, x, y, l, a, txt, fundo, tinta, tam);
}

// ---------------------------------------------------------------------------
// GLIFOS DAS COLUNAS
// ---------------------------------------------------------------------------
// As letras "C", "N" e "M" eram a fonte embutida em tamanho 4 - cada pixel dela
// virava um quadrado de 4, e era isso que serrilhava. Mesma solucao dos icones
// da tela inicial: primitivas em fracao do tamanho, que saem limpas em qualquer
// escala e ainda dizem o que a letra so abreviava.
//
// A COR VEM DE FORA porque cada glifo tem dois estados: selecionado desenha em
// preto sobre o azul da caixa, nao selecionado em branco sobre o fundo escuro.
// O desenho e o mesmo nos dois - so a tinta muda.
// ENGRENAGEM: o sinal de "aqui se configura" - no trilho e ao lado de cada
// opcao que da para alterar. Oito dentes em linhas grossas, o corpo e o furo
// com a cor do fundo (passada por quem chama).
static void engrenagem(lgfx::LovyanGFX& d, int cx, int cy, int r, uint16_t cor, uint16_t fundo)
{
  int dente = r * 2 / 5;
  if (dente < 2) { dente = 2; }
  for (int k = 0; k < 8; ++k) {
    const float ang = (float)M_PI * k / 4.0f;
    tab5_linha_grossa(d, cx + (int)(r * 0.55f * cosf(ang)), cy + (int)(r * 0.55f * sinf(ang)),
                   cx + (int)(r * cosf(ang)),         cy + (int)(r * sinf(ang)), dente, cor);
  }
  d.fillCircle(cx, cy, r * 3 / 4, cor);
  d.fillCircle(cx, cy, r / 3, fundo);
}

static void glifo(lgfx::LovyanGFX& d, glifo_t g, int cx, int cy, int lado, uint16_t cor)
{
  const int r = lado / 2;
  int gr = lado / 12;                       // espessura base
  if (gr < 2) { gr = 2; }

  switch (g) {
    case GL_CONVERSAS: {
      // Balao de fala: corpo arredondado e a ponta embaixo a esquerda. E o
      // desenho universal de "aqui se conversa".
      const int l = (int)(r * 1.5f), a = (int)(r * 1.1f);
      d.drawRoundRect(cx - l/2, cy - a/2 - r/6, l, a, a/3, cor);
      d.drawRoundRect(cx - l/2 + 1, cy - a/2 - r/6 + 1, l - 2, a - 2, a/3, cor);
      d.fillTriangle(cx - l/6, cy + a/2 - r/6 - 1,
                     cx - l/6 + a/3, cy + a/2 - r/6 - 1,
                     cx - l/3, cy + a/2 + r/3, cor);
      break;
    }
    case GL_NOS: {
      // Lista: tres pontos, cada um com a sua linha. E um CADASTRO - foi essa a
      // diferenca que faltava para nao se confundir com a malha.
      for (int i = -1; i <= 1; ++i) {
        const int y = cy + i * (r / 2);
        d.fillCircle(cx - r * 3 / 5, y, gr, cor);
        d.fillRect(cx - r / 5, y - gr / 2, r, gr, cor);
      }
      break;
    }
    case GL_MALHA: {
      // Grafo: um no no centro e quatro em volta, ligados. Aqui a informacao e
      // a LIGACAO, e nao a lista - por isso as arestas sao o que se ve primeiro.
      const int d4[4][2] = { {-1,-1}, {1,-1}, {-1,1}, {1,1} };
      for (int i = 0; i < 4; ++i) {
        const int x = cx + d4[i][0] * r * 3 / 5;
        const int y = cy + d4[i][1] * r * 3 / 5;
        d.drawLine(cx, cy, x, y, cor);
        d.fillCircle(x, y, gr + 1, cor);
      }
      d.fillCircle(cx, cy, gr + 2, cor);
      break;
    }
    case GL_CONFIG: {
      // A cor de fundo do furo: o azul do destaque na caixa selecionada (tinta clara),
      // o fundo do trilho na nao selecionada - a tinta diz qual e.
      engrenagem(d, cx, cy, r * 3 / 4, cor, cor == TEMA_SOBRE_DESTAQUE ? TEMA_DESTAQUE : TEMA_FUNDO);
      break;
    }
    case GL_DIFUSAO: {
      // Difusao: um ponto e ondas saindo dele para os dois lados. Fala para
      // todo mundo, e nao para alguem - e a diferenca que o icone precisa
      // carregar contra o GL_DIRETO.
      d.fillCircle(cx, cy, gr + 1, cor);
      for (int k = 1; k <= 2; ++k) {
        const int rr = k * r / 2;
        d.drawArc(cx, cy, rr, rr - gr + 1,  30,  150, cor);
        d.drawArc(cx, cy, rr, rr - gr + 1, 210,  330, cor);
      }
      break;
    }
    case GL_DIRETO: {
      // Uma pessoa: cabeca e ombros. Conversa direta e com ALGUEM.
      d.fillCircle(cx, cy - r / 3, r / 3, cor);
      d.fillArc(cx, cy + r * 3 / 4, r * 2 / 3, r * 2 / 3 - gr * 2, 180, 360, cor);
      break;
    }
  }
}

// Caixa com glifo no lugar do texto. Mesma geometria e mesmas cores da caixa()
// de texto, para os dois conviverem sem desalinhar.
void caixa_glifo(lgfx::LovyanGFX& d, int x, int y, int l, int a,
                        glifo_t g, uint16_t fundo, uint16_t tinta)
{
  d.fillRoundRect(x, y, l, a, 10, fundo);
  const int lado = (l < a ? l : a) * 3 / 5;
  glifo(d, g, x + l / 2, y + a / 2, lado, tinta);
}

// ---------------------------------------------------------------------------
// A MARCA DO MESHTASTIC
// ---------------------------------------------------------------------------
// Desenhada com primitivas e nao guardada como imagem: assim ela sai em
// qualquer tamanho sem serrilhar, nao ocupa flash, e o mesmo codigo serve para
// o icone de 190px da tela inicial e para o de 48px do trilho.
//
// A geometria saiu do SVG oficial (meshtastic/design, logo/logo.svg): fundo
// verde rgb(103,234,148) e duas cristas em rgb(44,45,60) - a crista cheia a
// direita e uma subida solta a esquerda, que e a perna de um segundo pico atras
// dela. O desenho e inspirado nos chirps do LoRa e le como montanha, tenda e a
// letra M ao mesmo tempo; a ambiguidade e proposital no original.
//
// Coordenadas em FRACAO do lado, nao em pixels. E o que permite o mesmo traco
// nos dois tamanhos sem uma segunda tabela para manter em dia.
void marca_mesh(lgfx::LovyanGFX& d, int x, int y, int lado)
{
  const uint16_t verde  = d.color565(103, 234, 148);
  const uint16_t escuro = d.color565(44, 45, 60);

  d.fillRoundRect(x, y, lado, lado, lado / 6, verde);

  // ESPESSURA MEDIDA NO SVG, nao estimada.
  //
  // A primeira versao usava 9% do lado e saiu gorda. No original o traco tem
  // ~14,7 unidades e a crista ~140 de largura: 10,5% DA CRISTA. Como a crista
  // ocupa cerca de metade do icone, sobra ~5% do lado - quase metade da
  // primeira estimativa.
  //
  // O piso de 3px continua: abaixo disso a marca some em vez de encolher.
  int grosso = (lado * 5) / 100;
  if (grosso < 3) { grosso = 3; }

  #define MX(f) (x + (int)((f) * lado))
  #define MY(f) (y + (int)((f) * lado))

  // Crista principal. Proporcao do original: ~0,49 de largura por ~0,36 de
  // altura - mais achatada do que a primeira versao.
  tab5_linha_grossa(d, MX(0.365f), MY(0.72f), MX(0.610f), MY(0.34f), grosso, escuro);
  tab5_linha_grossa(d, MX(0.610f), MY(0.34f), MX(0.855f), MY(0.72f), grosso, escuro);

  // A subida solta a esquerda: MESMA BASE, MESMO TOPO e MESMA inclinacao da
  // perna esquerda da crista. Ela e a perna de um segundo pico atras, e um pico
  // atras tem a altura do da frente - parando no meio do caminho, como estava,
  // ela virava um risco solto em vez de montanha.
  //
  // Sendo paralelas, as duas nunca se cruzam por construcao: a separacao e
  // constante em toda a altura, e nao depende da escolha exata dos
  // pontos.
  tab5_linha_grossa(d, MX(0.120f), MY(0.72f), MX(0.365f), MY(0.34f), grosso, escuro);

  #undef MX
  #undef MY
}

void desenhar_trilho(lgfx::LovyanGFX& d, const area_t& a)
{
  // Era SIS_FUNDO (a paleta do sistema): as colunas sao
  // MOLDURA, nao conteudo, e tinham que recuar como a barra recua. Trocado
  // pela paleta do proprio Mesh (fundo_msg) - para o trilho, a lista de conversas e o
  // painel de mensagens lerem como UMA SO tela, nao duas paletas coladas.
  tab5_tela_limpar(a.x, a.y, TRILHO_L, a.altura, fundo_msg(d));
  // RETANGULO INTEIRO, e nao so a linha da direita. Com uma borda so, a coluna
  // parecia sangrar para fora da area em cima e embaixo; fechada, ela vira uma
  // peca.
  //
  // SIS_BORDA e nao branco: o branco puro tinha o maior contraste possivel
  // contra o fundo escuro e a moldura gritava mais que o conteudo dela. Este
  // cinza se ve sem disputar.
  d.drawRect(a.x, a.y, TRILHO_L, a.altura, SIS_BORDA);
  const int nao_lidas = tab5_mesh_nao_lidas();
  const int diretas   = tab5_mesh_nao_lidas_diretas();
  const uint16_t FUNDO = fundo_msg(d);

  for (int i = 0; i < PAG_TOTAL; ++i) {
    const int y = a.y + 16 + i * (ITEM_A + 12);
    const bool sel = ((int)s_pag == i);
    // Fundo do icone NAO selecionado: era preto puro (achado em
    // "as imagens... com o fundo antigo") e destoava do trilho
    // ja recolorido ao redor dele. Agora acompanha a mesma paleta.
    caixa_glifo(d, a.x + 12, y, TRILHO_L - 24, ITEM_A, (glifo_t)i,
                sel ? TEMA_DESTAQUE : FUNDO, sel ? TEMA_SOBRE_DESTAQUE : TEMA_TEXTO);

    // PONTO, NAO NUMERO, e a diferenca e de proposito.
    //
    // O numero respondia "quantas" - uma pergunta que a coluna de conversas ja
    // responde melhor, conversa por conversa. Aqui, num icone de 66px, ele so
    // competia por espaco com a letra e ainda precisava apagar o proprio fundo.
    //
    // O que o trilho precisa dizer e "tem coisa nova ali", e para isso um ponto
    // basta - o olho pega de relance, sem ler. Vermelho quando ha mensagem
    // DIRETA: endereçada a voce nao e a mesma coisa que trafego do canal.
    if (i == PAG_MSGS && nao_lidas > 0) {
      const int px = a.x + TRILHO_L - 24;
      const int py = y + 12;
      d.fillCircle(px, py, 7, diretas > 0 ? TEMA_PERIGO : TEMA_DESTAQUE);
      d.drawCircle(px, py, 7, TEMA_FUNDO);      // contorno na cor do fundo: separa o ponto do icone
    }
  }
  // Estado do enlace no pe do trilho: verde vivo, vermelho mudo. E a resposta
  // para "esta conectado?" sem sair da pagina em que voce esta.
  const bool ok = tab5_mesh_no_ar();

  // MARCA EMBAIXO, PONTO EM CIMA. Juntos respondem "que rede e esta" e "ela
  // esta viva" sem ocupar linha de texto nenhuma - mas a marca e a identidade
  // do app e fecha melhor o pe do trilho; o ponto e estado, e estado sobe.
  const int ml    = TRILHO_L - 36;
  const int marca_y = a.y + a.altura - 18 - ml;   // mesma margem de pe de antes
  const int led_cy  = marca_y - 22 - 12;          // mesmo respiro de 22px

  d.fillCircle(a.x + TRILHO_L / 2, led_cy, 12, ok ? TEMA_SUCESSO : TEMA_PERIGO);
  marca_mesh(d, a.x + (TRILHO_L - ml) / 2, marca_y, ml);
}

// A COLUNA DE CONVERSAS E UMA kernel/lista. Item de altura fixa;
// a lista rola, recorta, desenha a faixa exposta, a barra e responde ao toque.
// A coluna so e repintada por inteiro em passada completa (a unica hora em que
// nomes, selecao e nao-lidas mudam); fora disso, so a rolagem.
static const int CONV_PASSO = ITEM_A + 8;

int conv_altura(int, void*) { return CONV_PASSO; }

// O que cada item mostra agora (nome e nao-lidas), para a coluna perceber
// sozinha o que mudou - antes, o contador de uma conversa que nao era a aberta
// so aparecia na proxima passada completa.
typedef struct { char nome[MESH_NOME_CONVERSA]; char sub[MESH_NOME_LONGO]; int nao_lidas; } conv_visto_t;
static conv_visto_t s_conv_visto[MESH_MAX_CONVERSAS];

void conv_desenhar(lgfx::LovyanGFX& d, int i, int x, int y, int largura, void*)
{
  mesh_conversa_t c;
  if (!tab5_mesh_conversa(i, &c)) { return; }
  if (i < MESH_MAX_CONVERSAS) {
    snprintf(s_conv_visto[i].nome, sizeof(s_conv_visto[i].nome), "%s", c.nome);
    snprintf(s_conv_visto[i].sub, sizeof(s_conv_visto[i].sub), "%s", c.sub);
    s_conv_visto[i].nao_lidas = c.nao_lidas;
  }
  const uint16_t FUNDO = fundo_msg(d);
  // O item comeca pelo respiro de 8px: o contador de nao-lidas sobe 6px acima
  // do icone e precisa estar DENTRO do proprio item - a lista so repinta o
  // item que cruza a faixa exposta, e um pedaco fora dele ficaria sem dono.
  y += 8;
  const bool sel = (i == mesh_s_conv);

  // O TIPO DA CONVERSA, e nao a inicial do nome. A inicial em tamanho 4 era a
  // fonte embutida ampliada, serrilhada, e dizia menos do que parecia: o nome
  // inteiro esta escrito ao lado, entao a inicial repetia informacao. O que a
  // coluna nao dizia era se aquilo e um CANAL (todo mundo ouve) ou alguem
  // especifico - e e isso que muda como voce escreve.
  // Cor do icone de TIPO (canal/direto) mantida saturada de proposito - e um
  // indicador funcional (distinguir os dois tipos de conversa de relance),
  // nao "a cor do balao"; deixei fora do recolorimento por
  // falta de referencia clara no que foi mandado (a captura de tela nao
  // mostra esta tela). Os avatares coloridos por remetente (cor_do_no), que
  // SAO equivalentes ao que a captura mostra, ja seguem a paleta nova.
  caixa_glifo(d, x + 10, y, ITEM_A, ITEM_A, c.canal ? GL_DIFUSAO : GL_DIRETO,
              sel ? TEMA_DESTAQUE : (c.canal ? TEMA_DESTAQUE_SUAVE : TEMA_SUPERFICIE_ALTA),
              sel ? TEMA_SOBRE_DESTAQUE : TEMA_TEXTO);

  // NOME COM LARGURA QUE CABE, medido de verdade: o texto comeca em
  // (10 + ITEM_A + 8) = 90px dentro da coluna, o divisor esta em CONV_L-1 =
  // 199. Antes disto era "12px por caractere" (a largura fixa da Font0);
  // a Font4 e proporcional, entao o corte agora mede com d.textWidth() e
  // tira uma letra de cada vez ate caber - a mesma tecnica usada no resto
  // do sistema desde a troca de fonte. O acolchoamento com
  // espacos a direita saiu: a lista limpa a faixa do item antes de
  // desenha-lo, entao nao sobra resto de nome antigo para apagar.
  const int nome_x = x + 10 + ITEM_A + 8;
  const int nome_max_l = largura - (nome_x - x);
  // +60%. RISCO
  // ACEITO: nome_max_l nao mudou (a coluna continua com 200px), entao um
  // nome que cabia inteiro antes pode passar a truncar uma letra a mais -
  // o corte por d.textWidth() logo abaixo ja protege contra estourar a
  // coluna, so mostra menos caracteres se precisar.
  tab5_texto(d, 3.84f);   // +60%
  d.setTextDatum(top_left);
  d.setTextColor(sel ? TEMA_DESTAQUE : TEMA_TEXTO, FUNDO);
  char nome[24];
  snprintf(nome, sizeof(nome), "%s", c.nome);
  tab5_cortar_ao_caber(d, nome, nome_max_l);   // por caractere: o nome pode ter acento
  d.drawString(nome, nome_x, y + 12);

  // SEGUNDA LINHA: o nome longo de quem e a direta (ou "canal N"). E o que
  // responde "quem e" quando o curto e uma sigla - sem recorrer ao id. Na
  // conversa aberta ela para antes do botao de apagar, que mora nesta altura.
  if (c.sub[0]) {
    tab5_texto(d, 2.4f);
    d.setTextColor(TEMA_TEXTO_3, FUNDO);
    const int sub_max_l = sel ? (apagar_dx() - (nome_x - x) - 6) : nome_max_l;
    char sub[MESH_NOME_LONGO];
    snprintf(sub, sizeof(sub), "%s", c.sub);
    tab5_cortar_ao_caber(d, sub, sub_max_l);   // por caractere: o longo pode ter acento
    d.drawString(sub, nome_x, y + 46);
  }

  // Contador sobre o CANTO do icone, como em aplicativo de mensagem. Antes
  // ele ficava a direita do nome e disputava espaco com ele - e o nome sempre
  // ganhava, porque e desenhado depois.
  if (c.nao_lidas > 0) {
    char nl[8]; snprintf(nl, sizeof(nl), "%d", c.nao_lidas);
    caixa(d, x + 10 + ITEM_A - 24, y - 6, 30, 26, nl, TEMA_PERIGO, TEMA_TEXTO, 2);
  }

  // APAGAR: so na conversa ABERTA - canal TAMBEM.
  //
  // So na aberta para nao encher a coluna de botoes vermelhos - voce apaga a
  // que esta olhando, que e a que acabou de perceber que nao queria. Ate
  // canal nao ganhava este botao (um canal apagado voltava sozinho) - deixou de ser verdade quando canal passou a entrar na
  // mesma flag de apagados que ja existia para no (ver o aviso grande em
  // tab5_mesh.cpp, junto dela). Reabrir depois: pela secao "CANAIS
  // DISPONIVEIS" na pagina de Nos, ou so esperando mensagem nova chegar.
  if (sel) {
    caixa(d, x + apagar_dx(), y + APAGAR_DY, APAGAR_L, APAGAR_A,
          "x", TEMA_PERIGO, TEMA_SOBRE_DESTAQUE, 2);
  }
}

// A janela da coluna: por DENTRO da moldura (1px) e acima do teclado, que
// cobre a largura inteira quando aberto. O que o teclado cobre nao rola nem
// responde - a lista so conhece o que desenhou.
// A largura da coluna sai daqui e fica gravada em s_col_l: o item (o "x" de
// apagar) e o toque leem o mesmo numero que a moldura usou.
static area_t janela_conversas(const area_t& a)
{
  s_col_l = em_pe(a) ? a.largura - TRILHO_L : CONV_L;
  const int alt = a.altura - (conversa_teclado_aberto(&s_c) ? tecvirt_altura(a) : 0);
  return area_t{ a.x + TRILHO_L + 1, a.y + 1, s_col_l - 2, alt - 2 };
}

void desenhar_conversas(lgfx::LovyanGFX& d, const area_t& a)
{
  const uint16_t FUNDO = fundo_msg(d);
  const area_t j = janela_conversas(a);
  const int n = tab5_mesh_num_conversas();

  if (TUDO || n != s_l_conv.n) {
    lista_itens_mudaram(&s_l_conv, n);
  } else {
    for (int i = 0; i < n && i < MESH_MAX_CONVERSAS; ++i) {
      mesh_conversa_t c;
      if (!tab5_mesh_conversa(i, &c)) { continue; }
      if (c.nao_lidas != s_conv_visto[i].nao_lidas || strcmp(c.nome, s_conv_visto[i].nome) != 0
       || strcmp(c.sub, s_conv_visto[i].sub) != 0) {
        lista_item_mudou(&s_l_conv, i);
      }
    }
  }
  const bool pintou = lista_desenhar(&s_l_conv, d, j, FUNDO, TUDO);

  // Lista vazia repintada (passada completa, ou a ultima conversa apagada):
  // o aviso vai por cima do fundo que a lista acabou de pintar.
  if (n == 0 && pintou) {
    tab5_texto(d, 2.4f);
    d.setTextDatum(top_left);
    d.setTextColor(TEMA_TEXTO_2, FUNDO);
    d.drawString(tr("sem conversas"), j.x + 11, j.y + 19);
  }
  // Moldura FORA da janela da lista: nada que a lista pinte a alcanca, e
  // basta desenha-la na passada completa. No cinza da moldura do sistema.
  if (TUDO) { d.drawRect(a.x + TRILHO_L, a.y, s_col_l, j.altura + 2, SIS_BORDA); }
}

// "HA QUANTO TEMPO", curto.
//
// E a pergunta que o app do celular responde e a nossa lista nao respondia:
// SNR alto num no que ninguem ouve ha dois dias nao quer dizer nada. Formato
// curto de proposito - a coluna e estreita e o que importa e a ordem de
// grandeza, nao o segundo exato.
static void idade_texto(int32_t s, char* saida, size_t tam)
{
  if      (s < 0)     { snprintf(saida, tam, "?");     }
  else if (s < 90)    { snprintf(saida, tam, tr("agora")); }
  else if (s < 3600)  { snprintf(saida, tam, "%dm", (int)(s / 60)); }
  else if (s < 86400) { snprintf(saida, tam, "%dh%02d",
                                 (int)(s / 3600), (int)((s % 3600) / 60)); }
  else                { snprintf(saida, tam, "%dd", (int)(s / 86400)); }
}

// Verde ate 15 min, amarelo ate 2h, cinza depois. As 2h sao a fronteira que o
// proprio Meshtastic usa para chamar um no de "online" - vale copiar, para a
// lista aqui e a do celular contarem a mesma historia.
static uint16_t idade_cor(int32_t s)
{
  if (s < 0)    { return TEMA_TEXTO_3; }
  if (s <= 900) { return TEMA_SUCESSO; }
  if (s <= 7200){ return TEMA_ALERTA; }
  return TEMA_TEXTO_3;
}

// ---------------------------------------------------------------------------
// A GRADE DAS COLUNAS
// ---------------------------------------------------------------------------
// Cabecalho e linhas sao montados a partir DESTA tabela, e so dela. E a unica
// forma de os dois nao se descolarem - e eles tinham se descolado.
//
// Antes a linha era um formato unico com os vaos contados a mao:
//
//   "%-22.22s  %-6.6s %5.1f %3u%%   %u   "
//
// Repare nos espacos: 2, 1, 1, 3, 3. Cada coluna com um respiro diferente, e o
// cabecalho era OUTRA string, com a sua propria contagem - entao "snr" caia um
// caractere fora dos valores e "visto" encostava em "saltos". Mexer numa das
// duas sem mexer na outra desalinhava tudo de novo.
//
// Regra unica agora: vao de 2 caracteres entre colunas, e o titulo alinha do
// mesmo jeito que o valor - numero a direita, texto a esquerda.
enum { C_NOME = 0, C_CURTO, C_ID, C_HW, C_SNR, C_BAT, C_SALTOS, C_VISTO, C_TOTAL };

// IDENTIDADE E ETIQUETA, LADO A LADO.
//
// "nome" e "curto" sao ETIQUETAS: quem renomeia o no no celular troca as duas.
// Nao dizem com que aparelho voce esta falando, e era so isso que a tabela
// mostrava. As duas colunas novas dizem:
//
//   id  - "!a1b2c3d4", derivado do MAC, acompanha o radio para sempre. E o
//         identificador que o Meshtastic usa em todo lugar: log, mapa, celular.
//   hw  - qual placa e (T-Beam, Heltec V3...). Explica sozinho por que um no
//         tem bateria e outro nao, por que um anda e outro nao.
//
// A tela e de 1280px em paisagem. A ultima coluna termina no caractere 85
// (com os vaos de 2 caracteres corrigidos), que da 1126px com a margem e o
// x0 de 90px do trilho - cabe com ~150px de folga para o indicador de
// rolagem e ainda sobra chao para uma coluna futura.
// OS VAOS ERAM DE 1 CARACTERE, NAO DE 2 - o comentario acima descrevia a
// regra certa, mas os numeros abaixo nao a seguiam (nome termina em 20,
// "curto" comecava em 21 - so 1 de vao, nao 2). Numa tabela de largura fixa,
// campo direita-alinhado colado a 12px do proximo campo esquerda-alinhado e
// exatamente o que le como "torto": o valor de uma coluna quase encosta no
// texto da vizinha, sem o respiro visual que separa "isto e uma coluna" de
// "isto e a proxima". Refeita com os 2 caracteres que a regra sempre pediu.
static const coluna_t COLUNA[C_TOTAL] = {
  {  0, 20, false, N_("nome")   },
  { 22,  5, false, N_("curto")  },
  { 29, 10, false, "id"     },
  { 41, 15, false, N_("modelo") },
  { 58,  5, true,  N_("snr")    },
  { 65,  4, true,  N_("bat")    },
  { 71,  6, true,  N_("saltos") },
  { 79,  6, false, N_("visto")  },
};

// A MESMA GRADE EM PE. Em 720 px, tirando o trilho, cabem ~48 caracteres -
// nao os 85 de cima. Ficam as colunas que respondem "da para falar com ele
// agora?": nome, sinal, bateria, saltos e ha quanto tempo foi ouvido. Saem
// curto, id e modelo (largura 0: campo() nao escreve nada), que continuam a
// um toque, no cartao do no. Mesma regra de vao de 2 caracteres.
static const coluna_t COLUNA_PE[C_TOTAL] = {
  {  0, 16, false, N_("nome")   },
  {  0,  0, false, N_("curto")  },
  {  0,  0, false, "id"     },
  {  0,  0, false, N_("modelo") },
  { 18,  5, true,  N_("snr")    },
  { 25,  4, true,  N_("bat")    },
  { 31,  6, true,  N_("saltos") },
  { 39,  6, false, N_("visto")  },
};

// A grade em uso: escolhida por desenhar_nos() a cada passada, pela area.
static const coluna_t* s_grade = COLUNA;

// Fonte 6x8 no tamanho 2 = 12px por caractere.
static const int NOS_CH = 12;
// A linha principal vai ate o fim de "saltos"; "visto" e desenhado a parte,
// porque tem cor propria. NOS_LINHA_CH e o MAIOR caso (deitado) - o tamanho
// dos buffers; o comprimento de agora vem da grade.
#define NOS_LINHA_CH  77        // COLUNA[C_SALTOS].col + larg
static int linha_ch(void) { return s_grade[C_SALTOS].col + s_grade[C_SALTOS].larg; }
// Margem esquerda da tabela: a coluna da estrela (favorito) mora nela, e com
// 16 px a estrela encostava na borda da area.
static const int NOS_MARGEM  = 34;
static const int NOS_ESTRELA_X = 16;              // centro da estrela, dentro da margem
static int visto_x(void) { return NOS_MARGEM + s_grade[C_VISTO].col * NOS_CH; }

// OS CANAIS NO TOPO DA PAGINA DE NOS - todos os ativos do no, como botoes.
//
// Antes era uma lista de "canais disponiveis": so os SEM conversa na coluna,
// e so os que a configuracao do radio tinha marcado como ativos - e o canal 0
// nunca chegava a ser marcado (ver o leitor de canais em tab5_mesh.cpp). Um
// LongFast apagado da coluna nao tinha onde ser achado. Agora todo canal ativo
// esta sempre aqui; o que nao esta na coluna aparece apagado, com "+".
//
// A geometria e GRAVADA no desenho e LIDA no toque (s_chips): os dois nunca
// discordam, nem com nomes de larguras diferentes.
chip_t s_chips[MESH_MAX_CANAIS + 1];
int    s_n_chips = 0;
static int    s_chips_altura = 0;          // altura da faixa dos canais, a partir do topo
static char   s_chips_assin[(MESH_MAX_CANAIS + 1) * (MESH_NOME_CONVERSA + 4) + 48];

static const int CHIP_A = 36;

// A CAIXA DE PESQUISA, no topo da pagina (onde ficava a escolha de rede). A
// tabela mostra so os nos que batem com o texto, a cada letra - o filtro e do
// servico (tab5_mesh_no_busca). Toque na caixa abre o teclado virtual; o
// fisico digita direto nela, sem precisar tocar. O "x" no fim limpa.
//
// So ASCII: a tabela e Font0, e a busca do servico ja ignora acento ("jose"
// acha "José"), entao nao ha o que digitar fora dele.
char      s_busca[MESH_BUSCA_MAX] = "";
static bool      s_busca_mudou = false;    // a tabela volta ao topo e e remontada
tecvirt_t s_kb_busca;
chip_t    s_chip_busca;             // a caixa inteira (coordenadas locais)
static char      s_busca_assin[MESH_BUSCA_MAX + 24];

// NO ESCOLHIDO NA TABELA: a pagina mostra o cartao dele em vez da tabela.
// Pelo NUMERO, nao pelo indice - a tabela se reordena por "visto".
uint32_t s_no_sel = 0;

// Escreve um campo na sua coluna dentro de 'linha', que ja chega cheia de
// espacos. memcpy e nao snprintf: um NUL no meio cortaria o resto da linha.
// O strnlen limitado a largura tambem e o que impede um nome longo de invadir
// a coluna seguinte.
void campo(char* linha, const coluna_t& c, const char* txt)
{
  const int n = (int)strnlen(txt, (size_t)c.larg);
  memcpy(linha + c.col + (c.direita ? c.larg - n : 0), txt, (size_t)n);
}

static void espacos(char* linha, size_t tam)
{
  memset(linha, ' ', tam - 1);
  linha[tam - 1] = 0;
}

// Troca cada sequencia UTF-8 de varios bytes (acento, emoji, qualquer coisa
// fora de ASCII puro) por UM UNICO '?'. Existe porque campo()/montar_linha()
// constroem a linha como uma GRADE DE LARGURA FIXA POR BYTE (1 byte = 1
// coluna de tela) - um nome com emoji (4 bytes UTF-8 desenhando UM icone so)
// ou so um acento (2 bytes por 1 letra, "Jose" -> "José") descola a conta:
// os bytes "extras" empurram os campos seguintes DAQUELA LINHA para a
// direita, sem empurrar as outras - e era exatamente o desalinhamento
// observado na tabela de nos.
//
// Nome de no vem de OUTRO radio, sem controle nosso, entao a defesa fica
// aqui, na entrada da tabela - nao adianta pedir para quem manda a mensagem
// n usar emoji. Nas CONVERSAS, onde o texto corre livre (sem grade de
// coluna), o emoji continua aparecendo de verdade - ver tab5_emoji_preparar().
static void sanitizar_ascii(const char* in, char* out, size_t out_tam)
{
  if (out_tam == 0) { return; }
  size_t o = 0;
  const unsigned char* p = (const unsigned char*)in;
  while (*p && o + 1 < out_tam) {
    if (*p < 0x80) { out[o++] = (char)*p; ++p; continue; }
    // Lead byte de sequencia multi-byte: pula ela INTEIRA de uma vez, senao
    // cada byte de continuacao viraria um '?' proprio - um emoji so ja vira
    // 4 pontos de interrogacao em vez de 1.
    const int n = ((*p & 0xE0) == 0xC0) ? 2
                : ((*p & 0xF0) == 0xE0) ? 3
                : ((*p & 0xF8) == 0xF0) ? 4
                : 1;    // byte de continuacao solto (UTF-8 invalido): 1 so
    out[o++] = '?';
    p += n;
  }
  out[o] = 0;
}

static void montar_linha(char* linha, size_t tam, const mesh_no_t& no)
{
  char v[24];
  char nome_limpo[MESH_NOME_LONGO];
  char curto_limpo[MESH_NOME_CURTO];
  espacos(linha, tam);

  // LONGO: mesma logica do curto logo abaixo - prefere a entrada AO VIVO
  // (NodeInfo desta sessao); sem ela, cai para o cache persistente
  // (tab5_mesh_nome_longo_cache(), nos.txt). Esta coluna so
  // olhava no.longo - a mesma classe de bug que o curto teve
  // (ver o aviso dele logo abaixo), so que nunca corrigida aqui: a maioria
  // dos nos nasce de telemetria/posicao/texto, nao de NodeInfo (a UNICA fonte
  // do nome longo), entao a tabela mostrava "(sem nome)" quase sempre.
  const char* longo_fonte = no.longo;
  char longo_cache[MESH_NOME_LONGO];
  if (!no.longo[0] && tab5_mesh_nome_longo_cache(no.num, longo_cache, sizeof(longo_cache))) {
    longo_fonte = longo_cache;
  }
  sanitizar_ascii(longo_fonte[0] ? longo_fonte : tr("(sem nome)"), nome_limpo, sizeof(nome_limpo));

  // CURTO: prefere o que ESTE no informou ao vivo (NodeInfo desta sessao).
  // Sem isso, cai para o cache persistente (nos.txt) - o mesmo cache que a
  // conversa ja consulta via tab5_mesh_nome_exibicao(), mas aqui SEM o
  // fallback de hex dela (ver tab5_mesh_nome_cache() e o aviso completo no
  // .hpp): a coluna ID logo abaixo ja mostra o hex quando falta nome, entao
  // repeti-lo aqui na coluna de nome curto seria redundante.
  //
  // ACHADO (a maioria das linhas saia sem nome longo nem curto): esta tabela so olhava a entrada AO VIVO (no.curto). A maior
  // parte das entradas nasce de achar_no() sendo chamado por um pacote de
  // TELEMETRIA/POSICAO/TEXTO (ver ler_packet()/ler_data() no servico), nao
  // por um NodeInfo - e NodeInfo (o UNICO tipo que traz nome) e bem mais
  // raro na malha que os outros. Um no com quem ja se conversou antes (nome
  // gravado no cache ha muito tempo) aparecia em branco de novo, so porque a
  // entrada ao vivo de AGORA ainda nao recebeu um NodeInfo novo - o mesmo
  // problema que a conversa ja tinha resolvido, so que a tabela nunca ganhou
  // a mesma correcao.
  const char* curto_fonte = no.curto;
  char curto_cache[MESH_NOME_CURTO];
  if (!no.curto[0] && tab5_mesh_nome_cache(no.num, curto_cache, sizeof(curto_cache))) {
    curto_fonte = curto_cache;
  }
  sanitizar_ascii(curto_fonte, curto_limpo, sizeof(curto_limpo));

  campo(linha, s_grade[C_NOME],  nome_limpo);
  campo(linha, s_grade[C_CURTO], curto_limpo);

  // O id vem do proprio radio como "!a1b2c3d4". Quando ainda nao veio (o
  // node_info dele nao chegou), montamos a partir do numero - e a MESMA conta
  // que o Meshtastic faz, entao o valor confere.
  if (no.id[0]) { campo(linha, s_grade[C_ID], no.id); }
  else {
    snprintf(v, sizeof(v), "!%08lx", (unsigned long)no.num);
    campo(linha, s_grade[C_ID], v);
  }

  // Modelo conhecido pelo nome; desconhecido pelo numero, que da para procurar.
  // Inventar um nome para um valor que a tabela nao tem seria pior que o numero.
  const char* hw = mesh_hw_nome(no.hw);
  if (hw) { campo(linha, s_grade[C_HW], hw); }
  else    { snprintf(v, sizeof(v), tr("modelo %u"), (unsigned)no.hw);
            campo(linha, s_grade[C_HW], v); }

  // O PONTO DECIMAL TEM QUE CAIR SEMPRE NA MESMA COLUNA - foi essa a raiz do
  // "torto" observado.
  //
  // campo() ja alinha a DIREITA corretamente (a string sempre termina rente a
  // borda direita do campo); o problema era o "%.1f" nao ter largura fixa.
  // "-18.5" tem 5 caracteres, "9.0" tem 3 - as duas terminam no mesmo lugar
  // (alinhadas a direita, do jeito certo), mas como comecam em colunas
  // DIFERENTES, o ponto decimal de uma linha nao fica embaixo do ponto
  // decimal da linha seguinte. Numa coluna de numeros, e o ponto decimal -
  // nao a borda - que o olho usa para saber que esta tudo alinhado.
  //
  // "%+05.1f" fixa sinal, dois digitos inteiros (com zero a esquerda quando
  // faltar) e uma casa decimal - sempre 5 caracteres, sempre no mesmo formato:
  // "-18.5", "+09.0", "-05.5", "+00.0". O ponto decimal cai sempre na mesma
  // posicao, para qualquer leitura entre -99.9 e +99.9 (a faixa real de SNR
  // do Meshtastic nunca chega perto disso).
  snprintf(v, sizeof(v), "%+05.1f", (double)no.snr);
  campo(linha, s_grade[C_SNR], (no.snr <= -128.0f) ? "-" : v);

  // 101 e o codigo do Meshtastic para "na tomada", nao uma bateria de 101%.
  // Mostrava "101%" antes. E 0 significa que a telemetria dele ainda nao veio.
  //
  // ZERO A ESQUERDA, pela mesma razao do SNR acima: "5%" e "100%" tem
  // larguras diferentes, e sem largura fixa o digito das dezenas de uma linha
  // nao cai embaixo do digito das dezenas da linha seguinte. "%03u%%" fixa
  // 4 caracteres sempre - "005%", "050%", "100%" - e o '%' e os digitos ficam
  // na mesma coluna em toda linha que tiver bateria de verdade.
  if      (no.bateria == 0)   { snprintf(v, sizeof(v), "-");  }
  else if (no.bateria > 100)  { snprintf(v, sizeof(v), "AC"); }
  else                        { snprintf(v, sizeof(v), "%03u%%", (unsigned)no.bateria); }
  campo(linha, s_grade[C_BAT], v);

  // "mqtt" no lugar do numero: no que chega pela internet nao tem salto de
  // radio nenhum, e mostrar "0" ali diria que ele e vizinho direto - o oposto
  // da verdade. E a informacao que decide se voce alcanca esse no com a
  // internet fora.
  //
  // "-" PARA DESCONHECIDO (nos apareciam sem nome e sem saltos): um no so criado por achar_no() ao ver um pacote
  // (sem NodeInfo ainda) tinha saltos=0 pelo memset, indistinguivel de um
  // vizinho direto de verdade. Agora o sentinela (MESH_SALTOS_DESCONHECIDO)
  // e explicito aqui, do jeito que o SNR (-128) ja era.
  if (no.via_mqtt) { snprintf(v, sizeof(v), "mqtt"); }
  else if (no.saltos == MESH_SALTOS_DESCONHECIDO) { snprintf(v, sizeof(v), "-"); }
  else             { snprintf(v, sizeof(v), "%u", (unsigned)no.saltos); }
  campo(linha, s_grade[C_SALTOS], v);
  // Em pe a linha e mais curta: termina aqui, senao os espacos do resto
  // passariam da borda da tela.
  if ((int)tam > linha_ch()) { linha[linha_ch()] = 0; }
}

// O que ja esta escrito em cada linha. Existe para NAO reescrever 32 linhas de
// texto por segundo: com o periodo de 1Hz e a idade correndo, quase toda
// passada mudava um punhado de linhas e redesenhava todas. Em paisagem isso
// custa caro - o M5GFX transpoe as coordenadas e cada linha de texto vira
// escrita em coluna na memoria do painel.
EXT_RAM_BSS_ATTR static char s_nos_ant[MESH_MAX_NOS][NOS_LINHA_CH + 1];
EXT_RAM_BSS_ATTR static char s_nos_visto_ant[MESH_MAX_NOS][8];
EXT_RAM_BSS_ATTR static bool s_nos_fav_ant[MESH_MAX_NOS];   // a estrela que esta desenhada

// 44 e nao 32: com rolagem, altura nao falta, e linhas coladas cansavam o
// olho. O texto (16 px) vai centrado e um fio escuro separa uma da outra.
static const int NOS_LINHA_A = 44;
static const int NOS_TEXTO_DY = (NOS_LINHA_A - 16) / 2;

int nos_altura(int, void*) { return NOS_LINHA_A; }

// Uma linha da tabela. Guarda o que escreveu (s_nos_ant/s_nos_visto_ant): e a
// referencia com que desenhar_nos() decide, na passada seguinte, se a linha
// mudou. Font0 ja esta ativa (ver a excecao em desenhar_nos).
// ESTRELA de cinco pontas, em primitivas: nenhuma fonte do sistema tem o
// caractere, e desenhada ela sai igual em qualquer tamanho. Cheia = favorito;
// vazia (so o contorno) = o botao de marcar.
static void estrela(lgfx::LovyanGFX& d, int cx, int cy, int r, uint16_t cor, bool cheia)
{
  float px[10], py[10];
  for (int k = 0; k < 10; ++k) {
    const float ang = (float)M_PI * (-0.5f + k * 0.2f);
    const float rr = (k & 1) ? r * 0.42f : (float)r;
    px[k] = cx + rr * cosf(ang);
    py[k] = cy + rr * sinf(ang);
  }
  for (int k = 0; k < 10; ++k) {
    const int j = (k + 1) % 10;
    if (cheia) { d.fillTriangle(cx, cy, (int)px[k], (int)py[k], (int)px[j], (int)py[j], cor); }
    else       { d.drawLine((int)px[k], (int)py[k], (int)px[j], (int)py[j], cor); }
  }
}

void nos_desenhar(lgfx::LovyanGFX& d, int i, int x, int y, int, void*)
{
  mesh_no_t no;
  if (!tab5_mesh_no_busca(s_busca, i, &no) || i >= MESH_MAX_NOS) { return; }

  // BRANCO = o nosso no. AMARELO = ouvido pelo ar (tem SNR): fixos no topo,
  // os que da para alcancar com a internet fora. Pela internet, o azul do
  // MQTT das conversas; sem SNR, cinza.
  const bool meu = (no.num == tab5_mesh_meu_numero());
  const uint16_t cor = meu ? TEMA_TEXTO
                     : (no.fixado || tab5_mesh_no_pelo_ar(&no)) ? TEMA_ALERTA
                     : no.via_mqtt ? TEMA_DESTAQUE : TEMA_TEXTO_3;
  d.setTextSize(2);
  d.setTextDatum(top_left);
  montar_linha(s_nos_ant[i], sizeof(s_nos_ant[i]), no);
  d.setTextColor(cor, TEMA_FUNDO);
  d.drawString(s_nos_ant[i], x + NOS_MARGEM, y + NOS_TEXTO_DY);
  // Favorito: estrela na margem, antes do nome (os 16 px que ja sobravam).
  if (no.favorito) { estrela(d, x + NOS_ESTRELA_X, y + NOS_LINHA_A / 2, 8, TEMA_ALERTA, true); }
  // Divisao fraca, de pouco contraste: separa sem disputar com o texto.
  d.drawFastHLine(x + 6, y + NOS_LINHA_A - 1, visto_x() + 6 * NOS_CH - 6, TEMA_LINHA);

  // Coluna separada porque tem cor propria: a idade responde "da para falar
  // com ele AGORA?", que e outra pergunta da qualidade do sinal.
  char curto[8];
  idade_texto(no.idade_s, curto, sizeof(curto));
  snprintf(s_nos_visto_ant[i], sizeof(s_nos_visto_ant[i]), "%-6.6s", curto);
  s_nos_fav_ant[i] = no.favorito;
  d.setTextColor(idade_cor(no.idade_s), TEMA_FUNDO);
  d.drawString(s_nos_visto_ant[i], x + visto_x(), y + NOS_TEXTO_DY);
}

// Assinatura dos canais oferecidos: muda quando um canal liga, desliga, troca
// de nome ou entra/sai da coluna - e ai a faixa e repintada inteira.
static void canais_assinatura(char* out, size_t tam)
{
  out[0] = 0;
  const int n = tab5_mesh_num_canais();
  for (int i = 0; i < n; ++i) {
    uint8_t idx; char nome[MESH_NOME_CONVERSA]; bool vis = false;
    if (!tab5_mesh_canal(i, &idx, nome, sizeof(nome), &vis)) { continue; }
    const size_t k = strlen(out);
    snprintf(out + k, tam - k, "%u%c%s|", (unsigned)idx, vis ? '+' : '-', nome);
  }
}

// Desenha a faixa dos canais e grava a geometria. Devolve a altura usada.
//
// NA FONTE DA TABELA (Font0 em tamanho 2, 12 px por caractere), e nao na
// suave do tab5_botao: a faixa e a tabela sao a mesma pagina, e duas fontes
// coladas liam como duas telas. Cada botao leva o numero do canal ("ch0") -
// e o que o Meshtastic usa para dizer qual e o primario, e o que distingue
// dois canais de nome parecido.
// Um botao da faixa, na fonte da tabela. Devolve a largura.
static int chip(lgfx::LovyanGFX& d, int x, int y, const char* rot, uint16_t fundo, uint16_t frente)
{
  const int l = (int)strlen(rot) * NOS_CH + 28;
  d.fillRoundRect(x, y, l, CHIP_A, 10, fundo);
  d.setTextColor(frente, fundo);
  d.drawString(rot, x + 14, y + CHIP_A / 2);
  return l;
}

static int desenhar_canais(lgfx::LovyanGFX& d, const area_t& a, int x0)
{
  d.setFont(&fonts::Font0);
  d.setTextSize(2);
  const int fim_x = a.x + a.largura - 16;
  const int ini_x = x0 + NOS_MARGEM + 8 * NOS_CH;   // depois do rotulo "canais"
  int x = ini_x, y = a.y + 8;
  d.setTextDatum(middle_left);

  // BUSCA: o rotulo e a caixa, na largura que sobra. O conteudo da caixa e de
  // desenhar_busca, que repinta so ela a cada letra.
  d.setTextColor(TEMA_TEXTO_2, TEMA_FUNDO);
  d.drawString(tr("buscar"), x0 + NOS_MARGEM, y + CHIP_A / 2);
  s_chip_busca = chip_t{ x - a.x, y - a.y, fim_x - x, CHIP_A, 0 };
  s_busca_assin[0] = 0;   // a caixa foi (re)posta: o conteudo vem a seguir
  x = ini_x;
  y += CHIP_A + 8;
  d.setTextColor(TEMA_TEXTO_2, TEMA_FUNDO);
  d.drawString(tr("canais"), x0 + NOS_MARGEM, y + CHIP_A / 2);
  s_n_chips = 0;
  const int n = tab5_mesh_num_canais();
  for (int i = 0; i < n && s_n_chips < MESH_MAX_CANAIS + 1; ++i) {
    uint8_t idx; char nome[MESH_NOME_CONVERSA]; bool vis = false;
    if (!tab5_mesh_canal(i, &idx, nome, sizeof(nome), &vis)) { continue; }
    char limpo[MESH_NOME_CONVERSA];
    sanitizar_ascii(nome, limpo, sizeof(limpo));   // Font0: so ASCII (ver sanitizar_ascii)
    char rot[MESH_NOME_CONVERSA + 12];
    snprintf(rot, sizeof(rot), "%sch%u %s", vis ? "" : "+ ", (unsigned)idx, limpo);
    const int l = (int)strlen(rot) * NOS_CH + 28;
    if (x + l > fim_x && x > ini_x) { x = ini_x; y += CHIP_A + 8; }
    const uint16_t fundo = vis ? TEMA_DESTAQUE_SUAVE : TEMA_SUPERFICIE_ALTA;
    d.fillRoundRect(x, y, l, CHIP_A, 10, fundo);
    d.setTextColor(vis ? TEMA_TEXTO : TEMA_TEXTO_2, fundo);
    d.drawString(rot, x + 14, y + CHIP_A / 2);
    s_chips[s_n_chips++] = chip_t{ x - a.x, y - a.y, l, CHIP_A, idx };
    x += l + 10;
  }
  d.setTextDatum(top_left);
  return (y - a.y) + CHIP_A + 8;
}

// O CONTEUDO DA CAIXA DE PESQUISA: o texto (com o cursor, se o teclado esta
// aberto) ou o convite, quantos nos bateram e o "x". Pinta so quando algo
// disso muda - a cada letra, nao a cada segundo.
static void desenhar_busca(lgfx::LovyanGFX& d, const area_t& a)
{
  const chip_t& k = s_chip_busca;
  if (!k.l) { return; }
  const bool kb = tecvirt_aberto(&s_kb_busca);
  const int n = tab5_mesh_num_nos_busca(s_busca);
  const int total = tab5_mesh_num_nos();
  char assin[sizeof(s_busca_assin)];
  snprintf(assin, sizeof(assin), "%d|%d|%d|%s", kb, n, total, s_busca);
  if (strcmp(assin, s_busca_assin) == 0) { return; }
  snprintf(s_busca_assin, sizeof(s_busca_assin), "%s", assin);

  const int x = a.x + k.x, y = a.y + k.y;
  const uint16_t fundo = TEMA_SUPERFICIE_ALTA;
  d.fillRoundRect(x, y, k.l, k.a, 10, fundo);
  d.drawRoundRect(x, y, k.l, k.a, 10, kb ? TEMA_DESTAQUE : TEMA_LINHA);
  d.setFont(&fonts::Font0);
  d.setTextSize(2);
  d.setTextDatum(middle_left);
  const int cy = y + k.a / 2;

  // A contagem na direita, antes do "x": "12 de 87" com busca, "87 nós" sem.
  char cont[24];
  if (s_busca[0]) { snprintf(cont, sizeof(cont), tr("%d de %d"), n, total); }
  else            { snprintf(cont, sizeof(cont), tr("%d nos"), total); }
  const int cont_l = (int)strlen(cont) * NOS_CH;
  const int fim_txt = x + k.l - BUSCA_LIMPAR_L - cont_l - 16;
  d.setTextColor(s_busca[0] && n == 0 ? TEMA_ALERTA : TEMA_TEXTO_3, fundo);
  d.drawString(cont, fim_txt + 8, cy);

  // O texto: o fim dele, se nao couber (e onde se esta digitando).
  const int max_ch = (fim_txt - (x + 14)) / NOS_CH;
  char t[MESH_BUSCA_MAX + 2];
  if (s_busca[0] || kb) {
    snprintf(t, sizeof(t), "%s%s", s_busca, kb ? "_" : "");
    const int nt = (int)strlen(t);
    const char* ini = (max_ch > 0 && nt > max_ch) ? t + (nt - max_ch) : t;
    d.setTextColor(TEMA_TEXTO, fundo);
    d.drawString(ini, x + 14, cy);
  } else {
    snprintf(t, sizeof(t), "%s", tr("nome, curto ou !id"));
    if (max_ch > 0 && (int)strlen(t) > max_ch) { t[max_ch] = 0; }
    d.setTextColor(TEMA_TEXTO_3, fundo);
    d.drawString(t, x + 14, cy);
  }
  if (s_busca[0]) {
    d.setTextColor(TEMA_TEXTO_2, fundo);
    d.setTextDatum(middle_center);
    d.drawString("x", x + k.l - BUSCA_LIMPAR_L / 2, cy);
  }
  d.setTextDatum(top_left);
}

void busca_abrir_teclado(void)
{
  if (tecvirt_aberto(&s_kb_busca)) { return; }
  tecvirt_abrir(&s_kb_busca, TECVIRT_BUSCA);
  pintura_pedir(&s_p);
  sistema_pedir_redesenho_completo();   // a tabela encolhe acima do teclado
}

void busca_fechar_teclado(void)
{
  if (!tecvirt_aberto(&s_kb_busca)) { return; }
  tecvirt_fechar(&s_kb_busca);
  pintura_pedir(&s_p);
  sistema_pedir_redesenho_completo();
}

void busca_limpar(void)
{
  if (!s_busca[0]) { return; }
  s_busca[0] = 0;
  s_busca_mudou = true;
  sistema_pedir_redesenho();
}

// Uma tecla (virtual ou fisica, ja como acao do tecvirt) na busca.
void busca_tecla(int ac)
{
  if (ac == TECVIRT_ENVIAR || ac == TECVIRT_FECHAR) { busca_fechar_teclado(); return; }
  char antes[sizeof(s_busca)];
  snprintf(antes, sizeof(antes), "%s", s_busca);
  tecvirt_editar(s_busca, sizeof(s_busca), ac);
  if (strcmp(antes, s_busca) != 0) { s_busca_mudou = true; sistema_pedir_redesenho(); }
}

static bool no_por_num(uint32_t num, mesh_no_t* out);

static bool no_por_num(uint32_t num, mesh_no_t* out)
{
  const int n = tab5_mesh_num_nos();
  for (int i = 0; i < n; ++i) {
    if (tab5_mesh_no(i, out) && out->num == num) { return true; }
  }
  return false;
}

// CARTAO DO NO - tudo o que se sabe dele, como o cliente web mostra. Cabe o
// que a tabela nao tem largura para mostrar (MAC, tensao, posicao) sem
// apertar mais uma coluna nela.
static const int DET_LIN   = 50;
static const int DET_BTN_L = 300;
// EM PE as medidas fixas passam da borda: dois botoes de 300 lado a lado nao
// cabem nos ~630 px que sobram do trilho. Desenho e toque passam pelas mesmas
// funcoes, entao concordam nas duas orientacoes. 'larg' = da coluna x0 ate o
// fim da area.
int det_btn_l(const area_t& a)
{
  const int larg = a.largura - TRILHO_L;
  const int cabe = (larg - 3 * 24) / 2;
  return cabe < DET_BTN_L ? cabe : DET_BTN_L;
}
int det_fav_l(const area_t& a) { return em_pe(a) ? 210 : DET_FAV_L; }
static int det_val_dx(const area_t& a) { return em_pe(a) ? 170 : 220; }   // coluna dos valores
static char s_det_assin[512];

static void desenhar_no_detalhe(lgfx::LovyanGFX& d, const area_t& a, int x0)
{
  mesh_no_t no;
  const bool achou = no_por_num(s_no_sel, &no);
  const bool meu = (s_no_sel == tab5_mesh_meu_numero());

  enum { R_ID = 0, R_MAC, R_HW, R_SINAL, R_SALTOS, R_BAT, R_VISTO, R_POS, R_TOTAL };
  static const char* const ROT[R_TOTAL] = { N_("id"), N_("MAC"), N_("modelo"), N_("sinal"), N_("saltos"), N_("bateria"), N_("visto"), N_("posição") };
  char val[R_TOTAL][64];
  char titulo[MESH_NOME_LONGO + 16], curto[MESH_NOME_CURTO * 2] = "";
  for (int k = 0; k < R_TOTAL; ++k) { snprintf(val[k], sizeof(val[k]), "-"); }

  if (!achou) {
    tab5_mesh_nome_exibicao(s_no_sel, titulo, sizeof(titulo));
    snprintf(val[R_ID], sizeof(val[R_ID]), tr("!%08lx  (saiu da tabela ao vivo)"), (unsigned long)s_no_sel);
  } else {
    // Titulo: o longo; sem ele, a regra de exibicao (curto, ou o id).
    char longo[MESH_NOME_LONGO] = "";
    if (no.longo[0]) { snprintf(longo, sizeof(longo), "%s", no.longo); }
    else             { tab5_mesh_nome_longo_cache(no.num, longo, sizeof(longo)); }
    if (longo[0]) { snprintf(titulo, sizeof(titulo), "%s", longo); }
    else          { tab5_mesh_nome_exibicao(no.num, titulo, sizeof(titulo)); }
    if (no.curto[0]) { snprintf(curto, sizeof(curto), "%s", no.curto); }
    else             { tab5_mesh_nome_cache(no.num, curto, sizeof(curto)); }

    snprintf(val[R_ID], sizeof(val[R_ID]), "%s   nº %lu",
             no.id[0] ? no.id : "", (unsigned long)no.num);
    if (!no.id[0]) { snprintf(val[R_ID], sizeof(val[R_ID]), "!%08lx   nº %lu", (unsigned long)no.num, (unsigned long)no.num); }

    // O MAC so quando o no o mandou (User.macaddr). O id e derivado dos 4
    // ultimos bytes dele, mas completar os 2 primeiros seria inventar.
    if (no.tem_mac) {
      snprintf(val[R_MAC], sizeof(val[R_MAC]), "%02X:%02X:%02X:%02X:%02X:%02X",
               no.mac[0], no.mac[1], no.mac[2], no.mac[3], no.mac[4], no.mac[5]);
    } else {
      snprintf(val[R_MAC], sizeof(val[R_MAC]), tr("não enviado pelo nó"));
    }

    const char* hw = mesh_hw_nome(no.hw);
    if (no.hw == 0)  { snprintf(val[R_HW], sizeof(val[R_HW]), tr("não informado")); }
    else if (hw)     { snprintf(val[R_HW], sizeof(val[R_HW]), "%s", hw); }
    else             { snprintf(val[R_HW], sizeof(val[R_HW]), tr("modelo %u"), (unsigned)no.hw); }

    if (no.snr > -128.0f) { snprintf(val[R_SINAL], sizeof(val[R_SINAL]), tr("SNR %+.1f dB%s"), (double)no.snr, no.via_mqtt ? tr("  (do gateway)") : ""); }

    if (no.via_mqtt)                                   { snprintf(val[R_SALTOS], sizeof(val[R_SALTOS]), tr("pela internet (MQTT)")); }
    else if (no.saltos == 0)                           { snprintf(val[R_SALTOS], sizeof(val[R_SALTOS]), tr("0 - vizinho direto")); }
    else if (no.saltos != MESH_SALTOS_DESCONHECIDO)    { snprintf(val[R_SALTOS], sizeof(val[R_SALTOS]), "%u", (unsigned)no.saltos); }

    char bat[32] = "";
    if (no.bateria > 100)    { snprintf(bat, sizeof(bat), tr("na tomada")); }
    else if (no.bateria > 0) { snprintf(bat, sizeof(bat), "%u %%", (unsigned)no.bateria); }
    if (no.tensao > 0.1f) {
      const size_t k = strlen(bat);
      snprintf(bat + k, sizeof(bat) - k, "%s%.2f V", k ? "   " : "", (double)no.tensao);
    }
    if (bat[0]) { snprintf(val[R_BAT], sizeof(val[R_BAT]), "%s", bat); }

    char idade[12];
    idade_texto(no.idade_s, idade, sizeof(idade));
    if (no.idade_s < 0)       { snprintf(val[R_VISTO], sizeof(val[R_VISTO]), tr("não ouvido nesta sessão")); }
    else if (no.idade_s < 90) { snprintf(val[R_VISTO], sizeof(val[R_VISTO]), tr("agora")); }
    else                      { snprintf(val[R_VISTO], sizeof(val[R_VISTO]), tr("há %s"), idade); }

    if (no.tem_pos) {
      const float lat = no.lat_i / 1e7f, lon = no.lon_i / 1e7f;
      float rlat, rlon; bool ref = false;
      const nmea_posicao_t fix = tab5_gps_posicao();
      if (fix.valida && fix.qualidade > 0) { rlat = (float)fix.lat; rlon = (float)fix.lon; ref = true; }
      else { ref = ajustes_casa(&rlat, &rlon); }
      if (ref && !meu) {
        float km; const char* dir;
        distancia_direcao(rlat, rlon, lat, lon, &km, &dir);
        snprintf(val[R_POS], sizeof(val[R_POS]), "%.5f, %.5f   %.1f km %s", (double)lat, (double)lon, (double)km, dir);
      } else {
        snprintf(val[R_POS], sizeof(val[R_POS]), "%.5f, %.5f", (double)lat, (double)lon);
      }
    }
  }

  const bool fav = achou ? no.favorito : tab5_mesh_favorito(s_no_sel);

  // So repinta quando algum texto mudou: a pagina roda a 1 Hz.
  char assin[sizeof(s_det_assin)];
  snprintf(assin, sizeof(assin), "%s|%s|%d|", titulo, curto, (int)fav);
  for (int k = 0; k < R_TOTAL; ++k) {
    const size_t n = strlen(assin);
    snprintf(assin + n, sizeof(assin) - n, "%s|", val[k]);
  }
  if (!pintura_mudou_s(&s_p, s_det_assin, sizeof(s_det_assin), assin)) { return; }

  tab5_tela_limpar(x0, a.y, a.x + a.largura - x0, a.altura, TEMA_FUNDO);
  d.setTextDatum(top_left);
  tab5_texto(d, 4.2f);
  d.setTextColor(TEMA_TEXTO, TEMA_FUNDO);
  // O titulo para antes da estrela (e do chip do curto).
  const int fav_l = det_fav_l(a);
  const int fav_x = a.x + a.largura - DET_FAV_DIR - fav_l;
  const int tit_max = fav_x - (x0 + 24) - 160;
  tab5_cortar_ao_caber(d, titulo, tit_max);
  d.drawString(titulo, x0 + 24, a.y + 18);
  if (!meu) {
    const uint16_t fundo = fav ? TEMA_SUPERFICIE_ALTA : TEMA_SUPERFICIE_ALTA;
    d.fillRoundRect(fav_x, a.y + DET_FAV_Y, fav_l, DET_FAV_A, 12, fundo);
    estrela(d, fav_x + 34, a.y + DET_FAV_Y + DET_FAV_A / 2, 16, fav ? TEMA_ALERTA : TEMA_TEXTO_2, fav);
    d.setFont(tab5_fonte_suave(true, 26));
    d.setTextDatum(middle_left);
    d.setTextColor(fav ? TEMA_ALERTA : TEMA_TEXTO_2, fundo);
    d.drawString(fav ? tr("FAVORITO") : tr("FAVORITAR"), fav_x + 62, a.y + DET_FAV_Y + DET_FAV_A / 2);
    d.setTextDatum(top_left);
    tab5_texto(d, 4.2f);
    d.setTextColor(TEMA_TEXTO, TEMA_FUNDO);
  }
  if (curto[0]) {
    const int tx = x0 + 24 + (int)d.textWidth(titulo) + 20;
    tab5_botao(d, tx, a.y + 16, 120, 44, curto, TEMA_DESTAQUE_SUAVE, TEMA_TEXTO);
  }
  if (meu) {
    tab5_texto(d, 2.4f);
    d.setTextColor(TEMA_SUCESSO, TEMA_FUNDO);
    d.drawString(tr("este é o seu nó"), x0 + 24, a.y + 62);
  }

  for (int k = 0; k < R_TOTAL; ++k) {
    const int y = a.y + 96 + k * DET_LIN;
    tab5_texto(d, 2.8f);
    d.setTextColor(TEMA_TEXTO_3, TEMA_FUNDO);
    d.drawString(tr(ROT[k]), x0 + 24, y + 4);
    // Valor longo (a posicao com distancia, sobretudo em pe): encolhe ate
    // caber e, no limite, corta - nunca passa da borda.
    const int vx = x0 + det_val_dx(a);
    texto_que_cabe(d, val[k], a.x + a.largura - 16 - vx, 3.4f, 2.6f);
    d.setTextColor(k == R_MAC && !(achou && no.tem_mac) ? TEMA_TEXTO_3 : TEMA_TEXTO, TEMA_FUNDO);
    d.drawString(val[k], vx, y);
  }

  const int by = a.y + a.altura - DET_BTN_Y_DA_BASE;
  const int bl = det_btn_l(a);
  tab5_botao(d, x0 + 24, by, bl, DET_BTN_A, tr("VOLTAR"), TEMA_SUPERFICIE_ALTA, TEMA_TEXTO);
  if (!meu) {
    tab5_botao(d, x0 + 24 + bl + 24, by, bl, DET_BTN_A, tr("CONVERSAR"), TEMA_DESTAQUE, TEMA_SOBRE_DESTAQUE);
  }
}

void desenhar_nos(lgfx::LovyanGFX& d, const area_t& a, int x0)
{
  char linha[NOS_LINHA_CH + 1];
  char visto[8];
  // A grade pela orientacao de agora. Trocar de grade so acontece num giro,
  // e o giro ja vem com passada completa: tudo e remontado abaixo.
  s_grade = em_pe(a) ? COLUNA_PE : COLUNA;

  if (s_no_sel) {
    lista_esconder(&s_l_nos);   // a tabela nao esta na tela: nao rola nem responde
    if (tecvirt_aberto(&s_kb_busca)) { tecvirt_fechar(&s_kb_busca); }
    desenhar_no_detalhe(d, a, x0);
    tab5_fonte_corpo(d);
    return;
  }

  // Os canais mudaram (config nova, conversa apagada/reaberta): a faixa muda
  // de altura e a tabela muda de lugar - passada completa.
  char assin[sizeof(s_chips_assin)];
  canais_assinatura(assin, sizeof(assin));
  if (strcmp(assin, s_chips_assin) != 0) {
    snprintf(s_chips_assin, sizeof(s_chips_assin), "%s", assin);
    if (!TUDO) { pintura_pedir(&s_p); sistema_pedir_redesenho_completo(); return; }
  }

  if (TUDO) {
    tab5_tela_limpar(x0, a.y, a.x + a.largura - x0, a.altura, TEMA_FUNDO);
    s_chips_altura = desenhar_canais(d, a, x0);
  }

  // EXCECAO A FONT4, DE PROPOSITO: esta tabela monta UMA STRING SO por linha
  // (campo() escreve cada valor num deslocamento de CARACTERE fixo dentro de
  // 'linha', ver COLUNA[].col acima) e desenha a linha inteira de uma vez. Isso
  // SO alinha em colunas se cada caractere tiver a MESMA largura em pixels -
  // exatamente o que a Font0 garante (6px, sempre) e a Font4 NAO (e
  // proporcional). Por isso ela fica na Font0 e so ela neste app.
  d.setFont(&fonts::Font0);

  if (TUDO) {
    // Cabecalho pela MESMA tabela das linhas - e o que garante que ele nunca
    // mais saia do lugar sozinho.
    espacos(linha, sizeof(linha));
    for (int i = 0; i < C_VISTO; ++i) { campo(linha, s_grade[i], tr(s_grade[i].titulo)); }
    linha[linha_ch()] = 0;
    d.setTextSize(2);
    d.setTextDatum(top_left);
    d.setTextColor(TEMA_TEXTO_2, TEMA_FUNDO);
    d.drawString(linha, x0 + NOS_MARGEM, a.y + s_chips_altura + 8);
    d.drawString(tr(s_grade[C_VISTO].titulo), x0 + visto_x(), a.y + s_chips_altura + 8);
  }
  d.setTextSize(2);
  d.setTextDatum(top_left);

  desenhar_busca(d, a);

  // CABECALHO FIXO ("sticky"): ele nao rola, so as linhas. Com o teclado da
  // busca aberto, a janela termina em cima dele.
  const int cabecalho_a = s_chips_altura + 36;
  const int kb_a = tecvirt_aberto(&s_kb_busca) ? tecvirt_altura(a) : 0;
  const area_t janela_nos = { x0, a.y + cabecalho_a, a.x + a.largura - x0, a.altura - cabecalho_a - kb_a };

  // O QUE MUDOU DE VALOR (a idade corre a cada segundo, o SNR muda com o
  // proximo pacote, a ordem muda quando alguem fala): compara com o que esta
  // escrito e marca SO essas linhas. Mudou a CONTAGEM: a lista inteira.
  const int n = tab5_mesh_num_nos_busca(s_busca);
  if (s_busca_mudou) { s_busca_mudou = false; lista_ao_inicio(&s_l_nos); lista_itens_mudaram(&s_l_nos, n); }
  else if (TUDO || n != s_l_nos.n) {
    lista_itens_mudaram(&s_l_nos, n);
  } else {
    for (int i = 0; i < n; ++i) {
      // Linha nunca desenhada (fora da janela): a rolagem a desenha quando
      // ela aparecer. Marca-la por ter o texto vazio arrastaria as visiveis
      // junto (a marca e um intervalo) - ver s_cfg_pintada na pagina "No".
      if (i < MESH_MAX_NOS && !s_nos_ant[i][0]) { continue; }
      mesh_no_t no;
      if (!tab5_mesh_no_busca(s_busca, i, &no)) { continue; }
      montar_linha(linha, sizeof(linha), no);
      char curto[8];
      idade_texto(no.idade_s, curto, sizeof(curto));
      snprintf(visto, sizeof(visto), "%-6.6s", curto);
      if (strcmp(linha, s_nos_ant[i]) != 0 || strcmp(visto, s_nos_visto_ant[i]) != 0
       || no.favorito != s_nos_fav_ant[i]) {
        lista_item_mudou(&s_l_nos, i);
      }
    }
  }
  const bool pintou = lista_desenhar(&s_l_nos, d, janela_nos, TEMA_FUNDO, TUDO);
  if (n == 0 && s_busca[0] && pintou) {
    d.setTextColor(TEMA_TEXTO_3, TEMA_FUNDO);
    d.drawString(tr("nenhum no com esse nome ou id"), janela_nos.x + NOS_MARGEM, janela_nos.y + 16);
  }

  if (tecvirt_aberto(&s_kb_busca)) {
    tecvirt_posicionar(&s_kb_busca, a);
    tecvirt_pintar(&s_kb_busca, d, TUDO);
  }

  tab5_fonte_corpo(d);   // devolve a Font4 - ver a excecao acima
}

void desenhar_malha(lgfx::LovyanGFX& d, const area_t& a, int x0)
{
  if (TUDO) { tab5_tela_limpar(x0, a.y, a.x + a.largura - x0, a.altura, TEMA_FUNDO); }
  mesh_saude_t s; tab5_mesh_saude(&s);
  char buf[160];

  // "-" QUANDO NAO VEIO. Os numeros de saude so existem com o LocalStats /
  // DeviceMetrics do NOSSO no; no modo internet (sem no) ou antes da primeira
  // telemetria eles sao zero - e "0.0 %" de uso do canal e um valor plausivel,
  // que mentia "malha vazia" quando a verdade era "nao sei".
  //
  // NOS ONLINE sem o LocalStats: contados aqui, com a mesma regra do
  // Meshtastic (ouvido nas ultimas 2 h) - marcados como tal.
  struct { const char* rot; char val[48]; uint16_t cor; } campos[10];
  if (s.tem_local) {
    snprintf(campos[0].val, sizeof(campos[0].val), "%u / %u", (unsigned)s.nos_online, (unsigned)s.nos_total);
    campos[0].rot = tr("nós online (do rádio)");
  } else {
    int on = 0;
    const int n = tab5_mesh_num_nos();
    for (int i = 0; i < n; ++i) {
      mesh_no_t no;
      if (tab5_mesh_no(i, &no) && no.idade_s >= 0 && no.idade_s <= 7200) { ++on; }
    }
    snprintf(campos[0].val, sizeof(campos[0].val), "%d / %d", on, n);
    campos[0].rot = tr("nós online (contados aqui, 2 h)");
  }
  campos[0].cor = TEMA_SUCESSO;
  const bool tem_uso = s.tem_local || s.tem_metricas;
  if (tem_uso) { snprintf(campos[1].val, sizeof(campos[1].val), "%.1f %%", (double)s.uso_canal); }
  else         { snprintf(campos[1].val, sizeof(campos[1].val), "-"); }
  campos[1].rot = tr("uso do canal"); campos[1].cor = TEMA_DESTAQUE;
  if (tem_uso) { snprintf(campos[2].val, sizeof(campos[2].val), "%.2f %%", (double)s.airtime_tx); }
  else         { snprintf(campos[2].val, sizeof(campos[2].val), "-"); }
  campos[2].rot = tr("airtime tx"); campos[2].cor = TEMA_DESTAQUE;
  if (tem_uso) {
    snprintf(campos[3].val, sizeof(campos[3].val), "%lu h %lu min",
             (unsigned long)(s.uptime_s / 3600), (unsigned long)((s.uptime_s % 3600) / 60));
  } else {
    snprintf(campos[3].val, sizeof(campos[3].val), "-");
  }
  campos[3].rot = tr("uptime do nó"); campos[3].cor = TEMA_TEXTO;
  // ENLACE COM A VIA: "no ar" sozinho nao dizia por onde.
  const uint8_t via = tab5_mesh_via();
  const char* nome_via = !tab5_mesh_servico_ativo() ? tr("parado")
                       : via == AJUSTES_MESH_BLUETOOTH ? "Bluetooth"
                       : via == AJUSTES_MESH_INTERNET  ? tr("Internet") : tr("Grove");
  snprintf(campos[4].val, sizeof(campos[4].val), "%s · %s", nome_via, tab5_mesh_no_ar() ? tr("no ar") : tr("sem quadro"));
  campos[4].rot = tr("enlace"); campos[4].cor = tab5_mesh_no_ar() ? TEMA_SUCESSO : TEMA_PERIGO;

  // SEGUNDA COLUNA: o que o radio ouviu e o entorno imediato. Os contadores de
  // pacote so existem com o LocalStats ("-" sem ele, como acima).
  if (s.tem_local) {
    snprintf(campos[5].val, sizeof(campos[5].val), "%lu / %lu", (unsigned long)s.pkt_rx, (unsigned long)s.pkt_tx);
    snprintf(campos[6].val, sizeof(campos[6].val), "%lu · %lu", (unsigned long)s.pkt_ruins, (unsigned long)s.pkt_repetidos);
  } else {
    snprintf(campos[5].val, sizeof(campos[5].val), "-");
    snprintf(campos[6].val, sizeof(campos[6].val), "-");
  }
  campos[5].rot = tr("pacotes recebidos / enviados"); campos[5].cor = TEMA_DESTAQUE;
  campos[6].rot = tr("pacotes ruins · repetidos");    campos[6].cor = TEMA_DESTAQUE;
  {
    const uint32_t meu_no = tab5_mesh_meu_numero();
    int vizinhos = 0, mais_recente = -1;
    int bat = -1;
    const int nn = tab5_mesh_num_nos();
    for (int i = 0; i < nn; ++i) {
      mesh_no_t no;
      if (!tab5_mesh_no(i, &no)) { continue; }
      if (meu_no && no.num == meu_no) { bat = no.bateria; continue; }
      if (no.idade_s < 0) { continue; }
      if (mais_recente < 0 || no.idade_s < mais_recente) { mais_recente = no.idade_s; }
      if (no.saltos == 0 && !no.via_mqtt && no.idade_s <= 7200) { ++vizinhos; }
    }
    snprintf(campos[7].val, sizeof(campos[7].val), "%d", vizinhos);
    campos[7].rot = tr("vizinhos diretos (2 h)"); campos[7].cor = TEMA_SUCESSO;
    if (mais_recente >= 0) {
      char t[8]; idade_texto(mais_recente, t, sizeof(t));
      snprintf(campos[8].val, sizeof(campos[8].val), tr("há %s"), t);
    } else {
      snprintf(campos[8].val, sizeof(campos[8].val), "-");
    }
    campos[8].rot = tr("último nó ouvido"); campos[8].cor = TEMA_TEXTO;
    if (bat < 0)        { snprintf(campos[9].val, sizeof(campos[9].val), "-"); }
    else if (bat > 100) { snprintf(campos[9].val, sizeof(campos[9].val), tr("na tomada")); }
    else                { snprintf(campos[9].val, sizeof(campos[9].val), "%d %%", bat); }
    campos[9].rot = tr("bateria do meu nó"); campos[9].cor = TEMA_TEXTO;
  }

  // DUAS COLUNAS, passo vertical repartido pela altura que sobra (nunca menor
  // que antes). A largura de cada valor e medida: o que nao couber na coluna
  // cai para a fonte menor em vez de invadir a vizinha.
  //
  // EM PE, UMA COLUNA SO: a metade de ~630 px nao cabe nem o rotulo mais
  // longo, e a altura que sobra da para as dez linhas uma embaixo da outra.
  const int colunas = em_pe(a) ? 1 : 2;
  const int por_col = 10 / colunas;
  const int col_l = (a.x + a.largura - x0 - 48) / colunas;
  int passo = (a.altura - 24 - 40) / por_col;
  if (passo < 88) { passo = 88; }
  if (passo > 120) { passo = 120; }

  // Repinta so o valor que mudou: a pagina roda a 1 Hz.
  static char s_val_ant[10][48];
  for (int i = 0; i < 10; ++i) {
    const int x = x0 + 24 + (i / por_col) * col_l;
    const int y = a.y + 24 + (i % por_col) * passo;
    if (!pintura_mudou_s(&s_p, s_val_ant[i], sizeof(s_val_ant[i]), campos[i].val)) { continue; }
    if (!TUDO) { tab5_tela_limpar(x, y, col_l - 8, passo - 2, TEMA_FUNDO); }
    tab5_texto(d, 3.12f);
    d.setTextDatum(top_left);
    d.setTextColor(TEMA_TEXTO_2, TEMA_FUNDO);
    d.drawString(campos[i].rot, x, y);
    tab5_texto(d, 5.85f);
    if (d.textWidth(campos[i].val) > col_l - 16) { tab5_texto(d, 4.2f); }
    d.setTextColor(campos[i].cor, TEMA_FUNDO);
    d.drawString(campos[i].val, x, y + 26);
  }

  // O NOSSO NO PELO NOME, com o id ao lado - era so o numero em hexa.
  const uint32_t meu = tab5_mesh_meu_numero();
  char curto[MESH_NOME_CURTO * 2] = "", longo[MESH_NOME_LONGO] = "";
  tab5_mesh_nome_cache(meu, curto, sizeof(curto));
  tab5_mesh_nome_longo_cache(meu, longo, sizeof(longo));
  if (!meu) { snprintf(buf, sizeof(buf), tr("meu nó: ainda não apresentado")); }
  else {
    snprintf(buf, sizeof(buf), tr("meu nó: %s%s%s%s  !%08lx   firmware %s"),
             longo, (longo[0] && curto[0]) ? " (" : "", curto, (longo[0] && curto[0]) ? ")" : "",
             (unsigned long)meu, tab5_mesh_firmware()[0] ? tab5_mesh_firmware() : "-");
  }
  static char s_rodape_ant[sizeof(buf)];
  if (pintura_mudou_s(&s_p, s_rodape_ant, sizeof(s_rodape_ant), buf)) {
    if (!TUDO) { tab5_tela_limpar(x0, a.y + a.altura - 34, a.x + a.largura - x0, 34, TEMA_FUNDO); }
    texto_que_cabe(d, buf, a.x + a.largura - 16 - (x0 + 24), 3.12f, 2.4f);
    d.setTextDatum(top_left);
    d.setTextColor(TEMA_TEXTO_3, TEMA_FUNDO);
    d.drawString(buf, x0 + 24, a.y + a.altura - 30);
  }
}
