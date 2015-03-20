
#include <linux/kernel.h> 
#include <linux/module.h>
#include <linux/param.h>
#include <linux/moduleparam.h>
#include <linux/init.h>
#include <linux/version.h>
#include <linux/types.h>
#include <linux/delay.h>
#include <linux/spinlock.h>
#include <linux/proc_fs.h>
#include <linux/miscdevice.h>
#include <linux/timer.h>

#include <asm/bl2348/base/stt_basic_defs.h>
#include <asm/bl2348/registers/mips.h>
#include <asm/bl2348/drv/BL234x_sw_reset.h>
#include <asm/bl2348/drv/BL234x_gpio.h>
#include <asm/bl2348/drv/BL234x_spi.h>
#include <asm/bl2348/BL2348_board.h>


#include "mcp23s17.h"
#include "mcp23s17_ioctl.h"
#include <asm/bl2348/spi_driver.h>
#include <linux/adt_common.h>

/*************************************************************************/
/*                               MACROS                                  */
/*************************************************************************/

#define MODULE 1
#define DRIVER_NAME "mcp23s17_drv"
#define DRIVER_VERSION "1.0"
#define MAX_ALARM_RECV 10

#define MODEL_ID_ENTRY 		"modelid"
#define REGISTERS_ENTRY 	"registers"
#define IO_EXPANDER_DIR 	"io_expander"
#define LEDS_DIR		"leds"

#define POWER_LED_YELLOW	(HZ/100)
#define POWER_LED_RED_OFF	(HZ/5)
#define POWER_LED_BURN_FLASH	(HZ/10)

/*************************************************************************/
/*                        FORWARD DECLARATION                            */
/*************************************************************************/

static void mcp23_spi_config (void);
static int mcp23_init_dev(int);
static int mcp23s17_read(struct mcp23s17 *mcp, uint8_t reg);
static int mcp23s17_write(struct mcp23s17 *mcp, uint8_t reg, uint8_t val);

static int mcp23_init_proc (void);
static void mcp23_cleanup_proc(int);

/*
  Directory view after driver initialization 
  /proc
       /adt
          /io_expander
              /leds
                0
                1
                15
            registers
       modelid
*/

static struct proc_dir_entry* io_expander_root;

/* I/O Exander leds */
static struct proc_dir_entry* leds_root;
static struct proc_dir_entry* led_entry[MAX_LEDS];
static int procfile_read(char *buf, char **start, off_t off,  int count, int *eof, void *data);
static ssize_t procfile_write(struct file* filp, const char __user *buf, unsigned long len,  void *data);
static inline int leds_proc_create (struct proc_dir_entry* root_entry);
static inline void leds_proc_cleanup (struct proc_dir_entry* root_entry);
/* I/O Exander leds end */

/* IOCTL wrappers */
static int drvIoctl(struct inode *inodeP,struct file *fileP,unsigned int cmd, unsigned long arg);
static int drvOpen(struct inode *inodeP, struct file *fileP);
static int drvRelease(struct inode *inodeP, struct file *fileP);
static int mcpIoctlImpl(mcp_ioctl_param_t* arg);
static int mcpResetLedImpl (int* value);
/* IOCTL wrappers  end*/

/* Model ID*/
static void get_model_id_info(void);
static struct proc_dir_entry* proc_model_id_entry;
static uint8_t model_id=0;
static int model_id_file_read(char *buf, char **start, off_t off,
                         int count, int *eof, void *data);
static inline struct proc_dir_entry* model_id_proc_create (struct proc_dir_entry* root_entry);
static inline void model_id_proc_cleanup (struct proc_dir_entry* root_entry);
/*Model ID end */

/* Registers */
static struct proc_dir_entry* registers;
static int registers_file_read(char *buf, char **start, off_t off,
                         int count, int *eof, void *data);
static inline struct proc_dir_entry* registers_proc_create (struct proc_dir_entry* root_entry);
static inline void registers_proc_cleanup (struct proc_dir_entry* root_entry);
/* Registers end */

/* Timer */
static void led_pattern_clbk (unsigned long);
/* Timer end */



/*************************************************************************/
/*                           GLOBAL VARIABLE                             */
/*************************************************************************/

static mcp23s17_t mcp;

static struct file_operations mcpOps =
{
    owner: THIS_MODULE,
    ioctl: drvIoctl,
    open: drvOpen,
    release: drvRelease,
};

static struct miscdevice mcpMiscDev =
{
    minor: MISC_DYNAMIC_MINOR,
    name: DRIVER_NAME,
    fops: &mcpOps,
};

static int array[MAX_LEDS];

struct sConfigDev
{
    wait_queue_head_t *queue;
};

static struct file *file[MAX_ALARM_RECV];

static struct timer_list led_pattern_timer;

static int power_led_state;
static int delay;

/*************************************************************************/
/*                        IMPLEMENTATION                                 */
/*************************************************************************/

static int drvIoctl(struct inode *inodeP,struct file *fileP,unsigned int cmd, unsigned long arg)
{
    if (_IOC_TYPE(cmd) != MCP_IOW_MAGIC)
    {
        return (-ENOTTY);
    }
    if (_IOC_NR(cmd) > 3 /*MAX number of commands*/)
    {
        return (-ENOTTY);
    }
    
    switch (cmd)
    {
        case MCP_IOCTL_CMD:
        {
            /* management of all calls */
            return mcpIoctlImpl((mcp_ioctl_param_t*)arg);
        }
        case MCP_RESET_LED_PATTERN:
        {
        	return mcpResetLedImpl ((int*) arg);
        }
	case MCP_HW_ID:
		copy_to_user (&model_id, (uint8_t*)arg, sizeof(uint8_t));
		return 0;
        default:
        {
            /* catch all unknown ioctl-commands */
            return (-ENOIOCTLCMD);
        }
    }
    
    return 0;

}


static int drvOpen(struct inode *inodeP, struct file *fileP)
{
    struct sConfigDev *devP;
    int i;

    devP = kmalloc(sizeof(struct sConfigDev), GFP_KERNEL);
    if (devP == NULL)
        return (-ENOMEM);
    memset(devP, 0, sizeof(struct sConfigDev));

    /* initalize private data */
    devP->queue = kmalloc(sizeof(wait_queue_head_t), GFP_KERNEL);
    if (devP->queue == NULL)
    {
        kfree(devP);
        return (-ENOMEM);
    }
    init_waitqueue_head(devP->queue);
    /* alloc memory for the alarm info records */
    fileP->private_data = devP;
    /* register this device (ie. put it into files array). We check this at
     * last, because as soon as we register us we can receive alarms, thus all
     * structures must be allocated and initialized first. */
    for (i = 0; i < MAX_ALARM_RECV; i++)
    {
        if (! file[i])
        {
            file[i] = fileP;
            break;
        }
    }
    if (i == MAX_ALARM_RECV)
    {
        /* Too many open devices. */
        kfree(devP->queue);
        kfree(devP);
        return -1;
    }

    return 0;
	
}

static int drvRelease(struct inode *inodeP, struct file *fileP)
{
    struct sConfigDev *devP = fileP->private_data;
    int i;

    /* cleanup private data */
    for (i = 0; i < MAX_ALARM_RECV; i++)
    {
        if (file[i] == fileP)
        {
            file[i] = NULL;
            break;
        }
    }
    kfree(devP->queue);
    devP = NULL;
    kfree(fileP->private_data);

    return 0;

}

static int mcpIoctlImpl(mcp_ioctl_param_t* arg)
{
	mcp_ioctl_param_t data;
	int result = 0;
	
	copy_from_user(&data, arg, sizeof(mcp_ioctl_param_t));
	
	switch (data.mode)
	{
	case MCP_REG_MODE_WRITE:
		result = mcp23s17_write(&mcp, data.address, data.value);
		break;
	case MCP_REG_MODE_READ:
		result = mcp23s17_read (&mcp, data.address);
		break;
	default:
		return -EFAULT;
	}

	return result;
}

static int mcpResetLedImpl (int* value)
{
	int data;
	int result;
	copy_from_user (&data, value, sizeof(int));
	
	switch (data)
	{
	case MCP_RESET_BUTTON_STOP:
	{
		del_timer_sync(&led_pattern_timer);
		/* stop Power LED blink */
		result = mcp23s17_read(&mcp, MCP_GPIOA);
		/* Power LED = GPA0*/
		
		if (power_led_state == 1)
		{
			result |= 1;
		}
		else
		{
			result &= ~1;
		}
		
		mcp23s17_write(&mcp, MCP_GPIOA, result);
		
		break;
	}
	case MCP_RESET_BUTTON_START:
	{
		/* start Power LED blink */
		power_led_state = mcp23s17_read(&mcp, MCP_GPIOA);
		/* Power LED = GPA0*/
		power_led_state &= 0x01;
		led_pattern_timer.expires = jiffies + delay;
	}
	case MCP_RESET_BUTTON_STATE2:
	case MCP_RESET_BUTTON_STATE4:
	{
		delay = POWER_LED_YELLOW;
		mod_timer(&led_pattern_timer, jiffies + delay);
		break;
	}
		
	case MCP_RESET_BUTTON_STATE1:
	{
		del_timer_sync(&led_pattern_timer);
		result = mcp23s17_read(&mcp, MCP_GPIOA);
		result &= ~1;
		mcp23s17_write(&mcp, MCP_GPIOA, result);
		break;
	}
	case MCP_RESET_BUTTON_STATE3:
	{
		delay = POWER_LED_RED_OFF;
		mod_timer(&led_pattern_timer, jiffies + delay);
		break;
	}

	case MCP_POWER_LED_BLINK:
	{
		power_led_state = mcp23s17_read(&mcp, MCP_GPIOA);
		/* Power LED = GPA0*/
		power_led_state &= 0x01;
		delay = POWER_LED_BURN_FLASH;
		mod_timer(&led_pattern_timer, jiffies + delay);
		break;
	}
	
	default:
		return -EFAULT;
	}
	return 0;
}

static int mcp23s17_read(struct mcp23s17 *mcp, uint8_t reg)
{
    uint8_t tx[2], rx[1];
    int    status;
    
    tx[0] = mcp->addr | 0x01;
    tx[1] = reg;
    status = adt_spi_write_then_read_cs(mcp->spi, tx, sizeof tx, rx, sizeof rx);
    
    return (status < 0) ? status : rx[0];
}

static int mcp23s17_write(struct mcp23s17 *mcp, uint8_t reg, uint8_t val)
{
    uint8_t      tx[3];
    
    tx[0] = mcp->addr;
    tx[1] = reg;
    tx[2] = val;
    
    return adt_spi_write_then_read_cs(mcp->spi, tx, sizeof tx, NULL, 0);
    
}


static void mcp23_spi_config (void)
{
	/*SPI config control register SPCR */
	{
		VPB_SPCR_DTE spcr ;
		BL_VPB_SPI_SPCR_READ ( 0, spcr ) ;
		spcr.mstr = 1 ; /* Goldenrod is always master */
		spcr.lsbf =  0 ; /* 0 - MSB first, 1 - LSB first */
		spcr.spie = 0; /* interrupt disable*/
		spcr.cpol = 1;	/* SPI_SCK is active low */
		spcr.cpha = 1; /* second edge */
		BL_VPB_SPI_SPCR_WRITE ( 0, spcr ) ;
	}
		
	/*SPI config control clock register SPCCR */
	{
		VPB_SPCCR_DTE spccr ;
		spccr.clkcnt = 0x0A;
		BL_VPB_SPI_SPCCR_WRITE ( 0, spccr ) ;
	}
    
}

void io_expander_read_gpb( uint32_t* data )
{
    *data = mcp23s17_read(&mcp, MCP_GPIOB);
}

EXPORT_SYMBOL ( io_expander_read_gpb );
/******************************************************************/
/*                       I/O Expander LEDS                        */
/******************************************************************/

static inline int leds_proc_create (struct proc_dir_entry* root_entry)
{
	int i;
	char buffer[16];

	for (i=0; i < MAX_LEDS; i++)
	{
		memset(buffer, 0x0, sizeof buffer);
		sprintf(buffer, "%d",i);
		led_entry[i] = create_proc_entry(buffer, 0644, root_entry);

		 if (led_entry[i] == NULL)
			goto fail;

		led_entry[i]->read_proc = procfile_read;
		led_entry[i]->write_proc = procfile_write;
		led_entry[i]->owner = THIS_MODULE;
		led_entry[i]->mode = S_IFREG | S_IRUGO;
		led_entry[i]->uid = 0;
		led_entry[i]->gid = 0;
		led_entry[i]->data = &array[i];

		array[i] = i;
	}
	
	return 0;

fail:
	while (i)
	{
		memset(buffer, 0x0, sizeof buffer);
		sprintf(buffer, "%d",i);
		remove_proc_entry(buffer, root_entry);
		i--;
	}
	
	return -ENOMEM;

}

static inline void leds_proc_cleanup (struct proc_dir_entry* root_entry)
{
	int i;
	for (i=0; i <  MAX_LEDS; i++)
	{
		char buffer[16];
		memset(buffer, 0x0, sizeof buffer);
		sprintf(buffer, "%d",i);
		remove_proc_entry(buffer, root_entry);
	}
}

static int procfile_read(char *buf, char **start, off_t off,
                          int count, int *eof, void *data)
{
	int len = 0 ;
	int regs;
	int result;
	
	if (off > 0)
	{
	    *eof = 1;
	    return len;
	}
	
	if (*(int*)data < 8)
	    regs = MCP_GPIOA;
	else
	    regs = MCP_GPIOB;
	
	result = mcp23s17_read(&mcp, regs);
	if  (result >= 0)
	{
	    int bit = *(int*)data < 8 ? *(int*)data : *(int*)data - 8;
	    result &= (1 << bit);
	    result = result >> bit;
	}
	
	len = sprintf (buf, "%d\n", result);
 
	return len;
}

static ssize_t procfile_write(struct file* filp, const char __user *buf, unsigned long len,  void *data)
{
	char buffer[2];
	char value;
	int reg_value, bit;
	uint8_t regs;
	
	memset(buffer, 0x0, sizeof buffer);
    
	if (len > sizeof buffer)
	    return -ENOSPC;

	if (copy_from_user(buffer, buf, len))
	{
	    return -EFAULT;
	}
	/* naive atoi implementation. 1, 10, 11 are equal in such siutation*/
	value = buffer[0] - '0';
	
	if (value  > 1)
	    return -EFAULT;
	
	if (*(int*)data < 8)
	{
	    bit = *(int*)data;
	    reg_value = mcp23s17_read(&mcp, MCP_IODIRA);
	    regs = MCP_GPIOA;
	}
	else
	{
	    bit = *(int*)data - 8;
	    reg_value = mcp23s17_read(&mcp, MCP_IODIRB);
	    regs = MCP_GPIOB;
	}
	
	
	if (reg_value & (1 << bit))
	    return -EFAULT;
	
	reg_value = mcp23s17_read(&mcp, regs);
	
	if  (reg_value >= 0)
	{
	    if (value)
    	        reg_value |= (1 << bit);
    	    else
    	        reg_value &= ~(1 << bit);
	}
	else
	    return -EFAULT;
	
	mcp23s17_write(&mcp, regs, reg_value);
	
	return len;
}

/******************************************************************/
/*                    END  I/O Expander LEDS                      */
/******************************************************************/

/******************************************************************/
/*                          MODEL ID                              */
/******************************************************************/
static void get_model_id_info(void)
{
	/* Pull low the MCP_GPIOA bits 0 to 7, one after the other,
	   and read MCP_GPIO8 every time.           
	   Invert these read values into model ID bits. */

	int mask, mid=0;
	for (mask = 0x01; mask < 0x80; mask = mask << 1)
	{
		mcp23s17_write(&mcp, MCP_GPIOA, (~mask) & 0x7F);
		if ((mcp23s17_read(&mcp, MCP_GPIOA) & 0x80) == 0)
			mid |= mask;
	}
	model_id = mid;
	
	/* Set MCP_GPIOA bits into default state. */
	mcp23s17_write(&mcp, MCP_GPIOA, 0);
}


static int model_id_file_read(char *buf, char **start, off_t off,
                          int count, int *eof, void *data)
{
	int len = 0 ;
	
	if (off > 0)
	{
	    *eof = 1;
	    return len;
	}
	
	len += sprintf (buf, "%d\n", model_id);
 
	return len;
}


static inline struct proc_dir_entry* model_id_proc_create (struct proc_dir_entry* root_entry)
{
	struct proc_dir_entry* tmp;
	tmp = create_proc_entry(MODEL_ID_ENTRY, 0644, root_entry);
        
	if (tmp == NULL)
        	return NULL ;

	tmp->read_proc = model_id_file_read;
	tmp->owner = THIS_MODULE;
	tmp->mode = S_IFREG | S_IRUGO;
	tmp->uid = 0;
	tmp->gid = 0;
	
	return tmp;
}

static inline void model_id_proc_cleanup (struct proc_dir_entry* root_entry)
{
	remove_proc_entry(MODEL_ID_ENTRY, root_entry);
}

/******************************************************************/
/*                        END  MODEL ID                           */
/******************************************************************/

/******************************************************************/
/*                        REGISTERS                               */
/******************************************************************/

static char* register_names[] = {
	"MCP_IODIRA",
	"MCP_IODIRB",
	"MCP_IPOLA",
	"MCP_IPOLB",
	"MCP_GPINTENA",
	"MCP_GPINTENB",
	"MCP_DEFVALA",
	"MCP_DEFVALB",
	"MCP_INTCONA",
	"MCP_INTCONB",
	"MCP_IOCONA",
	"MCP_IOCONB",
	"MCP_GPPUA",
	"MCP_GPPUB",
	"MCP_INTA",
	"MCP_INTB",
	"MCP_INTCAPA",
	"MCP_INTCAPB",
	"MCP_GPIOA",
	"MCP_GPIOB",
	"MCP_OLATA",
	"MCP_OLATB"
};

static int registers_file_read(char *buf, char **start, off_t off,
                         int count, int *eof, void *data)
{
	int len = 0 ;
	int i;
	
	if (off > 0)
	{
	    *eof = 1;
	    return len;
	}
	
	for (i = MCP_IODIRA ; i < MCP_OLATB + 1; i++)
	{
		len += sprintf (buf + len, "%-15s = %0X\n", register_names[i], mcp23s17_read(&mcp, i));
	}
 
	return len;

}

static inline struct proc_dir_entry* registers_proc_create (struct proc_dir_entry* root_entry)
{
	struct proc_dir_entry* tmp = create_proc_entry(REGISTERS_ENTRY, 0644, root_entry);

	if (tmp == NULL)
		return NULL ;

	tmp->read_proc = registers_file_read;
	tmp->owner = THIS_MODULE;
	tmp->mode = S_IFREG | S_IRUGO;
	tmp->uid = 0;
	tmp->gid = 0;
	
	return tmp;
}

static inline void registers_proc_cleanup (struct proc_dir_entry* root_entry)
{
	remove_proc_entry(REGISTERS_ENTRY, root_entry);

}

/******************************************************************/
/*                        END  REGISTERS                          */
/******************************************************************/

/******************************************************************/
/*                            TIMER                               */
/******************************************************************/

static void led_pattern_clbk (unsigned long value)
{
	int result = mcp23s17_read(&mcp, MCP_GPIOA);
	
	if ((result & 0x1) == 1)
	{
		result &= ~1;
	}
	else
	{
		result |= 1;
	}
	
	mcp23s17_write(&mcp, MCP_GPIOA, result);
	
	mod_timer(&led_pattern_timer, jiffies + delay);
}

/******************************************************************/
/*                          END  TIMER                            */
/******************************************************************/


/******************************************************************/
/*                        COMMON INIT                             */
/******************************************************************/

static int mcp23_init_dev(int id)
{
    mcp.spi = adt_get_spi_dev(id);
    
    if (mcp.spi)
    {
	mcp.spi->config = mcp23_spi_config;
	mcp.addr = 0x40; /* there is only the one device on board */
    }
	
    return mcp.spi ? 0 : -ENODEV;
    
}


static int mcp23_init_proc (void)
{
#if 0
	adios_root = proc_mkdir(ADT_PROC_DIR, NULL);

	if (!adios_root)
		return -ENOMEM;

	adios_root->owner = THIS_MODULE;
#endif
	io_expander_root = proc_mkdir(IO_EXPANDER_DIR, adios_root);

	if (!io_expander_root)
		goto fail0;

	io_expander_root->owner = THIS_MODULE;

	leds_root = proc_mkdir(LEDS_DIR, io_expander_root);
	
	if (!leds_root)
		goto fail1;
	
	leds_root->owner = THIS_MODULE;
	
	proc_model_id_entry  = model_id_proc_create(adios_root);

	if (!proc_model_id_entry)
		goto fail2;

	registers = registers_proc_create(io_expander_root);
	
	if (!registers)
		goto fail3;

	if (leds_proc_create(leds_root) != 0 )
		goto fail4;

	return  0;

fail4:
	registers_proc_cleanup(io_expander_root);

fail3:
	model_id_proc_cleanup(adios_root);

fail2:
	remove_proc_entry(LEDS_DIR, io_expander_root);

fail1:
	remove_proc_entry(IO_EXPANDER_DIR, adios_root);

fail0:
#if 0
	remove_proc_entry(ADT_PROC_DIR, NULL);
#endif
	return -ENOMEM;
}

static void mcp23_cleanup_proc(int max_number)
{
	leds_proc_cleanup(leds_root);
	registers_proc_cleanup(io_expander_root);
	model_id_proc_cleanup(adios_root);
	remove_proc_entry(LEDS_DIR,io_expander_root);
	remove_proc_entry(IO_EXPANDER_DIR, adios_root);
#if 0	
	remove_proc_entry(ADT_PROC_DIR, NULL);
#endif	
}


static int __init mcp23_init(void)
{
	int status;
	printk(KERN_INFO"Initialization of MCP\n");
    
	if (mcp23_init_dev(MCP23S17_SPI_DEV) != 0)
		return -ENODEV;

	
	status = misc_register(&mcpMiscDev);
	
	if (status)
	{
		printk (KERN_INFO"Error in register misc device\n");
		return status;
	}

	status = mcp23_init_proc();
    
	if (!status)
	{
		mcp23s17_write(&mcp, MCP_IODIRA, 0x80); /* GPA0-6 output, GPA7 - input*/
		mcp23s17_write(&mcp, MCP_IODIRB, 0xFD); /* GPB1 - output, GPB0-7 - input*/
		
		get_model_id_info();
	}
	else
	{
		misc_deregister(&mcpMiscDev);
	}

	init_timer (&led_pattern_timer);
	led_pattern_timer.data = 0;
	led_pattern_timer.function = led_pattern_clbk;
	
	return status == 0 ? 0 : status;
}

static void __exit mcp23_exit(void)
{
	mcp23_cleanup_proc(MAX_LEDS);
	misc_deregister(&mcpMiscDev);
	del_timer_sync(&led_pattern_timer);
	printk("Exiting MCP... OK\n");
}

int mcp23s17_set_led (int index, int value)
{
	int reg_value = mcp23s17_read(&mcp, MCP_GPIOA);
	
	if  (reg_value >= 0)
	{
	    if (value)
	        reg_value |= (1 << index);
	    else
	        reg_value &= ~(1 << index);
	}
	else
		return -EFAULT;
	
	mcp23s17_write(&mcp, MCP_GPIOA, reg_value);

}
/******************************************************************/
/*                        END COMMON INIT                         */
/******************************************************************/

EXPORT_SYMBOL(mcp23s17_set_led);

module_init(mcp23_init);
module_exit(mcp23_exit);

MODULE_AUTHOR("Stepchenko Aleksey");
MODULE_DESCRIPTION("Driver MCP23S17 on BL234x platfrom");
MODULE_LICENSE("GPL");


