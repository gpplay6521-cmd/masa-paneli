#include "ui.h"

#include <stdio.h>
#include <string.h>

#include "board.h"
#include "fonts/fonts.h"
#include "portal.h"
#include "proto.h"
#include "settings.h"
#include "strings_tr.h"
#include "theme.h"

/* ========================================================================== */
/* Ortak yardımcılar                                                           */
/* ========================================================================== */

typedef enum {
    SCR_MAIN, SCR_SETTINGS, SCR_THEME, SCR_IDLE, SCR_OFFLINE, SCR_SYSTEM, SCR_TIMER, SCR_ALARM, SCR_INSTALL,
    SCR_INSTALL_CHOOSE, SCR_INSTALL_USB, SCR_NONE
} scr_id_t;

static scr_id_t s_cur = SCR_NONE;
static lv_timer_t *s_scr_timer;          /* etkin ekranın güncelleme zamanlayıcısı */
static lv_style_t st_btn, st_press;      /* dokunma geri bildirimi (%92'ye küçülme, 140 ms) */
static lv_style_transition_dsc_t st_trans;
static lv_grad_dsc_t s_grads[32];        /* degrade tanımları kalıcı bellekte durmalı */
static int s_grad_i;
static lv_draw_buf_t *s_theme_tile_snap[THEME_COUNT];   /* bkz. theme_tile_snapshot() */
static uint32_t s_hold_until;            /* dokunuştan sonra PC durumunu kısa süre yok say */
static int s_hold_state;

#define IDLE_BRIGHTNESS_PCT 12

static bool s_dyn_active;                 /* kapak renkleri şu an geçerli mi (anahtar açık ve kapak var) */
static inline const theme_t *th(void)
{
    return s_dyn_active ? &g_theme_dyn : &THEMES[g_settings.theme % THEME_COUNT];
}
static inline const theme_t *th_selected(void) { return &THEMES[g_settings.theme % THEME_COUNT]; }
static bool dyn_refresh(void);
static inline lv_color_t hex(uint32_t c) { return lv_color_hex(c); }

static void ui_show(scr_id_t id, lv_screen_load_anim_t anim, uint32_t ms);

/* Aynı düğmeye çok yakın iki tıklamayı (dokunmatik sıçraması, çift dokunuş) tek say */
static bool tap_ok(uint32_t *last, uint32_t gap_ms)
{
    uint32_t now = lv_tick_get();
    if (*last != 0 && lv_tick_elaps(*last) < gap_ms) return false;
    *last = now ? now : 1;
    return true;
}

/* Etiketi en fazla max_lines satıra sığdırıp metni yazar; fazlası "..." ile kesilir.
 * Ölçüm ÖZGÜN metinle yapılır: LV_LABEL_LONG_DOT kısaltmayı etiketin kendi metninde yaptığı için etiketten okunan metin
 * (kısaltılmış) ölçülürse yükseklik hep bir önceki duruma takılır. Önce yükseklik, sonra metin verilir. */
static void fit_lines(lv_obj_t *lbl, const char *text, const lv_font_t *font, int width, int max_lines)
{
    lv_point_t sz;
    lv_text_get_size(&sz, text, font, 0, 0, width, LV_TEXT_FLAG_NONE);
    int lh = font->line_height;
    int h = sz.y;
    if (h > max_lines * lh) h = max_lines * lh;
    if (h < lh) h = lh;
    lv_obj_set_height(lbl, h);
    lv_label_set_text(lbl, text);
}

static lv_obj_t *plain(lv_obj_t *parent)
{
    lv_obj_t *o = lv_obj_create(parent);
    lv_obj_remove_style_all(o);
    lv_obj_remove_flag(o, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_remove_flag(o, LV_OBJ_FLAG_CLICKABLE);   /* LVGL nesneleri varsayılan tıklanabilir; yalnızca düğmeler açıkça tıklanabilir yapılır,
                                                       yoksa düğme içindeki kap dokunmayı yutar */
    return o;
}

static lv_obj_t *box(lv_obj_t *parent, int x, int y, int w, int h, int radius, uint32_t color)
{
    lv_obj_t *o = plain(parent);
    lv_obj_set_pos(o, x, y);
    lv_obj_set_size(o, w, h);
    lv_obj_set_style_radius(o, radius, 0);
    lv_obj_set_style_bg_color(o, hex(color), 0);
    lv_obj_set_style_bg_opa(o, LV_OPA_COVER, 0);
    return o;
}

static lv_obj_t *label(lv_obj_t *parent, const lv_font_t *font, uint32_t color, const char *text)
{
    lv_obj_t *l = lv_label_create(parent);
    lv_obj_set_style_text_font(l, font, 0);
    lv_obj_set_style_text_color(l, hex(color), 0);
    lv_label_set_text(l, text);
    return l;
}

static lv_obj_t *icon(lv_obj_t *parent, const lv_font_t *font, uint32_t color, const char *glyph)
{
    return label(parent, font, color, glyph);
}

/* Yuvarlak, dokunulabilir düğme; basınca küçülür */
static lv_obj_t *circle_btn(lv_obj_t *parent, int x, int y, int d, uint32_t bg, lv_event_cb_t cb, void *ud)
{
    lv_obj_t *b = box(parent, x, y, d, d, LV_RADIUS_CIRCLE, bg);
    lv_obj_add_flag(b, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_style_transform_pivot_x(b, d / 2, 0);
    lv_obj_set_style_transform_pivot_y(b, d / 2, 0);
    lv_obj_add_style(b, &st_btn, 0);
    lv_obj_add_style(b, &st_press, LV_STATE_PRESSED);
    if (cb) lv_obj_add_event_cb(b, cb, LV_EVENT_CLICKED, ud);
    return b;
}

static void styles_init(void)
{
    static bool done;
    if (done) return;
    done = true;
    static const lv_style_prop_t props[] = { LV_STYLE_TRANSFORM_SCALE_X, LV_STYLE_TRANSFORM_SCALE_Y, LV_STYLE_PROP_INV };
    lv_style_transition_dsc_init(&st_trans, props, lv_anim_path_ease_out, 140, 0, NULL);
    lv_style_init(&st_btn);
    lv_style_set_transition(&st_btn, &st_trans);
    lv_style_init(&st_press);
    lv_style_set_transform_scale_x(&st_press, 235);
    lv_style_set_transform_scale_y(&st_press, 235);
    lv_style_set_transition(&st_press, &st_trans);
}

static void page_bg(lv_obj_t *scr)
{
    lv_color_t top = lv_color_mix(hex(COL_BG), hex(th()->accent), 214);   /* %84 koyu */
    lv_obj_set_style_bg_color(scr, top, 0);
    lv_obj_set_style_bg_grad_color(scr, hex(COL_BG), 0);
    lv_obj_set_style_bg_grad_dir(scr, LV_GRAD_DIR_VER, 0);
    lv_obj_set_style_bg_main_stop(scr, 0, 0);
    lv_obj_set_style_bg_grad_stop(scr, 158, 0);
    lv_obj_set_style_bg_opa(scr, LV_OPA_COVER, 0);
}

static lv_obj_t *new_screen(void)
{
    lv_obj_t *scr = lv_obj_create(NULL);
    lv_obj_remove_flag(scr, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_pad_all(scr, 0, 0);
    lv_obj_set_style_border_width(scr, 0, 0);
    lv_obj_set_style_radius(scr, 0, 0);
    return scr;
}

/* Tema gradyanı: gece -> derin -> vurgu (yatay) */
static void hero_grad(lv_obj_t *o, const theme_t *t)
{
    lv_grad_dsc_t *g = &s_grads[s_grad_i++ % 32];
    memset(g, 0, sizeof(*g));
    g->dir = LV_GRAD_DIR_HOR;
    g->stops_count = 3;
    g->stops[0] = (lv_gradient_stop_t){ .color = hex(t->night), .opa = LV_OPA_COVER, .frac = 0 };
    g->stops[1] = (lv_gradient_stop_t){ .color = hex(t->deep), .opa = LV_OPA_COVER, .frac = 140 };
    g->stops[2] = (lv_gradient_stop_t){ .color = hex(t->accent), .opa = LV_OPA_COVER, .frac = 255 };
    lv_obj_set_style_bg_grad(o, g, 0);
    lv_obj_set_style_bg_opa(o, LV_OPA_COVER, 0);
}

/* Tema ekranındaki 7 karo her zaman kendi sabit temasının degradesini gösterir (THEMES[] sabit dizi, çalışma
 * zamanında değişmez); canlı bir bg_grad stili olarak her kaydırma animasyonu karesinde yeniden hesaplatmak yerine
 * (gerçek cihazda ölçüldü: bu yüzden Tema ekranı geçişi Ayarlar'a göre ~2 kat daha az kare/sn ve ~3 kat daha
 * fazla CPU harcıyordu — LVGL'nin yazılım degrade çizimi düz renkten çok daha pahalı), görüntüyü ilk kullanımda
 * bir kez "pişirip" (lv_snapshot) önbelleğe alır; sonraki her Tema ekranı açılışında hazır resmi yeniden kullanır. */
static lv_draw_buf_t *theme_tile_snapshot(int idx)
{
    if (s_theme_tile_snap[idx]) return s_theme_tile_snap[idx];
    lv_obj_t *tmp = plain(lv_layer_top());
    lv_obj_set_size(tmp, 87, 87);
    lv_obj_set_style_radius(tmp, 27, 0);
    hero_grad(tmp, &THEMES[idx]);
    lv_obj_update_layout(tmp);
    s_theme_tile_snap[idx] = lv_snapshot_take(tmp, LV_COLOR_FORMAT_ARGB8888);
    lv_obj_delete(tmp);
    return s_theme_tile_snap[idx];
}

/* Başlık kartındaki halkalar (kapak yerine) — kart içindeki koordinatlar, tuval v4 ile aynı */
static lv_obj_t *hero_rings(lv_obj_t *card, int w, int h, float k)
{
    lv_obj_t *g = plain(card);
    lv_obj_set_size(g, w, h);
    lv_obj_set_pos(g, 0, 0);
    int d1 = (int)(264 * k), d2 = (int)(200 * k), d3 = (int)(96 * k);
    lv_obj_t *r1 = plain(g);
    lv_obj_set_size(r1, d1, d1);
    lv_obj_set_pos(r1, w - (int)(200 * k), -(int)(52 * k));
    lv_obj_set_style_radius(r1, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_border_width(r1, (int)(44 * k), 0);
    lv_obj_set_style_border_color(r1, hex(0xFFFFFF), 0);
    lv_obj_set_style_border_opa(r1, 26, 0);
    lv_obj_t *r2 = plain(g);
    lv_obj_set_size(r2, d2, d2);
    lv_obj_set_pos(r2, w - (int)(28 * k) - d2, h + (int)(96 * k) - d2);
    lv_obj_set_style_radius(r2, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_color(r2, hex(0x000000), 0);
    lv_obj_set_style_bg_opa(r2, 56, 0);
    lv_obj_t *r3 = plain(g);
    lv_obj_set_size(r3, d3, d3);
    lv_obj_set_pos(r3, w - (int)(76 * k) - d3, (int)(32 * k));
    lv_obj_set_style_radius(r3, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_border_width(r3, (int)(12 * k), 0);
    lv_obj_set_style_border_color(r3, hex(0xFFFFFF), 0);
    lv_obj_set_style_border_opa(r3, 87, 0);
    return g;
}

/* ----- animasyon yardımcıları ----- */
static void a_opa(void *o, int32_t v) { lv_obj_set_style_opa(o, (lv_opa_t)v, 0); }
static void a_ty(void *o, int32_t v) { lv_obj_set_style_translate_y(o, v, 0); }
static void a_tx(void *o, int32_t v) { lv_obj_set_style_translate_x(o, v, 0); }
static void a_scale(void *o, int32_t v)
{
    lv_obj_set_style_transform_scale_x(o, v, 0);
    lv_obj_set_style_transform_scale_y(o, v, 0);
}
static void a_rot(void *o, int32_t v) { lv_obj_set_style_transform_rotation(o, v, 0); }
static void a_x(void *o, int32_t v) { lv_obj_set_x(o, v); }

static void anim_run(void *obj, lv_anim_exec_xcb_t cb, int32_t from, int32_t to, uint32_t ms, uint32_t delay,
                     lv_anim_path_cb_t path)
{
    lv_anim_t a;
    lv_anim_init(&a);
    lv_anim_set_var(&a, obj);
    lv_anim_set_exec_cb(&a, cb);
    lv_anim_set_values(&a, from, to);
    lv_anim_set_duration(&a, ms);
    lv_anim_set_delay(&a, delay);
    lv_anim_set_path_cb(&a, path ? path : lv_anim_path_ease_out);
    lv_anim_start(&a);
}

/* Ekran girişi: aşağıdan yükselerek belirir (320 ms) */
static void rise_in(lv_obj_t *o, uint32_t delay)
{
    lv_obj_set_style_opa(o, 0, 0);
    anim_run(o, a_ty, 18, 0, 320, delay, lv_anim_path_ease_out);
    anim_run(o, a_opa, 0, 255, 320, delay, lv_anim_path_ease_out);
}

/* Yaylanarak belirme (simge/durum değişimi) */
static void pop_in(lv_obj_t *o, int w, int h)
{
    lv_obj_set_style_transform_pivot_x(o, w / 2, 0);
    lv_obj_set_style_transform_pivot_y(o, h / 2, 0);
    anim_run(o, a_scale, 102, 256, 260, 0, lv_anim_path_overshoot);
}

/* Düğmeden dışa yayılan halka (520 ms) */
static void ring_done(lv_anim_t *a) { lv_obj_delete(a->var); }
static void ring_pulse(lv_obj_t *parent, int x, int y, int d, uint32_t color)
{
    lv_obj_t *r = plain(parent);
    lv_obj_set_pos(r, x, y);
    lv_obj_set_size(r, d, d);
    lv_obj_set_style_radius(r, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_border_width(r, 3, 0);
    lv_obj_set_style_border_color(r, hex(color), 0);
    lv_obj_set_style_border_opa(r, 140, 0);
    lv_obj_remove_flag(r, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_style_transform_pivot_x(r, d / 2, 0);
    lv_obj_set_style_transform_pivot_y(r, d / 2, 0);
    anim_run(r, a_scale, 256, 460, 520, 0, lv_anim_path_ease_out);
    anim_run(r, a_opa, 255, 0, 520, 0, lv_anim_path_ease_out);
    lv_anim_t *a = lv_anim_get(r, a_opa);
    if (a) lv_anim_set_completed_cb(a, ring_done);   /* opaklık animasyonu bitince halkayı sil */
}

/* ----- üç çubuk (çalarken oynar) ----- */
typedef struct { lv_obj_t *bar[3]; } eq_t;
static void a_eq(void *o, int32_t v)
{
    lv_obj_set_height(o, v);
    lv_obj_set_y(o, 18 - v);
}
static void eq_create(eq_t *e, lv_obj_t *parent, int x, int y, uint32_t color)
{
    lv_obj_t *c = plain(parent);
    lv_obj_set_pos(c, x, y);
    lv_obj_set_size(c, 18, 18);
    for (int i = 0; i < 3; i++) {
        e->bar[i] = box(c, i * 7, 13, 4, 5, 2, color);
    }
}
static void eq_run(eq_t *e, bool on)
{
    static const uint16_t dur[3] = { 720, 540, 880 };
    for (int i = 0; i < 3; i++) {
        lv_anim_delete(e->bar[i], a_eq);
        if (on) {
            lv_anim_t a;
            lv_anim_init(&a);
            lv_anim_set_var(&a, e->bar[i]);
            lv_anim_set_exec_cb(&a, a_eq);
            lv_anim_set_values(&a, 5, 18);
            lv_anim_set_duration(&a, dur[i]);
            lv_anim_set_playback_duration(&a, dur[i]);
            lv_anim_set_repeat_count(&a, LV_ANIM_REPEAT_INFINITE);
            lv_anim_set_path_cb(&a, lv_anim_path_ease_in_out);
            lv_anim_start(&a);
        } else {
            a_eq(e->bar[i], 5);
        }
    }
}

static void fmt_time(char *out, size_t n, int32_t ms)
{
    int s = ms / 1000;
    snprintf(out, n, "%d:%02d", s / 60, s % 60);
}

/* Ondalık virgüllü GB (Türkçe yazım): 18432 MB -> "18,0" */
static void fmt_gb(char *out, size_t n, int mb)
{
    int t = (mb * 10 + 512) / 1024;
    snprintf(out, n, "%d,%d", t / 10, t % 10);
}

/* Hız: kbps -> "12,4 Mbps" / "820 Kbps" */
static void fmt_rate(char *out, size_t n, int kbps)
{
    if (kbps == PROTO_NA || kbps < 0) snprintf(out, n, "—");
    else if (kbps >= 1000) snprintf(out, n, "%d,%d Mbps", (kbps + 50) / 1000, ((kbps + 50) / 100) % 10);
    else snprintf(out, n, "%d Kbps", kbps);
}

/* Süre: 4500 sn -> "1 sa 15 dk", 900 sn -> "15 dk" */
static void fmt_dur(char *out, size_t n, int sec)
{
    int h = sec / 3600, m = (sec / 60) % 60;
    if (h > 0 && m > 0) snprintf(out, n, "%d sa %d dk", h, m);
    else if (h > 0) snprintf(out, n, "%d sa", h);
    else snprintf(out, n, "%d dk", m);
}

/* ========================================================================== */
/* Zamanlayıcı çekirdeği: ekrandan bağımsız çalışır (başka ekranda da sayar)    */
/* ========================================================================== */

typedef enum { TM_IDLE, TM_RUNNING, TM_PAUSED, TM_DONE } tm_state_t;
enum { PH_FOCUS, PH_SHORT, PH_LONG };

static struct {
    int mode;               /* 0 geri sayım, 1 pomodoro */
    int phase;              /* pomodoro evresi */
    int round;              /* pomodoro: 1..4 */
    tm_state_t state;
    int32_t total_ms;       /* etkin evrenin toplam süresi */
    int32_t left_ms;        /* boşta/duraklatılmışken kalan süre */
    uint32_t end_tick;      /* çalışırken bitiş anı (lv_tick) */
    int32_t count_min;      /* geri sayım modunda seçili süre (dk) */
} tm = { .mode = 0, .phase = PH_FOCUS, .round = 1, .state = TM_IDLE, .count_min = 10 };

static int32_t tm_phase_min(int phase)
{
    static const int32_t mins[3] = { 25, 5, 15 };
    return mins[phase];
}

static void tm_load(void)          /* etkin evrenin süresini yükle, boşa al */
{
    tm.state = TM_IDLE;
    tm.total_ms = (tm.mode == 1 ? tm_phase_min(tm.phase) : tm.count_min) * 60000;
    tm.left_ms = tm.total_ms;
}

static int32_t tm_left(void)
{
    if (tm.state != TM_RUNNING) return tm.left_ms;
    int32_t d = (int32_t)(tm.end_tick - lv_tick_get());
    return d < 0 ? 0 : d;
}

static void tm_start(void)
{
    if (tm.state == TM_DONE || tm.left_ms <= 0) tm_load();
    tm.end_tick = lv_tick_get() + (uint32_t)tm.left_ms;
    tm.state = TM_RUNNING;
}

static void tm_pause(void)
{
    tm.left_ms = tm_left();
    tm.state = TM_PAUSED;
}

static void tm_reset(void)
{
    tm.phase = PH_FOCUS;
    tm.round = 1;
    tm_load();
}

/* Pomodoro: sıradaki evreye geç (odak -> mola -> odak ...; 4. odaktan sonra uzun mola) */
static void tm_next_phase(void)
{
    if (tm.phase == PH_FOCUS) {
        tm.phase = tm.round >= 4 ? PH_LONG : PH_SHORT;
    } else {
        tm.round = tm.phase == PH_LONG ? 1 : tm.round + 1;
        tm.phase = PH_FOCUS;
    }
    tm_load();
}

static const char *tm_phase_name(void)
{
    return tm.phase == PH_FOCUS ? TR_FOCUS : (tm.phase == PH_SHORT ? TR_SHORT_BREAK : TR_LONG_BREAK);
}

/* Mola hatırlatıcı: bilgisayar başında kesintisiz geçen süre (@U) ayardaki aralığa ulaşınca hatırlatır */
static int s_break_ref_s;        /* oturum saniyesi: son hatırlatma/mola/ayar değişimi anındaki değer (aralıklı kip) */
static int s_break_test_s;       /* tanılama: >0 ise aralıklı kipte hatırlatma aralığı (sn) */
static int s_hour_key = -1;      /* "saat başı" kipi: en son değerlendirilen saat (yyyymmddhh) */
static int s_hour_minute;        /* saat başı hatırlatmanın dakikası (0; tanılamada değişebilir) */
static int s_hour_min_sess_s = 20 * 60;   /* saat başında hatırlatmak için en az oturum süresi (yeni oturduysa "mola" anlamsız) */

static int break_interval_s(void)
{
    return s_break_test_s > 0 ? s_break_test_s : settings_break_min() * 60;
}

static int session_now(void)     /* -1: bilgisayar oturum süresini vermiyor */
{
    int sec;
    return proto_session_sec(&sec) ? sec : -1;
}

/* Üst çubuk: geri düğmesi + başlık (Ayarlar / Tema) */
static void back_cb(lv_event_t *e)
{
    ui_show(SCR_MAIN, LV_SCR_LOAD_ANIM_FADE_IN, 300);
}
static void header_back_cb(lv_obj_t *scr, const char *title, lv_event_cb_t cb)
{
    lv_obj_t *b = circle_btn(scr, 32, 24, 56, COL_SURFACE, cb, NULL);
    lv_obj_t *ic = icon(b, &font_ikon26, COL_TEXT, ICON_LEFT);
    lv_obj_center(ic);
    lv_obj_t *t = label(scr, &font_h1, COL_TEXT, title);
    lv_obj_align(t, LV_ALIGN_TOP_LEFT, 104, 24 + (56 - 41) / 2);
    lv_obj_set_style_translate_x(b, 56, 0);
    lv_obj_set_style_translate_x(t, 56, 0);
    lv_obj_set_style_opa(b, 0, 0);
    lv_obj_set_style_opa(t, 0, 0);
    anim_run(b, a_tx, 56, 0, 320, 0, lv_anim_path_ease_out);
    anim_run(b, a_opa, 0, 255, 320, 0, lv_anim_path_ease_out);
    anim_run(t, a_tx, 56, 0, 320, 0, lv_anim_path_ease_out);
    anim_run(t, a_opa, 0, 255, 320, 0, lv_anim_path_ease_out);
}
static void header_back(lv_obj_t *scr, const char *title)
{
    header_back_cb(scr, title, back_cb);
}

/* ========================================================================== */
/* 1) Şimdi çalıyor                                                            */
/* ========================================================================== */

static struct {
    eq_t eq;
    lv_obj_t *state_lbl, *title, *artist, *text_box, *rings;
    lv_obj_t *fill, *knob, *grab, *bubble, *bubble_lbl, *t_elapsed, *t_dur;
    bool dragging, moved;        /* topu tutup sürükleme: sarma hedefi drag_ms */
    int32_t drag_ms, dur_ms, grab_dx, press_x;
    lv_obj_t *play_btn, *play_icon;
    lv_obj_t *cover_ph, *cover_img, *tm_chip, *tm_chip_lbl;
    uint32_t shown_cover_seq;
    int shown_chip_s;            /* -1: gizli */
    char shown_title[PROTO_TEXT_MAX], shown_artist[PROTO_TEXT_MAX];
    int shown_playing;           /* -1: bilinmiyor */
    int32_t shown_w, shown_elapsed_s, shown_dur_s;
} m;

/* Kapak resim tanımı `m` dışında durur: ekran geçişi sırasında eski ekran bunu hâlâ çizebilir */
static lv_image_dsc_t s_cover_dsc = {
    .header = { .magic = LV_IMAGE_HEADER_MAGIC, .cf = LV_COLOR_FORMAT_RGB565, .w = PROTO_COVER_W,
                .h = PROTO_COVER_H, .stride = PROTO_COVER_W * 2 },
    .data_size = PROTO_COVER_W * PROTO_COVER_H * 2,
};

/* Kalan süreyi "dd:ss" yaz (zamanlayıcı en çok 99 dk) */
static void fmt_left(char *out, size_t n, int32_t ms)
{
    int s = (ms + 999) / 1000;
    snprintf(out, n, "%02d:%02d", s / 60, s % 60);
}

/* İlerleme çubuğu: kapsayıcı çubuktan büyüktür; top çubuğun uçlarında ve tutulunca büyüyünce kırpılmasın diye her yanda pay vardır
 * (LVGL çocuk nesneyi ebeveyninin sınırında keser). */
#define BAR_W 736
#define PROG_X 16
#define PROG_Y 258
#define PROG_H 86
#define BAR_X 16                       /* çubuk, kapsayıcı içinde */
#define BAR_Y 32
#define KNOB_D 26
#define KNOB_D_HELD 32                 /* tutulunca top büyür */
#define GRAB_W 96                      /* topu tutma alanı (parmak için topun çok dışına taşar) */
#define GRAB_H 64
#define BUBBLE_W 92

/* w: 0..BAR_W, çubuğun dolu kısmı (px). Top dolgunun ucunda durur. */
static void bar_set(int32_t w)
{
    int d = m.dragging ? KNOB_D_HELD : KNOB_D;
    lv_obj_set_width(m.fill, w < 10 ? 10 : w);
    lv_obj_set_size(m.knob, d, d);
    lv_obj_set_pos(m.knob, BAR_X + w - d / 2, BAR_Y + 5 - d / 2);
    lv_obj_set_pos(m.grab, BAR_X + w - GRAB_W / 2, BAR_Y + 5 - GRAB_H / 2);
    m.shown_w = w;
}

/* Sürükleme: parmağın x'inden (topa tuttuğu yerin farkı düşülerek) hedef konumu bulur, çubuğu, süreyi ve baloncuğu günceller */
static void drag_update(void)
{
    lv_point_t p;
    lv_indev_get_point(lv_indev_active(), &p);
    int32_t w = p.x - m.grab_dx - (PROG_X + BAR_X);
    if (w < 0) w = 0;
    if (w > BAR_W) w = BAR_W;
    if (LV_ABS(p.x - m.press_x) >= 4) m.moved = true;
    m.drag_ms = (int32_t)((int64_t)w * m.dur_ms / BAR_W);
    bar_set(w);
    if (m.drag_ms / 1000 != m.shown_elapsed_s) {
        char b[16];
        m.shown_elapsed_s = m.drag_ms / 1000;
        fmt_time(b, sizeof(b), m.drag_ms);
        lv_label_set_text(m.t_elapsed, b);
        lv_label_set_text(m.bubble_lbl, b);
        lv_obj_center(m.bubble_lbl);
    }
    int32_t bx = PROG_X + BAR_X + w - BUBBLE_W / 2;
    if (bx < 8) bx = 8;
    if (bx > 800 - 8 - BUBBLE_W) bx = 800 - 8 - BUBBLE_W;
    lv_obj_set_x(m.bubble, bx);
}

static void grab_cb(lv_event_t *e)
{
    lv_event_code_t c = lv_event_get_code(e);
    if (c == LV_EVENT_PRESSED) {
        proto_state_t s;
        proto_get(&s);
        if (s.state == 0 || s.dur_ms <= 0) return;                    /* çalan parça yok: sarılacak bir şey yok */
        lv_point_t p;
        lv_indev_get_point(lv_indev_active(), &p);
        m.dur_ms = s.dur_ms;
        m.press_x = p.x;
        m.grab_dx = p.x - (PROG_X + BAR_X + (m.shown_w < 0 ? 0 : m.shown_w));   /* parmak topun neresinde: top parmağa sıçramasın */
        m.dragging = true;
        m.moved = false;
        lv_obj_remove_flag(m.bubble, LV_OBJ_FLAG_HIDDEN);
        m.shown_elapsed_s = -1;
        drag_update();
    } else if (c == LV_EVENT_PRESSING && m.dragging) {
        drag_update();
    } else if ((c == LV_EVENT_RELEASED || c == LV_EVENT_PRESS_LOST) && m.dragging) {
        m.dragging = false;
        lv_obj_add_flag(m.bubble, LV_OBJ_FLAG_HIDDEN);
        bar_set(m.shown_w);                                            /* top normal boyuna döner */
        if (c == LV_EVENT_RELEASED && m.moved) {                       /* yalnızca dokunuldu (sürüklenmedi) ise sarma yok */
            int32_t t = m.drag_ms;
            if (t > m.dur_ms - 1000) t = m.dur_ms > 1000 ? m.dur_ms - 1000 : 0;   /* parça bitmesin */
            proto_send_seek_to(t);
            proto_local_seek_to(t);
            m.shown_w = -1;                                            /* ana döngü çubuğu yeni konuma göre yeniden yerleştirsin */
        }
    }
}

static void main_apply_playing(bool playing, bool animate)
{
    lv_label_set_text(m.state_lbl, playing ? TR_NOW_PLAYING : TR_PAUSED);
    lv_label_set_text(lv_obj_get_child(m.play_icon, 0), playing ? ICON_PAUSE : ICON_PLAY);
    lv_obj_center(lv_obj_get_child(m.play_icon, 0));
    eq_run(&m.eq, playing);
    if (animate) {
        lv_obj_set_style_opa(m.state_lbl, 0, 0);
        anim_run(m.state_lbl, a_opa, 0, 255, 260, 0, lv_anim_path_ease_out);
        pop_in(m.play_icon, 46, 46);
    }
}

static void main_tick(lv_timer_t *t)
{
    proto_state_t s;
    proto_get(&s);
    int state = (lv_tick_get() < s_hold_until) ? s_hold_state : s.state;

    const char *title = s.state == 0 ? TR_NO_MEDIA_TITLE : s.title;
    const char *artist = s.state == 0 ? TR_NO_MEDIA_ARTIST : s.artist;
    if (strcmp(title, m.shown_title) || strcmp(artist, m.shown_artist)) {
        bool first = m.shown_title[0] == 0 && m.shown_artist[0] == 0;
        snprintf(m.shown_title, sizeof(m.shown_title), "%s", title);
        snprintf(m.shown_artist, sizeof(m.shown_artist), "%s", artist);
        fit_lines(m.title, title, &font_baslik, 520, 2);       /* başlık en çok 2 satır, sanatçı 1 satır; fazlası "..." */
        fit_lines(m.artist, artist, &font_govde_b, 520, 1);
        if (!first) {   /* şarkı değişimi: yazılar ve halkalar yükselerek yenilenir */
            lv_obj_set_style_opa(m.text_box, 0, 0);
            anim_run(m.text_box, a_ty, 14, 0, 340, 60, lv_anim_path_ease_out);
            anim_run(m.text_box, a_opa, 0, 255, 340, 60, lv_anim_path_ease_out);
            lv_obj_set_style_opa(m.rings, 0, 0);
            anim_run(m.rings, a_ty, 14, 0, 420, 0, lv_anim_path_ease_out);
            anim_run(m.rings, a_opa, 0, 255, 420, 0, lv_anim_path_ease_out);
        }
    }

    int playing = state == 1;
    if (playing != m.shown_playing) {
        bool animate = m.shown_playing != -1;
        m.shown_playing = playing;
        main_apply_playing(playing, animate);
    }

    /* Kapak: PC'den gelen 128x128 RGB565 resim; yoksa müzik simgeli yer tutucu */
    proto_cover_t cv;
    proto_cover_get(&cv);
    if (cv.seq != m.shown_cover_seq) {
        m.shown_cover_seq = cv.seq;
        if (cv.valid) {
            s_cover_dsc.data = cv.data;
            lv_image_set_src(m.cover_img, &s_cover_dsc);
            lv_obj_invalidate(m.cover_img);
            lv_obj_remove_flag(m.cover_img, LV_OBJ_FLAG_HIDDEN);
            lv_obj_add_flag(m.cover_ph, LV_OBJ_FLAG_HIDDEN);
            lv_obj_set_style_opa(m.cover_img, 0, 0);
            anim_run(m.cover_img, a_opa, 0, 255, 320, 0, lv_anim_path_ease_out);
        } else {
            lv_obj_add_flag(m.cover_img, LV_OBJ_FLAG_HIDDEN);
            lv_obj_remove_flag(m.cover_ph, LV_OBJ_FLAG_HIDDEN);
        }
    }

    /* Zamanlayıcı çalışıyorsa ya da duruyorsa üst şeritte kalan süre görünür */
    int chip_s = (tm.state == TM_RUNNING || tm.state == TM_PAUSED) ? (tm_left() + 999) / 1000 : -1;
    if (chip_s != m.shown_chip_s) {
        if (chip_s < 0) {
            lv_obj_add_flag(m.tm_chip, LV_OBJ_FLAG_HIDDEN);
        } else {
            char b[16];
            fmt_left(b, sizeof(b), chip_s * 1000);
            lv_label_set_text(m.tm_chip_lbl, b);
            lv_obj_remove_flag(m.tm_chip, LV_OBJ_FLAG_HIDDEN);
        }
        m.shown_chip_s = chip_s;
    }

    if (m.dragging) return;              /* top tutulurken çubuğu ve süreyi parmak yönetir */
    int32_t dur = s.state == 0 ? 0 : s.dur_ms;
    int32_t pos = s.state == 0 ? 0 : proto_position_ms(&s);
    int32_t w = dur > 0 ? (int32_t)((int64_t)pos * BAR_W / dur) : 0;
    if (w != m.shown_w) bar_set(w);
    if (pos / 1000 != m.shown_elapsed_s || dur / 1000 != m.shown_dur_s) {
        char b[16];
        m.shown_elapsed_s = pos / 1000;
        m.shown_dur_s = dur / 1000;
        fmt_time(b, sizeof(b), pos);
        lv_label_set_text(m.t_elapsed, b);
        fmt_time(b, sizeof(b), dur);
        lv_label_set_text(m.t_dur, b);
    }
}

static void theme_open_cb(lv_event_t *e) { ui_show(SCR_THEME, LV_SCR_LOAD_ANIM_MOVE_LEFT, 320); }
static void settings_open_cb(lv_event_t *e) { ui_show(SCR_SETTINGS, LV_SCR_LOAD_ANIM_MOVE_LEFT, 320); }
static void system_open_cb(lv_event_t *e) { ui_show(SCR_SYSTEM, LV_SCR_LOAD_ANIM_MOVE_LEFT, 320); }
static void timer_open_cb(lv_event_t *e) { ui_show(SCR_TIMER, LV_SCR_LOAD_ANIM_MOVE_LEFT, 320); }

static void play_cb(lv_event_t *e)
{
    static uint32_t last;
    if (!tap_ok(&last, 250)) return;                  /* çift tıklama = iki kez aç-kapa = hiçbir şey olmaz; yut */
    proto_state_t s;
    proto_get(&s);
    int now_state = (lv_tick_get() < s_hold_until) ? s_hold_state : s.state;
    if (now_state == 0) return;                       /* çalan parça yok */
    proto_send_cmd("play_pause");
    s_hold_state = now_state == 1 ? 2 : 1;            /* PC onaylayana kadar hemen göster */
    s_hold_until = lv_tick_get() + 900;
    proto_local_set_state(s_hold_state);
    lv_obj_t *b = m.play_btn;
    ring_pulse(lv_obj_get_parent(b), lv_obj_get_x(b), lv_obj_get_y(b), 104, th()->accent);
}
static void next_cb(lv_event_t *e)
{
    static uint32_t last;
    if (tap_ok(&last, 350)) proto_send_cmd("next");
}
static void prev_cb(lv_event_t *e)
{
    static uint32_t last;
    if (tap_ok(&last, 350)) proto_send_cmd("prev");
}
static void quick_cmd_cb(lv_event_t *e)
{
    static uint32_t last;
    if (!tap_ok(&last, 400)) return;
    proto_send_cmd((const char *)lv_event_get_user_data(e));
}
static void seek_cb(lv_event_t *e)
{
    static uint32_t last[2];
    int32_t d = (int32_t)(intptr_t)lv_event_get_user_data(e);
    if (!tap_ok(&last[d > 0], 120)) return;
    proto_send_seek(d);
    proto_local_seek(d);
    lv_obj_t *arc = lv_obj_get_child(lv_event_get_target(e), 0);   /* ok simgesi: yönünde kısa dönüş */
    lv_obj_set_style_transform_pivot_x(arc, 17, 0);
    lv_obj_set_style_transform_pivot_y(arc, 20, 0);
    anim_run(arc, a_rot, 0, d > 0 ? 580 : -580, 200, 0, lv_anim_path_ease_out);
    lv_anim_t back;
    lv_anim_init(&back);
    lv_anim_set_var(&back, arc);
    lv_anim_set_exec_cb(&back, a_rot);
    lv_anim_set_values(&back, d > 0 ? 580 : -580, 0);
    lv_anim_set_delay(&back, 200);
    lv_anim_set_duration(&back, 220);
    lv_anim_set_path_cb(&back, lv_anim_path_overshoot);
    lv_anim_start(&back);
}

static lv_obj_t *seek_btn(lv_obj_t *parent, int x, int y, int32_t delta)
{
    lv_obj_t *b = circle_btn(parent, x, y, 64, COL_SURFACE, seek_cb, (void *)(intptr_t)delta);
    lv_obj_t *arc = icon(b, &font_ikon34, COL_TEXT, delta > 0 ? ICON_REDO : ICON_UNDO);
    lv_obj_center(arc);
    lv_obj_t *ten = label(b, &font_mini, COL_TEXT, "10");
    lv_obj_align(ten, LV_ALIGN_CENTER, 0, 2);
    return b;
}

static lv_obj_t *build_main(void)
{
    memset(&m, 0, sizeof(m));
    m.shown_playing = -1;
    m.shown_w = -1;
    m.shown_elapsed_s = -1;
    m.shown_dur_s = -1;
    m.shown_chip_s = -2;
    m.shown_cover_seq = 0xFFFFFFFFu;     /* ilk turda mevcut durumu (kapak var/yok) uygula */
    const theme_t *t = th();
    lv_obj_t *scr = new_screen();
    page_bg(scr);

    /* Üst şerit: sistem, zamanlayıcı, tema, ayarlar */
    lv_obj_t *hdr = plain(scr);
    lv_obj_set_pos(hdr, 32, 24);
    lv_obj_set_size(hdr, 736, 56);
    eq_create(&m.eq, hdr, 6, 19, t->accent);
    m.state_lbl = label(hdr, &font_etiket, COL_DIM, TR_NOW_PLAYING);
    lv_obj_align(m.state_lbl, LV_ALIGN_LEFT_MID, 44, 0);
    lv_obj_t *bsys = circle_btn(hdr, 488, 0, 56, COL_SURFACE, system_open_cb, NULL);
    lv_obj_center(icon(bsys, &font_ikon26, COL_TEXT, ICON_CHIP));
    lv_obj_t *btm = circle_btn(hdr, 552, 0, 56, COL_SURFACE, timer_open_cb, NULL);
    lv_obj_center(icon(btm, &font_ikon26, COL_TEXT, ICON_STOPWATCH));
    lv_obj_t *bt = circle_btn(hdr, 616, 0, 56, COL_SURFACE, theme_open_cb, NULL);
    lv_obj_center(icon(bt, &font_ikon26, t->accent, ICON_PALETTE));
    lv_obj_t *bs = circle_btn(hdr, 680, 0, 56, COL_SURFACE, settings_open_cb, NULL);
    lv_obj_center(icon(bs, &font_ikon26, COL_TEXT, ICON_SLIDERS));
    /* Çalışan zamanlayıcının kalan süresi (dokununca zamanlayıcı açılır) */
    m.tm_chip = circle_btn(hdr, 320, 8, 40, COL_SURFACE, timer_open_cb, NULL);
    lv_obj_set_size(m.tm_chip, 152, 40);
    lv_obj_set_style_radius(m.tm_chip, 20, 0);
    lv_obj_set_style_transform_pivot_x(m.tm_chip, 76, 0);
    lv_obj_add_flag(m.tm_chip, LV_OBJ_FLAG_HIDDEN);
    lv_obj_align(icon(m.tm_chip, &font_ikon26, t->accent, ICON_STOPWATCH), LV_ALIGN_LEFT_MID, 14, 0);
    m.tm_chip_lbl = label(m.tm_chip, &font_etiket, COL_TEXT, "00:00");
    lv_obj_align(m.tm_chip_lbl, LV_ALIGN_RIGHT_MID, -16, 0);
    rise_in(hdr, 0);

    /* Başlık kartı: solda kapak (yoksa yer tutucu), sağda başlık ve sanatçı */
    lv_obj_t *hero = plain(scr);
    lv_obj_set_pos(hero, 32, 96);
    lv_obj_set_size(hero, 736, 152);
    lv_obj_set_style_radius(hero, 32, 0);
    lv_obj_set_style_clip_corner(hero, true, 0);
    hero_grad(hero, t);
    m.rings = hero_rings(hero, 736, 152, 1.0f);
    lv_obj_t *cover = box(hero, 12, 12, PROTO_COVER_W, PROTO_COVER_H, 22, COL_SURFACE);
    lv_obj_set_style_clip_corner(cover, true, 0);
    m.cover_ph = plain(cover);
    lv_obj_set_size(m.cover_ph, PROTO_COVER_W, PROTO_COVER_H);
    lv_obj_center(icon(m.cover_ph, &font_ikon46, COL_DIM, ICON_MUSIC));
    m.cover_img = lv_image_create(cover);
    lv_obj_set_size(m.cover_img, PROTO_COVER_W, PROTO_COVER_H);
    lv_obj_add_flag(m.cover_img, LV_OBJ_FLAG_HIDDEN);
    lv_obj_remove_flag(m.cover_img, LV_OBJ_FLAG_CLICKABLE);
    m.text_box = plain(hero);
    lv_obj_set_pos(m.text_box, 164, 0);
    lv_obj_set_size(m.text_box, 540, 152);
    lv_obj_set_flex_flow(m.text_box, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(m.text_box, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START);
    lv_obj_set_style_pad_row(m.text_box, 6, 0);
    m.title = label(m.text_box, &font_baslik, COL_TEXT, "");
    lv_label_set_long_mode(m.title, LV_LABEL_LONG_DOT);
    lv_obj_set_width(m.title, 520);
    m.artist = label(m.text_box, &font_govde_b, 0xDDDEE3, "");
    lv_label_set_long_mode(m.artist, LV_LABEL_LONG_DOT);
    lv_obj_set_width(m.artist, 520);
    rise_in(hero, 40);

    /* İlerleme çubuğu */
    lv_obj_t *prog = plain(scr);
    lv_obj_set_pos(prog, PROG_X, PROG_Y);      /* başlık kartından ayrık: kart 96..248, çubuk 290 (= PROG_Y + BAR_Y) */
    lv_obj_set_size(prog, BAR_W + 2 * BAR_X, PROG_H);
    box(prog, BAR_X, BAR_Y, BAR_W, 10, 5, COL_TRACK);
    m.fill = box(prog, BAR_X, BAR_Y, 10, 10, 5, t->accent);
    m.knob = box(prog, 0, 0, KNOB_D, KNOB_D, LV_RADIUS_CIRCLE, 0xFFFFFF);
    m.grab = plain(prog);                       /* görünmez tutma alanı: topu parmakla yakalamak kolay olsun */
    lv_obj_set_size(m.grab, GRAB_W, GRAB_H);
    lv_obj_add_flag(m.grab, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(m.grab, grab_cb, LV_EVENT_ALL, NULL);
    bar_set(0);
    m.shown_w = -1;
    m.t_elapsed = label(prog, &font_etiket, COL_DIM, "0:00");
    lv_obj_set_pos(m.t_elapsed, BAR_X, BAR_Y + 28);
    m.t_dur = label(prog, &font_etiket, COL_DIM, "0:00");
    lv_obj_align(m.t_dur, LV_ALIGN_TOP_RIGHT, -BAR_X, BAR_Y + 28);
    rise_in(prog, 120);

    /* Denetimler: -10 sn, önceki, oynat/duraklat, sonraki, +10 sn */
    lv_obj_t *ctl = plain(scr);
    lv_obj_set_pos(ctl, 0, 344);
    lv_obj_set_size(ctl, 800, 104);
    seek_btn(ctl, 156, 20, -10000);
    lv_obj_t *pv = circle_btn(ctl, 244, 12, 80, COL_SURFACE, prev_cb, NULL);
    lv_obj_center(icon(pv, &font_ikon36, COL_TEXT, ICON_PREV));
    m.play_btn = circle_btn(ctl, 348, 0, 104, t->accent, play_cb, NULL);
    m.play_icon = plain(m.play_btn);
    lv_obj_set_size(m.play_icon, 46, 46);
    lv_obj_center(m.play_icon);
    lv_obj_center(icon(m.play_icon, &font_ikon46, COL_BG, ICON_PAUSE));
    lv_obj_t *nx = circle_btn(ctl, 476, 12, 80, COL_SURFACE, next_cb, NULL);
    lv_obj_center(icon(nx, &font_ikon36, COL_TEXT, ICON_NEXT));
    seek_btn(ctl, 580, 20, 10000);

    /* Beğen (solda) + Karıştır/Tekrarla (sağda), aynı sırada: SMTC bunları desteklemiyor, YouTube Music'e
     * tarayıcı eklentisiyle iletilir (bkz. ExtBridge, pc-helper/chrome-eklenti) */
    lv_obj_t *lk = circle_btn(ctl, 46, 20, 64, COL_SURFACE, quick_cmd_cb, (void *)"like");
    lv_obj_center(icon(lk, &font_ikon26, COL_TEXT, ICON_BOLT));
    lv_obj_t *sh = circle_btn(ctl, 660, 24, 56, COL_SURFACE, quick_cmd_cb, (void *)"shuffle");
    lv_obj_center(icon(sh, &font_ikon26, COL_TEXT, ICON_SYNC));
    lv_obj_t *rp = circle_btn(ctl, 728, 24, 56, COL_SURFACE, quick_cmd_cb, (void *)"repeat");
    lv_obj_center(icon(rp, &font_ikon26, COL_TEXT, ICON_REDO));
    rise_in(ctl, 180);

    /* Sürüklerken topun üstünde görünen süre baloncuğu (parmak örtmesin; başlık kartı ile top arasındaki boşluğa sığar) */
    m.bubble = box(scr, 0, 250, BUBBLE_W, 28, 14, t->accent);
    lv_obj_add_flag(m.bubble, LV_OBJ_FLAG_HIDDEN);
    m.bubble_lbl = label(m.bubble, &font_etiket, COL_BG, "0:00");
    lv_obj_center(m.bubble_lbl);

    s_scr_timer = lv_timer_create(main_tick, 100, NULL);
    main_tick(s_scr_timer);
    return scr;
}

/* ========================================================================== */
/* 2) Ayarlar                                                                  */
/* ========================================================================== */

static struct { lv_obj_t *val, *pill, *seg_lbl[4]; } st;

static void bri_cb(lv_event_t *e)
{
    lv_obj_t *sl = lv_event_get_target(e);
    int v = lv_slider_get_value(sl);
    char b[8];
    snprintf(b, sizeof(b), "%d%%", v);
    lv_label_set_text(st.val, b);
    g_settings.brightness = (uint8_t)v;
    board_backlight_percent(v);
    if (lv_event_get_code(e) == LV_EVENT_RELEASED) settings_save();
}

static void seg_apply(int idx)
{
    for (int i = 0; i < 4; i++) {
        lv_obj_set_style_text_color(st.seg_lbl[i], hex(i == idx ? COL_BG : COL_SUB), 0);
    }
}
static void seg_cb(lv_event_t *e)
{
    int idx = (int)(intptr_t)lv_event_get_user_data(e);
    if (idx == g_settings.dim_idx) return;
    g_settings.dim_idx = (uint8_t)idx;
    settings_save();
    seg_apply(idx);
    anim_run(st.pill, a_x, lv_obj_get_x(st.pill), 4 + idx * 168, 320, 0, lv_anim_path_ease_out);
}

static lv_obj_t *build_settings(void)
{
    memset(&st, 0, sizeof(st));
    const theme_t *t = th();
    lv_obj_t *scr = new_screen();
    page_bg(scr);
    header_back(scr, TR_SETTINGS);

    /* Tema kısayolu (sağ üst) */
    lv_obj_t *pill = circle_btn(scr, 568, 24, 56, COL_SURFACE, theme_open_cb, NULL);
    lv_obj_set_size(pill, 200, 56);
    lv_obj_set_style_radius(pill, 28, 0);
    lv_obj_set_style_transform_pivot_x(pill, 100, 0);
    lv_obj_t *dot = plain(pill);
    lv_obj_set_size(dot, 26, 26);
    lv_obj_set_pos(dot, 16, 15);
    lv_obj_set_style_radius(dot, LV_RADIUS_CIRCLE, 0);
    hero_grad(dot, th_selected());
    lv_obj_t *nm = label(pill, &font_etiket, COL_TEXT, th_selected()->name);
    lv_obj_align(nm, LV_ALIGN_LEFT_MID, 54, 0);
    lv_obj_t *chev = icon(pill, &font_ikon26, COL_DIM, ICON_RIGHT);
    lv_obj_align(chev, LV_ALIGN_RIGHT_MID, -14, 0);

    /* Parlaklık kartı */
    lv_obj_t *c1 = box(scr, 32, 104, 736, 154, 28, COL_CARD);
    lv_obj_t *sun = icon(c1, &font_ikon30, COL_SUB, ICON_SUN);
    lv_obj_align(sun, LV_ALIGN_TOP_LEFT, 28, 24);
    lv_obj_t *n1 = label(c1, &font_govde_b, COL_TEXT, TR_BRIGHTNESS);
    lv_obj_align(n1, LV_ALIGN_TOP_LEFT, 74, 28);
    char b[8];
    snprintf(b, sizeof(b), "%d%%", g_settings.brightness);
    st.val = label(c1, &font_h1, COL_TEXT, b);
    lv_obj_align(st.val, LV_ALIGN_TOP_RIGHT, -28, 20);
    lv_obj_t *sl = lv_slider_create(c1);
    lv_obj_set_size(sl, 680, 20);
    lv_obj_set_pos(sl, 28, 92);
    lv_slider_set_range(sl, 10, 100);
    lv_slider_set_value(sl, g_settings.brightness, LV_ANIM_OFF);
    lv_obj_set_style_bg_color(sl, hex(COL_TRACK), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(sl, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_radius(sl, 10, LV_PART_MAIN);
    lv_obj_set_style_bg_color(sl, hex(t->accent), LV_PART_INDICATOR);
    lv_obj_set_style_bg_opa(sl, LV_OPA_COVER, LV_PART_INDICATOR);
    lv_obj_set_style_radius(sl, 10, LV_PART_INDICATOR);
    lv_obj_set_style_bg_color(sl, hex(COL_TEXT), LV_PART_KNOB);
    lv_obj_set_style_bg_opa(sl, LV_OPA_COVER, LV_PART_KNOB);
    lv_obj_set_style_radius(sl, LV_RADIUS_CIRCLE, LV_PART_KNOB);
    lv_obj_set_style_pad_all(sl, 14, LV_PART_KNOB);
    lv_obj_add_event_cb(sl, bri_cb, LV_EVENT_VALUE_CHANGED, NULL);
    lv_obj_add_event_cb(sl, bri_cb, LV_EVENT_RELEASED, NULL);
    rise_in(c1, 60);

    /* Boşta karartma kartı */
    lv_obj_t *c2 = box(scr, 32, 274, 736, 156, 28, COL_CARD);
    lv_obj_t *moon = icon(c2, &font_ikon30, COL_SUB, ICON_MOON);
    lv_obj_align(moon, LV_ALIGN_TOP_LEFT, 28, 24);
    lv_obj_t *n2 = label(c2, &font_govde_b, COL_TEXT, TR_IDLE_DIM);
    lv_obj_align(n2, LV_ALIGN_TOP_LEFT, 74, 26);
    lv_obj_t *hint = label(c2, &font_kucuk, COL_DIM, TR_WAKE_TOUCH);
    lv_obj_align(hint, LV_ALIGN_TOP_RIGHT, -28, 28);
    lv_obj_t *seg = box(c2, 28, 72, 680, 64, 20, COL_SURFACE);
    st.pill = box(seg, 4 + g_settings.dim_idx * 168, 4, 168, 56, 16, t->accent);
    static const char *names[4] = { TR_DIM_30S, TR_DIM_2M, TR_DIM_10M, TR_DIM_OFF };
    for (int i = 0; i < 4; i++) {
        lv_obj_t *cell = plain(seg);
        lv_obj_set_pos(cell, 4 + i * 168, 4);
        lv_obj_set_size(cell, 168, 56);
        lv_obj_add_flag(cell, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_add_event_cb(cell, seg_cb, LV_EVENT_CLICKED, (void *)(intptr_t)i);
        st.seg_lbl[i] = label(cell, &font_etiket, COL_SUB, names[i]);
        lv_obj_center(st.seg_lbl[i]);
    }
    seg_apply(g_settings.dim_idx);
    rise_in(c2, 120);
    return scr;
}

/* ========================================================================== */
/* 3) Tema seçimi                                                              */
/* ========================================================================== */

static void tile_cb(lv_event_t *e)
{
    int idx = (int)(intptr_t)lv_event_get_user_data(e);
    if (idx == g_settings.theme) return;
    g_settings.theme = (uint8_t)idx;
    settings_save();
    /* Tema anında uygulanır: ekran yeni renklerle yeniden kurulup 450 ms'de çapraz solar */
    ui_show(SCR_THEME, LV_SCR_LOAD_ANIM_FADE_IN, 450);
}

static void cover_sw_cb(lv_event_t *e)
{
    g_settings.cover_color = lv_obj_has_state(lv_event_get_target(e), LV_STATE_CHECKED) ? 1 : 0;
    settings_save();
    dyn_refresh();                                   /* renkler hemen uygulanır */
    ui_show(SCR_THEME, LV_SCR_LOAD_ANIM_FADE_IN, 450);
}

static lv_obj_t *build_theme(void)
{
    const theme_t *t = th();
    lv_obj_t *scr = new_screen();
    page_bg(scr);
    header_back(scr, TR_THEME);
    lv_obj_t *cur = label(scr, &font_govde, t->accent, th_selected()->name);
    lv_obj_align(cur, LV_ALIGN_TOP_RIGHT, -32, 24 + 15);

    /* Canlı önizleme (sol) */
    lv_obj_t *pv = box(scr, 32, 104, 328, 344, 28, COL_CARD);
    lv_obj_t *hero = plain(pv);
    lv_obj_set_pos(hero, 22, 22);
    lv_obj_set_size(hero, 284, 104);
    lv_obj_set_style_radius(hero, 22, 0);
    lv_obj_set_style_clip_corner(hero, true, 0);
    hero_grad(hero, t);
    hero_rings(hero, 284, 104, 0.6f);
    lv_obj_t *pt = label(hero, &font_govde_b, COL_TEXT, "Şarkı Adı");
    lv_obj_set_pos(pt, 20, 26);
    lv_obj_t *pa = label(hero, &font_kucuk, 0xDDDEE3, "Sanatçı Adı");
    lv_obj_set_pos(pa, 20, 60);
    box(pv, 22, 176, 284, 6, 3, COL_TRACK);
    box(pv, 22, 176, 105, 6, 3, t->accent);
    box(pv, 22 + 105 - 8, 171, 16, 16, LV_RADIUS_CIRCLE, COL_TEXT);
    lv_obj_t *pb = box(pv, 22, 232, 52, 52, LV_RADIUS_CIRCLE, COL_SURFACE);
    lv_obj_center(icon(pb, &font_ikon26, COL_TEXT, ICON_PREV));
    lv_obj_t *pp = box(pv, 22 + 284 / 2 - 36, 222, 72, 72, LV_RADIUS_CIRCLE, t->accent);
    lv_obj_center(icon(pp, &font_ikon34, COL_BG, ICON_PAUSE));
    lv_obj_t *pn = box(pv, 22 + 284 - 52, 232, 52, 52, LV_RADIUS_CIRCLE, COL_SURFACE);
    lv_obj_center(icon(pn, &font_ikon26, COL_TEXT, ICON_NEXT));
    rise_in(pv, 60);

    /* Tema kareleri (sağ): 4 + 3 */
    for (int i = 0; i < THEME_COUNT; i++) {
        int col = i % 4, row = i / 4;
        int x = 384 + col * 99, y = 104 + row * 128;
        lv_obj_t *tile = circle_btn(scr, x, y, 87, COL_CARD, tile_cb, (void *)(intptr_t)i);
        lv_obj_set_style_radius(tile, 27, 0);
        lv_draw_buf_t *snap = theme_tile_snapshot(i);
        if (snap) {
            lv_obj_t *img = lv_image_create(tile);
            lv_image_set_src(img, snap);
            lv_obj_set_pos(img, 0, 0);
            lv_obj_remove_flag(img, LV_OBJ_FLAG_CLICKABLE);
        } else {
            hero_grad(tile, &THEMES[i]);      /* önbellekleme başarısız olursa (bellek yetersiz vb.) canlı degradeye düş */
        }
        bool on = (i == g_settings.theme);
        if (on) {
            lv_obj_set_style_border_width(tile, 3, 0);
            lv_obj_set_style_border_color(tile, hex(COL_TEXT), 0);
            lv_obj_set_style_border_opa(tile, LV_OPA_COVER, 0);
            lv_obj_t *ck = box(tile, 26, 26, 34, 34, LV_RADIUS_CIRCLE, COL_TEXT);
            lv_obj_remove_flag(ck, LV_OBJ_FLAG_CLICKABLE);
            lv_obj_center(icon(ck, &font_ikon26, COL_BG, ICON_CHECK));
            pop_in(ck, 34, 34);
            lv_obj_set_style_transform_scale_x(tile, 271, 0);
            lv_obj_set_style_transform_scale_y(tile, 271, 0);
        }
        lv_obj_t *nm = label(scr, &font_kucuk, on ? COL_TEXT : COL_SUB, THEMES[i].name);
        lv_obj_set_style_text_align(nm, LV_TEXT_ALIGN_CENTER, 0);
        lv_obj_set_width(nm, 87);
        lv_obj_set_pos(nm, x, y + 96);
        rise_in(tile, 100 + i * 45);
    }
    lv_obj_t *hint = label(scr, &font_kucuk, COL_DIM, TR_THEME_HINT);
    lv_obj_set_pos(hint, 384, 356);

    /* Kapak rengi anahtarı: açıkken vurgu rengi çalan şarkının kapağından gelir; kapalıyken seçilen tema geçerli */
    lv_obj_t *cc = box(scr, 384, 384, 384, 64, 24, COL_CARD);
    lv_obj_set_pos(label(cc, &font_govde_b, COL_TEXT, TR_COVER_COLOR), 22, 4);
    lv_obj_set_pos(label(cc, &font_kucuk, COL_DIM, TR_COVER_COLOR_HINT), 22, 36);
    lv_obj_t *sw = lv_switch_create(cc);
    lv_obj_set_size(sw, 64, 34);
    lv_obj_align(sw, LV_ALIGN_RIGHT_MID, -20, 0);
    lv_obj_set_style_bg_color(sw, hex(COL_TRACK), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(sw, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_bg_color(sw, hex(t->accent), LV_PART_INDICATOR | LV_STATE_CHECKED);
    lv_obj_set_style_bg_opa(sw, LV_OPA_COVER, LV_PART_INDICATOR | LV_STATE_CHECKED);
    lv_obj_set_style_bg_color(sw, hex(COL_TEXT), LV_PART_KNOB);
    lv_obj_set_style_bg_opa(sw, LV_OPA_COVER, LV_PART_KNOB);
    if (g_settings.cover_color) lv_obj_add_state(sw, LV_STATE_CHECKED);
    lv_obj_add_event_cb(sw, cover_sw_cb, LV_EVENT_VALUE_CHANGED, NULL);
    rise_in(cc, 420);
    return scr;
}

/* ========================================================================== */
/* 4) Boşta (karartılmış)                                                      */
/* ========================================================================== */

/* MGM hadise kodları ve adları: mgm.gov.tr'nin kendi sayfa betiğindeki (ililceler.js, convertHadise) tablo */
typedef struct { const char *code, *name, *day_icon, *night_icon; uint32_t color; } wx_t;
#define WX_SUN 0xFFC857
#define WX_MOON 0xC7D2FE
#define WX_CLOUD 0xB6BCC9
#define WX_RAIN 0x6CB4FF
#define WX_SNOW 0xDCEBFF
#define WX_FOG 0x9AA3B2
#define WX_WIND 0x8ED1C6
static const wx_t WX[] = {
    { "A", "Açık", ICON_SUN, ICON_MOON, WX_SUN },
    { "AB", "Az Bulutlu", ICON_CLOUD_SUN, ICON_CLOUD_MOON, WX_SUN },
    { "PB", "Parçalı Bulutlu", ICON_CLOUD_SUN, ICON_CLOUD_MOON, WX_SUN },
    { "CB", "Çok Bulutlu", ICON_CLOUD, ICON_CLOUD, WX_CLOUD },
    { "HY", "Hafif Yağmurlu", ICON_CLOUD_RAIN, ICON_CLOUD_RAIN, WX_RAIN },
    { "Y", "Yağmurlu", ICON_CLOUD_RAIN, ICON_CLOUD_RAIN, WX_RAIN },
    { "KY", "Kuvvetli Yağmurlu", ICON_CLOUD_HEAVY, ICON_CLOUD_HEAVY, WX_RAIN },
    { "HHY", "Yağışlı", ICON_CLOUD_RAIN, ICON_CLOUD_RAIN, WX_RAIN },
    { "KKY", "Karla Karışık Yağmurlu", ICON_CLOUD_HAIL, ICON_CLOUD_HAIL, WX_SNOW },
    { "HKY", "Hafif Kar Yağışlı", ICON_SNOW, ICON_SNOW, WX_SNOW },
    { "K", "Kar Yağışlı", ICON_SNOW, ICON_SNOW, WX_SNOW },
    { "KYK", "Yoğun Kar Yağışlı", ICON_SNOW, ICON_SNOW, WX_SNOW },
    { "YKY", "Yoğun Kar Yağışlı", ICON_SNOW, ICON_SNOW, WX_SNOW },
    { "HSY", "Hafif Sağanak Yağışlı", ICON_CLOUD_SUN_RAIN, ICON_CLOUD_MOON_RAIN, WX_RAIN },
    { "SY", "Sağanak Yağışlı", ICON_CLOUD_HEAVY, ICON_CLOUD_HEAVY, WX_RAIN },
    { "KSY", "Kuvvetli Sağanak Yağışlı", ICON_CLOUD_HEAVY, ICON_CLOUD_HEAVY, WX_RAIN },
    { "MSY", "Mevzi Sağanak Yağışlı", ICON_CLOUD_SUN_RAIN, ICON_CLOUD_MOON_RAIN, WX_RAIN },
    { "DY", "Dolu", ICON_CLOUD_HAIL, ICON_CLOUD_HAIL, WX_SNOW },
    { "GSY", "Gökgürültülü Sağanak Yağışlı", ICON_BOLT, ICON_BOLT, WX_SUN },
    { "KGY", "Kuvvetli Gökgürültülü Sağanak Yağışlı", ICON_BOLT, ICON_BOLT, WX_SUN },
    { "KGSY", "Kuvvetli Gökgürültülü Sağanak Yağışlı", ICON_BOLT, ICON_BOLT, WX_SUN },
    { "SIS", "Sisli", ICON_SMOG, ICON_SMOG, WX_FOG },
    { "PUS", "Puslu", ICON_SMOG, ICON_SMOG, WX_FOG },
    { "DNM", "Dumanlı", ICON_SMOG, ICON_SMOG, WX_FOG },
    { "KF", "Toz veya Kum Fırtınası", ICON_WIND, ICON_WIND, WX_WIND },
    { "R", "Rüzgarlı", ICON_WIND, ICON_WIND, WX_WIND },
    { "GKR", "Güneyli Kuvvetli Rüzgar", ICON_WIND, ICON_WIND, WX_WIND },
    { "KKR", "Kuzeyli Kuvvetli Rüzgar", ICON_WIND, ICON_WIND, WX_WIND },
    { "SCK", "Sıcak", ICON_HOT, ICON_HOT, 0xFF8A5B },
    { "SGK", "Soğuk", ICON_COLD, ICON_COLD, 0x7DC4FF },
};

static const wx_t *wx_find(const char *code)
{
    for (size_t i = 0; i < sizeof(WX) / sizeof(WX[0]); i++) {
        if (!strcmp(WX[i].code, code)) return &WX[i];
    }
    return NULL;
}

static const char *const TR_DAYS[7] = { "Pazartesi", "Salı", "Çarşamba", "Perşembe", "Cuma", "Cumartesi", "Pazar" };
static const char *const TR_MONTHS[12] = { "Ocak", "Şubat", "Mart", "Nisan", "Mayıs", "Haziran",
                                           "Temmuz", "Ağustos", "Eylül", "Ekim", "Kasım", "Aralık" };

static struct {
    lv_obj_t *dig[4], *colon, *track, *fill, *date, *wx_card, *wx_icon, *wx_temp, *wx_desc, *wx_detail, *wx_place, *wx_hi, *wx_lo;
    lv_obj_t *tm_chip, *tm_chip_lbl;
    eq_t eq;
    char shown_track[2 * PROTO_TEXT_MAX + 8];
    char shown_wx[80];
    int shown_hhmm, shown_day, shown_chip_s;
    int32_t shown_w;
} idl;

static void wake_cb(lv_event_t *e)
{
    board_backlight_percent(g_settings.brightness);
    ui_show(SCR_MAIN, LV_SCR_LOAD_ANIM_FADE_IN, 300);
}

static void a_colon(void *o, int32_t v) { lv_obj_set_style_opa(o, (lv_opa_t)v, 0); }

/* Sıcaklık, "°" ile ve eksi işaretiyle: -3 -> "-3°" */
static void fmt_deg(char *out, size_t n, int v)
{
    if (v == PROTO_NA) snprintf(out, n, "--");
    else snprintf(out, n, "%d°", v);
}

static void idle_weather_update(void)
{
    proto_weather_t w;
    bool ok = proto_weather_get(&w);
    char key[sizeof(idl.shown_wx)];
    snprintf(key, sizeof(key), "%d|%s|%d|%d|%d|%d|%d|%d", ok, w.code, w.temp, w.feels, w.hum, w.tmin, w.tmax, w.night);
    if (!strcmp(key, idl.shown_wx)) return;
    snprintf(idl.shown_wx, sizeof(idl.shown_wx), "%s", key);
    if (!ok) {                                   /* veri yok: kart gizlenir, düzen yeniden ortalanır */
        lv_obj_add_flag(idl.wx_card, LV_OBJ_FLAG_HIDDEN);
        return;
    }
    const wx_t *x = wx_find(w.code);
    lv_label_set_text(idl.wx_icon, x ? (w.night ? x->night_icon : x->day_icon) : ICON_CLOUD);
    lv_obj_set_style_text_color(idl.wx_icon, hex(x ? x->color : WX_CLOUD), 0);
    fit_lines(idl.wx_desc, x ? x->name : w.code, &font_govde_b, 270, 2);
    char b[80], d[16];
    fmt_deg(b, sizeof(b), w.temp);
    lv_label_set_text(idl.wx_temp, b);
    char feels[32] = "";
    if (w.feels != PROTO_NA) {
        fmt_deg(d, sizeof(d), w.feels);
        snprintf(feels, sizeof(feels), "%s %s", TR_FEELS_LIKE, d);
    }
    char hum[24] = "";
    if (w.hum != PROTO_NA) snprintf(hum, sizeof(hum), "%s %%%d", TR_HUMIDITY, w.hum);
    snprintf(b, sizeof(b), "%s%s%s", feels, (feels[0] && hum[0]) ? " · " : "", hum);
    lv_label_set_text(idl.wx_detail, b);
    lv_label_set_text(idl.wx_place, w.place);
    fmt_deg(d, sizeof(d), w.tmax);
    snprintf(b, sizeof(b), "%s %s", TR_TEMP_HIGH, d);
    lv_label_set_text(idl.wx_hi, b);
    fmt_deg(d, sizeof(d), w.tmin);
    snprintf(b, sizeof(b), "%s %s", TR_TEMP_LOW, d);
    lv_label_set_text(idl.wx_lo, b);
    lv_obj_remove_flag(idl.wx_card, LV_OBJ_FLAG_HIDDEN);
}

static void idle_tick(lv_timer_t *t)
{
    proto_state_t s;
    proto_get(&s);
    proto_datetime_t dt;
    bool have = proto_local_datetime(&dt);
    int hhmm = have ? dt.hh * 100 + dt.mm : -1;
    if (hhmm != idl.shown_hhmm) {
        idl.shown_hhmm = hhmm;
        char d[4][2] = { { '-', 0 }, { '-', 0 }, { '-', 0 }, { '-', 0 } };
        if (hhmm >= 0) {
            d[0][0] = '0' + dt.hh / 10;
            d[1][0] = '0' + dt.hh % 10;
            d[2][0] = '0' + dt.mm / 10;
            d[3][0] = '0' + dt.mm % 10;
        }
        for (int i = 0; i < 4; i++) lv_label_set_text(idl.dig[i], d[i]);
    }
    int day_key = have ? dt.year * 10000 + dt.month * 100 + dt.day : -1;
    if (day_key != idl.shown_day) {
        idl.shown_day = day_key;
        if (have) {
            char b[64];
            snprintf(b, sizeof(b), "%s, %d %s %d", TR_DAYS[dt.wday], dt.day, TR_MONTHS[dt.month - 1], dt.year);
            lv_label_set_text(idl.date, b);
            lv_obj_remove_flag(idl.date, LV_OBJ_FLAG_HIDDEN);
        } else {
            lv_obj_add_flag(idl.date, LV_OBJ_FLAG_HIDDEN);
        }
    }
    idle_weather_update();
    int chip_s = (tm.state == TM_RUNNING || tm.state == TM_PAUSED) ? (tm_left() + 999) / 1000 : -1;
    if (chip_s != idl.shown_chip_s) {
        idl.shown_chip_s = chip_s;
        if (chip_s < 0) {
            lv_obj_add_flag(idl.tm_chip, LV_OBJ_FLAG_HIDDEN);
        } else {
            char b[48], l[16];
            fmt_left(l, sizeof(l), chip_s * 1000);
            if (tm.mode == 1) snprintf(b, sizeof(b), "%s · %s", l, tm_phase_name());
            else snprintf(b, sizeof(b), "%s", l);
            lv_label_set_text(idl.tm_chip_lbl, b);
            lv_obj_remove_flag(idl.tm_chip, LV_OBJ_FLAG_HIDDEN);
        }
    }
    char line[sizeof(idl.shown_track)];
    if (s.state == 0) snprintf(line, sizeof(line), "%s", TR_NO_MEDIA_TITLE);
    else snprintf(line, sizeof(line), "%s · %s", s.title, s.artist);
    if (strcmp(line, idl.shown_track)) {
        snprintf(idl.shown_track, sizeof(idl.shown_track), "%s", line);
        lv_label_set_text(idl.track, line);
    }
    int32_t dur = s.state == 0 ? 0 : s.dur_ms;
    int32_t w = dur > 0 ? (int32_t)((int64_t)proto_position_ms(&s) * 800 / dur) : 0;
    if (w != idl.shown_w) {
        idl.shown_w = w;
        lv_obj_set_width(idl.fill, w < 1 ? 1 : w);
    }
}

static lv_obj_t *build_idle(void)
{
    memset(&idl, 0, sizeof(idl));
    idl.shown_hhmm = -2;
    idl.shown_day = -2;
    idl.shown_chip_s = -2;
    idl.shown_w = -1;
    const theme_t *t = th();
    lv_obj_t *scr = new_screen();
    lv_obj_set_style_bg_color(scr, hex(COL_IDLE_BG), 0);
    lv_obj_set_style_bg_opa(scr, LV_OPA_COVER, 0);

    /* Dikey akış: saat, tarih, hava durumu, çalan parça, zamanlayıcı; gizli öğeler yer kaplamaz, kalanlar ortalanır */
    lv_obj_t *col = plain(scr);
    lv_obj_set_size(col, 800, 440);
    lv_obj_set_pos(col, 0, 6);
    lv_obj_set_flex_flow(col, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(col, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_row(col, 10, 0);

    /* Saat: 4 rakam + iki nokta, sabit genişlikli hücreler (rakam genişliği oynamasın) */
    int cx[5] = { 194, 284, 374, 428, 518 };
    int cw[5] = { 90, 90, 52, 90, 90 };
    lv_obj_t *row = plain(col);
    lv_obj_set_size(row, 800, 160);
    for (int i = 0, d = 0; i < 5; i++) {
        lv_obj_t *cell = plain(row);
        lv_obj_set_size(cell, cw[i], 160);
        lv_obj_set_pos(cell, cx[i] - 6, 0);
        lv_obj_t *l = label(cell, &font_saat, 0xC9CCD6, i == 2 ? ":" : "-");
        lv_obj_center(l);
        if (i == 2) idl.colon = l; else idl.dig[d++] = l;
    }
    lv_anim_t a;                                  /* iki nokta yanıp söner */
    lv_anim_init(&a);
    lv_anim_set_var(&a, idl.colon);
    lv_anim_set_exec_cb(&a, a_colon);
    lv_anim_set_values(&a, 255, 40);
    lv_anim_set_duration(&a, 500);
    lv_anim_set_playback_duration(&a, 500);
    lv_anim_set_repeat_count(&a, LV_ANIM_REPEAT_INFINITE);
    lv_anim_set_path_cb(&a, lv_anim_path_step);
    lv_anim_start(&a);

    /* Tarih (Türkçe gün ve ay adları) */
    idl.date = label(col, &font_h1, 0x9EA3B0, "");
    lv_obj_add_flag(idl.date, LV_OBJ_FLAG_HIDDEN);

    /* Hava durumu (MGM): simge, sıcaklık, durum + hissedilen/nem, yer + en yüksek/en düşük */
    idl.wx_card = box(col, 0, 0, 640, 104, 28, 0x0E1015);
    lv_obj_add_flag(idl.wx_card, LV_OBJ_FLAG_HIDDEN);
    idl.wx_icon = icon(idl.wx_card, &font_ikon46, WX_CLOUD, ICON_CLOUD);
    lv_obj_align(idl.wx_icon, LV_ALIGN_LEFT_MID, 26, 0);
    lv_obj_set_width(idl.wx_icon, 56);
    lv_obj_set_style_text_align(idl.wx_icon, LV_TEXT_ALIGN_CENTER, 0);
    idl.wx_temp = label(idl.wx_card, &font_xl, 0xE6E8EE, "");
    lv_obj_align(idl.wx_temp, LV_ALIGN_LEFT_MID, 96, 0);
    lv_obj_t *txt = plain(idl.wx_card);
    lv_obj_set_pos(txt, 190, 0);
    lv_obj_set_size(txt, 270, 104);
    lv_obj_set_flex_flow(txt, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(txt, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START);
    lv_obj_set_style_pad_row(txt, 4, 0);
    idl.wx_desc = label(txt, &font_govde_b, 0xC9CCD6, "");
    lv_label_set_long_mode(idl.wx_desc, LV_LABEL_LONG_DOT);
    lv_obj_set_width(idl.wx_desc, 270);
    idl.wx_detail = label(txt, &font_kucuk, 0x7A8090, "");
    lv_obj_set_width(idl.wx_detail, 270);
    lv_label_set_long_mode(idl.wx_detail, LV_LABEL_LONG_DOT);
    lv_obj_t *rc = plain(idl.wx_card);
    lv_obj_set_pos(rc, 472, 0);
    lv_obj_set_size(rc, 152, 104);
    lv_obj_set_flex_flow(rc, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(rc, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_END, LV_FLEX_ALIGN_END);
    lv_obj_set_style_pad_row(rc, 3, 0);
    idl.wx_place = label(rc, &font_etiket, 0xC9CCD6, "");
    lv_obj_set_width(idl.wx_place, 152);
    lv_label_set_long_mode(idl.wx_place, LV_LABEL_LONG_DOT);
    lv_obj_set_style_text_align(idl.wx_place, LV_TEXT_ALIGN_RIGHT, 0);
    idl.wx_hi = label(rc, &font_kucuk, 0x7A8090, "");
    idl.wx_lo = label(rc, &font_kucuk, 0x7A8090, "");

    /* Çalan parça */
    lv_obj_t *line = plain(col);
    lv_obj_set_size(line, 700, 30);
    eq_create(&idl.eq, line, 0, 6, t->accent);
    eq_run(&idl.eq, false);   /* boşta çubuklar sönük durur (canlı animasyon yok) */
    idl.track = label(line, &font_govde, 0x7A8090, "");
    lv_label_set_long_mode(idl.track, LV_LABEL_LONG_DOT);
    lv_obj_set_width(idl.track, 640);
    lv_obj_set_height(idl.track, font_govde.line_height + 2);
    lv_obj_set_pos(idl.track, 34, 0);

    /* Çalışan zamanlayıcı */
    idl.tm_chip = box(col, 0, 0, 10, 40, 20, 0x0E1015);
    lv_obj_set_width(idl.tm_chip, LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(idl.tm_chip, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(idl.tm_chip, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_hor(idl.tm_chip, 18, 0);
    lv_obj_set_style_pad_column(idl.tm_chip, 10, 0);
    lv_obj_add_flag(idl.tm_chip, LV_OBJ_FLAG_HIDDEN);
    icon(idl.tm_chip, &font_ikon26, t->accent, ICON_STOPWATCH);
    idl.tm_chip_lbl = label(idl.tm_chip, &font_etiket, 0xC9CCD6, "");

    lv_obj_t *hint = label(scr, &font_kucuk, 0x7A8090, TR_TAP_TO_WAKE);
    lv_obj_set_width(hint, 800);
    lv_obj_set_style_text_align(hint, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_pos(hint, 0, 448);

    box(scr, 0, 477, 800, 3, 0, COL_CARD);
    idl.fill = box(scr, 0, 477, 1, 3, 0, t->accent);
    lv_obj_set_style_bg_opa(idl.fill, 180, 0);

    /* Tüm ekran: dokununca uyan */
    lv_obj_t *catcher = plain(scr);
    lv_obj_set_size(catcher, 800, 480);
    lv_obj_add_flag(catcher, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(catcher, wake_cb, LV_EVENT_CLICKED, NULL);

    s_scr_timer = lv_timer_create(idle_tick, 250, NULL);
    idle_tick(s_scr_timer);
    return scr;
}

/* ========================================================================== */
/* 5) Bağlantı yok                                                             */
/* ========================================================================== */

static struct { lv_obj_t *btn_lbl, *spin, *status; bool busy; } off;
static bool s_portal_wanted;                 /* kurulum ağı bu ekrandan açıldı (bilgisayar bağlanınca kapatılır) */
static uint32_t s_debug_hold_until;          /* tanılama: bu tick'e kadar bağlantı durumu ekranı değiştirmez ("go offline/install") */
static void install_open_cb(lv_event_t *e);

static void off_done(lv_timer_t *t)
{
    off.busy = false;
    if (s_cur != SCR_OFFLINE) return;
    lv_label_set_text(off.btn_lbl, TR_RETRY);
    lv_label_set_text(off.status, TR_WAIT_REPLY);
    lv_anim_delete(off.spin, a_rot);
    lv_obj_set_style_transform_rotation(off.spin, 0, 0);
}

static void retry_cb(lv_event_t *e)
{
    if (off.busy) return;
    off.busy = true;
    proto_send_hello();
    lv_label_set_text(off.btn_lbl, TR_TRYING);
    lv_label_set_text(off.status, TR_SEARCHING);
    lv_anim_t a;
    lv_anim_init(&a);
    lv_anim_set_var(&a, off.spin);
    lv_anim_set_exec_cb(&a, a_rot);
    lv_anim_set_values(&a, 0, 3600);
    lv_anim_set_duration(&a, 900);
    lv_anim_set_repeat_count(&a, LV_ANIM_REPEAT_INFINITE);
    lv_anim_set_path_cb(&a, lv_anim_path_linear);
    lv_anim_start(&a);
    lv_timer_t *t = lv_timer_create(off_done, 2400, NULL);
    lv_timer_set_repeat_count(t, 1);
}

static void a_ring_loop(void *o, int32_t v)
{
    lv_obj_set_style_transform_scale_x(o, v, 0);
    lv_obj_set_style_transform_scale_y(o, v, 0);
    lv_obj_set_style_border_opa(o, (lv_opa_t)(140 - (v - 256) * 140 / 234), 0);
}

static lv_obj_t *build_offline(void)
{
    memset(&off, 0, sizeof(off));
    const theme_t *t = th();
    lv_obj_t *scr = new_screen();
    page_bg(scr);

    lv_obj_t *top = plain(scr);
    lv_obj_set_size(top, 800, 220);
    lv_obj_set_pos(top, 0, 56);
    for (int i = 0; i < 2; i++) {          /* dışa yayılan iki halka */
        lv_obj_t *r = plain(top);
        lv_obj_set_size(r, 112, 112);
        lv_obj_set_pos(r, 344, 0);
        lv_obj_set_style_radius(r, LV_RADIUS_CIRCLE, 0);
        lv_obj_set_style_border_width(r, 2, 0);
        lv_obj_set_style_border_color(r, hex(t->accent), 0);
        lv_obj_set_style_transform_pivot_x(r, 56, 0);
        lv_obj_set_style_transform_pivot_y(r, 56, 0);
        lv_anim_t a;
        lv_anim_init(&a);
        lv_anim_set_var(&a, r);
        lv_anim_set_exec_cb(&a, a_ring_loop);
        lv_anim_set_values(&a, 256, 490);
        lv_anim_set_duration(&a, 2400);
        lv_anim_set_delay(&a, i * 1200);
        lv_anim_set_repeat_count(&a, LV_ANIM_REPEAT_INFINITE);
        lv_anim_set_path_cb(&a, lv_anim_path_ease_out);
        lv_anim_start(&a);
    }
    lv_obj_t *disc = box(top, 344, 0, 112, 112, LV_RADIUS_CIRCLE, COL_SURFACE);
    lv_obj_center(icon(disc, &font_ikon56, t->accent, ICON_PLUG));
    lv_obj_t *h = label(top, &font_xl, COL_TEXT, TR_WAITING_PC);
    lv_obj_set_width(h, 800);
    lv_obj_set_style_text_align(h, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_pos(h, 0, 140);
    lv_obj_t *p = label(scr, &font_govde, COL_SUB, TR_WAITING_HELP);
    lv_obj_set_width(p, 520);
    lv_obj_set_style_text_align(p, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_pos(p, 140, 240);
    rise_in(top, 0);

    /* Durum satırları */
    lv_obj_t *rows = plain(scr);
    lv_obj_set_pos(rows, 140, 300);
    lv_obj_set_size(rows, 520, 100);
    lv_obj_t *r1 = box(rows, 0, 0, 520, 44, 18, COL_CARD);
    box(r1, 22, 16, 12, 12, LV_RADIUS_CIRCLE, COL_GREEN);
    lv_obj_align(label(r1, &font_etiket, COL_TEXT, TR_PANEL), LV_ALIGN_LEFT_MID, 48, 0);
    lv_obj_align(label(r1, &font_kucuk, COL_DIM, TR_RUNNING), LV_ALIGN_RIGHT_MID, -22, 0);
    lv_obj_t *r2 = box(rows, 0, 54, 520, 44, 18, COL_CARD);
    box(r2, 22, 16, 12, 12, LV_RADIUS_CIRCLE, COL_AMBER);
    lv_obj_align(label(r2, &font_etiket, COL_TEXT, TR_PC_APP), LV_ALIGN_LEFT_MID, 48, 0);
    off.status = label(r2, &font_kucuk, COL_DIM, TR_WAIT_REPLY);
    lv_obj_align(off.status, LV_ALIGN_RIGHT_MID, -22, 0);
    rise_in(rows, 140);

    /* Yeniden dene + bu bilgisayara kur (panelin kendi Wi-Fi ağından kurulum dosyası) */
    lv_obj_t *btn = circle_btn(scr, 102, 412, 64, t->accent, retry_cb, NULL);
    lv_obj_set_size(btn, 220, 56);
    lv_obj_set_style_radius(btn, 28, 0);
    lv_obj_set_style_transform_pivot_x(btn, 110, 0);
    lv_obj_set_style_transform_pivot_y(btn, 28, 0);
    off.spin = icon(btn, &font_ikon26, COL_BG, ICON_SYNC);
    lv_obj_set_style_transform_pivot_x(off.spin, 13, 0);
    lv_obj_set_style_transform_pivot_y(off.spin, 14, 0);
    lv_obj_align(off.spin, LV_ALIGN_LEFT_MID, 24, 0);
    off.btn_lbl = label(btn, &font_govde, COL_BG, TR_RETRY);
    lv_obj_align(off.btn_lbl, LV_ALIGN_LEFT_MID, 64, 0);
    rise_in(btn, 220);
    lv_obj_t *ib = circle_btn(scr, 338, 412, 64, COL_SURFACE, install_open_cb, NULL);
    lv_obj_set_size(ib, 360, 56);
    lv_obj_set_style_radius(ib, 28, 0);
    lv_obj_set_style_transform_pivot_x(ib, 180, 0);
    lv_obj_set_style_transform_pivot_y(ib, 28, 0);
    lv_obj_align(icon(ib, &font_ikon26, t->accent, ICON_WIFI), LV_ALIGN_LEFT_MID, 24, 0);
    lv_obj_align(label(ib, &font_govde, COL_TEXT, TR_INSTALL_BTN), LV_ALIGN_LEFT_MID, 64, 0);
    rise_in(ib, 260);
    return scr;
}

/* ========================================================================== */
/* 6) Sistem izleme                                                            */
/* ========================================================================== */

static struct {
    lv_obj_t *arc[4], *val[4], *name[4], *l1[4], *host, *net[4];
    int shown_val[4];
    char c_name[4][48], c_l1[4][40], c_host[40], c_net[4][24];
} sy;

static void a_arc(void *o, int32_t v) { lv_arc_set_value(o, v); }

static void set_txt(lv_obj_t *l, char *cache, size_t cap, const char *txt)
{
    if (strncmp(cache, txt, cap - 1) == 0) return;
    size_t n = strlen(txt);
    if (n >= cap) n = cap - 1;
    memcpy(cache, txt, n);
    cache[n] = 0;
    lv_label_set_text(l, txt);
}

/* Model adı: en çok 2 satır (yükseklik metne göre ayarlanır; boşsa yer kaplamasın) */
static void set_name(lv_obj_t *l, char *cache, size_t cap, const char *txt)
{
    if (strncmp(cache, txt, cap - 1) == 0) return;
    size_t n = strlen(txt);
    if (n >= cap) n = cap - 1;
    memcpy(cache, txt, n);
    cache[n] = 0;
    if (!txt[0]) {
        lv_obj_set_height(l, 1);
        lv_label_set_text(l, "");
    } else {
        fit_lines(l, txt, &font_kucuk, 208, 2);
    }
}

/* Yuvarlak gösterge kartı: solda yüzde halkası, sağda başlık, model adı ve ayrıntı */
static void sys_card(lv_obj_t *scr, int i, int x, int y, const char *title, const char *glyph)
{
    lv_obj_t *c = box(scr, x, y, 360, 140, 28, COL_CARD);
    lv_obj_t *arc = lv_arc_create(c);
    lv_obj_set_size(arc, 100, 100);
    lv_obj_set_pos(arc, 20, 20);
    lv_arc_set_rotation(arc, 135);
    lv_arc_set_bg_angles(arc, 0, 270);
    lv_arc_set_range(arc, 0, 100);
    lv_arc_set_value(arc, 0);
    lv_obj_remove_style(arc, NULL, LV_PART_KNOB);
    lv_obj_remove_flag(arc, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_style_bg_opa(arc, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(arc, 0, 0);
    lv_obj_set_style_pad_all(arc, 0, 0);
    lv_obj_set_style_arc_width(arc, 11, LV_PART_MAIN);
    lv_obj_set_style_arc_width(arc, 11, LV_PART_INDICATOR);
    lv_obj_set_style_arc_color(arc, hex(COL_TRACK), LV_PART_MAIN);
    lv_obj_set_style_arc_color(arc, hex(th()->accent), LV_PART_INDICATOR);
    lv_obj_set_style_arc_rounded(arc, true, LV_PART_MAIN);
    lv_obj_set_style_arc_rounded(arc, true, LV_PART_INDICATOR);
    sy.arc[i] = arc;
    sy.val[i] = label(c, &font_h1, COL_TEXT, "—");
    lv_obj_set_width(sy.val[i], 100);
    lv_obj_set_style_text_align(sy.val[i], LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_pos(sy.val[i], 20, 20 + (100 - font_h1.line_height) / 2);
    lv_obj_t *ic = icon(c, &font_ikon26, th()->accent, glyph);
    lv_obj_set_pos(ic, 140, 16);
    lv_obj_t *tt = label(c, &font_govde_b, COL_TEXT, title);
    lv_obj_set_pos(tt, 176, 14);
    /* Model adı (en çok 2 satır) ve ayrıntı: dikey akışta alt alta, taşma yok */
    lv_obj_t *tb = plain(c);
    lv_obj_set_pos(tb, 140, 52);
    lv_obj_set_size(tb, 208, 84);
    lv_obj_set_flex_flow(tb, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(tb, 3, 0);
    sy.name[i] = label(tb, &font_kucuk, COL_DIM, "");
    sy.l1[i] = label(tb, &font_etiket, COL_TEXT, "");
    lv_obj_t *ls[2] = { sy.name[i], sy.l1[i] };
    const lv_font_t *fs[2] = { &font_kucuk, &font_etiket };
    for (int k = 0; k < 2; k++) {
        lv_obj_set_width(ls[k], 208);
        lv_label_set_long_mode(ls[k], LV_LABEL_LONG_DOT);
        lv_obj_set_height(ls[k], fs[k]->line_height);
    }
    sy.shown_val[i] = -1;
    rise_in(c, 60 + i * 60);
}

/* Alt şerit: indirme, yükleme, gecikme, oturum süresi */
static void sys_strip(lv_obj_t *scr)
{
    lv_obj_t *c = box(scr, 32, 396, 736, 60, 22, COL_CARD);
    static const char *cap[4] = { TR_DOWNLOAD, TR_UPLOAD, TR_LATENCY, TR_SESSION };
    static const char *ic[4] = { ICON_ARROW_DOWN, ICON_ARROW_UP, ICON_WIFI, ICON_HOURGLASS };
    for (int k = 0; k < 4; k++) {
        int x0 = k * 184;
        lv_obj_set_pos(icon(c, &font_ikon26, th()->accent, ic[k]), x0 + 20, 17);
        lv_obj_set_pos(label(c, &font_kucuk, COL_DIM, cap[k]), x0 + 58, 6);
        sy.net[k] = label(c, &font_etiket, COL_TEXT, "—");
        lv_obj_set_width(sy.net[k], 118);
        lv_label_set_long_mode(sy.net[k], LV_LABEL_LONG_DOT);
        lv_obj_set_height(sy.net[k], font_etiket.line_height);
        lv_obj_set_pos(sy.net[k], x0 + 58, 28);
    }
    rise_in(c, 300);
}

static void sys_set_value(int i, int v)
{
    if (v == sy.shown_val[i]) return;
    int from = sy.shown_val[i] < 0 ? 0 : sy.shown_val[i];
    sy.shown_val[i] = v;
    char b[8];
    if (v == PROTO_NA) snprintf(b, sizeof(b), "—");
    else snprintf(b, sizeof(b), "%d%%", v);
    lv_label_set_text(sy.val[i], b);
    int to = v == PROTO_NA ? 0 : v;
    lv_obj_set_style_arc_color(sy.arc[i], hex(to >= 85 ? COL_AMBER : th()->accent), LV_PART_INDICATOR);
    lv_anim_delete(sy.arc[i], a_arc);
    anim_run(sy.arc[i], a_arc, from, to, 400, 0, lv_anim_path_ease_out);
}

static void sys_tick(lv_timer_t *t)
{
    proto_sys_t p;
    bool ok = proto_sys_get(&p);
    bool gpu_ok = ok && p.gpu != PROTO_NA;
    char b[48], d[16], e[16];

    sys_set_value(0, ok ? p.cpu : PROTO_NA);
    sys_set_value(1, gpu_ok ? p.gpu : PROTO_NA);
    sys_set_value(2, ok ? p.ram : PROTO_NA);
    sys_set_value(3, gpu_ok && p.vram != PROTO_NA ? p.vram : PROTO_NA);

    set_txt(sy.host, sy.c_host, sizeof(sy.c_host), ok ? p.host : "");

    /* İşlemci: sıcaklık okunabiliyorsa o, değilse iş parçacığı sayısı */
    set_name(sy.name[0], sy.c_name[0], sizeof(sy.c_name[0]), ok ? p.cpu_name : "");
    b[0] = 0;
    if (ok && p.cpu_temp != PROTO_NA) snprintf(b, sizeof(b), "%d °C", p.cpu_temp);
    else if (ok && p.threads > 0) snprintf(b, sizeof(b), "%d %s", p.threads, TR_THREADS);
    else if (!ok) snprintf(b, sizeof(b), "%s", TR_WAIT_DATA);
    set_txt(sy.l1[0], sy.c_l1[0], sizeof(sy.c_l1[0]), b);

    /* Ekran kartı: sıcaklık · güç */
    set_name(sy.name[1], sy.c_name[1], sizeof(sy.c_name[1]), gpu_ok ? p.gpu_name : (ok ? TR_NOT_FOUND : ""));
    b[0] = 0;
    if (gpu_ok && p.gpu_temp != PROTO_NA) snprintf(b, sizeof(b), "%d °C", p.gpu_temp);
    if (gpu_ok && p.gpu_power != PROTO_NA) {
        size_t l = strlen(b);
        snprintf(b + l, sizeof(b) - l, "%s%d W", l ? " · " : "", p.gpu_power);
    }
    set_txt(sy.l1[1], sy.c_l1[1], sizeof(sy.c_l1[1]), b);

    /* Bellek */
    set_name(sy.name[2], sy.c_name[2], sizeof(sy.c_name[2]), ok ? TR_USED_TOTAL : "");
    b[0] = 0;
    if (ok && p.ram_total_mb > 0) {
        fmt_gb(d, sizeof(d), p.ram_used_mb);
        fmt_gb(e, sizeof(e), p.ram_total_mb);
        snprintf(b, sizeof(b), "%s / %s GB", d, e);
    }
    set_txt(sy.l1[2], sy.c_l1[2], sizeof(sy.c_l1[2]), b);

    /* Video belleği */
    bool vram_ok = gpu_ok && p.vram_total_mb > 0;
    set_name(sy.name[3], sy.c_name[3], sizeof(sy.c_name[3]), vram_ok ? TR_USED_TOTAL : "");
    b[0] = 0;
    if (vram_ok) {
        fmt_gb(d, sizeof(d), p.vram_used_mb);
        fmt_gb(e, sizeof(e), p.vram_total_mb);
        snprintf(b, sizeof(b), "%s / %s GB", d, e);
    }
    set_txt(sy.l1[3], sy.c_l1[3], sizeof(sy.c_l1[3]), b);

    /* Alt şerit */
    fmt_rate(b, sizeof(b), ok ? p.net_down_kbps : PROTO_NA);
    set_txt(sy.net[0], sy.c_net[0], sizeof(sy.c_net[0]), b);
    fmt_rate(b, sizeof(b), ok ? p.net_up_kbps : PROTO_NA);
    set_txt(sy.net[1], sy.c_net[1], sizeof(sy.c_net[1]), b);
    if (ok && p.ping_ms != PROTO_NA && p.ping_ms >= 0) snprintf(b, sizeof(b), "%d ms", p.ping_ms);
    else snprintf(b, sizeof(b), "—");
    set_txt(sy.net[2], sy.c_net[2], sizeof(sy.c_net[2]), b);
    int sess = session_now();
    if (sess >= 0) fmt_dur(b, sizeof(b), sess);
    else snprintf(b, sizeof(b), "—");
    set_txt(sy.net[3], sy.c_net[3], sizeof(sy.c_net[3]), b);
}

static lv_obj_t *build_system(void)
{
    memset(&sy, 0, sizeof(sy));
    lv_obj_t *scr = new_screen();
    page_bg(scr);
    header_back(scr, TR_SYSTEM);
    sy.host = label(scr, &font_etiket, COL_DIM, "");
    lv_obj_set_width(sy.host, 320);
    lv_obj_set_style_text_align(sy.host, LV_TEXT_ALIGN_RIGHT, 0);
    lv_label_set_long_mode(sy.host, LV_LABEL_LONG_DOT);
    lv_obj_set_pos(sy.host, 448, 24 + (56 - font_etiket.line_height) / 2);

    sys_card(scr, 0, 32, 92, TR_CPU, ICON_CHIP);
    sys_card(scr, 1, 408, 92, TR_GPU, ICON_CUBE);
    sys_card(scr, 2, 32, 244, TR_RAM, ICON_MEMORY);
    sys_card(scr, 3, 408, 244, TR_VRAM, ICON_LAYERS);
    sys_strip(scr);

    s_scr_timer = lv_timer_create(sys_tick, 500, NULL);
    sys_tick(s_scr_timer);
    return scr;
}

/* ========================================================================== */
/* 7) Zamanlayıcı ve Pomodoro                                                  */
/* ========================================================================== */

static int s_tab;                   /* Zamanlayıcı ekranı sekmesi: 0 geri sayım, 1 Pomodoro (= tm.mode), 2 Mola hatırlatıcı */

static struct {
    lv_obj_t *arc, *time, *sub, *rp, *seg_pill, *seg_lbl[3], *count_lbl, *brk_next;
    int shown_key, shown_sec, shown_arc;
    bool first;                     /* ilk kurulumda bilgi kartı yükselerek belirir; sonraki durum değişimlerinde animasyon yok */
} tmr;

static const int32_t PRESETS[6] = { 5, 10, 15, 30, 45, 60 };

/* Yuvarlatılmış düğme: ortada simge + metin */
static lv_obj_t *pill_btn(lv_obj_t *parent, int x, int y, int w, int h, uint32_t bg, uint32_t fg, const char *glyph,
                          const char *text, lv_event_cb_t cb, void *ud)
{
    lv_obj_t *b = circle_btn(parent, x, y, h, bg, cb, ud);
    lv_obj_set_size(b, w, h);
    lv_obj_set_style_radius(b, h / 2, 0);
    lv_obj_set_style_transform_pivot_x(b, w / 2, 0);
    lv_obj_set_flex_flow(b, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(b, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(b, 12, 0);
    if (glyph) icon(b, &font_ikon30, fg, glyph);
    if (text) label(b, &font_govde_b, fg, text);
    return b;
}

static void timer_rebuild_soon(void)      /* sağ paneli bir sonraki döngüde yeniden kur (olay geri çağrısı içinde silme yok) */
{
    tmr.shown_key = -1;
    if (s_scr_timer) lv_timer_ready(s_scr_timer);
}

static void tmr_seg_cb(lv_event_t *e)
{
    int idx = (int)(intptr_t)lv_event_get_user_data(e);
    if (idx == s_tab) return;
    if (idx != 2) {
        if (tm.state != TM_IDLE && idx != tm.mode) return;      /* çalışan sayaç sekme değişince kaybolmasın */
        if (tm.mode != idx) {
            tm.mode = idx;
            tm_reset();
        }
    }
    s_tab = idx;
    anim_run(tmr.seg_pill, a_x, lv_obj_get_x(tmr.seg_pill), 4 + idx * 128, 300, 0, lv_anim_path_ease_out);
    timer_rebuild_soon();
}

static void tmr_preset_cb(lv_event_t *e)
{
    tm.count_min = (int32_t)(intptr_t)lv_event_get_user_data(e);
    tm_load();
    timer_rebuild_soon();
}

static void tmr_adj_cb(lv_event_t *e)
{
    int32_t d = (int32_t)(intptr_t)lv_event_get_user_data(e);
    int32_t v = tm.count_min + d;
    if (v < 1) v = 1;
    if (v > 99) v = 99;
    if (v == tm.count_min) return;
    tm.count_min = v;
    tm_load();
    timer_rebuild_soon();
}

static void tmr_start_cb(lv_event_t *e)
{
    static uint32_t last;
    if (!tap_ok(&last, 250)) return;
    if (tm.state == TM_RUNNING) tm_pause(); else tm_start();
    timer_rebuild_soon();
}

static void tmr_reset_cb(lv_event_t *e)
{
    static uint32_t last;
    if (!tap_ok(&last, 250)) return;
    tm_reset();
    timer_rebuild_soon();
}

static void tmr_skip_cb(lv_event_t *e)
{
    static uint32_t last;
    if (!tap_ok(&last, 250)) return;
    tm_next_phase();
    timer_rebuild_soon();
}

/* ----- Mola hatırlatıcı sekmesi ----- */
static void brk_chip_cb(lv_event_t *e)
{
    g_settings.break_idx = (uint8_t)(intptr_t)lv_event_get_user_data(e);
    settings_save();
    int sn = session_now();
    s_break_ref_s = sn > 0 ? sn : 0;              /* yeni aralık şimdiden başlasın */
    timer_rebuild_soon();
}

static void brk_took_cb(lv_event_t *e)
{
    static uint32_t last;
    if (!tap_ok(&last, 300)) return;
    int sn = session_now();
    s_break_ref_s = sn > 0 ? sn : 0;              /* mola verildi: aralık baştan sayılır */
    timer_rebuild_soon();
}

static bool break_is_hourly(void)
{
    return settings_break_hourly() && s_break_test_s <= 0;
}

static void break_next_text(char *out, size_t n)
{
    if (break_is_hourly()) {                         /* "21:00'de" */
        proto_datetime_t dt;
        if (!proto_local_datetime(&dt)) {
            snprintf(out, n, "—");
            return;
        }
        snprintf(out, n, "%02d:%02d'de", dt.mm < s_hour_minute ? dt.hh : (dt.hh + 1) % 24, s_hour_minute);
        return;
    }
    int iv = break_interval_s();
    if (iv <= 0) {
        snprintf(out, n, "%s", TR_OFF);
        return;
    }
    int sn = session_now();
    if (sn < 0) {
        snprintf(out, n, "—");
        return;
    }
    int left = iv - (sn - s_break_ref_s);
    if (left < 0) left = 0;
    if (left >= 60) snprintf(out, n, "%d dk sonra", (left + 59) / 60);
    else snprintf(out, n, "%d sn sonra", left);
}

static void break_build_rp(lv_obj_t *rp, const theme_t *t)
{
    static const int mins[6] = { 0, 0, 30, 45, 60, 90 };
    char b[40];
    lv_obj_t *card = box(rp, 0, 0, 368, 104, 28, COL_CARD);
    lv_obj_set_pos(label(card, &font_kucuk, COL_DIM, TR_NEXT_BREAK), 28, 18);
    tmr.brk_next = label(card, &font_xl, COL_TEXT, "");
    lv_obj_set_pos(tmr.brk_next, 28, 44);
    for (int i = 0; i < 6; i++) {
        bool on = g_settings.break_idx == i;
        if (i == 0) snprintf(b, sizeof(b), "%s", TR_OFF);
        else if (i == 1) snprintf(b, sizeof(b), "%s", TR_HOURLY);
        else snprintf(b, sizeof(b), "%d dk", mins[i]);
        lv_obj_t *c = circle_btn(rp, (i % 3) * 128, 120 + (i / 3) * 68, 56, on ? t->accent : COL_SURFACE, brk_chip_cb,
                                 (void *)(intptr_t)i);
        lv_obj_set_size(c, 112, 56);
        lv_obj_set_style_radius(c, 28, 0);
        lv_obj_set_style_transform_pivot_x(c, 56, 0);
        lv_obj_center(label(c, &font_etiket, on ? COL_BG : COL_TEXT, b));
    }
    pill_btn(rp, 0, 264, 368, 80, COL_SURFACE, COL_TEXT, ICON_COFFEE, TR_TOOK_BREAK, brk_took_cb, NULL);
    break_next_text(b, sizeof(b));
    lv_label_set_text(tmr.brk_next, b);
    if (tmr.first) rise_in(card, 0);
}

static void break_tick(void)
{
    int sn = session_now();
    int disp = sn < 0 ? 0 : sn;
    if (disp / 60 != tmr.shown_sec) {              /* "s:dd" biçiminde, dakikada bir */
        tmr.shown_sec = disp / 60;
        char b[16];
        snprintf(b, sizeof(b), "%d:%02d", disp / 3600, (disp / 60) % 60);
        lv_label_set_text(tmr.time, b);
    }
    int iv = break_interval_s();
    int av = 0;
    if (break_is_hourly()) {                         /* halka: içinde bulunulan saatin ilerleyişi */
        proto_datetime_t dt;
        if (proto_local_datetime(&dt)) av = (dt.mm * 60 + dt.ss) * 1000 / 3600;
    } else if (iv > 0 && sn >= 0) {
        int64_t el = sn - s_break_ref_s;
        av = el <= 0 ? 0 : (int)(el * 1000 / iv);
        if (av > 1000) av = 1000;
    }
    if (av != tmr.shown_arc) {
        tmr.shown_arc = av;
        lv_arc_set_value(tmr.arc, av);
    }
    if (tmr.brk_next) {
        char b[40];
        break_next_text(b, sizeof(b));
        if (strcmp(lv_label_get_text(tmr.brk_next), b)) lv_label_set_text(tmr.brk_next, b);
    }
}

/* Sağ panel: mod ve duruma göre yeniden kurulur */
static void timer_build_rp(void)
{
    lv_obj_clean(tmr.rp);
    tmr.count_lbl = NULL;
    const theme_t *t = th();
    lv_obj_t *rp = tmr.rp;
    char b[48];
    bool idle = tm.state == TM_IDLE;
    bool run = tm.state == TM_RUNNING;
    tmr.brk_next = NULL;

    if (s_tab == 2) {
        break_build_rp(rp, t);
        return;
    }

    if (tm.mode == 0 && idle) {
        /* Süre seçimi: - / + ve hazır süreler */
        lv_obj_t *mn = circle_btn(rp, 0, 0, 64, COL_SURFACE, tmr_adj_cb, (void *)(intptr_t)-1);
        lv_obj_add_event_cb(mn, tmr_adj_cb, LV_EVENT_LONG_PRESSED_REPEAT, (void *)(intptr_t)-1);
        lv_obj_center(icon(mn, &font_ikon30, COL_TEXT, ICON_MINUS));
        lv_obj_t *pl = circle_btn(rp, 304, 0, 64, COL_SURFACE, tmr_adj_cb, (void *)(intptr_t)1);
        lv_obj_add_event_cb(pl, tmr_adj_cb, LV_EVENT_LONG_PRESSED_REPEAT, (void *)(intptr_t)1);
        lv_obj_center(icon(pl, &font_ikon30, COL_TEXT, ICON_PLUS));
        snprintf(b, sizeof(b), "%d dk", (int)tm.count_min);
        tmr.count_lbl = label(rp, &font_xl, COL_TEXT, b);
        lv_obj_set_width(tmr.count_lbl, 224);
        lv_obj_set_style_text_align(tmr.count_lbl, LV_TEXT_ALIGN_CENTER, 0);
        lv_obj_set_pos(tmr.count_lbl, 72, (64 - font_xl.line_height) / 2);
        for (int i = 0; i < 6; i++) {
            bool on = tm.count_min == PRESETS[i];
            snprintf(b, sizeof(b), "%d dk", (int)PRESETS[i]);
            lv_obj_t *c = circle_btn(rp, (i % 3) * 128, 88 + (i / 3) * 72, 56, on ? t->accent : COL_SURFACE,
                                     tmr_preset_cb, (void *)(intptr_t)PRESETS[i]);
            lv_obj_set_size(c, 112, 56);
            lv_obj_set_style_radius(c, 28, 0);
            lv_obj_set_style_transform_pivot_x(c, 56, 0);
            lv_obj_center(label(c, &font_etiket, on ? COL_BG : COL_TEXT, b));
        }
        pill_btn(rp, 0, 256, 368, 88, t->accent, COL_BG, ICON_PLAY, TR_START, tmr_start_cb, NULL);
        return;
    }

    /* Bilgi kartı */
    lv_obj_t *card = box(rp, 0, 0, 368, 232, 28, COL_CARD);
    if (tm.mode == 0) {
        lv_obj_t *l = label(card, &font_kucuk, COL_DIM, TR_TOTAL_TIME);
        lv_obj_set_pos(l, 28, 28);
        snprintf(b, sizeof(b), "%d dk", (int)(tm.total_ms / 60000));
        lv_obj_set_pos(label(card, &font_xl, COL_TEXT, b), 28, 52);
        l = label(card, &font_kucuk, COL_DIM, TR_ENDS_AT);
        lv_obj_set_pos(l, 28, 128);
        proto_datetime_t dt;
        if (run && proto_local_datetime(&dt)) {
            int end = (dt.hh * 3600 + dt.mm * 60 + dt.ss + (tm_left() + 999) / 1000) % 86400;
            snprintf(b, sizeof(b), "%02d:%02d", end / 3600, (end / 60) % 60);
        } else {
            snprintf(b, sizeof(b), "%s", TR_NO_TIME);
        }
        lv_obj_set_pos(label(card, &font_xl, COL_TEXT, b), 28, 152);
    } else {
        bool focus = tm.phase == PH_FOCUS;
        lv_obj_t *ic = icon(card, &font_ikon34, focus ? t->accent : COL_GREEN, focus ? ICON_BRAIN : ICON_COFFEE);
        lv_obj_set_pos(ic, 28, 26);
        lv_obj_set_pos(label(card, &font_baslik, COL_TEXT, tm_phase_name()), 80, 22);
        snprintf(b, sizeof(b), "%s %d / 4", TR_ROUND_OF_4, tm.round);
        lv_obj_set_pos(label(card, &font_etiket, COL_SUB, b), 28, 86);
        int done = focus ? tm.round - 1 : tm.round;
        for (int i = 0; i < 4; i++) {
            lv_obj_t *dot = box(card, 28 + i * 44, 124, 28, 28, LV_RADIUS_CIRCLE, i < done ? t->accent : COL_SURFACE);
            if (focus && i == done) {
                lv_obj_set_style_border_width(dot, 3, 0);
                lv_obj_set_style_border_color(dot, hex(t->accent), 0);
                lv_obj_set_style_border_opa(dot, LV_OPA_COVER, 0);
            }
        }
        lv_obj_t *plan = label(card, &font_kucuk, COL_DIM, TR_POMO_PLAN);
        lv_obj_set_width(plan, 312);
        lv_obj_set_pos(plan, 28, 172);
    }
    if (tmr.first) rise_in(card, 0);

    /* Düğmeler */
    if (idle) {
        pill_btn(rp, 0, 256, 368, 88, t->accent, COL_BG, ICON_PLAY, TR_START, tmr_start_cb, NULL);
    } else if (tm.mode == 0) {
        pill_btn(rp, 0, 256, 264, 88, t->accent, COL_BG, run ? ICON_PAUSE : ICON_PLAY, run ? TR_PAUSE : TR_RESUME,
                 tmr_start_cb, NULL);
        lv_obj_t *r = circle_btn(rp, 280, 256, 88, COL_SURFACE, tmr_reset_cb, NULL);
        lv_obj_center(icon(r, &font_ikon34, COL_TEXT, ICON_UNDO));
    } else {
        pill_btn(rp, 0, 256, 176, 88, t->accent, COL_BG, run ? ICON_PAUSE : ICON_PLAY, run ? TR_PAUSE : TR_RESUME,
                 tmr_start_cb, NULL);
        lv_obj_t *sk = circle_btn(rp, 184, 256, 88, COL_SURFACE, tmr_skip_cb, NULL);
        lv_obj_center(icon(sk, &font_ikon34, COL_TEXT, ICON_NEXT));
        lv_obj_t *r = circle_btn(rp, 280, 256, 88, COL_SURFACE, tmr_reset_cb, NULL);
        lv_obj_center(icon(r, &font_ikon34, COL_TEXT, ICON_UNDO));
    }
}

static void timer_apply_ring(void)
{
    if (s_tab == 2) {                              /* mola sekmesi: oturum süresi, yeşil halka */
        lv_obj_set_style_arc_color(tmr.arc, hex(COL_GREEN), LV_PART_INDICATOR);
        lv_label_set_text(tmr.sub, TR_SESSION_TIME);
        return;
    }
    bool brk = tm.mode == 1 && tm.phase != PH_FOCUS;
    lv_obj_set_style_arc_color(tmr.arc, hex(brk ? COL_GREEN : th()->accent), LV_PART_INDICATOR);
    const char *sub;
    if (tm.state == TM_PAUSED) sub = TR_PAUSED;
    else if (tm.mode == 1) sub = tm_phase_name();
    else sub = tm.state == TM_RUNNING ? TR_RUNNING_NOW : TR_READY;
    lv_label_set_text(tmr.sub, sub);
}

static void timer_tick(lv_timer_t *t)
{
    int key = (int)tm.state | (tm.phase << 4) | (tm.round << 8) | (tm.mode << 12) | ((int)tm.count_min << 16) |
              (s_tab << 24) | ((int)g_settings.break_idx << 26);
    if (key != tmr.shown_key) {
        tmr.shown_key = key;
        timer_build_rp();
        timer_apply_ring();
        tmr.first = false;
        tmr.shown_sec = -1;
        tmr.shown_arc = -1;
        for (int i = 0; i < 3; i++) {          /* çalışan zamanlayıcının modu dışındaki zamanlayıcı sekmesi değiştirilemez: soluk göster */
            lv_obj_set_style_text_color(tmr.seg_lbl[i], hex(i == s_tab ? COL_BG : COL_SUB), 0);
            bool locked = i < 2 && tm.state != TM_IDLE && i != tm.mode;
            lv_obj_set_style_opa(lv_obj_get_parent(tmr.seg_lbl[i]), locked ? 110 : LV_OPA_COVER, 0);
        }
    }
    if (s_tab == 2) {
        break_tick();
        return;
    }
    int32_t left = tm_left();
    int sec = (left + 999) / 1000;
    if (sec != tmr.shown_sec) {
        tmr.shown_sec = sec;
        char b[16];
        fmt_left(b, sizeof(b), left);
        lv_label_set_text(tmr.time, b);
    }
    int av = tm.total_ms > 0 ? (int)((int64_t)left * 1000 / tm.total_ms) : 0;
    if (av != tmr.shown_arc) {
        tmr.shown_arc = av;
        lv_arc_set_value(tmr.arc, av);
    }
}

static lv_obj_t *build_timer(void)
{
    memset(&tmr, 0, sizeof(tmr));
    const theme_t *t = th();
    lv_obj_t *scr = new_screen();
    page_bg(scr);
    header_back(scr, TR_TIMER);

    /* Sekmeler: Geri sayım | Pomodoro | Mola */
    if (s_tab != 2) s_tab = tm.mode;
    lv_obj_t *seg = box(scr, 376, 24, 392, 56, 28, COL_SURFACE);
    tmr.seg_pill = box(seg, 4 + s_tab * 128, 4, 128, 48, 24, t->accent);
    static const char *names[3] = { TR_COUNTDOWN, TR_POMODORO, TR_BREAK };
    for (int i = 0; i < 3; i++) {
        lv_obj_t *cell = plain(seg);
        lv_obj_set_pos(cell, 4 + i * 128, 4);
        lv_obj_set_size(cell, 128, 48);
        lv_obj_add_flag(cell, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_add_event_cb(cell, tmr_seg_cb, LV_EVENT_CLICKED, (void *)(intptr_t)i);
        tmr.seg_lbl[i] = label(cell, &font_etiket, COL_SUB, names[i]);
        lv_obj_center(tmr.seg_lbl[i]);
    }

    /* Halka + kalan süre */
    tmr.arc = lv_arc_create(scr);
    lv_obj_set_size(tmr.arc, 300, 300);
    lv_obj_set_pos(tmr.arc, 48, 124);
    lv_arc_set_rotation(tmr.arc, 270);
    lv_arc_set_bg_angles(tmr.arc, 0, 360);
    lv_arc_set_range(tmr.arc, 0, 1000);
    lv_arc_set_value(tmr.arc, 1000);
    lv_obj_remove_style(tmr.arc, NULL, LV_PART_KNOB);
    lv_obj_remove_flag(tmr.arc, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_style_bg_opa(tmr.arc, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(tmr.arc, 0, 0);
    lv_obj_set_style_pad_all(tmr.arc, 0, 0);
    lv_obj_set_style_arc_width(tmr.arc, 18, LV_PART_MAIN);
    lv_obj_set_style_arc_width(tmr.arc, 18, LV_PART_INDICATOR);
    lv_obj_set_style_arc_color(tmr.arc, hex(COL_TRACK), LV_PART_MAIN);
    lv_obj_set_style_arc_color(tmr.arc, hex(t->accent), LV_PART_INDICATOR);
    lv_obj_set_style_arc_rounded(tmr.arc, true, LV_PART_MAIN);
    lv_obj_set_style_arc_rounded(tmr.arc, true, LV_PART_INDICATOR);
    tmr.time = label(scr, &font_sayac, COL_TEXT, "00:00");
    lv_obj_set_width(tmr.time, 300);
    lv_obj_set_style_text_align(tmr.time, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_align_to(tmr.time, tmr.arc, LV_ALIGN_CENTER, 0, -12);
    tmr.sub = label(scr, &font_govde_b, COL_SUB, "");
    lv_obj_set_width(tmr.sub, 300);
    lv_obj_set_style_text_align(tmr.sub, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_align_to(tmr.sub, tmr.arc, LV_ALIGN_CENTER, 0, 62);
    rise_in(tmr.arc, 40);

    tmr.rp = plain(scr);
    lv_obj_set_pos(tmr.rp, 400, 104);
    lv_obj_set_size(tmr.rp, 368, 344);

    tmr.shown_key = -1;
    tmr.first = true;
    s_scr_timer = lv_timer_create(timer_tick, 200, NULL);
    timer_tick(s_scr_timer);
    return scr;
}

/* ========================================================================== */
/* 8) Alarm (süre doldu)                                                       */
/* ========================================================================== */

static void alarm_dismiss(bool start_next)
{
    board_backlight_percent(g_settings.brightness);
    if (tm.mode == 1) {
        tm_next_phase();
        if (start_next) tm_start();
    } else {
        tm_load();
    }
    ui_show(SCR_MAIN, LV_SCR_LOAD_ANIM_FADE_IN, 300);
}

static void alarm_close_cb(lv_event_t *e) { alarm_dismiss(false); }
static void alarm_next_cb(lv_event_t *e) { alarm_dismiss(true); }

static void a_bell(void *o, int32_t v) { lv_obj_set_style_transform_rotation(o, v, 0); }

/* Alarm metinleri: hem alarm ekranında hem bilgisayara gönderilen bildirimde aynı (Türkçe) metin.
 * primary: sıradaki evreyi başlatan düğmenin yazısı (geri sayımda NULL). */
static void alarm_texts(const char **title, char *sub, size_t n, const char **primary)
{
    if (tm.mode == 1) {
        int next = tm.phase == PH_FOCUS ? (tm.round >= 4 ? PH_LONG : PH_SHORT) : PH_FOCUS;
        const char *next_name = next == PH_FOCUS ? TR_FOCUS : (next == PH_SHORT ? TR_SHORT_BREAK : TR_LONG_BREAK);
        *title = tm.phase == PH_FOCUS ? TR_FOCUS_DONE : TR_BREAK_DONE;
        *primary = next == PH_FOCUS ? TR_START_FOCUS : TR_START_BREAK;
        snprintf(sub, n, "%s: %s · %d dk", TR_NEXT, next_name, (int)tm_phase_min(next));
    } else {
        *title = TR_TIME_UP;
        *primary = NULL;
        snprintf(sub, n, "%d dk tamamlandı", (int)(tm.total_ms / 60000));
    }
}

static lv_obj_t *build_alarm(void)
{
    const theme_t *t = th();
    lv_obj_t *scr = new_screen();
    page_bg(scr);

    for (int i = 0; i < 2; i++) {                /* dışa yayılan halkalar */
        lv_obj_t *r = plain(scr);
        lv_obj_set_size(r, 128, 128);
        lv_obj_set_pos(r, 336, 44);
        lv_obj_set_style_radius(r, LV_RADIUS_CIRCLE, 0);
        lv_obj_set_style_border_width(r, 3, 0);
        lv_obj_set_style_border_color(r, hex(t->accent), 0);
        lv_obj_set_style_transform_pivot_x(r, 64, 0);
        lv_obj_set_style_transform_pivot_y(r, 64, 0);
        lv_anim_t a;
        lv_anim_init(&a);
        lv_anim_set_var(&a, r);
        lv_anim_set_exec_cb(&a, a_ring_loop);
        lv_anim_set_values(&a, 256, 490);
        lv_anim_set_duration(&a, 1600);
        lv_anim_set_delay(&a, i * 800);
        lv_anim_set_repeat_count(&a, LV_ANIM_REPEAT_INFINITE);
        lv_anim_set_path_cb(&a, lv_anim_path_ease_out);
        lv_anim_start(&a);
    }
    lv_obj_t *disc = box(scr, 336, 44, 128, 128, LV_RADIUS_CIRCLE, t->accent);
    lv_obj_t *bell = icon(disc, &font_ikon56, COL_BG, ICON_BELL);
    lv_obj_center(bell);
    lv_obj_set_style_transform_pivot_x(bell, 28, 0);
    lv_obj_set_style_transform_pivot_y(bell, 4, 0);
    lv_anim_t sw;                                 /* zil sallanır */
    lv_anim_init(&sw);
    lv_anim_set_var(&sw, bell);
    lv_anim_set_exec_cb(&sw, a_bell);
    lv_anim_set_values(&sw, -160, 160);
    lv_anim_set_duration(&sw, 260);
    lv_anim_set_playback_duration(&sw, 260);
    lv_anim_set_repeat_count(&sw, LV_ANIM_REPEAT_INFINITE);
    lv_anim_set_path_cb(&sw, lv_anim_path_ease_in_out);
    lv_anim_start(&sw);

    const char *title, *primary;
    char sub[64];
    alarm_texts(&title, sub, sizeof(sub), &primary);
    lv_obj_t *h = label(scr, &font_xl, COL_TEXT, title);
    lv_obj_set_width(h, 800);
    lv_obj_set_style_text_align(h, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_pos(h, 0, 204);
    lv_obj_t *s = label(scr, &font_govde, COL_SUB, sub);
    lv_obj_set_width(s, 800);
    lv_obj_set_style_text_align(s, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_pos(s, 0, 262);
    rise_in(h, 0);

    if (primary) {
        pill_btn(scr, 116, 340, 340, 80, t->accent, COL_BG, ICON_PLAY, primary, alarm_next_cb, NULL);
        pill_btn(scr, 472, 340, 212, 80, COL_SURFACE, COL_TEXT, NULL, TR_DISMISS, alarm_close_cb, NULL);
    } else {
        pill_btn(scr, 250, 340, 300, 80, t->accent, COL_BG, ICON_CHECK, TR_DISMISS, alarm_close_cb, NULL);
    }
    return scr;
}

/* ========================================================================== */
/* Ekran yönetimi                                                              */
/* ========================================================================== */

/* Kapak rengi: anahtar açık ve kapak varsa kapaktan üretilen renkler geçerli olur; kapak geçici yoksa (parça değişimi)
 * eski renkler 6 sn kalır ki her parça değişiminde ekran iki kez yeniden boyanmasın. Etkin renkler değiştiyse true. */
static uint32_t s_dyn_seq = 0xFFFFFFFFu;
static uint32_t s_dyn_lost_tick;
static bool dyn_refresh(void)
{
    bool was = s_dyn_active, changed = false;
    proto_cover_t cv;
    proto_cover_get(&cv);
    if (g_settings.cover_color && cv.valid) {
        s_dyn_lost_tick = 0;
        if (!was || cv.seq != s_dyn_seq) {
            s_dyn_seq = cv.seq;
            changed = theme_dyn_from_cover((const uint16_t *)cv.data, PROTO_COVER_W, PROTO_COVER_H) || !was;
        }
        s_dyn_active = true;
    } else if (was) {
        if (g_settings.cover_color) {
            if (!s_dyn_lost_tick) s_dyn_lost_tick = lv_tick_get() ? lv_tick_get() : 1;
            if (lv_tick_elaps(s_dyn_lost_tick) < 6000) return false;
        }
        s_dyn_active = false;
        s_dyn_lost_tick = 0;
        changed = true;
    }
    return changed;
}

/* ========================================================================== */
/* 9) Bu bilgisayara kur: panelin kendi Wi-Fi ağı + gömülü kurulum dosyası      */
/* ========================================================================== */

static struct {
    lv_obj_t *pin, *status, *qr;
    char c_pin[12], c_status[80], c_qr_pw[12];
} ins;

static void install_close_cb(lv_event_t *e)
{
    portal_request(false);
    s_portal_wanted = false;
    ui_show(SCR_OFFLINE, LV_SCR_LOAD_ANIM_MOVE_RIGHT, 300);
}

static void install_open_cb(lv_event_t *e)
{
    static uint32_t last;
    if (!tap_ok(&last, 600)) return;
    ui_show(SCR_INSTALL_CHOOSE, LV_SCR_LOAD_ANIM_MOVE_LEFT, 320);
}

static void install_choose_close_cb(lv_event_t *e)
{
    ui_show(SCR_OFFLINE, LV_SCR_LOAD_ANIM_MOVE_RIGHT, 300);
}

static void install_choose_wifi_cb(lv_event_t *e)
{
    static uint32_t last;
    if (!tap_ok(&last, 600)) return;
    portal_request(true);
    s_portal_wanted = true;
    ui_show(SCR_INSTALL, LV_SCR_LOAD_ANIM_MOVE_LEFT, 320);
}

static void install_choose_usb_cb(lv_event_t *e)
{
    static uint32_t last;
    if (!tap_ok(&last, 600)) return;
    ui_show(SCR_INSTALL_USB, LV_SCR_LOAD_ANIM_MOVE_LEFT, 320);
}

static lv_obj_t *build_install_choose(void)
{
    const theme_t *t = th();
    lv_obj_t *scr = new_screen();
    page_bg(scr);
    header_back_cb(scr, TR_INSTALL_CHOOSE_TITLE, install_choose_close_cb);

    struct { int x; const char *icon_glyph; const char *title; const char *sub; lv_event_cb_t cb; } opts[2] = {
        { 32, ICON_WIFI, TR_INSTALL_WIFI_TITLE, TR_INSTALL_WIFI_SUB, install_choose_wifi_cb },
        { 408, ICON_PLUG, TR_INSTALL_USB_TITLE, TR_INSTALL_USB_SUB, install_choose_usb_cb },
    };
    for (int i = 0; i < 2; i++) {
        lv_obj_t *c = box(scr, opts[i].x, 110, 360, 260, 26, COL_CARD);
        lv_obj_add_flag(c, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_add_style(c, &st_btn, 0);
        lv_obj_add_style(c, &st_press, LV_STATE_PRESSED);
        lv_obj_add_event_cb(c, opts[i].cb, LV_EVENT_CLICKED, NULL);
        lv_obj_t *bd = box(c, 148, 44, 64, 64, LV_RADIUS_CIRCLE, COL_SURFACE);
        lv_obj_center(icon(bd, &font_ikon26, t->accent, opts[i].icon_glyph));
        lv_obj_t *ti = label(c, &font_govde_b, COL_TEXT, opts[i].title);
        lv_obj_set_width(ti, 320);
        lv_obj_set_style_text_align(ti, LV_TEXT_ALIGN_CENTER, 0);
        lv_obj_set_pos(ti, 20, 140);
        lv_obj_t *su = label(c, &font_kucuk, COL_SUB, opts[i].sub);
        lv_obj_set_width(su, 300);
        lv_obj_set_style_text_align(su, LV_TEXT_ALIGN_CENTER, 0);
        lv_obj_set_pos(su, 30, 182);
        rise_in(c, 60 + 60 * i);
    }
    return scr;
}

static void install_tick(lv_timer_t *t)
{
    portal_status_t p;
    portal_get(&p);
    char b[80];
    set_txt(ins.pin, ins.c_pin, sizeof(ins.c_pin), p.state == PORTAL_ON && p.password[0] ? p.password : "········");
    if (p.state == PORTAL_ON) snprintf(b, sizeof(b), TR_INSTALL_READY, p.clients, p.downloads);
    else if (p.state == PORTAL_FAILED) snprintf(b, sizeof(b), "%s", TR_INSTALL_FAIL);
    else snprintf(b, sizeof(b), "%s", TR_INSTALL_STARTING);
    set_txt(ins.status, ins.c_status, sizeof(ins.c_status), b);

    if (p.state == PORTAL_ON && p.password[0] && strcmp(ins.c_qr_pw, p.password) != 0) {
        char payload[96];
        snprintf(payload, sizeof(payload), "WIFI:T:WPA;S:%s;P:%s;;", PORTAL_SSID, p.password);
        lv_qrcode_update(ins.qr, payload, strlen(payload));
        snprintf(ins.c_qr_pw, sizeof(ins.c_qr_pw), "%s", p.password);
    }
}

static lv_obj_t *build_install(void)
{
    memset(&ins, 0, sizeof(ins));
    const theme_t *t = th();
    lv_obj_t *scr = new_screen();
    page_bg(scr);
    header_back_cb(scr, TR_INSTALL_BTN, install_close_cb);

    static const char *nums[3] = { "1", "2", "3" };
    static const int ys[3] = { 100, 200, 300 }, hs[3] = { 92, 92, 98 };
    static const int ws[3] = { 480, 480, 736 };
    lv_obj_t *card[3];
    for (int i = 0; i < 3; i++) {
        card[i] = box(scr, 32, ys[i], ws[i], hs[i], 26, COL_CARD);
        lv_obj_t *bd = box(card[i], 20, (hs[i] - 48) / 2, 48, 48, LV_RADIUS_CIRCLE, COL_SURFACE);
        lv_obj_center(label(bd, &font_govde_b, t->accent, nums[i]));
        rise_in(card[i], 40 + 50 * i);
    }
    lv_obj_set_pos(label(card[0], &font_kucuk, COL_DIM, TR_INSTALL_S1), 88, 12);
    lv_obj_set_pos(label(card[0], &font_baslik, COL_TEXT, PORTAL_SSID), 88, 36);
    lv_obj_set_pos(label(card[1], &font_kucuk, COL_DIM, TR_INSTALL_S2), 88, 12);
    ins.pin = label(card[1], &font_baslik, COL_TEXT, "········");
    lv_obj_set_style_text_letter_space(ins.pin, 8, 0);
    lv_obj_set_pos(ins.pin, 88, 36);
    lv_obj_t *s3 = label(card[2], &font_etiket, COL_SUB, TR_INSTALL_S3);
    lv_obj_set_width(s3, 630);
    lv_obj_set_pos(s3, 88, 10);

    /* Sağda: aynı bilgiyi (ağ adı + şifre) telefon kamerasıyla okutmak için QR kod; metinli talimatların yerine değil, yanına */
    lv_obj_t *qrcard = box(scr, 528, 100, 240, 192, 26, COL_CARD);
    rise_in(qrcard, 90);
    lv_obj_t *plate = box(qrcard, 45, 12, 150, 150, 14, 0xFFFFFF);
    ins.qr = lv_qrcode_create(plate);
    lv_qrcode_set_size(ins.qr, 136);
    lv_qrcode_set_dark_color(ins.qr, hex(0x000000));
    lv_qrcode_set_light_color(ins.qr, hex(0xFFFFFF));
    lv_obj_center(ins.qr);
    lv_obj_remove_flag(ins.qr, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_t *qrcap = label(qrcard, &font_kucuk, COL_DIM, TR_INSTALL_QR);
    lv_obj_set_width(qrcap, 200);
    lv_obj_set_style_text_align(qrcap, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_pos(qrcap, 20, 166);

    ins.status = label(scr, &font_etiket, COL_SUB, TR_INSTALL_STARTING);
    lv_obj_set_pos(ins.status, 40, 424);
    lv_obj_t *cb = circle_btn(scr, 588, 410, 64, t->accent, install_close_cb, NULL);
    lv_obj_set_size(cb, 180, 52);
    lv_obj_set_style_radius(cb, 26, 0);
    lv_obj_set_style_transform_pivot_x(cb, 90, 0);
    lv_obj_set_style_transform_pivot_y(cb, 26, 0);
    lv_obj_center(label(cb, &font_govde, COL_BG, TR_CLOSE));
    rise_in(cb, 240);

    s_scr_timer = lv_timer_create(install_tick, 250, NULL);
    install_tick(s_scr_timer);
    return scr;
}

/* ---- USB ile kurulum: Wi-Fi'siz bilgisayar; PowerShell komutu panelden USB-seri üzerinden dosyayı ister ---- */
static struct {
    lv_obj_t *status, *bar;
    char c_status[64];
    int shown_pct;
} insu;

static void install_usb_tick(lv_timer_t *t)
{
    proto_installer_t p;
    proto_installer_get(&p);
    char b[64];
    int pct = 0;
    if (p.state == INSTALLER_SENDING) {
        pct = p.total ? (int)((uint64_t)p.sent * 100 / p.total) : 0;
        snprintf(b, sizeof(b), TR_INSTALL_USB_SENDING, pct);
    } else if (p.state == INSTALLER_DONE) {
        pct = 100;
        snprintf(b, sizeof(b), "%s", TR_INSTALL_USB_DONE);
    } else {
        snprintf(b, sizeof(b), "%s", TR_INSTALL_USB_WAITING);
    }
    set_txt(insu.status, insu.c_status, sizeof(insu.c_status), b);
    if (pct != insu.shown_pct) {
        insu.shown_pct = pct;
        lv_bar_set_value(insu.bar, pct, LV_ANIM_ON);
    }
}

static lv_obj_t *build_install_usb(void)
{
    memset(&insu, 0, sizeof(insu));
    lv_obj_t *scr = new_screen();
    page_bg(scr);
    header_back_cb(scr, TR_INSTALL_USB_TITLE, install_choose_close_cb);

    lv_obj_t *hint = label(scr, &font_kucuk, COL_SUB, TR_INSTALL_USB_HINT);
    lv_obj_set_width(hint, 736);
    lv_obj_set_pos(hint, 32, 96);
    rise_in(hint, 40);

    lv_obj_t *panel = box(scr, 32, 150, 736, 236, 22, COL_CARD);
    lv_obj_add_flag(panel, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_scroll_dir(panel, LV_DIR_VER);
    lv_obj_set_style_pad_all(panel, 18, 0);
    lv_obj_t *script = label(panel, &font_etiket, COL_TEXT, TR_INSTALL_USB_SCRIPT);
    lv_obj_set_width(script, 700);
    rise_in(panel, 80);

    insu.status = label(scr, &font_etiket, COL_SUB, TR_INSTALL_USB_WAITING);
    lv_obj_set_pos(insu.status, 32, 398);
    insu.bar = lv_bar_create(scr);
    lv_obj_set_size(insu.bar, 736, 14);
    lv_obj_set_pos(insu.bar, 32, 424);
    lv_bar_set_range(insu.bar, 0, 100);
    lv_bar_set_value(insu.bar, 0, LV_ANIM_OFF);
    rise_in(insu.status, 120);
    rise_in(insu.bar, 120);

    s_scr_timer = lv_timer_create(install_usb_tick, 250, NULL);
    install_usb_tick(s_scr_timer);
    return scr;
}

static void ui_show(scr_id_t id, lv_screen_load_anim_t anim, uint32_t ms)
{
    dyn_refresh();                                   /* ekran güncel renklerle kurulsun */
    if (s_scr_timer) {
        lv_timer_delete(s_scr_timer);
        s_scr_timer = NULL;
    }
    bool first = (s_cur == SCR_NONE);
    lv_obj_t *scr;
    switch (id) {
    case SCR_SETTINGS: scr = build_settings(); break;
    case SCR_THEME: scr = build_theme(); break;
    case SCR_IDLE: scr = build_idle(); break;
    case SCR_OFFLINE: scr = build_offline(); break;
    case SCR_SYSTEM: scr = build_system(); break;
    case SCR_TIMER: scr = build_timer(); break;
    case SCR_ALARM: scr = build_alarm(); break;
    case SCR_INSTALL: scr = build_install(); break;
    case SCR_INSTALL_CHOOSE: scr = build_install_choose(); break;
    case SCR_INSTALL_USB: scr = build_install_usb(); break;
    default: scr = build_main(); id = SCR_MAIN; break;
    }
    s_cur = id;
    lv_screen_load_anim(scr, first ? LV_SCR_LOAD_ANIM_NONE : anim, first ? 0 : ms, 0, true);
}

/* Bağlantı, boşta ve uyanma yönetimi (250 ms) */
static void supervisor(lv_timer_t *t)
{
    /* Zamanlayıcı bitti: bilgisayar bağlı olmasa da, boşta ya da karartılmış olsa da alarm çalar */
    if (tm.state == TM_RUNNING && tm_left() <= 0) {
        tm.state = TM_DONE;
        tm.left_ms = 0;
        board_backlight_percent(100);
        ui_show(SCR_ALARM, LV_SCR_LOAD_ANIM_FADE_IN, 300);
        const char *atitle, *aprimary;
        char asub[64];
        alarm_texts(&atitle, asub, sizeof(asub), &aprimary);
        proto_send_alarm(atitle, asub, false);   /* bilgisayarda Windows bildirimi göstersin (kapatılana kadar kalır) */
        return;
    }
    if (s_cur == SCR_ALARM) return;             /* alarm dokunulana kadar kalır */
    proto_state_t s;
    proto_get(&s);
    if (s_debug_hold_until && (int32_t)(s_debug_hold_until - lv_tick_get()) > 0) return;    /* tanılama: ekran zorlandı */
    if (!s.alive) {
        if (s_cur != SCR_OFFLINE && s_cur != SCR_INSTALL && s_cur != SCR_INSTALL_CHOOSE && s_cur != SCR_INSTALL_USB) {  /* kurulum ekranları bağlantı yokken de kalır */
            board_backlight_percent(g_settings.brightness);
            ui_show(SCR_OFFLINE, LV_SCR_LOAD_ANIM_FADE_IN, 300);
        }
        return;
    }
    if (s_portal_wanted) {                       /* bilgisayardaki program bağlandı: kurulum ağı işini bitirdi */
        portal_request(false);
        s_portal_wanted = false;
    }
    if (s_cur == SCR_OFFLINE || s_cur == SCR_INSTALL || s_cur == SCR_INSTALL_CHOOSE || s_cur == SCR_INSTALL_USB) {
        lv_display_trigger_activity(NULL);       /* bağlanınca boşta sayacı sıfırdan başlasın (yoksa hemen karartılırdı) */
        ui_show(SCR_MAIN, LV_SCR_LOAD_ANIM_FADE_IN, 300);
        return;
    }
    /* Mola hatırlatıcı: YALNIZCA bilgisayara (kısa süre görünen) Windows bildirimi gönderir; panelin ekranına dokunmaz.
     * Saat başı kipi: yerel saat HH:00 olunca, en az 20 dk oturum varsa. Aralıklı kip: kesintisiz oturum süresi aralığa ulaşınca. */
    int sess = session_now();
    if (sess >= 0) {
        if (sess < s_break_ref_s) s_break_ref_s = 0;         /* oturum sıfırlandı (bilgisayardan uzaklaşıldı) */
        char dur[32], msg[96];
        fmt_dur(dur, sizeof(dur), sess);
        if (break_is_hourly()) {
            proto_datetime_t dt;
            if (proto_local_datetime(&dt) && dt.mm == s_hour_minute) {
                int key = ((dt.year * 100 + dt.month) * 100 + dt.day) * 100 + dt.hh;
                if (key != s_hour_key) {                      /* her saatte yalnızca bir kez değerlendirilir */
                    s_hour_key = key;
                    if (sess >= s_hour_min_sess_s) {
                        snprintf(msg, sizeof(msg), "Saat %02d:%02d · %s'dır bilgisayar başındasın", dt.hh, dt.mm, dur);
                        proto_send_alarm(TR_BREAK_TIME, msg, true);
                    }
                }
            }
        } else {
            int iv = break_interval_s();
            if (iv > 0 && sess - s_break_ref_s >= iv) {
                s_break_ref_s = sess;
                snprintf(msg, sizeof(msg), "%s'dır bilgisayar başındasın", dur);
                proto_send_alarm(TR_BREAK_TIME, msg, true);
            }
        }
    }
    if (dyn_refresh() && s_cur == SCR_MAIN) {                /* kapak rengi değişti: ana ekran yeni renklerle yeniden kurulur */
        ui_show(SCR_MAIN, LV_SCR_LOAD_ANIM_FADE_IN, 450);
        return;
    }
    uint32_t dim = settings_dim_ms();
    if (s_cur != SCR_IDLE && dim > 0 && lv_display_get_inactive_time(NULL) > dim) {
        board_backlight_percent(IDLE_BRIGHTNESS_PCT);
        ui_show(SCR_IDLE, LV_SCR_LOAD_ANIM_FADE_IN, 600);
    }
}

void ui_init(void)
{
    styles_init();
    tm_load();                              /* zamanlayıcı seçili süreyle hazır başlar */
    /* Açılışta bağlantı yok ekranı; PC'den ilk kalp atışı gelince Şimdi çalıyor'a geçilir */
    ui_show(SCR_OFFLINE, LV_SCR_LOAD_ANIM_NONE, 0);
    board_backlight_percent(g_settings.brightness);
    lv_timer_create(supervisor, 250, NULL);
}

int ui_screen_id(void)
{
    return (int)s_cur;
}

void ui_debug_break_interval(int seconds)
{
    s_break_test_s = seconds > 0 ? seconds : 0;
    int sn = session_now();
    s_break_ref_s = sn > 0 ? sn : 0;
}

void ui_debug_hourly(int minute, int min_session_s)
{
    s_hour_minute = (minute >= 0 && minute < 60) ? minute : 0;
    s_hour_min_sess_s = minute >= 0 ? min_session_s : 20 * 60;
    s_hour_key = -1;
}

void ui_debug_go(int screen_id)
{
    if (screen_id < 0 || screen_id >= SCR_NONE) return;
    if (lvgl_lock(500)) {
        lv_display_trigger_activity(NULL);            /* boşta karartmaya girmesin */
        board_backlight_percent(g_settings.brightness);
        if (screen_id == SCR_OFFLINE || screen_id == SCR_INSTALL || screen_id == SCR_INSTALL_CHOOSE || screen_id == SCR_INSTALL_USB) {
            s_debug_hold_until = lv_tick_get() + 90000;   /* bilgisayar bağlı olsa da denetleyici ekranı hemen geri almasın */
            if (screen_id == SCR_INSTALL) {
                portal_request(true);
                s_portal_wanted = true;
            }
        } else {
            s_debug_hold_until = 0;
        }
        ui_show((scr_id_t)screen_id, LV_SCR_LOAD_ANIM_NONE, 0);
        lvgl_unlock();
    }
}
