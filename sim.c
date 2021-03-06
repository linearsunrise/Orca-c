#include "sim.h"
#include "gbuffer.h"
#include "mark.h"

//////// Utilities

static Glyph const indexed_glyphs[] = {
    '0', '1', '2', '3', '4', '5', '6', '7', '8', '9', 'a', 'b', //  0 - 11
    'c', 'd', 'e', 'f', 'g', 'h', 'i', 'j', 'k', 'l', 'm', 'n', // 12 - 23
    'o', 'p', 'q', 'r', 's', 't', 'u', 'v', 'w', 'x', 'y', 'z', // 24 - 35
};

enum { Glyphs_index_count = sizeof indexed_glyphs };

// Always returns 0 through (sizeof indexed_glyphs) - 1, and works on
// capitalized glyphs as well. The index of the lower-cased glyph is returned
// if the glyph is capitalized.
#if 1
// Branchless implementation. Assumes two's complement.
static Usz index_of(Glyph c) {
  int i = c;
  enum {
    // All number chars have this bit set. Some alpha chars do.
    Num_bit = 1 << 4,
    // All alpha chars have this bit set. No number chars do.
    Alpha_bit = 1 << 6,
    // The bits we use from a number char (0000 1111) to get an index number
    Lower_4 = 0xF,
    // The bits we use from an alpha char (0001 1111) to get an index number
    Lower_5 = 0x1F,
  };
  union {
    uint32_t u;
    int32_t i;
  } pui;
  // Turn the alpha bit into a mask of all 32 bits
  pui.u = (uint32_t)(i & Alpha_bit) << UINT32_C(25);
  int alpha_mask = pui.i >> 31;
  // Turn the number bit into a mask of all 32 bits
  pui.u = (uint32_t)(i & Num_bit) << UINT32_C(27);
  int num_mask = pui.i >> 31;
  // If it's an alpha char, we add 9 to it, bringing 'a'/'A' from 1 to 10, 'b'
  // to 11, etc.
  return (Usz)((i & ((alpha_mask & Lower_5) | (num_mask & Lower_4))) +
               (9 & alpha_mask));
  // If the glyph might be a non-valid char in certain ranges (like '^' char)
  // we will return a number here greater than 35. We could do % 36 here if we
  // wanted to be really safe.
}
#else
// Reference implementation
static Usz index_of(Glyph c) {
  if (c == '.' || c == '*')
    return 0;
  if (c >= '0' && c <= '9')
    return (Usz)(c - '0');
  if (c >= 'A' && c <= 'Z')
    return (Usz)(c - 'A' + 10);
  if (c >= 'a' && c <= 'z')
    return (Usz)(c - 'a' + 10);
  return 0;
}
#endif

static Usz safe_index_of(Glyph c) { return index_of(c) % 36; }

static inline Glyph glyph_of(Usz index) {
  assert(index < Glyphs_index_count);
  return indexed_glyphs[index];
}

static inline bool glyph_is_lowercase(Glyph g) { return g & (1 << 5); }
static inline Glyph glyph_lowered_unsafe(Glyph g) {
  return (Glyph)(g | (1 << 5));
}

ORCA_PURE static bool oper_has_neighboring_bang(Glyph const* gbuf, Usz h, Usz w,
                                                Usz y, Usz x) {
  Glyph const* gp = gbuf + w * y + x;
  if (x < w - 1 && gp[1] == '*')
    return true;
  if (x > 0 && *(gp - 1) == '*')
    return true;
  if (y < h - 1 && gp[w] == '*')
    return true;
  // note: negative array subscript on rhs of short-circuit, may cause ub if
  // the arithmetic under/overflows, even if guarded the guard on lhs is false
  if (y > 0 && *(gp - w) == '*')
    return true;
  return false;
}

// Returns UINT8_MAX if not a valid note.
static U8 midi_note_number_of(Glyph g) {
  switch (g) {
  case 'C':
    return 0;
  case 'c':
    return 1;
  case 'D':
    return 2;
  case 'd':
    return 3;
  case 'E':
    return 4;
  case 'F':
    return 5;
  case 'f':
    return 6;
  case 'G':
    return 7;
  case 'g':
    return 8;
  case 'A':
    return 9;
  case 'a':
    return 10;
  case 'B':
    return 11;
  default:
    return UINT8_MAX;
  }
}

static ORCA_FORCE_NO_INLINE U8 midi_velocity_of(Glyph g) {
  Usz n = index_of(g);
  // scale [0,9] to [0,127]
  if (n < 10)
    return (U8)(n * 14 + 1);
  n -= 10;
  // scale [0,25] to [0,127]
  // js seems to send 1 when original n is < 10, and 0 when n is 11. Is that
  // the intended behavior?
  if (n == 0)
    return UINT8_C(0);
  if (n >= 26)
    return UINT8_C(127);
  return (U8)(n * 5 - 3);
}

typedef struct {
  Glyph* vars_slots;
  Piano_bits piano_bits;
  Oevent_list* oevent_list;
  Usz random_seed;
} Oper_extra_params;

static void oper_poke_and_stun(Glyph* restrict gbuffer, Mark* restrict mbuffer,
                               Usz height, Usz width, Usz y, Usz x, Isz delta_y,
                               Isz delta_x, Glyph g) {
  Isz y0 = (Isz)y + delta_y;
  Isz x0 = (Isz)x + delta_x;
  if (y0 < 0 || x0 < 0 || (Usz)y0 >= height || (Usz)x0 >= width)
    return;
  Usz offs = (Usz)y0 * width + (Usz)x0;
  gbuffer[offs] = g;
  mbuffer[offs] |= Mark_flag_sleep;
}

#define OPER_FUNCTION_ATTRIBS ORCA_FORCE_NO_INLINE static void

#define BEGIN_OPERATOR(_oper_name)                                             \
  OPER_FUNCTION_ATTRIBS oper_behavior_##_oper_name(                            \
      Glyph* const restrict gbuffer, Mark* const restrict mbuffer,             \
      Usz const height, Usz const width, Usz const y, Usz const x,             \
      Usz Tick_number, Oper_extra_params* const extra_params,                  \
      Mark const cell_flags, Glyph const This_oper_char) {                     \
    (void)gbuffer;                                                             \
    (void)mbuffer;                                                             \
    (void)height;                                                              \
    (void)width;                                                               \
    (void)y;                                                                   \
    (void)x;                                                                   \
    (void)Tick_number;                                                         \
    (void)extra_params;                                                        \
    (void)cell_flags;                                                          \
    (void)This_oper_char;

#define END_OPERATOR }

#define PEEK(_delta_y, _delta_x)                                               \
  gbuffer_peek_relative(gbuffer, height, width, y, x, _delta_y, _delta_x)
#define POKE(_delta_y, _delta_x, _glyph)                                       \
  gbuffer_poke_relative(gbuffer, height, width, y, x, _delta_y, _delta_x,      \
                        _glyph)
#define STUN(_delta_y, _delta_x)                                               \
  mbuffer_poke_relative_flags_or(mbuffer, height, width, y, x, _delta_y,       \
                                 _delta_x, Mark_flag_sleep)
#define POKE_STUNNED(_delta_y, _delta_x, _glyph)                               \
  oper_poke_and_stun(gbuffer, mbuffer, height, width, y, x, _delta_y,          \
                     _delta_x, _glyph)
#define LOCK(_delta_y, _delta_x)                                               \
  mbuffer_poke_relative_flags_or(mbuffer, height, width, y, x, _delta_y,       \
                                 _delta_x, Mark_flag_lock)

#define IN Mark_flag_input
#define OUT Mark_flag_output
#define NONLOCKING Mark_flag_lock
#define PARAM Mark_flag_haste_input

#define LOWERCASE_REQUIRES_BANG                                                \
  if (glyph_is_lowercase(This_oper_char) &&                                    \
      !oper_has_neighboring_bang(gbuffer, height, width, y, x))                \
  return

#define STOP_IF_NOT_BANGED                                                     \
  if (!oper_has_neighboring_bang(gbuffer, height, width, y, x))                \
  return

#define PORT(_delta_y, _delta_x, _flags)                                       \
  mbuffer_poke_relative_flags_or(mbuffer, height, width, y, x, _delta_y,       \
                                 _delta_x, (_flags) ^ Mark_flag_lock)
//////// Operators

#define UNIQUE_OPERATORS(_)                                                    \
  _('!', keys)                                                                 \
  _('#', comment)                                                              \
  _('*', bang)                                                                 \
  _(':', midi)                                                                 \
  _(';', udp)                                                                  \
  _('=', osc)

#define ALPHA_OPERATORS(_)                                                     \
  _('A', add)                                                                  \
  _('B', bounce)                                                               \
  _('C', clock)                                                                \
  _('D', delay)                                                                \
  _('E', movement)                                                             \
  _('F', if)                                                                   \
  _('G', generator)                                                            \
  _('H', halt)                                                                 \
  _('I', increment)                                                            \
  _('J', jump)                                                                 \
  _('K', konkat)                                                               \
  _('L', lesser)                                                               \
  _('M', multiply)                                                             \
  _('N', movement)                                                             \
  _('O', offset)                                                               \
  _('P', push)                                                                 \
  _('Q', query)                                                                \
  _('R', random)                                                               \
  _('S', movement)                                                             \
  _('T', track)                                                                \
  _('U', uclid)                                                                \
  _('V', variable)                                                             \
  _('W', movement)                                                             \
  _('X', teleport)                                                             \
  _('Y', yump)                                                                 \
  _('Z', lerp)

#define MOVEMENT_CASES                                                         \
  'N' : case 'n' : case 'E' : case 'e' : case 'S' : case 's' : case 'W'        \
      : case 'w'

BEGIN_OPERATOR(movement)
  if (glyph_is_lowercase(This_oper_char) &&
      !oper_has_neighboring_bang(gbuffer, height, width, y, x))
    return;
  Isz delta_y, delta_x;
  switch (glyph_lowered_unsafe(This_oper_char)) {
  case 'n':
    delta_y = -1;
    delta_x = 0;
    break;
  case 'e':
    delta_y = 0;
    delta_x = 1;
    break;
  case 's':
    delta_y = 1;
    delta_x = 0;
    break;
  case 'w':
    delta_y = 0;
    delta_x = -1;
    break;
  default:
    // could cause strict aliasing problem, maybe
    delta_y = 0;
    delta_x = 0;
    break;
  }
  Isz y0 = (Isz)y + delta_y;
  Isz x0 = (Isz)x + delta_x;
  if (y0 >= (Isz)height || x0 >= (Isz)width || y0 < 0 || x0 < 0) {
    gbuffer[y * width + x] = '*';
    return;
  }
  Glyph* restrict g_at_dest = gbuffer + (Usz)y0 * width + (Usz)x0;
  if (*g_at_dest == '.') {
    *g_at_dest = This_oper_char;
    gbuffer[y * width + x] = '.';
    mbuffer[(Usz)y0 * width + (Usz)x0] |= Mark_flag_sleep;
  } else {
    gbuffer[y * width + x] = '*';
  }
END_OPERATOR

BEGIN_OPERATOR(keys)
  PORT(0, 1, IN);
  PORT(1, 0, OUT);
  Glyph g = PEEK(0, 1);
  Piano_bits pb = piano_bits_of(g);
  // instead of this extra branch, could maybe just leave output port unlocked
  // so the '*' goes away on its own?
  if (pb == ORCA_PIANO_BITS_NONE)
    return;
  Glyph o;
  if (ORCA_LIKELY((pb & extra_params->piano_bits) == ORCA_PIANO_BITS_NONE))
    o = '.';
  else
    o = '*';
  POKE(1, 0, o);
END_OPERATOR

BEGIN_OPERATOR(comment)
  // restrict probably ok here...
  Glyph const* restrict gline = gbuffer + y * width;
  Mark* restrict mline = mbuffer + y * width;
  Usz max_x = x + 255;
  if (width < max_x)
    max_x = width;
  for (Usz x0 = x + 1; x0 < max_x; ++x0) {
    Glyph g = gline[x0];
    mline[x0] |= (Mark)Mark_flag_lock;
    if (g == '#')
      break;
  }
END_OPERATOR

BEGIN_OPERATOR(bang)
  gbuffer_poke(gbuffer, height, width, y, x, '.');
END_OPERATOR

BEGIN_OPERATOR(midi)
  for (Usz i = 1; i < 6; ++i) {
    PORT(0, (Isz)i, IN);
  }
  STOP_IF_NOT_BANGED;
  Glyph channel_g = PEEK(0, 1);
  Glyph octave_g = PEEK(0, 2);
  Glyph note_g = PEEK(0, 3);
  Glyph velocity_g = PEEK(0, 4);
  Glyph length_g = PEEK(0, 5);
  U8 octave_num = (U8)index_of(octave_g);
  if (octave_num == 0)
    return;
  if (octave_num > 9)
    octave_num = 9;
  U8 note_num = midi_note_number_of(note_g);
  if (note_num == UINT8_MAX)
    return;
  Usz channel_num = index_of(channel_g);
  if (channel_num > 15)
    channel_num = 15;
  Oevent_midi* oe =
      (Oevent_midi*)oevent_list_alloc_item(extra_params->oevent_list);
  oe->oevent_type = (U8)Oevent_type_midi;
  oe->channel = (U8)channel_num;
  oe->octave = octave_num;
  oe->note = note_num;
  oe->velocity = midi_velocity_of(velocity_g);
  oe->bar_divisor = (U8)(index_of(length_g) + 1);
END_OPERATOR

BEGIN_OPERATOR(udp)
  Usz n = width - x - 1;
  if (n > 16)
    n = 16;
  Glyph const* restrict gline = gbuffer + y * width + x + 1;
  Mark* restrict mline = mbuffer + y * width + x + 1;
  Glyph cpy[Oevent_udp_string_count];
  Usz i;
  for (i = 0; i < n; ++i) {
    Glyph g = gline[i];
    if (g == '.')
      break;
    cpy[i] = g;
    mline[i] |= Mark_flag_lock;
  }
  n = i;
  STOP_IF_NOT_BANGED;
  Oevent_udp_string* oe =
      (Oevent_udp_string*)oevent_list_alloc_item(extra_params->oevent_list);
  oe->oevent_type = (U8)Oevent_type_udp_string;
  oe->count = (U8)n;
  for (i = 0; i < n; ++i) {
    oe->chars[i] = cpy[i];
  }
END_OPERATOR

BEGIN_OPERATOR(osc)
  PORT(0, 1, IN | PARAM);
  PORT(0, 2, IN | PARAM);
  Usz len = index_of(PEEK(0, 2));
  if (len > Oevent_osc_int_count)
    len = Oevent_osc_int_count;
  for (Usz i = 0; i < len; ++i) {
    PORT(0, (Isz)i + 3, IN);
  }
  STOP_IF_NOT_BANGED;
  Glyph g = PEEK(0, 1);
  if (g != '.') {
    U8 buff[Oevent_osc_int_count];
    for (Usz i = 0; i < len; ++i) {
      buff[i] = (U8)index_of(PEEK(0, (Isz)i + 3));
    }
    Oevent_osc_ints* oe =
        &oevent_list_alloc_item(extra_params->oevent_list)->osc_ints;
    oe->oevent_type = (U8)Oevent_type_osc_ints;
    oe->glyph = g;
    oe->count = (U8)len;
    for (Usz i = 0; i < len; ++i) {
      oe->numbers[i] = buff[i];
    }
  }
END_OPERATOR

BEGIN_OPERATOR(add)
  LOWERCASE_REQUIRES_BANG;
  PORT(0, -1, IN | PARAM);
  PORT(0, 1, IN);
  PORT(1, 0, OUT);
  Usz a = index_of(PEEK(0, -1));
  Usz b = index_of(PEEK(0, 1));
  POKE(1, 0, indexed_glyphs[(a + b) % Glyphs_index_count]);
END_OPERATOR

BEGIN_OPERATOR(bounce)
  LOWERCASE_REQUIRES_BANG;
  PORT(0, -1, IN | PARAM);
  PORT(0, 1, IN);
  PORT(1, 0, OUT);
  Isz a = (Isz)index_of(PEEK(0, -1));
  Isz b = (Isz)index_of(PEEK(0, 1));
  Isz val = b - a;
  if (val < 0)
    val = -val;
  POKE(1, 0, glyph_of((Usz)val));
END_OPERATOR

BEGIN_OPERATOR(clock)
  LOWERCASE_REQUIRES_BANG;
  PORT(0, -1, IN | PARAM);
  PORT(0, 1, IN);
  PORT(1, 0, OUT);
  Usz rate = index_of(PEEK(0, -1));
  Usz mod_num = index_of(PEEK(0, 1));
  if (rate == 0)
    rate = 1;
  if (mod_num == 0)
    mod_num = 8;
  Glyph g = glyph_of(Tick_number / rate % mod_num);
  POKE(1, 0, g);
END_OPERATOR

BEGIN_OPERATOR(delay)
  LOWERCASE_REQUIRES_BANG;
  PORT(0, -1, IN | PARAM);
  PORT(0, 1, IN);
  PORT(1, 0, OUT);
  Usz rate = index_of(PEEK(0, -1));
  Usz mod_num = index_of(PEEK(0, 1));
  if (rate == 0)
    rate = 1;
  if (mod_num == 0)
    mod_num = 8;
  Glyph g = Tick_number % (rate * mod_num) == 0 ? '*' : '.';
  POKE(1, 0, g);
END_OPERATOR

BEGIN_OPERATOR(if)
  LOWERCASE_REQUIRES_BANG;
  PORT(0, -1, IN | PARAM);
  PORT(0, 1, IN);
  PORT(1, 0, OUT);
  Glyph g0 = PEEK(0, -1);
  Glyph g1 = PEEK(0, 1);
  POKE(1, 0, g0 == g1 ? '*' : '.');
END_OPERATOR

BEGIN_OPERATOR(generator)
  LOWERCASE_REQUIRES_BANG;
  Isz out_x = (Isz)index_of(PEEK(0, -3));
  Isz out_y = (Isz)index_of(PEEK(0, -2)) + 1;
  Isz len = (Isz)index_of(PEEK(0, -1));
  PORT(0, -3, IN | PARAM); // x
  PORT(0, -2, IN | PARAM); // y
  PORT(0, -1, IN | PARAM); // len
  for (Isz i = 0; i < len; ++i) {
    PORT(0, i + 1, IN);
    PORT(out_y, out_x + i, OUT | NONLOCKING);
    Glyph g = PEEK(0, i + 1);
    POKE_STUNNED(out_y, out_x + i, g);
  }
END_OPERATOR

BEGIN_OPERATOR(halt)
  LOWERCASE_REQUIRES_BANG;
  PORT(1, 0, OUT);
END_OPERATOR

BEGIN_OPERATOR(increment)
  LOWERCASE_REQUIRES_BANG;
  PORT(0, -1, IN | PARAM);
  PORT(0, 1, IN);
  PORT(1, 0, IN | OUT);
  Glyph g = PEEK(0, -1);
  Usz rate = 1;
  if (g != '.' && g != '*')
    rate = index_of(g);
  Usz max = index_of(PEEK(0, 1));
  Usz val = index_of(PEEK(1, 0));
  if (max == 0)
    max = 36;
  val = val + rate;
  val = val % max;
  POKE(1, 0, glyph_of(val));
END_OPERATOR

BEGIN_OPERATOR(jump)
  LOWERCASE_REQUIRES_BANG;
  PORT(-1, 0, IN);
  PORT(1, 0, OUT);
  POKE(1, 0, PEEK(-1, 0));
END_OPERATOR

// Note: this is merged from a pull request without being fully tested or
// optimized
BEGIN_OPERATOR(konkat)
  LOWERCASE_REQUIRES_BANG;
  Isz len = (Isz)index_of(PEEK(0, -1));
  if (len == 0)
    len = 1;
  PORT(0, -1, IN | PARAM);
  for (Isz i = 0; i < len; ++i) {
    PORT(0, i + 1, IN);
    Glyph var = PEEK(0, i + 1);
    if (var != '.') {
      Usz var_idx = safe_index_of(var);
      if (var_idx != 0) {
        Glyph result = extra_params->vars_slots[var_idx];
        PORT(1, i + 1, OUT);
        POKE(1, i + 1, result);
      }
    }
  }
END_OPERATOR

BEGIN_OPERATOR(lesser)
  LOWERCASE_REQUIRES_BANG;
  PORT(0, -1, IN | PARAM);
  PORT(0, 1, IN);
  PORT(1, 0, OUT);
  Glyph ga = PEEK(0, -1);
  Glyph gb = PEEK(0, 1);
  if (ga == '.' || gb == '.') {
    POKE(1, 0, '.');
  } else {
    Usz a = index_of(ga);
    Usz b = index_of(gb);
    Usz out = (a < b) ? a : b;
    POKE(1, 0, indexed_glyphs[out]);
  }
END_OPERATOR

BEGIN_OPERATOR(multiply)
  LOWERCASE_REQUIRES_BANG;
  PORT(0, -1, IN | PARAM);
  PORT(0, 1, IN);
  PORT(1, 0, OUT);
  Usz ia = index_of(PEEK(0, -1));
  Usz ib = index_of(PEEK(0, 1));
  POKE(1, 0, indexed_glyphs[(ia * ib) % Glyphs_index_count]);
END_OPERATOR

BEGIN_OPERATOR(offset)
  LOWERCASE_REQUIRES_BANG;
  Isz in_x = (Isz)index_of(PEEK(0, -2)) + 1;
  Isz in_y = (Isz)index_of(PEEK(0, -1));
  PORT(0, -1, IN | PARAM);
  PORT(0, -2, IN | PARAM);
  PORT(in_y, in_x, IN);
  PORT(1, 0, OUT);
  POKE(1, 0, PEEK(in_y, in_x));
END_OPERATOR

BEGIN_OPERATOR(push)
  LOWERCASE_REQUIRES_BANG;
  Usz key = index_of(PEEK(0, -2));
  Usz len = index_of(PEEK(0, -1));
  PORT(0, -1, IN | PARAM);
  PORT(0, -2, IN | PARAM);
  PORT(0, 1, IN);
  if (len == 0)
    return;
  Isz out_x = (Isz)(key % len);
  for (Usz i = 0; i < len; ++i) {
    LOCK(1, (Isz)i);
  }
  PORT(1, out_x, OUT);
  POKE(1, out_x, PEEK(0, 1));
END_OPERATOR

BEGIN_OPERATOR(query)
  LOWERCASE_REQUIRES_BANG;
  Isz in_x = (Isz)index_of(PEEK(0, -3)) + 1;
  Isz in_y = (Isz)index_of(PEEK(0, -2));
  Isz len = (Isz)index_of(PEEK(0, -1));
  Isz out_x = 1 - len;
  PORT(0, -3, IN | PARAM); // x
  PORT(0, -2, IN | PARAM); // y
  PORT(0, -1, IN | PARAM); // len
  // todo direct buffer manip
  for (Isz i = 0; i < len; ++i) {
    PORT(in_y, in_x + i, IN);
    PORT(1, out_x + i, OUT);
    Glyph g = PEEK(in_y, in_x + i);
    POKE(1, out_x + i, g);
  }
END_OPERATOR

static Usz hash32_shift_mult(Usz key) {
  Usz c2 = UINT32_C(0x27d4eb2d);
  key = (key ^ UINT32_C(61)) ^ (key >> UINT32_C(16));
  key = key + (key << UINT32_C(3));
  key = key ^ (key >> UINT32_C(4));
  key = key * c2;
  key = key ^ (key >> UINT32_C(15));
  return key;
}

BEGIN_OPERATOR(random)
  LOWERCASE_REQUIRES_BANG;
  PORT(0, -1, IN | PARAM);
  PORT(0, 1, IN);
  PORT(1, 0, OUT);
  Usz a = index_of(PEEK(0, -1));
  Usz b = index_of(PEEK(0, 1));
  if (b == 0)
    b = 36;
  Usz min, max;
  if (a == b) {
    POKE(1, 0, glyph_of(a));
    return;
  } else if (a < b) {
    min = a;
    max = b;
  } else {
    min = b;
    max = a;
  }
  Usz key = hash32_shift_mult((extra_params->random_seed + y * width + x) ^
                              (Tick_number << UINT32_C(16)));
  Usz val = key % (max - min) + min;
  POKE(1, 0, glyph_of(val));
END_OPERATOR

BEGIN_OPERATOR(track)
  LOWERCASE_REQUIRES_BANG;
  Usz key = index_of(PEEK(0, -2));
  Usz len = index_of(PEEK(0, -1));
  PORT(0, -2, IN | PARAM);
  PORT(0, -1, IN | PARAM);
  if (len == 0)
    return;
  Isz read_val_x = (Isz)(key % len) + 1;
  for (Usz i = 0; i < len; ++i) {
    LOCK(0, (Isz)(i + 1));
  }
  PORT(0, (Isz)read_val_x, IN);
  PORT(1, 0, OUT);
  POKE(1, 0, PEEK(0, read_val_x));
END_OPERATOR

// optimized starting from from
// https://www.computermusicdesign.com/simplest-euclidean-rhythm-algorithm-explained/
BEGIN_OPERATOR(uclid)
  LOWERCASE_REQUIRES_BANG;
  PORT(0, -1, IN | PARAM);
  PORT(0, 1, IN);
  PORT(1, 0, OUT);
  Glyph left = PEEK(0, -1);
  Usz steps = 1;
  if (left != '.' && left != '*')
    steps = index_of(left);
  Usz max = index_of(PEEK(0, 1));
  if (max == 0)
    max = 8;
  Usz bucket = (steps * (Tick_number + max - 1)) % max + steps;
  Glyph g = (bucket >= max) ? '*' : '.';
  POKE(1, 0, g);
END_OPERATOR

BEGIN_OPERATOR(variable)
  LOWERCASE_REQUIRES_BANG;
  PORT(0, -1, IN | PARAM);
  PORT(0, 1, IN | PARAM);
  Glyph left = PEEK(0, -1);
  Glyph right = PEEK(0, 1);
  if (left != '.') {
    // Write
    Usz var_idx = safe_index_of(left);
    extra_params->vars_slots[var_idx] = right;
  } else if (right != '.') {
    // Read
    PORT(1, 0, OUT);
    Usz var_idx = safe_index_of(right);
    Glyph result = extra_params->vars_slots[var_idx];
    POKE(1, 0, result);
  }
END_OPERATOR

BEGIN_OPERATOR(teleport)
  LOWERCASE_REQUIRES_BANG;
  Isz out_x = (Isz)index_of(PEEK(0, -2));
  Isz out_y = (Isz)index_of(PEEK(0, -1)) + 1;
  PORT(0, -2, IN | PARAM); // x
  PORT(0, -1, IN | PARAM); // y
  PORT(0, 1, IN);
  PORT(out_y, out_x, OUT | NONLOCKING);
  POKE_STUNNED(out_y, out_x, PEEK(0, 1));
END_OPERATOR

BEGIN_OPERATOR(yump)
  LOWERCASE_REQUIRES_BANG;
  PORT(0, -1, IN);
  PORT(0, 1, OUT);
  POKE(0, 1, PEEK(0, -1));
END_OPERATOR

BEGIN_OPERATOR(lerp)
  LOWERCASE_REQUIRES_BANG;
  PORT(0, -1, IN | PARAM);
  PORT(0, 1, IN);
  PORT(1, 0, IN | OUT);
  Glyph g = PEEK(0, -1);
  Isz rate = 1;
  if (g != '.' && g != '*')
    rate = (Isz)index_of(g);
  Isz target = (Isz)index_of(PEEK(0, 1));
  Isz val = (Isz)index_of(PEEK(1, 0));
  Isz mod = (val <= target - rate)
                ? rate
                : ((val >= target + rate) ? -rate : target - val);
  POKE(1, 0, glyph_of((Usz)(val + mod)));
END_OPERATOR

//////// Run simulation

void orca_run(Glyph* restrict gbuf, Mark* restrict mbuf, Usz height, Usz width,
              Usz tick_number, Oevent_list* oevent_list, Piano_bits piano_bits,
              Usz random_seed) {
  Glyph vars_slots[Glyphs_index_count];
  memset(vars_slots, '.', sizeof(vars_slots));
  mbuffer_clear(mbuf, height, width);
  oevent_list_clear(oevent_list);
  Oper_extra_params extras;
  extras.vars_slots = &vars_slots[0];
  extras.piano_bits = piano_bits;
  extras.oevent_list = oevent_list;
  extras.random_seed = random_seed;

  for (Usz iy = 0; iy < height; ++iy) {
    Glyph const* glyph_row = gbuf + iy * width;
    Mark const* mark_row = mbuf + iy * width;
    for (Usz ix = 0; ix < width; ++ix) {
      Glyph glyph_char = glyph_row[ix];
      if (ORCA_LIKELY(glyph_char == '.'))
        continue;
      Mark cell_flags = mark_row[ix] & (Mark_flag_lock | Mark_flag_sleep);
      if (cell_flags & (Mark_flag_lock | Mark_flag_sleep))
        continue;
      switch (glyph_char) {
#define UNIQUE_CASE(_oper_char, _oper_name)                                    \
  case _oper_char:                                                             \
    oper_behavior_##_oper_name(gbuf, mbuf, height, width, iy, ix, tick_number, \
                               &extras, cell_flags, glyph_char);               \
    break;

#define ALPHA_CASE(_upper_oper_char, _oper_name)                               \
  case _upper_oper_char:                                                       \
  case ((char)(_upper_oper_char | (1 << 5))):                                  \
    oper_behavior_##_oper_name(gbuf, mbuf, height, width, iy, ix, tick_number, \
                               &extras, cell_flags, glyph_char);               \
    break;
        UNIQUE_OPERATORS(UNIQUE_CASE)
        ALPHA_OPERATORS(ALPHA_CASE)
#undef UNIQUE_CASE
#undef ALPHA_CASE
      }
    }
  }
}
