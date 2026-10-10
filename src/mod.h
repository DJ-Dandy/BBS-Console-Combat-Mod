#pragma once
#include "core.h"
/* mp.c */
int   mp_in_burn(void);
int   mp_blocks_link(void);                    /* MP charge keeps a new D-Link from being started (DLinkDuringCharge = 0 only) */
float mp_burn_pct(void);
u8   *mp_use(u8 *plate);            /* the deck "use" gate: COMMAND* or NULL; charges MP */
float mp_cost(int id);
int   mp_would_burn(int id);         /* using this command now would empty the MP bar */
void  hud_timer_bar(int *ph, float left, float top, float w, float hgt, float frac, u32 fill, int prio, int show);
/* bundle.c */
int  bundle_check(void);
void bundle_apply(void);
int  bundle_combo_in_exe(void);     /* the old Combo Flow exe patch is installed */
void bundle_restore_sites(void);    /* ... take its three hooks out again (in memory) */
/* speed.c */
int  speed_check(void);
void speed_apply(void);
int  speed_enabled(void);
/* menu.c */
int  menu_check(void);
void menu_apply(void);
void menu_frame(u8 *gauge);
void menu_dbg(const char *cmd, char *args);
/* tex.c */
int  tex_check(void);
void tex_apply(void);
void tex_enable(int on);
int  tex_art_ready(void);           /* 1 once KH2's MP bar art is in the gauge texture */
int  tex_stat_art_ready(void);      /* 1 once the "MP" label is in the menu texture */
/* the mod's own 2D instances, for the crash log: appends " name=handle" for every live one */
void mod_own_handles(char *out, int n);
int  own_add(char *out, int n, const char *name, int h);
void menu_own(char *out, int n);
void style_own(char *out, int n);
void status_own(char *out, int n);
void sccamp_own(char *out, int n);
int  tex_hd_art_ready(void);        /* 1 once the list header words are in the command texture */
/* shortcut.c: L1 + face button = the command in a chosen deck slot */
#define SC_ROWS 4                   /* circle, triangle, square, cross */
#define SC_SETS 2                   /* shortcut sets: the d-pad flips between them while the list is up */
int  shortcut_check(void);
void shortcut_apply(void);
int  sc_enabled(void);
int  sc_held(void);                 /* L1 held and the shortcut list shown this frame */
int  sc_want(void);                 /* row of a pressed shortcut still waiting to be carried out, or -1 */
void sc_done(void);
int  sc_slot(int row);              /* deck slot 0..7 of that row's shortcut in the set shown, -1 = none */
int  sc_assign(int row, int slot);
int  sc_slot_in(int set, int row);  /* ... of a given set */
int  sc_assign_in(int set, int row, int slot);
int  sc_sets(void);                 /* sets in use: 1 or 2 */
int  sc_page(void);                 /* the set shown in battle, 0 or 1 */
u32  sc_row_mask(int row);
const char *sc_row_icon(int row);   /* text code of the button's picture */
void sc_set_hud(int on);
/* sccamp.c: the Shortcuts entry of the menu's Command Decks screen */
int  sccamp_check(void);
void sccamp_apply(void);
void sccamp_on_destroy(int h);      /* an instance of the 2D runtime is about to be destroyed */
/* status.c */
/* what SetFontParam (1401a7450 -> 1401aa820) reads: flags 1 text, 2 colour, 4 pitch, 8 alignment, 0x40 size, 0x80 kind */
typedef struct { const char *text; u32 colour; u8 space[4]; u8 pad10[4]; u16 flags; u8 b16, pitch, align, size, b1a, b1b, kind, pad[3]; } FontParam;
int  l2d_attach_seq(int parent, u8 *parent_ctrl, u16 node, u16 seq);
int  l2d_destroy_hook(void);        /* install the hook on Destroy (once); 0 on failure */
int  l2d_destroy_site_ok(void);
void l2d_destroy(int h);            /* Destroy, past the hook */
int  status_check(void);
void status_apply(void);
/* mp.c */
void mp_get(float *cur, float *max);
/* menu.c */
int  menu_react_prompt(u8 *cmd);    /* a context prompt answered with triangle is up */
/* style.c */
int  style_check(void);
void style_apply(void);
void style_frame(u8 *cmd, int hud, float x, float y);    /* x, y: where a prompt plate goes (above the gauge window) */
int  style_prompt_active(void);
int  style_prompt_shown(void);

/* guard.c: 2D nodes whose texture is gone are not drawn with it (and the log says which) */
int  combomaster_check(void);
void combomaster_apply(void);
void combomaster_texts(u8 *self);        /* a message file is in memory (called from mp.c's hook) */
int  combomaster_on(void);
int  guard_check(void);
void guard_apply(void);
void guard_stats(int *fixed, int *skipped);
/* an instance of ours whose file was unloaded under it is destroyed and *ph set to 0 (returns 0 then, else 1) */
int  l2d_live(int *ph);
void menu_shutdown(void);        /* menu.c: every HUD instance of the menu and the style offer is destroyed */
void style_shutdown(void);       /* style.c */
