/*
 * probe-rs (<= 0.32) starts RAM-loaded STM32N6 images with PRIMASK = 1 and stale
 * NVIC state (probe-rs#4390, fix pending in #4703). The ST firmware never calls
 * __enable_irq(), so SysTick/I3C/DMA/EXTI would never fire and HAL_Delay() hangs.
 * Runs from __libc_init_array, before main(). No-op after a real reset.
 */
#include "stm32n6xx.h"

__attribute__((constructor)) static void irq_unmask(void) {
    SysTick->CTRL = 0;
    SCB->ICSR = SCB_ICSR_PENDSTCLR_Msk | SCB_ICSR_PENDSVCLR_Msk;
    for (unsigned i = 0; i < sizeof(NVIC->ICER) / sizeof(NVIC->ICER[0]); i++) {
        NVIC->ICER[i] = 0xFFFFFFFFU;
        NVIC->ICPR[i] = 0xFFFFFFFFU;
    }
    __DSB();
    __ISB();
    __enable_irq();
}
