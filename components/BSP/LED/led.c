#include "led.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
// 静态变量：记录LED状态（仅本文件可用）
static bool led_state = false;
/**
 * @brief 初始化LED引脚
 */
void led_init(void)
{
    // 配置GPIO2为输出模式
    gpio_config_t io_conf = {
        .pin_bit_mask = (1ULL << LED_PIN),
        .mode = GPIO_MODE_OUTPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE
    };
    gpio_config(&io_conf);
    
    // 初始状态为熄灭
    led_off();
}

/**
 * @brief 点亮LED
 */
void led_on(void)
{
    gpio_set_level(LED_PIN, 1);
}

/**
 * @brief 熄灭LED
 */
void led_off(void)
{
    gpio_set_level(LED_PIN, 0);
}

/**
 * @brief 切换LED状态
 */

// 静态变量：记录LED当前状态（0=灭，1=亮）

void led_toggle(void)
{
    // 翻转状态变量
    led_state = !led_state;
    // 直接设置电平
    gpio_set_level(LED_PIN, led_state);
}




