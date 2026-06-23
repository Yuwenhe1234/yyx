#ifndef KEY_H
#define KEY_H

#include "driver/gpio.h"

#define KEY_BOOT_PIN GPIO_NUM_0

/**
 * @brief 初始化按键引脚
 */
void key_init(void);

/**
 * @brief 读取按键状态
 * @return 按键状态，1为按下，0为释放
 */
uint8_t key_read(void);

/**
 * @brief 等待按键按下
 */
void key_wait_for_press(void);

/**
 * @brief 检测按键是否按下（带消抖）
 * @return 按键状态，1为按下，0为释放
 */
uint8_t key_is_pressed(void);

#endif // KEY_H
