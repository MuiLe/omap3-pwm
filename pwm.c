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

 Authors: Scott Ellis, Jack Elston, Curt Olson
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

/* default TCLR is off state */
#define DEFAULT_TCLR (GPT_TCLR_PT | GPT_TCLR_TRG_OVFL_MATCH | GPT_TCLR_CE | GPT_TCLR_AR) 

static int frequency = 0;
module_param(frequency, int, S_IRUGO);
MODULE_PARM_DESC(frequency, "PWM frequency");

#define MAX_TIMERS 4
static int timers[MAX_TIMERS] = { 8, 9, 10, 11 };
static int num_timers = 0;
module_param_array(timers, int, &num_timers, 0000);
MODULE_PARM_DESC(timers, "List of PWM timers to control");

static int servo = 0;
module_param(servo, int, S_IRUGO);
MODULE_PARM_DESC(servo, "Enable servo mode operation");

#define SERVO_ABSOLUTE_MIN 10000
#define SERVO_ABSOLUTE_MAX 20000
#define SERVO_CENTER 15000

static int servo_min = 10000;
module_param(servo_min, int, S_IRUGO);
MODULE_PARM_DESC(servo_min, "Servo min value in tenths of usec, default 10000");

static int servo_max = 20000;
module_param(servo_max, int, S_IRUGO);
MODULE_PARM_DESC(servo_max, "Servo max value in tenths of usec, default 20000");


#define USER_BUFF_SIZE	128

struct pwm_dev {
	dev_t devt;
	struct cdev cdev;
	struct device *device;
	struct semaphore sem;
	u32 pwm;
	u32 mux_offset;
	u32 phys_base;
	void __iomem *virt_base;
	struct clk *clk;
	u32 input_freq;
	u32 old_mux;
	u32 tldr;
	u32 tmar;
	u32 tclr;
	u32 num_freqs;
	u32 current_val;
	char *user_buff;
};

// only one class
struct class *pwm_class;

static struct pwm_dev pwm_dev[MAX_TIMERS];


static int pwm_init_mux(struct pwm_dev *pd)
{
	void __iomem *base;

	base = ioremap(OMAP34XX_PADCONF_START, OMAP34XX_PADCONF_SIZE);
	if (!base) {
		printk(KERN_ALERT "pwm_init_mux: ioremap failed\n");
		return -1;
	}

	pd->old_mux = ioread16(base + pd->mux_offset);
	iowrite16(PWM_ENABLE_MUX, base + pd->mux_offset);
	iounmap(base);	

	return 0;
}

static int pwm_restore_mux(struct pwm_dev *pd)
{
	void __iomem *base;

	if (pd->old_mux) {
		base = ioremap(OMAP34XX_PADCONF_START, OMAP34XX_PADCONF_SIZE);
	
		if (!base) {
			printk(KERN_ALERT "pwm_restore_mux: ioremap failed\n");
			return -1; 
		}

		iowrite16(pd->old_mux, base + pd->mux_offset);
		iounmap(base);	
	}

	return 0;
}

static int pwm_enable_clock(struct pwm_dev *pd)
{
	char id[16];

	if (pd->clk)
		return 0;

	sprintf(id, "gpt%d_fck", pd->pwm);
	
	pd->clk = clk_get(NULL, id);

	if (IS_ERR(pd->clk)) {
		printk(KERN_ERR "Failed to get %s\n", id);
		return -1;
	}

	pd->input_freq = clk_get_rate(pd->clk);
		
	if (clk_enable(pd->clk)) {
		clk_put(pd->clk);
		pd->clk = NULL;
		printk(KERN_ERR "Error enabling %s\n", id);
		return -1;
	}
	
	return 0;	
}

static void pwm_free_clock(struct pwm_dev *pd)
{
	if (pd->clk) {
		clk_disable(pd->clk);
		clk_put(pd->clk);
	}
}

#define CLKSEL_GPT11 0x80
#define CLKSEL_GPT10 0x40
/*
 * Changes PWM10 and PWM11 to use the CM_SYS_CLK as a source rather then the
 * CM_32K_CLK. We override the input_freq we got directly from the clk.
 * This sucks and is hackish, but I haven't figured out how to do this through
 * the clock infrastructure. It would nice if I could use something like
 * clk_sel_rate() or omap2_clksel_set_rate(). No joy so far.
 */
static int pwm_use_sys_clk(void)
{
	void __iomem *base;
	u32 val, mask, i;

	mask = 0;
	
	for (i = 0; i < num_timers; i++) {
		if (pwm_dev[i].pwm == 10) {
			mask |= CLKSEL_GPT10;
			pwm_dev[i].input_freq = CLK_SYS_FREQ;
		}
		else if (pwm_dev[i].pwm == 11) {
			mask |= CLKSEL_GPT11;
			pwm_dev[i].input_freq = CLK_SYS_FREQ;
		}
	}

	// nothing to do
	if (mask == 0)
		return 0;
		
	base = ioremap(CLOCK_CONTROL_REG_CM_START, CLOCK_CONTROL_REG_CM_SIZE);

	if (!base) {
		printk(KERN_ALERT "pwm_use_sys_clk: ioremap failed\n");
		return -1;
	}

	val = ioread32(base + CM_CLKSEL_CORE_OFFSET);
	val |= mask;
	iowrite32(val, base + CM_CLKSEL_CORE_OFFSET);
	iounmap(base);

	return 0;
}

/* Restore PWM10 and PWM11 to using the CM_32K_CLK */
static int pwm_restore_32k_clk(void)
{
	void __iomem *base;
	u32 val, i;

	base = ioremap(CLOCK_CONTROL_REG_CM_START, CLOCK_CONTROL_REG_CM_SIZE);

	if (!base) {
		printk(KERN_ALERT "pwm_restore_32k_clk: ioremap failed\n");
		return -1;
	}

	val = ioread32(base + CM_CLKSEL_CORE_OFFSET);
	val &= ~(CLKSEL_GPT10 | CLKSEL_GPT11);
	iowrite32(val, base + CM_CLKSEL_CORE_OFFSET);
	iounmap(base);

	for (i = 0; i < num_timers; i++) {
		if (pwm_dev[i].pwm == 10 || pwm_dev[i].pwm == 11)
			pwm_dev[i].input_freq = CLK_32K_FREQ;
	}
	
	return 0;
}

static int pwm_set_frequency(struct pwm_dev *pd)
{
	if (frequency > (pd->input_freq / 2)) 
		frequency = pd->input_freq / 2;

	pd->tldr = 0xFFFFFFFF - ((pd->input_freq / frequency) - 1);

	pd->num_freqs = 0xFFFFFFFE - pd->tldr;	

	iowrite32(pd->tldr, pd->virt_base + GPT_TLDR);

	// initialize TCRR to TLDR, have to start somewhere
	iowrite32(pd->tldr, pd->virt_base + GPT_TCRR);

	return 0;
}

static int pwm_off(struct pwm_dev *pd)
{
	pd->tclr &= ~GPT_TCLR_ST;
	iowrite32(pd->tclr, pd->virt_base + GPT_TCLR); 

	pd->current_val = 0;
	
	return 0;
}

static int pwm_on(struct pwm_dev *pd)
{
	/* set the duty cycle */
	iowrite32(pd->tmar, pd->virt_base + GPT_TMAR);
	
	/* now turn it on */
	pd->tclr = ioread32(pd->virt_base + GPT_TCLR);
	pd->tclr |= GPT_TCLR_ST;
	iowrite32(pd->tclr, pd->virt_base + GPT_TCLR); 
	
	return 0;
}

static int pwm_set_duty_cycle(struct pwm_dev *pd, u32 duty_cycle) 
{
	u32 new_tmar;

	if (duty_cycle > 100)
		return -EINVAL;

	pd->current_val = duty_cycle;

	if (duty_cycle == 0) {
		pwm_off(pd);
		return 0;
	}
 
	new_tmar = (duty_cycle * pd->num_freqs) / 100;

	if (new_tmar < 1) 
		new_tmar = 1;
	else if (new_tmar > pd->num_freqs)
		new_tmar = pd->num_freqs;
		
	pd->tmar = pd->tldr + new_tmar;

	return pwm_on(pd);
}

static int pwm_set_servo_pulse(struct pwm_dev *pd, u32 tenths_us)
{
	u32 new_tmar, factor;
	
	if (tenths_us < servo_min || tenths_us > servo_max) 
		return -EINVAL;

	pd->current_val = tenths_us;
		
	factor = 10000000 / (frequency * 2);
	new_tmar = (tenths_us * (pd->num_freqs / 2)) / factor;
	
	if (new_tmar < 1)
		new_tmar = 1;
	else if (new_tmar > pd->num_freqs)
		new_tmar = pd->num_freqs;
		
	pd->tmar = pd->tldr + new_tmar;

	return pwm_on(pd);
}

static void pwm_timer_cleanup(void)
{
	int i;
	
	// since this is called by error handling code, only call this 
	// function if PWM10 or PWM11 fck is enabled or you might oops
	for (i = 0; i < num_timers; i++) {
		if ((pwm_dev[i].pwm == 10 || pwm_dev[i].pwm == 11)
				&& pwm_dev[i].clk) {
			pwm_restore_32k_clk();
			break;
		}
	}
	
	for (i = 0; i < num_timers; i++) {
		pwm_free_clock(&pwm_dev[i]);
		pwm_restore_mux(&pwm_dev[i]);	
		
		if (pwm_dev[i].virt_base) {
			iounmap(pwm_dev[i].virt_base);
			pwm_dev[i].virt_base = NULL;
		}
	}
}

static int pwm_timer_init(void)
{
	int i;

	for (i = 0; i < num_timers; i++) {
		if (pwm_init_mux(&pwm_dev[i]))
			goto timer_init_fail;
		
		if (pwm_enable_clock(&pwm_dev[i]))
			goto timer_init_fail;			
	}

	// not configurable right now, we always use it
	if (pwm_use_sys_clk()) 
		goto timer_init_fail;
	
	for (i = 0; i < num_timers; i++) {
		pwm_dev[i].virt_base = ioremap(pwm_dev[i].phys_base, 
						GPT_REGS_PAGE_SIZE);
						
		if (!pwm_dev[i].virt_base)
			goto timer_init_fail;

		pwm_off(&pwm_dev[i]);
		
		// frequency is a global module param for all timers
		if (pwm_set_frequency(&pwm_dev[i]))
			goto timer_init_fail;

		if (servo)
			pwm_set_servo_pulse(&pwm_dev[i], SERVO_CENTER);
	}

	return 0;
	
timer_init_fail:

	pwm_timer_cleanup();
	
	return -1;
}

static ssize_t pwm_read(struct file *filp, char __user *buff, size_t count,
			loff_t *offp)
{
	size_t len;
	ssize_t status;
	struct pwm_dev *pd = filp->private_data;

	if (!buff) 
		return -EFAULT;

	// for user progs like cat that will keep asking forever
	if (*offp > 0) 
		return 0;

	if (down_interruptible(&pd->sem)) 
		return -ERESTARTSYS;

	if (pd->tclr & GPT_TCLR_ST) {
		len = sprintf(pd->user_buff, "%u\n", pd->current_val); 
	}
	else {
		len = sprintf(pd->user_buff, "%u\n", pd->current_val); 
	}

	if (len + 1 < count) 
		count = len + 1;

	if (copy_to_user(buff, pd->user_buff, count))  {
		printk(KERN_ALERT "pwm_read: copy_to_user failed\n");
		status = -EFAULT;
	}
	else {
		*offp += count;
		status = count;
	}

	up(&pd->sem);

	return status;	
}

static ssize_t pwm_write(struct file *filp, const char __user *buff, 
			size_t count, loff_t *offp)
{
	size_t len;
	u32 val;
	ssize_t status = 0;

	struct pwm_dev *pd = filp->private_data;
	
	if (down_interruptible(&pd->sem)) 
		return -ERESTARTSYS;

	if (!buff || count < 1) {
		printk(KERN_ALERT "pwm_write: input check failed\n");
		status = -EFAULT; 
		goto pwm_write_done;
	}
	
	if (count > 8)
		len = 8;
	else
		len = count;
		
	memset(pd->user_buff, 0, 16);

	if (copy_from_user(pd->user_buff, buff, len)) {
		printk(KERN_ALERT "pwm_write: copy_from_user failed\n"); 
		status = -EFAULT; 	
		goto pwm_write_done;
	}

	val = simple_strtoul(pd->user_buff, NULL, 0);

	if (servo)
		status = pwm_set_servo_pulse(pd, val);
	else
		status = pwm_set_duty_cycle(pd, val);

	*offp += count;

	if (!status)
		status = count;

pwm_write_done:

	up(&pd->sem);
	
	return status;
}

static int pwm_open(struct inode *inode, struct file *filp)
{
	int error = 0;
	struct pwm_dev *pd = container_of(inode->i_cdev, struct pwm_dev, cdev);
	filp->private_data = pd;

	if (down_interruptible(&pd->sem)) 
		return -ERESTARTSYS;

	if (!pd->user_buff) {
		pd->user_buff = kmalloc(USER_BUFF_SIZE, GFP_KERNEL);
		if (!pd->user_buff)
			error = -ENOMEM;
	}

	up(&pd->sem);

	return error;	
}

static struct file_operations pwm_fops = {
	.owner = THIS_MODULE,
	.read = pwm_read,
	.write = pwm_write,
	.open = pwm_open,
};

static int __init pwm_init_cdev(struct pwm_dev *pd)
{
	int error;

	error = alloc_chrdev_region(&pd->devt, pd->pwm, 1, "pwm");

	if (error < 0) {
		printk(KERN_ALERT "alloc_chrdev_region fail: %d \n", error);
		pd->devt = 0;
		return -1;
	}

	cdev_init(&pd->cdev, &pwm_fops);
	pd->cdev.owner = THIS_MODULE;
	
	error = cdev_add(&pd->cdev, pd->devt, 1);
	if (error) {
		printk(KERN_ALERT "cdev_add failed: %d\n", error);
		unregister_chrdev_region(pd->devt, 1);
		pd->devt = 0;
		return -1;
	}	

	return 0;
}

static int __init pwm_init_class(struct pwm_dev *pd)
{
	if (!pwm_class) {
		pwm_class = class_create(THIS_MODULE, "pwm");

		if (!pwm_class) {
			printk(KERN_ALERT "class_create failed\n");
			return -1;
		}
	}
	
	pd->device = device_create(pwm_class, NULL, pd->devt, NULL, "pwm%d", 
				MINOR(pd->devt));
					
	if (!pd->device) {				
		printk(KERN_ALERT "device_create(..., pwm) failed\n");					
		return -1;
	}

	return 0;
}

static void pwm_dev_cleanup(void)
{
	int i;
	
	for (i = 0; i < num_timers; i++) {
		if (pwm_dev[i].device)
			device_destroy(pwm_class, pwm_dev[i].devt);
	}
	
	class_destroy(pwm_class);
	
	for (i = 0; i < num_timers; i++) {
		cdev_del(&pwm_dev[i].cdev);
		unregister_chrdev_region(pwm_dev[i].devt, 1);
	}			
}

struct timer_init {
	u32 pwm;
	u32 mux_offset;
	u32 phys_base;
	u32 used;
};

static struct timer_init timer_init[MAX_TIMERS] = {
	{ 8, GPT8_MUX_OFFSET, PWM8_CTL_BASE, 0 },
	{ 9, GPT9_MUX_OFFSET, PWM9_CTL_BASE, 0 },
	{ 10, GPT10_MUX_OFFSET, PWM10_CTL_BASE, 0 },
	{ 11, GPT11_MUX_OFFSET, PWM11_CTL_BASE, 0 }
};
	
static int pwm_init_timer_list(void)
{
	int i, j;
	
	if (num_timers == 0)
		num_timers = 4;
		
	for (i = 0; i < num_timers; i++) {
		for (j = 0; j < MAX_TIMERS; j++) {
			if (timers[i] == timer_init[j].pwm)
				break;			
		}
		
		if (j == MAX_TIMERS) {
			printk(KERN_ERR "Invalid timer requested: %d\n", 
				timers[i]);
				
			return -1;
		}
		
		if (timer_init[j].used) {
			printk(KERN_ERR "Timer %d specified more then once\n",
				timers[i]);
			return -1;	
		}
		
		timer_init[j].used = 1;
		pwm_dev[i].pwm = timer_init[j].pwm;
		pwm_dev[i].mux_offset = timer_init[j].mux_offset;
		pwm_dev[i].phys_base = timer_init[j].phys_base;
	}
						
	return 0;			
}

static int __init pwm_init(void)
{
	int i;

	if (pwm_init_timer_list())
		return -1;
	
	for (i = 0; i < num_timers; i++) {
		pwm_dev[i].tclr = DEFAULT_TCLR;

		sema_init(&pwm_dev[i].sem, 1);

		if (pwm_init_cdev(&pwm_dev[i]))
			goto init_fail;

		if (pwm_init_class(&pwm_dev[i]))
			goto init_fail;
	}

	if (servo)
		frequency = 50;
	else if (frequency <= 0)
		frequency = 1024;


	if (servo) {
		if (servo_min < SERVO_ABSOLUTE_MIN)
			servo_min = SERVO_ABSOLUTE_MIN;

		if (servo_max > SERVO_ABSOLUTE_MAX)
			servo_max = SERVO_ABSOLUTE_MAX;

		if (servo_min >= servo_max) {
			servo_min = SERVO_ABSOLUTE_MIN;
			servo_max = SERVO_ABSOLUTE_MAX;
		}
	}

	if (pwm_timer_init())
		goto init_fail_2;

	if (servo) {
		printk(KERN_INFO 
			"pwm: frequency=%d Hz servo=%d servo_min = %d servo_max = %d\n",
			frequency, servo, servo_min, servo_max);
	}
	else {
		printk(KERN_INFO "pwm: frequency=%d Hz  servo=%d\n",
			frequency, servo);
	}
	
	return 0;
	
init_fail_2:	
	pwm_timer_cleanup();
	
init_fail:	
	pwm_dev_cleanup();
	
	return -1;
}
module_init(pwm_init);

static void __exit pwm_exit(void)
{
	int i;
	
	pwm_dev_cleanup();
	pwm_timer_cleanup();
	
	for (i = 0; i < num_timers; i++) {
		if (pwm_dev[i].user_buff)
			kfree(pwm_dev[i].user_buff);
	}
}
module_exit(pwm_exit);

MODULE_LICENSE("GPL");
MODULE_AUTHOR("Scott Ellis - Jumpnow");
MODULE_DESCRIPTION("PWM example for Gumstix Overo"); 

