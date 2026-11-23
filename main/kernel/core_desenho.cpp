// CORE - drawing: the usable area, measuring full passes, the refused-app
// screen and the content drawing pass (home, lock screen, notices or
// app). Split from core.cpp (the engine); what the parts share is in
// core_interno.hpp.

#include "tab5_tema.hpp"
#include "ajustes.h"
#include "esp_attr.h"
#include "ritmo.hpp"
#include "rolagem.hpp"
#include "tela_virtual.hpp"
#include "compositor.hpp"
#include "tab5_tela.hpp"
#include "tab5_ui.hpp"
#include "tab5_hora.hpp"
#include "idioma.h"
#include <stdio.h>
#include "esp_timer.h"
#include "core_interno.hpp"

area_t area_util(void)
{
  area_t a;
  a.x       = 0;
  a.y       = ALTURA_BARRA;
  a.largura = M5.Display.width();
  a.altura  = M5.Display.height() - ALTURA_BARRA - ALTURA_NAV;
  return a;
}

// Published in app.h: it is the ONLY correct source of geometry for apps (and
// for kernel/shell.cpp, which uses it through the same public door instead of the
// private area_util() - see the note in kernel/internal.hpp). Apps used to
// copy the heights from here, and the copy went stale on its own.
area_t sistema_area(void) { return area_util(); }
// limpar_area = true ONLY when the screen changed content (app opened/closed).
// On data redraws, the content is painted over the previous one.
// The app needs to know when the system cleared the area beneath it. Without that,
// an app that paints by difference (remembering what it already drew) is left with a
// BLANK screen after idle: it thinks nothing changed, but everything was erased.
// That is what happened with the Settings app.
static bool s_area_limpa = false;
bool sistema_area_foi_limpa(void) { return s_area_limpa; }
float sistema_quadro_dt(void) { return ritmo_dt(); }

// NOT the generic module's rolagem_limite() (kernel/rolagem.hpp), on
// purpose - this one consults s_app->altura_conteudo/altura_agora,
// which are app/core policy, not pure scroll geometry. A different
// name so as not to invite confusing the two.
// The content height of the open app (altura_agora wins over altura_conteudo).
static int altura_do_app(const area_t& a)
{
  if (!s_app) { return 0; }
  return s_app->altura_agora ? s_app->altura_agora(a) : s_app->altura_conteudo;
}

int limite_do_app(const area_t& a)
{
  const int h = altura_do_app(a);
  if (h <= a.altura) { return 0; }
  return h - a.altura;
}

// 'limpar_area' says to PHYSICALLY clear the area (tab5_tela_limpar + nav
// bar) - it costs the big PSRAM burst. 'completo_para_apps' is what each
// app SEES in pintura_completa(): TRUE says "redraw your whole static structure
// (title/help/button kept behind an 'if (s_tudo)'), not just the
// fields whose value changed".
//
// The two used to be the SAME parameter, because there were only two
// cases: full repaint (both true - open/close app, wake,
// requested redraw) or purely incremental (both false - only one value changed,
// nothing moved position). The scroll shift (tab5_tela_rolar,
// see the main loop) created a THIRD case: the on-screen position CHANGED (the
// pixels were shifted, not cleared) but nobody physically cleared
// anything. Calling with (false, true): skips the clearing burst (the
// pixels whose value did not change are already right, the shift took care
// of that) but still tells each app it MUST redraw its own
// static structure - because the strip just exposed by the shift was never
// painted AT THIS position before, and an app whose title/button only draws
// when s_tudo is true (most of Settings does this) would simply
// never fill that strip. It was seen live: scrolling down
// looked fine (the newly exposed strip is thin, the gap went unnoticed),
// but scrolling back up exposed at once the accumulated trail of never-painted
// strips - "everything vanished".
//
// 'recorte_app' - only used by the scroll-shift path
// (see the main loop): limits the app's pixel writes to ONE
// STRIP of the usable area, instead of the whole area. Correct because the app is called
// with the SAME 'rolada' as always (it does not know a clip exists) and
// LovyanGFX silently discards any pixel outside the clip - the
// pixels outside the strip are already right (the shift took care of that before
// calling here), so there is no visual difference in skipping them, just FAR fewer
// bytes written to PSRAM. nullptr (the default) uses the whole usable area, as
// before this date.
// FULL PASS MEASUREMENT, by phase (see sistema_passadas_completas).
uint32_t s_fase_limpar_us = 0;   // the last clearing of the area (+ nav)

#define PASSADAS_MAX 24
EXT_RAM_BSS_ATTR static sistema_passada_t s_passadas[PASSADAS_MAX];   // only the UI and the console read it
static int s_n_passadas = 0;

void anotar_passada(const char* nome, uint32_t us, uint32_t barra, uint32_t limpar, uint32_t conteudo)
{
  sistema_passada_t* p = nullptr;
  for (int i = 0; i < s_n_passadas; ++i) { if (s_passadas[i].nome == nome) { p = &s_passadas[i]; break; } }
  if (!p) {
    if (s_n_passadas >= PASSADAS_MAX) { return; }
    p = &s_passadas[s_n_passadas++];
    *p = {};
    p->nome = nome;
  }
  ++p->n;
  p->us += us;
  p->us_barra += barra;
  p->us_limpar += limpar;
  p->us_conteudo += conteudo;
  if (us > p->max_us) { p->max_us = us; }
}

// Read by the console (another task) without a lock: a copy of diagnostic
// numbers; a skewed value at a rare instant fools nobody.
int sistema_passadas_completas(sistema_passada_t* out, int max)
{
  const int n = s_n_passadas < max ? s_n_passadas : max;
  for (int i = 0; i < n; ++i) { out[i] = s_passadas[i]; }
  return n;
}

// The system screen in place of a refused app (see recusar_app).
static void desenhar_recusa(lgfx::LovyanGFX& d, const area_t& a)
{
  const int cx = a.x + a.largura / 2;
  int y = a.y + a.altura / 2 - 90;
  d.setTextDatum(middle_center);
  tab5_texto(d, 3);
  d.setTextColor(TFT_ORANGE, TFT_BLACK);
  char l[64];
  snprintf(l, sizeof(l), tr("%s precisa do cartão SD"), tr(s_app->nome));
  d.drawString(l, cx, y);
  tab5_texto(d, 2);
  d.setTextColor(TFT_WHITE, TFT_BLACK);
  y += 70;
  d.drawString(tr("O cartão está no computador (modo pendrive)."), cx, y);
  y += 44;
  d.setTextColor(TFT_LIGHTGREY, TFT_BLACK);
  d.drawString(tr("Ele volta ao ejetar no computador,"), cx, y);
  y += 40;
  d.drawString(tr("ou em Ajustes > USB. O app abre sozinho."), cx, y);
  d.setTextDatum(top_left);
  tab5_fonte_corpo(d);
}

void desenhar_conteudo(bool limpar_area, bool completo_para_apps,
                       const area_t* recorte_app)
{
  // THE VIRTUAL SCREEN, not M5.Display (docs/COMPOSITOR.md, stage 3a): nothing here
  // writes any more to the page the panel scans.
  auto& d = tela_virtual();
  area_t a = area_util();

  // EVERY frame goes through the pace (kernel/ritmo): schedules the next periodic one and
  // gives the dt to whoever animates. The owner is whoever is drawing NOW - if it changes, the
  // clock restarts.
  static char DONO_BLOQUEIO, DONO_AVISOS, DONO_HOME;   // only the addresses matter
  const void* dono = s_bloqueado ? (const void*)&DONO_BLOQUEIO
                   : s_painel_avisos ? (const void*)&DONO_AVISOS
                   : s_app ? (const void*)s_app : (const void*)&DONO_HOME;
  ritmo_quadro(dono, periodo_do_app());

  d.startWrite();
  if (limpar_area) {
    // IN STRIPS, not in a single fillRect. The usable area is ~1.6MB of writes to PSRAM
    // and the DSI panel reads from the same memory non-stop; the two bursts together
    // drain its FIFO and the frame comes out BLUE. See tab5_tela.hpp - the IDF
    // driver describes the symptom in those words.
    //
    // It was what happened when LEAVING an app for home: home is almost entirely
    // black, and a big black area is the fastest, most contiguous fill there
    // is. Opening an app writes the same amount, but spread over dozens of
    // small operations - which is why it never flickered on the way in.
    const int64_t limpar_t0 = esp_timer_get_time();
    // THE CLEARING COLOUR (see sistema_cor_fundo, further down). Clearing straight to the
    // background colour of whoever is about to draw makes their identical request (home, for
    // example) fall into the "already clear" below - without this there would be TWO big
    // bursts in a row, exactly what made the frame come out blue.
    const uint16_t cor_limpa = sistema_cor_fundo();
    tab5_tela_limpar(a.x, a.y, a.largura, a.altura, cor_limpa);
    // TELL THE SCREEN LAYER. From here to the end of this pass, every request to
    // clear in this same colour within this area is waived - and the doubled burst that
    // produced the blue flash no longer happens, for EVERY app, without any
    // of them needing to know the rule.
    tab5_tela_marcar_limpo(a.x, a.y, a.largura, a.altura, cor_limpa);
    if (s_bloqueado) {
      // Locked there is no navigation: the bottom strip is cleared here and becomes
      // part of the lock screen (see desenhar_bloqueio, further down).
      tab5_tela_limpar(0, d.height() - ALTURA_NAV, d.width(), ALTURA_NAV, TFT_BLACK);
    } else {
      desenhar_barra_nav(d);
    }
    s_fase_limpar_us = (uint32_t)(esp_timer_get_time() - limpar_t0);
  }
  s_area_limpa = completo_para_apps;
  // s_app AND s_app_carregando (not just one of them): both start as NULL, and
  // "nullptr == nullptr" is true - without s_app here, home (with no app
  // open) fell into this condition and tried to read s_app->nome with s_app
  // null. That was the boot crash: Load access fault right on this line.
  if (s_bloqueado) {
    // THE WHOLE SCREEN BELOW THE BAR, including the navigation strip (which
    // does not exist when locked): before, it stayed black, left over at the bottom.
    area_t bq = a;
    bq.altura += ALTURA_NAV;
    desenhar_bloqueio(d, bq, limpar_area);
  } else if (s_painel_avisos) {
    desenhar_painel_avisos(d, a, limpar_area);
  } else if (s_app && s_app == s_app_recusado) {
    desenhar_recusa(d, a);
  } else if (s_app && s_app == s_app_carregando) {
    // STILL MOUNTING THE SD. s_app already points to the app (see abrir_app()), but
    // its ao_abrir() has NOT been called - the app may have no state at all to
    // draw, and calling s_app->desenhar() here would be trusting memory that the
    // app itself has not yet had a chance to prepare. A simple waiting screen
    // until concluir_abertura_do_sd() releases the app for real.
    d.setTextDatum(middle_center);
    tab5_texto(d, 2);
    d.setTextColor(TFT_LIGHTGREY, TFT_BLACK);
    char msg[48];
    snprintf(msg, sizeof(msg), tr("abrindo %s..."), tr(s_app->nome));
    d.drawString(msg, a.x + a.largura / 2, a.y + a.altura / 2);
  } else if (s_app) {
    const int max = limite_do_app(a);
    if (s_app->desenhar && max > 0) {
      // CLIP FIRST. With the content shifted, the app would draw over
      // the status bar and the navigation without noticing - it does not know
      // scrolling exists. The clip is what makes that ignorance safe.
      //
      // recorte_app, when filled in, NARROWS that clip to just the
      // newly exposed strip (see the big note at the top of the function) - the app
      // keeps receiving the SAME 'rolada' as always, without knowing the
      // difference.
      const area_t& clip = recorte_app ? *recorte_app : a;
      d.setClipRect(clip.x, clip.y, clip.largura, clip.altura);
      area_t rolada = a;
      rolada.y     -= s_rol.atual;
      rolada.altura = altura_do_app(a);
      s_app->desenhar(d, rolada);
      d.clearClipRect();
    } else if (s_app->desenhar) {
      // ALWAYS CLIP, even without scrolling: the app (in C or Berry) only paints its
      // own area. A script with the maths wrong - a shape that crosses the
      // edge, a long text - painted the status bar and the navigation
      // (seen in Estresse.be, 14/11/2026). Clipped here, for every app, without
      // any of them needing to check its own coordinates.
      d.setClipRect(a.x, a.y, a.largura, a.altura);
      s_app->desenhar(d, a);
      d.clearClipRect();
    }
  } else if (limpar_area) {
    // ICONS DO NOT CHANGE. Home is static: eight icons and eight labels, always the
    // same. What moves on it - battery, temperature, the services line -
    // is drawn separately, with its own path and a comparison before painting.
    //
    // Redrawing everything on a DATA pass was repainting 578KB to arrive at the
    // same drawing. In a transition that is the difference between fitting and not fitting
    // in the bandwidth the panel leaves over; and the data pass happens on the
    // VISIBLE page, without the protection of the page swap.
    //
    // The condition is exactly right: there is only something to redraw here when the
    // area was CLEARED - and then the drawing really did vanish.
    desenhar_home(d, a);
  }
  d.endWrite();

  s_area_limpa = false;
  // ONLY CLEAR s_sujo_layout WHEN IT WAS THIS PASS THAT SERVED IT
  // (limpar_area==true, that is: the main loop called here precisely
  // because s_sujo_layout was true). This line used to zero it
  // UNCONDITIONALLY - and s_app->desenhar() (called above, inside the
  // INCREMENTAL pass too, limpar_area==false) may legitimately
  // request a full repaint IN THE MIDDLE of its own data pass (e.g.:
  // the Camera app calling sistema_travar_navegacao(false) from inside
  // its desenhar(), on noticing that a capture finished). Zeroing here
  // unconditionally erased that request before the main loop got to see it
  // - the navigation bar was never redrawn unlocked, and INICIO (home)
  // stayed dark forever (observed defect, "captura concluida" appeared
  // in the log and the button stayed dark all the same).
  //
  // NOT JUST THE CAMERA: any app that calls sistema_pedir_redesenho_completo()/
  // sistema_travar_navegacao() from inside its own
  // desenhar() would fall into the same trap - the fix is here, not in each app.
  if (limpar_area) { s_sujo_layout = false; }
  s_sujo_dados  = false;
  s_ultimo_desenho = tab5_agora_ms();

  // THE MARK DIES WITH THE PASS. It asserts "this rectangle is this colour
  // NOW"; once the app has drawn on top, it stops being true. Leaving it
  // alive until the next pass would make the next clearing be waived over
  // real content - and the app would be left with a trail because of a system optimisation.
  tab5_tela_esquecer_limpo();
}

// One frame of the lock screen during the wake-up brightness ramp.
void animar_na_rampa(void)
{
  if (!s_bloqueado) { return; }
  desenhar_conteudo(false, false);
  comp_apresentar();
}

// The area's background colour right now (declared in app.h). Home without a
// wallpaper and apps already migrated (app_t.fundo_tema) use the theme background; the
// rest - lock screen, home with a wallpaper, apps not yet migrated - stays
// on black, because they draw text with a TFT_BLACK background.
uint16_t sistema_cor_fundo(void)
{
  if (s_bloqueado) { return TFT_BLACK; }
  if (s_app)       { return s_app->fundo_tema ? TEMA_FUNDO : TFT_BLACK; }
  return ajustes_parede() == AJUSTES_PAREDE_NENHUM ? TEMA_FUNDO : TFT_BLACK;
}
