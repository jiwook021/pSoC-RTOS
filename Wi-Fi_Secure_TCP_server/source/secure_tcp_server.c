/******************************************************************************
* File Name:   secure_tcp_server.c
*
* Description: This file contains declaration of task and functions related to
* TCP server operation.
*
*******************************************************************************
* Copyright 2019-2024, Cypress Semiconductor Corporation (an Infineon company) or
* an affiliate of Cypress Semiconductor Corporation.  All rights reserved.
*
* This software, including source code, documentation and related
* materials ("Software") is owned by Cypress Semiconductor Corporation
* or one of its affiliates ("Cypress") and is protected by and subject to
* worldwide patent protection (United States and foreign),
* United States copyright laws and international treaty provisions.
* Therefore, you may use this Software only as provided in the license
* agreement accompanying the software package from which you
* obtained this Software ("EULA").
* If no EULA applies, Cypress hereby grants you a personal, non-exclusive,
* non-transferable license to copy, modify, and compile the Software
* source code solely for use in connection with Cypress's
* integrated circuit products.  Any reproduction, modification, translation,
* compilation, or representation of this Software except as specified
* above is prohibited without the express written permission of Cypress.
*
* Disclaimer: THIS SOFTWARE IS PROVIDED AS-IS, WITH NO WARRANTY OF ANY KIND,
* EXPRESS OR IMPLIED, INCLUDING, BUT NOT LIMITED TO, NONINFRINGEMENT, IMPLIED
* WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE. Cypress
* reserves the right to make changes to the Software without notice. Cypress
* does not assume any liability arising out of the application or use of the
* Software or any product or circuit described in the Software. Cypress does
* not authorize its products for use in any products where a malfunction or
* failure of the Cypress product may reasonably be expected to result in
* significant property damage, injury or death ("High Risk Product"). By
* including Cypress's product in a High Risk Product, the manufacturer
* of such system or application assumes all risk of such use and in doing
* so agrees to indemnify Cypress against all liability.
*******************************************************************************/

/* Header file includes */
#include "cyhal.h"
#include "cybsp.h"
#include "cy_retarget_io.h"

/* FreeRTOS header file */
#include <FreeRTOS.h>
#include <task.h>

/* Cypress secure socket header file */
#include "cy_secure_sockets.h"
#include "cy_tls.h"

/* Wi-Fi connection manager header files */
#include "cy_wcm.h"
#include "cy_wcm_error.h"

/* Standard C header file */
#include <string.h>

#include "network_credentials.h"

/* TCP server task header file. */
#include "secure_tcp_server.h"

/* IP address related header files (part of the lwIP TCP/IP stack). */
#include "ip_addr.h"

/* to use the portable formatting macros */
#include <inttypes.h>

/*******************************************************************************
* Macros
********************************************************************************/                                             

/* Length of the LED ON/OFF command issued from the TCP server. */
#define TCP_LED_CMD_LEN                                (1)

/* LED ON and LED OFF commands. */
#define LED_ON_CMD                                     '1'
#define LED_OFF_CMD                                    '0'

/* Interrupt priority of the user button. */
#define USER_BTN_INTR_PRIORITY                         (5)

/*******************************************************************************
* Function Prototypes
********************************************************************************/
cy_rslt_t create_secure_tcp_server_socket(void);
cy_rslt_t tcp_connection_handler(cy_socket_t socket_handle, void *arg);
cy_rslt_t tcp_receive_msg_handler(cy_socket_t socket_handle, void *arg);
cy_rslt_t tcp_disconnection_handler(cy_socket_t socket_handle, void *arg);
void isr_button_press( void *callback_arg, cyhal_gpio_event_t event);
void print_heap_usage(char *msg);

#if(USE_AP_INTERFACE)
    static cy_rslt_t softap_start(void);
#else
    static cy_rslt_t connect_to_wifi_ap(void);
#endif /* USE_AP_INTERFACE */

/*******************************************************************************
* Global Variables
********************************************************************************/
/* Secure socket variables. */
cy_socket_sockaddr_t tcp_server_addr, peer_addr;
cy_socket_t server_handle, client_handle;
cy_wcm_ip_address_t ip_address;

/* TLS credentials of the TCP server. */
static const char tcp_server_cert[] = keySERVER_CERTIFICATE_PEM;
static const char server_private_key[] = keySERVER_PRIVATE_KEY_PEM;

/* Root CA certificate for TCP client identity verification. */
static const char tcp_client_ca_cert[] = keyCLIENT_ROOTCA_PEM;

/* Variable to store the TLS identity (certificate and private key). */
void *tls_identity;

/* Size of the peer socket address. */
uint32_t peer_addr_len;

/* Flags to tack the LED state and command. */
bool led_state = CYBSP_LED_STATE_OFF;

/* Secure TCP server task handle. */
extern TaskHandle_t server_task_handle;

/* Flag variable to check if TCP client is connected. */
bool client_connected;

cyhal_gpio_callback_data_t cb_data =
{
.callback = isr_button_press,
.callback_arg = NULL
};

/*******************************************************************************
 * Function Name: tcp_secure_server_task
 *******************************************************************************
 * Summary:
 *  Task used to establish a connection with a remote TCP client to exchange
 *  data between the TCP server and TCP client.
 *
 * Parameters:
 *  void *args: Task parameter defined during task creation (unused)
 *
 * Return:
 *  void
 *
 *******************************************************************************/
void tcp_secure_server_task(void *arg)
{
    cy_rslt_t result;
    /*The configuration in which WCM should be initialized*/
    cy_wcm_config_t wifi_config = { .interface = WIFI_INTERFACE_TYPE };

    /* Variable to store number of bytes sent over TCP socket. */
    uint32_t bytes_sent = 0;

    /* Variable to receive LED ON/OFF command from the user button ISR. */
    uint32_t led_state_cmd = LED_OFF_CMD;

    /* Initialize the user button (CYBSP_USER_BTN) and register interrupt on falling edge. */
    cyhal_gpio_init(CYBSP_USER_BTN, CYHAL_GPIO_DIR_INPUT, CYHAL_GPIO_DRIVE_PULLUP, CYBSP_BTN_OFF);
    cyhal_gpio_register_callback(CYBSP_USER_BTN, &cb_data);
    cyhal_gpio_enable_event(CYBSP_USER_BTN, CYHAL_GPIO_IRQ_FALL, USER_BTN_INTR_PRIORITY, true);

    /* TCP server certificate length and private key length. */
    const size_t tcp_server_cert_len = strlen( tcp_server_cert );
    const size_t pkey_len = strlen( server_private_key );

    /* Initialize Wi-Fi connection manager. */
    result = cy_wcm_init(&wifi_config);
    if (result != CY_RSLT_SUCCESS)
    {
        printf("Wi-Fi Connection Manager initialization failed! Error code: 0x%08"PRIx32"\n", (uint32_t)result);
        CY_ASSERT(0);
    }
    printf("Wi-Fi Connection Manager initialized.\r\n");

    #if(USE_AP_INTERFACE)
        /* Start the Wi-Fi device as a Soft AP interface. */
        result = softap_start();
        if (result != CY_RSLT_SUCCESS)
        {
            printf("Failed to Start Soft AP! Error code: 0x%08"PRIx32"\n", (uint32_t)result);
            CY_ASSERT(0);
        }
    #else
        /* Connect to Wi-Fi AP */
        result = connect_to_wifi_ap();
        if(result != CY_RSLT_SUCCESS )
        {
            printf("\n Failed to connect to Wi-Fi AP! Error code: 0x%08"PRIx32"\n", (uint32_t)result);
            CY_ASSERT(0);
        }
    #endif /* USE_AP_INTERFACE */

    /* Initialize secure socket library. */
    result = cy_socket_init();
    if (result != CY_RSLT_SUCCESS)
    {
        printf("Secure Socket initialization failed! Error code: %"PRIu32"\n", result);
        CY_ASSERT(0);
    }
    printf("Secure Socket initialized\n");

    /* Create TCP server identity using the SSL certificate and private key. */
    result = cy_tls_create_identity(tcp_server_cert, tcp_server_cert_len,
                                    server_private_key, pkey_len, &tls_identity);
    if(result != CY_RSLT_SUCCESS)
    {
        printf("Failed cy_tls_create_identity! Error code: %"PRIu32"\n", result);
        CY_ASSERT(0);
    }

    /* Initializes the global trusted RootCA certificate. This examples uses a self signed
     * certificate which implies that the RootCA certificate is same as the TCP client
     * certificate. */
    result = cy_tls_load_global_root_ca_certificates(tcp_client_ca_cert, strlen(tcp_client_ca_cert));

    if( result != CY_RSLT_SUCCESS)
    {
        printf("cy_tls_load_global_root_ca_certificates failed! Error code: %"PRIu32"\n", result);
        CY_ASSERT(0);
    }
    else
    {
        printf("Global trusted RootCA certificate loaded\n");
    }

    /* Create secure TCP server socket. */
    result = create_secure_tcp_server_socket();
    if( result != CY_RSLT_SUCCESS)
    {
        printf("Failed to create socket! Error code: %"PRIu32"\n", result);
        CY_ASSERT(0);
    }

    /* Start listening on the secure TCP socket. */
    result = cy_socket_listen(server_handle, TCP_SERVER_MAX_PENDING_CONNECTIONS);
    if (result != CY_RSLT_SUCCESS)
    {
        cy_socket_delete(server_handle);
        printf("cy_socket_listen returned error. Error: %"PRIu32"\n", result);
        CY_ASSERT(0);
    }
    else
    {
        printf("===============================================================\n");
        printf("Listening for incoming TCP client connection on Port: %d\n",
                tcp_server_addr.port);
    }

    while(true)
    {
        /* Wait till user button is pressed to send LED ON/OFF command to TCP client. */
        xTaskNotifyWait(0, 0, &led_state_cmd, portMAX_DELAY);

        /* Send LED ON/OFF command to TCP client if there is an active
         *  TCP client connection. */
        if(client_connected)
        {
            /* Send the command to TCP client. */
            result = cy_socket_send(client_handle, &led_state_cmd, TCP_LED_CMD_LEN,
                           CY_SOCKET_FLAGS_NONE, &bytes_sent);
            if(result == CY_RSLT_SUCCESS )
            {
                if(led_state_cmd == LED_ON_CMD)
                {
                    printf("LED ON command sent to TCP client\n");
                }
                else
                {
                    printf("LED OFF command sent to TCP client\n");
                }

            }
            else
            {
                printf("Failed to send command to client. Error: %"PRIu32"\n", result);
                if(result == CY_RSLT_MODULE_SECURE_SOCKETS_CLOSED)
                {
                    /* Disconnect the socket. */
                    cy_socket_disconnect(client_handle, 0);
                    /* Delete the socket. */
                    cy_socket_delete(client_handle);
                }
            }
        }

        print_heap_usage("After sending LED ON/OFF command to client");
    }
 }

#if(!USE_AP_INTERFACE)
/*******************************************************************************
 * Function Name: connect_to_wifi_ap()
 *******************************************************************************
 * Summary:
 *  Connects to Wi-Fi AP using the user-configured credentials, retries up to a
 *  configured number of times until the connection succeeds.
 *
 * Return:
 *  cy_result result: Result of the operation.
 *
 *******************************************************************************/
cy_rslt_t connect_to_wifi_ap(void)
{
    cy_rslt_t result;

    /* Variables used by Wi-Fi connection manager.*/
    cy_wcm_connect_params_t wifi_conn_param;

    /* Set the Wi-Fi SSID, password and security type. */
    memset(&wifi_conn_param, 0, sizeof(cy_wcm_connect_params_t));
    memcpy(wifi_conn_param.ap_credentials.SSID, WIFI_SSID, sizeof(WIFI_SSID));
    memcpy(wifi_conn_param.ap_credentials.password, WIFI_PASSWORD, sizeof(WIFI_PASSWORD));
    wifi_conn_param.ap_credentials.security = WIFI_SECURITY_TYPE;

    /* Join the Wi-Fi AP. */
    for(uint32_t conn_retries = 0; conn_retries < MAX_WIFI_CONN_RETRIES; conn_retries++ )
    {
        result = cy_wcm_connect_ap(&wifi_conn_param, &ip_address);

        if(result == CY_RSLT_SUCCESS)
        {
            printf("Successfully connected to Wi-Fi network '%s'.\n",
                                wifi_conn_param.ap_credentials.SSID);

            /* IP address and TCP port number of the TCP server */
            #if(USE_IPV6_ADDRESS)
                /* Get the IPv6 address.*/
                result = cy_wcm_get_ipv6_addr(CY_WCM_INTERFACE_TYPE_STA, CY_WCM_IPV6_LINK_LOCAL, &ip_address);
                if(result == CY_RSLT_SUCCESS)
                {
                    printf("IPv6 address (link-local) assigned: %s\n",
                            ip6addr_ntoa((const ip6_addr_t*)&ip_address.ip.v6));
                    memcpy(ip_address.ip.v6, tcp_server_addr.ip_address.ip.v6, sizeof(ip_address.ip.v6));
                    tcp_server_addr.ip_address.version = CY_SOCKET_IP_VER_V6;
                }
            #else
                printf("IPv4 address assigned: %s\n", ip4addr_ntoa((const ip4_addr_t*)&ip_address.ip.v4));
                tcp_server_addr.ip_address.ip.v4 = ip_address.ip.v4;
                tcp_server_addr.ip_address.version = CY_SOCKET_IP_VER_V4;
            #endif /* USE_IPV6_ADDRESS */
            tcp_server_addr.port = TCP_SERVER_PORT;
            return result;
        }

        printf("Connection to Wi-Fi network failed with error code %"PRIu32"."
               "Retrying in %d ms...\n", result, WIFI_CONN_RETRY_INTERVAL_MSEC);
        vTaskDelay(pdMS_TO_TICKS(WIFI_CONN_RETRY_INTERVAL_MSEC));
    }

    /* Stop retrying after maximum retry attempts. */
    printf("Exceeded maximum Wi-Fi connection attempts\n");

    return result;
}
#endif

/*******************************************************************************
 * Function Name: create_secure_tcp_server_socket
 *******************************************************************************
 * Summary:
 *  Function to create a socket and set the socket options for configuring TLS
 *  identity, socket connection handler, message reception handler and
 *  socket disconnection handler.
 *
 * Return:
 *  cy_result result: Result of the operation.
 *
 *******************************************************************************/
cy_rslt_t create_secure_tcp_server_socket(void)
{
    cy_rslt_t result;
    /* TCP socket receive timeout period. */
    uint32_t tcp_recv_timeout = TCP_SERVER_RECV_TIMEOUT_MS;

    /* Variables used to set socket options. */
    cy_socket_opt_callback_t tcp_receive_option;
    cy_socket_opt_callback_t tcp_connection_option;
    cy_socket_opt_callback_t tcp_disconnect_option;

    /* TLS authentication mode.*/
    cy_socket_tls_auth_mode_t tls_auth_mode = CY_SOCKET_TLS_VERIFY_REQUIRED;

    /* Create a Secure TCP socket. */
    #if(USE_IPV6_ADDRESS)
    result = cy_socket_create(CY_SOCKET_DOMAIN_AF_INET6, CY_SOCKET_TYPE_STREAM,
                              CY_SOCKET_IPPROTO_TLS, &server_handle);
    #else
    result = cy_socket_create(CY_SOCKET_DOMAIN_AF_INET, CY_SOCKET_TYPE_STREAM,
                                  CY_SOCKET_IPPROTO_TLS, &server_handle);
    #endif
    if(result != CY_RSLT_SUCCESS)
    {
        printf("Failed to create socket! Error code: %"PRIu32"\n", result);
        return result;
    }

    /* Set the TCP socket receive timeout period. */
    result = cy_socket_setsockopt(server_handle, CY_SOCKET_SOL_SOCKET,
                                 CY_SOCKET_SO_RCVTIMEO, &tcp_recv_timeout,
                                 sizeof(tcp_recv_timeout));
    if(result != CY_RSLT_SUCCESS)
    {
        printf("Set socket option: CY_SOCKET_SO_RCVTIMEO failed! Error code: %"PRIu32"\n", result);
        return result;
    }

    /* Register the callback function to handle connection request from a TCP client. */
    tcp_connection_option.callback = tcp_connection_handler;
    tcp_connection_option.arg = NULL;

    result = cy_socket_setsockopt(server_handle, CY_SOCKET_SOL_SOCKET,
                                  CY_SOCKET_SO_CONNECT_REQUEST_CALLBACK,
                                  &tcp_connection_option, sizeof(cy_socket_opt_callback_t));
    if(result != CY_RSLT_SUCCESS)
    {
        printf("Set socket option: CY_SOCKET_SO_CONNECT_REQUEST_CALLBACK failed! Error code: %"PRIu32"\n", result);
        return result;
    }

    /* Register the callback function to handle messages received from a TCP client. */
    tcp_receive_option.callback = tcp_receive_msg_handler;
    tcp_receive_option.arg = NULL;

    result = cy_socket_setsockopt(server_handle, CY_SOCKET_SOL_SOCKET,
                                  CY_SOCKET_SO_RECEIVE_CALLBACK,
                                  &tcp_receive_option, sizeof(cy_socket_opt_callback_t));
    if(result != CY_RSLT_SUCCESS)
    {
        printf("Set socket option: CY_SOCKET_SO_RECEIVE_CALLBACK failed! Error code: %"PRIu32"\n", result);
        return result;
    }

    /* Register the callback function to handle disconnection. */
    tcp_disconnect_option.callback = tcp_disconnection_handler;
    tcp_disconnect_option.arg = NULL;

    result = cy_socket_setsockopt(server_handle, CY_SOCKET_SOL_SOCKET,
                                  CY_SOCKET_SO_DISCONNECT_CALLBACK,
                                  &tcp_disconnect_option, sizeof(cy_socket_opt_callback_t));
    if(result != CY_RSLT_SUCCESS)
    {
        printf("Set socket option: CY_SOCKET_SO_DISCONNECT_CALLBACK failed! Error code: %"PRIu32"\n", result);
        return result;
    }

    /* Set the TCP socket to use the TLS identity. */
    result = cy_socket_setsockopt(server_handle, CY_SOCKET_SOL_TLS, CY_SOCKET_SO_TLS_IDENTITY,
                                  tls_identity, sizeof((uint32_t)tls_identity));
    if(result != CY_RSLT_SUCCESS)
    {
        printf("Failed cy_socket_setsockopt! Error code: %"PRIu32"\n", result);
        return result;
    }

    /* Set the TLS authentication mode. */
    cy_socket_setsockopt(server_handle, CY_SOCKET_SOL_TLS, CY_SOCKET_SO_TLS_AUTH_MODE,
                        &tls_auth_mode, sizeof(cy_socket_tls_auth_mode_t));

     /* Bind the TCP socket created to Server IP address and to TCP port. */
    result = cy_socket_bind(server_handle, &tcp_server_addr, sizeof(tcp_server_addr));
    if(result != CY_RSLT_SUCCESS)
    {
        printf("Failed to bind to socket! Error code: %"PRIu32"\n", result);
    }

    return result;
}

/*******************************************************************************
 * Function Name: tcp_connection_handler
 *******************************************************************************
 * Summary:
 *  Callback function to handle incoming secure TCP client connection.
 *
 * Parameters:
 * cy_socket_t socket_handle: Connection handle for the TCP server socket
 *  void *args : Parameter passed on to the function (unused)
 *
 * Return:
 *  cy_result result: Result of the operation
 *
 *******************************************************************************/
cy_rslt_t tcp_connection_handler(cy_socket_t socket_handle, void *arg)
{
    cy_rslt_t result;
    /* Accept new incoming connection from a TCP client and
     * perform TLS handshake. */
    result = cy_socket_accept(socket_handle, &peer_addr, &peer_addr_len,
                              &client_handle);
    if(result == CY_RSLT_SUCCESS)
    {
        printf("Incoming TCP connection accepted\n");
        printf("TLS Handshake successful and communication secured!\n");
        printf("Press the user button to send LED ON/OFF command to the TCP client\n");

        /* Set the client connection flag as true. */
        client_connected = true;
    }
    else
    {
        printf("Failed to accept incoming client connection. Error: %"PRIu32"\n", result);
        printf("===============================================================\n");
        printf("Listening for incoming TCP client connection on Port: %d\n",
                tcp_server_addr.port);
    }

    return result;
}

 /*******************************************************************************
 * Function Name: tcp_receive_msg_handler
 *******************************************************************************
 * Summary:
 *  Callback function to handle incoming TCP client messages.
 *
 * Parameters:
 * cy_socket_t socket_handle: Connection handle for the TCP client socket
 *  void *args : Parameter passed on to the function (unused)
 *
 * Return:
 *  cy_result result: Result of the operation
 *
 *******************************************************************************/
cy_rslt_t tcp_receive_msg_handler(cy_socket_t socket_handle, void *arg)
{
    char message_buffer[MAX_TCP_RECV_BUFFER_SIZE];
    cy_rslt_t result;

    /* Variable to store number of bytes received from TCP client. */
    uint32_t bytes_received = 0;
    result = cy_socket_recv(socket_handle, message_buffer, MAX_TCP_RECV_BUFFER_SIZE,
                            CY_SOCKET_FLAGS_NONE, &bytes_received);

    if(result == CY_RSLT_SUCCESS)
    {
        /* Terminate the received string with '\0'. */
        message_buffer[bytes_received] = '\0';
        printf("\r\nAcknowledgement from TCP Client: %s\n", message_buffer);

        /* Set the LED state based on the acknowledgement received from the TCP client. */
        if(strcmp(message_buffer, "LED ON ACK") == 0)
        {
            led_state = CYBSP_LED_STATE_ON;
        }
        else
        {
            led_state = CYBSP_LED_STATE_OFF;
        }
    }
    else
    {
        printf("Failed to receive acknowledgement from the secure TCP client. Error: %"PRIu32"\n",
        result);
        if(result == CY_RSLT_MODULE_SECURE_SOCKETS_CLOSED)
        {
            /* Disconnect the socket. */
            cy_socket_disconnect(socket_handle, 0);
            /* Delete the socket. */
            cy_socket_delete(socket_handle);
        }
    }

    print_heap_usage("After receiving ACK from client");

    printf("===============================================================\n");
    printf("Press the user button to send LED ON/OFF command to the TCP client\n");

    return result;
}

 /*******************************************************************************
 * Function Name: tcp_disconnection_handler
 *******************************************************************************
 * Summary:
 *  Callback function to handle TCP client disconnection event.
 *
 * Parameters:
 * cy_socket_t socket_handle: Connection handle for the TCP client socket
 *  void *args : Parameter passed on to the function (unused)
 *
 * Return:
 *  cy_result result: Result of the operation
 *
 *******************************************************************************/
cy_rslt_t tcp_disconnection_handler(cy_socket_t socket_handle, void *arg)
{
    cy_rslt_t result;

    /* Disconnect the TCP client. */
    result = cy_socket_disconnect(socket_handle, 0);
    /* Delete the socket. */
    cy_socket_delete(socket_handle);

    /* Set the client connection flag as false. */
    client_connected = false;
    printf("TCP Client disconnected! Please reconnect the TCP Client\n");
    printf("===============================================================\n");
    printf("Listening for incoming TCP client connection on Port:%d\n",
            tcp_server_addr.port);

    return result;
}

/*******************************************************************************
 * Function Name: isr_button_press
 *******************************************************************************
 *
 * Summary:
 *  GPIO interrupt service routine. This function detects button
 *  presses and sets the command to be sent to the secure TCP client.
 *
 * Parameters:
 *  void *callback_arg : pointer to the variable passed to the ISR
 *  cyhal_gpio_event_t event : GPIO event type
 *
 * Return:
 *  None
 *
 *******************************************************************************/
void isr_button_press( void *callback_arg, cyhal_gpio_event_t event)
{
    BaseType_t xHigherPriorityTaskWoken = pdFALSE;

    /* Variable to hold the LED ON/OFF command to be sent to the TCP client. */
    uint32_t led_state_cmd;

    /* Set the command to be sent to TCP client. */
    if(led_state == CYBSP_LED_STATE_ON)
    {
        led_state_cmd = LED_OFF_CMD;
    }
    else
    {
        led_state_cmd = LED_ON_CMD;
    }
    
    /* Set the flag to send command to TCP client. */
    xTaskNotifyFromISR(server_task_handle, led_state_cmd,
                      eSetValueWithoutOverwrite, &xHigherPriorityTaskWoken);

    /* Force a context switch if xHigherPriorityTaskWoken is now set to pdTRUE. */
    portYIELD_FROM_ISR(xHigherPriorityTaskWoken);
}

#if(USE_AP_INTERFACE)
/********************************************************************************
 * Function Name: softap_start
 ********************************************************************************
 * Summary:
 *  This function configures device in AP mode and initializes
 *  a SoftAP with the given credentials (SOFTAP_SSID, SOFTAP_PASSWORD and
 *  SOFTAP_SECURITY_TYPE).
 *
 * Parameters:
 *  void
 *
 * Return:
 *  cy_rslt_t: Returns CY_RSLT_SUCCESS if the Soft AP is started successfully,
 *  a WCM error code otherwise.
 *
 *******************************************************************************/
static cy_rslt_t softap_start(void)
{
    cy_rslt_t result = CY_RSLT_SUCCESS;

    /* Initialize the Wi-Fi device as a Soft AP. */
    cy_wcm_ap_credentials_t softap_credentials = {SOFTAP_SSID, SOFTAP_PASSWORD,
                                                  SOFTAP_SECURITY_TYPE};
    static const cy_wcm_ip_setting_t softap_ip_info = {
        INITIALISER_IPV4_ADDRESS(.ip_address, SOFTAP_IP_ADDRESS),
        INITIALISER_IPV4_ADDRESS(.gateway,    SOFTAP_GATEWAY),
        INITIALISER_IPV4_ADDRESS(.netmask,    SOFTAP_NETMASK)
        };

    cy_wcm_ap_config_t softap_config = {softap_credentials, SOFTAP_RADIO_CHANNEL,
                                        softap_ip_info,
                                        NULL};

    /* Start the the Wi-Fi device as a Soft AP. */
    result = cy_wcm_start_ap(&softap_config);
    if(result == CY_RSLT_SUCCESS)
    {
        printf("Wi-Fi Device configured as Soft AP\n");
        printf("Connect TCP client device to the network: SSID: %s Password:%s\n",
                SOFTAP_SSID, SOFTAP_PASSWORD);
    #if(USE_IPV6_ADDRESS)
        /* Get the IPv6 address.*/
        result = cy_wcm_get_ipv6_addr(CY_WCM_INTERFACE_TYPE_AP, CY_WCM_IPV6_LINK_LOCAL, &ip_address);
        if(result == CY_RSLT_SUCCESS)
        {
            printf("SofAP : IPv6 address (link-local) assigned: %s\n",
                   ip6addr_ntoa((const ip6_addr_t*)&ip_address.ip.v6));
            memcpy(ip_address.ip.v6, tcp_server_addr.ip_address.ip.v6, sizeof(ip_address.ip.v6));
            tcp_server_addr.ip_address.version = CY_SOCKET_IP_VER_V6;
        }
    #else
        printf("SofAP : IPv4 address assigned : %s\n\n",
                ip4addr_ntoa((const ip4_addr_t *)&softap_ip_info.ip_address.ip.v4));

        /* IP address and TCP port number of the TCP server. */
        tcp_server_addr.ip_address.ip.v4 = softap_ip_info.ip_address.ip.v4;
        tcp_server_addr.ip_address.version = CY_SOCKET_IP_VER_V4;
    #endif            
        tcp_server_addr.port = TCP_SERVER_PORT;

    }

    return result;
}
#endif /* USE_AP_INTERFACE */

/* [] END OF FILE */
