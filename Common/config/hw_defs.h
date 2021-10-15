#ifndef __HW_DEFS
#define __HW_DEFS

#define LED_RED_Pin 			GPIO_PIN_6
#define LED_RED_GPIO_Port 		GPIOH

#define LED_GREEN_Pin 			GPIO_PIN_7
#define LED_GREEN_GPIO_Port 	GPIOH

#define MXCHIP_FLOW_Pin 		GPIO_PIN_15
#define MXCHIP_FLOW_GPIO_Port 	GPIOG
#define MXCHIP_FLOW_EXTI_IRQn 	EXTI15_IRQn

#define MXCHIP_NOTIFY_Pin 		GPIO_PIN_14
#define MXCHIP_NOTIFY_GPIO_Port GPIOD
#define MXCHIP_NOTIFY_EXTI_IRQn EXTI14_IRQn

#define MXCHIP_NSS_Pin 			GPIO_PIN_12
#define MXCHIP_NSS_GPIO_Port 	GPIOB

#define MXCHIP_RESET_Pin 		GPIO_PIN_15
#define MXCHIP_RESET_GPIO_Port 	GPIOF

#endif /* __HW_DEFS */
