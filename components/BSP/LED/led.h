#ifndef LED_H
#define LED_H

#include "driver/gpio.h"

#define LED_PIN GPIO_NUM_2

/**
 * @brief 初始化LED引脚
 */
void led_init(void);

/**
 * @brief 点亮LED
 */
void led_on(void);

/**
 * @brief 熄灭LED
 */
void led_off(void);

/**
 * @brief 切换LED状态
 */
void led_toggle(void);


#endif // LED_H
