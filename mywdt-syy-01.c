/******************************
 * 看门狗设备的完整char驱动
 * 1.char驱动部分支持write和unlocked_ioctl;
 * 2.支持proc文件，可以通过proc文件获得狗的当前工作状态
 * 3.支持中断处理函数，一旦将狗配置为中断模式，则定时器归0时会调用中断处理函数；
 * 4.可以增加锁，确保用户态同一时间只有一个人控制狗
 * 5.用miscdevice注册；
 * 6.在模块的入口中获得并使能狗的时钟；
 * 7.默认采用最大分频值，此时WTCNT寄存器每秒递减3052次计数;最大喂狗的间隔为21s
 * Author : syy
 * Date: 2017-05-12
 * ***************************/
#include <linux/module.h>
#include <linux/fs.h>
#include <linux/time.h>
#include <linux/sched.h>
#include <linux/slab.h>	//kzalloc
#include <linux/uaccess.h>
#include <linux/miscdevice.h>
//proc文件
#include <linux/proc_fs.h>
//中断处理
#include <linux/interrupt.h>
#include <mach/irqs.h>	//中断号
//寄存器访问
#include <linux/ioport.h>
#include <linux/io.h>
//锁
#include <linux/mutex.h>
//时钟 
#include <linux/clk.h>

//常量定义
#define	CNT_IN_1S		3052	//1秒计数器递减的数量
#define DEF_TIME		10		//默认喂狗间隔为10s
#define MAX_TIME		21		//如果PCLK为100MHZ，则最大喂狗间隔为21s
#define MODE_RESET		1		//狗的工作模式为reset/irq
#define MODE_IRQ		2
#define DEF_MODE		MODE_IRQ
//用于设定分频比
#define DEF_PRESC		255		//WTCON[15:8]
#define DEF_DIV			3		//WTCON[4:3]
//定义寄存器的物理基地址和偏移
#define WDT_BASE		0x10060000
#define WDT_SIZE		0x1000	//ioremap的范围
#define WTCON			0x0
#define WTDAT			0x4
#define WTCNT			0x8
#define WTCLRINT		0xC
//定义ioctl命令
#define WDT_TYPE		'W'
#define WDT_ON			_IO(WDT_TYPE, 1)		//使能看门狗
#define WDT_OFF			_IO(WDT_TYPE, 2)		//关闭看门狗
#define WDT_FEED		_IO(WDT_TYPE, 3)		//用当前计数器来喂狗(根据喂狗间隔时间转化)
#define WDT_CHG_MODE	_IOW(WDT_TYPE, 4, int)	//改变狗的工作模式，支持MODE_RESET|MODE_IRQ
#define WDT_CHG_TIME	_IOW(WDT_TYPE, 5, int)	//改变喂狗间隔时间，不能超过最大间隔

//定义变量
static void __iomem *vir_base;	//虚拟基地址
static int wdt_irq;		//存储看门狗中断的中断号
static int wdt_time;	//当前的喂狗间隔时间
static int wdt_mode;	//当前的模式(RESET|IRQ)
static int wdt_cnt;		//根据喂狗间隔转化的计数值(3052*wdt_time)
static struct clk *wdt_clk;	//指向狗的时钟结构体的指针

//proc文件的读函数
static int wdt_proc_read(char *page, char **start, off_t off, 
		int count, int *eof, void *data)
{
	int ret = 0;
	//获得狗的当前状态，包括wdt_time/wdt_mode/wdt_cnt
	//也包括WTCON/WTDAT/WTCNT寄存器的当前值
	ret = sprintf(page, "=======看门狗信息=======\n");
	ret += sprintf(page+ret, "当前喂狗间隔时间为%ds\n", wdt_time);
	ret += sprintf(page+ret, "当前看门狗的模式为%s\n", (wdt_mode==1) ? "MODE_RESET" : "MODE_IRQ");
	ret += sprintf(page+ret, "当前看门狗计数值为%d\n", wdt_cnt);
	ret += sprintf(page+ret, "WTCON: %d\n", readl(vir_base+WTCON));
	ret += sprintf(page+ret, "WTDAT: %d\n", readl(vir_base+WTDAT));
	ret += sprintf(page+ret, "WTCNT: %d\n", readl(vir_base+WTCNT));
	return ret;
}

//中断处理函数
static irqreturn_t wdt_service(int irq, void *dev_id)
{
	struct timeval tval;
	//一旦调用了中断函数，说明看门狗在给定时间内没有喂狗
	//在中断处理函数中通知用户没有喂狗
	//可以用printk输出当前的时间,也可以用蜂鸣器响一声
	//用timeval获得当前的绝对时间输出
	do_gettimeofday(&tval);
	printk("当前的绝对时间为: %ld秒+%ld微秒\n", tval.tv_sec, tval.tv_usec);
	//向WTCLRINT寄存器写任意值，清除中断状态
	writel(1, (vir_base+WTCLRINT));
	return IRQ_HANDLED;
}

//file_operations->write
//支持用户态用echo来控制狗
//$>echo on >/dev/mywdt
//$>echo off >/dev/mywdt
//$>echo feed >/dev/mywdt
//一旦用on使能狗，用户态需要不断使用feed来喂狗，一旦超过wdt_time，会引起中断或reset
static ssize_t wdt_write(struct file *filp, const char __user *buf, size_t count, loff_t *f_ops)
{
	int value;
	char tmp[10] = {0};
	if(copy_from_user(tmp, buf, 4))
		return -EFAULT;
	if(strncmp(tmp, "on", 2) == 0){
		value = readl(vir_base+WTCON);
		if(wdt_mode == 1)
			value |= (0x1<<2);
		else 
			value |= (0x1<<0);
		writel(value, (vir_base+WTCON));
	}else if(strncmp(tmp, "off", 3) == 0){
		value = readl(vir_base+WTCON);
		value &= (0x0<<0) | (0x0<<2);
		writel(value, (vir_base+WTCON));
	}else if(strncmp(tmp, "feed", 4) == 0){
		writel(wdt_cnt, (vir_base+WTCNT));
	}else{
		printk("Only support on|off|feed\n");
		return -1;
	}
	return count;
}

//file_operations->unlocked_ioctl
static long wdt_ioctl(struct file *filp, unsigned int req, unsigned long arg)
{
	int value;
	switch(req){
		case WDT_ON:
			value = readl(vir_base+WTCON);
			if(wdt_mode == 1){
				value |= 0x1<<0;
			}else{
				value |= 0x1<<2;
			}
			writel(value, (vir_base+WTCON));
			break;
		case WDT_OFF:
			value = readl(vir_base+WTCON);
			value &= 0x0<<0 | 0x0<<2;
			break;
		case WDT_FEED:
			writel(wdt_cnt, (vir_base+WTCNT));
			break;
		case WDT_CHG_MODE:
			value = readl(vir_base+WTCON);
			if(arg == 1){
				value |= 0x1<<0;
				value |= 0x0<<2;
			}else{
				value |= 0x1<<2;
				value |= 0x0<<0;
			}
			writel(value, (vir_base+WTCON));
			wdt_mode = arg;
			break;
		case WDT_CHG_TIME:
			writel(arg, (vir_base+WTDAT));
			wdt_time = arg;
			wdt_cnt = wdt_time*3052;
			break;
		default:
			break;
	}
	return 0;
}

static struct file_operations wdt_fops = {
	.owner	= THIS_MODULE,
	.write	= wdt_write,
	.unlocked_ioctl	= wdt_ioctl,
};

static struct miscdevice wdt_misc	= {
	.minor	= MISC_DYNAMIC_MINOR,
	.name	= "mywdt",
	.fops	= &wdt_fops
};

static int __init my_init(void)
{
	int ret, flags, value;
	struct proc_dir_entry *file;
	//1.ioremap获得虚拟基地址
	vir_base = ioremap(WDT_BASE, WDT_SIZE);
	if(!vir_base){
		printk("无法映射地址0x%x\n", WDT_BASE);
		return -EIO;
	}
	//2.获得并使能看门狗
	wdt_clk = clk_get(NULL, "watchdog");
	if(IS_ERR(wdt_clk)){
		printk("无法正确获得狗的时钟\n");
		ret = PTR_ERR(wdt_clk);
		return -1;
	}
	clk_enable(wdt_clk);
	//3.初始化wdt_time等变量
	wdt_time = DEF_TIME;
	wdt_mode = DEF_MODE;
	wdt_cnt = CNT_IN_1S*wdt_time;
	//根据变量的设定，初始化WTCON寄存器
	value = readl(vir_base+WTCON);
	value &= ~(0xffff);
	value |=(DEF_DIV<<3)|(0x1<<5)|(DEF_PRESC<<8);
	writel(value, (vir_base+WTCON));
	//4.找到中断号，然后request_irq
	flags = 0;
	wdt_irq = IRQ_SPI(43);
	ret = request_irq(wdt_irq,
			wdt_service,
			flags,
			"mywdt",
			NULL);
	//5.创建proc文件并关联函数
	file = create_proc_entry("wdtinfo", 0444, NULL);
	if (!file) {
		printk("创建/proc/wdtinfo错误\n");
		return -1;
	}
	file->read_proc = wdt_proc_read;
	//6.注册miscdevice设备
	ret = misc_register(&wdt_misc);
	if(ret){
		printk("注册看门狗设备失败\n");
		return ret;
	}
	printk("注册看门狗设备成功\n");
	return 0;
}

static void __exit my_exit(void)
{
	//注销miscdevice
	misc_deregister(&wdt_misc);
	//删除proc文件
	remove_proc_entry("wdtinfo", NULL);
	//free_irq
	free_irq(wdt_irq, NULL);
	//clk_disable(wdt_clk);
	clk_disable(wdt_clk);
	//iounmap();
	iounmap(vir_base);
}
module_init(my_init);
module_exit(my_exit);
MODULE_AUTHOR("SYY:");
MODULE_LICENSE("GPL");



