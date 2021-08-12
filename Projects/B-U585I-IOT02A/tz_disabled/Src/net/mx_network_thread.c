/*
 * FreeRTOS STM32 Reference Integration
 * Copyright (C) 2021 Amazon.com, Inc. or its affiliates.  All Rights Reserved.
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy of
 * this software and associated documentation files (the "Software"), to deal in
 * the Software without restriction, including without limitation the rights to
 * use, copy, modify, merge, publish, distribute, sublicense, and/or sell copies of
 * the Software, and to permit persons to whom the Software is furnished to do so,
 * subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in all
 * copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY, FITNESS
 * FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE AUTHORS OR
 * COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER
 * IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN
 * CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.
 *
 * https://www.FreeRTOS.org
 * https://github.com/FreeRTOS
 *
 */

#include "logging_levels.h"
#define LOG_LEVEL LOG_DEBUG
#include "logging.h"

#include <stdint.h>
#include <limits.h>

#include "network_thread.h"
#include "mx_prv.h"

#include "FreeRTOS.h"
#include "task.h"
#include "ConfigStore.h"
#include "lwip/tcpip.h"
#include "lwip/netifapi.h"

/* Async updates from callback functions */
#define NET_EVT_IDX                     0x1
#define NET_LWIP_READY_BIT              0x1
#define NET_LWIP_IP_CHANGE_BIT          0x2
#define NET_LWIP_IFUP_BIT               0x4
#define NET_LWIP_IFDOWN_BIT             0x8
#define NET_LWIP_LINK_UP_BIT            0x10
#define NET_LWIP_LINK_DOWN_BIT          0x20
#define MX_STATUS_UPDATE_BIT            0x40
#define ASYNC_REQUEST_RECONNECT_BIT     0x80

#define MACADDR_RETRY_WAIT_TIME_TICKS   pdMS_TO_TICKS( 10 * 1000 )

static TaskHandle_t xNetTaskHandle = NULL;
static MxDataplaneCtx_t xDataPlaneCtx;
static ControlPlaneCtx_t xControlPlaneCtx;

/*
 * @brief Converts from a MxEvent_t to a C string.
 */
static const char* pcMxStatusToString( MxStatus_t xStatus )
{
	const char * pcReturn = "Unknown";
	switch( xStatus )
	{
	case MX_STATUS_NONE:
	    pcReturn = "None";
	    break;
	case MX_STATUS_STA_DOWN:
	    pcReturn = "Station Down";
	    break;
	case MX_STATUS_STA_UP:
	    pcReturn = "Station Up";
	    break;
	case MX_STATUS_STA_GOT_IP:
	    pcReturn = "Station Got IP";
	    break;
	case MX_STATUS_AP_DOWN:
	    pcReturn = "AP Down";
	    break;
	case MX_STATUS_AP_UP:
	    pcReturn = "AP Up";
	    break;
	default:
		/* default to "Unknown" string */
		break;
	}
	return pcReturn;
}

/* Wait for all bits in ulTargetBits */
static uint32_t ulWaitForNotifyBits( BaseType_t uxIndexToWaitOn, uint32_t ulTargetBits, TickType_t xTicksToWait )
{
    TickType_t xRemainingTicks = xTicksToWait;
    TimeOut_t xTimeOut;

    vTaskSetTimeOutState( &xTimeOut );

    uint32_t ulNotifyValueAccumulate = 0x0;

    LogDebug( "Starting wait for notification at index: %d matching bitmask: 0x%X.", uxIndexToWaitOn, ulTargetBits );

    while( ( ulNotifyValueAccumulate & ulTargetBits ) != ulTargetBits )
    {
        uint32_t ulNotifyValue = 0x0;
        (void) xTaskNotifyWaitIndexed( uxIndexToWaitOn,
                                       0x0,
                                       ulTargetBits, /* Clear only the target bits on return */
                                       &ulNotifyValue,
                                       xRemainingTicks );

        /* Accumulate notification bits */
        if( ulNotifyValue > 0 )
        {
            ulNotifyValueAccumulate |= ulNotifyValue;
        }

        /* xTaskCheckForTimeOut adjusts xRemainingTicks */
        if( xTaskCheckForTimeOut( &xTimeOut, &xRemainingTicks ) == pdTRUE )
        {
            LogDebug( "Timed out while waiting for notification at index: %d matching bitmask: 0x%X.", uxIndexToWaitOn, ulTargetBits );
            /* Timed out. Exit loop */
            break;
        }
    }

    /* Check for other event bits received */
    if( ( ulNotifyValueAccumulate & ( ~ulTargetBits ) ) > 0 )
    {
        /* send additional notification so these events are not lost */
        ( void ) xTaskNotifyIndexed( xTaskGetCurrentTaskHandle(),
                                     uxIndexToWaitOn,
                                     0,
                                     eNoAction );
    }
    return( ulTargetBits & ulNotifyValueAccumulate );
}

static BaseType_t xWaitForMxStatus( MxNetConnectCtx_t * pxCtx, MxStatus_t xTargetStatus, TickType_t xTicksToWait )
{
    TickType_t xRemainingTicks = xTicksToWait;
    TimeOut_t xTimeOut;

	if( pxCtx->xStatus == xTargetStatus )
	{
	    return pdTRUE;
	}

	vTaskSetTimeOutState( &xTimeOut );

	while( pxCtx->xStatus != xTargetStatus )
    {
	    uint32_t ulNotifyValue;
	    ulNotifyValue = ulWaitForNotifyBits( NET_EVT_IDX,
	                                         MX_STATUS_UPDATE_BIT,
	                                         xRemainingTicks );

        /* xTaskCheckForTimeOut adjusts xRemainingTicks */
        if( xTaskCheckForTimeOut( &xTimeOut, &xRemainingTicks ) == pdTRUE )
        {
            /* Timed out. Exit loop */
            break;
        }

    }
	return( pxCtx->xStatus == xTargetStatus );
}

BaseType_t net_request_reconnect( void )
{
    BaseType_t xReturn = pdFALSE;
    if( xNetTaskHandle != NULL )
    {
        xReturn = xTaskNotifyIndexed( xNetTaskHandle,
                                      NET_EVT_IDX,
                                      ASYNC_REQUEST_RECONNECT_BIT,
                                      eSetBits );
    }
    return xReturn;
}

/*
 * Handles network interface state change notifications from the control plane.
 */
static void vMxStatusNotify( MxStatus_t xNewStatus, void * pvCtx )
{
    MxNetConnectCtx_t * pxCtx = ( MxNetConnectCtx_t * ) pvCtx;

    LogDebug( "Mx Status notification: %s -> %s ",
              pcMxStatusToString( xPreviousStatus ),
              pcMxStatusToString( xNewStatus ) );

	pxCtx->xStatus = xNewStatus;

    (void) xTaskNotifyIndexed( xNetTaskHandle,
                               NET_EVT_IDX,
                               MX_STATUS_UPDATE_BIT,
                               eSetBits );
}

/* Callback for lwip netif events
 * netif_set_status_callback metif_set_link_callback */
static void vLwipStatusCallback( struct netif * pxNetif )
{
    static ip_addr_t xLastAddr;
    static uint8_t xLastFlags = 0;
    xLastAddr.addr = 0;

    uint32_t ulNotifyValue = 0;

    /* Check for change in flags */
    if( ( pxNetif->flags ^ xLastFlags ) & NETIF_FLAG_UP  )
    {
        if( pxNetif->flags & NETIF_FLAG_UP )
        {
            ulNotifyValue |= NET_LWIP_IFUP_BIT;
        }
        else
        {
            ulNotifyValue |= NET_LWIP_IFDOWN_BIT;
        }
    }
    else if ( ( pxNetif->flags ^ xLastFlags ) & NETIF_FLAG_LINK_UP )
    {
        if( pxNetif->flags & NETIF_FLAG_LINK_UP )
        {
            ulNotifyValue |= NET_LWIP_LINK_UP_BIT;
        }
        else
        {
            ulNotifyValue |= NET_LWIP_LINK_DOWN_BIT;
        }
    }

    if( pxNetif->ip_addr.addr != xLastAddr.addr )
    {
        ulNotifyValue |= NET_LWIP_IP_CHANGE_BIT;
    }

    if( ulNotifyValue > 0 )
    {
        (void) xTaskNotifyIndexed( xNetTaskHandle,
                                   NET_EVT_IDX,
                                   ulNotifyValue,
                                   eSetBits );
    }

    xLastAddr = pxNetif->ip_addr;
    xLastFlags = pxNetif->flags;
}

static void vLwipReadyCallback( void * pvCtx )
{
    TaskHandle_t xNetTaskHandle = ( TaskHandle_t ) pvCtx;

    if( xNetTaskHandle != NULL )
    {
        (void) xTaskNotifyIndexed( xNetTaskHandle,
                                   NET_EVT_IDX,
                                   NET_LWIP_READY_BIT,
                                   eSetBits );
    }
}

static BaseType_t xConnectToAP( MxNetConnectCtx_t * pxCtx )
{
    IPCError_t xErr = IPC_SUCCESS;


    if( pxCtx->xStatus == MX_STATUS_NONE ||
        pxCtx->xStatus == MX_STATUS_STA_DOWN )
    {
        xErr |= mx_SetBypassMode( WIFI_BYPASS_MODE_STATION,
                                  pdMS_TO_TICKS( MX_DEFAULT_TIMEOUT_MS ) );

        const char * pcSSID = (char *) ConfigStore_getEntryData( CS_WIFI_PREFERRED_AP_SSID );
        const char * pcPSK = (char *) ConfigStore_getEntryData( CS_WIFI_PREFERRED_AP_CREDENTIALS );

        xErr = mx_Connect( pcSSID, pcPSK, MX_TIMEOUT_CONNECT );

        if( xErr != IPC_SUCCESS)
        {
            LogError("Failed to connect to access point.");
        }
        else
        {
            ( void ) waitForMxStatus( MX_STATUS_STA_UP, pxCtx, MX_DEFAULT_TIMEOUT_TICK );
        }
    }

    return( pxCtx->xStatus >= MX_STATUS_STA_UP );
}

static void vInitDataPlaneCtx( MxDataplaneCtx_t * pxCtx )
{

}


static void vHandleMxStatusUpdate( MxNetConnectCtx_t * pxCtx )
{
    switch( pxCtx->xStatus )
    {
    case MX_STATUS_STA_UP:
    case MX_STATUS_STA_GOT_IP:
    case MX_STATUS_AP_UP:
        /* Set link up */
        netifapi_netif_set_link_up( &( pxCtx->xNetif ) );
        break;
    case MX_STATUS_NONE:
    case MX_STATUS_STA_DOWN:
    case MX_STATUS_AP_DOWN:
        netifapi_netif_set_link_down( &( pxCtx->xNetif ) );
        break;
    default:
        LogWarn( "Unknown mxchip status indication: %d", xCtx.xStatus );

        /* Fail safe to setting link up */
        netifapi_netif_set_link_up( &( pxCtx->xNetif ) );
        break;
    }
}

static void vInitializeWifiModule( MxNetConnectCtx_t * pxCtx )
{
    IPCError_t xErr = IPC_ERROR_INTERNAL;

    while( xErr != IPC_SUCCESS )
    {
        /* Query mac address and firmware revision */
        xErr = mx_RequestVersion( xCtx.pcFirmwareRevision, MX_FIRMWARE_REVISION_SIZE, portMAX_DELAY );

        /* Ensure null termination */
        xCtx.pcFirmwareRevision[ MX_FIRMWARE_REVISION_SIZE ] = '\0';

        if( xErr != IPC_SUCCESS )
        {
            LogError("Error while querying module firmware revision.");
        }

        if( xErr == IPC_SUCCESS )
        {
            /* Request mac address */
            xErr = mx_GetMacAddress( &( xCtx.xMacAddress ), portMAX_DELAY );

            if( xErr != IPC_SUCCESS )
            {
                LogError("Error while querying wifi module mac address.");
            }
        }

        if( xErr != IPC_SUCCESS )
        {
            vTaskDelay( MACADDR_RETRY_WAIT_TIME_TICKS );
        }
        else
        {
            LogInfo( "Firmware Version:   %s", xCtx.pcFirmwareRevision );
            LogInfo( "HW Address:         %02X.%02X.%02X.%02X.%02X.%02X",
                    xCtx.xMacAddress.addr[0], xCtx.xMacAddress.addr[1],
                    xCtx.xMacAddress.addr[2], xCtx.xMacAddress.addr[3],
                    xCtx.xMacAddress.addr[4], xCtx.xMacAddress.addr[5] );
        }
    }
}

static void vDoLinkHealthCheck( MxNetConnectCtx_t * pxCtx )
{

}

static void vNetConnectMainLoop( MxNetConnectCtx_t * pxCtx )
{

}

static void xInitializeContexts( MxNetConnectCtx_t * pxCtx )
{
    MessageBufferHandle_t xControlPlaneResponseBuff;
    QueueHandle_t xControlPlaneSendQueue;
    QueueHandle_t xDataPlaneSendQueue;

    /* Construct queues */
    xDataPlaneSendQueue = xQueueCreate( CONTROL_PLANE_QUEUE_LEN, sizeof( PacketBuffer_t * ) );
    configASSERT( xDataPlaneSendQueue != NULL );

    xControlPlaneResponseBuff = xMessageBufferCreate( CONTROL_PLANE_BUFFER_SZ );
    configASSERT( xControlPlaneResponseBuff != NULL );

    xControlPlaneSendQueue = xQueueCreate( CONTROL_PLANE_QUEUE_LEN, sizeof( PacketBuffer_t * ) );
    configASSERT( xControlPlaneSendQueue != NULL );


    /* Initialize wifi connect context */
    pxCtx->xStatus = MX_STATUS_NONE;

    ( void ) memset( &( pxCtx->pcFirmwareRevision ), 0, MX_FIRMWARE_REVISION_SIZE );
    ( void ) memset( &( pxCtx->xMacAddress ), 0, sizeof( MacAddress_t ) );

    pxCtx->xDataPlaneSendQueue = xDataPlaneSendQueue;
    pxCtx->pulTxPacketsWaiting = &( xDataPlaneCtx.ulTxPacketsWaiting );


    /* Construct dataplane context */
    extern SPI_HandleTypeDef hspi2;

    /* Initialize GPIO pins map / handles */
    xDataPlaneCtx.gpio_flow = &( xGpioMap[ GPIO_MX_FLOW ] );
    xDataPlaneCtx.gpio_reset = &( xGpioMap[ GPIO_MX_RESET ] );
    xDataPlaneCtx.gpio_nss = &( xGpioMap[ GPIO_MX_NSS ] );
    xDataPlaneCtx.gpio_notify = &( xGpioMap[ GPIO_MX_NOTIFY ] );

    /* Set SPI handle */
    xDataPlaneCtxpxSpiHandle = &hspi2;

    /* Initialize waiting packet counters */
    xDataPlaneCtx.ulRxPacketsWaiting = 0;
    xDataPlaneCtx.ulTxPacketsWaiting = 0;


    /* Set queue handles */
    xDataPlaneCtx.xControlPlaneSendQueue = xControlPlaneSendQueue;
    xDataPlaneCtx.xControlPlaneResponseBuff = xControlPlaneResponseBuff;
    xDataPlaneCtx.xDataPlaneSendQueue = xDataPlaneSendQueue;
    xDataPlaneCtx.pxNetif = &( pxCtx->xNetif );

    /* Construct controlplane context */
    xControlPlaneCtx.pxEventCallbackCtx = &xCtx;
    xControlPlaneCtx.xEventCallback = vMxStatusNotify;
    xControlPlaneCtx.xControlPlaneResponseBuff = xControlPlaneResponseBuff;
    xControlPlaneCtx.xDataPlaneTaskHandle = NULL;
    xControlPlaneCtx.xControlPlaneSendQueue = xControlPlaneSendQueue;

}

/*
 * Networking thread main function.
 */
void net_main( void * pvParameters )
{
	BaseType_t xResult = 0;


    MxNetConnectCtx_t xCtx;
    struct netif * pxNetif = &( xCtx.xNetif );

    /* Set static task handle var for callbacks */
    xNetTaskHandle = xTaskGetCurrentTaskHandle();



    /* Initialize lwip */
    tcpip_init( vLwipReadyCallback, xTaskGetCurrentTaskHandle() );

    /* Wait for lwip ready callback */
    xResult = ulWaitForNotifyBits( NET_EVT_IDX,
                                   NET_LWIP_READY_BIT,
                                   portMAX_DELAY );

	/* Start dataplane thread (does hw reset on initialization) */
	xResult = xTaskCreate( &vDataplaneThread,
	                       "MxDataPlane",
	                       4096,
	                       &xDataPlaneCtx,
	                       25,
	                       &xDataPlaneCtx.xDataPlaneTaskHandle );

	configASSERT( xResult == pdTRUE );
	xControlPlaneCtx.xDataPlaneTaskHandle = xDataPlaneCtx.xDataPlaneTaskHandle;

	/* Start control plane thread */
	xResult = xTaskCreate( &prvControlPlaneRouter,
	                       "MxControlPlaneRouter",
	                       4096,
	                       &xControlPlaneCtx,
	                       24,
	                       NULL );

	configASSERT( xResult == pdTRUE );

	/* vInitializeWifiModule returns after receiving a firmware revision and mac address */
	vInitializeWifiModule( &xCtx );


	err_t xLwipError;

    /* Set lwip status callback */
	pxNetif->status_callback = vLwipStatusCallback;
	pxNetif->link_callback = vLwipStatusCallback;

    /* Register lwip netif */
	xLwipError = netifapi_netif_add( pxNetif,
                                     NULL, NULL, NULL,
                                     &xCtx,
                                     &prvInitNetInterface,
                                     tcpip_input );

	configASSERT( xLwipError == ERR_OK );\

	netifapi_netif_set_default( pxNetif );

	netifapi_netif_set_up( pxNetif );

	/* If already connected to the AP, bring interface up */
	if( xCtx.xStatus >= MX_STATUS_STA_UP )
	{
	    netifapi_netif_set_link_up( pxNetif );
	    xLwipError = netifapi_dhcp_start( pxNetif );
	}

	MxStatus_t xLastMxStatus = MX_STATUS_NONE;

	/* Outer loop. Reinitializing */
	for( ; ; )
	{
	    /* Make a connection attempt */
	    if( xCtx.xStatus != MX_STATUS_STA_UP &&
	        xCtx.xStatus != MX_STATUS_STA_GOT_IP )
	    {
	        xConnectToAP( &xCtx );
	    }

	    /*
         * Wait for any event or timeout after 5 minutes.
	     * TODO: Backoff timer when not connected
	     * TODO: Constant delay when connected
	     */
	    xResult = waitForNotification( 0xFFFFFFFF, pdMS_TO_TICKS( 30 * 1000 ) );

	    if( xResult != 0 )
	    {
	        /* State update from driver while connected */
	        if( ( xResult & MX_STATUS_UPDATE_BIT ) &&
	            xCtx.xStatus != xLastMxStatus )
	        {
	            vHandleMxStatusUpdate( &xCtx );
	        }

	        if( xResult & NET_LWIP_READY_BIT )
	        {

	        }

	        if( xResult )

	        if( xResult & vHandleMxStatusUpdate )

	            /* Make a connection attempt */
	            if( xCtx.xStatus < MX_STATUS_STA_UP )
	            {
	                xConnectToAP( &xCtx );
	            }
	            /* Link down, but ip still assigned -> end dhcp */
	            else if( ( pxNetif->flags & NETIF_FLAG_LINK_UP ) == 0 &&
	                     pxNetif->ip_addr.addr != 0 )
	            {
	                xLwipError = netifapi_dhcp_release_and_stop( pxNetif );

	                if( xLwipError != ERR_OK )
	                {
	                    LogError( "lwip dhcp_release returned err code %d.", xLwipError );
	                }
	            }
	            /* Link up without an IP -> start DHCP */
	            else if( ( pxNetif->flags & NETIF_FLAG_LINK_UP ) != 0 &&
	                     pxNetif->ip_addr.addr == 0 )
	            {
                    xLwipError = netifapi_dhcp_start( pxNetif );

                    if( xLwipError != ERR_OK )
                    {
                        LogError( "lwip dhcp_start returned err code %d.", xLwipError );
                    }
	            }
	        }

	        /* Reconnect requested by configStore or cli process */
	        if( xResult & ASYNC_REQUEST_RECONNECT_BIT )
	        {
	            ( void ) mx_Disconnect( pdMS_TO_TICKS( 1000 ) );
	            xConnectToAP( &xCtx );
	        }
	    }
	}

}