// Copyright 2015-2016 Espressif Systems (Shanghai) PTE LTD
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at

//     http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.
#include <esp_types.h>
#include "esp_err.h"
#include "esp_intr.h"
#include "gpio.h"
#include "soc/soc.h"

//TODO: move debug options to menuconfig
#define GPIO_DBG_ENABLE     (0)
#define GPIO_WARNING_ENABLE (0)
#define GPIO_ERROR_ENABLE   (0)
#define GPIO_INFO_ENABLE    (0)
//DBG INFOR 
#if GPIO_INFO_ENABLE
#define GPIO_INFO ets_printf
#else
#define GPIO_INFO(...)
#endif
#if GPIO_WARNING_ENABLE
#define GPIO_WARNING(format,...) do{\
        ets_printf("[waring][%s#%u]",__FUNCTION__,__LINE__);\
        ets_printf(format,##__VA_ARGS__);\
}while(0)
#else 
#define GPIO_WARNING(...)
#endif
#if GPIO_ERROR_ENABLE
#define GPIO_ERROR(format,...) do{\
        ets_printf("[error][%s#%u]",__FUNCTION__,__LINE__);\
        ets_printf(format,##__VA_ARGS__);\
}while(0)
#else 
#define GPIO_ERROR(...)
#endif 

const uint32_t GPIO_PIN_MUX_REG[GPIO_PIN_COUNT] = {
    GPIO_PIN_REG_0,
    GPIO_PIN_REG_1,
    GPIO_PIN_REG_2,
    GPIO_PIN_REG_3,
    GPIO_PIN_REG_4,
    GPIO_PIN_REG_5,
    GPIO_PIN_REG_6,
    GPIO_PIN_REG_7,
    GPIO_PIN_REG_8,
    GPIO_PIN_REG_9,
    GPIO_PIN_REG_10,
    GPIO_PIN_REG_11,
    GPIO_PIN_REG_12,
    GPIO_PIN_REG_13,
    GPIO_PIN_REG_14,
    GPIO_PIN_REG_15,
    GPIO_PIN_REG_16,
    GPIO_PIN_REG_17,
    GPIO_PIN_REG_18,
    GPIO_PIN_REG_19,
    0,
    GPIO_PIN_REG_21,
    GPIO_PIN_REG_22,
    GPIO_PIN_REG_23,
    0,
    GPIO_PIN_REG_25,
    GPIO_PIN_REG_26,
    GPIO_PIN_REG_27,
    0,
    0,
    0,
    0,
    GPIO_PIN_REG_32,
    GPIO_PIN_REG_33,
    GPIO_PIN_REG_34,
    GPIO_PIN_REG_35,
    GPIO_PIN_REG_36,
    GPIO_PIN_REG_37,
    GPIO_PIN_REG_38,
    GPIO_PIN_REG_39
};

static int is_valid_gpio(int gpio_num)
{
    if(gpio_num >= GPIO_PIN_COUNT || GPIO_PIN_MUX_REG[gpio_num] == 0) {
        GPIO_ERROR("GPIO io_num=%d does not exist\n",gpio_num);
        return 0;
    }
    return 1;
}

esp_err_t gpio_set_intr_type(gpio_num_t gpio_num, gpio_int_type_t intr_type)
{
    if(!is_valid_gpio(gpio_num)) {
        return ESP_ERR_INVALID_ARG;
    }
    if(intr_type >= GPIO_INTR_MAX) {
        GPIO_ERROR("Unknown GPIO intr:%u\n",intr_type);
        return ESP_ERR_INVALID_ARG;
    }
    GPIO.pin[gpio_num].int_type = intr_type;
    return ESP_OK;
}

static esp_err_t gpio_output_disable(gpio_num_t gpio_num)
{
    if(!is_valid_gpio(gpio_num)) {
        return ESP_ERR_INVALID_ARG;
    }
    if(gpio_num < 32) {
        GPIO.enable_w1tc = (0x1 << gpio_num);
    } else {
        GPIO.enable1_w1tc.data = (0x1 << (gpio_num - 32));
    }
    return ESP_OK;
}

static esp_err_t gpio_output_enable(gpio_num_t gpio_num)
{
    if(gpio_num >= 34) {
        GPIO_ERROR("io_num=%d can only be input\n",gpio_num);
        return ESP_ERR_INVALID_ARG;
    }
    if(!is_valid_gpio(gpio_num)) {
        return ESP_ERR_INVALID_ARG;
    }
    if(gpio_num < 32) {
        GPIO.enable_w1ts = (0x1 << gpio_num);
    } else {
        GPIO.enable1_w1ts.data = (0x1 << (gpio_num - 32));
    }
    return ESP_OK;
}

esp_err_t gpio_set_level(gpio_num_t gpio_num, uint32_t level)
{
    if(!GPIO_IS_VALID_GPIO(gpio_num)) {
        return ESP_ERR_INVALID_ARG;
    }
    if(level) {
        if(gpio_num < 32) {
            GPIO.out_w1ts = (1 << gpio_num);
        } else {
            GPIO.out1_w1ts.data = (1 << (gpio_num - 32));
        }
    } else {
        if(gpio_num < 32) {
            GPIO.out_w1tc = (1 << gpio_num);
        } else {
            GPIO.out1_w1tc.data = (1 << (gpio_num - 32));
        }
    }
    return ESP_OK;
}

int gpio_get_level(gpio_num_t gpio_num)
{
    if(gpio_num < 32) {
        return (GPIO.in >> gpio_num) & 0x1;
    } else {
        return (GPIO.in1.data >> (gpio_num - 32)) & 0x1;
    }
}

void gpio_get_levels(gpio_num_t gpio_num, uint32_t* buf, uint32_t count)
{
    int i;    

    if (gpio_num < 32) {
        for ( i = 0; i < count; ++i) {
            buf[i] = (GPIO.in >> gpio_num) & 0x1;
        }
    } else {
		gpio_num -= 32;
		for ( i = 0; i < count; ++i) {
            buf[i] = (GPIO.in1.data >> gpio_num) & 0x1;
        }
    }
}

esp_err_t gpio_set_pull_mode(gpio_num_t gpio_num, gpio_pull_mode_t pull)
{
    if(!is_valid_gpio(gpio_num)) {
        return ESP_ERR_INVALID_ARG;
    }
    esp_err_t ret = ESP_OK;
    switch(pull) {
        case GPIO_PULLUP_ONLY:
            PIN_PULLUP_EN(GPIO_PIN_MUX_REG[gpio_num]);
            PIN_PULLDWN_DIS(GPIO_PIN_MUX_REG[gpio_num]);
            break;
        case GPIO_PULLDOWN_ONLY:
            PIN_PULLUP_DIS(GPIO_PIN_MUX_REG[gpio_num]);
            PIN_PULLDWN_EN(GPIO_PIN_MUX_REG[gpio_num]);
            break;
        case GPIO_PULLUP_PULLDOWN:
            PIN_PULLUP_EN(GPIO_PIN_MUX_REG[gpio_num]);
            PIN_PULLDWN_EN(GPIO_PIN_MUX_REG[gpio_num]);
            break;
        case GPIO_FLOATING:
            PIN_PULLUP_DIS(GPIO_PIN_MUX_REG[gpio_num]);
            PIN_PULLDWN_DIS(GPIO_PIN_MUX_REG[gpio_num]);
            break;
        default:
            GPIO_ERROR("Unknown pull up/down mode,gpio_num=%u,pull=%u\n",gpio_num,pull);
            ret = ESP_ERR_INVALID_ARG;
            break;
    }
    return ret;
}

esp_err_t gpio_set_direction(gpio_num_t gpio_num, gpio_mode_t mode)
{
    if(!is_valid_gpio(gpio_num)) {
        return ESP_ERR_INVALID_ARG;
    }
    if(gpio_num >= 34 && (mode & (GPIO_MODE_DEF_OUTPUT))) {
        GPIO_ERROR("io_num=%d can only be input\n",gpio_num);
        return ESP_ERR_INVALID_ARG;
    }
    esp_err_t ret = ESP_OK;
    if(mode & GPIO_MODE_DEF_INPUT) {
        PIN_INPUT_ENABLE(GPIO_PIN_MUX_REG[gpio_num]);
    } else {
        PIN_INPUT_DISABLE(GPIO_PIN_MUX_REG[gpio_num]);
    }
    if(mode & GPIO_MODE_DEF_OUTPUT) {
        if(gpio_num < 32) {
            GPIO.enable_w1ts = (0x1 << gpio_num);
        } else {
            GPIO.enable1_w1ts.data = (0x1 << (gpio_num - 32));
        }
    } else {
        if(gpio_num < 32) {
            GPIO.enable_w1tc = (0x1 << gpio_num);
        } else {
            GPIO.enable1_w1tc.data = (0x1 << (gpio_num - 32));
        }
    }
    if(mode & GPIO_MODE_DEF_OD) {
        GPIO.pin[gpio_num].pad_driver = 1;
    } else {
        GPIO.pin[gpio_num].pad_driver = 0;
    }
    return ret;
}

esp_err_t gpio_config(gpio_config_t *pGPIOConfig)
{
    uint64_t gpio_pin_mask = (pGPIOConfig->pin_bit_mask);
    uint32_t io_reg = 0;
    uint32_t io_num = 0;
    uint64_t bit_valid = 0;
    if(pGPIOConfig->pin_bit_mask == 0 || pGPIOConfig->pin_bit_mask >= (((uint64_t) 1) << GPIO_PIN_COUNT)) {
        GPIO_ERROR("GPIO_PIN mask error \n");
        return ESP_ERR_INVALID_ARG;
    }
    if((pGPIOConfig->mode) & (GPIO_MODE_DEF_OUTPUT)) {
        //GPIO 34/35/36/37/38/39 can only be used as input mode;
        if((gpio_pin_mask & ( GPIO_SEL_34 | GPIO_SEL_35 | GPIO_SEL_36 | GPIO_SEL_37 | GPIO_SEL_38 | GPIO_SEL_39))) {
            GPIO_ERROR("GPIO34-39 can only be used as input mode\n");
            return ESP_ERR_INVALID_ARG;
        }
    }
    do {
        io_reg = GPIO_PIN_MUX_REG[io_num];
        if(((gpio_pin_mask >> io_num) & BIT(0)) && io_reg) {
            GPIO_INFO("Gpio%02d |Mode:",io_num);
            if((pGPIOConfig->mode) & GPIO_MODE_DEF_INPUT) {
                GPIO_INFO("INPUT ");
                PIN_INPUT_ENABLE(GPIO_PIN_MUX_REG[io_num]);
            } else {
                PIN_INPUT_DISABLE(GPIO_PIN_MUX_REG[io_num]);
            }
            if((pGPIOConfig->mode) & GPIO_MODE_DEF_OD) {
                GPIO_INFO("OD ");
                GPIO.pin[io_num].pad_driver = 1; /*0x01 Open-drain */
            } else {
                GPIO.pin[io_num].pad_driver = 0; /*0x00 Normal gpio output */
            }
            if((pGPIOConfig->mode) & GPIO_MODE_DEF_OUTPUT) {
                GPIO_INFO("OUTPUT ");
                gpio_output_enable(io_num);
            } else {
                gpio_output_disable(io_num);
            }
            GPIO_INFO("|");
            if(pGPIOConfig->pull_up_en) {
                GPIO_INFO("PU ");
                PIN_PULLUP_EN(io_reg);
            } else {
                PIN_PULLUP_DIS(io_reg);
            }
            if(pGPIOConfig->pull_down_en) {
                GPIO_INFO("PD ");
                PIN_PULLDWN_EN(io_reg);
            } else {
                PIN_PULLDWN_DIS(io_reg);
            }
            GPIO_INFO("Intr:%d |\n",pGPIOConfig->intr_type);
            gpio_set_intr_type(io_num, pGPIOConfig->intr_type);
//            We don't care about interrupts here
//            if(pGPIOConfig->intr_type) {
//                gpio_intr_enable(io_num);
//            } else {
//                gpio_intr_disable(io_num);
//            }
            PIN_FUNC_SELECT(io_reg, PIN_FUNC_GPIO); /*function number 2 is GPIO_FUNC for each pin */
        } else if(bit_valid && (io_reg == 0)) {
            GPIO_WARNING("io_num=%d does not exist\n",io_num);
        }
        io_num++;
    } while(io_num < GPIO_PIN_COUNT);
    return ESP_OK;
}
