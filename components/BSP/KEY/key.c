#include "key.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

/**
 * @brief 初始化按键引脚
 */
void key_init(void)
{
    // 配置BOOT按键引脚为输入模式
    gpio_config_t io_conf = {
        .pin_bit_mask = (1ULL << KEY_BOOT_PIN),
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_ENABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE
    };
    gpio_config(&io_conf);
}

/**
 * @brief 读取按键状态
 * @return 按键状态，1为按下，0为释放
 */
uint8_t key_read(void)
{
    // 按键按下时为低电平
    return !gpio_get_level(KEY_BOOT_PIN);
}

/**
 * @brief 等待按键按下
 */
void key_wait_for_press(void)
{
    while (!key_read()) {
        vTaskDelay(pdMS_TO_TICKS(10));
    }
}

/**
 * @brief 检测按键是否按下（带消抖）
 * @return 按键状态，1为按下，0为释放
 */
uint8_t key_is_pressed(void)
{
    // 读取按键状态
    if (key_read()) {
        // 延时消抖
        vTaskDelay(pdMS_TO_TICKS(20));
        // 再次读取确认
        if (key_read()) {
            return 1;
        }
    }
    return 0;
}
