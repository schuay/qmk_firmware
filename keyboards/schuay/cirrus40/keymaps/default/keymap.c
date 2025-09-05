#include QMK_KEYBOARD_H

const uint16_t PROGMEM keymaps[][MATRIX_ROWS][MATRIX_COLS] = {
    [0] = LAYOUT(
        KC_TAB,    KC_Q,    KC_W,    KC_E,    KC_R,    KC_T,                             KC_Y,    KC_U,    KC_I,    KC_O,    KC_P,    KC_LBRC,
        KC_CAPS,   KC_A,    KC_S,    KC_D,    KC_F,    KC_G,                             KC_H,    KC_J,    KC_K,    KC_L,    KC_SCLN, KC_QUOT,
        LCTL_T(KC_LEFT), KC_Z, KC_X, KC_C,    KC_V,    KC_B,                             KC_N,    KC_M,    KC_COMM, KC_DOT,  KC_SLSH, RCTL_T(KC_RGHT),
                                   KC_LGUI, TG(2),   KC_LALT, LSFT_T(KC_SPC), RSFT_T(KC_ENT),   KC_RALT, TG(1), RGUI_T(KC_BSPC)
    ),

    [1] = LAYOUT(
        _______, XXXXXXX, KC_UP,   XXXXXXX, XXXXXXX, XXXXXXX,                          KC_HASH, KC_KP_4, KC_KP_5, KC_KP_6, XXXXXXX, XXXXXXX,
        XXXXXXX, KC_LEFT, KC_DOWN, KC_RIGHT,KC_LBRC, KC_RBRC,                          KC_AMPR, KC_KP_1, KC_KP_2, KC_KP_3, XXXXXXX, XXXXXXX,
        _______, XXXXXXX, KC_PAST, KC_PPLS, KC_PSLS, KC_EQL,                           KC_DLR,  KC_KP_0, KC_PDOT, KC_COLN, KC_PMNS, _______,
                                   _______, _______, _______, _______,        _______, _______, _______, _______ 
    ),

    [2] = LAYOUT(
        _______, _______, _______, _______, _______, _______,                          _______, _______, _______, _______, _______, _______,
        _______, _______, _______, _______, _______, _______,                          _______, _______, _______, _______, _______, _______,
        _______, _______, _______, _______, _______, _______,                          _______, _______, _______, _______, _______, _______,
                                   _______, _______, _______, _______,        _______, _______, _______, _______
    ),

    [3] = LAYOUT(
        QK_RBT, XXXXXXX, XXXXXXX, XXXXXXX, XXXXXXX, XXXXXXX,                           XXXXXXX, XXXXXXX, XXXXXXX,  UG_PREV, UG_SATD, RGB_M_P,
        XXXXXXX, XXXXXXX, XXXXXXX, XXXXXXX, XXXXXXX, KC_CAPS,                          XXXXXXX, XXXXXXX, XXXXXXX, UG_HUEU, UG_VALU, RGB_M_B,
        XXXXXXX, XXXXXXX, XXXXXXX, XXXXXXX, XXXXXXX, XXXXXXX,                          XXXXXXX, XXXXXXX, XXXXXXX, UG_HUED, UG_VALD, RGB_M_R,
                                   _______, _______, _______, _______,        _______, _______, _______, _______ 
    )
};

#if defined(ENCODER_MAP_ENABLE)
const uint16_t PROGMEM encoder_map[][NUM_ENCODERS][NUM_DIRECTIONS] = {
    [0] = { ENCODER_CCW_CW(KC_VOLD, KC_VOLU), ENCODER_CCW_CW(KC_VOLD, KC_VOLU) },
    [1] = { ENCODER_CCW_CW(KC_TRNS, KC_TRNS), ENCODER_CCW_CW(KC_TRNS, KC_TRNS) },
    [2] = { ENCODER_CCW_CW(KC_TRNS, KC_TRNS), ENCODER_CCW_CW(KC_TRNS, KC_TRNS) },
    [3] = { ENCODER_CCW_CW(KC_TRNS, KC_TRNS), ENCODER_CCW_CW(KC_TRNS, KC_TRNS) }
};
#endif
