/*
 *
 * The MIT License (MIT)
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to deal
 * in the Software without restriction, including without limitation the rights
 * to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 * copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 * AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
 * THE SOFTWARE.
 */

#include <stdio.h>
#include <string.h>
#include <math.h>
#include <stdlib.h>
#include <unistd.h>

#include "py/nlr.h"
#include "py/runtime.h"

#if MICROPY_HW_HAS_NEOPIX


#include "neopix.h"
#include "ti/devices/msp432e4/driverlib/driverlib.h"
#include <ti/drivers/Power.h>
#include <ti/drivers/power/PowerMSP432E4.h>
#include <ti/drivers/dpl/HwiP.h>



/// \moduleref Neopix
/// UPDATE ME

static uint16_t * frame_buffer = NULL;
static uint32_t frame_buffer_size = 0;

static volatile int inprogress = 0;

#define WS_800HZ 800000
#define WS_400HZ 400000
#define WS_FREQ WS_800HZ
#define SYSCLOCK 120000000
#define WS2812_TIMER_INTVAL     (SYSCLOCK/WS_FREQ)
#define WS2812_DUTYCYCLE_0      (((25-8)*WS2812_TIMER_INTVAL)/25)
#define WS2812_DUTYCYCLE_1      (((25-16)*WS2812_TIMER_INTVAL)/25)
#define WS2812_DUTYCYCLE_RESET  (WS2812_TIMER_INTVAL)
#define WS2812_RESET_BIT_N      30

static void setup_ws_timer_dma(void);

typedef struct _pyb_neopix_t {
    mp_obj_base_t base;
} pyb_neopix_obj_t;


/// \classmethod \constructor(pin)
///
/// Construct an Neopix object. 
STATIC mp_obj_t pyb_neopix_make_new(const mp_obj_type_t *type, mp_uint_t n_args, mp_uint_t n_kw, const mp_obj_t *args) {
    // check arguments
    mp_arg_check_num(n_args, n_kw, 0, 0, false);

    // create object
    pyb_neopix_obj_t *neo = m_new_obj(pyb_neopix_obj_t);
    neo->base.type = &pyb_neopix_type;
    
    setup_ws_timer_dma();
	
	return neo;
}

// called when the dma transfer is complete
void
TIMER3B_IRQHandler(unsigned int a)
{
    uint32_t getTimerIntStatus;

    // Get the timer interrupt status and clear the same 
    getTimerIntStatus = MAP_TimerIntStatus(TIMER3_BASE, true);
    MAP_TimerIntClear(TIMER3_BASE, getTimerIntStatus);

    MAP_TimerDisable(TIMER3_BASE, TIMER_BOTH);

    inprogress = 0;
    
}

static void setup_ws_timer_dma(void)
{
    //MAP_SysCtlPeripheralEnable(SYSCTL_PERIPH_GPIOD);
    Power_setDependency(PowerMSP432E4_PERIPH_TIMER3);
    MAP_GPIOPinConfigure(GPIO_PD4_T3CCP0);
    MAP_GPIOPinTypeTimer(GPIO_PORTD_BASE, GPIO_PIN_4);

    MAP_SysCtlPeripheralEnable(SYSCTL_PERIPH_TIMER3);
    MAP_TimerConfigure(TIMER3_BASE, TIMER_CFG_SPLIT_PAIR | TIMER_CFG_A_PWM | TIMER_CFG_B_PERIODIC_UP);

    MAP_TimerLoadSet(TIMER3_BASE, TIMER_BOTH, WS2812_TIMER_INTVAL);

    // FIXME: this function was not meant to be used for setting TAPLO
    // but it is the most convenient way to set these bits in GPTMTAMR
    TimerUpdateMode(TIMER3_BASE, TIMER_A, TIMER_TAMR_TAPLO | TIMER_TAMR_TAMRSU);

    MAP_TimerMatchSet(TIMER3_BASE, TIMER_A, WS2812_DUTYCYCLE_RESET);
    
    
    HwiP_create(INT_TIMER3B, TIMER3B_IRQHandler, NULL);
    MAP_TimerIntEnable(TIMER3_BASE, TIMER_TIMB_DMA);
    
    
    MAP_TimerDMAEventSet(TIMER3_BASE, TIMER_DMA_TIMEOUT_B);

    MAP_SysCtlPeripheralEnable(SYSCTL_PERIPH_UDMA);
    //MAP_uDMAEnable();
    //MAP_uDMAControlBaseSet(pui8ControlTable);
    MAP_uDMAChannelAssign(UDMA_CH3_TIMER3B);
    MAP_uDMAChannelAttributeDisable(UDMA_CH3_TIMER3B,
                                    UDMA_ATTR_ALTSELECT | UDMA_ATTR_USEBURST |
                                    UDMA_ATTR_HIGH_PRIORITY |
                                    UDMA_ATTR_REQMASK);
    MAP_uDMAChannelAttributeEnable(UDMA_CH3_TIMER3B,
                                    UDMA_ATTR_HIGH_PRIORITY
                                    );
    MAP_uDMAChannelControlSet(UDMA_CH3_TIMER3B | UDMA_PRI_SELECT,
                                  UDMA_SIZE_16 | UDMA_SRC_INC_16 | UDMA_DST_INC_NONE |
                                  UDMA_ARB_1);


}

void ws_start_transfer( uint32_t len){

    MAP_TimerMatchSet(TIMER3_BASE, TIMER_A, frame_buffer[0]);

    MAP_uDMAChannelTransferSet(UDMA_CH3_TIMER3B | UDMA_PRI_SELECT,
                                   UDMA_MODE_BASIC,
                                   (void *)frame_buffer+2, (void*)&TIMER3->TAMATCHR,
                                   len-1);
    //MAP_uDMAChannelTransferSet(UDMA_CH3_TIMER3B | UDMA_PRI_SELECT,
    //                               UDMA_MODE_BASIC,
    //                               (void *)&fb, (void*)&TIMER3->TAMATCHR,
    //                               len);

    inprogress = 1;

    MAP_uDMAChannelEnable(UDMA_CH3_TIMER3B);

    MAP_IntEnable(INT_TIMER3B);
    MAP_TimerEnable(TIMER3_BASE, TIMER_BOTH);

//    MAP_uDMAChannelEnable(UDMA_CH3_TIMER3B);
    
}

/// \method display()
///
/// Takes an array of RGB values, or a single one.
/// Uses the 0xRRGGBB format
STATIC mp_obj_t pyb_neopix_display(mp_obj_t self_in, mp_obj_t rgb) {
	//pyb_neopix_obj_t *self = self_in;
	
	mp_uint_t len;
	int tx;
	int mask;
	int val;

        while(inprogress) {
	    usleep(100);
        }

	mp_obj_t *items;
	
	if (MP_OBJ_IS_INT(rgb))
		len = 1;
	else
		mp_obj_get_array(rgb, &len, &items);

    uint32_t buf_ptr = 0;

    if (frame_buffer_size < (24*len+WS2812_RESET_BIT_N)*sizeof(uint16_t)){
        if (frame_buffer != NULL)
            free(frame_buffer);
        frame_buffer = malloc((24*len+WS2812_RESET_BIT_N)*sizeof(uint16_t));
        if (frame_buffer == NULL)
            return mp_const_none;
        frame_buffer_size = (24*len+WS2812_RESET_BIT_N)*sizeof(uint16_t);
    }

    
//    frame_buffer[buf_ptr++] = WS2812_DUTYCYCLE_RESET;
	
	while(len){
		len--;
		
		if (MP_OBJ_IS_INT(rgb))
			val = mp_obj_get_int(rgb);
		else
			val = mp_obj_get_int(items[len]);
		
		mask = (1<<23);
		tx = ((val & 0xFF00) << 8) | ((val & 0xFF0000) >> 8) | (val & 0xFF);
		while (mask){
						
			if (mask & tx)
				frame_buffer[buf_ptr++] = WS2812_DUTYCYCLE_1;
			else
				frame_buffer[buf_ptr++] = WS2812_DUTYCYCLE_0;
						
			mask = mask >> 1;
		}
	}

    for(int i=0;i<WS2812_RESET_BIT_N;i++) {
        frame_buffer[buf_ptr++] = WS2812_DUTYCYCLE_RESET;
    }

	ws_start_transfer( buf_ptr);
	
	return mp_const_none;
}
STATIC MP_DEFINE_CONST_FUN_OBJ_2(pyb_neopix_display_obj, pyb_neopix_display);

/// \method destroy()
///
/// Stops the timer
STATIC mp_obj_t pyb_neopix_destroy(mp_obj_t self_in) {
	free(frame_buffer);
    frame_buffer = NULL;
    frame_buffer_size = 0;
	return mp_const_none;
}
STATIC MP_DEFINE_CONST_FUN_OBJ_1(pyb_neopix_destroy_obj, pyb_neopix_destroy);	

STATIC const mp_map_elem_t pyb_neopix_locals_dict_table[] = {
    // instance methods
    { MP_OBJ_NEW_QSTR(MP_QSTR___del__), (mp_obj_t)&pyb_neopix_destroy_obj},    
    { MP_OBJ_NEW_QSTR(MP_QSTR_display), (mp_obj_t)&pyb_neopix_display_obj },
    { MP_OBJ_NEW_QSTR(MP_QSTR_destroy), (mp_obj_t)&pyb_neopix_destroy_obj },

	//class constants
    //{ MP_OBJ_NEW_QSTR(MP_QSTR_RED),        MP_OBJ_NEW_SMALL_INT(Red) },

};

STATIC MP_DEFINE_CONST_DICT(pyb_neopix_locals_dict, pyb_neopix_locals_dict_table);

const mp_obj_type_t pyb_neopix_type = {
    { &mp_type_type },
    .name = MP_QSTR_Neopix,
    .make_new = pyb_neopix_make_new,
    .locals_dict = (mp_obj_t)&pyb_neopix_locals_dict,
};


#endif // MICROPY_HW_HAS_NEOPIX
