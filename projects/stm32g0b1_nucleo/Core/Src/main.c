#include "main.h"
#include <stddef.h>
#include <stdint.h>
#include <string.h>

/**
 * \brief           Number of LEDs (or LED drivers in case of WS2811) connected to single strip in a row
 *
 * This value should be set to your defined length
 */
#define LED_CFG_COUNT                 16

/**
 * \brief           Number of bytes for one LED color description
 * \note            Set to `3` for RGB, or set to `4` for RGBW use case
 */
#define LED_CFG_BYTES_PER_LED         3

/**
 * \brief           Defines size of raw transmission leds in units of "data-for-leds"
 * \note            `1 unit` represents `30us` at `800 kHz and 24-bit of LED data (RGB)`
 *                      or `40us` at ˙800 kHz and 32-bit of LED data (RGBW)`
 *
 * Based on the value:
 * - Greater it is, less frequent are DMA interrupts, but interrupt handling takes potentially more time
 * - Greater it is, larger is the raw dma buffer as it has to accomodate for `2*value` number of elements for led's data
 *
 * \note            Values should be greater-or-equal than `1` and (advise) not higher than `8˙
 * \note            Value set to `6` means DMA TC or HT interrupts are triggered at each `180us at 800kHz tim update rate for RGB leds`
 */
#define LED_CFG_LEDS_PER_DMA_IRQ      16

/**
 * \brief           Defines minimum number of "1-led-transmission-time" blocks
 *                  to send logical `0` to the bus, indicating reset before data transmission starts.
 *
 * This is a must to generate reset value.
 * As per datasheet, different devices require different min reset time, normally min `280us`,
 * that represents minimum `10 led cycles`
 *
 * \note            Few conditions apply to the value and all must be a pass
 *                      - Must be greater than `0`
 *                      - Must be set to a value to support minimum reset time required by the driver
 *
 *                  Further advices
 *                      - Set it to `2*LED_CFG_LEDS_PER_DMA_IRQ` or higher
 *                      - Set it as multiplier of \ref LED_CFG_LEDS_PER_DMA_IRQ
 */
#define LED_RESET_PRE_MIN_LED_CYCLES  1

/**
 * \brief           Number of "1-led-transmission-time" units to send all zeros
 *                  after last led has been sent out
 * \note            It indicates minimum required value, while actual may be longer
 *                  and it has to do with multipliers of \ref LED_CFG_LEDS_PER_DMA_IRQ
 */
#define LED_RESET_POST_MIN_LED_CYCLES 8

/**
 * \brief           Application array to store led colors for application use case
 *                      Data in format R,G,B,R,G,B,...
 *
 * Array used by user application to store data to
 */
static uint8_t leds_color_data[LED_CFG_BYTES_PER_LED * LED_CFG_COUNT];

/**
 * \brief           This buffer acts as raw buffer for DMA to transfer data from memory to timer compare
 * \note            DMA must have access to this variable (memory location)
 *
 * It is a multi-dimentional array:
 * - First part represents 2 parts- one before half transfer; two after half transfer before transfer complete;
 * - Second part is number of LEDs to transmit before DMA HT/TC interrupts get fired
 * - Third part are entries for raw timer compare register to send logical 1 and 0 to the bus
 *
 * Type of variable should be unsigned 32-bit, to satisfy TIM that is 32-bit timer
 */
static uint8_t dma_buffer[(2) * (LED_CFG_LEDS_PER_DMA_IRQ) * (LED_CFG_BYTES_PER_LED * 8)];

/* Used macros */

/* Number of elements in raw DMA buffer array - used by DMA transfer length */
#define DMA_BUFF_ELE_LEN         ((uint32_t)(sizeof(dma_buffer) / sizeof((dma_buffer)[0])))
/* Half number of elements in raw DMA buffer */
#define DMA_BUFF_ELE_HALF_LEN    ((uint32_t)(DMA_BUFF_ELE_LEN >> 1))
/* Size of (in bytes) half length of array -> used for memory set */
#define DMA_BUFF_ELE_HALF_SIZEOF ((size_t)(sizeof(dma_buffer[0]) * DMA_BUFF_ELE_HALF_LEN))
/* Number of array indexes required for one led */
#define DMA_BUFF_ELE_LED_LEN     ((uint32_t)(LED_CFG_BYTES_PER_LED * 8))
/* Size of (in bytes) of one led memory in DMA buffer */
#define DMA_BUFF_ELE_LED_SIZEOF  ((size_t)(sizeof(dma_buffer[0]) * DMA_BUFF_ELE_LED_LEN))

#define USE_DEBUG_PINS           1

/* Pin configuration goes here */
#define LED_GPIO_PORT                             GPIOA
#define LED_GPIO_PIN                              LL_GPIO_PIN_15
#define LED_GPIO_CLK_EN                           LL_IOP_GRP1_EnableClock(LL_IOP_GRP1_PERIPH_GPIOA)
#define LED_GPIO_PIN_TIM_AF                       LL_GPIO_AF_2
#define LED_TIM                                   TIM2
#define LED_TIM_CLK_EN                            LL_APB1_GRP1_EnableClock(LL_APB1_GRP1_PERIPH_TIM2)
#define LED_TIM_CHANNEL                           LL_TIM_CHANNEL_CH1
#define LED_TIM_CHANNEL_DMA                       DMA1
#define LED_TIM_CHANNEL_DMA_CLK_EN                LL_AHB1_GRP1_EnableClock(LL_AHB1_GRP1_PERIPH_DMA1)
#define LED_TIM_CHANNEL_DMA_CHANNEL               LL_DMA_CHANNEL_1
#define LED_TIM_CHANNEL_DMA_CHANNEL_REQ           LL_DMAMUX_REQ_TIM2_CH1
#define LED_TIM_CHANNEL_DATA_REG                  &LED_TIM->CCR1
#define LED_TIM_CHANNEL_DMA_IRQn                  DMA1_Channel1_IRQn
#define LED_TIM_CHANNEL_DMA_IRQHandler            DMA1_Channel1_IRQHandler
#define LED_TIM_CHANNEL_DMA_CHANNEL_IS_ACTIVE_HT  LL_DMA_IsActiveFlag_HT1(LED_TIM_CHANNEL_DMA)
#define LED_TIM_CHANNEL_DMA_CHANNEL_IS_ACTIVE_TC  LL_DMA_IsActiveFlag_TC1(LED_TIM_CHANNEL_DMA)
#define LED_TIM_CHANNEL_DMA_CHANNEL_CLEAR_FLAG_HT LL_DMA_ClearFlag_HT1(LED_TIM_CHANNEL_DMA)
#define LED_TIM_CHANNEL_DMA_CHANNEL_CLEAR_FLAG_TC LL_DMA_ClearFlag_TC1(LED_TIM_CHANNEL_DMA)
#define LED_TIM_CHANNEL_ENABLE_DMA_REQ            LL_TIM_EnableDMAReq_CC1(LED_TIM)
#define LED_TIM_CHANNEL_SET_COMPARE(val)          LL_TIM_OC_SetCompareCH1(LED_TIM, (val))

/* Control variables for transfer */
static volatile uint8_t is_updating = 0;    /* Set to `1` when update is in progress */
static volatile uint32_t led_cycles_cnt;    /* Counts how many "1-led-time" have been transmitted so far */
static volatile uint8_t brightness = 0xFF;  /* Brightness between 0 and 0xFF */
static volatile uint32_t color_counter = 1; /* Color, being increased each fade reaching 0 */

/* Application variables for fading effect */
int8_t fade_step;
int16_t fade_value;

/* Start data transfer */
static uint8_t led_start_transfer(void);
void SystemClock_Config(void);
static void tim_init(void);
static void gpio_init(void);
static void led_fill_led_pwm_data(size_t ledx, uint32_t* ptr);

/* Debug control pins */
#if USE_DEBUG_PINS
#define DBG_PIN_UPDATING_HIGH  LL_GPIO_SetOutputPin(GPIOA, LL_GPIO_PIN_0)
#define DBG_PIN_UPDATING_LOW   LL_GPIO_ResetOutputPin(GPIOA, LL_GPIO_PIN_0)
#define DBG_PIN_IRQ_HIGH       LL_GPIO_SetOutputPin(GPIOA, LL_GPIO_PIN_1)
#define DBG_PIN_IRQ_LOW        LL_GPIO_ResetOutputPin(GPIOA, LL_GPIO_PIN_1)
#define DBG_PIN_DATA_FILL_HIGH LL_GPIO_SetOutputPin(GPIOA, LL_GPIO_PIN_4)
#define DBG_PIN_DATA_FILL_LOW  LL_GPIO_ResetOutputPin(GPIOA, LL_GPIO_PIN_4)
#else
#define DBG_PIN_UPDATING_HIGH
#define DBG_PIN_UPDATING_LOW
#define DBG_PIN_IRQ_HIGH
#define DBG_PIN_IRQ_LOW
#define DBG_PIN_DATA_FILL_HIGH
#define DBG_PIN_DATA_FILL_LOW
#endif

#define DBG_PIN_UPDATING_TOGGLE  LL_GPIO_TogglePin(GPIOA, LL_GPIO_PIN_0)

/**
 * \brief           The application entry point
 */
int
main(void) {
    LL_APB2_GRP1_EnableClock(LL_APB2_GRP1_PERIPH_SYSCFG);
    LL_APB1_GRP1_EnableClock(LL_APB1_GRP1_PERIPH_PWR);

    /* Disable the internal Pull-Up in Dead Battery pins of UCPD peripheral */
    LL_SYSCFG_DisableDBATT(LL_SYSCFG_UCPD1_STROBE);

    /* Configure the system clock */
    SystemClock_Config();

    /* Initialize all configured peripherals */
    tim_init();
    gpio_init();

    /* Set up the first leds with dummy color */
    for (size_t i = 0; i < LED_CFG_COUNT; ++i) {
        leds_color_data[i * LED_CFG_BYTES_PER_LED + 0] = 0x00;
        leds_color_data[i * LED_CFG_BYTES_PER_LED + 1] = 0x00;
        leds_color_data[i * LED_CFG_BYTES_PER_LED + 2] = 0x01;
    }

    /* Define fade init values */
    fade_step = 1;
    fade_value = 0;

    /* Enable systick interrupt generation */
    SysTick->CTRL |= SysTick_CTRL_TICKINT_Msk;

    /*
     * Everything is happening in SysTick_Handler function,
     * defined later in this file
     */

    /* Infinite loop */
    while (1) {}
    return 0;
}

/**
 * \brief           Update sequence function,
 *                  called on each DMA half transfer or transfer complete complete event
 * \param[in]       tc: Set to `1` when function is called from DMA TC event, `0` when from HT event  tc 0 indicates the lower (first) half of the data and 1 indicates the upper (Second) half of the buffer
 */
static void
led_update_sequence(uint8_t tc) {
    DBG_PIN_IRQ_HIGH;
    if(tc)
    {
        DBG_PIN_IRQ_LOW;
        DBG_PIN_IRQ_HIGH;
    }
    if (!is_updating) {
        DBG_PIN_IRQ_LOW;
        return;
    }

    /*
     * led_cycles_cnt variable represents minimum number of
     * led cycles that will be transmitted on the bus.
     *
     * It is set to non-0 value after transfer and gets increased in each interrupt.
     *
     * Interrupts are triggered (TC or HT) when DMA transfers `LED_CFG_LEDS_PER_DMA_IRQ` led cycles of data elements
     */
    led_cycles_cnt += LED_CFG_LEDS_PER_DMA_IRQ;

    if (led_cycles_cnt < LED_RESET_PRE_MIN_LED_CYCLES) {
#if LED_CFG_LEDS_PER_DMA_IRQ > 1
        /*
         * We are still in reset sequence for the moment.
         *
         * When LED_CFG_LEDS_PER_DMA_IRQ > 1
         * and pre-reset led cycles is not aligned to LED_CFG_LEDS_PER_DMA_IRQ value,
         * first leds need to be filled already at this stage
         */
        if ((led_cycles_cnt + LED_CFG_LEDS_PER_DMA_IRQ) > LED_RESET_PRE_MIN_LED_CYCLES) {
            uint32_t index = LED_RESET_PRE_MIN_LED_CYCLES - led_cycles_cnt;
            /* We need to preload some values for some leds now */
            /* Otherwise we will miss some leds */
            for (uint32_t i = 0; index < LED_CFG_LEDS_PER_DMA_IRQ && i < LED_CFG_COUNT; ++index, ++i) {
                led_fill_led_pwm_data(i, &dma_buffer[tc * DMA_BUFF_ELE_HALF_LEN
                                                     + (index % LED_CFG_LEDS_PER_DMA_IRQ) * DMA_BUFF_ELE_LED_LEN]);
            }
        }
#endif /* LED_CFG_LEDS_PER_DMA_IRQ > 1 */
    } else if (led_cycles_cnt < (LED_RESET_PRE_MIN_LED_CYCLES + LED_CFG_COUNT)) {
        /*
         * This is where we prepare data for next cycle only
         *
         * next led is simply calculated by subtracting counter from reset cycle
         * If reset is not aligned, it is important to handle first leds already in previous if statement,
         * otherwise we will miss first x leds (where x depends on how much we are away from alignment)
         */
        uint32_t next_led = led_cycles_cnt - LED_RESET_PRE_MIN_LED_CYCLES;
#if LED_CFG_LEDS_PER_DMA_IRQ == 1
        led_fill_led_pwm_data(next_led, &dma_buffer[tc * DMA_BUFF_ELE_HALF_LEN]);
#endif /* LED_CFG_LEDS_PER_DMA_IRQ == 1 */
#if LED_CFG_LEDS_PER_DMA_IRQ > 1
        uint32_t counter = 0;
        /*
         * Fill buffer with led data
         *
         * If counter stops earlier than buffer ends (low number of leds or not aligned),
         * fill remaining with all zeros to indicate post-reset signal
         */
        for (; counter < LED_CFG_LEDS_PER_DMA_IRQ && next_led < LED_CFG_COUNT; ++counter, ++next_led) {
            led_fill_led_pwm_data(next_led, &dma_buffer[tc * DMA_BUFF_ELE_HALF_LEN + counter * DMA_BUFF_ELE_LED_LEN]);
        }
        if (counter < LED_CFG_LEDS_PER_DMA_IRQ) {
            memset(&dma_buffer[tc * DMA_BUFF_ELE_HALF_LEN + counter * DMA_BUFF_ELE_LED_LEN], 0x00,
                   (LED_CFG_LEDS_PER_DMA_IRQ - counter) * DMA_BUFF_ELE_LED_SIZEOF);
        }
#endif /* LED_CFG_LEDS_PER_DMA_IRQ > 1 */
    } else if (led_cycles_cnt < (LED_RESET_PRE_MIN_LED_CYCLES + LED_CFG_COUNT + LED_RESET_POST_MIN_LED_CYCLES
                                 + LED_CFG_LEDS_PER_DMA_IRQ)) {
        /*
         * This is post-reset and must be set to at least 1 level in size
         * and sends all zeros to the leds.
         *
         * Manually reset array to all zeros using memset, but only if it has not been set already
         * (to not waste CPU resources for small MCUs)
         */
        if (led_cycles_cnt < (LED_RESET_PRE_MIN_LED_CYCLES + LED_CFG_COUNT + 2 * LED_CFG_LEDS_PER_DMA_IRQ)) {
            memset(&dma_buffer[tc * DMA_BUFF_ELE_HALF_LEN], 0x00, DMA_BUFF_ELE_HALF_SIZEOF);
        }
    } else {
        /*
         * We are now ready to stop DMA and TIM channel,
         * otherwise transfers will continue to occur (circular mode).
         *
         * Disable interrupts prior disabling DMA transfer.
         *
         * Some STM32 (F2, F4, F7) may generate TC or HT interrupts when DMA is manually disabled,
         * and since it is not necessary to receive these interrupts from now-on,
         * it's better to simply disable them
         */
        LL_DMA_DisableIT_TC(LED_TIM_CHANNEL_DMA, LED_TIM_CHANNEL_DMA_CHANNEL);
        LL_DMA_DisableIT_HT(LED_TIM_CHANNEL_DMA, LED_TIM_CHANNEL_DMA_CHANNEL);
        LL_DMA_DisableChannel(LED_TIM_CHANNEL_DMA, LED_TIM_CHANNEL_DMA_CHANNEL);
        LL_TIM_CC_DisableChannel(LED_TIM, LED_TIM_CHANNEL);
        is_updating = 0;
        DBG_PIN_UPDATING_LOW;
    }
    DBG_PIN_IRQ_LOW;
}

/**
 * \brief           DMA channel interrupt handler
 */
void
LED_TIM_CHANNEL_DMA_IRQHandler(void) {
    if (LL_DMA_IsEnabledIT_HT(LED_TIM_CHANNEL_DMA, LED_TIM_CHANNEL_DMA_CHANNEL)
        && LED_TIM_CHANNEL_DMA_CHANNEL_IS_ACTIVE_HT) {
        LED_TIM_CHANNEL_DMA_CHANNEL_CLEAR_FLAG_HT;
        led_update_sequence(0);
    }
    if (LL_DMA_IsEnabledIT_TC(LED_TIM_CHANNEL_DMA, LED_TIM_CHANNEL_DMA_CHANNEL)
        && LED_TIM_CHANNEL_DMA_CHANNEL_IS_ACTIVE_TC) {
        LED_TIM_CHANNEL_DMA_CHANNEL_CLEAR_FLAG_TC;
        led_update_sequence(1);
    }
}

/**
 * \brief           Prepares data from memory for PWM output for timer
 * \note            Memory is in format R,G,B, while PWM must be configured in G,R,B[,W]
 * \param[in]       ledx: LED index to set the color
 * \param[out]      ptr: Output array with at least LED_CFG_RAW_BYTES_PER_LED-words of memory
 */
static void
led_fill_led_pwm_data(size_t ledx, uint32_t* ptr) {
    const uint32_t arr = LED_TIM->ARR + 1;
    const uint32_t pulse_low  = (1 * arr / 4) - 1;  // 1/4 ARR is 19
    const uint32_t pulse_high = (3 * arr / 4) - 1;  // 3/4 ARR is 59

    DBG_PIN_DATA_FILL_HIGH;
    if (ledx < LED_CFG_COUNT) {
        uint32_t r, g, b;

//        r = (uint8_t)(((uint32_t)leds_color_data[ledx * LED_CFG_BYTES_PER_LED + 0] * (uint32_t)brightness) / (uint32_t)0xFF);
//        g = (uint8_t)(((uint32_t)leds_color_data[ledx * LED_CFG_BYTES_PER_LED + 1] * (uint32_t)brightness) / (uint32_t)0xFF);
//        b = (uint8_t)(((uint32_t)leds_color_data[ledx * LED_CFG_BYTES_PER_LED + 2] * (uint32_t)brightness) / (uint32_t)0xFF);
//        r = leds_color_data[ledx * LED_CFG_BYTES_PER_LED + 0] & brightness;
//        g = leds_color_data[ledx * LED_CFG_BYTES_PER_LED + 1] & brightness;
//        b = leds_color_data[ledx * LED_CFG_BYTES_PER_LED + 2] & brightness;
        r = (/*ledx + */color_counter) & 0x01 ? brightness : 0x00;
        g = (/*ledx + */color_counter) & 0x02 ? brightness : 0x00;
        b = (/*ledx + */color_counter) & 0x04 ? brightness : 0x00;
        for (size_t i = 0; i < 8; i++) {
            ptr[     i] = (g & (1 << (7 - i))) ? pulse_high : pulse_low;
            ptr[ 8 + i] = (r & (1 << (7 - i))) ? pulse_high : pulse_low;
            ptr[16 + i] = (b & (1 << (7 - i))) ? pulse_high : pulse_low;
        }
    }
    DBG_PIN_DATA_FILL_LOW;
}

/**
 * \brief           Start with transfer process to update LED on the strip
 * \return          `1` if transfer has started, `0` otherwise
 */
static uint8_t
led_start_transfer(void) {
    DBG_PIN_UPDATING_HIGH;
    if (is_updating) {
        return 0;
    }

    /* Set initial values */
    is_updating = 1;
    led_cycles_cnt = LED_CFG_LEDS_PER_DMA_IRQ;

    /*
     * At the very beginning we should prepare data for full array length
     *
     * 2 steps are done
     * - Reset buffer to zero, for reset pulse (all zeros)
     * - Manually set data for first round, in case reset length is set lower than size of array
     */
    memset(dma_buffer, 0x00, sizeof(dma_buffer));
    for (uint32_t i = 0, index = LED_RESET_PRE_MIN_LED_CYCLES; index < 2 * LED_CFG_LEDS_PER_DMA_IRQ; ++index, ++i) {
        led_fill_led_pwm_data(i, &dma_buffer[index * DMA_BUFF_ELE_LED_LEN]);  // why can't this fill with half data of initial LEDs?
    }

    /*
     * This is where DMA gets configured for data transfer
     *
     * - Circular mode, continuous transmission
     * - Memory length (number of elements) for 2 LEDs of data
     */
    LL_DMA_SetMode(LED_TIM_CHANNEL_DMA, LED_TIM_CHANNEL_DMA_CHANNEL, LL_DMA_MODE_CIRCULAR);
    LL_DMA_SetMemoryAddress(LED_TIM_CHANNEL_DMA, LED_TIM_CHANNEL_DMA_CHANNEL, (uint32_t)dma_buffer);  //dma_buffer consumed here,  the datatype of dma_buffer can be uint8, while th epointer passed still has to be uint32_t*
    LL_DMA_SetDataLength(LED_TIM_CHANNEL_DMA, LED_TIM_CHANNEL_DMA_CHANNEL, DMA_BUFF_ELE_LEN);

    /* Clear flags, enable interrupts */
    LED_TIM_CHANNEL_DMA_CHANNEL_CLEAR_FLAG_TC;
    LED_TIM_CHANNEL_DMA_CHANNEL_CLEAR_FLAG_HT;
    LL_DMA_EnableIT_TC(LED_TIM_CHANNEL_DMA, LED_TIM_CHANNEL_DMA_CHANNEL);
    LL_DMA_EnableIT_HT(LED_TIM_CHANNEL_DMA, LED_TIM_CHANNEL_DMA_CHANNEL);

	DBG_PIN_UPDATING_TOGGLE;
	DBG_PIN_UPDATING_TOGGLE;

    /* Enable DMA, TIM channel and TIM counter */
    LL_DMA_EnableChannel(LED_TIM_CHANNEL_DMA, LED_TIM_CHANNEL_DMA_CHANNEL);
    LL_TIM_CC_EnableChannel(LED_TIM, LED_TIM_CHANNEL);
    LL_TIM_EnableCounter(LED_TIM);

	DBG_PIN_UPDATING_TOGGLE;
	DBG_PIN_UPDATING_TOGGLE;
    /* All the rest is happening in DMA interrupt from now on */
    return 1;
}

/**
 * \brief           TIM Initialization Function
 */
static void
tim_init(void) {
    LL_TIM_InitTypeDef TIM_InitStruct = {0};
    LL_TIM_OC_InitTypeDef TIM_OC_InitStruct = {0};
    LL_GPIO_InitTypeDef GPIO_InitStruct = {0};

    /* Peripheral clock enable */
    LED_GPIO_CLK_EN;
    LED_TIM_CLK_EN;
    LED_TIM_CHANNEL_DMA_CLK_EN;

    /*
     * TIM2 GPIO Configuration
     *
     * PA3   ------> TIM2_CH4
     */
    GPIO_InitStruct.Pin = LED_GPIO_PIN;
    GPIO_InitStruct.Mode = LL_GPIO_MODE_ALTERNATE;
    GPIO_InitStruct.Speed = LL_GPIO_SPEED_FREQ_LOW;
    GPIO_InitStruct.OutputType = LL_GPIO_OUTPUT_PUSHPULL;
    GPIO_InitStruct.Pull = LL_GPIO_PULL_NO;
    GPIO_InitStruct.Alternate = LED_GPIO_PIN_TIM_AF;
    LL_GPIO_Init(LED_GPIO_PORT, &GPIO_InitStruct);

    /* DMA interrupts */
    NVIC_SetPriority(LED_TIM_CHANNEL_DMA_IRQn, 0);
    NVIC_EnableIRQ(LED_TIM_CHANNEL_DMA_IRQn);

    /* DMA setup */
    LL_DMA_SetPeriphRequest(LED_TIM_CHANNEL_DMA, LED_TIM_CHANNEL_DMA_CHANNEL, LED_TIM_CHANNEL_DMA_CHANNEL_REQ);
    LL_DMA_SetDataTransferDirection(LED_TIM_CHANNEL_DMA, LED_TIM_CHANNEL_DMA_CHANNEL,
                                    LL_DMA_DIRECTION_MEMORY_TO_PERIPH);
    LL_DMA_SetChannelPriorityLevel(LED_TIM_CHANNEL_DMA, LED_TIM_CHANNEL_DMA_CHANNEL, LL_DMA_PRIORITY_MEDIUM);
    LL_DMA_SetMode(LED_TIM_CHANNEL_DMA, LED_TIM_CHANNEL_DMA_CHANNEL, LL_DMA_MODE_CIRCULAR);
    LL_DMA_SetPeriphIncMode(LED_TIM_CHANNEL_DMA, LED_TIM_CHANNEL_DMA_CHANNEL, LL_DMA_PERIPH_NOINCREMENT);
    LL_DMA_SetMemoryIncMode(LED_TIM_CHANNEL_DMA, LED_TIM_CHANNEL_DMA_CHANNEL, LL_DMA_MEMORY_INCREMENT);
    LL_DMA_SetPeriphSize(LED_TIM_CHANNEL_DMA, LED_TIM_CHANNEL_DMA_CHANNEL, LL_DMA_PDATAALIGN_HALFWORD);
    LL_DMA_SetMemorySize(LED_TIM_CHANNEL_DMA, LED_TIM_CHANNEL_DMA_CHANNEL, LL_DMA_MDATAALIGN_BYTE);
    LL_DMA_SetPeriphAddress(LED_TIM_CHANNEL_DMA, LED_TIM_CHANNEL_DMA_CHANNEL, (uint32_t)LED_TIM_CHANNEL_DATA_REG);  // select CCR address as target

    /*
     * Set basic timer settings
     *
     * - Set no prescaler for max resolution
     * - Set autoreload for 800kHz refresh rate (64 MHz TIM kernel clock)
     */
    TIM_InitStruct.Prescaler = 0;
    TIM_InitStruct.CounterMode = LL_TIM_COUNTERMODE_UP;
    TIM_InitStruct.Autoreload = 79;   //ARR is fixed 79
    TIM_InitStruct.ClockDivision = LL_TIM_CLOCKDIVISION_DIV1;
    LL_TIM_Init(LED_TIM, &TIM_InitStruct);
    LL_TIM_EnableARRPreload(LED_TIM);
    LL_TIM_SetTriggerOutput(LED_TIM, LL_TIM_TRGO_RESET);
    LL_TIM_DisableMasterSlaveMode(LED_TIM);

    /* Channel settings */
    TIM_OC_InitStruct.OCMode = LL_TIM_OCMODE_PWM1;
    TIM_OC_InitStruct.OCState = LL_TIM_OCSTATE_ENABLE;
    TIM_OC_InitStruct.OCNState = LL_TIM_OCSTATE_DISABLE;
    TIM_OC_InitStruct.CompareValue = 0;
    TIM_OC_InitStruct.OCPolarity = LL_TIM_OCPOLARITY_HIGH;
    LL_TIM_OC_Init(LED_TIM, LED_TIM_CHANNEL, &TIM_OC_InitStruct);
    LL_TIM_OC_DisableFast(LED_TIM, LED_TIM_CHANNEL);
    LL_TIM_OC_EnablePreload(LED_TIM, LED_TIM_CHANNEL);

    /* Enable channel DMA requests */
    LED_TIM_CHANNEL_ENABLE_DMA_REQ;
    LED_TIM_CHANNEL_SET_COMPARE(0);
}

/**
 * \brief           Initialize debug GPIOs
 */
static void
gpio_init(void) {
#if USE_DEBUG_PINS
    LL_GPIO_InitTypeDef GPIO_InitStruct = {0};

    /* Peripheral clock enable */
    LL_IOP_GRP1_EnableClock(LL_IOP_GRP1_PERIPH_GPIOA);

    /*
     * GPIO configuration
     *
     * PA0   ------> Pin set to high when transmission is on-going
     * PA1   ------> Pin toggled on each DMA interrupt
     * PA4   ------> Pin toggled when LED data are being prepared already before DMA run
     * PA5   ------> Pin toggled when LED data are being prepared in interrupt when values are not-aligned
     */
    GPIO_InitStruct.Pin = LL_GPIO_PIN_0 | LL_GPIO_PIN_1 | LL_GPIO_PIN_4 | LL_GPIO_PIN_5;
    GPIO_InitStruct.Mode = LL_GPIO_MODE_OUTPUT;
    GPIO_InitStruct.Speed = LL_GPIO_SPEED_FREQ_LOW;
    GPIO_InitStruct.OutputType = LL_GPIO_OUTPUT_PUSHPULL;
    GPIO_InitStruct.Pull = LL_GPIO_PULL_NO;
    LL_GPIO_Init(GPIOA, &GPIO_InitStruct);
#endif /* USE_DEBUG_PINS */
}

/**
 * \brief           This function handles System tick timer
 */
void
SysTick_Handler(void) {
    static uint32_t counter = 0;

    /* Perform strip color update every 10 ms */
    if (++counter % 10 == 0) {
        if (is_updating) {
            return;
        }

        /* Calculate fading efect */
        fade_value += fade_step;
        if (fade_value > 0xFF) {
            fade_value = 0xFF;
            fade_step = -fade_step;
        } else if (fade_value < 0) {
            fade_value = 0;
            fade_step = -fade_step;
            color_counter++;
        }

        /* Calculate new brightness */
        brightness = fade_value * fade_value / 0xFF;

        /* Check if we need to change the color */
//        if (fade_value == 0) {
//            for (size_t i = 0; i < LED_CFG_COUNT; ++i) {
//                leds_color_data[i * LED_CFG_BYTES_PER_LED + 0] = color_counter & 0x01 ? 0xFF : 0x00;
//                leds_color_data[i * LED_CFG_BYTES_PER_LED + 1] = color_counter & 0x02 ? 0xFF : 0x00;
//                leds_color_data[i * LED_CFG_BYTES_PER_LED + 2] = color_counter & 0x04 ? 0xFF : 0x00;
//            }
//        }

        /* Start data transfer in non-blocking mode */
        led_start_transfer();
    }
}

/**
 * \brief           System Clock Configuration
 */
void
SystemClock_Config(void) {
    LL_FLASH_SetLatency(LL_FLASH_LATENCY_2);
    while (LL_FLASH_GetLatency() != LL_FLASH_LATENCY_2) {}

    /* HSI configuration and activation */
    LL_RCC_HSI_Enable();
    while (!LL_RCC_HSI_IsReady()) {}

    /* Main PLL configuration and activation */
    LL_RCC_PLL_ConfigDomain_SYS(LL_RCC_PLLSOURCE_HSI, LL_RCC_PLLM_DIV_1, 8, LL_RCC_PLLR_DIV_2);
    LL_RCC_PLL_Enable();
    LL_RCC_PLL_EnableDomain_SYS();
    while (!LL_RCC_PLL_IsReady()) {}

    /* Set AHB prescaler */
    LL_RCC_SetAHBPrescaler(LL_RCC_SYSCLK_DIV_1);

    /* Sysclk activation on the main PLL */
    LL_RCC_SetSysClkSource(LL_RCC_SYS_CLKSOURCE_PLL);
    while (LL_RCC_GetSysClkSource() != LL_RCC_SYS_CLKSOURCE_STATUS_PLL) {}

    /* Set APB1 prescaler */
    LL_RCC_SetAPB1Prescaler(LL_RCC_APB1_DIV_1);
    LL_Init1msTick(64000000);

    /* Enable SysTick interrupt */
    NVIC_SetPriority(SysTick_IRQn, 3);
    NVIC_EnableIRQ(SysTick_IRQn);

    /* Update CMSIS variable (which can be updated also through SystemCoreClockUpdate function) */
    LL_SetSystemCoreClock(64000000);
}

/**
 * \brief           This function is executed in case of error occurrence
 */
void
Error_Handler(void) {
    __disable_irq();
    while (1) {}
}
