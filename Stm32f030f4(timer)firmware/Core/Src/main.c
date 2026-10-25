/* main.c – SysTick-based non-blocking timer with Flash-saved settings
 *
 * MCU:
 * STM32F030F4P6
 *
 * Features:
 * - Saves ON/OFF times in the last 1 KB Flash page.
 * - If valid settings exist after reset/power-up, the timer starts immediately.
 * - SET button held for ~3 seconds while RUN_CYCLE re-enters setup mode.
 * - Short SET presses while RUN_CYCLE are ignored.
 *
 * IMPORTANT:
 * The linker/project Flash length MUST leave 0x08003C00-0x08003FFF
 * unused by the application. For a 16 KB STM32F030F4, pages are 1 KB.
 */

#include "main.h"
#include <stdio.h>
#include <stdbool.h>
#include <string.h>
#include "stm32f0xx_hal.h"

/* Keep your LCD includes (absolute paths as in your project) */
#include <F:\Sundry\Projects\Completed documents\stm_timer\Stm32f030f4(timer)firmware\Core\Inc\LCD16X2\LCD16X2.h>
#include <F:\Sundry\Projects\Completed documents\stm_timer\Stm32f030f4(timer)firmware\Core\Inc\LCD16X2\LCD16X2_cfg.h>
#define MyLCD LCD16X2_1

/* Private prototypes */
void SystemClock_Config(void);
static void MX_GPIO_Init(void);

/* Application time variables are declared before Flash helpers because
 * the Flash load/save functions access these values. */
int on_hour    = 0;
int on_minute  = 0;
int off_hour   = 0;
int off_minute = 0;

/* ============================================================
 * Flash settings
 * ============================================================ */

#define SETTINGS_FLASH_ADDR    ((uint32_t)0x08003C00UL)
#define SETTINGS_MAGIC         ((uint16_t)0x5AA5U)
#define SET_HOLD_MS            ((uint32_t)3000U)

/*
 * Stored as half-words because STM32F0 main Flash is programmed
 * in 16-bit units.
 */
typedef struct
{
    uint16_t magic;
    uint16_t on_hour;
    uint16_t on_minute;
    uint16_t off_hour;
    uint16_t off_minute;
    uint16_t checksum;
} TimerSettings_t;

static uint16_t Settings_Checksum(const TimerSettings_t *s)
{
    return (uint16_t)(
        s->magic ^
        s->on_hour ^
        s->on_minute ^
        s->off_hour ^
        s->off_minute
    );
}

static bool Settings_IsValid(const TimerSettings_t *s)
{
    uint32_t on_seconds;
    uint32_t off_seconds;

    if (s->magic != SETTINGS_MAGIC)
        return false;

    if (s->checksum != Settings_Checksum(s))
        return false;

    if (s->on_hour > 23U || s->off_hour > 23U)
        return false;

    if (s->on_minute > 59U || s->off_minute > 59U)
        return false;

    on_seconds =
        ((uint32_t)s->on_hour * 3600UL) +
        ((uint32_t)s->on_minute * 60UL);

    off_seconds =
        ((uint32_t)s->off_hour * 3600UL) +
        ((uint32_t)s->off_minute * 60UL);

    /*
     * The existing application requires both phases to be > 0.
     * Keep the same rule for saved settings.
     */
    if (on_seconds == 0UL || off_seconds == 0UL)
        return false;

    return true;
}

static bool Settings_Load(void)
{
    const TimerSettings_t *saved =
        (const TimerSettings_t *)SETTINGS_FLASH_ADDR;

    if (!Settings_IsValid(saved))
        return false;

    on_hour    = (int)saved->on_hour;
    on_minute  = (int)saved->on_minute;
    off_hour   = (int)saved->off_hour;
    off_minute = (int)saved->off_minute;

    return true;
}

static bool Settings_Save(void)
{
    TimerSettings_t settings;
    FLASH_EraseInitTypeDef erase_init;
    uint32_t page_error = 0U;
    uint32_t address;
    bool ok = true;

    settings.magic      = SETTINGS_MAGIC;
    settings.on_hour    = (uint16_t)on_hour;
    settings.on_minute  = (uint16_t)on_minute;
    settings.off_hour   = (uint16_t)off_hour;
    settings.off_minute = (uint16_t)off_minute;
    settings.checksum   = Settings_Checksum(&settings);

    HAL_FLASH_Unlock();

    /* Erase the last 1 KB Flash page. */
    memset(&erase_init, 0, sizeof(erase_init));
    erase_init.TypeErase = FLASH_TYPEERASE_PAGES;
    erase_init.PageAddress = SETTINGS_FLASH_ADDR;
    erase_init.NbPages = 1U;

    if (HAL_FLASHEx_Erase(&erase_init, &page_error) != HAL_OK)
    {
        ok = false;
    }
    else
    {
        /*
         * Program the six 16-bit fields.
         */
        const uint16_t *data = (const uint16_t *)&settings;

        for (uint32_t i = 0U; i < 6U; i++)
        {
            address = SETTINGS_FLASH_ADDR + (i * 2U);

            if (HAL_FLASH_Program(
                    FLASH_TYPEPROGRAM_HALFWORD,
                    address,
                    data[i]) != HAL_OK)
            {
                ok = false;
                break;
            }
        }
    }

    HAL_FLASH_Lock();

    return ok;
}

/* ============================================================
 * Application state
 * ============================================================ */

enum State
{
    SELECT_MODE,
    SET_ON_HOUR,
    SET_ON_MINUTE,
    SET_OFF_HOUR,
    SET_OFF_MINUTE,
    RUN_CYCLE
};

volatile enum State timerState = SELECT_MODE;

/* UI & run-cycle helpers */
static uint32_t last_ui_ms = 0;
static bool run_initialized = false;
static uint32_t on_cycle_ms = 0;
static uint32_t off_cycle_ms = 0;
static uint32_t remaining_ms = 0;
static bool phase_on = true;

/*
 * SET long-press detection while running.
 * A long press must start while RUN_CYCLE is already active.
 */
static uint32_t set_hold_start_ms = 0;
static bool set_hold_active = false;
static bool set_hold_action_done = false;

/* Button debounce structure */
typedef struct
{
    uint8_t stable;
    uint8_t counter;
    bool pressed_edge;
    uint16_t pin;
} Btn;

Btn btn_set, btn_inc, btn_dec;

/* Function prototypes */
void Update_LCD(int x, int y);
void Handle_Buttons_nonblock(void);
void Run_Cycle_nonblock(void);

/* Helper: read raw button (pressed if HAL_GPIO_ReadPin == GPIO_PIN_SET) */
static inline uint8_t ReadBtnRaw(uint16_t pin)
{
    return (HAL_GPIO_ReadPin(GPIOA, pin) == GPIO_PIN_SET) ? 1U : 0U;
}

/* Relay helper: active-low */
static inline void Relay_OFF(void)
{
    HAL_GPIO_WritePin(GPIOA, GPIO_PIN_1, GPIO_PIN_SET);
}

static inline void Relay_ON(void)
{
    HAL_GPIO_WritePin(GPIOA, GPIO_PIN_1, GPIO_PIN_RESET);
}

/* ============================================================
 * main
 * ============================================================ */

int main(void)
{
    HAL_Init();
    SystemClock_Config();
    MX_GPIO_Init();

    /* init button debounce structs */
    btn_set.pin = Set_Pin;
    btn_set.stable = ReadBtnRaw(Set_Pin);
    btn_set.counter = 0;
    btn_set.pressed_edge = false;

    btn_inc.pin = Inc_Pin;
    btn_inc.stable = ReadBtnRaw(Inc_Pin);
    btn_inc.counter = 0;
    btn_inc.pressed_edge = false;

    btn_dec.pin = Dec_Pin;
    btn_dec.stable = ReadBtnRaw(Dec_Pin);
    btn_dec.counter = 0;
    btn_dec.pressed_edge = false;

    /* LCD splash using HAL_GetTick() */
    HAL_GPIO_WritePin(Backlit_GPIO_Port, Backlit_Pin, GPIO_PIN_SET);
    LCD16X2_Init(MyLCD);
    LCD16X2_Clear(MyLCD);
    //LCD16X2_Set_Cursor(MyLCD, 1, 1);
    LCD16X2_Write_String(MyLCD, "Adjustable_Timer");

    uint32_t tstart = HAL_GetTick();
    while ((HAL_GetTick() - tstart) < 2900U)
    {
        /* ~2.9 s splash */
    }

    LCD16X2_Clear(MyLCD);

    /*
     * IMPORTANT STARTUP BEHAVIOR:
     *
     * Valid saved times  -> start RUN_CYCLE directly.
     * No valid saved data -> go to SELECT_MODE for first setup.
     */
    run_initialized = false;
    phase_on = true;
    set_hold_active = false;
    set_hold_action_done = false;

    if (Settings_Load())
    {
        on_cycle_ms =
            ((uint32_t)on_hour * 3600UL +
             (uint32_t)on_minute * 60UL) * 1000UL;

        off_cycle_ms =
            ((uint32_t)off_hour * 3600UL +
             (uint32_t)off_minute * 60UL) * 1000UL;

        timerState = RUN_CYCLE;
    }
    else
    {
        /*
         * No valid stored settings.
         * First power-up/setup starts here.
         */
        on_hour = 0;
        on_minute = 0;
        off_hour = 0;
        off_minute = 0;

        timerState = SELECT_MODE;
    }

    while (1)
    {
        uint32_t now = HAL_GetTick();

        /* Sample buttons every 10 ms for debounce */
        static uint32_t last_sample_ms = 0;

        if ((now - last_sample_ms) >= 10U)
        {
            last_sample_ms = now;

            /* Require 50 ms stable to accept a button state change */
#define SAMPLE_BUTTON(B) do {                                      \
    uint8_t raw = ReadBtnRaw(B.pin);                               \
    if (raw != B.stable) {                                        \
        B.counter += 10U;                                         \
        if (B.counter >= 50U) {                                   \
            if ((raw == 1U) && (B.stable == 0U))                 \
                B.pressed_edge = true;                            \
            B.stable = raw;                                       \
            B.counter = 0U;                                       \
        }                                                         \
    } else {                                                      \
        B.counter = 0U;                                           \
    }                                                             \
} while (0)

            SAMPLE_BUTTON(btn_set);
            SAMPLE_BUTTON(btn_inc);
            SAMPLE_BUTTON(btn_dec);

#undef SAMPLE_BUTTON
        }

        Handle_Buttons_nonblock();

        if (timerState == RUN_CYCLE)
        {
            Run_Cycle_nonblock();
        }

        /* Throttle LCD updates (~200 ms) */
        if ((now - last_ui_ms) >= 200U)
        {
            last_ui_ms = now;

            /* LCD updates for setup states */
            if (timerState == SELECT_MODE)
            {
                LCD16X2_Clear(MyLCD);
                LCD16X2_Set_Cursor(MyLCD, 1, 1);
                LCD16X2_Write_String(MyLCD, "Press SET to");
                LCD16X2_Set_Cursor(MyLCD, 2, 1);
                LCD16X2_Write_String(MyLCD, "start setup...");
            }
            else if (timerState == SET_ON_HOUR)
            {
                LCD16X2_Clear(MyLCD);
                LCD16X2_Set_Cursor(MyLCD, 1, 1);
                LCD16X2_Write_String(MyLCD, "Set ON Hour:");

                char buf[17];
                snprintf(buf, sizeof(buf), "      %02d        ", on_hour);

                LCD16X2_Set_Cursor(MyLCD, 2, 1);
                LCD16X2_Write_String(MyLCD, buf);
            }
            else if (timerState == SET_ON_MINUTE)
            {
                LCD16X2_Clear(MyLCD);
                LCD16X2_Set_Cursor(MyLCD, 1, 1);
                LCD16X2_Write_String(MyLCD, "Set ON Min:");

                char buf[17];
                snprintf(buf, sizeof(buf), "      %02d        ", on_minute);

                LCD16X2_Set_Cursor(MyLCD, 2, 1);
                LCD16X2_Write_String(MyLCD, buf);
            }
            else if (timerState == SET_OFF_HOUR)
            {
                LCD16X2_Clear(MyLCD);
                LCD16X2_Set_Cursor(MyLCD, 1, 1);
                LCD16X2_Write_String(MyLCD, "Set OFF Hour:");

                char buf[17];
                snprintf(buf, sizeof(buf), "      %02d        ", off_hour);

                LCD16X2_Set_Cursor(MyLCD, 2, 1);
                LCD16X2_Write_String(MyLCD, buf);
            }
            else if (timerState == SET_OFF_MINUTE)
            {
                LCD16X2_Clear(MyLCD);
                LCD16X2_Set_Cursor(MyLCD, 1, 1);
                LCD16X2_Write_String(MyLCD, "Set OFF Min:");

                char buf[17];
                snprintf(buf, sizeof(buf), "      %02d        ", off_minute);

                LCD16X2_Set_Cursor(MyLCD, 2, 1);
                LCD16X2_Write_String(MyLCD, buf);
            }
        }
    }
}

/* ============================================================
 * helper implementations
 * ============================================================ */

void Update_LCD(int x, int y)
{
    char buffer[17];

    snprintf(buffer, sizeof(buffer), "%02d:%02d", x, y);

    LCD16X2_Clear(MyLCD);
    LCD16X2_Set_Cursor(MyLCD, 1, 1);
    LCD16X2_Write_String(MyLCD, "Press '+' for H&M");
    LCD16X2_Set_Cursor(MyLCD, 2, 1);
    LCD16X2_Write_String(MyLCD, "'-' for M :");
    LCD16X2_Set_Cursor(MyLCD, 2, 14);
    LCD16X2_Write_String(MyLCD, buffer);
}

void Handle_Buttons_nonblock(void)
{
    if (timerState == SELECT_MODE)
    {
        /*
         * SET short press starts setup.
         * This is intentionally separate from the long-press behavior
         * used while RUN_CYCLE is active.
         */
        if (btn_set.pressed_edge)
        {
            btn_set.pressed_edge = false;

            set_hold_active = false;
            set_hold_action_done = false;

            timerState = SET_ON_HOUR;
        }
    }
    else if (timerState == SET_ON_HOUR)
    {
        if (btn_inc.pressed_edge)
        {
            btn_inc.pressed_edge = false;
            on_hour = (on_hour + 1) % 24;
        }
        else if (btn_dec.pressed_edge)
        {
            btn_dec.pressed_edge = false;
            on_hour = (on_hour - 1 + 24) % 24;
        }
        else if (btn_set.pressed_edge)
        {
            btn_set.pressed_edge = false;
            timerState = SET_ON_MINUTE;
        }
    }
    else if (timerState == SET_ON_MINUTE)
    {
        if (btn_inc.pressed_edge)
        {
            btn_inc.pressed_edge = false;
            on_minute = (on_minute + 1) % 60;
        }
        else if (btn_dec.pressed_edge)
        {
            btn_dec.pressed_edge = false;
            on_minute = (on_minute - 1 + 60) % 60;
        }
        else if (btn_set.pressed_edge)
        {
            btn_set.pressed_edge = false;
            timerState = SET_OFF_HOUR;
        }
    }
    else if (timerState == SET_OFF_HOUR)
    {
        if (btn_inc.pressed_edge)
        {
            btn_inc.pressed_edge = false;
            off_hour = (off_hour + 1) % 24;
        }
        else if (btn_dec.pressed_edge)
        {
            btn_dec.pressed_edge = false;
            off_hour = (off_hour - 1 + 24) % 24;
        }
        else if (btn_set.pressed_edge)
        {
            btn_set.pressed_edge = false;
            timerState = SET_OFF_MINUTE;
        }
    }
    else if (timerState == SET_OFF_MINUTE)
    {
        if (btn_inc.pressed_edge)
        {
            btn_inc.pressed_edge = false;
            off_minute = (off_minute + 1) % 60;
        }
        else if (btn_dec.pressed_edge)
        {
            btn_dec.pressed_edge = false;
            off_minute = (off_minute - 1 + 60) % 60;
        }
        else if (btn_set.pressed_edge)
        {
            btn_set.pressed_edge = false;

            /* Convert selected times to milliseconds */
            on_cycle_ms =
                ((uint32_t)on_hour * 3600UL +
                 (uint32_t)on_minute * 60UL) * 1000UL;

            off_cycle_ms =
                ((uint32_t)off_hour * 3600UL +
                 (uint32_t)off_minute * 60UL) * 1000UL;

            /* Validate: both ON and OFF durations must be > 0 */
            if (on_cycle_ms == 0UL || off_cycle_ms == 0UL)
            {
                LCD16X2_Clear(MyLCD);
                LCD16X2_Set_Cursor(MyLCD, 1, 1);
                LCD16X2_Write_String(MyLCD, "   Error!");
                LCD16X2_Set_Cursor(MyLCD, 2, 1);
                LCD16X2_Write_String(MyLCD, " Time cannot be 0");

                uint32_t t = HAL_GetTick();
                while ((HAL_GetTick() - t) < 2000UL)
                {
                    /* brief blocking wait */
                }

                timerState = SELECT_MODE;
            }
            else
            {
                /*
                 * SAVE ONLY AFTER THE USER HAS COMPLETELY CONFIRMED
                 * both ON and OFF times.
                 */
                if (Settings_Save())
                {
                    timerState = RUN_CYCLE;
                    run_initialized = false;
                }
                else
                {
                    LCD16X2_Clear(MyLCD);
                    LCD16X2_Set_Cursor(MyLCD, 1, 1);
                    LCD16X2_Write_String(MyLCD, " Flash save");
                    LCD16X2_Set_Cursor(MyLCD, 2, 1);
                    LCD16X2_Write_String(MyLCD, "    ERROR!");

                    uint32_t t = HAL_GetTick();
                    while ((HAL_GetTick() - t) < 2000UL)
                    {
                        /* brief blocking wait */
                    }

                    /*
                     * Keep the selected values in RAM, but do not claim
                     * they were saved. User can retry setup.
                     */
                    timerState = SELECT_MODE;
                }
            }
        }
    }
    else if (timerState == RUN_CYCLE)
    {
        /*
         * SHORT SET PRESS:
         * ignored while running.
         *
         * LONG SET PRESS:
         * hold SET continuously for about 3 seconds to enter setup.
         */
        if (btn_set.pressed_edge)
        {
            btn_set.pressed_edge = false;

            set_hold_start_ms = HAL_GetTick();
            set_hold_active = true;
            set_hold_action_done = false;
        }

        if (!btn_set.stable)
        {
            /*
             * Button released before 3 seconds.
             * Nothing happens.
             */
            set_hold_active = false;
            set_hold_action_done = false;
        }
        else if (set_hold_active && !set_hold_action_done)
        {
            uint32_t held_ms = HAL_GetTick() - set_hold_start_ms;

            if (held_ms >= SET_HOLD_MS)
            {
                set_hold_action_done = true;
                set_hold_active = false;

                /*
                 * Stop the current cycle and go to setup.
                 * New settings will be written to Flash only after
                 * the user reaches the final SET confirmation again.
                 */
                Relay_OFF();

                run_initialized = false;
                phase_on = true;
                timerState = SELECT_MODE;
            }
        }
    }
}

void Run_Cycle_nonblock(void)
{
    uint32_t now = HAL_GetTick();

    static uint32_t last_ms = 0;

    if (!run_initialized)
    {
        on_cycle_ms =
            ((uint32_t)on_hour * 3600UL +
             (uint32_t)on_minute * 60UL) * 1000UL;

        off_cycle_ms =
            ((uint32_t)off_hour * 3600UL +
             (uint32_t)off_minute * 60UL) * 1000UL;

        remaining_ms = on_cycle_ms;
        phase_on = true;

        /* Relay ON (active-low: LOW = ON) */
        Relay_ON();

        run_initialized = true;
        last_ms = now;
    }

    /* Compute elapsed time safely */
    uint32_t delta = now - last_ms;
    last_ms = now;

    if (delta > 0U)
    {
        if (remaining_ms > delta)
            remaining_ms -= delta;
        else
            remaining_ms = 0U;
    }

    /* End of current phase reached -> toggle and load next duration */
    if (remaining_ms == 0U)
    {
        phase_on = !phase_on;

        if (phase_on)
        {
            /* Switch to ON phase */
            Relay_ON();
            remaining_ms = on_cycle_ms;
        }
        else
        {
            /* Switch to OFF phase */
            Relay_OFF();
            remaining_ms = off_cycle_ms;
        }
    }

    /* Flicker-free LCD update */
    char buf[17];

    uint32_t total_seconds = remaining_ms / 1000UL;
    uint32_t hh = total_seconds / 3600UL;
    uint32_t mm = (total_seconds % 3600UL) / 60UL;
    uint32_t ss = total_seconds % 60UL;

    /* Format time string, then pad with spaces to 16 characters */
    if (hh > 0U)
    {
        snprintf(
            buf,
            sizeof(buf),
            "%02u:%02u:%02u",
            (unsigned)hh,
            (unsigned)mm,
            (unsigned)ss
        );
    }
    else
    {
        snprintf(
            buf,
            sizeof(buf),
            "   %02u:%02u   ",
            (unsigned)mm,
            (unsigned)ss
        );
    }

    int len = (int)strlen(buf);

    while (len < 16)
    {
        buf[len++] = ' ';
    }

    buf[16] = '\0';

    /* Line 1: show phase */
    char line1[17];

    if (phase_on)
        snprintf(line1, sizeof(line1), "Loop (ON)       ");
    else
        snprintf(line1, sizeof(line1), "Loop (OFF)      ");

    line1[16] = '\0';

    LCD16X2_Set_Cursor(MyLCD, 1, 1);
    LCD16X2_Write_String(MyLCD, line1);

    LCD16X2_Set_Cursor(MyLCD, 2, 1);
    LCD16X2_Write_String(MyLCD, buf);
}

/* ============================================================
 * GPIO / clock
 * ============================================================ */

static void MX_GPIO_Init(void)
{
    GPIO_InitTypeDef GPIO_InitStruct = {0};

    /* GPIO Ports Clock Enable */
    __HAL_RCC_GPIOA_CLK_ENABLE();
    __HAL_RCC_GPIOB_CLK_ENABLE();

    /* Configure GPIO pin for Backlit (PA10) */
    GPIO_InitStruct.Pin = Backlit_Pin;
    GPIO_InitStruct.Mode = GPIO_MODE_OUTPUT_PP;
    GPIO_InitStruct.Pull = GPIO_NOPULL;
    GPIO_InitStruct.Speed = GPIO_SPEED_FREQ_LOW;
    HAL_GPIO_Init(Backlit_GPIO_Port, &GPIO_InitStruct);

    /* Keep Backlit always ON */
    HAL_GPIO_WritePin(Backlit_GPIO_Port, Backlit_Pin, GPIO_PIN_SET);

    /* Configure GPIO pin for Relay (PA1) */
    GPIO_InitStruct.Pin = GPIO_PIN_1;
    GPIO_InitStruct.Mode = GPIO_MODE_OUTPUT_PP;
    GPIO_InitStruct.Pull = GPIO_NOPULL;
    GPIO_InitStruct.Speed = GPIO_SPEED_FREQ_LOW;
    HAL_GPIO_Init(GPIOA, &GPIO_InitStruct);

    /*
     * Initial state of LCD & relay outputs.
     * Relay HIGH = OFF because relay is active-low.
     */
    HAL_GPIO_WritePin(
        GPIOA,
        D4_Pin | D5_Pin | D6_Pin | D7_Pin | RS_Pin | Backlit_Pin,
        GPIO_PIN_RESET
    );

    HAL_GPIO_WritePin(GPIOA, Relay_Pin, GPIO_PIN_SET);

    /* Configure GPIO output level */
    HAL_GPIO_WritePin(E_GPIO_Port, E_Pin, GPIO_PIN_RESET);

    /* LCD + Relay + Backlight outputs */
    GPIO_InitStruct.Pin =
        D4_Pin | Relay_Pin | D5_Pin | D6_Pin | D7_Pin | RS_Pin | Backlit_Pin;

    GPIO_InitStruct.Mode = GPIO_MODE_OUTPUT_PP;
    GPIO_InitStruct.Pull = GPIO_NOPULL;
    GPIO_InitStruct.Speed = GPIO_SPEED_FREQ_LOW;
    HAL_GPIO_Init(GPIOA, &GPIO_InitStruct);

    /* Buttons */
    GPIO_InitStruct.Pin = Set_Pin | Inc_Pin | Dec_Pin;
    GPIO_InitStruct.Mode = GPIO_MODE_INPUT;
    GPIO_InitStruct.Pull = GPIO_NOPULL;
    HAL_GPIO_Init(GPIOA, &GPIO_InitStruct);

    /* E pin */
    GPIO_InitStruct.Pin = E_Pin;
    GPIO_InitStruct.Mode = GPIO_MODE_OUTPUT_PP;
    GPIO_InitStruct.Pull = GPIO_NOPULL;
    GPIO_InitStruct.Speed = GPIO_SPEED_FREQ_LOW;
    HAL_GPIO_Init(E_GPIO_Port, &GPIO_InitStruct);
}

void SystemClock_Config(void)
{
    RCC_OscInitTypeDef RCC_OscInitStruct = {0};
    RCC_ClkInitTypeDef RCC_ClkInitStruct = {0};

    RCC_OscInitStruct.OscillatorType = RCC_OSCILLATORTYPE_HSI;
    RCC_OscInitStruct.HSIState = RCC_HSI_ON;
    RCC_OscInitStruct.HSICalibrationValue = RCC_HSICALIBRATION_DEFAULT;
    RCC_OscInitStruct.PLL.PLLState = RCC_PLL_ON;
    RCC_OscInitStruct.PLL.PLLSource = RCC_PLLSOURCE_HSI;
    RCC_OscInitStruct.PLL.PLLMUL = RCC_PLL_MUL12;
    RCC_OscInitStruct.PLL.PREDIV = RCC_PREDIV_DIV1;

    if (HAL_RCC_OscConfig(&RCC_OscInitStruct) != HAL_OK)
    {
        Error_Handler();
    }

    RCC_ClkInitStruct.ClockType =
        RCC_CLOCKTYPE_HCLK |
        RCC_CLOCKTYPE_SYSCLK |
        RCC_CLOCKTYPE_PCLK1;

    RCC_ClkInitStruct.SYSCLKSource = RCC_SYSCLKSOURCE_PLLCLK;
    RCC_ClkInitStruct.AHBCLKDivider = RCC_SYSCLK_DIV1;
    RCC_ClkInitStruct.APB1CLKDivider = RCC_HCLK_DIV1;

    if (HAL_RCC_ClockConfig(&RCC_ClkInitStruct, FLASH_LATENCY_1) != HAL_OK)
    {
        Error_Handler();
    }
}

void Error_Handler(void)
{
    __disable_irq();

    while (1)
    {
    }
}
