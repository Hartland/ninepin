/* Copyright 2013 by Chris Osborn <fozztexx@fozztexx.com>
 *
 * $Id$
 */

/* FIXME - put some easter eggs in here referencing the R.E.M. song
   "Driver 8" because it's a driver to emulate the 1541 which is
   normally device number 8. */

#include <linux/init.h>
#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/slab.h>
#include <linux/fs.h>
#include <linux/errno.h>
#include <linux/types.h>
#include <linux/proc_fs.h>
#include <linux/fcntl.h>
#include <asm/system.h>
#include <asm/uaccess.h>
#include <linux/interrupt.h>
#include <linux/gpio.h>
#include <asm/io.h>
#include <linux/ioport.h>
#include <linux/delay.h>

#define DRIVER_AUTHOR	"Chris Osborn <fozztexx@fozztexx.com>"
#define DRIVER_DESC	"Commodore IEC serial driver"

#define IEC_ATN		25
#define IEC_CLK		8
#define IEC_DATA	7

#define IEC_DESC	"Clock pin for CBM IEC"
#define DEVICE_DESC	"cbm-iec"
#define IEC_BUFSIZE	1024

#define DATA_EOI	0x100
#define DATA_ATN	0x200

enum {
  IECWaitState = 1,
  IECEOIState,
  IECReadState
};

#define BCM2708_PERI_BASE   0x20000000
#define GPIO_BASE  (BCM2708_PERI_BASE + 0x200000)

#define INPUT	0
#define OUTPUT	1
#define LOW	0
#define HIGH	1

#define digitalRead(pin)	({int _p = (pin) & 31; (*(gpio + 13) & (1 << _p)) >> _p;})
#define digitalWrite(pin, val)	({int _p = (pin) & 31; *(gpio + 7 + ((val) ? 0 : 3)) = \
							 1 << _p;})
#define pinMode(pin, mode)	({int _p = (pin) & 31; *(gpio + _p / 10) = \
							 (*(gpio + _p / 10) & \
							  ~(7 << (_p % 10) * 3)) | \
							 ((mode) << (_p % 10) * 3);})

static short int iec_irq = 0;
static int iec_major = 60;
static uint16_t *iec_buffer;
static short iec_inpos, iec_outpos;
static volatile uint32_t *gpio;

int iec_open(struct inode *inode, struct file *filp);
int iec_close(struct inode *inode, struct file *filp);
ssize_t iec_read(struct file *filp, char __user *buf, size_t count, loff_t *f_pos);
ssize_t iec_write(struct file *filp, const char __user *buf, size_t count, loff_t *f_pos);

struct file_operations iec_fops = {
 owner: THIS_MODULE,
 read: iec_read,
 write: iec_write,
 open: iec_open,
 release: iec_close
};

static irqreturn_t iec_handler(int irq, void *dev_id, struct pt_regs *regs)
{
  unsigned long flags;
  struct timeval start, now;
  int eoi, abort;
  int elapsed;
  int len, bits;


  // disable hard interrupts (remember them in flag 'flags')
  local_irq_save(flags);

  pinMode(IEC_DATA, INPUT);
  do_gettimeofday(&start);
  for (eoi = abort = 0; digitalRead(IEC_CLK); ) {
    do_gettimeofday(&now);
    elapsed = (now.tv_sec - start.tv_sec) * 1000000 + (now.tv_usec - start.tv_usec);

    if (!eoi && elapsed >= 200) {
      pinMode(IEC_DATA, OUTPUT);
      udelay(80);
      pinMode(IEC_DATA, INPUT);
      eoi = 1;
    }

    if (elapsed > 10000) {
      printk("IEC: Timeout during start\n");
      abort = 1;
      break;
    }
  }

  for (len = bits = 0; !abort && len < 8; len++) {
    do_gettimeofday(&start);
    while (!digitalRead(IEC_CLK)) {
      do_gettimeofday(&now);
      elapsed = (now.tv_sec - start.tv_sec) * 1000000 + (now.tv_usec - start.tv_usec);
      if (elapsed >= 10000) {
	printk("IEC: timeout waiting for bit %i\n", len);
	abort = 1;
	break;
      }
    }

    if (abort)
      break;

    if (digitalRead(IEC_DATA))
      bits |= 1 << len;

    do_gettimeofday(&start);
    while (digitalRead(IEC_CLK)) {
      do_gettimeofday(&now);
      elapsed = (now.tv_sec - start.tv_sec) * 1000000 + (now.tv_usec - start.tv_usec);
      if (elapsed >= 10000) {
	printk("IEC: Timeout after bit %i %i\n", len, elapsed);
	if (len < 7)
	  abort = 1;
	break;
      }
    }
  }

  pinMode(IEC_DATA, OUTPUT);

  if (!abort) {
    if (eoi)
      bits |= DATA_EOI;
    if (!digitalRead(IEC_ATN))
      bits |= DATA_ATN;

    iec_buffer[iec_inpos] = bits;
    printk("IEC Read: %03x\n", iec_buffer[iec_inpos]);
    iec_inpos = (iec_inpos + 1) % IEC_BUFSIZE;
  }

  // restore hard interrupts
  local_irq_restore(flags);

  return IRQ_HANDLED;
}

void iec_config(void)
{
  if (gpio_request(IEC_CLK, IEC_DESC)) {
    printk("GPIO request faiure: %s\n", IEC_DESC);
    return;
  }

  if ((iec_irq = gpio_to_irq(IEC_CLK)) < 0) {
    printk("GPIO to IRQ mapping faiure %s\n", IEC_DESC);
    return;
  }

  printk(KERN_NOTICE "Mapped int %d\n", iec_irq);

  if (request_irq(iec_irq, (irq_handler_t) iec_handler, 
		  IRQF_TRIGGER_RISING, IEC_DESC, DEVICE_DESC)) {
    printk("Irq Request failure\n");
    return;
  }

  return;
}

/****************************************************************************/
/* Module init / cleanup block.                                             */
/****************************************************************************/
int iec_init(void)
{
  int result;
  struct resource *mem;


  /* FIXME - dynamically allocate entry in dev with dynamic major */
  /* http://www.makelinux.com/ldd3/ */
  /* http://stackoverflow.com/questions/5970595/create-a-device-node-in-code */
  
  mem = request_mem_region(GPIO_BASE, 4096, DEVICE_DESC);
  gpio = ioremap(GPIO_BASE, 4096);

  pinMode(IEC_ATN, INPUT);
  pinMode(IEC_CLK, INPUT);
  pinMode(IEC_DATA, INPUT);

  digitalWrite(IEC_CLK, LOW);
  digitalWrite(IEC_DATA, LOW);

  /* Pull IEC_DATA low to signal we exist */
  pinMode(IEC_DATA, OUTPUT);
  
  if ((result = register_chrdev(iec_major, DEVICE_DESC, &iec_fops)) < 0) {
    printk(KERN_NOTICE "IEC: cannot obtain major number %i\n", iec_major);
    return result;
  }

  if (!(iec_buffer = kmalloc(IEC_BUFSIZE * sizeof(uint16_t), GFP_KERNEL))) {
    printk(KERN_NOTICE "IEC: failed to allocate buffer\n");
    return -ENOMEM;
  }
  iec_inpos = iec_outpos = 0;

  /* FIXME - don't just say loaded, check iec_config results */
  iec_config();
  printk(KERN_NOTICE "IEC module loaded\n");
  
  return 0;
}

void iec_cleanup(void)
{
  unregister_chrdev(iec_major, DEVICE_DESC);
  kfree(iec_buffer);

  free_irq(iec_irq, DEVICE_DESC);
  gpio_free(IEC_CLK);

  iounmap(gpio);
  printk(KERN_NOTICE "IEC module removed\n");
  return;
}

int iec_open(struct inode *inode, struct file *filp)
{
  return 0;
}

int iec_close(struct inode *inode, struct file *filp)
{
  return 0;
}

ssize_t iec_read(struct file *filp, char *buf, size_t count, loff_t *f_pos)
{
  unsigned long remaining;
  int avail;


  avail = (IEC_BUFSIZE + iec_inpos - iec_outpos) % IEC_BUFSIZE;
  avail *= 2;
  /* FIXME - if avail is zero, block */
  
  if (count % 2)
    count--;
  if (count > avail)
    count = avail;
  remaining = copy_to_user(buf, &iec_buffer[iec_outpos], count);
  iec_outpos += (count - remaining) / 2;

  if (*f_pos == 0) {
    *f_pos += count;
    return count;
  }

  return 0;
}

ssize_t iec_write(struct file *filp, const char __user *buf, size_t count, loff_t *f_pos)
{
  return 1;
}

module_init(iec_init);
module_exit(iec_cleanup);

/****************************************************************************/
/* Module licensing/description block.                                      */
/****************************************************************************/
MODULE_LICENSE("GPL");
MODULE_AUTHOR(DRIVER_AUTHOR);
MODULE_DESCRIPTION(DRIVER_DESC);
