/*
 *  Arcade Joystick Driver for RaspberryPi
 *
 *  Copyright (c) 2014 Matthieu Proucelle
 *
 *  Based on the gamecon driver by Vojtech Pavlik, and Markus Hiienkari
 */


/*
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 * 
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 * 
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston,
 * MA 02110-1301, USA.
 */

#define pr_fmt(fmt) KBUILD_MODNAME ": " fmt

#include <linux/fs.h>
#include <linux/kernel.h>
#include <linux/delay.h>
#include <linux/module.h>
#include <linux/init.h>
#include <linux/input.h>
#include <linux/mutex.h>
#include <linux/slab.h>
#include <linux/version.h>
#include <linux/ioport.h>
#include <asm/io.h>


MODULE_AUTHOR("Matthieu Proucelle");
MODULE_DESCRIPTION("GPIO and MCP23017 Arcade Joystick Driver");
MODULE_LICENSE("GPL");

#if LINUX_VERSION_CODE >= KERNEL_VERSION(4,15,0)
#define HAVE_TIMER_SETUP
#endif

#define MK_MAX_DEVICES		2
#define MK_MAX_BUTTONS      13

//define for RPI4
#define GPPUPPDN0                57        // Pin pull-up/down for pins 15:0  
#define GPPUPPDN1                58        // Pin pull-up/down for pins 31:16 
#define GPPUPPDN2                59        // Pin pull-up/down for pins 47:32 
#define GPPUPPDN3                60        // Pin pull-up/down for pins 57:48 
//define for RPI0-1-2-3 
#define GPPUDCLK0                38
#define GPPUD                    37

#define GPIO_BASE_OFFSET  0x00200000 /* GPIO controller */

#define INP_GPIO(g) *(gpio+((g)/10)) &= ~(7<<(((g)%10)*3))
#define OUT_GPIO(g) *(gpio+((g)/10)) |=  (1<<(((g)%10)*3))
#define GPIO_READ(g)  *(gpio + 13) &= (1<<(g))

#define SET_GPIO_ALT(g,a) *(gpio+(((g)/10))) |= (((a)<=3?(a)+4:(a)==4?3:2)<<(((g)%10)*3))

#define GPIO_SET *(gpio+7)
#define GPIO_CLR *(gpio+10)

static volatile unsigned *gpio;
int is_2711;


struct mk_config {
    int args[MK_MAX_DEVICES];
    unsigned int nargs;
};

static struct mk_config mk_cfg __initdata;

module_param_array_named(map, mk_cfg.args, int, &(mk_cfg.nargs), 0);
MODULE_PARM_DESC(map, "Enable or disable GPIO and Custom Arcade Joystick");

struct gpio_config {
    int mk_arcade_gpio_maps_custom[MK_MAX_BUTTONS];
    unsigned int nargs;
};

// for player 1 
static struct gpio_config gpio_cfg __initdata;

module_param_array_named(gpio, gpio_cfg.mk_arcade_gpio_maps_custom, int, &(gpio_cfg.nargs), 0);
MODULE_PARM_DESC(gpio, "Numbers of custom GPIO for Arcade Joystick 1");

// for player 2
static struct gpio_config gpio_cfg2 __initdata;

module_param_array_named(gpio2, gpio_cfg2.mk_arcade_gpio_maps_custom, int, &(gpio_cfg2.nargs), 0);
MODULE_PARM_DESC(gpio2, "Numbers of custom GPIO for Arcade Joystick 2");


enum mk_type {
    MK_NONE = 0,
    MK_ARCADE_GPIO_P1,
    MK_ARCADE_GPIO_P2,
    MK_ARCADE_GPIO_CUSTOM1,
    MK_ARCADE_GPIO_CUSTOM2,
    MK_MAX
};


#define MK_REFRESH_TIME	HZ/100


struct mk_pad {
    struct input_dev *dev;
    enum mk_type type;
    char phys[32];
    int gpio_maps[MK_MAX_BUTTONS];
};

struct mk_nin_gpio {
    unsigned pad_id;
    unsigned cmd_setinputs;
    unsigned cmd_setoutputs;
    unsigned valid_bits;
    unsigned request;
    unsigned request_len;
    unsigned response_len;
    unsigned response_bufsize;
};

struct mk {
    struct mk_pad pads[MK_MAX_DEVICES];
    struct timer_list timer;
    int used;
    struct mutex mutex;
    int total_pads;
};

struct mk_subdev {
    unsigned int idx;
};

static struct mk *mk_base;

//Player1

// Map of the gpios :                     up, down, left, right, start, select, a,  b,  tr, y,  x,  tl, hk
static const int mk_arcade_gpio_maps_p1[] = {4,  17,    27,  22,    10,    9,      25, 24, 23, 18, 15, 14, 2 };
                                  
//Player 2
// 2nd joystick on the b+ GPIOS                 up, down, left, right, start, select, a,  b,  tr, y,  x,  tl, hk
static const int mk_arcade_gpio_maps_p2[] = {11, 5,    6,    13,    19,    26,     21, 20, 16, 12, 7,  8,  3 };

static const short mk_arcade_gpio_btn[] = {
    BTN_START, BTN_SELECT, BTN_A, BTN_B, BTN_TR, BTN_Y, BTN_X, BTN_TL, BTN_MODE
};

static const char *mk_names[] = {
    NULL, "GPIO Controller 1", "GPIO Controller 2",  "GPIO Controller 1" , "GPIO Controller 2"
};


uint32_t get_hwbase(void)
{
    char *board;
    uint32_t reg, ret;    
 
    /* read the system register */
#ifdef __aarch64__
    asm volatile ("mrs %0, midr_el1" : "=r" (reg));
#else
    asm volatile ("mrc p15,0,%0,c0,c0,0" : "=r" (reg));
#endif
 
    /* get the PartNum, detect board and MMIO base address */
    switch ((reg >> 4) & 0xFFF) {
        case 0xB76: board = "Rpi0/1"; ret = 0x20000000; break;
        case 0xC07: board = "Rpi2"  ; ret = 0x3F000000; break;
        case 0xD03: board = "Rpi3"  ; ret = 0x3F000000; break;
        case 0xD08: board = "Rpi4"  ; ret = 0xFE000000; break;
        default:    ret = 0x00000000; break;
    }
    if (ret>0){
        pr_err("Found %s with memory base at 0x%08x\n", board,ret);
    }
    else
    {
        pr_err("Unable to detect Memory base address\n");
    }
    return ret;


}




static void setGpioPullUp(int gpioPin) {
    
    if (is_2711){  //pullups changed on RPI4
    int pullreg = GPPUPPDN0 + (gpioPin>>4);
    int pullshift = (gpioPin & 0xf) << 1;
    unsigned int pullbits;
    unsigned int pull = 1; //PULLUP
    pullbits = *(gpio + pullreg);
    pullbits &= ~(3 << pullshift);
    pullbits |= (pull << pullshift);
    *(gpio + pullreg) = pullbits;
    }
    else  //previous RPI
    {
       int clkreg = GPPUDCLK0 + (gpioPin>>5);
      int clkbit = 1 << (gpioPin & 0x1f);

    *(gpio + GPPUD) = 0x02;
    udelay(10);
    *(gpio + clkreg) = clkbit;
    udelay(10);
    *(gpio + GPPUD) = 0;
    udelay(10);
    *(gpio + clkreg) = 0;
    udelay(10);
    } 
}


static void setGpioAsInput(int gpioNum) {
    INP_GPIO(gpioNum);
}



static void mk_gpio_read_packet(struct mk_pad * pad, unsigned char *data) {
    int i;

    for (i = 0; i < MK_MAX_BUTTONS; i++) {
        if(pad->gpio_maps[i] != -1){    // to avoid unused buttons
            int read = GPIO_READ(pad->gpio_maps[i]);
            if (read == 0) data[i] = 1;
            else data[i] = 0;
        }else data[i] = 0;
    }

}

static void mk_input_report(struct mk_pad * pad, unsigned char * data) {
    struct input_dev * dev = pad->dev;
    int j;
    input_report_abs(dev, ABS_Y, !data[0]-!data[1]);
    input_report_abs(dev, ABS_X, !data[2]-!data[3]);
    for (j = 4; j < MK_MAX_BUTTONS; j++) {
        input_report_key(dev, mk_arcade_gpio_btn[j - 4], data[j]);
    }
    input_sync(dev);
}

static void mk_process_packet(struct mk *mk) {

    unsigned char data[MK_MAX_BUTTONS];
    struct mk_pad *pad;
    int i;

    for (i = 0; i < mk->total_pads; i++) {
        pad = &mk->pads[i];
        mk_gpio_read_packet(pad, data);
        mk_input_report(pad, data);
    }

}

/*
 * mk_timer() initiates reads of console pads data.
 */
#ifdef HAVE_TIMER_SETUP
static void mk_timer(struct timer_list *t) {
    struct mk *mk = from_timer(mk, t, timer);
#else
static void mk_timer(unsigned long private) {
    struct mk *mk = (void *) private;
#endif
    mk_process_packet(mk);
    mod_timer(&mk->timer, jiffies + MK_REFRESH_TIME);
}

static int mk_open(struct input_dev *dev) {
    struct mk *mk = input_get_drvdata(dev);
    int err;

    err = mutex_lock_interruptible(&mk->mutex);
    if (err)
        return err;

    if (!mk->used++)
        mod_timer(&mk->timer, jiffies + MK_REFRESH_TIME);

    mutex_unlock(&mk->mutex);
    return 0;
}

static void mk_close(struct input_dev *dev) {
    struct mk *mk = input_get_drvdata(dev);

    mutex_lock(&mk->mutex);
    if (!--mk->used) {
        del_timer_sync(&mk->timer);
    }
    mutex_unlock(&mk->mutex);
}

static int __init mk_setup_pad(struct mk *mk, int idx, int pad_type_arg) {
    struct mk_pad *pad = &mk->pads[idx];
    struct input_dev *input_dev;
    int i, pad_type;
    int err;

    pad_type = pad_type_arg;

    if (pad_type < 1 || pad_type >= MK_MAX) {
        pr_err("Pad type %d unknown\n", pad_type);
        return -EINVAL;
    }

    if (pad_type == MK_ARCADE_GPIO_CUSTOM1) {

        // if the device is custom, be sure to get correct pins
        if (gpio_cfg.nargs < 1) {
            pr_err("Custom device needs gpio argument\n");
            return -EINVAL;
        } else if(gpio_cfg.nargs != MK_MAX_BUTTONS){
             pr_err("Invalid gpio argument\n");
             return -EINVAL;
        }
    
    }

    if (pad_type == MK_ARCADE_GPIO_CUSTOM2) {

        // if the device is custom, be sure to get correct pins
        if (gpio_cfg2.nargs < 1) {
            pr_err("Custom device needs gpio argument\n");
            return -EINVAL;
        } else if(gpio_cfg2.nargs != MK_MAX_BUTTONS){
             pr_err("Invalid gpio argument\n");
             return -EINVAL;
        }
    
    }

    pr_info("pad type : %d\n",pad_type);
    pad->dev = input_dev = input_allocate_device();
    if (!input_dev) {
        pr_err("Not enough memory for input device\n");
        return -ENOMEM;
    }

    pad->type = pad_type;
    snprintf(pad->phys, sizeof (pad->phys),
            "input%d", idx);

    input_dev->name = mk_names[pad_type];
    input_dev->phys = pad->phys;
    input_dev->id.bustype = BUS_PARPORT;
    input_dev->id.vendor = 0x0001;
    input_dev->id.product = pad_type;
    input_dev->id.version = 0x0100;

    input_set_drvdata(input_dev, mk);

    input_dev->open = mk_open;
    input_dev->close = mk_close;

    input_dev->evbit[0] = BIT_MASK(EV_KEY) | BIT_MASK(EV_ABS);

    for (i = 0; i < 2; i++)
        input_set_abs_params(input_dev, ABS_X + i, -1, 1, 0, 0);
    for (i = 0; i < MK_MAX_BUTTONS - 4; i++)
        __set_bit(mk_arcade_gpio_btn[i], input_dev->keybit);

    mk->total_pads++;

    // asign gpio pins
    switch (pad_type) {
        case MK_ARCADE_GPIO_P1:
            memcpy(pad->gpio_maps, mk_arcade_gpio_maps_p1, MK_MAX_BUTTONS *sizeof(int));
            break;
        case MK_ARCADE_GPIO_P2:
            memcpy(pad->gpio_maps, mk_arcade_gpio_maps_p2, MK_MAX_BUTTONS *sizeof(int));
            break;
        case MK_ARCADE_GPIO_CUSTOM1:
            memcpy(pad->gpio_maps, gpio_cfg.mk_arcade_gpio_maps_custom, MK_MAX_BUTTONS *sizeof(int));
            break;
        case MK_ARCADE_GPIO_CUSTOM2:
            memcpy(pad->gpio_maps, gpio_cfg2.mk_arcade_gpio_maps_custom, MK_MAX_BUTTONS *sizeof(int));
            break;
    }

    // initialize gpio
    for (i = 0; i < MK_MAX_BUTTONS; i++) {
        if(pad->gpio_maps[i] != -1){    // to avoid unused buttons
            setGpioAsInput(pad->gpio_maps[i]);
            setGpioPullUp(pad->gpio_maps[i]);
        }
    }                
        pr_err("GPIO configured for pad%d\n", idx);

    err = input_register_device(pad->dev);
    if (err)
        goto err_free_dev;

    return 0;

err_free_dev:
    input_free_device(pad->dev);
    pad->dev = NULL;
    return err;
}
    
    
    
static struct mk __init *mk_probe(int *pads, int n_pads) {
    struct mk *mk;
    int i;
    int count = 0;
    int err;

    mk = kzalloc(sizeof (struct mk), GFP_KERNEL);
    if (!mk) {
        pr_err("Not enough memory\n");
        err = -ENOMEM;
        goto err_out;
    }

    mutex_init(&mk->mutex);
#ifdef HAVE_TIMER_SETUP
    timer_setup(&mk->timer, mk_timer, 0);
#else
    setup_timer(&mk->timer, mk_timer, (long) mk);
#endif

    for (i = 0; i < n_pads && i < MK_MAX_DEVICES; i++) {
        if (!pads[i])
            continue;

        err = mk_setup_pad(mk, i, pads[i]);
        if (err)
            goto err_unreg_devs;

        count++;
    }

    if (count == 0) {
        pr_err("No valid devices specified\n");
        err = -EINVAL;
        goto err_free_mk;
    }

    return mk;

err_unreg_devs:
    while (--i >= 0)
        if (mk->pads[i].dev)
            input_unregister_device(mk->pads[i].dev);
err_free_mk:
    kfree(mk);
err_out:
    return ERR_PTR(err);
}

static void mk_remove(struct mk *mk) {
    int i;

    for (i = 0; i < MK_MAX_DEVICES; i++)
        if (mk->pads[i].dev)
            input_unregister_device(mk->pads[i].dev);
    kfree(mk);
}

static int __init mk_init(void) {
    /* Set up gpio pointer for direct register access */
    uint32_t hwbase;
    hwbase = get_hwbase();
    if ((gpio = ioremap(GPIO_BASE_OFFSET+hwbase, 0xB0)) == NULL) {
        pr_err("io remap failed\n");
        return -EBUSY;
    }
    is_2711 = (*(gpio+GPPUPPDN3) != 0x6770696f);
    if (mk_cfg.nargs < 1) {
        pr_err("at least one device must be specified\n");
        return -EINVAL;
    } else {
        mk_base = mk_probe(mk_cfg.args, mk_cfg.nargs);
        if (IS_ERR(mk_base))
            return -ENODEV;
    }
    return 0;
}

static void __exit mk_exit(void) {
    if (mk_base)
        mk_remove(mk_base);

    iounmap(gpio);
}

module_init(mk_init);
module_exit(mk_exit);
