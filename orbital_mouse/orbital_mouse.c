// Copyright 2023-2026 Google LLC
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//     https://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

/**
 * @file orbital_mouse.c
 * @brief Orbital Mouse community module implementation
 *
 * For documentation, see
 * <https://getreuer.info/posts/keyboards/orbital-mouse>
 */

#include "orbital_mouse.h"

#ifndef ORBITAL_MOUSE_RADIUS
#define ORBITAL_MOUSE_RADIUS 36
#endif  // ORBITAL_MOUSE_RADIUS
#ifndef ORBITAL_MOUSE_SLOW_MOVE_FACTOR
#define ORBITAL_MOUSE_SLOW_MOVE_FACTOR 0.333
#endif  // ORBITAL_MOUSE_SLOW_MOVE_FACTOR
#ifndef ORBITAL_MOUSE_SLOW_TURN_FACTOR
#define ORBITAL_MOUSE_SLOW_TURN_FACTOR 0.5
#endif  // ORBITAL_MOUSE_SLOW_TURN_FACTOR
#ifndef ORBITAL_MOUSE_WHEEL_SPEED
#define ORBITAL_MOUSE_WHEEL_SPEED 0.2
#endif  // ORBITAL_MOUSE_WHEEL_SPEED
#ifndef ORBITAL_MOUSE_WHEEL_TAP_STEP
#define ORBITAL_MOUSE_WHEEL_TAP_STEP 1.0
#endif  // ORBITAL_MOUSE_WHEEL_TAP_STEP
#ifndef ORBITAL_MOUSE_FAST_MOVE_FACTOR
#define ORBITAL_MOUSE_FAST_MOVE_FACTOR 3.0
#endif  // ORBITAL_MOUSE_SLOW_MOVE_FACTOR
#ifndef ORBITAL_MOUSE_FAST_TURN_FACTOR
#define ORBITAL_MOUSE_FAST_TURN_FACTOR 2.0
#endif  // ORBITAL_MOUSE_SLOW_TURN_FACTOR
#ifndef ORBITAL_MOUSE_DBL_DELAY_MS
#define ORBITAL_MOUSE_DBL_DELAY_MS 50
#endif  // ORBITAL_MOUSE_DBL_DELAY_MS
#ifdef ORBITAL_MOUSE_FLAT_SPEED

#ifdef ORBITAL_MOUSE_SPEED_CURVE
#warning "Ignoring ORBITAL_MOUSE_SPEED_CURVE, since ORBITAL_MOUSE_FLAT_SPEED is also defined."
#undef ORBITAL_MOUSE_SPEED_CURVE
#endif // ORBITAL_MOUSE_SPEED_CURVE

#define ORBITAL_MOUSE_SPEED_CURVE \
      {ORBITAL_MOUSE_FLAT_SPEED, ORBITAL_MOUSE_FLAT_SPEED, ORBITAL_MOUSE_FLAT_SPEED, ORBITAL_MOUSE_FLAT_SPEED, \
       ORBITAL_MOUSE_FLAT_SPEED, ORBITAL_MOUSE_FLAT_SPEED, ORBITAL_MOUSE_FLAT_SPEED, ORBITAL_MOUSE_FLAT_SPEED, \
       ORBITAL_MOUSE_FLAT_SPEED, ORBITAL_MOUSE_FLAT_SPEED, ORBITAL_MOUSE_FLAT_SPEED, ORBITAL_MOUSE_FLAT_SPEED, \
       ORBITAL_MOUSE_FLAT_SPEED, ORBITAL_MOUSE_FLAT_SPEED, ORBITAL_MOUSE_FLAT_SPEED, ORBITAL_MOUSE_FLAT_SPEED}
#endif  // ORBITAL_MOUSE_FLAT_SPEED
#ifndef ORBITAL_MOUSE_SPEED_CURVE
#define ORBITAL_MOUSE_SPEED_CURVE \
      {24, 24, 24, 32, 58, 66, 66, 66, 66, 66, 66, 66, 66, 66, 66, 66}
//     |               |               |               |           |
// t = 0.000           1.024           2.048           3.072       3.840 s
#endif  // ORBITAL_MOUSE_SPEED_CURVE
#ifndef ORBITAL_MOUSE_INTERVAL_MS
#define ORBITAL_MOUSE_INTERVAL_MS 16
#endif  // ORBITAL_MOUSE_INTERVAL_MS

#if !(0 <= ORBITAL_MOUSE_RADIUS && ORBITAL_MOUSE_RADIUS <= 63)
#error "Invalid ORBITAL_MOUSE_RADIUS. Value must be in [0, 63]."
#endif

enum {
  /** Number of distinct angles. */
  NUM_ANGLES = 64,
  /** Number of intervals in speed curve table. */
  NUM_SPEED_CURVE_INTERVALS = 16,
  /** Orbit radius in pixels as a Q6.2 value. */
  RADIUS_Q6_2 = (uint8_t)((ORBITAL_MOUSE_RADIUS) * 4 + 0.5),
  /** Slow mode movement speed factor as a Q.8 value. */
  SLOW_MOVE_FACTOR_Q_8 = (ORBITAL_MOUSE_SLOW_MOVE_FACTOR) < 0.99
      ? ((uint8_t)((ORBITAL_MOUSE_SLOW_MOVE_FACTOR) * 256 + 0.5)) : 255,
  /** Slow mode turn speed factor as a Q.8 value. */
  SLOW_TURN_FACTOR_Q_8 = (ORBITAL_MOUSE_SLOW_TURN_FACTOR) < 0.99
      ? ((uint8_t)((ORBITAL_MOUSE_SLOW_TURN_FACTOR) * 256 + 0.5)) : 255,
  /** Fast mode movement speed factor as a Q4.4 value. */
  FAST_MOVE_FACTOR_Q4_4 = (ORBITAL_MOUSE_FAST_MOVE_FACTOR) < 15.999
      ? ((uint8_t)((ORBITAL_MOUSE_FAST_MOVE_FACTOR) * 16 + 0.5)) : 255,
  /** Fast mode turn speed factor as a Q4.4 value. */
  FAST_TURN_FACTOR_Q4_4 = (ORBITAL_MOUSE_FAST_TURN_FACTOR) < 15.99
      ? ((uint8_t)((ORBITAL_MOUSE_FAST_TURN_FACTOR) * 16 + 0.5)) : 255,
  /** Wheel speed in steps/frame as a Q2.6 value. */
  WHEEL_SPEED_Q2_6 = (ORBITAL_MOUSE_WHEEL_SPEED) < 3.99
      ? ((uint8_t)((ORBITAL_MOUSE_WHEEL_SPEED) * 64 + 0.5)) : 255,
  /** Wheel movement emitted for a tap as a Q2.6 value. */
  WHEEL_TAP_STEP_Q2_6 = (ORBITAL_MOUSE_WHEEL_TAP_STEP) < 3.99
      ? ((uint8_t)((ORBITAL_MOUSE_WHEEL_TAP_STEP) * 64 + 0.5)) : 255,
  /** Double click delay in units of intervals. */
  DOUBLE_CLICK_DELAY_INTERVALS =
      (ORBITAL_MOUSE_DBL_DELAY_MS) / (ORBITAL_MOUSE_INTERVAL_MS),
};

// Masks for the `held_keys` bitfield.
enum {
  HELD_U = 1,
  HELD_D = 2,
  HELD_L = 4,
  HELD_R = 8,
  HELD_W_U = 16,
  HELD_W_D = 32,
  HELD_W_L = 64,
  HELD_W_R = 128,
  /** Mask of the wheel direction bits. */
  WHEEL_MASK = HELD_W_U | HELD_W_D | HELD_W_L | HELD_W_R,
};

static const uint8_t init_speed_curve[NUM_SPEED_CURVE_INTERVALS] =
  ORBITAL_MOUSE_SPEED_CURVE;
static struct {
  report_mouse_t report;
  // Current speed curve, should point to a table of 16 values.
  const uint8_t* speed_curve;
  // Time when the Orbital Mouse task function should next run.
  uint16_t timer;
  // Fractional displacement of the cursor as Q7.8 values.
  int16_t x;
  int16_t y;
  // Fractional displacement of the mouse wheel as Q9.6 values.
  int16_t wheel_x;
  int16_t wheel_y;
  // Current cursor movement speed as a Q9.6 value.
  int16_t speed;
  // Bitfield tracking which movement keys are currently held.
  uint8_t held_keys;
  // Bitfield tracking which cardinal movement keys are held.
  uint8_t held_card_keys;
  // Cursor movement time, counted in number of intervals.
  uint8_t move_t;
  // Cursor movement direction, 1 => forward, -1 => backward.
  int8_t move_dir;
  // Steering direction, 1 => counter-clockwise, -1 => clockwise.
  int8_t steer_dir;
  // Mouse wheel movement directions.
  int8_t wheel_x_dir;
  int8_t wheel_y_dir;
  // Wheel direction bits whose press has already scrolled a whole step.
  uint8_t wheel_stepped;
  // Heading direction as a Q6.8 value, with 0 => up, 16 * 256 => left, etc.
  uint16_t angle;
  // Selected mouse button as a base-0 index.
  uint8_t selected_button;
  // Tracks double click action.
  uint8_t double_click_frame;
  // When true, movement and turning are slower.
  bool slow;
  // When true, movement and turning are faster.
  bool fast;
  // if slow and fast are both true then fast is ignored.
} state = {.speed_curve = init_speed_curve};

/**
 * Fixed-point sine with specified amplitude and phase.
 *
 * @param amplitude Nonnegative Q6.2 value.
 * @param phase Value in [0, 63].
 * @returns Result as a Q6.8 value.
 */
static int16_t scaled_sin(uint8_t amplitude, uint8_t phase) {
  // Look up table covers half a cycle of a sine wave.
  static const uint8_t lut[NUM_ANGLES / 2] PROGMEM = {
      0,   25,  50,  74,  98,  120, 142, 162, 180, 197, 212,
      225, 236, 244, 250, 254, 255, 254, 250, 244, 236, 225,
      212, 197, 180, 162, 142, 120, 98,  74,  50,  25};
  // amplitude Q6.2 and lut is Q0.8. Shift down by 2 so that the result is Q6.8.
  int16_t value = (int16_t)(((uint16_t)amplitude
        * pgm_read_byte(lut + (phase & (NUM_ANGLES / 2 - 1))) + 2) >> 2);
  return ((NUM_ANGLES / 2) & phase) == 0 ? value : -value;
}

/** Computes fixed-point cosine. */
static int16_t scaled_cos(uint8_t amplitude, uint8_t phase) {
  return scaled_sin(amplitude, phase + (NUM_ANGLES / 4));
}

/** Wakes the Orbital Mouse task.  */
static void wake_orbital_mouse_task(void) {
  if (!state.timer) {
    state.timer = timer_read() | 1;
  }
}

/** Converts a keycode to a mask for  the `held_keys` bitfield. */
static uint8_t keycode_to_held_mask(uint16_t keycode) {
  switch (keycode) {
    case OM_U: return HELD_U;
    case OM_D: return HELD_D;
    case OM_L: return HELD_L;
    case OM_R: return HELD_R;
    case OM_W_U: return HELD_W_U;
    case OM_W_D: return HELD_W_D;
    case OM_W_L: return HELD_W_L;
    case OM_W_R: return HELD_W_R;
  }
  return 0;
}

/** Presses mouse button i, with i being a base-0 index. */
static void press_mouse_button(uint8_t i, bool pressed) {
  if (i >= 8) {
    i = state.selected_button;
  }
  const uint8_t mask = 1 << i;
  if (pressed) {
    state.report.buttons |= mask;
  } else {
    state.report.buttons &= ~mask;
  }
  wake_orbital_mouse_task();
}

/** Selects mouse button i. */
static void select_mouse_button(uint8_t i) {
  state.selected_button = i;
  // Reset buttons and double-click state when switching selection.
  state.report.buttons = 0;
  state.double_click_frame = 0;
  wake_orbital_mouse_task();
}

static int8_t get_dir_from_held_keys(uint8_t bit_shift) {
  static const int8_t dir[4] = {0, 1, -1, 0};
  return dir[(state.held_keys >> bit_shift) & 3];
}

static uint8_t get_card_angle_from_held_keys(void) {
  static const uint8_t card_angles[16] PROGMEM = {
    // Zero values in this array represent "no movement."
    [HELD_U]          = 0x80, // Up. Set high bit to distinguish from zero.
    [HELD_U | HELD_L] = 1 * (NUM_ANGLES / 8), // Up+Left.
    [HELD_L]          = 2 * (NUM_ANGLES / 8), // Left.
    [HELD_D | HELD_L] = 3 * (NUM_ANGLES / 8), // Down+Left.
    [HELD_D]          = 4 * (NUM_ANGLES / 8), // Down.
    [HELD_D | HELD_R] = 5 * (NUM_ANGLES / 8), // Down+Right.
    [HELD_R]          = 6 * (NUM_ANGLES / 8), // Right.
    [HELD_U | HELD_R] = 7 * (NUM_ANGLES / 8), // Up+Right.
  };
  return pgm_read_byte(card_angles + state.held_card_keys);
}

void set_orbital_mouse_speed_curve(const uint8_t* speed_curve) {
  state.speed_curve = (speed_curve != NULL) ? speed_curve : init_speed_curve;
}

uint8_t get_orbital_mouse_angle(void) {
  return (state.angle >> 8) & (NUM_ANGLES - 1);
}

static void set_orbital_mouse_angle_fractional(uint16_t angle) {
  state.x += scaled_sin(RADIUS_Q6_2, state.angle >> 8);
  state.y += scaled_cos(RADIUS_Q6_2, state.angle >> 8);
  state.angle = angle;
  state.x -= scaled_sin(RADIUS_Q6_2, angle >> 8);
  state.y -= scaled_cos(RADIUS_Q6_2, angle >> 8);
  wake_orbital_mouse_task();
}

void set_orbital_mouse_angle(uint8_t angle) {
  set_orbital_mouse_angle_fractional((uint16_t)angle << 8);
}

// Adds one wheel step for the direction of the wheel key in `held_mask`.
static void add_wheel_step(uint8_t held_mask) {
  switch (held_mask) {
    case HELD_W_U: state.wheel_y += WHEEL_TAP_STEP_Q2_6; break;
    case HELD_W_D: state.wheel_y -= WHEEL_TAP_STEP_Q2_6; break;
    case HELD_W_L: state.wheel_x -= WHEEL_TAP_STEP_Q2_6; break;
    case HELD_W_R: state.wheel_x += WHEEL_TAP_STEP_Q2_6; break;
  }
}

// Sends a report with the whole parts of the accumulated deltas, retaining the
// fractional parts for the next update.
static void flush_mouse_report(void) {
  state.report.x = state.x / 256;
  state.report.y = state.y / 256;
  state.x -= (int16_t)state.report.x * 256;
  state.y -= (int16_t)state.report.y * 256;
  state.report.h = state.wheel_x / 64;
  state.report.v = state.wheel_y / 64;
  state.wheel_x -= (int16_t)state.report.h * 64;
  state.wheel_y -= (int16_t)state.report.v * 64;
  host_mouse_send(&state.report);
}

bool process_record_orbital_mouse(uint16_t keycode, keyrecord_t* record) {
  if (!(IS_MOUSE_KEYCODE(keycode) ||
        (OM_CS_U <= keycode && keycode <= OM_SEL8))) {
    return true;
  }

  uint8_t held_mask = keycode_to_held_mask(keycode);
  if (held_mask != 0) {
    if (held_mask & WHEEL_MASK) {
      if (IS_ENCODEREVENT(record->event)) {
        // A rotary encoder detent presses and releases the key within a single
        // task iteration, so the periodic wheel movement never observes the key
        // as held. Emit the step directly on the press, like a native mouse
        // wheel key does, so that each detent scrolls exactly one step.
        if (record->event.pressed) {
          add_wheel_step(held_mask);
          flush_mouse_report();
        }
      } else if (record->event.pressed) {
        state.wheel_stepped &= ~held_mask;
      } else if (!(state.wheel_stepped & held_mask)) {
        // The key was released before the periodic movement scrolled it, so this
        // press is a tap: emit one step, as a native mouse wheel key does.
        const uint8_t axis_mask = (held_mask & (HELD_W_U | HELD_W_D))
          ? (HELD_W_U | HELD_W_D)
          : (HELD_W_L | HELD_W_R);
        if (!(state.held_keys & (uint8_t)~held_mask & axis_mask)) {
          // Retain nothing of this press's fractional movement, so that
          // repeated taps each scroll exactly one step.
          if (held_mask & (HELD_W_U | HELD_W_D)) {
            state.wheel_y = 0;
          } else {
            state.wheel_x = 0;
          }
        }
        add_wheel_step(held_mask);
        flush_mouse_report();
      }
    }
    // Update `held_keys` bitfield.
    if (record->event.pressed) {
      state.held_keys |= held_mask;
    } else {
      state.held_keys &= ~held_mask;
    }
  } else if (OM_CS_U <= keycode && keycode <= OM_CS_R) {
    const uint8_t card_mask = 1 << (keycode - OM_CS_U);
    if (record->event.pressed) {
      state.held_card_keys |= card_mask;
    } else {
      state.held_card_keys &= ~card_mask;
    }
  } else {
    switch (keycode) {
      case OM_BTN1 ... OM_BTN8:
        press_mouse_button(keycode - OM_BTN1, record->event.pressed);
        return true;
      case OM_BTNS:
        press_mouse_button(255, record->event.pressed);
        return true;
      case OM_HLDS:
        if (record->event.pressed) {
          press_mouse_button(255, true);
        }
        return true;
      case OM_RELS:
        if (record->event.pressed) {
          press_mouse_button(255, false);
        }
        return true;
      case OM_DBLS:
        if (record->event.pressed) {
          state.double_click_frame = 1;
        }
        break;
      case OM_SLOW:
        state.slow = record->event.pressed;
        return true;
      case OM_FAST:
       state.fast = record->event.pressed;
        return true;
      case OM_SEL1 ... OM_SEL8:
        if (record->event.pressed) {
          select_mouse_button(keycode - OM_SEL1);
        }
        return true;
      // Check if cardinal snapping is desired
      case OM_CS_U:
        if (record->event.pressed) {
          state.angle = 0 << 8; //16*0
          state.steer_dir = 0;
          return true;
        }
      case OM_CS_L:
        if (record->event.pressed) {
          state.angle = 16 << 8; //16*1
          state.steer_dir = 0;
          return true;
        }
      case OM_CS_D:
        if (record->event.pressed) {
          state.angle = 32 << 8; //16*2
          state.steer_dir = 0;
          return true;
        }
      case OM_CS_R:
        if (record->event.pressed) {
          state.angle = 48 << 8; //16*3
          state.steer_dir = 0;
          return true;
        }
    }
  }

  int8_t move_dir = 0;

  // Update cursor movement direction.
  if (state.held_card_keys) {  // If any cardinal key is held.
    const uint8_t angle = get_card_angle_from_held_keys();  // Map to angle.
    if (angle) {  // If the held keys combination is valid.
      state.angle = (uint16_t)angle << 8;
      move_dir = 1;
    }
    state.steer_dir = 0;  // Freeze steering.
  } else {  // Otherwise, the default polar controls apply.
    move_dir = get_dir_from_held_keys(0);
    // Update steering direction.
    state.steer_dir = get_dir_from_held_keys(2);
  }

  if (state.move_dir != move_dir) {
    state.move_dir = move_dir;
    state.move_t = 0;
  }

  // Update wheel movement.
  state.wheel_y_dir = get_dir_from_held_keys(4);
  state.wheel_x_dir = get_dir_from_held_keys(6);
  wake_orbital_mouse_task();

  return true;
}

void housekeeping_task_orbital_mouse(void) {
  const uint16_t now = timer_read();
  if (!state.timer || !timer_expired(now, state.timer)) {
    return;
  }

  bool active = false;

  // Update position if moving.
  if (state.move_dir) {
    // Update speed, interpolated from speed_curve.
    if (state.move_t <= 16 * (NUM_SPEED_CURVE_INTERVALS - 1)) {
      if (state.move_t == 0) {
        state.speed = (int16_t)state.speed_curve[0] * 16;
      } else {
        const uint8_t i = (state.move_t - 1) / 16;
        state.speed += (int16_t)state.speed_curve[i + 1]
                     - (int16_t)state.speed_curve[i];
      }

      ++state.move_t;
    }
    // Round and cast from Q9.6 to Q6.2.
    uint8_t speed = (state.speed + 8) / 16;
    if (state.slow) {
      speed = ((uint16_t)speed) * (1 + (uint16_t)SLOW_MOVE_FACTOR_Q_8) >> 8;
    }
    else if (state.fast) {
      speed = ((uint16_t)speed) * (1 + (uint16_t)FAST_MOVE_FACTOR_Q4_4) >> 4;
    }

    state.x -= state.move_dir * scaled_sin(speed, state.angle >> 8);
    state.y -= state.move_dir * scaled_cos(speed, state.angle >> 8);
    active = true;
  }
  // Update heading angle if steering.
  if (state.steer_dir) {
    int16_t angle_step = state.slow
      ? SLOW_TURN_FACTOR_Q_8
      : (state.fast ? FAST_TURN_FACTOR_Q4_4 << 4: 256);
    if (state.steer_dir == -1) {
      angle_step = -angle_step;
    }
    set_orbital_mouse_angle_fractional(state.angle + angle_step);
    active = true;
  }

  // Update mouse wheel if active.
  if (state.wheel_x_dir || state.wheel_y_dir) {
    state.wheel_x -= state.wheel_x_dir * WHEEL_SPEED_Q2_6;
    state.wheel_y += state.wheel_y_dir * WHEEL_SPEED_Q2_6;
    // A whole step is accumulated: note which keys have scrolled, so that
    // releasing them is not additionally counted as a tap.
    if (state.wheel_y / 64 != 0) {
      state.wheel_stepped |= state.held_keys & (HELD_W_U | HELD_W_D);
    }
    if (state.wheel_x / 64 != 0) {
      state.wheel_stepped |= state.held_keys & (HELD_W_L | HELD_W_R);
    }
    active = true;
  }

  // Update double click action.
  if (state.double_click_frame) {
    ++state.double_click_frame;
    const uint8_t mask = 1 << state.selected_button;
    switch (state.double_click_frame) {
      case 2:
      case 3:
      case 4 + DOUBLE_CLICK_DELAY_INTERVALS:
        state.report.buttons ^= mask;
        break;
      case 5 + DOUBLE_CLICK_DELAY_INTERVALS:
        state.report.buttons &= ~mask;
        state.double_click_frame = 0;
    }
    active = true;
  }

  // Schedule when task should run again, or go to sleep if inactive.
  state.timer = active ? ((now + ORBITAL_MOUSE_INTERVAL_MS) | 1) : 0;

  // Send the whole part of the movement deltas, retaining fractional parts.
  flush_mouse_report();
}
