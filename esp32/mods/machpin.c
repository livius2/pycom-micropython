/*
 * Copyright (c) 2016, Pycom Limited.
 *
 * This software is licensed under the GNU GPL version 3 or any
 * later version, with permitted additional terms. For more information
 * see the Pycom Licence v1.0 document supplied with this file, or
 * available at https://www.pycom.io/opensource/licensing
 */

#include <stdio.h>
#include <stdint.h>
#include <string.h>

#include "py/mpconfig.h"
#include "py/obj.h"
#include "py/runtime.h"
#include "py/gc.h"
#include "py/mpstate.h"

#include "heap_alloc_caps.h"
#include "sdkconfig.h"
#include "esp_system.h"
#include "esp_spi_flash.h"
#include "nvs_flash.h"
#include "esp_event.h"
#include "esp_intr.h"

#include "gpio.h"
#include "machpin.h"
#include "mpirq.h"
#include "pins.h"
//#include "pybsleep.h"
#include "mpexception.h"
#include "mperror.h"

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "freertos/xtensa_api.h"


/******************************************************************************
DECLARE PRIVATE FUNCTIONS
******************************************************************************/
STATIC pin_obj_t *pin_find_named_pin(const mp_obj_dict_t *named_pins, mp_obj_t name);
STATIC pin_obj_t *pin_find_pin_by_num (const mp_obj_dict_t *named_pins, uint pin_num);
STATIC void pin_obj_configure (const pin_obj_t *self);
STATIC void pin_validate_mode (uint mode);
STATIC void pin_validate_pull (uint pull);
STATIC void pin_validate_drive (uint strength);

static void machpin_enable_pull_up (uint8_t gpio_num);
static void machpin_disable_pull_up (uint8_t gpio_num);
static void machpin_enable_pull_down (uint8_t gpio_num);
static void machpin_disable_pull_down (uint8_t gpio_num);

/******************************************************************************
DEFINE CONSTANTS
******************************************************************************/
#define MACHPIN_SIMPLE_OUTPUT               0x100
#define ETS_GPIO_INUM                       13

/******************************************************************************
DEFINE TYPES
******************************************************************************/
//typedef struct {
//    bool       active;
//    int8_t     lpds;
//    int8_t     hib;
//} pybpin_wake_pin_t;

/******************************************************************************
DECLARE PRIVATE DATA
******************************************************************************/
STATIC const mp_irq_methods_t pin_irq_methods;
//STATIC pybpin_wake_pin_t pybpin_wake_pin[PYBPIN_NUM_WAKE_PINS] =
//                                    { {.active = false, .lpds = PYBPIN_WAKES_NOT, .hib = PYBPIN_WAKES_NOT},
//                                      {.active = false, .lpds = PYBPIN_WAKES_NOT, .hib = PYBPIN_WAKES_NOT},
//                                      {.active = false, .lpds = PYBPIN_WAKES_NOT, .hib = PYBPIN_WAKES_NOT},
//                                      {.active = false, .lpds = PYBPIN_WAKES_NOT, .hib = PYBPIN_WAKES_NOT},
//                                      {.active = false, .lpds = PYBPIN_WAKES_NOT, .hib = PYBPIN_WAKES_NOT},
//                                      {.active = false, .lpds = PYBPIN_WAKES_NOT, .hib = PYBPIN_WAKES_NOT} } ;
/******************************************************************************
 DEFINE PUBLIC FUNCTIONS
 ******************************************************************************/
void pin_init0(void) {
    // initialize all pins as inputs with pull downs enabled
    // mp_map_t *named_map = mp_obj_dict_get_map((mp_obj_t)&pin_module_pins_locals_dict);
    // for (uint i = 0; i < named_map->used - 1; i++) {
    //     pin_obj_t *self = (pin_obj_t *)named_map->table[i].value;
    //     if (self != &PIN_MODULE_P1 && self->pin_number != 32) {
    //         pin_config(self, -1, -1, GPIO_MODE_INPUT, MACHPIN_PULL_DOWN, 0, 0);
    //     }
    // }
}

// C API used to convert a user-supplied pin name into an ordinal pin number
// If the pin is a board and it's not found in the pin list, it will be created
pin_obj_t *pin_find(mp_obj_t user_obj) {
    pin_obj_t *pin_obj;

    // if a pin was provided, use it
    if (MP_OBJ_IS_TYPE(user_obj, &pin_type)) {
        return user_obj;
    }

    // see if the pin name matches a expansion board pin
    pin_obj = pin_find_named_pin(&pin_exp_board_pins_locals_dict, user_obj);
    if (pin_obj) {
        return pin_obj;
    }

    // otherwise see if the pin name matches a module pin
    pin_obj = pin_find_named_pin(&pin_module_pins_locals_dict, user_obj);
    if (pin_obj) {
        return pin_obj;
    }

    nlr_raise(mp_obj_new_exception_msg(&mp_type_ValueError, mpexception_value_invalid_arguments));
}

void pin_config (pin_obj_t *self, int af_in, int af_out, uint mode, uint pull, int value, uint strength) {
    self->mode = mode, self->pull = pull, self->strength = strength;
    // if af is -1, then we want to keep it as it is
    if (af_in >= 0) {
        self->af_in = af_in;
    }
    if (af_out >= 0) {
        self->af_out = af_out;
    }

    // if value is -1, then we want to keep it as it is
    if (value >= 0) {
        self->value = value;
    }

    // mark the pin as used
    self->used = true;
    pin_obj_configure ((const pin_obj_t *)self);

//    // register it with the sleep module
//    pyb_sleep_add ((const mp_obj_t)self, (WakeUpCB_t)pin_obj_configure);
}

IRAM_ATTR void pin_set_value (const pin_obj_t* self) {
    uint32_t pin_number = self->pin_number;
    if (pin_number < 32) {
        // set the pin value
        if (self->value) {
            GPIO_REG_WRITE(GPIO_OUT_W1TS_REG, 1 << pin_number);
        } else {
            GPIO_REG_WRITE(GPIO_OUT_W1TC_REG, 1 << pin_number);
        }
    } else {
        if (self->value) {
            GPIO_REG_WRITE(GPIO_OUT1_W1TS_REG, 1 << pin_number);
        } else {
            GPIO_REG_WRITE(GPIO_OUT1_W1TC_REG, 1 << pin_number);
        }
    }
}

IRAM_ATTR uint32_t pin_get_value (const pin_obj_t* self) {
    return gpio_get_level(self->pin_number);
}

void pin_get_values (const pin_obj_t* self, uint32_t* buf, uint32_t count) {	
	gpio_get_levels(self->pin_number, buf, count);
}

static IRAM_ATTR void machpin_intr_process (void* arg) {
    ESP_GPIO_INTR_DISABLE();
    uint32_t gpio_intr_status = READ_PERI_REG(GPIO_STATUS_REG);
    uint32_t gpio_intr_status_h = READ_PERI_REG(GPIO_STATUS1_REG);
    // clear the interrupts
    SET_PERI_REG_MASK(GPIO_STATUS_W1TC_REG, gpio_intr_status);
    SET_PERI_REG_MASK(GPIO_STATUS1_W1TC_REG, gpio_intr_status_h);
    uint32_t gpio_num = 0;
    do {
        bool int_pend = false;
        if (gpio_num < 32) {
            if (gpio_intr_status & BIT(gpio_num)) {
                int_pend = true;
            }
        } else {
            if (gpio_intr_status_h & BIT(gpio_num - 32)) {
                int_pend = true;
            }
        }
        if (int_pend) {
            // we must search on the cpu dictionry instead of the board one
            // otherwise we won't find the interrupts enabled on the internal pins (e.g. LoRa int)
            pin_obj_t *self = (pin_obj_t *)pin_find_pin_by_num(&pin_cpu_pins_locals_dict, gpio_num);
            mp_irq_handler(mp_irq_find(self));
        }
    } while (++gpio_num < GPIO_PIN_COUNT);
    ESP_GPIO_INTR_ENABLE();
}

/******************************************************************************
DEFINE PRIVATE FUNCTIONS
 ******************************************************************************/
STATIC pin_obj_t *pin_find_named_pin(const mp_obj_dict_t *named_pins, mp_obj_t name) {
    mp_map_t *named_map = mp_obj_dict_get_map((mp_obj_t)named_pins);
    mp_map_elem_t *named_elem = mp_map_lookup(named_map, name, MP_MAP_LOOKUP);
    if (named_elem != NULL && named_elem->value != NULL) {
        return named_elem->value;
    }
    return NULL;
}

STATIC IRAM_ATTR pin_obj_t *pin_find_pin_by_num (const mp_obj_dict_t *named_pins, uint pin_num) {
    mp_map_t *named_map = mp_obj_dict_get_map((mp_obj_t)named_pins);
    for (uint i = 0; i < named_map->used; i++) {
        if ((((pin_obj_t *)named_map->table[i].value)->pin_number == pin_num)) {
            return named_map->table[i].value;
        }
    }
    return NULL;
}

STATIC void pin_obj_configure (const pin_obj_t *self) {
    // first detach the pin from any outputs
    gpio_matrix_out(self->pin_number, MACHPIN_SIMPLE_OUTPUT, 0, 0);
    // assign the alternate function
    if (self->mode == GPIO_MODE_INPUT) {
        if (self->af_in >= 0) {
            gpio_matrix_in(self->pin_number, self->af_in, 0);
        }
    } else {    // output or open drain
        // set the value before configuring the GPIO matrix (to minimze glitches)
        pin_set_value(self);
        if (self->af_out >= 0) {
            gpio_matrix_out(self->pin_number, self->af_out, 0, 0);
        }
    }

    // configure the pin
    gpio_config_t gpioconf = {.pin_bit_mask = 1 << self->pin_number,
                              .mode = self->mode,
                              .pull_up_en = (self->pull == MACHPIN_PULL_UP) ? GPIO_PULLUP_ENABLE : GPIO_PULLUP_DISABLE,
                              .pull_down_en = (self->pull == MACHPIN_PULL_DOWN) ? GPIO_PULLDOWN_ENABLE : GPIO_PULLDOWN_DISABLE,
                              .intr_type = self->irq_trigger};
    gpio_config(&gpioconf);

    if (gpioconf.pull_up_en == GPIO_PULLUP_ENABLE) {
        machpin_enable_pull_up(self->pin_number);
    } else {
        machpin_disable_pull_up(self->pin_number);
    }

    if (gpioconf.pull_down_en == GPIO_PULLDOWN_ENABLE) {
        machpin_enable_pull_down(self->pin_number);
    } else {
        machpin_disable_pull_down(self->pin_number);
    }
}

void pin_irq_enable (mp_obj_t self_in) {
    ESP_GPIO_INTR_ENABLE();
}

void pin_irq_disable (mp_obj_t self_in) {
    ESP_GPIO_INTR_DISABLE();
}

int pin_irq_flags (mp_obj_t self_in) {
    return 0;
}

void pin_extint_register(pin_obj_t *self, uint32_t trigger, uint32_t priority) {
    self->irq_trigger = trigger;
    pin_obj_configure(self);
    gpio_isr_register(ETS_GPIO_INUM, machpin_intr_process, NULL);
}

STATIC void pin_validate_mode (uint mode) {
    if (mode != GPIO_MODE_INPUT && mode != GPIO_MODE_OUTPUT && mode != GPIO_MODE_INPUT_OUTPUT_OD) {
        nlr_raise(mp_obj_new_exception_msg(&mp_type_ValueError, mpexception_value_invalid_arguments));
    }
}
STATIC void pin_validate_pull (uint pull) {
    if (pull != MACHPIN_PULL_UP && pull != MACHPIN_PULL_DOWN) {
        nlr_raise(mp_obj_new_exception_msg(&mp_type_ValueError, mpexception_value_invalid_arguments));
    }
}

STATIC void pin_validate_drive(uint strength) {
//    if (strength != PIN_STRENGTH_2MA && strength != PIN_STRENGTH_4MA && strength != PIN_STRENGTH_6MA) {
//        nlr_raise(mp_obj_new_exception_msg(&mp_type_ValueError, mpexception_value_invalid_arguments));
//    }
}

/******************************************************************************/
// Micro Python bindings

STATIC const mp_arg_t pin_init_args[] = {
    { MP_QSTR_mode,                        MP_ARG_OBJ, {.u_obj = MP_OBJ_NULL} },
    { MP_QSTR_pull,                        MP_ARG_OBJ, {.u_obj = mp_const_none} },
    { MP_QSTR_value,    MP_ARG_KW_ONLY  |  MP_ARG_OBJ, {.u_obj = MP_OBJ_NULL} },
    { MP_QSTR_drive,    MP_ARG_KW_ONLY  |  MP_ARG_INT, {.u_int = 0} },
    { MP_QSTR_alt,      MP_ARG_KW_ONLY  |  MP_ARG_OBJ, {.u_obj = mp_const_none} },
};
#define pin_INIT_NUM_ARGS MP_ARRAY_SIZE(pin_init_args)

STATIC mp_obj_t pin_obj_init_helper(pin_obj_t *self, mp_uint_t n_args, const mp_obj_t *pos_args, mp_map_t *kw_args) {
    // parse args
    mp_arg_val_t args[pin_INIT_NUM_ARGS];
    mp_arg_parse_all(n_args, pos_args, kw_args, pin_INIT_NUM_ARGS, pin_init_args, args);

    // get the io mode
    uint mode;
    //  default is input
    if (args[0].u_obj == MP_OBJ_NULL) {
        mode = GPIO_MODE_INPUT;
    } else {
        mode = mp_obj_get_int(args[0].u_obj);
        pin_validate_mode (mode);
    }

    // get the pull type
    uint pull;
    if (args[1].u_obj == mp_const_none) {
        pull = MACHPIN_PULL_NONE;
    } else {
        pull = mp_obj_get_int(args[1].u_obj);
        pin_validate_pull (pull);
    }

    // get the value
    int value = -1;
    if (args[2].u_obj != MP_OBJ_NULL) {
        if (mp_obj_is_true(args[2].u_obj)) {
            value = 1;
        } else {
            value = 0;
        }
    }

    // get the strenght
    uint strength = args[3].u_int;
    pin_validate_drive(strength);

    // get the alternate function
    int af_in = -1, af_out = -1;
    int af = (args[4].u_obj != mp_const_none) ? mp_obj_get_int(args[4].u_obj) : -1;
    if (af > 255) {
        goto invalid_args;
    }
    if (mode == GPIO_MODE_INPUT) {
        af_in = af;
    } else if (mode == GPIO_MODE_OUTPUT) {
        af_out = af;
    } else {    // open drain
        af_in = af;
        af_out = af;
    }

    pin_config(self, af_in, af_out, mode, pull, value, strength);

    return mp_const_none;

invalid_args:
   nlr_raise(mp_obj_new_exception_msg(&mp_type_ValueError, mpexception_value_invalid_arguments));
}

STATIC void pin_print(const mp_print_t *print, mp_obj_t self_in, mp_print_kind_t kind) {
    pin_obj_t *self = self_in;
    uint32_t pull = self->pull;
    //uint32_t drive = self->strength; // FIXME

    // pin name
    mp_printf(print, "Pin('%q'", self->name);

    // pin mode
    qstr mode_qst;
    uint32_t mode = self->mode;
    if (mode == GPIO_MODE_INPUT) {
        mode_qst = MP_QSTR_IN;
    } else if (mode == GPIO_MODE_OUTPUT) {
        mode_qst = MP_QSTR_OUT;
    } else {
        mode_qst = MP_QSTR_OPEN_DRAIN;
    }
    mp_printf(print, ", mode=Pin.%q", mode_qst);

    // pin pull
    qstr pull_qst;
    if (pull == MACHPIN_PULL_NONE) {
        mp_printf(print, ", pull=%q", MP_QSTR_None);
    } else {
        if (pull == MACHPIN_PULL_UP) {
            pull_qst = MP_QSTR_PULL_UP;
        } else {
            pull_qst = MP_QSTR_PULL_DOWN;
        }
        mp_printf(print, ", pull=Pin.%q", pull_qst);
    }

//    // pin drive
//    qstr drv_qst;
//    if (drive == PIN_STRENGTH_2MA) {
//        drv_qst = MP_QSTR_LOW_POWER;
//    } else if (drive == PIN_STRENGTH_4MA) {
//        drv_qst = MP_QSTR_MED_POWER;
//    } else {
//        drv_qst = MP_QSTR_HIGH_POWER;
//    }
    qstr drv_qst = MP_QSTR_MED_POWER; // FIXME
    mp_printf(print, ", drive=Pin.%q", drv_qst);

    // pin af
    int alt = (self->af_in >= 0) ? self->af_in : self->af_out;
    mp_printf(print, ", alt=%d)", alt);
}

STATIC mp_obj_t pin_make_new(const mp_obj_type_t *type, mp_uint_t n_args, mp_uint_t n_kw, const mp_obj_t *args) {
    mp_arg_check_num(n_args, n_kw, 1, MP_OBJ_FUN_ARGS_MAX, true);

    // Run an argument through the mapper and return the result.
    pin_obj_t *self = (pin_obj_t *)pin_find(args[0]);

    mp_map_t kw_args;
    mp_map_init_fixed_table(&kw_args, n_kw, args + n_args);
    pin_obj_init_helper(self, n_args - 1, args + 1, &kw_args);

    return (mp_obj_t)self;
}

STATIC mp_obj_t pin_obj_init(mp_uint_t n_args, const mp_obj_t *args, mp_map_t *kw_args) {
    return pin_obj_init_helper(args[0], n_args - 1, args + 1, kw_args);
}
MP_DEFINE_CONST_FUN_OBJ_KW(pin_init_obj, 1, pin_obj_init);

STATIC mp_obj_t pin_value(mp_uint_t n_args, const mp_obj_t *args) {
    pin_obj_t *self = args[0];
    if (n_args == 1) {
        // get the value
        return MP_OBJ_NEW_SMALL_INT(pin_get_value(self));
    } else {
        self->value = mp_obj_is_true(args[1]);
        pin_set_value(self);
        return mp_const_none;
    }
}
STATIC MP_DEFINE_CONST_FUN_OBJ_VAR_BETWEEN(pin_value_obj, 1, 2, pin_value);

STATIC mp_obj_t pin_toggle(mp_obj_t self_in) {
    pin_obj_t *self = self_in;
    self->value = !self->value;
    pin_set_value(self);
    return mp_const_none;
}
STATIC MP_DEFINE_CONST_FUN_OBJ_1(pin_toggle_obj, pin_toggle);

STATIC mp_obj_t pin_id(mp_obj_t self_in) {
    pin_obj_t *self = self_in;
    return MP_OBJ_NEW_QSTR(self->name);
}
STATIC MP_DEFINE_CONST_FUN_OBJ_1(pin_id_obj, pin_id);

STATIC mp_obj_t pin_mode(mp_uint_t n_args, const mp_obj_t *args) {
    pin_obj_t *self = args[0];
    if (n_args == 1) {
        return mp_obj_new_int(self->mode);
    } else {
        uint32_t mode = mp_obj_get_int(args[1]);
        pin_validate_mode(mode);
        self->mode = mode;
        pin_obj_configure(self);
        return mp_const_none;
    }
}
STATIC MP_DEFINE_CONST_FUN_OBJ_VAR_BETWEEN(pin_mode_obj, 1, 2, pin_mode);

STATIC mp_obj_t pin_pull(mp_uint_t n_args, const mp_obj_t *args) {
    pin_obj_t *self = args[0];
    if (n_args == 1) {
        if (self->pull == MACHPIN_PULL_NONE) {
            return mp_const_none;
        }
        return mp_obj_new_int(self->pull);
    } else {
        uint32_t pull;
        if (args[1] == mp_const_none) {
            pull = MACHPIN_PULL_NONE;
        } else {
            pull = mp_obj_get_int(args[1]);
            pin_validate_pull (pull);
        }
        self->pull = pull;
        pin_obj_configure(self);
        return mp_const_none;
    }
}
STATIC MP_DEFINE_CONST_FUN_OBJ_VAR_BETWEEN(pin_pull_obj, 1, 2, pin_pull);

STATIC mp_obj_t pin_drive(mp_uint_t n_args, const mp_obj_t *args) {
    pin_obj_t *self = args[0];
    if (n_args == 1) {
        return mp_obj_new_int(self->strength);
    } else {
        uint32_t strength = mp_obj_get_int(args[1]);
        pin_validate_drive (strength);
        self->strength = strength;
        pin_obj_configure(self);
        return mp_const_none;
    }
}
STATIC MP_DEFINE_CONST_FUN_OBJ_VAR_BETWEEN(pin_drive_obj, 1, 2, pin_drive);

STATIC mp_obj_t pin_call(mp_obj_t self_in, mp_uint_t n_args, mp_uint_t n_kw, const mp_obj_t *args) {
    mp_arg_check_num(n_args, n_kw, 0, 1, false);
    mp_obj_t _args[2] = {self_in, *args};
    return pin_value (n_args + 1, _args);
}

//STATIC mp_obj_t pin_alt_list(mp_obj_t self_in) {
//    pin_obj_t *self = self_in;
//    mp_obj_t af[2];
//    mp_obj_t afs = mp_obj_new_list(0, NULL);
//
//    for (int i = 0; i < self->num_afs; i++) {
//        af[0] = MP_OBJ_NEW_QSTR(self->af_list[i].name);
//        af[1] = mp_obj_new_int(self->af_list[i].idx);
//        mp_obj_list_append(afs, mp_obj_new_tuple(MP_ARRAY_SIZE(af), af));
//    }
//    return afs;
//}
//STATIC MP_DEFINE_CONST_FUN_OBJ_1(pin_alt_list_obj, pin_alt_list);

/// \method irq(trigger, priority, handler, wake)
STATIC mp_obj_t pin_irq (mp_uint_t n_args, const mp_obj_t *pos_args, mp_map_t *kw_args) {
    mp_arg_val_t args[mp_irq_INIT_NUM_ARGS];
    mp_arg_parse_all(n_args - 1, pos_args + 1, kw_args, mp_irq_INIT_NUM_ARGS, mp_irq_init_args, args);
    pin_obj_t *self = pos_args[0];

//    // convert the priority to the correct value
//    uint priority = mp_irq_translate_priority (args[1].u_int);

    // verify and translate the interrupt mode
    uint trigger = mp_obj_get_int(args[0].u_obj);
//    if (mp_trigger == (PYB_PIN_FALLING_EDGE | PYB_PIN_RISING_EDGE)) {
//        trigger = GPIO_BOTH_EDGES;
//    } else {
//        switch (mp_trigger) {
//        case PYB_PIN_FALLING_EDGE:
//            trigger = GPIO_FALLING_EDGE;
//            break;
//        case PYB_PIN_RISING_EDGE:
//            trigger = GPIO_RISING_EDGE;
//            break;
//        case PYB_PIN_LOW_LEVEL:
//            trigger = GPIO_LOW_LEVEL;
//            break;
//        case PYB_PIN_HIGH_LEVEL:
//            trigger = GPIO_HIGH_LEVEL;
//            break;
//        default:
//            goto invalid_args;
//        }
//    }

//    uint8_t pwrmode = (args[3].u_obj == mp_const_none) ? PYB_PWR_MODE_ACTIVE : mp_obj_get_int(args[3].u_obj);
//    if (pwrmode > (PYB_PWR_MODE_ACTIVE | PYB_PWR_MODE_LPDS | PYB_PWR_MODE_HIBERNATE)) {
//        goto invalid_args;
//    }

    // interrupt before we update anything.
    pin_irq_disable(self);

    pin_extint_register((pin_obj_t *)self, trigger, 0);

//    // get the wake info from this pin
//    uint hib_pin, idx;
//    pin_get_hibernate_pin_and_idx ((const pin_obj_t *)self, &hib_pin, &idx);
//    if (pwrmode & PYB_PWR_MODE_LPDS) {
//        if (idx >= PYBPIN_NUM_WAKE_PINS) {
//            goto invalid_args;
//        }
//        // wake modes are different in LDPS
//        uint wake_mode;
//        switch (trigger) {
//        case GPIO_FALLING_EDGE:
//            wake_mode = PRCM_LPDS_FALL_EDGE;
//            break;
//        case GPIO_RISING_EDGE:
//            wake_mode = PRCM_LPDS_RISE_EDGE;
//            break;
//        case GPIO_LOW_LEVEL:
//            wake_mode = PRCM_LPDS_LOW_LEVEL;
//            break;
//        case GPIO_HIGH_LEVEL:
//            wake_mode = PRCM_LPDS_HIGH_LEVEL;
//            break;
//        default:
//            goto invalid_args;
//            break;
//        }
//
//        // first clear the lpds value from all wake-able pins
//        for (uint i = 0; i < PYBPIN_NUM_WAKE_PINS; i++) {
//            pybpin_wake_pin[i].lpds = PYBPIN_WAKES_NOT;
//        }
//
//        // enable this pin as a wake-up source during LPDS
//        pybpin_wake_pin[idx].lpds = wake_mode;
//    } else if (idx < PYBPIN_NUM_WAKE_PINS) {
//        // this pin was the previous LPDS wake source, so disable it completely
//        if (pybpin_wake_pin[idx].lpds != PYBPIN_WAKES_NOT) {
//            MAP_PRCMLPDSWakeupSourceDisable(PRCM_LPDS_GPIO);
//        }
//        pybpin_wake_pin[idx].lpds = PYBPIN_WAKES_NOT;
//    }
//
//    if (pwrmode & PYB_PWR_MODE_HIBERNATE) {
//        if (idx >= PYBPIN_NUM_WAKE_PINS) {
//            goto invalid_args;
//        }
//        // wake modes are different in hibernate
//        uint wake_mode;
//        switch (trigger) {
//        case GPIO_FALLING_EDGE:
//            wake_mode = PRCM_HIB_FALL_EDGE;
//            break;
//        case GPIO_RISING_EDGE:
//            wake_mode = PRCM_HIB_RISE_EDGE;
//            break;
//        case GPIO_LOW_LEVEL:
//            wake_mode = PRCM_HIB_LOW_LEVEL;
//            break;
//        case GPIO_HIGH_LEVEL:
//            wake_mode = PRCM_HIB_HIGH_LEVEL;
//            break;
//        default:
//            goto invalid_args;
//            break;
//        }
//
//        // enable this pin as wake-up source during hibernate
//        pybpin_wake_pin[idx].hib = wake_mode;
//    } else if (idx < PYBPIN_NUM_WAKE_PINS) {
//        pybpin_wake_pin[idx].hib = PYBPIN_WAKES_NOT;
//    }
//
//    // we need to update the callback atomically, so we disable the
//    if (pwrmode & PYB_PWR_MODE_ACTIVE) {
//        // register the interrupt
//        pin_extint_register((pin_obj_t *)self, trigger, priority);
//        if (idx < PYBPIN_NUM_WAKE_PINS) {
//            pybpin_wake_pin[idx].active = true;
//        }
//    } else if (idx < PYBPIN_NUM_WAKE_PINS) {
//        pybpin_wake_pin[idx].active = false;
//    }
//
    // all checks have passed, we can create the irq object
    mp_obj_t _irq = mp_irq_new (self, args[2].u_obj, &pin_irq_methods, true);
//    if (pwrmode & PYB_PWR_MODE_LPDS) {
//        pyb_sleep_set_gpio_lpds_callback (_irq);
//    }

    // enable the interrupt just before leaving
    pin_irq_enable(self);

    return _irq;
//
//invalid_args:
//    nlr_raise(mp_obj_new_exception_msg(&mp_type_ValueError, mpexception_value_invalid_arguments));
}
STATIC MP_DEFINE_CONST_FUN_OBJ_KW(pin_irq_obj, 1, pin_irq);

void machpin_register_irq_c_handler(pin_obj_t *self, void *handler) {
    mp_irq_new ((mp_obj_t)self, (mp_obj_t)handler, &pin_irq_methods, false);
}

STATIC const mp_map_elem_t pin_locals_dict_table[] = {
    // instance methods
    { MP_OBJ_NEW_QSTR(MP_QSTR_init),                    (mp_obj_t)&pin_init_obj },
    { MP_OBJ_NEW_QSTR(MP_QSTR_value),                   (mp_obj_t)&pin_value_obj },
    { MP_OBJ_NEW_QSTR(MP_QSTR_toggle),                  (mp_obj_t)&pin_toggle_obj },
    { MP_OBJ_NEW_QSTR(MP_QSTR_id),                      (mp_obj_t)&pin_id_obj },
    { MP_OBJ_NEW_QSTR(MP_QSTR_mode),                    (mp_obj_t)&pin_mode_obj },
    { MP_OBJ_NEW_QSTR(MP_QSTR_pull),                    (mp_obj_t)&pin_pull_obj },
    { MP_OBJ_NEW_QSTR(MP_QSTR_drive),                   (mp_obj_t)&pin_drive_obj },
//    { MP_OBJ_NEW_QSTR(MP_QSTR_alt_list),                (mp_obj_t)&pin_alt_list_obj },
    { MP_OBJ_NEW_QSTR(MP_QSTR_irq),                     (mp_obj_t)&pin_irq_obj },

    // class attributes
    { MP_OBJ_NEW_QSTR(MP_QSTR_module),                  (mp_obj_t)&pin_module_pins_obj_type },
    { MP_OBJ_NEW_QSTR(MP_QSTR_exp_board),               (mp_obj_t)&pin_exp_board_pins_obj_type },

    // class constants
    { MP_OBJ_NEW_QSTR(MP_QSTR_IN),                      MP_OBJ_NEW_SMALL_INT(GPIO_MODE_INPUT) },
    { MP_OBJ_NEW_QSTR(MP_QSTR_OUT),                     MP_OBJ_NEW_SMALL_INT(GPIO_MODE_OUTPUT) },
    { MP_OBJ_NEW_QSTR(MP_QSTR_OPEN_DRAIN),              MP_OBJ_NEW_SMALL_INT(GPIO_MODE_INPUT_OUTPUT_OD) },

    { MP_OBJ_NEW_QSTR(MP_QSTR_PULL_UP),                 MP_OBJ_NEW_SMALL_INT(MACHPIN_PULL_UP) },
    { MP_OBJ_NEW_QSTR(MP_QSTR_PULL_DOWN),               MP_OBJ_NEW_SMALL_INT(MACHPIN_PULL_DOWN) },

//    { MP_OBJ_NEW_QSTR(MP_QSTR_LOW_POWER),               MP_OBJ_NEW_SMALL_INT(0) },
//    { MP_OBJ_NEW_QSTR(MP_QSTR_MED_POWER),               MP_OBJ_NEW_SMALL_INT(1) },
//    { MP_OBJ_NEW_QSTR(MP_QSTR_HIGH_POWER),              MP_OBJ_NEW_SMALL_INT(2) },

    { MP_OBJ_NEW_QSTR(MP_QSTR_IRQ_FALLING),             MP_OBJ_NEW_SMALL_INT(GPIO_INTR_NEGEDGE) },
    { MP_OBJ_NEW_QSTR(MP_QSTR_IRQ_RISING),              MP_OBJ_NEW_SMALL_INT(GPIO_INTR_POSEDGE) },
    { MP_OBJ_NEW_QSTR(MP_QSTR_IRQ_LOW_LEVEL),           MP_OBJ_NEW_SMALL_INT(GPIO_INTR_LOW_LEVEL) },
    { MP_OBJ_NEW_QSTR(MP_QSTR_IRQ_HIGH_LEVEL),          MP_OBJ_NEW_SMALL_INT(GPIO_INTR_HIGH_LEVEL) },
};

STATIC MP_DEFINE_CONST_DICT(pin_locals_dict, pin_locals_dict_table);

const mp_obj_type_t pin_type = {
    { &mp_type_type },
    .name = MP_QSTR_Pin,
    .print = pin_print,
    .make_new = pin_make_new,
    .call = pin_call,
    .locals_dict = (mp_obj_t)&pin_locals_dict,
};

STATIC const mp_irq_methods_t pin_irq_methods = {
    .init = pin_irq,
    .enable = pin_irq_enable,
    .disable = pin_irq_disable,
    .flags = pin_irq_flags,
};

STATIC void pin_named_pins_obj_print(const mp_print_t *print, mp_obj_t self_in, mp_print_kind_t kind) {
    pin_named_pins_obj_t *self = self_in;
    mp_printf(print, "<Pin.%q>", self->name);
}

const mp_obj_type_t pin_module_pins_obj_type = {
    { &mp_type_type },
    .name = MP_QSTR_cpu,
    .print = pin_named_pins_obj_print,
    .locals_dict = (mp_obj_t)&pin_module_pins_locals_dict,
};

const mp_obj_type_t pin_exp_board_pins_obj_type = {
    { &mp_type_type },
    .name = MP_QSTR_board,
    .print = pin_named_pins_obj_print,
    .locals_dict = (mp_obj_t)&pin_exp_board_pins_locals_dict,
};



//-----------API functions--------------
#include "driver/gpio.h"
#include "soc/rtc_io_reg.h"
#include "soc/io_mux_reg.h"

static void machpin_enable_pull_up (uint8_t gpio_num)
{
    switch(gpio_num) {
        case GPIO_NUM_0:
            SET_PERI_REG_MASK(RTC_IO_TOUCH_PAD1_REG,RTC_IO_TOUCH_PAD1_RUE_M);
            break;
        case GPIO_NUM_2:
            SET_PERI_REG_MASK(RTC_IO_TOUCH_PAD2_REG,RTC_IO_TOUCH_PAD2_RUE_M);
            break;
        case GPIO_NUM_4:
            SET_PERI_REG_MASK(RTC_IO_TOUCH_PAD0_REG,RTC_IO_TOUCH_PAD0_RUE_M);
            break;
        case GPIO_NUM_12:
            SET_PERI_REG_MASK(RTC_IO_TOUCH_PAD5_REG, RTC_IO_TOUCH_PAD5_RUE_M);
            break;
        case GPIO_NUM_13:
            SET_PERI_REG_MASK(RTC_IO_TOUCH_PAD4_REG, RTC_IO_TOUCH_PAD4_RUE_M);
            break;
        case GPIO_NUM_14:
            SET_PERI_REG_MASK(RTC_IO_TOUCH_PAD6_REG, RTC_IO_TOUCH_PAD6_RUE_M);
            break;
        case GPIO_NUM_15:
            SET_PERI_REG_MASK(RTC_IO_TOUCH_PAD3_REG, RTC_IO_TOUCH_PAD3_RUE_M);
            break;
        case GPIO_NUM_25:
            SET_PERI_REG_MASK(RTC_IO_PAD_DAC1_REG, RTC_IO_PDAC1_RUE_M);
            break;
        case GPIO_NUM_26:
            SET_PERI_REG_MASK(RTC_IO_PAD_DAC2_REG, RTC_IO_PDAC2_RUE_M);
            break;
        case GPIO_NUM_27:
            SET_PERI_REG_MASK(RTC_IO_TOUCH_PAD7_REG, RTC_IO_TOUCH_PAD7_RUE_M);
            break;
        case GPIO_NUM_32:
            SET_PERI_REG_MASK(RTC_IO_XTAL_32K_PAD_REG, RTC_IO_X32P_RUE_M);
            break;
        case GPIO_NUM_33:
            SET_PERI_REG_MASK(RTC_IO_XTAL_32K_PAD_REG, RTC_IO_X32N_RUE_M);
            break;
        case GPIO_NUM_36:
        case GPIO_NUM_37:
        case GPIO_NUM_38:
        case GPIO_NUM_39:
            break;
        default:
            PIN_PULLUP_EN(GPIO_PIN_MUX_REG[gpio_num]);
            break;
    }
}

static void machpin_disable_pull_up (uint8_t gpio_num)
{
    switch(gpio_num) {
        case GPIO_NUM_0:
            CLEAR_PERI_REG_MASK(RTC_IO_TOUCH_PAD1_REG,RTC_IO_TOUCH_PAD1_RUE_M);
            break;
        case GPIO_NUM_2:
            CLEAR_PERI_REG_MASK(RTC_IO_TOUCH_PAD2_REG,RTC_IO_TOUCH_PAD2_RUE_M);
            break;
        case GPIO_NUM_4:
            CLEAR_PERI_REG_MASK(RTC_IO_TOUCH_PAD0_REG,RTC_IO_TOUCH_PAD0_RUE_M);
            break;
        case GPIO_NUM_12:
            CLEAR_PERI_REG_MASK(RTC_IO_TOUCH_PAD5_REG, RTC_IO_TOUCH_PAD5_RUE_M);
            break;
        case GPIO_NUM_13:
            CLEAR_PERI_REG_MASK(RTC_IO_TOUCH_PAD4_REG, RTC_IO_TOUCH_PAD4_RUE_M);
            break;
        case GPIO_NUM_14:
            CLEAR_PERI_REG_MASK(RTC_IO_TOUCH_PAD6_REG, RTC_IO_TOUCH_PAD6_RUE_M);
            break;
        case GPIO_NUM_15:
            CLEAR_PERI_REG_MASK(RTC_IO_TOUCH_PAD3_REG, RTC_IO_TOUCH_PAD3_RUE_M);
            break;
        case GPIO_NUM_25:
            CLEAR_PERI_REG_MASK(RTC_IO_PAD_DAC1_REG, RTC_IO_PDAC1_RUE_M);
            break;
        case GPIO_NUM_26:
            CLEAR_PERI_REG_MASK(RTC_IO_PAD_DAC2_REG, RTC_IO_PDAC2_RUE_M);
            break;
        case GPIO_NUM_27:
            CLEAR_PERI_REG_MASK(RTC_IO_TOUCH_PAD7_REG, RTC_IO_TOUCH_PAD7_RUE_M);
            break;
        case GPIO_NUM_32:
            CLEAR_PERI_REG_MASK(RTC_IO_XTAL_32K_PAD_REG, RTC_IO_X32P_RUE_M);
            break;
        case GPIO_NUM_33:
            CLEAR_PERI_REG_MASK(RTC_IO_XTAL_32K_PAD_REG, RTC_IO_X32N_RUE_M);
            break;
        case GPIO_NUM_36:
        case GPIO_NUM_37:
        case GPIO_NUM_38:
        case GPIO_NUM_39:
            break;
        default:
            PIN_PULLUP_DIS(GPIO_PIN_MUX_REG[gpio_num]);
            break;
    }
}

static void machpin_enable_pull_down (uint8_t gpio_num)
{
    switch(gpio_num) {
        case GPIO_NUM_0:
            SET_PERI_REG_MASK(RTC_IO_TOUCH_PAD1_REG,RTC_IO_TOUCH_PAD1_RDE_M);
            break;
        case GPIO_NUM_2:
            SET_PERI_REG_MASK(RTC_IO_TOUCH_PAD2_REG,RTC_IO_TOUCH_PAD2_RDE_M);
            break;
        case GPIO_NUM_4:
            SET_PERI_REG_MASK(RTC_IO_TOUCH_PAD0_REG,RTC_IO_TOUCH_PAD0_RDE_M);
            break;
        case GPIO_NUM_12:
            SET_PERI_REG_MASK(RTC_IO_TOUCH_PAD5_REG, RTC_IO_TOUCH_PAD5_RDE_M);
            break;
        case GPIO_NUM_13:
            SET_PERI_REG_MASK(RTC_IO_TOUCH_PAD4_REG, RTC_IO_TOUCH_PAD4_RDE_M);
            break;
        case GPIO_NUM_14:
            SET_PERI_REG_MASK(RTC_IO_TOUCH_PAD6_REG, RTC_IO_TOUCH_PAD6_RDE_M);
            break;
        case GPIO_NUM_15:
            SET_PERI_REG_MASK(RTC_IO_TOUCH_PAD3_REG, RTC_IO_TOUCH_PAD3_RDE_M);
            break;
        case GPIO_NUM_25:
            SET_PERI_REG_MASK(RTC_IO_PAD_DAC1_REG, RTC_IO_PDAC1_RDE_M);
            break;
        case GPIO_NUM_26:
            SET_PERI_REG_MASK(RTC_IO_PAD_DAC2_REG, RTC_IO_PDAC2_RDE_M);
            break;
        case GPIO_NUM_27:
            SET_PERI_REG_MASK(RTC_IO_TOUCH_PAD7_REG, RTC_IO_TOUCH_PAD7_RDE_M);
            break;
        case GPIO_NUM_32:
            SET_PERI_REG_MASK(RTC_IO_XTAL_32K_PAD_REG, RTC_IO_X32P_RDE_M);
            break;
        case GPIO_NUM_33:
            SET_PERI_REG_MASK(RTC_IO_XTAL_32K_PAD_REG, RTC_IO_X32N_RDE_M);
            break;
        case GPIO_NUM_36:
        case GPIO_NUM_37:
        case GPIO_NUM_38:
        case GPIO_NUM_39:
            break;
        default:
            PIN_PULLDWN_EN(GPIO_PIN_MUX_REG[gpio_num]);
            break;
    }
}

static void machpin_disable_pull_down (uint8_t gpio_num)
{
    switch(gpio_num) {
        case GPIO_NUM_0:
            CLEAR_PERI_REG_MASK(RTC_IO_TOUCH_PAD1_REG,RTC_IO_TOUCH_PAD1_RDE_M);
            break;
        case GPIO_NUM_2:
            CLEAR_PERI_REG_MASK(RTC_IO_TOUCH_PAD2_REG,RTC_IO_TOUCH_PAD2_RDE_M);
            break;
        case GPIO_NUM_4:
            CLEAR_PERI_REG_MASK(RTC_IO_TOUCH_PAD0_REG,RTC_IO_TOUCH_PAD0_RDE_M);
            break;
        case GPIO_NUM_12:
            CLEAR_PERI_REG_MASK(RTC_IO_TOUCH_PAD5_REG, RTC_IO_TOUCH_PAD5_RDE_M);
            break;
        case GPIO_NUM_13:
            CLEAR_PERI_REG_MASK(RTC_IO_TOUCH_PAD4_REG, RTC_IO_TOUCH_PAD4_RDE_M);
            break;
        case GPIO_NUM_14:
            CLEAR_PERI_REG_MASK(RTC_IO_TOUCH_PAD6_REG, RTC_IO_TOUCH_PAD6_RDE_M);
            break;
        case GPIO_NUM_15:
            CLEAR_PERI_REG_MASK(RTC_IO_TOUCH_PAD3_REG, RTC_IO_TOUCH_PAD3_RDE_M);
            break;
        case GPIO_NUM_25:
            CLEAR_PERI_REG_MASK(RTC_IO_PAD_DAC1_REG, RTC_IO_PDAC1_RDE_M);
            break;
        case GPIO_NUM_26:
            CLEAR_PERI_REG_MASK(RTC_IO_PAD_DAC2_REG, RTC_IO_PDAC2_RDE_M);
            break;
        case GPIO_NUM_27:
            CLEAR_PERI_REG_MASK(RTC_IO_TOUCH_PAD7_REG, RTC_IO_TOUCH_PAD7_RDE_M);
            break;
        case GPIO_NUM_32:
            CLEAR_PERI_REG_MASK(RTC_IO_XTAL_32K_PAD_REG, RTC_IO_X32P_RDE_M);
            break;
        case GPIO_NUM_33:
            CLEAR_PERI_REG_MASK(RTC_IO_XTAL_32K_PAD_REG, RTC_IO_X32N_RDE_M);
            break;
        case GPIO_NUM_36:
        case GPIO_NUM_37:
        case GPIO_NUM_38:
        case GPIO_NUM_39:
            break;
        default:
            PIN_PULLDWN_DIS(GPIO_PIN_MUX_REG[gpio_num]);
            break;
    }
}
