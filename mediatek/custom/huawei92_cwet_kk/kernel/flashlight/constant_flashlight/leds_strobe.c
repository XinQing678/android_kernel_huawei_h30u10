// Move code from Android 4.2 base
#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/init.h>
#include <linux/types.h>
#include <linux/wait.h>
#include <linux/slab.h>
#include <linux/fs.h>
#include <linux/sched.h>
#include <linux/poll.h>
#include <linux/device.h>
#include <linux/interrupt.h>
#include <linux/delay.h>
#include <linux/platform_device.h>
#include <linux/cdev.h>
#include <linux/errno.h>
#include <linux/time.h>
#include "kd_flashlight.h"
#include <asm/io.h>
#include <asm/uaccess.h>
#include "kd_camera_hw.h"
#include <cust_gpio_usage.h>
#include <linux/hrtimer.h>
#include <linux/ktime.h>
#include <linux/xlog.h>
#include <linux/version.h>
#include <mach/upmu_common.h>
#include <mach/mt6333.h>
/* Add the LM3642 fl driver*/
#include <linux/hardware_self_adapt.h>

/******************************************************************************
 * Debug configuration
******************************************************************************/
// availible parameter
// ANDROID_LOG_ASSERT
// ANDROID_LOG_ERROR
// ANDROID_LOG_WARNING
// ANDROID_LOG_INFO
// ANDROID_LOG_DEBUG
// ANDROID_LOG_VERBOSE
#define TAG_NAME "leds_strobe.c"
#define PK_DBG_NONE(fmt, arg...)    do {} while (0)
#define PK_DBG_FUNC(fmt, arg...)    xlog_printk(ANDROID_LOG_DEBUG  , TAG_NAME, KERN_INFO  "%s: " fmt, __FUNCTION__ ,##arg)
#define PK_WARN(fmt, arg...)        xlog_printk(ANDROID_LOG_WARNING, TAG_NAME, KERN_WARNING  "%s: " fmt, __FUNCTION__ ,##arg)
#define PK_NOTICE(fmt, arg...)      xlog_printk(ANDROID_LOG_DEBUG  , TAG_NAME, KERN_NOTICE  "%s: " fmt, __FUNCTION__ ,##arg)
#define PK_INFO(fmt, arg...)        xlog_printk(ANDROID_LOG_INFO   , TAG_NAME, KERN_INFO  "%s: " fmt, __FUNCTION__ ,##arg)
#define PK_TRC_FUNC(f)              xlog_printk(ANDROID_LOG_DEBUG  , TAG_NAME,  "<%s>\n", __FUNCTION__);
#define PK_TRC_VERBOSE(fmt, arg...) xlog_printk(ANDROID_LOG_VERBOSE, TAG_NAME,  fmt, ##arg)
#define PK_ERROR(fmt, arg...)       xlog_printk(ANDROID_LOG_ERROR  , TAG_NAME, KERN_ERR "%s: " fmt, __FUNCTION__ ,##arg)


#define DEBUG_LEDS_STROBE
#ifdef  DEBUG_LEDS_STROBE
	#define PK_DBG PK_DBG_FUNC
	#define PK_VER PK_TRC_VERBOSE
	#define PK_ERR PK_ERROR
#else
	#define PK_DBG(a,...)
	#define PK_VER(a,...)
	#define PK_ERR(a,...)
#endif

/******************************************************************************
 * local variables
******************************************************************************/
static DEFINE_SPINLOCK(g_strobeSMPLock); /* cotta-- SMP proection */
static struct work_struct workTimeOut;
static struct work_struct workWDReset;
static int g_duty=-1;
static int g_timeOutTimeMs=0;
                       //0, 1,  2,  3,  4,  5,  6,  7,  8,  9, 10, 11, 12, 13, 14, 15
static int dimLevel[] = {-1,-1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1};
static int flashCur[] = { 1, 2,  3,  4,  5,  6,  7,  8,  9, 10, 11, 12, 13, 14};
static int torchEn [] = { 1, 1,  1,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0};

static int g_reg;
static int g_val;
static u32 strobe_Res = 0;
/*****************************************************************************
Functions
*****************************************************************************/
static void work_timeOutFunc(struct work_struct *data);
static void work_WDResetFunc(struct work_struct *data);
extern kal_uint32 mt6333_config_interface (kal_uint8 RegNum, kal_uint8 val, kal_uint8 MASK, kal_uint8 SHIFT);
extern kal_uint32 mt6333_read_interface (kal_uint8 RegNum, kal_uint8 *val, kal_uint8 MASK, kal_uint8 SHIFT);

void mt6333_set_rg_chrwdt_en(kal_uint8 val); //enable
extern void mt6333_set_rg_chrwdt_wr(kal_uint8 val); //reset
extern void mt6333_set_rg_chrwdt_td(kal_uint8 val); //wdog time set

/* Add the LM3642 fl driver*/
/* before LUT[7] is Torch mode, after is Flashmode, so Torch index must be 
    less than or equal to TORCH_MAX_INDEX, the max current is 375mA.
    So, if need flash current less than 375mA, choose index less than 7(including 7),
   if need flash current greater than 375mA, choose index larger than 7.
   */
#define MAX_LEVELS 32
#define TORCH_MAX_INDEX    7
static const MUINT32 strobeLevelLUT[MAX_LEVELS] =
    {0, 1, 2, 3, 4, 5, 6, 7,/* <-Torch, Flash->*/ 4, 4, 5, 5, 6, 6 , 7, 7 , 8 , 8, 9 , 9 , 10 , 10 , 11 , 11 , 12 , 12, 13 , 13 , 14 , 14 , 15 , 15};
extern int FLASHLIGHT_read_byte(u8 addr, u8 *data);
extern int FLASHLIGHT_write_byte(u8 addr, u8 data);

static void setReg(int reg, int val)
{
    mt6333_config_interface(reg, val, 0xff, 0);
}
static int getReg(int reg)
{
    kal_uint8 valTemp;
    mt6333_read_interface(reg, &valTemp, 0xff, 0);
    return valTemp;
}
static void setRegEx(int reg, int val, int mask, int shift)
{
    int v;
    v=getReg(reg);
    v &= ~(mask << shift);
    v |= (val << shift);
    setReg(reg, v);
}

static int FL_preOn(void)
{
    hw_product_type boardType = get_hardware_product_version();
    PK_DBG("flboardType=%d\n",boardType);

    if(((boardType & HW_VER_MAIN_MASK) == HW_G750_VER))
    {
        if((boardType & HW_VER_SUB_MASK) == HW_VER_SUB_VA)
        {   // MT6632 Mode
            PK_DBG("FL_preOn");
            setRegEx(0xF3,0x1,0x1,5); //work around for chip issue
            setRegEx(0xF7,0x1,0x1,5); //work around for chip issue
            if(dimLevel[g_duty]>=0)
            {
                setRegEx(0x32,0x1,0x1,4);  //torch mode
                setRegEx(0x35,flashCur[g_duty],0xf,0); //current
                setRegEx(0x33,dimLevel[g_duty],0x1f,0); //dimming
                //setRegEx(0x34,0,0xff,0); //dim freq
                setRegEx(0x34,2,0xff,0); //dim freq
                setRegEx(0x32,1,0x1,0); //dim en
                setRegEx(0x32,1,0x1,6); //dim charger in en
                setRegEx(0xe2,0x40,0xff,0); //workaround, must call
                setRegEx(0xeb,0x40,0xff,0); //workaround, must call
                setRegEx(0x16,0x1,0x1,0); //power source on
            }
            else if(torchEn[g_duty]==1)
            {
                setRegEx(0x32,0x1,0x1,4);  //torch mode
                setRegEx(0x35,flashCur[g_duty],0xf,0); //current
                setRegEx(0x32,0,0x1,0); //dim en
                setRegEx(0xe2,0x40,0xff,0); //workaround, must call
                setRegEx(0xeb,0x40,0xff,0); //workaround, must call
                setRegEx(0x16,0x1,0x1,0); //power source on
            }
            else
            {
                setRegEx(0x32,0x0,0x1,4);  //torch mode=0, flash mode
                setRegEx(0x35,flashCur[g_duty],0xf,0); //current
                setRegEx(0x37,0x3,0x3,6); //time out, 800ms
                setRegEx(0xe2,0x40,0xff,0); //workaround, must call
                setRegEx(0xeb,0x40,0xff,0); //workaround, must call
                setRegEx(0x16,0x1,0x1,0); //power source on
            }
        }
        else
        {   // LM3642 Mode
            // do nothing;
         }
    }
    else  // Other Product
    {
        // do nothing;
    }
    return 0;

}

static int FL_Enable(void)
{
    PK_DBG("FL_Enable+");
    hw_product_type boardType = get_hardware_product_version();
    PK_DBG("flboardType=%d\n",boardType);

    if(((boardType & HW_VER_MAIN_MASK) == HW_G750_VER))
    {
        if((boardType & HW_VER_SUB_MASK) == HW_VER_SUB_VA)
        {
            // MT6632 Mode
            setRegEx(0x31,0x1,0x1,0); //flash en
            PK_DBG("MT6322 FL_Enable-");
        }
        else
        {   // LM3642 Mode
            int value = 0;
            u8 regValue0B = 0;
            //read 0x0B to release the lock in case of driver ic entering standby mode.
            FLASHLIGHT_read_byte(0x0B, &regValue0B);
            PK_DBG(" FL_Enable :reg 0x0B :0x%x, LINE:%d\n", regValue0B, __LINE__);
            if (g_duty <= TORCH_MAX_INDEX )//Torch Mode
            {
                PK_DBG("LM3642 FL_Enable : Torch Mode, duty :%d, LINE:%d\n", g_duty, __LINE__);
                value = (strobeLevelLUT[g_duty] << 4) & 0xF0 ;
                FLASHLIGHT_write_byte(0x09, value);//current setting
                FLASHLIGHT_write_byte(0x0A, 0x02);//torch mode
            }
            else//Flash Mode
            {
                PK_DBG("LM3642 FL_Enable : Flash Mode, duty :%d, LINE:%d\n", g_duty, __LINE__);
                value = strobeLevelLUT[g_duty]  & 0x0F;
                FLASHLIGHT_write_byte(0x09, value);//current setting
                FLASHLIGHT_write_byte(0x08, 0x07);//timeout:800ms
                FLASHLIGHT_write_byte(0x0A, 0x03);//flash mode
             }
        }
    }
    else  // Other Product
    {
        // do nothing;
    }
    return 0;
}

static int FL_Disable(void)
{
    hw_product_type boardType = get_hardware_product_version();
    PK_DBG("flboardType=%d\n",boardType);

    if(((boardType & HW_VER_MAIN_MASK) == HW_G750_VER))
    {
        if(((boardType & HW_VER_SUB_MASK) == HW_VER_SUB_VA))
        {   
            // MT6632 Mode
            setRegEx(0x31,0x0,0x1,0);
            setRegEx(0x16,0x0,0x1,0);
            PK_DBG("MT6322 FL_Disable line=%d\n",__LINE__);
        }
        else 
        {   // LM3642 Mode
            u8 regValue0B = 0;
            //read 0x0B to release the lock in case of driver ic entering standby mode.
            FLASHLIGHT_read_byte(0x0B, &regValue0B);
            PK_DBG(" FL_Disable :reg 0x0B :0x%x, LINE:%d\n", regValue0B, __LINE__);
            FLASHLIGHT_write_byte(0x0A, 0x00);//turn of flash
            PK_DBG("LM3642 FL_Disable line=%d\n",__LINE__);
        }
    }
    else  // Other Product
    {
        // do nothing;
    }

    return 0;
}

static int FL_dim_duty(kal_uint32 duty)
{
    hw_product_type boardType = get_hardware_product_version();
    PK_DBG("flboardType=%d\n",boardType);

    if(((boardType & HW_VER_MAIN_MASK) == HW_G750_VER))
    {
        if((boardType & HW_VER_SUB_MASK) == HW_VER_SUB_VA)
        {   // MT6632 Mode
            PK_DBG("MT6322 FL_dim_duty line=%d\n",__LINE__);
            g_duty = duty;
        }
        else  
        {   // LM3642 Mode
            PK_DBG(" LM3642 FL_dim_duty , duty:%d, line=%d\n", duty, __LINE__);
            if (duty > (MAX_LEVELS -1)) //Max duty is 31
            {
                duty = (MAX_LEVELS -1);
            }
            g_duty =  duty;
            return 0;
        }
    }
    else  // Other Product
    {
        // do nothing;
    }
    return 0;
}
static int FL_Init(void)
{
    PK_DBG(" FL_Init line=%d\n",__LINE__);
    INIT_WORK(&workTimeOut, work_timeOutFunc);
    return 0;
}
static int FL_Uninit(void)
{
	FL_Disable();
    return 0;
}

/*****************************************************************************
User interface
*****************************************************************************/
static struct hrtimer g_timeOutTimer;
static struct hrtimer g_WDResetTimer;
int g_b1stInit=1;
int g_bOpen = 0;
static void work_timeOutFunc(struct work_struct *data)
{
    FL_Disable();
    PK_DBG("ledTimeOut_callback\n");
}

static void work_WDResetFunc(struct work_struct *data)
{
	ktime_t ktime;
	mt6333_set_rg_chrwdt_wr(1); // write 1 to kick chr wdt
	if(g_bOpen==1)
	{		
		ktime = ktime_set( 0, 1000*1000000 );//1s
		hrtimer_start( &g_WDResetTimer, ktime, HRTIMER_MODE_REL );
	}
}

static enum hrtimer_restart ledTimeOutCallback(struct hrtimer *timer)
{
    schedule_work(&workTimeOut);
    return HRTIMER_NORESTART;
}
static enum hrtimer_restart ledWDResetCallback(struct hrtimer *timer)
{
    schedule_work(&workWDReset);
    return HRTIMER_NORESTART;
}


static void timerInit(void)
{
	ktime_t ktime;


	//mt6333_set_rg_chrwdt_en(0);
    mt6333_set_rg_chrwdt_wr(1); // write 1 to kick chr wdt
    //mt6333_set_rg_chrwdt_td(0); //4 sec
    //mt6333_set_rg_chrwdt_en(1);

    //mt6333_set_rg_chrwdt_en(0);

	if(g_b1stInit==1)
	{
		g_b1stInit=0;
	    INIT_WORK(&workWDReset, work_WDResetFunc);
	    hrtimer_init( &g_WDResetTimer, CLOCK_MONOTONIC, HRTIMER_MODE_REL );
	    g_WDResetTimer.function=ledWDResetCallback;

	  	INIT_WORK(&workTimeOut, work_timeOutFunc);
		g_timeOutTimeMs=1000; //1s
		hrtimer_init( &g_timeOutTimer, CLOCK_MONOTONIC, HRTIMER_MODE_REL );
		g_timeOutTimer.function=ledTimeOutCallback;
	}
	ktime = ktime_set( 0, 1000*1000000 );//1s
	hrtimer_start( &g_WDResetTimer, ktime, HRTIMER_MODE_REL );

}


static int constant_flashlight_ioctl(MUINT32 cmd, MUINT32 arg)
{
	int temp;
	int i4RetValue = 0;
	int ior_shift;
	int iow_shift;
	int iowr_shift;
	kal_uint8 valTemp;
	ior_shift = cmd - (_IOR(FLASHLIGHT_MAGIC,0, int));
	iow_shift = cmd - (_IOW(FLASHLIGHT_MAGIC,0, int));
	iowr_shift = cmd - (_IOWR(FLASHLIGHT_MAGIC,0, int));
	PK_DBG("constant_flashlight_ioctl() line=%d ior_shift=%d, iow_shift=%d iowr_shift=%d arg=%d\n",__LINE__, ior_shift, iow_shift, iowr_shift, arg);
    switch(cmd)
    {

		case FLASH_IOC_SET_TIME_OUT_TIME_MS:
			PK_DBG("FLASH_IOC_SET_TIME_OUT_TIME_MS: %d\n",arg);
			g_timeOutTimeMs=arg;
		break;


    	case FLASH_IOC_SET_DUTY :
    		PK_DBG("FLASHLIGHT_DUTY: %d\n",arg);
    		FL_dim_duty(arg);
    		break;


    	case FLASH_IOC_SET_STEP:
    		PK_DBG("FLASH_IOC_SET_STEP: %d\n",arg);

    		break;

    	case FLASH_IOC_SET_ONOFF :
    		PK_DBG("FLASHLIGHT_ONOFF: %d\n",arg);
    		if(arg==1)
    		{
				if(g_timeOutTimeMs!=0)
	            {
	            	ktime_t ktime;
					ktime = ktime_set( 0, g_timeOutTimeMs*1000000 );
					hrtimer_start( &g_timeOutTimer, ktime, HRTIMER_MODE_REL );
	            }
    			FL_Enable();
    		}
    		else
    		{
    			FL_Disable();
				hrtimer_cancel( &g_timeOutTimer );
    		}
    		break;

    	case FLASH_IOC_PRE_ON:
    		PK_DBG("FLASH_IOC_PRE_ON\n");
			FL_preOn();
    		break;
    	case FLASH_IOC_GET_PRE_ON_TIME_MS:
    		PK_DBG("FLASH_IOC_GET_PRE_ON_TIME_MS: %d\n",arg);
    		temp=13;
    		if(copy_to_user((void __user *) arg , (void*)&temp , 4))
            {
                PK_DBG(" ioctl copy to user failed\n");
                return -1;
            }
    		break;

        case FLASH_IOC_SET_REG_ADR:
            PK_DBG("FLASH_IOC_SET_REG_ADR: %d\n",arg);
            g_reg = arg;
            break;
        case FLASH_IOC_SET_REG_VAL:
            PK_DBG("FLASH_IOC_SET_REG_VAL: %d\n",arg);
            g_val = arg;
            break;
        case FLASH_IOC_SET_REG:
            PK_DBG("FLASH_IOC_SET_REG: %d %d\n",g_reg, g_val);
            mt6333_config_interface(g_reg, g_val, 0xff, 0);
            break;

        case FLASH_IOC_GET_REG:
            PK_DBG("FLASH_IOC_GET_REG: %d\n",arg);
            mt6333_read_interface(arg, &valTemp, 0xff, 0);
            i4RetValue = valTemp;
            PK_DBG("FLASH_IOC_GET_REG: v=%d\n",valTemp);
            break;

        default :
    		PK_DBG(" No such command \n");
    		i4RetValue = -EPERM;
    		break;
    }
    return i4RetValue;
}




static int constant_flashlight_open(void *pArg)
{
    int i4RetValue = 0;
    PK_DBG("constant_flashlight_open line=%d\n", __LINE__);

	if (0 == strobe_Res)
	{
	    FL_Init();
		timerInit();
	}
	PK_DBG("constant_flashlight_open line=%d\n", __LINE__);
	spin_lock_irq(&g_strobeSMPLock);
	g_bOpen=1;

    if(strobe_Res)
    {
        PK_ERR(" busy!\n");
        i4RetValue = -EBUSY;
    }
    else
    {
        strobe_Res += 1;
    }


    spin_unlock_irq(&g_strobeSMPLock);
    PK_DBG("constant_flashlight_open line=%d\n", __LINE__);

    return i4RetValue;

}


static int constant_flashlight_release(void *pArg)
{
    PK_DBG(" constant_flashlight_release\n");

    if (strobe_Res)
    {
        spin_lock_irq(&g_strobeSMPLock);
        g_bOpen=0;
        strobe_Res = 0;


        spin_unlock_irq(&g_strobeSMPLock);
    	FL_Uninit();
    }
    PK_DBG(" Done\n");
    return 0;
}


static FLASHLIGHT_FUNCTION_STRUCT	constantFlashlightFunc=
{
	constant_flashlight_open,
	constant_flashlight_release,
	constant_flashlight_ioctl
};


MUINT32 constantFlashlightInit(PFLASHLIGHT_FUNCTION_STRUCT *pfFunc)
{
    if (pfFunc != NULL)
    {
        *pfFunc = &constantFlashlightFunc;
    }
    return 0;
}



/* LED flash control for high current capture mode*/
ssize_t strobe_VDIrq(void)
{

    return 0;
}

EXPORT_SYMBOL(strobe_VDIrq);



