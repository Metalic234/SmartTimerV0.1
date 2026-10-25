/*
 * File: LCD16X2.c
 * Driver Name: [[ LCD16x2 Display (GPIO 4-Bit Mode) ]]
 * SW Layer:   ECUAL
 * Created on: Jun 28, 2020
 * Author:     Khaled Magdy
 * -------------------------------------------
 * For More Information, Tutorials, etc.
 * Visit Website: www.DeepBlueMbedded.com
 *
 */

#include "LCD16X2.h"
#include "LCD16X2_cfg.h"
#include "F:\Sundry\Projects\Completed documents\stm_timer\Stm32f030f4(timer)firmware\Core\Inc\util\Util.h"

//-----[ Alphanumeric LCD16X2 Functions ]-----

void LCD16X2_DATA(uint8_t LCD16X2_Index, unsigned char Data)
{
    if(Data & 1)
    	HAL_GPIO_WritePin(LCD16X2_CfgParam[LCD16X2_Index].D4_GPIOx, LCD16X2_CfgParam[LCD16X2_Index].D4_PINx, 1);
    else
    	HAL_GPIO_WritePin(LCD16X2_CfgParam[LCD16X2_Index].D4_GPIOx, LCD16X2_CfgParam[LCD16X2_Index].D4_PINx, 0);
    if(Data & 2)
    	HAL_GPIO_WritePin(LCD16X2_CfgParam[LCD16X2_Index].D5_GPIOx, LCD16X2_CfgParam[LCD16X2_Index].D5_PINx, 1);
    else
    	HAL_GPIO_WritePin(LCD16X2_CfgParam[LCD16X2_Index].D5_GPIOx, LCD16X2_CfgParam[LCD16X2_Index].D5_PINx, 0);
    if(Data & 4)
    	HAL_GPIO_WritePin(LCD16X2_CfgParam[LCD16X2_Index].D6_GPIOx, LCD16X2_CfgParam[LCD16X2_Index].D6_PINx, 1);
    else
    	HAL_GPIO_WritePin(LCD16X2_CfgParam[LCD16X2_Index].D6_GPIOx, LCD16X2_CfgParam[LCD16X2_Index].D6_PINx, 0);
    if(Data & 8)
    	HAL_GPIO_WritePin(LCD16X2_CfgParam[LCD16X2_Index].D7_GPIOx, LCD16X2_CfgParam[LCD16X2_Index].D7_PINx, 1);
    else
    	HAL_GPIO_WritePin(LCD16X2_CfgParam[LCD16X2_Index].D7_GPIOx, LCD16X2_CfgParam[LCD16X2_Index].D7_PINx, 0);
}

void LCD16X2_CMD(uint8_t LCD16X2_Index, unsigned char CMD)
{
    // Select Command Register
	HAL_GPIO_WritePin(LCD16X2_CfgParam[LCD16X2_Index].RS_GPIOx, LCD16X2_CfgParam[LCD16X2_Index].RS_PINx, 0);
    // Move The Command Data To LCD
	LCD16X2_DATA(LCD16X2_Index, CMD);
    // Send The EN Clock Signal
    HAL_GPIO_WritePin(LCD16X2_CfgParam[LCD16X2_Index].EN_GPIOx, LCD16X2_CfgParam[LCD16X2_Index].EN_PINx, 0);
    DELAY_US(5);
    HAL_GPIO_WritePin(LCD16X2_CfgParam[LCD16X2_Index].EN_GPIOx, LCD16X2_CfgParam[LCD16X2_Index].EN_PINx, 1);
    DELAY_US(5);
    HAL_GPIO_WritePin(LCD16X2_CfgParam[LCD16X2_Index].EN_GPIOx, LCD16X2_CfgParam[LCD16X2_Index].EN_PINx, 0);
    DELAY_US(100);
}

void LCD16X2_Clear(uint8_t LCD16X2_Index)
{
	LCD16X2_CMD(LCD16X2_Index, 0);
	LCD16X2_CMD(LCD16X2_Index, 1);
    DELAY_MS(2);
}

void LCD16X2_Set_Cursor(uint8_t LCD16X2_Index, unsigned char r, unsigned char c)
{
    unsigned char Temp,Low4,High4;
    if(r == 1)
    {
      Temp  = 0x80 + c - 1; //0x80 is used to move the cursor
      High4 = Temp >> 4;
      Low4  = Temp & 0x0F;
      LCD16X2_CMD(LCD16X2_Index, High4);
      LCD16X2_CMD(LCD16X2_Index, Low4);
    }
    if(r == 2)
    {
      Temp  = 0xC0 + c - 1;
      High4 = Temp >> 4;
      Low4  = Temp & 0x0F;
      LCD16X2_CMD(LCD16X2_Index, High4);
      LCD16X2_CMD(LCD16X2_Index, Low4);
    }
}

void LCD16X2_Init(uint8_t LCD16X2_Index)
{
    /*
     * HD44780 startup:
     * Wait at least 40 ms after LCD power becomes stable.
     * 100 ms is used here for extra robustness.
     */
    DELAY_MS(100);

    /* Make sure control lines start inactive */
    HAL_GPIO_WritePin(
        LCD16X2_CfgParam[LCD16X2_Index].RS_GPIOx,
        LCD16X2_CfgParam[LCD16X2_Index].RS_PINx,
        GPIO_PIN_RESET
    );

    HAL_GPIO_WritePin(
        LCD16X2_CfgParam[LCD16X2_Index].EN_GPIOx,
        LCD16X2_CfgParam[LCD16X2_Index].EN_PINx,
        GPIO_PIN_RESET
    );

    /*
     * Make data bus known.
     * This is especially useful after MCU-only reset,
     * because the LCD itself was NOT reset.
     */
    LCD16X2_DATA(LCD16X2_Index, 0x00);

    DELAY_MS(10);

    /*
     * Force LCD into 8-bit communication state.
     * Send 0x03 three times as required by HD44780 startup.
     */
    LCD16X2_CMD(LCD16X2_Index, 0x03);
    DELAY_MS(5);

    LCD16X2_CMD(LCD16X2_Index, 0x03);
    DELAY_MS(5);

    LCD16X2_CMD(LCD16X2_Index, 0x03);
    DELAY_US(200);

    /*
     * Switch LCD to 4-bit mode.
     */
    LCD16X2_CMD(LCD16X2_Index, 0x02);
    DELAY_US(200);

    /*
     * Function Set:
     * 4-bit
     * 2 lines
     * 5x8 font
     */
    LCD16X2_CMD(LCD16X2_Index, 0x02);
    LCD16X2_CMD(LCD16X2_Index, 0x08);

    /*
     * Display ON
     */
    LCD16X2_CMD(LCD16X2_Index, 0x00);
    LCD16X2_CMD(LCD16X2_Index, 0x0C);

    /*
     * Entry mode
     */
    LCD16X2_CMD(LCD16X2_Index, 0x00);
    LCD16X2_CMD(LCD16X2_Index, 0x06);

    /*
     * Clear display
     */
    LCD16X2_CMD(LCD16X2_Index, 0x00);
    LCD16X2_CMD(LCD16X2_Index, 0x01);

    /*
     * IMPORTANT:
     * CLEAR DISPLAY needs ~1.5 ms.
     */
    DELAY_MS(2);
}

void LCD16X2_Write_Char(uint8_t LCD16X2_Index, char Data)
{
   char Low4,High4;
   Low4  = Data & 0x0F;
   High4 = Data & 0xF0;

   HAL_GPIO_WritePin(LCD16X2_CfgParam[LCD16X2_Index].RS_GPIOx, LCD16X2_CfgParam[LCD16X2_Index].RS_PINx, 1);

   LCD16X2_DATA(LCD16X2_Index, (High4>>4));
   HAL_GPIO_WritePin(LCD16X2_CfgParam[LCD16X2_Index].EN_GPIOx, LCD16X2_CfgParam[LCD16X2_Index].EN_PINx, 0);
   DELAY_US(5);
   HAL_GPIO_WritePin(LCD16X2_CfgParam[LCD16X2_Index].EN_GPIOx, LCD16X2_CfgParam[LCD16X2_Index].EN_PINx, 1);
   DELAY_US(5);
   HAL_GPIO_WritePin(LCD16X2_CfgParam[LCD16X2_Index].EN_GPIOx, LCD16X2_CfgParam[LCD16X2_Index].EN_PINx, 0);
   DELAY_US(100);

   LCD16X2_DATA(LCD16X2_Index, Low4);
   HAL_GPIO_WritePin(LCD16X2_CfgParam[LCD16X2_Index].EN_GPIOx, LCD16X2_CfgParam[LCD16X2_Index].EN_PINx, 0);
   DELAY_US(5);
   HAL_GPIO_WritePin(LCD16X2_CfgParam[LCD16X2_Index].EN_GPIOx, LCD16X2_CfgParam[LCD16X2_Index].EN_PINx, 1);
   DELAY_US(5);
   HAL_GPIO_WritePin(LCD16X2_CfgParam[LCD16X2_Index].EN_GPIOx, LCD16X2_CfgParam[LCD16X2_Index].EN_PINx, 0);
   DELAY_US(100);
}

void LCD16X2_Write_String(uint8_t LCD16X2_Index, char *str)
{
    int i;
    for(i=0; str[i]!='\0'; i++)
    {
    	LCD16X2_Write_Char(LCD16X2_Index, str[i]);
    }
}

void LCD16X2_SL(uint8_t LCD16X2_Index)
{
	LCD16X2_CMD(LCD16X2_Index, 0x01);
	LCD16X2_CMD(LCD16X2_Index, 0x08);
}

void LCD16X2_SR(uint8_t LCD16X2_Index)
{
	LCD16X2_CMD(LCD16X2_Index, 0x01);
	LCD16X2_CMD(LCD16X2_Index, 0x0C);
}
