/* ============================================================
 * main.c – SysTick-based non-blocking timer
 *
 * MCU:
 * STM32F030F4P6
 *
 * IMPORTANT LINKER REQUIREMENT:
 * ------------------------------------------------------------
 * The Flash region used by the linker MUST end at 0x08003BFF.
 *
 * 0x08000000 - 0x08003BFF : PROGRAM
 * 0x08003C00 - 0x08003FFF : TIMER SETTINGS
 *
 * The last 1 KB Flash page is reserved exclusively for
 * TimerSettings_t.
 * ============================================================ */

#include "main.h"
#include <stdbool.h>
#include "stm32f0xx_hal.h"


/* ============================================================
 * LCD
 * ============================================================ */

#include <F:\Sundry\Projects\Completed documents\stm_timer\Stm32f030f4(timer)firmware\Core\Inc\LCD16X2\LCD16X2.h>
#include <F:\Sundry\Projects\Completed documents\stm_timer\Stm32f030f4(timer)firmware\Core\Inc\LCD16X2\LCD16X2_cfg.h>

#define MyLCD LCD16X2_1


/* ============================================================
 * FLASH SETTINGS STORAGE
 * ============================================================ */

#define FLASH_SETTINGS_ADDRESS   0x08003C00UL
#define SETTINGS_MAGIC           0x5AA5U


typedef struct
{
    uint16_t magic;
    uint16_t on_hour;
    uint16_t on_minute;
    uint16_t off_hour;
    uint16_t off_minute;
    uint16_t checksum;

} TimerSettings_t;


/* ============================================================
 * FUNCTION PROTOTYPES
 * ============================================================ */

void SystemClock_Config(void);

void Error_Handler(void);

static void MX_GPIO_Init(void);

static void Check_Reenter_Setup_Mode(void);

static void Handle_Buttons_nonblock(void);

static void Run_Cycle_nonblock(void);

static uint16_t Settings_Checksum(
    const TimerSettings_t *settings
);

static bool Save_Settings(void);

static bool Load_Settings(void);

static void Relay_On(void);

static void Relay_Off(void);

static void Format_Setup_Value(
    char *buf,
    int value
);

static void Format_Countdown(
    char *buf,
    uint32_t hh,
    uint32_t mm,
    uint32_t ss
);


/* ============================================================
 * TIMER STATES
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


static volatile enum State timerState = SELECT_MODE;


/* ============================================================
 * TIMER SETTINGS
 * ============================================================ */

static int on_hour    = 0;
static int on_minute  = 0;

static int off_hour   = 0;
static int off_minute = 0;


/* ============================================================
 * RUN-CYCLE VARIABLES
 * ============================================================ */

static uint32_t last_ui_ms = 0;

static bool run_initialized = false;

static uint32_t on_cycle_ms  = 0;
static uint32_t off_cycle_ms = 0;

static uint32_t remaining_ms = 0;

static bool phase_on = true;


/* ============================================================
 * RELAY HELPERS
 *
 * Active LOW:
 *
 * LOW  = ON
 * HIGH = OFF
 * ============================================================ */

static void Relay_On(void)
{
    HAL_GPIO_WritePin(
        GPIOA,
        Relay_Pin,
        GPIO_PIN_RESET
    );
}


static void Relay_Off(void)
{
    HAL_GPIO_WritePin(
        GPIOA,
        Relay_Pin,
        GPIO_PIN_SET
    );
}


/* ============================================================
 * BUTTON DEBOUNCE STRUCTURE
 *
 * Active LOW:
 *
 * 1 = PRESSED
 * 0 = RELEASED
 * ============================================================ */

typedef struct
{
    uint8_t  stable;

    uint8_t  counter;

    bool     pressed_edge;

    uint16_t pin;

} Btn;


static Btn btn_set;
static Btn btn_inc;
static Btn btn_dec;


/* ============================================================
 * READ RAW BUTTON
 *
 * Active LOW:
 * GPIO RESET = button pressed
 * ============================================================ */

static uint8_t ReadBtnRaw(uint16_t pin)
{
    if (
        HAL_GPIO_ReadPin(
            GPIOA,
            pin
        ) == GPIO_PIN_RESET
    )
    {
        return 1;
    }

    return 0;
}


/* ============================================================
 * FORMAT SETUP VALUE
 *
 * Produces exactly 16 characters.
 * ============================================================ */

static void Format_Setup_Value(
    char *buf,
    int value
)
{
    int i;


    for (i = 0; i < 16; i++)
    {
        buf[i] = ' ';
    }


    buf[6] =
        (char)('0' + ((value / 10) % 10));


    buf[7] =
        (char)('0' + (value % 10));


    buf[16] =
        '\0';
}


/* ============================================================
 * FORMAT COUNTDOWN
 *
 * Produces exactly 16 characters.
 * ============================================================ */

static void Format_Countdown(
    char *buf,
    uint32_t hh,
    uint32_t mm,
    uint32_t ss
)
{
    int i;


    for (i = 0; i < 16; i++)
    {
        buf[i] = ' ';
    }


    if (hh > 0)
    {
        buf[0] =
            (char)('0' + ((hh / 10U) % 10U));

        buf[1] =
            (char)('0' + (hh % 10U));


        buf[2] = ':';


        buf[3] =
            (char)('0' + ((mm / 10U) % 10U));

        buf[4] =
            (char)('0' + (mm % 10U));


        buf[5] = ':';


        buf[6] =
            (char)('0' + ((ss / 10U) % 10U));

        buf[7] =
            (char)('0' + (ss % 10U));
    }

    else
    {
        buf[3] =
            (char)('0' + ((mm / 10U) % 10U));

        buf[4] =
            (char)('0' + (mm % 10U));


        buf[5] = ':';


        buf[6] =
            (char)('0' + ((ss / 10U) % 10U));

        buf[7] =
            (char)('0' + (ss % 10U));
    }


    buf[16] =
        '\0';
}


/* ============================================================
 * RE-ENTER SETUP MODE
 *
 * Hold INC + DEC simultaneously for 5 seconds.
 *
 * Active LOW buttons.
 * ============================================================ */

static void Check_Reenter_Setup_Mode(void)
{
    static uint32_t both_buttons_start = 0;

    static uint8_t both_buttons_active = 0;

    GPIO_PinState inc_state;

    GPIO_PinState dec_state;

    uint32_t now;


    inc_state =
        HAL_GPIO_ReadPin(
            GPIOA,
            Inc_Pin
        );


    dec_state =
        HAL_GPIO_ReadPin(
            GPIOA,
            Dec_Pin
        );


    now =
        HAL_GetTick();


    /* --------------------------------------------------------
     * Both buttons pressed
     * -------------------------------------------------------- */

    if (
        (inc_state == GPIO_PIN_RESET) &&
        (dec_state == GPIO_PIN_RESET)
    )
    {
        if (!both_buttons_active)
        {
            both_buttons_active =
                1;

            both_buttons_start =
                now;
        }

        else
        {
            if (
                (now - both_buttons_start)
                >= 5000UL
            )
            {
                if (
                    timerState != SELECT_MODE
                )
                {
                    /* ----------------------------------------
                     * Stop relay
                     * ---------------------------------------- */

                    Relay_Off();


                    /* ----------------------------------------
                     * Enter setup
                     * ---------------------------------------- */

                    timerState =
                        SELECT_MODE;


                    run_initialized =
                        false;


                    phase_on =
                        true;


                    remaining_ms =
                        0;


                    /* ----------------------------------------
                     * Reset UI timer so SELECT screen updates
                     * immediately on the next update cycle.
                     * ---------------------------------------- */

                    last_ui_ms =
                        now;


                    LCD16X2_Clear(
                        MyLCD
                    );


                    both_buttons_active =
                        0;
                }

                else
                {
                    /*
                     * Prevent continuous retriggering while
                     * buttons remain held.
                     */

                    both_buttons_start =
                        now;
                }
            }
        }
    }

    else
    {
        both_buttons_active =
            0;
    }
}


/* ============================================================
 * FLASH CHECKSUM
 * ============================================================ */

static uint16_t Settings_Checksum(
    const TimerSettings_t *settings
)
{
    uint16_t checksum = 0;


    checksum ^=
        settings->magic;


    checksum ^=
        settings->on_hour;


    checksum ^=
        settings->on_minute;


    checksum ^=
        settings->off_hour;


    checksum ^=
        settings->off_minute;


    return checksum;
}


/* ============================================================
 * SAVE SETTINGS TO FLASH
 *
 * Procedure:
 *
 * 1. Build settings structure.
 * 2. Calculate checksum.
 * 3. Erase last page.
 * 4. Write everything except magic.
 * 5. Write magic LAST as commit marker.
 * 6. Lock Flash.
 * 7. Verify the complete structure.
 *
 * Returns:
 *
 * true  = save successful
 * false = save failed
 * ============================================================ */

static bool Save_Settings(void)
{
    HAL_StatusTypeDef status;

    FLASH_EraseInitTypeDef EraseInitStruct;

    uint32_t PageError = 0;

    TimerSettings_t settings;

    const uint16_t *data;

    uint32_t halfwords;

    uint32_t i;

    const TimerSettings_t *verify;


    /* --------------------------------------------------------
     * Build settings
     * -------------------------------------------------------- */

    settings.magic =
        SETTINGS_MAGIC;


    settings.on_hour =
        (uint16_t)on_hour;


    settings.on_minute =
        (uint16_t)on_minute;


    settings.off_hour =
        (uint16_t)off_hour;


    settings.off_minute =
        (uint16_t)off_minute;


    settings.checksum =
        Settings_Checksum(
            &settings
        );


    /* --------------------------------------------------------
     * Unlock Flash
     * -------------------------------------------------------- */

    status =
        HAL_FLASH_Unlock();


    if (
        status != HAL_OK
    )
    {
        return false;
    }


    /* --------------------------------------------------------
     * Prepare erase
     * -------------------------------------------------------- */

    EraseInitStruct.TypeErase =
        FLASH_TYPEERASE_PAGES;


    EraseInitStruct.PageAddress =
        FLASH_SETTINGS_ADDRESS;


    EraseInitStruct.NbPages =
        1;


    /* --------------------------------------------------------
     * Erase settings page
     * -------------------------------------------------------- */

    status =
        HAL_FLASHEx_Erase(
            &EraseInitStruct,
            &PageError
        );


    if (
        status != HAL_OK
    )
    {
        HAL_FLASH_Lock();

        return false;
    }


    /* --------------------------------------------------------
     * Pointer to settings as halfwords
     * -------------------------------------------------------- */

    data =
        (const uint16_t *)&settings;


    halfwords =
        sizeof(TimerSettings_t) / 2U;


    /* --------------------------------------------------------
     * Write all fields except magic
     *
     * This includes:
     *
     * on_hour
     * on_minute
     * off_hour
     * off_minute
     * checksum
     * -------------------------------------------------------- */

    for (
        i = 1;
        i < halfwords;
        i++
    )
    {
        status =
            HAL_FLASH_Program(
                FLASH_TYPEPROGRAM_HALFWORD,
                FLASH_SETTINGS_ADDRESS + (i * 2U),
                (uint64_t)data[i]
            );


        if (
            status != HAL_OK
        )
        {
            HAL_FLASH_Lock();

            return false;
        }
    }


    /* --------------------------------------------------------
     * Write MAGIC LAST
     *
     * This acts as a commit marker.
     * -------------------------------------------------------- */

    status =
        HAL_FLASH_Program(
            FLASH_TYPEPROGRAM_HALFWORD,
            FLASH_SETTINGS_ADDRESS,
            (uint64_t)SETTINGS_MAGIC
        );


    if (
        status != HAL_OK
    )
    {
        HAL_FLASH_Lock();

        return false;
    }


    /* --------------------------------------------------------
     * Lock Flash
     * -------------------------------------------------------- */

    HAL_FLASH_Lock();


    /* --------------------------------------------------------
     * Verify readback
     * -------------------------------------------------------- */

    verify =
        (const TimerSettings_t *)
        FLASH_SETTINGS_ADDRESS;


    if (
        verify->magic != SETTINGS_MAGIC
    )
    {
        return false;
    }


    if (
        verify->on_hour !=
        settings.on_hour
    )
    {
        return false;
    }


    if (
        verify->on_minute !=
        settings.on_minute
    )
    {
        return false;
    }


    if (
        verify->off_hour !=
        settings.off_hour
    )
    {
        return false;
    }


    if (
        verify->off_minute !=
        settings.off_minute
    )
    {
        return false;
    }


    if (
        verify->checksum !=
        settings.checksum
    )
    {
        return false;
    }


    return true;
}


/* ============================================================
 * LOAD SETTINGS FROM FLASH
 * ============================================================ */

static bool Load_Settings(void)
{
    const TimerSettings_t *flash_settings;

    TimerSettings_t settings;


    flash_settings =
        (const TimerSettings_t *)
        FLASH_SETTINGS_ADDRESS;


    settings =
        *flash_settings;


    /* --------------------------------------------------------
     * Magic
     * -------------------------------------------------------- */

    if (
        settings.magic != SETTINGS_MAGIC
    )
    {
        return false;
    }


    /* --------------------------------------------------------
     * Checksum
     * -------------------------------------------------------- */

    if (
        settings.checksum !=
        Settings_Checksum(
            &settings
        )
    )
    {
        return false;
    }


    /* --------------------------------------------------------
     * Range validation
     * -------------------------------------------------------- */

    if (
        settings.on_hour >= 24
    )
    {
        return false;
    }


    if (
        settings.on_minute >= 60
    )
    {
        return false;
    }


    if (
        settings.off_hour >= 24
    )
    {
        return false;
    }


    if (
        settings.off_minute >= 60
    )
    {
        return false;
    }


    /* --------------------------------------------------------
     * Copy into active variables
     * -------------------------------------------------------- */

    on_hour =
        settings.on_hour;


    on_minute =
        settings.on_minute;


    off_hour =
        settings.off_hour;


    off_minute =
        settings.off_minute;


    return true;
}


/* ============================================================
 * BUTTON SAMPLING
 *
 * Sample every 10 ms.
 * Require 50 ms stable state.
 * ============================================================ */

#define SAMPLE_BUTTON(B)                                      \
    do                                                        \
    {                                                         \
        uint8_t raw = ReadBtnRaw((B).pin);                    \
                                                              \
        if (raw != (B).stable)                                \
        {                                                     \
            (B).counter += 10U;                               \
                                                              \
            if ((B).counter >= 50U)                           \
            {                                                 \
                if (                                             \
                    (raw == 1U) &&                            \
                    ((B).stable == 0U)                        \
                )                                             \
                {                                             \
                    (B).pressed_edge = true;                  \
                }                                             \
                                                              \
                (B).stable = raw;                             \
                (B).counter = 0;                             \
            }                                                 \
        }                                                     \
        else                                                  \
        {                                                     \
            (B).counter = 0;                                  \
        }                                                     \
    } while (0)


/* ============================================================
 * MAIN
 * ============================================================ */

int main(void)
{
    uint32_t tstart;

    uint32_t last_sample_ms = 0;


    /* ========================================================
     * HAL
     * ======================================================== */

    HAL_Init();


    /* ========================================================
     * SYSTEM CLOCK
     * ======================================================== */

    SystemClock_Config();


    /* ========================================================
     * GPIO
     * ======================================================== */

    MX_GPIO_Init();


    /* ========================================================
     * BUTTON INITIALIZATION
     * ======================================================== */

    btn_set.pin =
        Set_Pin;

    btn_set.stable =
        ReadBtnRaw(
            Set_Pin
        );

    btn_set.counter =
        0;

    btn_set.pressed_edge =
        false;


    btn_inc.pin =
        Inc_Pin;

    btn_inc.stable =
        ReadBtnRaw(
            Inc_Pin
        );

    btn_inc.counter =
        0;

    btn_inc.pressed_edge =
        false;


    btn_dec.pin =
        Dec_Pin;

    btn_dec.stable =
        ReadBtnRaw(
            Dec_Pin
        );

    btn_dec.counter =
        0;

    btn_dec.pressed_edge =
        false;


    /* ========================================================
     * LCD BACKLIGHT
     * ======================================================== */

    HAL_GPIO_WritePin(
        Backlit_GPIO_Port,
        Backlit_Pin,
        GPIO_PIN_SET
    );


    /* ========================================================
     * LCD SPLASH
     * ======================================================== */

    LCD16X2_Init(
        MyLCD
    );


    LCD16X2_Clear(
        MyLCD
    );


    LCD16X2_Set_Cursor(
        MyLCD,
        1,
        1
    );


    LCD16X2_Write_String(
        MyLCD,
        "Adjustable_Timer"
    );


    tstart =
        HAL_GetTick();


    while (
        (HAL_GetTick() - tstart)
        < 2900UL
    )
    {
        /* Splash delay */
    }


    LCD16X2_Clear(
        MyLCD
    );


    /* ========================================================
     * LOAD SAVED SETTINGS
     * ======================================================== */

    if (
        Load_Settings()
    )
    {
        /* ----------------------------------------------------
         * Valid settings found.
         *
         * Automatically start timer.
         * ---------------------------------------------------- */

        timerState =
            RUN_CYCLE;


        run_initialized =
            false;


        phase_on =
            true;
    }

    else
    {
        /* ----------------------------------------------------
         * No valid saved settings.
         *
         * Enter setup.
         * ---------------------------------------------------- */

        timerState =
            SELECT_MODE;


        on_hour =
            0;

        on_minute =
            0;

        off_hour =
            0;

        off_minute =
            0;


        run_initialized =
            false;


        phase_on =
            true;
    }


    /* ========================================================
     * MAIN LOOP
     * ======================================================== */

    while (1)
    {
        uint32_t now;


        now =
            HAL_GetTick();


        /* ====================================================
         * CHECK RE-ENTER SETUP
         * ==================================================== */

        Check_Reenter_Setup_Mode();


        /* ====================================================
         * BUTTON SAMPLE
         * ==================================================== */

        if (
            (now - last_sample_ms)
            >= 10UL
        )
        {
            last_sample_ms =
                now;


            SAMPLE_BUTTON(btn_set);

            SAMPLE_BUTTON(btn_inc);

            SAMPLE_BUTTON(btn_dec);
        }


        /* ====================================================
         * BUTTON STATE MACHINE
         * ==================================================== */

        Handle_Buttons_nonblock();


        /* ====================================================
         * RUN TIMER
         * ==================================================== */

        if (
            timerState == RUN_CYCLE
        )
        {
            Run_Cycle_nonblock();
        }


        /* ====================================================
         * SETUP SCREEN UPDATE
         * ==================================================== */

        if (
            (now - last_ui_ms)
            >= 200UL
        )
        {
            char buf[17];


            last_ui_ms =
                now;


            /* ------------------------------------------------
             * SELECT MODE
             * ------------------------------------------------ */

            if (
                timerState == SELECT_MODE
            )
            {
                LCD16X2_Clear(
                    MyLCD
                );


                LCD16X2_Set_Cursor(
                    MyLCD,
                    1,
                    1
                );


                LCD16X2_Write_String(
                    MyLCD,
                    "Press SET to"
                );


                LCD16X2_Set_Cursor(
                    MyLCD,
                    2,
                    1
                );


                LCD16X2_Write_String(
                    MyLCD,
                    "start setup..."
                );
            }


            /* ------------------------------------------------
             * ON HOUR
             * ------------------------------------------------ */

            else if (
                timerState == SET_ON_HOUR
            )
            {
                LCD16X2_Clear(
                    MyLCD
                );


                LCD16X2_Set_Cursor(
                    MyLCD,
                    1,
                    1
                );


                LCD16X2_Write_String(
                    MyLCD,
                    "Set ON Hour:"
                );


                Format_Setup_Value(
                    buf,
                    on_hour
                );


                LCD16X2_Set_Cursor(
                    MyLCD,
                    2,
                    1
                );


                LCD16X2_Write_String(
                    MyLCD,
                    buf
                );
            }


            /* ------------------------------------------------
             * ON MINUTE
             * ------------------------------------------------ */

            else if (
                timerState == SET_ON_MINUTE
            )
            {
                LCD16X2_Clear(
                    MyLCD
                );


                LCD16X2_Set_Cursor(
                    MyLCD,
                    1,
                    1
                );


                LCD16X2_Write_String(
                    MyLCD,
                    "Set ON Min:"
                );


                Format_Setup_Value(
                    buf,
                    on_minute
                );


                LCD16X2_Set_Cursor(
                    MyLCD,
                    2,
                    1
                );


                LCD16X2_Write_String(
                    MyLCD,
                    buf
                );
            }


            /* ------------------------------------------------
             * OFF HOUR
             * ------------------------------------------------ */

            else if (
                timerState == SET_OFF_HOUR
            )
            {
                LCD16X2_Clear(
                    MyLCD
                );


                LCD16X2_Set_Cursor(
                    MyLCD,
                    1,
                    1
                );


                LCD16X2_Write_String(
                    MyLCD,
                    "Set OFF Hour:"
                );


                Format_Setup_Value(
                    buf,
                    off_hour
                );


                LCD16X2_Set_Cursor(
                    MyLCD,
                    2,
                    1
                );


                LCD16X2_Write_String(
                    MyLCD,
                    buf
                );
            }


            /* ------------------------------------------------
             * OFF MINUTE
             * ------------------------------------------------ */

            else if (
                timerState == SET_OFF_MINUTE
            )
            {
                LCD16X2_Clear(
                    MyLCD
                );


                LCD16X2_Set_Cursor(
                    MyLCD,
                    1,
                    1
                );


                LCD16X2_Write_String(
                    MyLCD,
                    "Set OFF Min:"
                );


                Format_Setup_Value(
                    buf,
                    off_minute
                );


                LCD16X2_Set_Cursor(
                    MyLCD,
                    2,
                    1
                );


                LCD16X2_Write_String(
                    MyLCD,
                    buf
                );
            }
        }
    }
}


/* ============================================================
 * BUTTON HANDLER
 * ============================================================ */

static void Handle_Buttons_nonblock(void)
{
    /* ========================================================
     * SELECT MODE
     * ======================================================== */

    if (
        timerState == SELECT_MODE
    )
    {
        if (
            btn_set.pressed_edge
        )
        {
            btn_set.pressed_edge =
                false;


            timerState =
                SET_ON_HOUR;
        }
    }


    /* ========================================================
     * SET ON HOUR
     * ======================================================== */

    else if (
        timerState == SET_ON_HOUR
    )
    {
        if (
            btn_inc.pressed_edge
        )
        {
            btn_inc.pressed_edge =
                false;


            on_hour =
                (on_hour + 1) % 24;
        }

        else if (
            btn_dec.pressed_edge
        )
        {
            btn_dec.pressed_edge =
                false;


            on_hour =
                (on_hour - 1 + 24) % 24;
        }

        else if (
            btn_set.pressed_edge
        )
        {
            btn_set.pressed_edge =
                false;


            timerState =
                SET_ON_MINUTE;
        }
    }


    /* ========================================================
     * SET ON MINUTE
     * ======================================================== */

    else if (
        timerState == SET_ON_MINUTE
    )
    {
        if (
            btn_inc.pressed_edge
        )
        {
            btn_inc.pressed_edge =
                false;


            on_minute =
                (on_minute + 1) % 60;
        }

        else if (
            btn_dec.pressed_edge
        )
        {
            btn_dec.pressed_edge =
                false;


            on_minute =
                (on_minute - 1 + 60) % 60;
        }

        else if (
            btn_set.pressed_edge
        )
        {
            btn_set.pressed_edge =
                false;


            timerState =
                SET_OFF_HOUR;
        }
    }


    /* ========================================================
     * SET OFF HOUR
     * ======================================================== */

    else if (
        timerState == SET_OFF_HOUR
    )
    {
        if (
            btn_inc.pressed_edge
        )
        {
            btn_inc.pressed_edge =
                false;


            off_hour =
                (off_hour + 1) % 24;
        }

        else if (
            btn_dec.pressed_edge
        )
        {
            btn_dec.pressed_edge =
                false;


            off_hour =
                (off_hour - 1 + 24) % 24;
        }

        else if (
            btn_set.pressed_edge
        )
        {
            btn_set.pressed_edge =
                false;


            timerState =
                SET_OFF_MINUTE;
        }
    }


    /* ========================================================
     * SET OFF MINUTE
     * ======================================================== */

    else if (
        timerState == SET_OFF_MINUTE
    )
    {
        if (
            btn_inc.pressed_edge
        )
        {
            btn_inc.pressed_edge =
                false;


            off_minute =
                (off_minute + 1) % 60;
        }

        else if (
            btn_dec.pressed_edge
        )
        {
            btn_dec.pressed_edge =
                false;


            off_minute =
                (off_minute - 1 + 60) % 60;
        }

        else if (
            btn_set.pressed_edge
        )
        {
            uint32_t t;

            btn_set.pressed_edge =
                false;


            /* ------------------------------------------------
             * Calculate ON duration
             * ------------------------------------------------ */

            on_cycle_ms =
                (
                    ((uint32_t)on_hour * 3600UL)
                    +
                    ((uint32_t)on_minute * 60UL)
                )
                * 1000UL;


            /* ------------------------------------------------
             * Calculate OFF duration
             * ------------------------------------------------ */

            off_cycle_ms =
                (
                    ((uint32_t)off_hour * 3600UL)
                    +
                    ((uint32_t)off_minute * 60UL)
                )
                * 1000UL;


            /* ------------------------------------------------
             * Zero duration is invalid
             * ------------------------------------------------ */

            if (
                (on_cycle_ms == 0) ||
                (off_cycle_ms == 0)
            )
            {
                LCD16X2_Clear(
                    MyLCD
                );


                LCD16X2_Set_Cursor(
                    MyLCD,
                    1,
                    1
                );


                LCD16X2_Write_String(
                    MyLCD,
                    "   Error!"
                );


                LCD16X2_Set_Cursor(
                    MyLCD,
                    2,
                    1
                );


                LCD16X2_Write_String(
                    MyLCD,
                    " Time cannot be 0"
                );


                t =
                    HAL_GetTick();


                while (
                    (HAL_GetTick() - t)
                    < 2000UL
                )
                {
                    /* 2 second error message */
                }


                timerState =
                    SELECT_MODE;


                last_ui_ms =
                    HAL_GetTick();
            }

            else
            {
                /* --------------------------------------------
                 * Save settings.
                 * -------------------------------------------- */

                if (
                    Save_Settings()
                )
                {
                    /* ----------------------------------------
                     * Flash save succeeded.
                     *
                     * Start timer.
                     * ---------------------------------------- */

                    timerState =
                        RUN_CYCLE;


                    run_initialized =
                        false;


                    phase_on =
                        true;


                    remaining_ms =
                        0;
                }

                else
                {
                    /* ----------------------------------------
                     * Flash save failed.
                     *
                     * DO NOT start timer.
                     * ---------------------------------------- */

                    LCD16X2_Clear(
                        MyLCD
                    );


                    LCD16X2_Set_Cursor(
                        MyLCD,
                        1,
                        1
                    );


                    LCD16X2_Write_String(
                        MyLCD,
                        " Flash error!"
                    );


                    LCD16X2_Set_Cursor(
                        MyLCD,
                        2,
                        1
                    );


                    LCD16X2_Write_String(
                        MyLCD,
                        " Settings NOT saved"
                    );


                    t =
                        HAL_GetTick();


                    while (
                        (HAL_GetTick() - t)
                        < 2000UL
                    )
                    {
                        /* 2 second error message */
                    }


                    timerState =
                        SELECT_MODE;


                    last_ui_ms =
                        HAL_GetTick();
                }
            }
        }
    }


    /* ========================================================
     * RUN CYCLE
     * ======================================================== */

    else if (
        timerState == RUN_CYCLE
    )
    {
        if (
            btn_set.pressed_edge
        )
        {
            btn_set.pressed_edge =
                false;


            timerState =
                SELECT_MODE;


            run_initialized =
                false;


            phase_on =
                true;


            remaining_ms =
                0;


            Relay_Off();


            last_ui_ms =
                HAL_GetTick();
        }
    }
}


/* ============================================================
 * RUN TIMER CYCLE
 *
 * Uses HAL_GetTick().
 *
 * Important:
 * uint32_t subtraction naturally handles SysTick rollover.
 * ============================================================ */

static void Run_Cycle_nonblock(void)
{
    static uint32_t last_ms = 0;

    static uint32_t last_lcd_refresh = 0;

    uint32_t now;

    uint32_t delta;

    char buf[17];

    uint32_t total_seconds;

    uint32_t hh;

    uint32_t mm;

    uint32_t ss;


    now =
        HAL_GetTick();


    /* ========================================================
     * FIRST ENTRY INITIALIZATION
     * ======================================================== */

    if (
        !run_initialized
    )
    {
        /* ----------------------------------------------------
         * Calculate durations
         * ---------------------------------------------------- */

        on_cycle_ms =
            (
                ((uint32_t)on_hour * 3600UL)
                +
                ((uint32_t)on_minute * 60UL)
            )
            * 1000UL;


        off_cycle_ms =
            (
                ((uint32_t)off_hour * 3600UL)
                +
                ((uint32_t)off_minute * 60UL)
            )
            * 1000UL;


        /* ----------------------------------------------------
         * Start ON phase
         * ---------------------------------------------------- */

        remaining_ms =
            on_cycle_ms;


        phase_on =
            true;


        Relay_On();


        run_initialized =
            true;


        last_ms =
            now;


        last_lcd_refresh =
            now;


        /* ----------------------------------------------------
         * Return immediately.
         *
         * This avoids calculating an artificial delta during
         * the same invocation in which the timer starts.
         * ---------------------------------------------------- */

        return;
    }


    /* ========================================================
     * ELAPSED TIME
     * ======================================================== */

    delta =
        now - last_ms;


    /* --------------------------------------------------------
     * No new SysTick yet.
     *
     * IMPORTANT:
     * Do not change last_ms here.
     * -------------------------------------------------------- */

    if (
        delta == 0
    )
    {
        return;
    }


    /* --------------------------------------------------------
     * Update baseline
     * -------------------------------------------------------- */

    last_ms =
        now;


    /* ========================================================
     * DECREASE REMAINING TIME
     * ======================================================== */

    if (
        remaining_ms > delta
    )
    {
        remaining_ms -=
            delta;
    }

    else
    {
        remaining_ms =
            0;
    }


    /* ========================================================
     * PHASE TRANSITION
     * ======================================================== */

    if (
        remaining_ms == 0
    )
    {
        phase_on =
            !phase_on;


        /* ----------------------------------------------------
         * ON phase
         * ---------------------------------------------------- */

        if (
            phase_on
        )
        {
            Relay_On();


            remaining_ms =
                on_cycle_ms;
        }

        /* ----------------------------------------------------
         * OFF phase
         * ---------------------------------------------------- */

        else
        {
            Relay_Off();


            remaining_ms =
                off_cycle_ms;
        }
    }


    /* ========================================================
     * LCD REFRESH
     *
     * Only update every 250 ms.
     * ======================================================== */

    if (
        (now - last_lcd_refresh)
        < 250UL
    )
    {
        return;
    }


    last_lcd_refresh =
        now;


    /* ========================================================
     * COUNTDOWN CALCULATION
     * ======================================================== */

    total_seconds =
        remaining_ms / 1000UL;


    hh =
        total_seconds / 3600UL;


    mm =
        (total_seconds % 3600UL) / 60UL;


    ss =
        total_seconds % 60UL;


    Format_Countdown(
        buf,
        hh,
        mm,
        ss
    );


    /* ========================================================
     * LCD LINE 1
     * ======================================================== */

    LCD16X2_Set_Cursor(
        MyLCD,
        1,
        1
    );


    if (
        phase_on
    )
    {
        LCD16X2_Write_String(
            MyLCD,
            "Loop (ON)       "
        );
    }

    else
    {
        LCD16X2_Write_String(
            MyLCD,
            "Loop (OFF)      "
        );
    }


    /* ========================================================
     * LCD LINE 2
     * ======================================================== */

    LCD16X2_Set_Cursor(
        MyLCD,
        2,
        1
    );


    LCD16X2_Write_String(
        MyLCD,
        buf
    );
}


/* ============================================================
 * GPIO INITIALIZATION
 *
 * Active LOW buttons use internal pull-ups.
 * ============================================================ */

static void MX_GPIO_Init(void)
{
    GPIO_InitTypeDef GPIO_InitStruct =
        {0};


    /* ========================================================
     * ENABLE GPIO CLOCKS
     * ======================================================== */

    __HAL_RCC_GPIOA_CLK_ENABLE();

    __HAL_RCC_GPIOB_CLK_ENABLE();


    /* ========================================================
     * DEFAULT OUTPUT STATES
     * ======================================================== */

    HAL_GPIO_WritePin(
        GPIOA,
        Backlit_Pin |
        Relay_Pin,
        GPIO_PIN_SET
    );


    HAL_GPIO_WritePin(
        E_GPIO_Port,
        E_Pin,
        GPIO_PIN_RESET
    );


    /* ========================================================
     * LCD DATA + RS + BACKLIGHT + RELAY
     *
     * These are GPIOA pins in the current design.
     * ======================================================== */

    GPIO_InitStruct.Pin =
        D4_Pin |
        D5_Pin |
        D6_Pin |
        D7_Pin |
        RS_Pin |
        Backlit_Pin |
        Relay_Pin;


    GPIO_InitStruct.Mode =
        GPIO_MODE_OUTPUT_PP;


    GPIO_InitStruct.Pull =
        GPIO_NOPULL;


    GPIO_InitStruct.Speed =
        GPIO_SPEED_FREQ_LOW;


    HAL_GPIO_Init(
        GPIOA,
        &GPIO_InitStruct
    );


    /* ========================================================
     * LCD E PIN
     * ======================================================== */

    GPIO_InitStruct.Pin =
        E_Pin;


    GPIO_InitStruct.Mode =
        GPIO_MODE_OUTPUT_PP;


    GPIO_InitStruct.Pull =
        GPIO_NOPULL;


    GPIO_InitStruct.Speed =
        GPIO_SPEED_FREQ_LOW;


    HAL_GPIO_Init(
        E_GPIO_Port,
        &GPIO_InitStruct
    );


    /* ========================================================
     * BUTTON INPUTS
     *
     * Active LOW:
     *
     * released = HIGH
     * pressed  = LOW
     *
     * Internal pull-up enabled.
     * ======================================================== */

    GPIO_InitStruct.Pin =
        Set_Pin |
        Inc_Pin |
        Dec_Pin;


    GPIO_InitStruct.Mode =
        GPIO_MODE_INPUT;


    GPIO_InitStruct.Pull =
        GPIO_PULLUP;


    GPIO_InitStruct.Speed =
        GPIO_SPEED_FREQ_LOW;


    HAL_GPIO_Init(
        GPIOA,
        &GPIO_InitStruct
    );
}


/* ============================================================
 * SYSTEM CLOCK
 *
 * HSI = 8 MHz
 * PLL x12
 * SYSCLK = 48 MHz
 * ============================================================ */

void SystemClock_Config(void)
{
    RCC_OscInitTypeDef RCC_OscInitStruct =
        {0};


    RCC_ClkInitTypeDef RCC_ClkInitStruct =
        {0};


    /* ========================================================
     * OSCILLATOR
     * ======================================================== */

    RCC_OscInitStruct.OscillatorType =
        RCC_OSCILLATORTYPE_HSI;


    RCC_OscInitStruct.HSIState =
        RCC_HSI_ON;


    RCC_OscInitStruct.HSICalibrationValue =
        RCC_HSICALIBRATION_DEFAULT;


    RCC_OscInitStruct.PLL.PLLState =
        RCC_PLL_ON;


    RCC_OscInitStruct.PLL.PLLSource =
        RCC_PLLSOURCE_HSI;


    RCC_OscInitStruct.PLL.PLLMUL =
        RCC_PLL_MUL12;


    RCC_OscInitStruct.PLL.PREDIV =
        RCC_PREDIV_DIV1;


    if (
        HAL_RCC_OscConfig(
            &RCC_OscInitStruct
        ) != HAL_OK
    )
    {
        Error_Handler();
    }


    /* ========================================================
     * CLOCK CONFIGURATION
     * ======================================================== */

    RCC_ClkInitStruct.ClockType =
        RCC_CLOCKTYPE_HCLK |
        RCC_CLOCKTYPE_SYSCLK |
        RCC_CLOCKTYPE_PCLK1;


    RCC_ClkInitStruct.SYSCLKSource =
        RCC_SYSCLKSOURCE_PLLCLK;


    RCC_ClkInitStruct.AHBCLKDivider =
        RCC_SYSCLK_DIV1;


    RCC_ClkInitStruct.APB1CLKDivider =
        RCC_HCLK_DIV1;


    if (
        HAL_RCC_ClockConfig(
            &RCC_ClkInitStruct,
            FLASH_LATENCY_1
        ) != HAL_OK
    )
    {
        Error_Handler();
    }
}


/* ============================================================
 * ERROR HANDLER
 * ============================================================ */

void Error_Handler(void)
{
    __disable_irq();


    while (1)
    {
    }
}