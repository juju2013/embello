/*
 * This file is part of the libopencm3 project.
 *
 * Copyright (C) 2009 Uwe Hermann <uwe@hermann-uwe.de>,
 * Copyright (C) 2010 Gareth McMullin <gareth@blacksphere.co.nz>
 * Copyright (C) 2011 Piotr Esden-Tempski <piotr@esden.net>
 * Copyright (C) 2016 Jean-Claude Wippler <jc@wippler.nl>
 *
 * This library is free software: you can redistribute it and/or modify
 * it under the terms of the GNU Lesser General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public License
 * along with this library.  If not, see <http://www.gnu.org/licenses/>.
 */

#include <stdlib.h>
#include <libopencm3/stm32/rcc.h>
#include <libopencm3/stm32/gpio.h>
#include <libopencm3/stm32/usart.h>
#include <libopencm3/cm3/nvic.h>
#include <libopencm3/usb/usbd.h>
#include <libopencm3/usb/cdc.h>

/******************************************************************************
 * Simple ringbuffer implementation from open-bldc's libgovernor that
 * you can find at:
 * https://github.com/open-bldc/open-bldc/tree/master/source/libgovernor
 *****************************************************************************/

typedef int32_t ring_size_t;

struct ring {
    uint8_t *data;
    ring_size_t size;
    uint32_t begin;
    uint32_t end;
};

#define RING_SIZE(RING)  ((RING)->size - 1)
#define RING_DATA(RING)  (RING)->data
#define RING_EMPTY(RING) ((RING)->begin == (RING)->end)

static void ring_init(struct ring *ring, uint8_t *buf, ring_size_t size)
{
    ring->data = buf;
    ring->size = size;
    ring->begin = 0;
    ring->end = 0;
}

static int32_t ring_write_ch(struct ring *ring, uint8_t ch)
{
    if (((ring->end + 1) % ring->size) != ring->begin) {
        ring->data[ring->end++] = ch;
        ring->end %= ring->size;
        return (uint32_t)ch;
    }

    return -1;
}

static int32_t ring_write(struct ring *ring, uint8_t *data, ring_size_t size)
{
    int32_t i;

    for (i = 0; i < size; i++) {
        if (ring_write_ch(ring, data[i]) < 0)
            return -i;
    }

    return i;
}

static int32_t ring_read_ch(struct ring *ring, uint8_t *ch)
{
    int32_t ret = -1;

    if (ring->begin != ring->end) {
        ret = ring->data[ring->begin++];
        ring->begin %= ring->size;
        if (ch)
            *ch = ret;
    }

    return ret;
}

static int32_t ring_read(struct ring *ring, uint8_t *data, ring_size_t size)
{
    int32_t i;

    for (i = 0; i < size; i++) {
        if (ring_read_ch(ring, data + i) < 0)
            return i;
    }

    return -i;
}

/*****************************************************************************/

#define BUFFER_SIZE 1024

struct ring input_ring, output_ring;
uint8_t input_ring_buffer[BUFFER_SIZE], output_ring_buffer[BUFFER_SIZE];

static void clock_setup(void)
{
    //rcc_clock_setup_in_hsi_out_48mhz();
    rcc_clock_setup_in_hse_8mhz_out_72mhz();

    rcc_periph_clock_enable(RCC_GPIOA);
    rcc_periph_clock_enable(RCC_GPIOC);
    rcc_periph_clock_enable(RCC_AFIO);
    rcc_periph_clock_enable(RCC_USART1);
}

static void gpio_setup(void)
{
    gpio_set(GPIOC, GPIO12);

    /* Setup GPIO6 and 7 (in GPIO port A) for LED use. */
    gpio_set_mode(GPIOC, GPIO_MODE_OUTPUT_50_MHZ,
            GPIO_CNF_OUTPUT_PUSHPULL, GPIO12);

    gpio_set(GPIOA, GPIO0); // PA0 low is USB enable for HyTiny
    gpio_set_mode(GPIOA, GPIO_MODE_OUTPUT_2_MHZ,
            GPIO_CNF_OUTPUT_PUSHPULL, GPIO0);
}

static void usart_setup(void)
{
    /* Initialize input and output ring buffers. */
    ring_init(&input_ring, input_ring_buffer, BUFFER_SIZE);
    ring_init(&output_ring, output_ring_buffer, BUFFER_SIZE);

    /* Enable the USART1 interrupt. */
    nvic_enable_irq(NVIC_USART1_IRQ);

    /* Setup GPIO pin GPIO_USART1_RE_TX on GPIO port A for transmit. */
    gpio_set_mode(GPIOA, GPIO_MODE_OUTPUT_50_MHZ,
            GPIO_CNF_OUTPUT_ALTFN_PUSHPULL, GPIO_USART1_TX);

    /* Setup GPIO pin GPIO_USART1_RE_RX on GPIO port A for receive. */
    gpio_set_mode(GPIOA, GPIO_MODE_INPUT,
            GPIO_CNF_INPUT_FLOAT, GPIO_USART1_RX);

    /* Setup UART parameters. */
    usart_set_baudrate(USART1, 115200);
    usart_set_databits(USART1, 8);
    usart_set_stopbits(USART1, USART_STOPBITS_1);
    usart_set_parity(USART1, USART_PARITY_NONE);
    usart_set_flow_control(USART1, USART_FLOWCONTROL_NONE);
    usart_set_mode(USART1, USART_MODE_TX_RX);

    /* Enable USART1 Receive interrupt. */
    USART_CR1(USART1) |= USART_CR1_RXNEIE;

    /* Finally enable the USART. */
    usart_enable(USART1);
}

/* telnet escape codes and special values: */
enum {
    IAC=255, WILL=251, SB=250, SE=240,
    CPO=44, SETPAR=3, SETCTL=5,
    PAR_NONE=1, PAR_ODD=2, PAR_EVEN=3,
    DTR_ON=8, DTR_OFF=9, RTS_ON=11, RTS_OFF=12,
};

void usart1_isr(void)
{
    /* Check if we were called because of RXNE. */
    if (((USART_CR1(USART1) & USART_CR1_RXNEIE) != 0) &&
            ((USART_SR(USART1) & USART_SR_RXNE) != 0)) {

        /* Indicate that we got data. */
        gpio_toggle(GPIOC, GPIO12);

        /* Retrieve the data from the peripheral. */
        uint8_t c = usart_recv(USART1);
        ring_write_ch(&input_ring, c);

        /* telnet: escape the escape character, i.e. send it twice */
        if (c == IAC)
            ring_write_ch(&input_ring, c);
    }

    /* Check if we were called because of TXE. */
    if (((USART_CR1(USART1) & USART_CR1_TXEIE) != 0) &&
            ((USART_SR(USART1) & USART_SR_TXE) != 0)) {

        int32_t data;

        data = ring_read_ch(&output_ring, NULL);

        if (data == -1) {
            /* Disable the TXE interrupt, it's no longer needed. */
            USART_CR1(USART1) &= ~USART_CR1_TXEIE;
        } else {
            /* state machine to decode telnet request before sending them on */
            static int state = 0;

            switch (state) {
                default: // default state
                    if (data == IAC)
                        state = 1;
                    else
                        usart_send(USART1, data);
                    break;

                case 1: // IAC seen
                    state = 0;
                    if (data == IAC)
                        usart_send(USART1, data);
                    else
                        state = data == SB ? 3 : 2;
                    break;

                case 2: // IAC, WILL (or other) seen
                    state = 0;
                    break;

                case 3: // IAC, SB seen
                    state = data == CPO ? 4 : 5;
                    break;

                case 4: // IAC, SB, CPO seen
                    state = data == SETPAR ? 7 :
                            data == SETCTL ? 8 : 5;
                    break;

                case 5: // wait for IAC + SE
                    if (data == IAC)
                        state = 6;
                    break;

                case 6: // wait for SE
                    if (data != IAC)
                        state = data == SE ? 0 : 5;
                    break;

                case 7: // set parity
                    state = 5;
                    if (data == PAR_NONE)
                        usart_set_parity(USART1, USART_PARITY_NONE);
                    else if (data == PAR_ODD)
                        usart_set_parity(USART1, USART_PARITY_ODD);
                    else if (data == PAR_EVEN)
                        usart_set_parity(USART1, USART_PARITY_EVEN);
                    break;

                case 8: // set control
                    state = 5;
                    switch (data) {
                        case DTR_ON:
                            usart_send(USART1, 'D'); break;
                        case DTR_OFF:
                            usart_send(USART1, 'd'); break;
                        case RTS_ON:
                            usart_send(USART1, 'R'); break;
                        case RTS_OFF:
                            usart_send(USART1, 'r'); break;
                        default: break;
                    }
                    break;
            }
        }
    }
}

/*****************************************************************************/

static const struct usb_device_descriptor dev = {
    .bLength = USB_DT_DEVICE_SIZE,
    .bDescriptorType = USB_DT_DEVICE,
    .bcdUSB = 0x0200,
    .bDeviceClass = USB_CLASS_CDC,
    .bDeviceSubClass = 0,
    .bDeviceProtocol = 0,
    .bMaxPacketSize0 = 64,
    .idVendor = 0x0483,
    .idProduct = 0x5740,
    .bcdDevice = 0x0200,
    .iManufacturer = 1,
    .iProduct = 2,
    .iSerialNumber = 3,
    .bNumConfigurations = 1,
};

/*
 * This notification endpoint isn't implemented. According to CDC spec its
 * optional, but its absence causes a NULL pointer dereference in Linux
 * cdc_acm driver.
 */
static const struct usb_endpoint_descriptor comm_endp[] = {{
    .bLength = USB_DT_ENDPOINT_SIZE,
    .bDescriptorType = USB_DT_ENDPOINT,
    .bEndpointAddress = 0x83,
    .bmAttributes = USB_ENDPOINT_ATTR_INTERRUPT,
    .wMaxPacketSize = 16,
    .bInterval = 255,
}};

static const struct usb_endpoint_descriptor data_endp[] = {{
    .bLength = USB_DT_ENDPOINT_SIZE,
    .bDescriptorType = USB_DT_ENDPOINT,
    .bEndpointAddress = 0x01,
    .bmAttributes = USB_ENDPOINT_ATTR_BULK,
    .wMaxPacketSize = 64,
    .bInterval = 1,
}, {
    .bLength = USB_DT_ENDPOINT_SIZE,
    .bDescriptorType = USB_DT_ENDPOINT,
    .bEndpointAddress = 0x82,
    .bmAttributes = USB_ENDPOINT_ATTR_BULK,
    .wMaxPacketSize = 64,
    .bInterval = 1,
}};

static const struct {
    struct usb_cdc_header_descriptor header;
    struct usb_cdc_call_management_descriptor call_mgmt;
    struct usb_cdc_acm_descriptor acm;
    struct usb_cdc_union_descriptor cdc_union;
} __attribute__((packed)) cdcacm_functional_descriptors = {
    .header = {
        .bFunctionLength = sizeof(struct usb_cdc_header_descriptor),
        .bDescriptorType = CS_INTERFACE,
        .bDescriptorSubtype = USB_CDC_TYPE_HEADER,
        .bcdCDC = 0x0110,
    },
    .call_mgmt = {
        .bFunctionLength =
            sizeof(struct usb_cdc_call_management_descriptor),
        .bDescriptorType = CS_INTERFACE,
        .bDescriptorSubtype = USB_CDC_TYPE_CALL_MANAGEMENT,
        .bmCapabilities = 0,
        .bDataInterface = 1,
    },
    .acm = {
        .bFunctionLength = sizeof(struct usb_cdc_acm_descriptor),
        .bDescriptorType = CS_INTERFACE,
        .bDescriptorSubtype = USB_CDC_TYPE_ACM,
        .bmCapabilities = 0,
    },
    .cdc_union = {
        .bFunctionLength = sizeof(struct usb_cdc_union_descriptor),
        .bDescriptorType = CS_INTERFACE,
        .bDescriptorSubtype = USB_CDC_TYPE_UNION,
        .bControlInterface = 0,
        .bSubordinateInterface0 = 1,
     },
};

static const struct usb_interface_descriptor comm_iface[] = {{
    .bLength = USB_DT_INTERFACE_SIZE,
    .bDescriptorType = USB_DT_INTERFACE,
    .bInterfaceNumber = 0,
    .bAlternateSetting = 0,
    .bNumEndpoints = 1,
    .bInterfaceClass = USB_CLASS_CDC,
    .bInterfaceSubClass = USB_CDC_SUBCLASS_ACM,
    .bInterfaceProtocol = USB_CDC_PROTOCOL_AT,
    .iInterface = 0,

    .endpoint = comm_endp,

    .extra = &cdcacm_functional_descriptors,
    .extralen = sizeof(cdcacm_functional_descriptors),
}};

static const struct usb_interface_descriptor data_iface[] = {{
    .bLength = USB_DT_INTERFACE_SIZE,
    .bDescriptorType = USB_DT_INTERFACE,
    .bInterfaceNumber = 1,
    .bAlternateSetting = 0,
    .bNumEndpoints = 2,
    .bInterfaceClass = USB_CLASS_DATA,
    .bInterfaceSubClass = 0,
    .bInterfaceProtocol = 0,
    .iInterface = 0,

    .endpoint = data_endp,
}};

static const struct usb_interface ifaces[] = {{
    .num_altsetting = 1,
    .altsetting = comm_iface,
}, {
    .num_altsetting = 1,
    .altsetting = data_iface,
}};

static const struct usb_config_descriptor config = {
    .bLength = USB_DT_CONFIGURATION_SIZE,
    .bDescriptorType = USB_DT_CONFIGURATION,
    .wTotalLength = 0,
    .bNumInterfaces = 2,
    .bConfigurationValue = 1,
    .iConfiguration = 0,
    .bmAttributes = 0x80,
    .bMaxPower = 0x32,

    .interface = ifaces,
};

static const char *usb_strings[] = {
    "Black Sphere Technologies",
    "CDC-ACM Demo",
    "DEMO",
};

/* Buffer to be used for control requests. */
uint8_t usbd_control_buffer[128];

static int cdcacm_control_request(usbd_device *usbd_dev, struct usb_setup_data *req, uint8_t **buf,
        uint16_t *len, void (**complete)(usbd_device *usbd_dev, struct usb_setup_data *req))
{
    (void)complete;
    (void)buf;
    (void)usbd_dev;

    switch (req->bRequest) {
    case USB_CDC_REQ_SET_CONTROL_LINE_STATE: {
        /*
         * This Linux cdc_acm driver requires this to be implemented
         * even though it's optional in the CDC spec, and we don't
         * advertise it in the ACM functional descriptor.
         */
        char local_buf[10];
        struct usb_cdc_notification *notif = (void *)local_buf;

        /* We echo signals back to host as notification. */
        notif->bmRequestType = 0xA1;
        notif->bNotification = USB_CDC_NOTIFY_SERIAL_STATE;
        notif->wValue = 0;
        notif->wIndex = 0;
        notif->wLength = 2;
        local_buf[8] = req->wValue & 3;
        local_buf[9] = 0;
        // usbd_ep_write_packet(0x83, buf, 10);
        return 1;
        }
    case USB_CDC_REQ_SET_LINE_CODING:
        if (*len < sizeof(struct usb_cdc_line_coding))
            return 0;
        return 1;
    }
    return 0;
}

static void cdcacm_data_rx_cb(usbd_device *usbd_dev, uint8_t ep)
{
    (void)ep;
    (void)usbd_dev;

    uint8_t buf[64];
    int len = usbd_ep_read_packet(usbd_dev, 0x01, buf, 64);

    if (len) {
        /* Retrieve the data from the peripheral. */
        ring_write(&output_ring, buf, len);

        /* Enable transmit interrupt so it sends back the data. */
        USART_CR1(USART1) |= USART_CR1_TXEIE;
    }
}

static void cdcacm_set_config(usbd_device *usbd_dev, uint16_t wValue)
{
    (void)wValue;
    (void)usbd_dev;

    usbd_ep_setup(usbd_dev, 0x01, USB_ENDPOINT_ATTR_BULK, 64, cdcacm_data_rx_cb);
    usbd_ep_setup(usbd_dev, 0x82, USB_ENDPOINT_ATTR_BULK, 64, NULL);
    usbd_ep_setup(usbd_dev, 0x83, USB_ENDPOINT_ATTR_INTERRUPT, 16, NULL);

    usbd_register_control_callback(
                usbd_dev,
                USB_REQ_TYPE_CLASS | USB_REQ_TYPE_INTERFACE,
                USB_REQ_TYPE_TYPE | USB_REQ_TYPE_RECIPIENT,
                cdcacm_control_request);
}

int main(void)
{
    clock_setup();
    gpio_setup();
    usart_setup();

    usbd_device *usbd_dev = usbd_init(&st_usbfs_v1_usb_driver, &dev, &config,
            usb_strings, 3, usbd_control_buffer, sizeof(usbd_control_buffer));
    usbd_register_set_config_callback(usbd_dev, cdcacm_set_config);

    for (int i = 0; i < 0x800000; i++)
        __asm__("");
    gpio_clear(GPIOA, GPIO0);

    while (1) {
        usbd_poll(usbd_dev);

        uint8_t buf[64];
        int len = ring_read(&input_ring, buf, sizeof buf);
        if (len > 0) {
            usbd_ep_write_packet(usbd_dev, 0x82, buf, len);
            //buf[len] = 0;
        }
    }
}