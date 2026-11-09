/* Signed OAM coordinate override contract (PpuWsOamCoordinates /
 * PpuWsSetOamCoordinates).
 *
 * A widescreen title can know a sprite's true signed screen position even
 * when the 9-bit OAM X (or the parked Y) cannot express it. It publishes a
 * value-owned coordinate table plus the exact OAM words/high pair the pack
 * produced; at decode time a generation-valid slot renders at its real
 * position and bypasses the unhinted-left admission, the per-pixel
 * ws_clip_left edge clip and the HUD anchor shift. Everything else --
 * signature-mismatched, unpublished, cleared or margin-less decode -- keeps
 * ordinary native behavior, fail-closed for widened-only positions.
 *
 * ROM-free: register state, OAM, VRAM and CGRAM are staged directly on the
 * PPU like the other ppu test modules. Both pixel renderers consume the
 * shared sprite evaluation, so the core scenario runs under each. */
#include "snes/ppu.h"
#include "snes/snes.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

Snes *g_snes;
int snes_frame_counter;
unsigned char g_snesrecomp_last_hdmaen;
uint16_t WsShadowTile(int layer, int x, uint32_t y, uint16_t scroll,
                      uint16_t address, uint16_t tile) {
  (void)layer;
  (void)x;
  (void)y;
  (void)scroll;
  (void)address;
  return tile;
}
bool WsShadowLayerActive(int layer) {
  (void)layer;
  return false;
}
uint32_t WsShadowWorldX(int layer) {
  (void)layer;
  return 0;
}
uint32_t WsShadowWorldY(int layer) {
  (void)layer;
  return 0;
}
uint32_t WsShadowScrollX(int layer) {
  (void)layer;
  return 0;
}
uint32_t WsShadowScrollY(int layer) {
  (void)layer;
  return 0;
}
uint32_t WsShadowPresentWorldY(int layer, int x) {
  (void)layer;
  (void)x;
  return 0;
}
void WsShadowOnVramWrite(uint16_t address, uint16_t value) {
  (void)address;
  (void)value;
}

#define CHECK(cond)                                                            \
  do {                                                                         \
    if (!(cond)) {                                                             \
      fprintf(stderr, "%s:%d: %s\n", __FILE__, __LINE__, #cond);               \
      exit(1);                                                                 \
    }                                                                          \
  } while (0)

/* Framebuffer column for logical screen x (priority-buffer convention). */
#define COL(x) ((x) + kPpuExtraLeftRight)

static Ppu p;
static PpuWsOamCoordinates coords;
static uint32_t frame[kPpuBufWidth * 240];

static unsigned cases;

static uint32_t px(int x, int row) {
  return frame[(size_t)row * kPpuBufWidth + COL(x)];
}
static unsigned emit5(unsigned v) { return (v << 3) | (v >> 2); }
static uint32_t emit_rgb(unsigned r, unsigned g, unsigned b) {
  return emit5(b) | (emit5(g) << 8) | (emit5(r) << 16);
}
static uint32_t emit_color(uint16_t c) {
  return emit_rgb(c & 31, (c >> 5) & 31, (c >> 10) & 31);
}

enum {
  BACKDROP = 0x7fff, /* RGB15 white */
  SPR_A = 0x03e0,    /* palette 0 color 15: green */
  SPR_B = 0x001f,    /* palette 1 color 15: red   */
  SPR_C = 0x03ff,    /* palette 4 color 15: yellow */
  BG1_C = 0x7c00,    /* BG1 palette 0 color 2 */
  PKY = 40           /* packed Y the fixtures use: sprite rows 40..55 */
};

/* All 128 slots parked off-screen (packed Y=0xf0), 16x16 (high-OAM size bit
 * set, X9 clear), tile 0, sprite palette 0, priority 3. Tiles 0..63 are
 * solid color-15. */
static void park_all_oam(void) {
  for (unsigned s = 0; s < 128; ++s) {
    p.oam[s * 2] = 0xf000;     /* X=0, packed Y=0xf0 -> parked */
    p.oam[s * 2 + 1] = 0x3000; /* tile 0, palette 0, priority 3 */
  }
  memset(p.highOam, 0xaa, sizeof p.highOam); /* pair 0b10: x9=0, size=1 */
  memset(p.vram, 0xff, 64 * 16 * sizeof(uint16_t));
}

/* Common widescreen fixture: live 272px margins, OBJ on main screen only. */
static void fixture(unsigned render_flags) {
  memset(&p, 0, sizeof p);
  ppu_reset(&p);
  p.inidisp = 15;
  p.bgmode = 1;
  p.screenEnabled[0] = 16;
  park_all_oam();
  p.cgram[0] = BACKDROP;
  p.cgram[0x8f] = SPR_A;
  p.cgram[0x9f] = SPR_B;
  p.cgram[0xcf] = SPR_C;
  p.cgram[2] = BG1_C;
  PpuSetExtraSpace(&p, kPpuExtraLeftRight);
  memset(frame, 0, sizeof frame);
  PpuBeginDrawing(&p, (uint8_t *)frame, kPpuBufWidth * 4, render_flags);
  ppu_runLine(&p, 0);
}

/* Repark the three scenario slots but leave their native X *visible* aliases
 * so a wrong decode would show up at a different column:
 *   slot 0: real x=300  -> alias (300 & 0x1ff) = 44  (on-screen)
 *   slot 1: real x=520  -> alias (520 & 0x1ff) = 8   (on-screen)
 *   slot 2: real x=-270 -> alias ((-270 & 0xff) | 0x100) = 298 (right margin)
 */
static void stage_alias_slots(void) {
  p.oam[0] = (PKY << 8) | 44;
  p.oam[2] = (PKY << 8) | 8;
  p.oam[4] = (PKY << 8) | 42;
  p.highOam[0] = (uint8_t)((p.highOam[0] & ~0x30u) | 0x30u); /* slot2: x9=1 */
}

/* Same three slots, truly parked (packed Y=0xf0 stays): no decode of this
 * table can place a pixel anywhere. Used for NULL/signature/residue tests. */
static void stage_parked_slots(void) {
  p.oam[0] = 0xf02c; /* x=44  (alias of 300), parked */
  p.oam[2] = 0xf008; /* x=8   (alias of 520), parked */
  p.oam[4] = 0xf02a; /* x=42, high pair below, parked */
  p.highOam[0] = (uint8_t)((p.highOam[0] & ~0x30u) | 0x30u); /* slot2 x9=1 */
}

/* Snapshot the live OAM tables into the publishable struct. */
static void publish_base(void) {
  memset(&coords, 0, sizeof coords);
  memcpy(coords.expectedOam, p.oam, sizeof coords.expectedOam);
  memcpy(coords.expectedHighOam, p.highOam, sizeof coords.expectedHighOam);
}
static void publish_slot(unsigned slot, int16_t x, int16_t packed_y) {
  coords.x[slot] = x;
  coords.y[slot] = packed_y;
  coords.valid[slot >> 3] |= (uint8_t)(1u << (slot & 7));
}

static void draw_rows(int first, int last) {
  for (int row = first; row <= last; ++row) {
    ++snes_frame_counter;
    ppu_runLine(&p, row + 1);
  }
}
static const char *cur_case = "?";
static unsigned cur_flags;
static void expect_span(int x, int row, int w, uint32_t color) {
  for (int i = 0; i < w; ++i) {
    uint32_t got = px(x + i, row);
    if (got != color) {
      fprintf(stderr, "%s:%d: %s x=%d row=%d want=%08x got=%08x\n", __FILE__,
              __LINE__, cur_case, x + i, row, color, got);
      exit(1);
    }
  }
}

static void both_renderers_named(const char *name, void (*fn)(unsigned)) {
  cur_case = name;
  cur_flags = 0;
  fn(0);
  cur_flags = kPpuRenderFlags_NewRenderer;
  fn(kPpuRenderFlags_NewRenderer);
}

/* Core required outcome: signed positions win over every packed alias, in
 * both pixel renderers. */
static void signed_positions(unsigned flags) {
  uint32_t spr = emit_color(SPR_A), bg = emit_color(BACKDROP);
  fixture(flags);
  stage_alias_slots();
  p.oam[6] = (PKY << 8) | 150; /* slot3: ordinary native sprite at x=150 */
  publish_base();
  /* Garbage in slots whose valid bit is clear must be ignored entirely. */
  coords.x[9] = 100;
  coords.y[9] = PKY;
  publish_slot(0, 300, PKY);
  publish_slot(1, 520, PKY);
  publish_slot(2, -270, PKY);
  PpuWsSetOamCoordinates(&p, &coords);
  /* Value ownership: trashing the caller's copy must not change the PPU. */
  memset(&coords, 0, sizeof coords);
  draw_rows(PKY - 1, PKY + 16);
  for (int row = PKY; row < PKY + 16; ++row) {
    expect_span(300, row, 16, spr); /* x=300  -> 300, never 44  */
    expect_span(44, row, 16, bg);
    expect_span(520, row, 8, spr); /* x=520 -> 520; 528.. clip past margin */
    expect_span(8, row, 16, bg);
    expect_span(-270, row, 16, spr); /* x=-270 -> -270, never 298/42 */
    expect_span(298, row, 2, bg);    /* alias 298: only cols before sprite */
    expect_span(42, row, 16, bg);
    expect_span(150, row, 16, spr); /* ordinary native slot unaffected */
    expect_span(100, row, 16, bg);  /* invalid-slot garbage not applied */
  }
  /* Row isolation: nothing above or below the 16-row sprite band (the two
   * border rows are drawn so the backdrop really covers the margin). */
  expect_span(300, PKY - 1, 16, bg);
  expect_span(520, PKY - 1, 8, bg);
  expect_span(-270, PKY - 1, 16, bg);
  expect_span(300, PKY + 16, 16, bg);
  expect_span(520, PKY + 16, 8, bg);
  expect_span(-270, PKY + 16, 16, bg);
  cases++;
}

/* NULL, a cleared valid bit, or any signature divergence -> ordinary native
 * decode; a parked widened-only object therefore emits nothing. */
static void null_and_signature_fallback(unsigned flags) {
  uint32_t bg = emit_color(BACKDROP);
  /* NULL: no publication at all. */
  fixture(flags);
  stage_parked_slots();
  draw_rows(PKY, PKY + 2);
  for (int row = PKY; row <= PKY + 2; ++row) {
    expect_span(300, row, 16, bg);
    expect_span(44, row, 16, bg);
    expect_span(520, row, 8, bg);
    expect_span(-270, row, 16, bg);
  }
  /* Published, then explicitly cleared. */
  fixture(flags);
  stage_parked_slots();
  publish_base();
  publish_slot(0, 300, PKY);
  publish_slot(1, 520, PKY);
  publish_slot(2, -270, PKY);
  PpuWsSetOamCoordinates(&p, &coords);
  PpuWsSetOamCoordinates(&p, NULL);
  draw_rows(PKY, PKY + 2);
  for (int row = PKY; row <= PKY + 2; ++row) {
    expect_span(300, row, 16, bg);
    expect_span(520, row, 8, bg);
    expect_span(-270, row, 16, bg);
  }
  /* Each signature element mismatched independently: only that slot falls
   * back. Mismatch kinds: X word (word0), tile/attribute word (word1),
   * packed-Y byte of word0, and the slot's high-OAM pair. */
  static const struct {
    unsigned kind; /* 0 word0, 1 word1, 2 Y byte, 3 high pair */
  } kinds[4] = {{0}, {1}, {2}, {3}};
  for (unsigned k = 0; k < 4; ++k) {
    fixture(flags);
    stage_parked_slots();
    publish_base();
    publish_slot(0, 300, PKY);
    publish_slot(1, 520, PKY);
    publish_slot(2, -270, PKY);
    switch (kinds[k].kind) {
    case 0:
      coords.expectedOam[0] ^= 0x0001;
      break; /* slot0 x low  */
    case 1:
      coords.expectedOam[1] ^= 0x0400;
      break; /* slot0 attr   */
    case 2:
      coords.expectedOam[0] ^= 0x8000;
      break; /* slot0 Y bit  */
    case 3:
      coords.expectedHighOam[0] ^= 0x03;
      break; /* slot0 pair   */
    }
    PpuWsSetOamCoordinates(&p, &coords);
    draw_rows(PKY, PKY + 2);
    for (int row = PKY; row <= PKY + 2; ++row) {
      expect_span(300, row, 16, bg); /* slot0 fell back: parked */
      expect_span(520, row, 8, emit_color(SPR_A));
      expect_span(-270, row, 16, emit_color(SPR_A));
    }
    cases++;
  }
  /* Recycled slot: the game rewrote OAM after the pack was published, so the
   * signature no longer matches and the stale coordinate cannot bind. */
  fixture(flags);
  stage_parked_slots();
  publish_base();
  publish_slot(0, 300, PKY);
  publish_slot(1, 520, PKY);
  PpuWsSetOamCoordinates(&p, &coords);
  p.oam[0] = (PKY << 8) | 90; /* slot 0 repurposed on-screen */
  draw_rows(PKY, PKY + 2);
  for (int row = PKY; row <= PKY + 2; ++row) {
    expect_span(300, row, 16, bg);               /* stale x=300 not reused */
    expect_span(90, row, 16, emit_color(SPR_A)); /* native decode instead */
    expect_span(520, row, 8, emit_color(SPR_A));
  }
  cases++;
}

/* Reserved-range slots: the generic API does not forbid them; signature
 * still rules. (Titles themselves refuse to publish slots 0..15.) */
static void reserved_slots_override(unsigned flags) {
  uint32_t spr = emit_color(SPR_A), bg = emit_color(BACKDROP);
  fixture(flags);
  stage_parked_slots();
  publish_base();
  publish_slot(4, 300, PKY); /* a slot inside a title's reserved range */
  publish_slot(5, -100, PKY);
  PpuWsSetOamCoordinates(&p, &coords);
  draw_rows(PKY, PKY + 2);
  for (int row = PKY; row <= PKY + 2; ++row) {
    expect_span(300, row, 16, spr);
    expect_span(-100, row, 16, spr);
    expect_span(-84, row, 16, bg);
  }
  cases++;
}

/* Both live margins zero (centered/pillarbox with a nonzero budget): the
 * coordinates are inert; ordinary native decode is all that runs. */
static void inert_with_zero_live_margins(unsigned flags) {
  uint32_t spr = emit_color(SPR_A);
  fixture(flags);
  PpuSetExtraSpaceCentered(&p, kPpuExtraLeftRight); /* budget set, cur = 0 */
  stage_alias_slots();
  publish_base();
  publish_slot(0, 300, PKY);
  publish_slot(1, 520, PKY);
  publish_slot(2, -270, PKY);
  PpuWsSetOamCoordinates(&p, &coords);
  draw_rows(PKY, PKY + 2);
  for (int row = PKY; row <= PKY + 2; ++row) {
    expect_span(44, row, 16, spr); /* native alias wins: ordinary decode */
    expect_span(8, row, 16, spr);
    expect_span(300, row, 16, 0); /* pillarbox region never drawn */
    expect_span(520, row, 8, 0);
    expect_span(-270, row, 16, 0);
    /* slot2 raw alias is 298 >= 256+0 -> wraps to -214, clipped: nothing */
    expect_span(298, row, 16, 0);
  }
  /* Margins go live again -> the same publication activates. */
  PpuSetExtraSideSpace(&p, 272, 272, 0);
  draw_rows(PKY, PKY + 2);
  for (int row = PKY; row <= PKY + 2; ++row) {
    expect_span(300, row, 16, spr);
    expect_span(44, row, 16, emit_color(BACKDROP));
    expect_span(-270, row, 16, spr);
  }
  cases++;
}

/* A generation-valid slot bypasses the unhinted-left admission gate AND the
 * per-pixel ws_clip_left clip; an unhinted ordinary slot is rejected. Margins
 * are small so a negative raw decode actually exists. */
static void hint_and_clip_bypass(unsigned flags) {
  uint32_t spr = emit_color(SPR_A), bg = emit_color(BACKDROP);
  /* Unhinted rejection control, no coordinates. */
  fixture(flags);
  PpuSetExtraSpaceCentered(&p, kPpuExtraLeftRight);
  PpuSetExtraSideSpace(&p, 64, 32, 0);
  p.wsOamLeftHintStrict = 1;   /* strict, zero hints */
  p.wsOamMotionGraceOn = 0;    /* admit ONLY hinted slots */
  p.oam[0] = (PKY << 8) | 212; /* raw 9-bit 468 -> -44 */
  p.highOam[0] = (uint8_t)((p.highOam[0] & ~0x03u) | 0x03u); /* x9=1,size=1 */
  draw_rows(PKY, PKY + 2);
  for (int row = PKY; row <= PKY + 2; ++row)
    expect_span(-44, row, 16, bg); /* rejected wholesale */
  /* Same slot, now coordinate-valid: hint gate and ws_clip_left bypassed, all
   * 16 pixels drawn even though the true x straddles the screen edge. */
  fixture(flags);
  PpuSetExtraSpaceCentered(&p, kPpuExtraLeftRight);
  PpuSetExtraSideSpace(&p, 64, 32, 0);
  p.wsOamLeftHintStrict = 1;
  p.wsOamMotionGraceOn = 0;
  p.oam[0] = (PKY << 8) | 212;
  p.highOam[0] = (uint8_t)((p.highOam[0] & ~0x03u) | 0x03u);
  publish_base();
  publish_slot(0, -8, PKY);
  PpuWsSetOamCoordinates(&p, &coords);
  draw_rows(PKY, PKY + 2);
  for (int row = PKY; row <= PKY + 2; ++row) {
    expect_span(-8, row, 16, spr); /* includes the x<0 half */
    expect_span(8, row, 8, bg);
    expect_span(-44, row, 16, bg); /* alias not drawn either */
  }
  cases++;
}

/* HUD relocation bypass: a valid slot renders exactly where published; a
 * non-valid slot in the HUD range keeps the anchored shift. */
static void hud_bypass(unsigned flags) {
  uint32_t spr = emit_color(SPR_A), bg = emit_color(BACKDROP);
  fixture(flags);
  PpuSetExtraSpaceCentered(&p, kPpuExtraLeftRight);
  PpuSetExtraSideSpace(&p, 64, 64, 0);
  p.wsHudOamHeight = 224;
  p.wsHudLeftEnd = 32;
  p.wsHudRightStart = 224;
  p.wsHudSplitHeight = 0x80; /* full-budget HUD */
  p.wsHudOamFirstSlot = 0;
  p.wsHudOamSlots = 2;        /* slots 0,1 are HUD-anchored */
  p.oam[0] = (PKY << 8) | 20; /* native x=20 < leftEnd -> would shift */
  p.oam[2] = (PKY << 8) | 20; /* control: same, no coordinates */
  publish_base();
  publish_slot(0, 20, PKY); /* published exactly at the native x */
  PpuWsSetOamCoordinates(&p, &coords);
  draw_rows(PKY, PKY + 2);
  for (int row = PKY; row <= PKY + 2; ++row) {
    expect_span(20, row, 16, spr); /* NOT relocated to 20-272 */
    expect_span(36, row, 16, bg);
  }
  /* The un-published control HUD sprite relocated out by the full budget and
   * is not visible anywhere (its target -252 is outside the 64px margin). */
  fixture(flags);
  PpuSetExtraSpaceCentered(&p, kPpuExtraLeftRight);
  PpuSetExtraSideSpace(&p, 64, 64, 0);
  p.wsHudOamHeight = 224;
  p.wsHudLeftEnd = 32;
  p.wsHudRightStart = 224;
  p.wsHudSplitHeight = 0x80;
  p.wsHudOamFirstSlot = 0;
  p.wsHudOamSlots = 2;
  p.oam[0] = (PKY << 8) | 20;
  publish_base();
  publish_slot(1, 90, PKY); /* slot1 binds: override x=90; slot0 stays native */
  PpuWsSetOamCoordinates(&p, &coords);
  draw_rows(PKY, PKY + 2);
  for (int row = PKY; row <= PKY + 2; ++row) {
    expect_span(20, row, 16, bg);
    expect_span(90, row, 16, emit_color(SPR_A)); /* slot1's own override */
  }
  cases++;
}

/* Hardware windows still clip overridden sprites. */
static void windows_apply(unsigned flags) {
  uint32_t spr = emit_color(SPR_A), bg = emit_color(BACKDROP);
  fixture(flags);
  stage_alias_slots();
  publish_base();
  publish_slot(0, 100, PKY); /* override x=100, window hides 104..111 */
  PpuWsSetOamCoordinates(&p, &coords);
  p.windowsel = 2u << 16; /* OBJ layer: window 1 enabled */
  p.window1left = 104;
  p.window1right = 111;
  p.screenWindowed[0] = 16;
  draw_rows(PKY, PKY + 2);
  for (int row = PKY; row <= PKY + 2; ++row) {
    expect_span(100, row, 4, spr);
    expect_span(104, row, 8, bg);
    expect_span(112, row, 4, spr);
  }
  cases++;
}

/* Color math still applies to overridden sprite pixels. */
static void color_math(unsigned flags) {
  fixture(flags);
  stage_alias_slots();
  p.oam[1] = 0x3800;           /* palette 4, prio 3 (math-eligible class) */
  p.oam[7] = 0x3800;           /* native world sibling, same attributes */
  p.oam[6] = (PKY << 8) | 150; /* ... at x=150 */
  p.cgadsub = 0x50;            /* math on OBJ (layer 4), half color math */
  p.fixedColor = 31 | (31 << 5) | (31 << 10);
  publish_base();
  publish_slot(0, 300, PKY);
  PpuWsSetOamCoordinates(&p, &coords);
  draw_rows(PKY, PKY + 2);
  for (int row = PKY; row <= PKY + 2; ++row)
    for (int i = 0; i < 16; ++i) {
      uint32_t nat = px(150 + i, row), ovr = px(300 + i, row);
      if (nat != ovr) {
        fprintf(stderr,
                "%s:%d: %s flags=%u native x=%d vs override x=%d row=%d "
                "%08x!=%08x\n",
                __FILE__, __LINE__, cur_case, cur_flags, 150 + i, 300 + i, row,
                nat, ovr);
        exit(1);
      }
    }
  cases++;
}

/* Priority: overridden sprites keep their OAM priority against BG1. Mode 1:
 * sprite prio3 (14) > BG1 prio1 (12) > sprite prio1 (6); BG1 prio0 (8) >
 * sprite prio0 (2). */
static void priority_preserved(unsigned flags) {
  uint32_t spr = emit_color(SPR_A), bgc = emit_color(BG1_C);
  static const unsigned prios[3] = {0, 1, 3};
  static const uint16_t bg_prio[3] = {1, 1, 0}; /* BG1 tile priority bit */
  static const int sprite_wins[3] = {0, 0, 1};
  for (unsigned t = 0; t < 3; ++t) {
    fixture(flags);
    p.screenEnabled[0] = 17; /* OBJ + BG1 */
    p.bgXsc[0] = 0x10;       /* BG1 tilemap at vram 0x1000 */
    p.bgTileAdr = 0x4;       /* BG1 tiles at vram 0x4000 */
    for (unsigned i = 0; i < 32 * 32; ++i)
      p.vram[0x1000 + i] = (uint16_t)(1 | (bg_prio[t] ? 0x2000 : 0));
    /* Plane 1 high byte set: every pixel = color index 2 -> cgram[2]. */
    for (unsigned i = 0; i < 8; ++i)
      p.vram[0x4000 + 16 + i] = 0xff00;
    memset(&p.vram[0x4000 + 24], 0, 24 * sizeof(uint16_t));
    stage_alias_slots();
    p.oam[1] = (uint16_t)(0x3000 * (prios[t] == 3) |
                          0x1000 * (prios[t] == 1)); /* tile0,prio(t) */
    publish_base();
    publish_slot(0, 300, PKY);
    PpuWsSetOamCoordinates(&p, &coords);
    draw_rows(PKY, PKY + 2);
    for (int row = PKY; row <= PKY + 2; ++row)
      expect_span(300, row, 16, sprite_wins[t] ? spr : bgc);
    cases++;
  }
}

/* Full 272px capacity: sprites across the whole widened span render. */
static void full_span_capacity(unsigned flags) {
  uint32_t spr = emit_color(SPR_A);
  fixture(flags | kPpuRenderFlags_NoSpriteLimits);
  stage_alias_slots();
  publish_base();
  static const int xs[] = {-272, -240, -208, -176, -144, -112, -80, -48, 256,
                           288,  320,  352,  384,  416,  448,  480, 512};
  for (unsigned s = 0; s < sizeof xs / sizeof xs[0]; ++s)
    publish_slot(s, (int16_t)xs[s], PKY);
  PpuWsSetOamCoordinates(&p, &coords);
  draw_rows(PKY, PKY + 2);
  for (unsigned s = 0; s < sizeof xs / sizeof xs[0]; ++s)
    for (int row = PKY; row <= PKY + 2; ++row)
      expect_span(xs[s], row, 16, spr);
  cases += sizeof xs / sizeof xs[0];
}

/* The publication is host state: a frame boundary with no fresh pack must
 * not clear it (the restored state is not consumed by rendering). */
static void persistence_across_frames(unsigned flags) {
  uint32_t spr = emit_color(SPR_A);
  fixture(flags);
  stage_parked_slots();
  publish_base();
  publish_slot(0, 300, PKY);
  PpuWsSetOamCoordinates(&p, &coords);
  draw_rows(PKY, PKY + 2);
  expect_span(300, PKY, 16, spr);
  for (unsigned f = 0; f < 3; ++f) {
    ppu_runLine(&p, 0);
    draw_rows(PKY, PKY + 2);
    for (int row = PKY; row <= PKY + 2; ++row)
      expect_span(300, row, 16, spr);
  }
  cases++;
}

/* Save/load trampolines over one buffer. */
static size_t sl_pos;
static uint8_t sl_blob[8 + PPU_SAVESTATE_REGS_SIZE + PPU_SAVESTATE_MEM_SIZE];
static void sl_save_fn(SaveLoadInfo *sli, void *data, size_t size) {
  (void)sli;
  memcpy(sl_blob + sl_pos, data, size);
  sl_pos += size;
}
static void sl_load_fn(SaveLoadInfo *sli, void *data, size_t size) {
  (void)sli;
  memcpy(data, sl_blob + sl_pos, size);
  sl_pos += size;
}

static void save_and_load(unsigned flags) {
  uint32_t spr = emit_color(SPR_A), bg = emit_color(BACKDROP);
  fixture(flags);
  stage_parked_slots();
  publish_base();
  publish_slot(0, 300, PKY);
  PpuWsSetOamCoordinates(&p, &coords);
  draw_rows(PKY, PKY + 2);
  expect_span(300, PKY, 16, spr);
  /* Save: guest snapshot leaves the coordinate state alone. */
  SaveLoadInfo sli;
  sl_pos = 0;
  sli.func = sl_save_fn;
  sli.peek = NULL;
  ppu_saveload(&p, &sli);
  draw_rows(PKY, PKY + 2);
  for (int row = PKY; row <= PKY + 2; ++row)
    expect_span(300, row, 16, spr);
  /* Load of the byte-identical snapshot: OAM comes back the same generation,
   * so the signature is still honest and the publication survives. (The
   * saveload API carries no direction flag; a restore that changes nothing
   * is indistinguishable from a save and must not clear.) */
  sl_pos = 0;
  sli.func = sl_load_fn;
  ppu_saveload(&p, &sli);
  draw_rows(PKY, PKY + 2);
  for (int row = PKY; row <= PKY + 2; ++row)
    expect_span(300, row, 16, spr);
  /* Load of a *changed* snapshot: the guest arrays move, the publication is
   * cleared, and the title must republish. */
  sl_blob[8 + PPU_SAVESTATE_REGS_SIZE + 0x200] ^= 1; /* first OAM word byte */
  sl_pos = 0;
  ppu_saveload(&p, &sli);
  draw_rows(PKY, PKY + 2);
  for (int row = PKY; row <= PKY + 2; ++row) {
    expect_span(300, row, 16, bg);
    expect_span(520, row, 8, bg);
  }
  /* Registers-only delta: a load that changes just the PPU register block
   * (OAM, highOAM, CGRAM, VRAM all byte-identical) is still a new guest
   * generation and must clear. The inidisp flip also changes global
   * brightness, so compare against a known-empty backdrop column rather
   * than the pre-load `bg` constant. */
  publish_base();
  publish_slot(0, 300, PKY);
  PpuWsSetOamCoordinates(&p, &coords);
  draw_rows(PKY, PKY + 2);
  expect_span(300, PKY, 16, spr);
  sl_blob[8] ^= 1; /* first byte of the regs block (inidisp) */
  sl_pos = 0;
  ppu_saveload(&p, &sli);
  draw_rows(PKY, PKY + 2);
  for (int row = PKY; row <= PKY + 2; ++row)
    for (int i = 0; i < 16; ++i)
      if (px(300 + i, row) != px(200, row)) { /* col 200: empty backdrop */
        fprintf(stderr,
                "%s:%d: %s regs-only load: x=%d row=%d %08x != backdrop %08x\n",
                __FILE__, __LINE__, cur_case, 300 + i, row, px(300 + i, row),
                px(200, row));
        exit(1);
      }
  /* Upper-CGRAM-only delta: the raw visitor writes 512 cgram bytes, so a
   * change in entries 128..255 must also clear. */
  publish_base();
  publish_slot(0, 300, PKY);
  PpuWsSetOamCoordinates(&p, &coords);
  draw_rows(PKY, PKY + 2);
  /* Global brightness is still 14 from the regs-only load, so assert the
   * sprite is present (non-backdrop) rather than the pre-load `spr` value. */
  for (int i = 0; i < 16; ++i)
    if (px(300 + i, PKY) == px(200, PKY)) {
      fprintf(stderr, "%s:%d: %s republished sprite missing at x=%d\n",
              __FILE__, __LINE__, cur_case, 300 + i);
      exit(1);
    }
  sl_blob[8 + PPU_SAVESTATE_REGS_SIZE + 511] ^= 1; /* cgram[255] high byte */
  sl_pos = 0;
  ppu_saveload(&p, &sli);
  draw_rows(PKY, PKY + 2);
  for (int row = PKY; row <= PKY + 2; ++row)
    for (int i = 0; i < 16; ++i)
      if (px(300 + i, row) != px(200, row)) {
        fprintf(
            stderr,
            "%s:%d: %s cgram-only load: x=%d row=%d %08x != backdrop %08x\n",
            __FILE__, __LINE__, cur_case, 300 + i, row, px(300 + i, row),
            px(200, row));
        exit(1);
      }
  /* Ordinary reset also clears. */
  publish_base();
  publish_slot(0, 300, PKY);
  PpuWsSetOamCoordinates(&p, &coords);
  draw_rows(PKY, PKY + 2);
  for (int i = 0; i < 16; ++i) /* brightness still 14: non-backdrop check */
    if (px(300 + i, PKY) == px(200, PKY)) {
      fprintf(stderr, "%s:%d: %s republished sprite missing at x=%d\n",
              __FILE__, __LINE__, cur_case, 300 + i);
      exit(1);
    }
  ppu_reset(&p);
  p.inidisp = 15;
  p.bgmode = 1;
  p.screenEnabled[0] = 16;
  park_all_oam();
  stage_parked_slots();
  p.cgram[0] = BACKDROP;
  p.cgram[0x8f] = SPR_A;
  PpuSetExtraSpace(&p, kPpuExtraLeftRight);
  memset(frame, 0, sizeof frame);
  PpuBeginDrawing(&p, (uint8_t *)frame, kPpuBufWidth * 4, flags);
  ppu_runLine(&p, 0);
  draw_rows(PKY, PKY + 2);
  for (int row = PKY; row <= PKY + 2; ++row)
    expect_span(300, row, 16, bg);
  cases++;
}

/* Rollback residue carries the value-owned coordinates: a rewound frame
 * restores them so a speculative frame cannot leak its own publication. */
static void rollback_residue_roundtrip(unsigned flags) {
  uint32_t spr = emit_color(SPR_A), bg = emit_color(BACKDROP);
  fixture(flags);
  stage_parked_slots();
  publish_base();
  publish_slot(0, 300, PKY);
  PpuWsSetOamCoordinates(&p, &coords);
  draw_rows(PKY, PKY + 2);
  expect_span(300, PKY, 16, spr);

  PpuRollbackResidue rb;
  memset(&rb, 0, sizeof rb);
  ppu_rb_residue_get(&p, &rb);

  /* Speculative frame: a different publication goes through the same API. */
  publish_base();
  publish_slot(0, 520, PKY);
  publish_slot(1, 60, PKY);
  PpuWsSetOamCoordinates(&p, &coords);
  memset(frame, 0, sizeof frame);
  ppu_runLine(&p, 0);
  draw_rows(PKY, PKY + 2);
  for (int row = PKY; row <= PKY + 2; ++row) {
    expect_span(520, row, 8, spr);
    expect_span(60, row, 16, spr);
    expect_span(300, row, 16, bg);
  }

  /* Rewind: residue set restores the earlier coordinates, even though no
   * pack ran between the get and the set. */
  ppu_rb_residue_set(&p, &rb);
  memset(frame, 0, sizeof frame);
  ppu_runLine(&p, 0);
  draw_rows(PKY, PKY + 2);
  for (int row = PKY; row <= PKY + 2; ++row) {
    expect_span(300, row, 16, spr);
    expect_span(520, row, 8, bg);
    expect_span(60, row, 16, bg);
  }

  /* After the restore the coordinates stay put for following frames: no
   * setter ran since the residue set. */
  ppu_runLine(&p, 0);
  draw_rows(PKY, PKY + 2);
  for (int row = PKY; row <= PKY + 2; ++row)
    expect_span(300, row, 16, spr);
  cases++;
}

/* Coordinate publication must not be cleared by PpuResetWidescreenOamHistory
 * (which only re-seeds the motion classifier). */
static void history_reset_independence(unsigned flags) {
  uint32_t spr = emit_color(SPR_A);
  fixture(flags);
  stage_parked_slots();
  publish_base();
  publish_slot(0, 300, PKY);
  PpuWsSetOamCoordinates(&p, &coords);
  PpuResetWidescreenOamHistory(&p);
  draw_rows(PKY, PKY + 2);
  for (int row = PKY; row <= PKY + 2; ++row)
    expect_span(300, row, 16, spr);
  cases++;
}

int main(void) {
  both_renderers_named("signed_positions", signed_positions);
  both_renderers_named("null_sig_fallback", null_and_signature_fallback);
  both_renderers_named("reserved_slots", reserved_slots_override);
  both_renderers_named("inert_zero_margins", inert_with_zero_live_margins);
  both_renderers_named("hint_clip_bypass", hint_and_clip_bypass);
  both_renderers_named("hud_bypass", hud_bypass);
  both_renderers_named("windows_apply", windows_apply);
  both_renderers_named("color_math", color_math);
  both_renderers_named("priority_preserved", priority_preserved);
  both_renderers_named("full_span_capacity", full_span_capacity);
  both_renderers_named("persistence_across_frames", persistence_across_frames);
  both_renderers_named("save_and_load", save_and_load);
  both_renderers_named("rollback_residue", rollback_residue_roundtrip);
  both_renderers_named("history_reset_independence",
                       history_reset_independence);
  printf("ppu_oam_coordinates: PASS cases=%u\n", cases);
  return 0;
}
