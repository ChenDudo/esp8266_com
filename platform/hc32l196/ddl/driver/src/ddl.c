/******************************************************************************
 * Copyright (C) 2021, Xiaohua Semiconductor Co., Ltd. All rights reserved.
 *
 * This software component is licensed by XHSC under BSD 3-Clause license
 * (the "License"); You may not use this file except in compliance with the
 * License. You may obtain a copy of the License at:
 *                    opensource.org/licenses/BSD-3-Clause
 *
 ******************************************************************************/

/******************************************************************************
 * @file   ddl.c
 *
 * @brief  Source file for DDL functions
 *
 * @author MADS Team 
 *
 ******************************************************************************/

/******************************************************************************/
/* Include files                                                              */
/******************************************************************************/
#include "ddl.h"

/**
 ******************************************************************************
 ** \addtogroup DDL Common Functions
 ******************************************************************************/
//@{

/******************************************************************************/
/* Local pre-processor symbols/macros ('#define')                             */
/******************************************************************************/

/******************************************************************************/
/* Global variable definitions (declared in header file with 'extern')        */
/******************************************************************************/

/******************************************************************************/
/* Local type definitions ('typedef')                                         */
/******************************************************************************/

/******************************************************************************/
/* Local variable definitions ('static')                                      */
/******************************************************************************/

/******************************************************************************/
/* Local function prototypes ('static')                                       */
/******************************************************************************/

/******************************************************************************/
/* Function implementation - global ('extern') and local ('static')           */
/******************************************************************************/
#ifndef __DEBUG
#define __DEBUG
//#define __CC_ARM
#endif

uint32_t Log2(uint32_t u32Val)
{
    uint32_t u32V1 = 0;
    
    if(0u == u32Val)
    {
        return 0;
    }
    
    while(u32Val > 1u)
    {
        u32V1++;
        u32Val /=2;
    }
    
    return u32V1;
}


/**
 *******************************************************************************
 ** \brief Memory clear function for DDL_ZERO_STRUCT()
 ******************************************************************************/
void ddl_memclr(void *pu8Address, uint32_t u32Count)
{
    uint8_t *pu8Addr = (uint8_t *)pu8Address;
    
    if(NULL == pu8Addr)
    {
        return;
    }
    
    while (u32Count--)
    {
        *pu8Addr++ = 0;
    }
}

/**
 * \brief   delay1ms
 *          delay approximately 1ms.
 * \param   [in]  u32Cnt
 * \retval  void
 */
void delay1ms(uint32_t u32Cnt)
{   
	uint32_t ValNow,CNT = 0;
	uint32_t end = (u32Cnt*(SystemCoreClock/1000));
			   	 
	uint32_t StaVal = SysTick->VAL;        				    
	while(1)
	{
		ValNow=SysTick->VAL;	
		
		if(ValNow != StaVal)
		{			
			if(ValNow<StaVal)
            {
                CNT += StaVal -ValNow;
            }
			else
            {
                CNT += SysTick->LOAD - ValNow+StaVal;
            }				
			StaVal = ValNow;			
			if(CNT >= end ) 
            {
                break;
            }
		}
	}
}

/**
 * \brief   delay100us
 *          delay approximately 100us.
 * \param   [in]  u32Cnt
 * \retval  void
 */
void delay100us(uint32_t u32Cnt)
{
    uint32_t ValNow,CNT = 0;
	uint32_t end = (u32Cnt*(SystemCoreClock/10000));
			   	 
	uint32_t StaVal = SysTick->VAL;        				    
	while(1)
	{
		ValNow=SysTick->VAL;	
		
		if(ValNow != StaVal)
		{			
			if(ValNow<StaVal)
            {
                CNT += StaVal -ValNow;
            }
			else
            {
                CNT += SysTick->LOAD - ValNow+StaVal;
            }
            
			StaVal = ValNow;
            
			if(CNT >=end)
            {
                break;
            }
		}
	}
}

/**
 * \brief   delay10us
 *          delay approximately 10us.
 * \param   [in]  u32Cnt
 * \retval  void
 */
void delay10us(uint32_t u32Cnt)
{
    uint32_t ValNow,CNT = 0;
	   	 
	uint32_t StaVal = SysTick->VAL;        				    
	while(1)
	{
		ValNow=SysTick->VAL;	
		
		if(ValNow != StaVal)
		{			
			if(ValNow<StaVal)
            {
                CNT += StaVal -ValNow;
            }
			else
            {
                CNT += SysTick->LOAD - ValNow+StaVal;
            }
            
			StaVal = ValNow;
            
			if(CNT >=(u32Cnt*(SystemCoreClock/100000)))
            {
                break;
            }
		}
	}
}

/**
 * \brief   set register bit
 *          
 * \param   [in]  addr
 * \param   [in]  offset
 * \retval  void
 */
void SetBit(uint32_t addr, uint32_t offset, boolean_t bFlag)
{
    if(TRUE == bFlag)
    {
        *((volatile uint32_t *)(addr)) |= ((1UL)<<(offset));
    }
    else
    {
        *((volatile uint32_t *)(addr)) &= (~(1UL<<(offset)));
    }    
    
    
}


/**
 * \brief   get register bit
 *          
 * \param   [in]  addr
 * \param   [in]  offset
 * \retval  void
 */
boolean_t GetBit(uint32_t addr, uint32_t offset)
{
    return ((((*((volatile uint32_t *)(addr))) >> (offset)) & 1u) > 0) ? TRUE : FALSE;
}
//@} // DDL Functions

/******************************************************************************
 * EOF (not truncated)
 ******************************************************************************/

