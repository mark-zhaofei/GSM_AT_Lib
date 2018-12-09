/**
 * \file            gsm_netconn.c
 * \brief           API functions for sequential calls
 */

/*
 * Copyright (c) 2018 Tilen Majerle
 *
 * Permission is hereby granted, free of charge, to any person
 * obtaining a copy of this software and associated documentation
 * files (the "Software"), to deal in the Software without restriction,
 * including without limitation the rights to use, copy, modify, merge,
 * publish, distribute, sublicense, and/or sell copies of the Software,
 * and to permit persons to whom the Software is furnished to do so,
 * subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be
 * included in all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND,
 * EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES
 * OF MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE
 * AND NONINFRINGEMENT. IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT
 * HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER LIABILITY,
 * WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING
 * FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR
 * OTHER DEALINGS IN THE SOFTWARE.
 *
 * This file is part of GSM-AT library.
 *
 * Author:          Tilen MAJERLE <tilen@majerle.eu>
 */
#include "gsm/gsm_netconn.h"
#include "gsm/gsm_private.h"
#include "gsm/gsm_conn.h"
#include "gsm/gsm_mem.h"

#if GSM_CFG_NETCONN || __DOXYGEN__

/* Check conditions */
#if !GSM_CFG_CONN
#error "GSM_CFG_CONN must be enabled for NETCONN API!"
#endif /* !GSM_CFG_CONN */

/**
 * \brief           Sequential API structure
 */
typedef struct gsm_netconn {
    struct gsm_netconn* next;                   /*!< Linked list entry */

    gsm_netconn_type_t type;                    /*!< Netconn type */

    size_t rcv_packets;                         /*!< Number of received packets so far on this connection */
    gsm_conn_p conn;                            /*!< Pointer to actual connection */

    gsm_sys_mbox_t mbox_receive;                /*!< Message queue for receive mbox */

    gsm_linbuff_t buff;                         /*!< Linear buffer structure */

    uint16_t conn_timeout;                      /*!< Connection timeout in units of seconds when
                                                    netconn is in server (listen) mode.
                                                    Connection will be automatically closed if there is no
                                                    data exchange in time. Set to `0` when timeout feature is disabled. */

#if GSM_CFG_NETCONN_RECEIVE_TIMEOUT || __DOXYGEN__
    uint32_t rcv_timeout;                       /*!< Receive timeout in unit of milliseconds */
#endif
} gsm_netconn_t;

static uint8_t recv_closed = 0xFF, recv_not_present = 0xFF;
static gsm_netconn_t* netconn_list;             /*!< Linked list of netconn entries */

/**
 * \brief           Flush all mboxes and clear possible used memories
 * \param[in]       nc: Pointer to netconn to flush
 */
static void
flush_mboxes(gsm_netconn_t* nc, uint8_t protect) {
    gsm_pbuf_p pbuf;
    if (protect) {
        gsm_core_lock();                        /* Protect core */
    }
    if (gsm_sys_mbox_isvalid(&nc->mbox_receive)) {
        while (gsm_sys_mbox_getnow(&nc->mbox_receive, (void **)&pbuf)) {
            if (pbuf != NULL && (uint8_t *)pbuf != (uint8_t *)&recv_closed) {
                gsm_pbuf_free(pbuf);            /* Free received data buffers */
            }
        }
        gsm_sys_mbox_delete(&nc->mbox_receive); /* Delete message queue */
        gsm_sys_mbox_invalid(&nc->mbox_receive);/* Invalid handle */
    }
    if (protect) {
        gsm_core_unlock();                      /* Release protection */
    }
}

/**
 * \brief           Callback function for every server connection
 * \param[in]       evt: Pointer to callback structure
 * \return          Member of \ref gsmr_t enumeration
 */
static gsmr_t
netconn_evt(gsm_evt_t* evt) {
    gsm_conn_p conn;
    gsm_netconn_t* nc = NULL;
    uint8_t close = 0;

    conn = gsm_conn_get_from_evt(evt);          /* Get connection from event */
    switch (gsm_evt_get_type(evt)) {
        /*
         * A new connection has been active
         * and should be handled by netconn API
         */
        case GSM_EVT_CONN_ACTIVE: {             /* A new connection active is active */
            if (gsm_conn_is_client(conn)) {     /* Was connection started by us? */
                nc = gsm_conn_get_arg(conn);    /* Argument should be already set */
                if (nc != NULL) {
                    nc->conn = conn;            /* Save actual connection */
                } else {
                    close = 1;                  /* Close this connection, invalid netconn */
                }
            } else {
                GSM_DEBUGF(GSM_CFG_DBG_NETCONN | GSM_DBG_TYPE_TRACE | GSM_DBG_LVL_WARNING,
                    "[NETCONN] Closing connection, it is not in client mode!\r\n");
                close = 1;                      /* Close the connection at this point */
            }

            /* Decide if some events want to close the connection */
            if (close) {
                if (nc != NULL) {
                    gsm_conn_set_arg(conn, NULL);   /* Reset argument */
                    gsm_netconn_delete(nc);     /* Free memory for API */
                }
                gsm_conn_close(conn, 0);        /* Close the connection */
                close = 0;
            }
            break;
        }

        /*
         * We have a new data received which
         * should have netconn structure as argument
         */
        case GSM_EVT_CONN_DATA_RECV: {
            gsm_pbuf_p pbuf;

            nc = gsm_conn_get_arg(conn);        /* Get API from connection */
            pbuf = gsm_evt_conn_data_recv_get_buff(evt);/* Get received buff */

            gsm_conn_recved(conn, pbuf);        /* Notify stack about received data */
            nc->rcv_packets++;                  /* Increase number of received packets */

            /*
             * First increase reference number to prevent
             * other thread to process the incoming packet
             * and free it while we still need it here
             *
             * In case of problems writing packet to queue,
             * simply force free to decrease reference counter back to previous value
             */
            gsm_pbuf_ref(pbuf);                 /* Increase reference counter */
            if (!nc || !gsm_sys_mbox_isvalid(&nc->mbox_receive)
                || !gsm_sys_mbox_putnow(&nc->mbox_receive, pbuf)) {
                GSM_DEBUGF(GSM_CFG_DBG_NETCONN,
                    "[NETCONN] Ignoring more data for receive!\r\n");
                gsm_pbuf_free(pbuf);            /* Free pbuf */
                return gsmOKIGNOREMORE;         /* Return OK to free the memory and ignore further data */
            }
            GSM_DEBUGF(GSM_CFG_DBG_NETCONN | GSM_DBG_TYPE_TRACE,
                "[NETCONN] Written %d bytes to receive mbox\r\n",
                (int)gsm_pbuf_length(pbuf, 0));
            break;
        }

        /* Connection was just closed */
        case GSM_EVT_CONN_CLOSED: {
            nc = gsm_conn_get_arg(conn);        /* Get API from connection */

            /*
             * In case we have a netconn available,
             * simply write pointer to received variable to indicate closed state
             */
            if (nc != NULL && gsm_sys_mbox_isvalid(&nc->mbox_receive)) {
                gsm_sys_mbox_putnow(&nc->mbox_receive, (void *)&recv_closed);
            }

            break;
        }
        default:
            return gsmERR;
    }
    return gsmOK;
}

/**
 * \brief           Global event callback function
 * \param[in]       evt: Callback information and data
 * \return          \ref gsmOK on success, member of \ref gsmr_t otherwise
 */
static gsmr_t
gsm_evt(gsm_evt_t* evt) {
    switch (gsm_evt_get_type(evt)) {
        default: break;
    }
    return gsmOK;
}

/**
 * \brief           Create new netconn connection
 * \param[in]       type: Netconn connection type
 * \return          New netconn connection on success, `NULL` otherwise
 */
gsm_netconn_p
gsm_netconn_new(gsm_netconn_type_t type) {
    gsm_netconn_t* a;
    static uint8_t first = 1;

    /* Register only once! */
    GSM_CORE_PROTECT();
    if (first) {
        first = 0;
        gsm_evt_register(gsm_evt);              /* Register global event function */
    }
    GSM_CORE_UNPROTECT();
    a = gsm_mem_calloc(1, sizeof(*a));          /* Allocate memory for core object */
    if (a != NULL) {
        a->type = type;                         /* Save netconn type */
        a->conn_timeout = 0;                    /* Default connection timeout */
        if (!gsm_sys_mbox_create(&a->mbox_receive, GSM_CFG_NETCONN_RECEIVE_QUEUE_LEN)) {    /* Allocate memory for receiving message box */
            GSM_DEBUGF(GSM_CFG_DBG_NETCONN | GSM_DBG_TYPE_TRACE | GSM_DBG_LVL_DANGER,
                "[NETCONN] Cannot create receive MBOX\r\n");
            goto free_ret;
        }
        GSM_CORE_PROTECT();
        if (netconn_list == NULL) {             /* Add new netconn to the existing list */
            netconn_list = a;
        } else {
            a->next = netconn_list;             /* Add it to beginning of the list */
            netconn_list = a;
        }
        GSM_CORE_UNPROTECT();
    }
    return a;
free_ret:
    if (gsm_sys_mbox_isvalid(&a->mbox_receive)) {
        gsm_sys_mbox_delete(&a->mbox_receive);
        gsm_sys_mbox_invalid(&a->mbox_receive);
    }
    if (a != NULL) {
        gsm_mem_free(a);
        a = NULL;
    }
    return NULL;
}

/**
 * \brief           Delete netconn connection
 * \param[in]       nc: Netconn handle
 * \return          \ref gsmOK on success, member of \ref gsmr_t enumeration otherwise
 */
gsmr_t
gsm_netconn_delete(gsm_netconn_p nc) {
    GSM_ASSERT("netconn != NULL", nc != NULL);  /* Assert input parameters */

    GSM_CORE_PROTECT();

    flush_mboxes(nc, 0);                        /* Clear mboxes */

    /* Remove netconn from linkedlist */
    if (netconn_list == nc) {
        netconn_list = netconn_list->next;      /* Remove first from linked list */
    } else if (netconn_list != NULL) {
        gsm_netconn_p tmp, prev;
        /* Find element on the list */
        for (prev = netconn_list, tmp = netconn_list->next;
            tmp != NULL; prev = tmp, tmp = tmp->next) {
            if (nc == tmp) {
                prev->next = tmp->next;         /* Remove tmp from linked list */
                break;
            }
        }
    }
    GSM_CORE_UNPROTECT();

    gsm_mem_free(nc);                           /* Free the memory */
    nc = NULL;
    return gsmOK;
}

/**
 * \brief           Connect to server as client
 * \param[in]       nc: Netconn handle
 * \param[in]       host: Pointer to host, such as domain name or IP address in string format
 * \param[in]       port: Target port to use
 * \return          gsmOK if successfully connected, member of \ref gsmr_t otherwise
 */
gsmr_t
gsm_netconn_connect(gsm_netconn_p nc, const char* host, gsm_port_t port) {
    gsmr_t res;

    GSM_ASSERT("nc != NULL", nc != NULL);       /* Assert input parameters */
    GSM_ASSERT("host != NULL", host != NULL);   /* Assert input parameters */
    GSM_ASSERT("port > 0", port);               /* Assert input parameters */

    /*
     * Start a new connection as client and:
     *
     *  - Set current netconn structure as argument
     *  - Set netconn callback function for connection management
     *  - Start connection in blocking mode
     */
    res = gsm_conn_start(NULL, (gsm_conn_type_t)nc->type, host, port, nc, netconn_evt, 1);
    return res;
}

/**
 * \brief           Write data to connection output buffers
 * \note            This function may only be used on TCP or SSL connections
 * \param[in]       nc: Netconn handle used to write data to
 * \param[in]       data: Pointer to data to write
 * \param[in]       btw: Number of bytes to write
 * \return          \ref gsmOK on success, member of \ref gsmr_t enumeration otherwise
 */
gsmr_t
gsm_netconn_write(gsm_netconn_p nc, const void* data, size_t btw) {
    size_t len, sent;
    const uint8_t* d = data;
    gsmr_t res;

    GSM_ASSERT("nc != NULL", nc != NULL);       /* Assert input parameters */
    GSM_ASSERT("nc->type must be TCP\r\n", nc->type == GSM_NETCONN_TYPE_TCP);   /* Assert input parameters */

    /*
     * Several steps are done in write process
     *
     * 1. Check if buffer is set and check if there is something to write to it.
     *    1. In case buffer will be full after copy, send it and free memory.
     * 2. Check how many bytes we can write directly without needed to copy
     * 3. Try to allocate a new buffer and copy remaining input data to it
     * 4. In case buffer allocation fails, send data directly (may affect on speed and effectivenes)
     */

    /* Step 1 */
    if (nc->buff.buff != NULL) {                /* Is there a write buffer ready to accept more data? */
        len = GSM_MIN(nc->buff.len - nc->buff.ptr, btw);    /* Get number of bytes we can write to buffer */
        if (len) {
            GSM_MEMCPY(&nc->buff.buff[nc->buff.ptr], data, len);/* Copy memory to temporary write buffer */
            d += len;
            nc->buff.ptr += len;
            btw -= len;
        }

        /* Step 1.1 */
        if (nc->buff.ptr == nc->buff.len) {
            res = gsm_conn_send(nc->conn, nc->buff.buff, nc->buff.len, &sent, 1);

            gsm_mem_free(nc->buff.buff);        /* Free memory */
            nc->buff.buff = NULL;               /* Invalidate buffer */
            if (res != gsmOK) {
                return res;
            }
        } else {
            return gsmOK;                       /* Buffer is not yet full yet */
        }
    }

    /* Step 2 */
    if (btw >= GSM_CFG_CONN_MAX_DATA_LEN) {
        size_t rem;
        rem = btw % GSM_CFG_CONN_MAX_DATA_LEN;  /* Get remaining bytes for max data length */
        res = gsm_conn_send(nc->conn, d, btw - rem, &sent, 1);  /* Write data directly */
        if (res != gsmOK) {
            return res;
        }
        d += sent;                              /* Advance in data pointer */
        btw -= sent;                            /* Decrease remaining data to send */
    }

    if (!btw) {                                 /* Sent everything? */
        return gsmOK;
    }

    /* Step 3 */
    if (nc->buff.buff == NULL) {                /* Check if we should allocate a new buffer */
        nc->buff.buff = gsm_mem_alloc(sizeof(*nc->buff.buff) * GSM_CFG_CONN_MAX_DATA_LEN);
        nc->buff.len = GSM_CFG_CONN_MAX_DATA_LEN;   /* Save buffer length */
        nc->buff.ptr = 0;                       /* Save buffer pointer */
    }

    /* Step 4 */
    if (nc->buff.buff != NULL) {                /* Memory available? */
        GSM_MEMCPY(&nc->buff.buff[nc->buff.ptr], d, btw);   /* Copy data to buffer */
        nc->buff.ptr += btw;
    } else {                                    /* Still no memory available? */
        return gsm_conn_send(nc->conn, data, btw, NULL, 1); /* Simply send directly blocking */
    }
    return gsmOK;
}

/**
 * \brief           Flush buffered data on netconn `TCP/SSL` connection
 * \note            This function may only be used on `TCP/SSL` connection
 * \param[in]       nc: Netconn handle to flush data
 * \return          \ref gsmOK on success, member of \ref gsmr_t enumeration otherwise
 */
gsmr_t
gsm_netconn_flush(gsm_netconn_p nc) {
    GSM_ASSERT("nc != NULL", nc != NULL);       /* Assert input parameters */
    GSM_ASSERT("nc->type must be TCP\r\n", nc->type == GSM_NETCONN_TYPE_TCP);   /* Assert input parameters */

    /*
     * In case we have data in write buffer,
     * flush them out to network
     */
    if (nc->buff.buff != NULL) {                /* Check remaining data */
        if (nc->buff.ptr > 0) {                 /* Do we have data in current buffer? */
            gsm_conn_send(nc->conn, nc->buff.buff, nc->buff.ptr, NULL, 1);  /* Send data */
        }
        gsm_mem_free(nc->buff.buff);            /* Free memory */
        nc->buff.buff = NULL;                   /* Invalid memory */
    }
    return gsmOK;
}

/**
 * \brief           Send packet to specific IP and port
 * \param[in]       nc: Netconn handle used to send
 * \param[in]       data: Pointer to data to write
 * \param[in]       btw: Number of bytes to write
 * \return          \ref gsmOK on success, member of \ref gsmr_t enumeration otherwise
 */
gsmr_t
gsm_netconn_send(gsm_netconn_p nc, const void* data, size_t btw) {
    GSM_ASSERT("nc != NULL", nc != NULL);       /* Assert input parameters */
    GSM_ASSERT("nc->type must be UDP\r\n", nc->type == GSM_NETCONN_TYPE_UDP);   /* Assert input parameters */

    return gsm_conn_send(nc->conn, data, btw, NULL, 1);
}

/**
 * \brief           Send packet to specific IP and port
 * \note            Use this function in case of UDP type netconn
 * \param[in]       nc: Netconn handle used to send
 * \param[in]       ip: Pointer to IP address
 * \param[in]       port: Port number used to send data
 * \param[in]       data: Pointer to data to write
 * \param[in]       btw: Number of bytes to write
 * \return          \ref gsmOK on success, member of \ref gsmr_t enumeration otherwise
 */
gsmr_t
gsm_netconn_sendto(gsm_netconn_p nc, const gsm_ip_t* ip, gsm_port_t port, const void* data, size_t btw) {
    GSM_ASSERT("nc != NULL", nc != NULL);       /* Assert input parameters */
    GSM_ASSERT("nc->type must be UDP\r\n", nc->type == GSM_NETCONN_TYPE_UDP);   /* Assert input parameters */

    return gsm_conn_sendto(nc->conn, ip, port, data, btw, NULL, 1);
}

/**
 * \brief           Receive data from connection
 * \param[in]       nc: Netconn handle used to receive from
 * \param[in]       pbuf: Pointer to pointer to save new receive buffer to.
 *                     When function returns, user must check for valid pbuf value `pbuf != NULL`
 * \return          \ref gsmOK when new data ready, \ref gsmCLOSED when connection closed by remote side,
 *                  \ref gsmTIMEOUT when receive timeout occurs or any other member of \ref gsmr_t otherwise
 */
gsmr_t
gsm_netconn_receive(gsm_netconn_p nc, gsm_pbuf_p* pbuf) {
    GSM_ASSERT("nc != NULL", nc != NULL);       /* Assert input parameters */
    GSM_ASSERT("pbuf != NULL", pbuf != NULL);   /* Assert input parameters */

    *pbuf = NULL;
#if GSM_CFG_NETCONN_RECEIVE_TIMEOUT
    /*
     * Wait for new received data for up to specific timeout
     * or throw error for timeout notification
     */
    if (gsm_sys_mbox_get(&nc->mbox_receive, (void **)pbuf, nc->rcv_timeout) == GSM_SYS_TIMEOUT) {
        return gsmTIMEOUT;
    }
#else /* GSM_CFG_NETCONN_RECEIVE_TIMEOUT */
    /* Forever wait for new receive packet */
    gsm_sys_mbox_get(&nc->mbox_receive, (void **)pbuf, 0);
#endif /* !GSM_CFG_NETCONN_RECEIVE_TIMEOUT */

    /* Check if connection closed */
    if ((uint8_t *)(*pbuf) == (uint8_t *)&recv_closed) {
        *pbuf = NULL;                           /* Reset pbuf */
        return gsmCLOSED;
    }
    return gsmOK;                               /* We have data available */
}

/**
 * \brief           Close a netconn connection
 * \param[in]       nc: Netconn handle to close
 * \return          \ref gsmOK on success, member of \ref gsmr_t enumeration otherwise
 */
gsmr_t
gsm_netconn_close(gsm_netconn_p nc) {
    gsm_conn_p conn;

    GSM_ASSERT("nc != NULL", nc != NULL);       /* Assert input parameters */
    conn = nc->conn;
    nc->conn = NULL;
    GSM_ASSERT("conn != NULL", conn != NULL);   /* Assert input parameters */

    gsm_netconn_flush(nc);                      /* Flush data and ignore result */
    gsm_conn_set_arg(conn, NULL);               /* Reset argument */
    gsm_conn_close(conn, 1);                    /* Close the connection */
    flush_mboxes(nc, 1);                        /* Flush message queues */
    return gsmOK;
}

/**
 * \brief           Get connection number used for netconn
 * \param[in]       nc: Netconn handle
 * \return          `-1` on failure, connection number otherwise
 */
int8_t
gsm_netconn_getconnnum(gsm_netconn_p nc) {
    if (nc != NULL && nc->conn != NULL) {
        return gsm_conn_getnum(nc->conn);
    }
    return -1;
}

#if GSM_CFG_NETCONN_RECEIVE_TIMEOUT || __DOXYGEN__

/**
 * \brief           Set timeout value for receiving data.
 *
 *                  When enabled, \ref gsm_netconn_receive will only block for up to
 *                  \arg timeout value and will return if no new data within this time
 * \param[in]       nc: Netconn handle
 * \param[in]       timeout: Timeout in units of milliseconds.
 *                  Set to `0` to disable timeout for \ref gsm_netconn_receive function
 */
void
gsm_netconn_set_receive_timeout(gsm_netconn_p nc, uint32_t timeout) {
    nc->rcv_timeout = timeout;
}

/**
 * \brief           Get netconn receive timeout value
 * \param[in]       nc: Netconn handle
 * \return          Timeout in units of milliseconds. If value is `0`, timeout is disabled (wait forever)
 */
uint32_t
gsm_netconn_get_receive_timeout(gsm_netconn_p nc) {
    return nc->rcv_timeout;                     /* Return receive timeout */
}

#endif /* GSM_CFG_NETCONN_RECEIVE_TIMEOUT || __DOXYGEN__ */

#endif /* GSM_CFG_NETCONN || __DOXYGEN__ */
