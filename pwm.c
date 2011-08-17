/*
 Copyright (c) 2010, Scott Ellis
 All rights reserved.

 Redistribution and use in source and binary forms, with or without
 modification, are permitted provided that the following conditions are met:
	* Redistributions of source code must retain the above copyright
	  notice, this list of conditions and the following disclaimer.
	* Redistributions in binary form must reproduce the above copyright
	  notice, this list of conditions and the following disclaimer in the
	  documentation and/or other materials provided with the distribution.
	* Neither the name of the <organization> nor the
	  names of its contributors may be used to endorse or promote products
	  derived from this software without specific prior written permission.

 THIS SOFTWARE IS PROVIDED BY Scott Ellis ''AS IS'' AND ANY
 EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED
 WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE
 DISCLAIMED. IN NO EVENT SHALL Scott Ellis BE LIABLE FOR ANY
 DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES
 (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES;
 LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND
 ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF THIS
 SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
*/

#include <linux/init.h> 
#include <linux/module.h>
#include <linux/device.h>
#include <linux/semaphore.h>
#include <linux/cdev.h>
#include <linux/fs.h>
#include <linux/errno.h>
#include <asm/io.h>
#include <asm/uaccess.h>
#include <linux/moduleparam.h>
#include <linux/slab.h>
#include <linux/clk.h>

#include "pwm.h"

/* default frequency of 1 kHz */
#define DEFAULT_TLDR	0xFFFFFFE0

/* default 50% duty cycle */
/* TMAR = (0xFFFFFFFF - ((0xFFFFFFFF - (DEFAULT_TLDR + 1)) / 2)) */
#define DEFAULT_TMAR	0xFFFFFFEF

/* default TCLR is off state */
#define DEFAULT_TCLR (GPT_TCLR_PT | GPT_TCLR_TRG_OVFL_MATCH | GPT_TCLR_CE | GPT_TCLR_AR) 

static int pwm = 10;
module_param(pwm, int, S_IRUGO);
MODULE_PARM_DESC(pwm, "which pwm");

#define DEFAULT_PWM_FREQUENCY 1024

static int frequency = DEFAULT_PWM_FREQUENCY;
module_param(frequency, int, S_IRUGO);
MODULE_PARM_DESC(frequency, "the pwm frequency");

static int use_sys_clock = 0;
module_param(use_sys_clock, int, S_IRUGO);
MODULE_PARM_DESC(use_sys_clock, "use 13 MHz source clock");


#define USER_BUFF_SIZE	128


struct pwm_dev {
	dev_t devt;
	struct cdev cdev;
	struct class *class;
	struct semaphore sem;
	u32 pwm;
	u32 mux_offset;
	u32 gpt_base;
	struct clk *clk;
	u32 input_freq;
	u32 old_mux;
	u32 tldr;
	u32 tmar;
	u32 tclr;
	u32 num_freqs;
	char *user_buff;
};

static struct pwm_dev pwm_dev;


static int init_mux(void)
{
	void __iomem *base;

	base = ioremap(OMAP34XX_PADCONF_START, OMAP34XX_PADCONF_SIZE);
	if (!base) {
		printk(KERN_ALERT "init_mux(): ioremap() failed\n");
		return -1;
	}

	pwm_dev.old_mux = ioread16(base + pwm_dev.mux_offset);
	iowrite16(PWM_ENABLE_MUX, base + pwm_dev.mux_offset);
	iounmap(base);	

	return 0;
}

static int restore_mux(void)
{
	void __iomem *base;

	if (pwm_dev.old_mux) {
		base = ioremap(OMAP34XX_PADCONF_START, OMAP34XX_PADCONF_SIZE);
	
		if (!base) {
			printk(KERN_ALERT "restore_mux(): ioremap() failed\n");
			return -1;
		}

		iowrite16(pwm_dev.old_mux, base + pwm_dev.mux_offset);
		iounmap(base);	
	}

	return 0;
}

static int pwm_enable_clock(void)
{
	char id[16];

	if (pwm_dev.clk)
		return 0;

	sprintf(id, "gpt%d_fck", pwm_dev.pwm);
	
	pwm_dev.clk = clk_get(NULL, id);

	if (IS_ERR(pwm_dev.clk)) {
		printk(KERN_ERR "Failed to get %s\n", id);
		return -1;
	}

	pwm_dev.input_freq = clk_get_rate(pwm_dev.clk);
		
	if (clk_enable(pwm_dev.clk)) {
		clk_put(pwm_dev.clk);
		pwm_dev.clk = NULL;
		printk(KERN_ERR "Error enabling %s\n", id);
		return -1;
	}
	
	return 0;	
}

static void pwm_free_clock(void)
{
	if (pwm_dev.clk) {
		clk_disable(pwm_dev.clk);
		clk_put(pwm_dev.clk);
	}
}

#define CLKSEL_GPT11 0x80
#define CLKSEL_GPT10 0x40

/* 
 * Change PWM10 or PWM11 to use the 13MHz CM_SYS_CLK rather then the
 * 32kHz CM_32K_CLK 
 * It would be nice to do this with clk_set_rate() or omap2_clksel_set_rate()
 * or some built-in function like that, but no joy getting one to work thus
 * far.
 */
static int pwm_use_sys_clk(void)
{
	void __iomem *base;
	u32 val;

	if (pwm_dev.pwm != 10 && pwm_dev.pwm != 11)
		return 0;
		
	if (pwm_dev.input_freq == CLK_SYS_FREQ)
		return 0;
		
	base = ioremap(CLOCK_CONTROL_REG_CM_START, CLOCK_CONTROL_REG_CM_SIZE);

	if (!base) {
		printk(KERN_ALERT "use_sys_clk(): ioremap() failed\n");
		return -1;
	}

	val = ioread32(base + CM_CLKSEL_CORE_OFFSET);
	
	if (pwm_dev.pwm == 10)
		val |= CLKSEL_GPT10;
	else
		val |= CLKSEL_GPT11;
		
	iowrite32(val, base + CM_CLKSEL_CORE_OFFSET);
	iounmap(base);

	pwm_dev.input_freq = CLK_SYS_FREQ;
		
	return 0;
}

/* 
 * Restore PWM10 or PWM11 to using the CM_32K_CLK 
 */
static int pwm_restore_32k_clk(void)
{
	void __iomem *base;
	u32 val;

	if (pwm_dev.pwm != 10 && pwm_dev.pwm != 11)
		return 0;
		
	if (pwm_dev.input_freq == CLK_32K_FREQ)
		return 0;

	base = ioremap(CLOCK_CONTROL_REG_CM_START, CLOCK_CONTROL_REG_CM_SIZE);

	if (!base) {
		printk(KERN_ALERT "restore_32k_clk(): ioremap() failed\n");
		return -1;
	}

	val = ioread32(base + CM_CLKSEL_CORE_OFFSET);
	
	if (pwm_dev.pwm == 10)
		val &= ~CLKSEL_GPT10;
	else
		val &= ~CLKSEL_GPT11;
		
	iowrite32(val, base + CM_CLKSEL_CORE_OFFSET);
	iounmap(base);

	pwm_dev.input_freq = CLK_32K_FREQ;
	
	return 0;
}

static int set_pwm_frequency(void)
{
	void __iomem *base;

	base = ioremap(pwm_dev.gpt_base, GPT_REGS_PAGE_SIZE);
	if (!base) {
		printk(KERN_ALERT "set_pwm_frequency(): ioremap failed\n");
		return -1;
	}

	if (frequency <= 0)
		frequency = DEFAULT_PWM_FREQUENCY;
	else if (frequency > (pwm_dev.input_freq / 2)) 
		frequency = pwm_dev.input_freq / 2;

	/* PWM_FREQ = 32768 / ((0xFFFF FFFF - TLDR) + 1) */
	pwm_dev.tldr = 0xFFFFFFFF - ((pwm_dev.input_freq / frequency) - 1);

	/* just for convenience */	
	pwm_dev.num_freqs = 0xFFFFFFFE - pwm_dev.tldr;	

	iowrite32(pwm_dev.tldr, base + GPT_TLDR);

	// initialize TCRR to TLDR, have to start somewhere
	iowrite32(pwm_dev.tldr, base + GPT_TCRR);

	iounmap(base);

	return 0;
}

static int pwm_off(void)
{
	void __iomem *base;

	base = ioremap(pwm_dev.gpt_base, GPT_REGS_PAGE_SIZE);
	if (!base) {
		printk(KERN_ALERT "pwm_off(): ioremap failed\n");
		return -1;
	}

	pwm_dev.tclr &= ~GPT_TCLR_ST;
	iowrite32(pwm_dev.tclr, base + GPT_TCLR); 
	iounmap(base);

	return 0;
}

static int pwm_on(void)
{
	void __iomem *base;

	base = ioremap(pwm_dev.gpt_base, GPT_REGS_PAGE_SIZE);

	if (!base) {
		printk(KERN_ALERT "pwm_on(): ioremap failed\n");
		return -1;
	}

	/* set the duty cycle */
	iowrite32(pwm_dev.tmar, base + GPT_TMAR);
	
	/* now turn it on */
	pwm_dev.tclr = ioread32(base + GPT_TCLR);
	pwm_dev.tclr |= GPT_TCLR_ST;
	iowrite32(pwm_dev.tclr, base + GPT_TCLR); 
	iounmap(base);

	return 0;
}

static int set_duty_cycle(unsigned int duty_cycle) 
{
	unsigned int new_tmar;

	pwm_off();

	if (duty_cycle == 0)
		return 0;
 
	new_tmar = (duty_cycle * pwm_dev.num_freqs) / 100;

	if (new_tmar < 1) 
		new_tmar = 1;
	else if (new_tmar > pwm_dev.num_freqs)
		new_tmar = pwm_dev.num_freqs;
		
	pwm_dev.tmar = pwm_dev.tldr + new_tmar;
	
	return pwm_on();
}

static ssize_t pwm_read(struct file *filp, char __user *buff, size_t count,
			loff_t *offp)
{
	size_t len;
	unsigned int duty_cycle;
	ssize_t error = 0;

	if (!buff) 
		return -EFAULT;

	/* tell the user there is no more */
	if (*offp > 0) 
		return 0;

	if (down_interruptible(&pwm_dev.sem)) 
		return -ERESTARTSYS;

	if (pwm_dev.tclr & GPT_TCLR_ST) {
		duty_cycle = (100 * (pwm_dev.tmar - pwm_dev.tldr)) 
				/ pwm_dev.num_freqs;

		snprintf(pwm_dev.user_buff, USER_BUFF_SIZE,
				"PWM%d Frequency %u Hz Duty Cycle %u%%\n",
				pwm_dev.pwm, frequency, duty_cycle);
	}
	else {
		snprintf(pwm_dev.user_buff, USER_BUFF_SIZE,
				"PWM%d Frequency %u Hz Stopped\n",
				pwm_dev.pwm, frequency);
	}

	len = strlen(pwm_dev.user_buff);
 
	if (len + 1 < count) 
		count = len + 1;

	if (copy_to_user(buff, pwm_dev.user_buff, count))  {
		printk(KERN_ALERT "pwm_read(): copy_to_user() failed\n");
		error = -EFAULT;
	}
	else {
		*offp += count;
		error = count;
	}

	up(&pwm_dev.sem);

	return error;	
}

static ssize_t pwm_write(struct file *filp, const char __user *buff, 
			size_t count, loff_t *offp)
{
	size_t len;
	unsigned int duty_cycle;
	ssize_t error = 0;
	
	if (down_interruptible(&pwm_dev.sem)) 
		return -ERESTARTSYS;

	if (!buff || count < 1) {
		printk(KERN_ALERT "pwm_write(): input check failed\n");
		error = -EFAULT; 
		goto pwm_write_done;
	}
	
	/* we are only expecting a small integer, ignore anything else */
	if (count > 8)
		len = 8;
	else
		len = count;
		
	memset(pwm_dev.user_buff, 0, 16);

	if (copy_from_user(pwm_dev.user_buff, buff, len)) {
		printk(KERN_ALERT "pwm_write(): copy_from_user() failed\n"); 
		error = -EFAULT; 	
		goto pwm_write_done;
	}


	duty_cycle = simple_strtoul(pwm_dev.user_buff, NULL, 0);

	set_duty_cycle(duty_cycle);

	/* pretend we ate it all */
	*offp += count;

	error = count;

pwm_write_done:

	up(&pwm_dev.sem);
	
	return error;
}

static int pwm_open(struct inode *inode, struct file *filp)
{
	int error = 0;

	if (down_interruptible(&pwm_dev.sem)) 
		return -ERESTARTSYS;

	if (pwm_dev.old_mux == 0) {
		if (init_mux())  
			error = -EIO;
		else if (set_pwm_frequency()) 
			error = -EIO;		
	}

	if (!pwm_dev.user_buff) {
		pwm_dev.user_buff = kmalloc(USER_BUFF_SIZE, GFP_KERNEL);
		if (!pwm_dev.user_buff)
			error = -ENOMEM;
	}

	up(&pwm_dev.sem);

	return error;	
}

static struct file_operations pwm_fops = {
	.owner = THIS_MODULE,
	.read = pwm_read,
	.write = pwm_write,
	.open = pwm_open,
};

static int __init pwm_init_cdev(void)
{
	int error;

	error = alloc_chrdev_region(&pwm_dev.devt, pwm_dev.pwm, 
					1, "pwm");

	if (error < 0) {
		printk(KERN_ALERT "alloc_chrdev_region() failed: %d \n", 
			error);
		return error;
	}

	cdev_init(&pwm_dev.cdev, &pwm_fops);
	pwm_dev.cdev.owner = THIS_MODULE;
	
	error = cdev_add(&pwm_dev.cdev, pwm_dev.devt, 1);
	if (error) {
		printk(KERN_ALERT "cdev_add() failed: %d\n", error);
		unregister_chrdev_region(pwm_dev.devt, 1);
		return error;
	}	

	return 0;
}

static int __init pwm_init_class(void)
{
	pwm_dev.class = class_create(THIS_MODULE, "pwm");

	if (!pwm_dev.class) {
		printk(KERN_ALERT "class_create() failed\n");
		return -1;
	}

	if (!device_create(pwm_dev.class, NULL, pwm_dev.devt, NULL, "pwm%d", 
				MINOR(pwm_dev.devt))) {
		printk(KERN_ALERT "device_create(..., pwm) failed\n");
		class_destroy(pwm_dev.class);
		return -1;
	}

	return 0;
}
 
static int __init pwm_init(void)
{
	int error;

	if (pwm < 8 || pwm > 11) {
		printk(KERN_ERR "Invalid pwm argument: %d\n", pwm);
		return -EINVAL;
	}
	
	pwm_dev.pwm = pwm;
	
	switch (pwm) {
	case 8:	
		pwm_dev.mux_offset = GPT8_MUX_OFFSET;
		pwm_dev.gpt_base = PWM8_CTL_BASE;	
		break;
		
	case 9:
		pwm_dev.mux_offset = GPT9_MUX_OFFSET;
		pwm_dev.gpt_base = PWM9_CTL_BASE;
		break;
		
	case 10:
		pwm_dev.mux_offset = GPT10_MUX_OFFSET;
		pwm_dev.gpt_base = PWM10_CTL_BASE;
		break;
		
	case 11:	
		pwm_dev.mux_offset = GPT11_MUX_OFFSET;
		pwm_dev.gpt_base = PWM11_CTL_BASE;
	}
		
	pwm_dev.tldr = DEFAULT_TLDR;
	pwm_dev.tmar = DEFAULT_TMAR;
	pwm_dev.tclr = DEFAULT_TCLR;

	sema_init(&pwm_dev.sem, 1);

	error = pwm_init_cdev();
	if (error)
		goto init_fail;

	error = pwm_init_class();
	if (error)
		goto init_fail_2;

	error = pwm_enable_clock();
	if (error)
		goto init_fail_3;

	if (use_sys_clock) {
		 error = pwm_use_sys_clk();
		 if (error)
			goto init_fail_4;
	}
	
	printk(KERN_INFO "source clock rate %u\n", pwm_dev.input_freq);
		
	return 0;

init_fail_4:
	pwm_free_clock();
	
init_fail_3:
	device_destroy(pwm_dev.class, pwm_dev.devt);
	class_destroy(pwm_dev.class);
	
init_fail_2:
	cdev_del(&pwm_dev.cdev);
	unregister_chrdev_region(pwm_dev.devt, 1);

init_fail:
	return error;
}
module_init(pwm_init);

static void __exit pwm_exit(void)
{
	device_destroy(pwm_dev.class, pwm_dev.devt);
	class_destroy(pwm_dev.class);

	cdev_del(&pwm_dev.cdev);
	unregister_chrdev_region(pwm_dev.devt, 1);

	pwm_off();
	
	if (use_sys_clock)
		pwm_restore_32k_clk();
		
	pwm_free_clock();
	restore_mux();

	if (pwm_dev.user_buff)
		kfree(pwm_dev.user_buff);
}
module_exit(pwm_exit);

MODULE_LICENSE("GPL");
MODULE_AUTHOR("Scott Ellis - Jumpnow");
MODULE_DESCRIPTION("PWM example for Gumstix Overo"); 

