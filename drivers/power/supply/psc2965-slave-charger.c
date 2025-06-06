// SPDX-License-Identifier: GPL-2.0:
// Copyright (c) 2021 unisoc.

/*
 * Driver for the psc2965 charger.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 as
 * published by the Free Software Foundation.
 */
#include <linux/alarmtimer.h>
#include <linux/gpio/consumer.h>
#include <linux/interrupt.h>
#include <linux/i2c.h>
#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/of_gpio.h>
#include <linux/of_platform.h>
#include <linux/platform_device.h>
#include <linux/power_supply.h>
#include <linux/power/charger-manager.h>
#include <linux/power/sprd_battery_info.h>
#include <linux/regmap.h>
#include <linux/regulator/driver.h>
#include <linux/regulator/machine.h>
#include <linux/sysfs.h>
#include <linux/usb/phy.h>
#include <linux/pm_wakeup.h>
#include <uapi/linux/usb/charger.h>
#include "prj/prj_config.h"

#define PSC2965_REG_0				0x0
#define PSC2965_REG_1				0x1
#define PSC2965_REG_2				0x2
#define PSC2965_REG_3				0x3
#define PSC2965_REG_4				0x4
#define PSC2965_REG_5				0x5
#define PSC2965_REG_6				0x6
#define PSC2965_REG_7				0x7
#define PSC2965_REG_8				0x8
#define PSC2965_REG_9				0x9
#define PSC2965_REG_A				0xa
#define PSC2965_REG_B				0xb
#define PSC2965_REG_C				0xc
#define PSC2965_REG_D				0xd
#define PSC2965_REG_NUM			13

#define PSC2965_BATTERY_NAME			"sc27xx-fgu"
#define BIT_DP_DM_BC_ENB			BIT(0)
#define PSC2965_OTG_ALARM_TIMER_MS		15000

#define	PSC2965_REG_IINLIM_BASE			100

#define PSC2965_REG_ICHG_LSB			60

#define PSC2965_REG_ICHG_MASK			GENMASK(5, 0)

#define PSC2965_REG_CHG_MASK			GENMASK(4, 4)
#define PSC2965_REG_CHG_SHIFT			4


#define PSC2965_REG_RESET_MASK			GENMASK(6, 6)

#define PSC2965_REG_OTG_MASK			GENMASK(5, 5)
#define PSC2965_REG_BOOST_FAULT_MASK		GENMASK(6, 6)

#define PSC2965_REG_WATCHDOG_MASK		GENMASK(6, 6)

#define PSC2965_REG_WATCHDOG_TIMER_MASK		GENMASK(5, 4)
#define PSC2965_REG_WATCHDOG_TIMER_SHIFT	4

#define PSC2965_REG_TERMINAL_VOLTAGE_MASK	GENMASK(7, 3)
#define PSC2965_REG_TERMINAL_VOLTAGE_SHIFT	3

#define PSC2965_REG_TERMINAL_CUR_MASK		GENMASK(3, 0)

#define PSC2965_REG_VINDPM_VOL            4300
#define PSC2965_REG_VINDPM_VOLTAGE_MASK		GENMASK(3, 0)
#define PSC2965_REG_OVP_MASK			GENMASK(7, 6)
#define PSC2965_REG_OVP_SHIFT			6

#define PSC2965_REG_EN_HIZ_MASK			GENMASK(7, 7)
#define PSC2965_REG_EN_HIZ_SHIFT		7

#define PSC2965_REG_EN_ITERM_MASK			GENMASK(7, 7)

#define PSC2965_REG_LIMIT_CURRENT_MASK		GENMASK(4, 0)

#define PSC2965_REG_0x0D_MASK		      GENMASK(7, 6)
#define PSC2965_REG_0x0D_TOFF_MASK		GENMASK(5, 4)
#define PSC2965_REG_ADD_R_MASK        GENMASK(3, 2)

#define PSC2965_REG_0x0C_MASK		GENMASK(6, 6)

#define PSC2965_REG_NTC_DIS_MASK		GENMASK(4, 4)

#define PSC2965_DISABLE_PIN_MASK		BIT(0)
#define PSC2965_DISABLE_PIN_MASK_2721		BIT(15)

#define PSC2965_OTG_VALID_MS			500
#define PSC2965_FEED_WATCHDOG_VALID_MS		50
#define PSC2965_OTG_RETRY_TIMES			10
#define PSC2965_LIMIT_CURRENT_MAX		3200000
#define PSC2965_LIMIT_CURRENT_OFFSET		100000
#define PSC2965_REG_IINDPM_LSB			100

#define PSC2965_ROLE_MASTER_DEFAULT		1
#define PSC2965_ROLE_SLAVE			2

#define PSC2965_FCHG_OVP_6V			6000
#define PSC2965_FCHG_OVP_9V			9000
#define PSC2965_FCHG_OVP_14V			14000
#define PSC2965_FAST_CHARGER_VOLTAGE_MAX	10500000
#define PSC2965_NORMAL_CHARGER_VOLTAGE_MAX	6500000

#define PSC2965_WAKE_UP_MS			1000
#define PSC2965_CURRENT_WORK_MS			msecs_to_jiffies(100)

#define PSC2965_PD_HARD_RESET_MS		500
#define PSC2965_PD_RECONNECT_MS			3000

struct psc2965_charger_sysfs {
	char *name;
	struct attribute_group attr_g;
	struct device_attribute attr_psc2965_dump_reg;
	struct device_attribute attr_psc2965_lookup_reg;
	struct device_attribute attr_psc2965_sel_reg_id;
	struct device_attribute attr_psc2965_reg_val;
	struct attribute *attrs[5];

	struct psc2965_charger_info *info;
};

struct psc2965_charge_current {
	int sdp_limit;
	int sdp_cur;
	int dcp_limit;
	int dcp_cur;
	int cdp_limit;
	int cdp_cur;
	int unknown_limit;
	int unknown_cur;
	int fchg_limit;
	int fchg_cur;
};

struct psc2965_charger_info {
	struct i2c_client *client;
	struct device *dev;
	struct usb_phy *usb_phy;
	struct notifier_block usb_notify;
	struct power_supply *psy_usb;
	struct psc2965_charge_current cur;
	struct work_struct work;
	struct mutex lock;
	struct delayed_work otg_work;
	struct delayed_work wdt_work;
	struct delayed_work cur_work;
	struct regmap *pmic;
	struct gpio_desc *gpiod;
	struct extcon_dev *typec_extcon;
	struct notifier_block typec_extcon_nb;
	struct delayed_work typec_extcon_work;
	struct extcon_dev *pd_extcon;
	struct notifier_block pd_extcon_nb;
	struct delayed_work pd_hard_reset_work;
	bool pd_hard_reset;
	bool pd_extcon_enable;
	bool typec_online;
	struct alarm otg_timer;
	struct psc2965_charger_sysfs *sysfs;
	u32 charger_detect;
	u32 charger_pd;
	u32 charger_pd_mask;
	u32 limit;
	u32 new_charge_limit_cur;
	u32 current_charge_limit_cur;
	u32 new_input_limit_cur;
	u32 current_input_limit_cur;
	u32 last_limit_cur;
	u32 actual_limit_cur;
	u32 role;
	bool charging;
	bool need_disable_Q1;
	int termination_cur;
	bool otg_enable;
	unsigned int irq_gpio;
	bool is_wireless_charge;

	int reg_id;
	bool disable_power_path;
};

struct psc2965_charger_reg_tab {
	int id;
	u32 addr;
	char *name;
};

static struct psc2965_charger_reg_tab reg_tab[PSC2965_REG_NUM + 1] = {
	{0, PSC2965_REG_0, "EN_HIZ/EN_ICHG_MON/IINDPM"},
	{1, PSC2965_REG_1, "PFM _DIS/WD_RST/OTG_CONFIG/CHG_CONFIG/SYS_Min/Min_VBAT_SEL"},
	{2, PSC2965_REG_2, "BOOST_LIM/Q1_FULLON/ICHG"},
	{3, PSC2965_REG_3, "IPRECHG/ITERM"},
	{4, PSC2965_REG_4, "VREG/TOPOFF_TIMER/VRECHG"},
	{5, PSC2965_REG_5, "EN_TERM/WATCHDOG/EN_TIMER/CHG_TIMER/TREG/JEITA_ISET"},
	{6, PSC2965_REG_6, "OVP/BOOSTV/VINDPM"},
	{7, PSC2965_REG_7, "IINDET_EN/TMR2X_EN/BATFET_DIS/JEITA_VSET/BATFET_DLY/BATFET_RST_EN/VDPM_BAT_TRACK"},
	{8, PSC2965_REG_8, "VBUS_STAT/CHRG_STAT/PG_STAT/THERM_STAT/VSYS_STAT"},
	{9, PSC2965_REG_9, "WATCHDOG_FAULT/BOOST_FAULT/CHRG_FAULT/BAT_FAULT/NTC_FAULT"},
	{10, PSC2965_REG_A, "VBUS_GD/VINDPM_STAT/IINDPM_STAT/TOPOFF_ACTIVE/ACOV_STAT/VINDPM_INT_ MASK/IINDPM_INT_ MASK"},
	{11, PSC2965_REG_B, "REG_RST/PN/DEV_REV"},
	{12, PSC2965_REG_D, "CHARGE_CURRENT_LIMIT/ADD_R/THERMAL_DIS"},
	{13, 0, "null"},
};

static void power_path_control(struct psc2965_charger_info *info)
{
	struct device_node *cmdline_node;
	const char *cmd_line;
	int ret;
	char *match;
	char result[5];

	cmdline_node = of_find_node_by_path("/chosen");
	ret = of_property_read_string(cmdline_node, "bootargs", &cmd_line);
	if (ret) {
		info->disable_power_path = false;
		return;
	}

	if (strncmp(cmd_line, "charger", strlen("charger")) == 0)
		info->disable_power_path = true;

	match = strstr(cmd_line, "androidboot.mode=");
	if (match) {
		memcpy(result, (match + strlen("androidboot.mode=")),
			sizeof(result) - 1);
		if ((!strcmp(result, "cali")) || (!strcmp(result, "auto")))
			info->disable_power_path = true;
	}
}

static int
psc2965_charger_set_limit_current(struct psc2965_charger_info *info,
				  u32 limit_cur);

static bool psc2965_charger_is_bat_present(struct psc2965_charger_info *info)
{
	struct power_supply *psy;
	union power_supply_propval val;
	bool present = false;
	int ret;

	psy = power_supply_get_by_name(PSC2965_BATTERY_NAME);
	if (!psy) {
		dev_err(info->dev, "Failed to get psy of sc27xx_fgu\n");
		return present;
	}

	val.intval = 0;
	ret = power_supply_get_property(psy, POWER_SUPPLY_PROP_PRESENT,
					&val);
	if (ret == 0 && val.intval)
		present = true;
	power_supply_put(psy);

	if (ret)
		dev_err(info->dev,
			"Failed to get property of present:%d\n", ret);

	return present;
}

static int psc2965_charger_is_fgu_present(struct psc2965_charger_info *info)
{
	struct power_supply *psy;

	psy = power_supply_get_by_name(PSC2965_BATTERY_NAME);
	if (!psy) {
		dev_err(info->dev, "Failed to find psy of sc27xx_fgu\n");
		return -ENODEV;
	}
	power_supply_put(psy);

	return 0;
}

static int psc2965_read(struct psc2965_charger_info *info, u8 reg, u8 *data)
{
	int ret;

	ret = i2c_smbus_read_byte_data(info->client, reg);
	if (ret < 0)
		return ret;

	*data = ret;
	return 0;
}

static int psc2965_write(struct psc2965_charger_info *info, u8 reg, u8 data)
{
	return i2c_smbus_write_byte_data(info->client, reg, data);
}

static int psc2965_update_bits(struct psc2965_charger_info *info, u8 reg,
			       u8 mask, u8 data)
{
	u8 v;
	int ret;

	ret = psc2965_read(info, reg, &v);
	if (ret < 0)
		return ret;

	v &= ~mask;
	v |= (data & mask);

	return psc2965_write(info, reg, v);
}

static int
psc2965_charger_set_vindpm(struct psc2965_charger_info *info, u32 vol)
{
	u8 reg_val;

	if (vol < 4000)
		reg_val = 0x0;
	else if (vol > 5500)
		reg_val = 0x0f;
	else
		reg_val = (vol - 4000) / 100;

	return psc2965_update_bits(info, PSC2965_REG_6,
				   PSC2965_REG_VINDPM_VOLTAGE_MASK, reg_val);
}


static int
psc2965_charger_set_ovp(struct psc2965_charger_info *info, u32 vol)
{
	u8 reg_val;
	int ret = 0;

	if (vol <= 5500)
		reg_val = 0x0;
	else if (vol > 5500 && vol <=7000)
		reg_val = 0x01;
	else if (vol >7000 && vol <=11500)
		reg_val = 0x02;
	else
		reg_val = 0x03;

  if(reg_val == 0x02)
  { 
    ret = psc2965_update_bits(info, PSC2965_REG_D, PSC2965_REG_0x0D_TOFF_MASK, 0x02<<4);
    if (ret) {
      dev_err(info->dev, "set 0x0D failed\n");
      return ret;
    }
  }
  else
  { 
    ret = psc2965_update_bits(info, PSC2965_REG_D, PSC2965_REG_0x0D_TOFF_MASK, 0x00<<4);
    if (ret) {
      dev_err(info->dev, "set 0x0D failed\n");
      return ret;
    }
  }

	return psc2965_update_bits(info, PSC2965_REG_6,
				   PSC2965_REG_OVP_MASK,
				   reg_val << PSC2965_REG_OVP_SHIFT);
}

static int psc2965_charger_set_termina_vol(struct psc2965_charger_info *info, u32 vol)
{
  u8 reg_val;

  if (vol < 3984)
    vol = 3984;
  else if (vol >= 4450)
    vol = 4464;

  reg_val = (vol - 3856) / 32;
  
  //this is for 4200, improve 4176 to 4208
  if(reg_val == 10)
    reg_val = 11;

  //this is for 4350, improve 4336 to 4368
  if(reg_val == 15)
    reg_val = 16;

  return psc2965_update_bits(info, PSC2965_REG_4, PSC2965_REG_TERMINAL_VOLTAGE_MASK, reg_val << PSC2965_REG_TERMINAL_VOLTAGE_SHIFT);
}

static int
psc2965_charger_set_termina_cur(struct psc2965_charger_info *info, u32 cur)
{
	u8 reg_val;

	if (cur <= 100)
		reg_val = 0x0;
	else if (cur >= 800)
		reg_val = 0x8;
	else
		reg_val = (cur - 100) / 100;

	return psc2965_update_bits(info, PSC2965_REG_3,
				   PSC2965_REG_TERMINAL_CUR_MASK,
				   reg_val);
}

static int psc2965_charger_hw_init(struct psc2965_charger_info *info)
{
	struct sprd_battery_info bat_info = {};
	int voltage_max_microvolt, termination_cur;
	int ret;

	ret = sprd_battery_get_battery_info(info->psy_usb, &bat_info);
	if (ret) {
		dev_warn(info->dev, "no battery information is supplied\n");

		info->cur.sdp_limit = 500000;
		info->cur.sdp_cur = 500000;
		info->cur.dcp_limit = 1500000;
		info->cur.dcp_cur = 1500000;
		info->cur.cdp_limit = 1000000;
		info->cur.cdp_cur = 1000000;
		info->cur.unknown_limit = 500000;
		info->cur.unknown_cur = 500000;

		/*
		 * If no battery information is supplied, we should set
		 * default charge termination current to 120 mA, and default
		 * charge termination voltage to 4.44V.
		 */
		voltage_max_microvolt = 4440;
		termination_cur = 120;
		info->termination_cur = termination_cur;
	} else {
		info->cur.sdp_limit = bat_info.cur.sdp_limit;
		info->cur.sdp_cur = bat_info.cur.sdp_cur;
		info->cur.dcp_limit = bat_info.cur.dcp_limit;
		info->cur.dcp_cur = bat_info.cur.dcp_cur;
		info->cur.cdp_limit = bat_info.cur.cdp_limit;
		info->cur.cdp_cur = bat_info.cur.cdp_cur;
		info->cur.unknown_limit = bat_info.cur.unknown_limit;
		info->cur.unknown_cur = bat_info.cur.unknown_cur;
		info->cur.fchg_limit = bat_info.cur.fchg_limit;
		info->cur.fchg_cur = bat_info.cur.fchg_cur;

		voltage_max_microvolt = bat_info.constant_charge_voltage_max_uv / 1000;
		termination_cur = bat_info.charge_term_current_ua / 1000;
		info->termination_cur = termination_cur;
		sprd_battery_put_battery_info(info->psy_usb, &bat_info);
	}

	ret = psc2965_update_bits(info, PSC2965_REG_B,
				  PSC2965_REG_RESET_MASK,
				  PSC2965_REG_RESET_MASK);
	if (ret) {
		dev_err(info->dev, "reset psc2965 failed\n");
		return ret;
	}

	 if (info->role == PSC2965_ROLE_SLAVE) {
		ret = psc2965_charger_set_ovp(info, PSC2965_FCHG_OVP_9V);
		if (ret) {
			dev_err(info->dev, "set psc2965 slave ovp failed\n");
			return ret;
		}
	}

	ret = psc2965_charger_set_vindpm(info, PSC2965_REG_VINDPM_VOL);
	if (ret) {
		dev_err(info->dev, "set psc2965 vindpm vol failed\n");
		return ret;
	}

   ret = psc2965_update_bits(info, PSC2965_REG_D,
				  PSC2965_REG_0x0D_MASK,
				  0x03<< 6 );
	if (ret) {
		dev_err(info->dev, "set 0x0D failed\n");
		return ret;
	}
	
  ret = psc2965_update_bits(info, PSC2965_REG_D,
				  PSC2965_REG_0x0D_TOFF_MASK,
				  0x00<< 4 );
	if (ret) {
		dev_err(info->dev, "set 0x0D failed\n");
		return ret;
	}

  if(voltage_max_microvolt >= 4450)
  {
    ret = psc2965_update_bits(info, PSC2965_REG_D, PSC2965_REG_ADD_R_MASK, 0x02 << 2);
  	if (ret) {
  		dev_err(info->dev, "set 0x0D 0x02 failed\n");
  		return ret;
  	}
  }
  else
  {
    ret = psc2965_update_bits(info, PSC2965_REG_D, PSC2965_REG_ADD_R_MASK, 0x00 << 2);
    if (ret) {
  		dev_err(info->dev, "set 0x0D 0x00 failed\n");
  		return ret;
    }
  }
	
	//  the system takes  over charge termination control ,nedd disable the PSC2965  charge termination fucntion.
	ret = psc2965_update_bits(info, PSC2965_REG_5, PSC2965_REG_EN_ITERM_MASK, 0x0 << 7);
	if (ret) {
		dev_err(info->dev, "set iterm control  disable failed\n");
		return ret;
	}

   ret = psc2965_update_bits(info, PSC2965_REG_C,
				  PSC2965_REG_0x0C_MASK,
				  0x0 << 6);
	if (ret) {
		dev_err(info->dev, "set 0x0C failed\n");
		return ret;
	}

   ret = psc2965_update_bits(info, PSC2965_REG_C,
				  PSC2965_REG_NTC_DIS_MASK,
				  0x1 << 4);
	if (ret) {
		dev_err(info->dev, "set NTC_DIS failed\n");
		return ret;
	}
	
	ret = psc2965_charger_set_termina_vol(info, voltage_max_microvolt);
	if (ret) {
		dev_err(info->dev, "set psc2965 terminal vol failed\n");
		return ret;
	}

	ret = psc2965_charger_set_termina_cur(info, termination_cur);
	if (ret) {
		dev_err(info->dev, "set psc2965 terminal cur failed\n");
		return ret;
	}

	ret = psc2965_charger_set_limit_current(info, info->cur.unknown_cur);
	if (ret)
		dev_err(info->dev, "set psc2965 limit current failed\n");

	info->current_charge_limit_cur = PSC2965_REG_ICHG_LSB * 1000;
	info->current_input_limit_cur = PSC2965_REG_IINDPM_LSB * 1000;

	return ret;
}

static int
psc2965_charger_get_charge_voltage(struct psc2965_charger_info *info,
				   u32 *charge_vol)
{
	struct power_supply *psy;
	union power_supply_propval val;
	int ret;

	psy = power_supply_get_by_name(PSC2965_BATTERY_NAME);
	if (!psy) {
		dev_err(info->dev, "failed to get PSC2965_BATTERY_NAME\n");
		return -ENODEV;
	}

	val.intval = 0;
	ret = power_supply_get_property(psy,
					POWER_SUPPLY_PROP_CONSTANT_CHARGE_VOLTAGE,
					&val);
	power_supply_put(psy);
	if (ret) {
		dev_err(info->dev, "failed to get CONSTANT_CHARGE_VOLTAGE\n");
		return ret;
	}

	*charge_vol = val.intval;

	return 0;
}

#if 0
static void psc2965_dump_chg_reg(struct psc2965_charger_info *info) {
	int i = 0;
	int ret;
	unsigned char v = 0;
	for (i = 0; i <= PSC2965_REG_NUM; i++) {
		ret = psc2965_read(info, i, &v);
		if (ret < 0) {
			dev_err(info->dev, "read [%d] error", i);
		}
		dev_info(info->dev, "psc2965_dump_chg_reg reg[%02x] = %02x\n", i, v);
	}
}
#endif

static int psc2965_charger_start_charge(struct psc2965_charger_info *info)
{
	int ret = 0;

	ret = psc2965_update_bits(info, PSC2965_REG_0,
				  PSC2965_REG_EN_HIZ_MASK, 0);
	if (ret)
		dev_err(info->dev, "disable HIZ mode failed\n");

	ret = psc2965_update_bits(info, PSC2965_REG_5,
				 BIT(3),
				 0x01 << 3);
	if (ret) {
		dev_err(info->dev, "Failed to enable psc2965 watchdog\n");
		return ret;
	}

	if (info->role == PSC2965_ROLE_SLAVE) {
#if defined(FCHG_CHG_EN_DEFAULT_LOW)
		gpiod_set_value_cansleep(info->gpiod, 1);
#else
		gpiod_set_value_cansleep(info->gpiod, 0);
#endif
	}

	ret = psc2965_charger_set_limit_current(info,
						info->last_limit_cur);
	if (ret) {
		dev_err(info->dev, "failed to set limit current\n");
		return ret;
	}

	ret = psc2965_charger_set_termina_cur(info, info->termination_cur);
	if (ret)
		dev_err(info->dev, "set psc2965 terminal cur failed\n");

#if 0
  psc2965_dump_chg_reg(info);
#endif

	return ret;
}

static void psc2965_charger_stop_charge(struct psc2965_charger_info *info, bool present)
{
	int ret;

	if (info->role == PSC2965_ROLE_SLAVE) {
		ret = psc2965_update_bits(info, PSC2965_REG_0,
					  PSC2965_REG_EN_HIZ_MASK,
					  0x01 << PSC2965_REG_EN_HIZ_SHIFT);
		if (ret)
			dev_err(info->dev, "enable HIZ mode failed\n");

#if defined(FCHG_CHG_EN_DEFAULT_LOW)
		gpiod_set_value_cansleep(info->gpiod, 0);
#else
		gpiod_set_value_cansleep(info->gpiod, 1);
#endif
	}

	if (info->disable_power_path) {
		ret = psc2965_update_bits(info, PSC2965_REG_0,
					  PSC2965_REG_EN_HIZ_MASK,
					  0x01 << PSC2965_REG_EN_HIZ_SHIFT);
		if (ret)
			dev_err(info->dev, "Failed to disable power path\n");
	}

	ret = psc2965_update_bits(info, PSC2965_REG_5,
				  BIT(3), 0);
	if (ret)
		dev_err(info->dev, "Failed to disable psc2965 watchdog\n");
}

static int psc2965_charger_set_current(struct psc2965_charger_info *info,
				       u32 cur)
{
	u8 reg_val;

	cur = cur / 1000;
	if (cur > 3000) {
		reg_val = 0x32;
	} else {
		reg_val = cur / PSC2965_REG_ICHG_LSB;
		reg_val &= PSC2965_REG_ICHG_MASK;
	}

	return psc2965_update_bits(info, PSC2965_REG_2,
				   PSC2965_REG_ICHG_MASK,
				   reg_val);
}

static int psc2965_charger_get_current(struct psc2965_charger_info *info,
				       u32 *cur)
{
	u8 reg_val;
	int ret;

	ret = psc2965_read(info, PSC2965_REG_2, &reg_val);
	if (ret < 0)
		return ret;

	reg_val &= PSC2965_REG_ICHG_MASK;
	*cur = reg_val * PSC2965_REG_ICHG_LSB * 1000;

	return 0;
}

static int
psc2965_charger_set_limit_current(struct psc2965_charger_info *info,
				  u32 limit_cur)
{
	u8 reg_val;
	int ret;

	if (limit_cur >= PSC2965_LIMIT_CURRENT_MAX)
		limit_cur = PSC2965_LIMIT_CURRENT_MAX;

	info->last_limit_cur = limit_cur;
	limit_cur -= PSC2965_LIMIT_CURRENT_OFFSET;
	limit_cur = limit_cur / 1000;
	reg_val = limit_cur / PSC2965_REG_IINLIM_BASE;

	ret = psc2965_update_bits(info, PSC2965_REG_0,
				  PSC2965_REG_LIMIT_CURRENT_MASK,
				  reg_val);
	if (ret)
		dev_err(info->dev, "set psc2965 limit cur failed\n");

	info->actual_limit_cur = reg_val * PSC2965_REG_IINLIM_BASE * 1000;
	info->actual_limit_cur += PSC2965_LIMIT_CURRENT_OFFSET;

#if 0	
	psc2965_dump_chg_reg(info);
#endif
  
	return ret;
}

static u32
psc2965_charger_get_limit_current(struct psc2965_charger_info *info,
				  u32 *limit_cur)
{
	u8 reg_val;
	int ret;

	ret = psc2965_read(info, PSC2965_REG_0, &reg_val);
	if (ret < 0)
		return ret;

	reg_val &= PSC2965_REG_LIMIT_CURRENT_MASK;
	*limit_cur = reg_val * PSC2965_REG_IINLIM_BASE * 1000;
	*limit_cur += PSC2965_LIMIT_CURRENT_OFFSET;
	if (*limit_cur >= PSC2965_LIMIT_CURRENT_MAX)
		*limit_cur = PSC2965_LIMIT_CURRENT_MAX;

	return 0;
}

static int psc2965_charger_get_health(struct psc2965_charger_info *info,
				      u32 *health)
{
	*health = POWER_SUPPLY_HEALTH_GOOD;

	return 0;
}

static int psc2965_charger_get_online(struct psc2965_charger_info *info,
				      u32 *online)
{
	if (info->limit)
		*online = true;
	else
		*online = false;

	return 0;
}

static void psc2965_dump_register(struct psc2965_charger_info *info)
{
	int i, ret, len, idx = 0;
	u8 reg_val;
	char buf[256];

	memset(buf, '\0', sizeof(buf));
	for (i = 0; i < PSC2965_REG_NUM; i++) {
		ret = psc2965_read(info,  reg_tab[i].addr, &reg_val);
		if (ret == 0) {
			len = snprintf(buf + idx, sizeof(buf) - idx,
				       "[REG_0x%.2x]=0x%.2x  ",
				       reg_tab[i].addr, reg_val);
			idx += len;
		}
	}

	dev_info(info->dev, "%s: %s", __func__, buf);
}

static int psc2965_charger_feed_watchdog(struct psc2965_charger_info *info)
{
	int ret;
	u32 limit_cur = 0;

	ret = psc2965_update_bits(info, PSC2965_REG_1,
				  PSC2965_REG_RESET_MASK,
				  PSC2965_REG_RESET_MASK);
	if (ret) {
		dev_err(info->dev, "reset psc2965 failed\n");
		return ret;
	}

	if (info->otg_enable)
		return 0;

	ret = psc2965_charger_get_limit_current(info, &limit_cur);
	if (ret) {
		dev_err(info->dev, "get limit cur failed\n");
		return ret;
	}

	if (info->actual_limit_cur == limit_cur)
		return 0;

	ret = psc2965_charger_set_limit_current(info, info->actual_limit_cur);
	if (ret) {
		dev_err(info->dev, "set limit cur failed\n");
		return ret;
	}
  
	return 0;
}

static irqreturn_t psc2965_int_handler(int irq, void *dev_id)
{
	struct psc2965_charger_info *info = dev_id;

	if (!info) {
		pr_err("%s:line%d: NULL pointer!!!\n", __func__, __LINE__);
		return IRQ_HANDLED;
	}

	dev_info(info->dev, "interrupt occurs\n");
	psc2965_dump_register(info);

	return IRQ_HANDLED;
}

static int psc2965_charger_get_status(struct psc2965_charger_info *info)
{
	if (info->charging)
		return POWER_SUPPLY_STATUS_CHARGING;
	else
		return POWER_SUPPLY_STATUS_NOT_CHARGING;
}

static bool psc2965_charger_get_power_path_status(struct psc2965_charger_info *info)
{
	u8 value;
	int ret;
	bool power_path_enabled = true;

	ret = psc2965_read(info, PSC2965_REG_0, &value);
	if (ret < 0) {
		dev_err(info->dev, "Fail to get power path status, ret = %d\n", ret);
		return power_path_enabled;
	}

	if (value & PSC2965_REG_EN_HIZ_MASK)
		power_path_enabled = false;

	return power_path_enabled;
}

static int psc2965_charger_set_power_path_status(struct psc2965_charger_info *info, bool enable)
{
	int ret = 0;
	u8 value = 0x1;

	if (enable)
		value = 0;

	ret = psc2965_update_bits(info, PSC2965_REG_0,
				  PSC2965_REG_EN_HIZ_MASK,
				  value << PSC2965_REG_EN_HIZ_SHIFT);
	if (ret)
		dev_err(info->dev, "%s HIZ mode failed, ret = %d\n",
			enable ? "Enable" : "Disable", ret);

	return ret;
}

static void psc2965_check_wireless_charge(struct psc2965_charger_info *info, bool enable)
{
	int ret;

	if (!enable)
		cancel_delayed_work_sync(&info->cur_work);

	if (info->is_wireless_charge && enable) {
		cancel_delayed_work_sync(&info->cur_work);
		ret = psc2965_charger_set_current(info, info->current_charge_limit_cur);
		if (ret < 0)
			dev_err(info->dev, "%s:set charge current failed\n", __func__);

		ret = psc2965_charger_set_current(info, info->current_input_limit_cur);
		if (ret < 0)
			dev_err(info->dev, "%s:set charge current failed\n", __func__);

		pm_wakeup_event(info->dev, PSC2965_WAKE_UP_MS);
		schedule_delayed_work(&info->cur_work, PSC2965_CURRENT_WORK_MS);
	} else if (info->is_wireless_charge && !enable) {
		info->new_charge_limit_cur = info->current_charge_limit_cur;
		info->current_charge_limit_cur = PSC2965_REG_ICHG_LSB * 1000;
		info->new_input_limit_cur = info->current_input_limit_cur;
		info->current_input_limit_cur = PSC2965_REG_IINDPM_LSB * 1000;
	} else if (!info->is_wireless_charge && !enable) {
		info->new_charge_limit_cur = PSC2965_REG_ICHG_LSB * 1000;
		info->current_charge_limit_cur = PSC2965_REG_ICHG_LSB * 1000;
		info->new_input_limit_cur = PSC2965_REG_IINDPM_LSB * 1000;
		info->current_input_limit_cur = PSC2965_REG_IINDPM_LSB * 1000;
	}
}

static int psc2965_charger_set_status(struct psc2965_charger_info *info,
				      int val, u32 input_vol, bool bat_present)
{
	int ret = 0;

	if (val == CM_FAST_CHARGE_OVP_ENABLE_CMD) {
		ret = psc2965_charger_set_ovp(info, PSC2965_FCHG_OVP_9V);
		if (ret) {
			dev_err(info->dev, "failed to set fast charge 9V ovp\n");
			return ret;
		}
	} else if (val == CM_FAST_CHARGE_OVP_DISABLE_CMD) {
		ret = psc2965_charger_set_ovp(info, PSC2965_FCHG_OVP_6V);
		if (ret) {
			dev_err(info->dev, "failed to set fast charge 5V ovp\n");
			return ret;
		}
	}

	if (val > CM_FAST_CHARGE_NORMAL_CMD)
		return 0;

	if (!val && info->charging) {
		psc2965_check_wireless_charge(info, false);
		psc2965_charger_stop_charge(info, bat_present);
		info->charging = false;
	} else if (val && !info->charging) {
		psc2965_check_wireless_charge(info, true);
		ret = psc2965_charger_start_charge(info);
		if (ret)
			dev_err(info->dev, "start charge failed\n");
		else
			info->charging = true;
	}

	return ret;
}

static void psc2965_charger_work(struct work_struct *data)
{
	struct psc2965_charger_info *info =
		container_of(data, struct psc2965_charger_info, work);
	bool present = psc2965_charger_is_bat_present(info);

	if (!info) {
		pr_err("%s:line%d: NULL pointer!!!\n", __func__, __LINE__);
		return;
	}

	if (info->limit)
		schedule_delayed_work(&info->wdt_work, 0);
	else
		cancel_delayed_work_sync(&info->wdt_work);

	dev_info(info->dev, "battery present = %d, charger type = %d, limit = %d\n",
		 present, info->usb_phy->chg_type, info->limit);
	cm_notify_event(info->psy_usb, CM_EVENT_CHG_START_STOP, NULL);
}

static void psc2965_current_work(struct work_struct *data)
{
	struct delayed_work *dwork = to_delayed_work(data);
	struct psc2965_charger_info *info =
		container_of(dwork, struct psc2965_charger_info, cur_work);
	int ret = 0;
	bool need_return = false;

	if (!info) {
		pr_err("%s:line%d: NULL pointer!!!\n", __func__, __LINE__);
		return;
	}

	if (info->current_charge_limit_cur > info->new_charge_limit_cur) {
		ret = psc2965_charger_set_current(info, info->new_charge_limit_cur);
		if (ret < 0)
			dev_err(info->dev, "%s: set charge limit cur failed\n", __func__);
		return;
	}

	if (info->current_input_limit_cur > info->new_input_limit_cur) {
		ret = psc2965_charger_set_limit_current(info, info->new_input_limit_cur);
		if (ret < 0)
			dev_err(info->dev, "%s: set input limit cur failed\n", __func__);
		return;
	}

	if (info->current_charge_limit_cur + PSC2965_REG_ICHG_LSB * 1000 <=
	    info->new_charge_limit_cur)
		info->current_charge_limit_cur += PSC2965_REG_ICHG_LSB * 1000;
	else
		need_return = true;

	if (info->current_input_limit_cur + PSC2965_REG_IINDPM_LSB * 1000 <=
	    info->new_input_limit_cur)
		info->current_input_limit_cur += PSC2965_REG_IINDPM_LSB * 1000;
	else if (need_return)
		return;

	ret = psc2965_charger_set_current(info, info->current_charge_limit_cur);
	if (ret < 0) {
		dev_err(info->dev, "set charge limit current failed\n");
		return;
	}

	ret = psc2965_charger_set_limit_current(info, info->current_input_limit_cur);
	if (ret < 0) {
		dev_err(info->dev, "set input limit current failed\n");
		return;
	}

	dev_info(info->dev, "set charge_limit_cur %duA, input_limit_curr %duA\n",
		info->current_charge_limit_cur, info->current_input_limit_cur);

	schedule_delayed_work(&info->cur_work, PSC2965_CURRENT_WORK_MS);
}

#if IS_ENABLED(CONFIG_SC27XX_PD)
static int psc2965_charger_pd_extcon_event(struct notifier_block *nb,
					   unsigned long event, void *param)
{
	struct psc2965_charger_info *info =
		container_of(nb, struct psc2965_charger_info, pd_extcon_nb);

	if (info->pd_hard_reset) {
		dev_info(info->dev, "Already receive USB PD hard reset\n");
		return NOTIFY_OK;
	}

	info->pd_hard_reset = true;
	dev_info(info->dev, "Receive USB PD hard reset request\n");
	schedule_delayed_work(&info->pd_hard_reset_work,
			      msecs_to_jiffies(PSC2965_PD_HARD_RESET_MS));

	return NOTIFY_OK;
}

static void psc2965_charger_detect_pd_extcon_status(struct psc2965_charger_info *info)
{
	if (!info->pd_extcon_enable)
		return;

	if (extcon_get_state(info->pd_extcon, EXTCON_CHG_USB_PD)) {
		info->pd_hard_reset = true;
		dev_info(info->dev, "Detect USB PD hard reset request\n");
		schedule_delayed_work(&info->pd_hard_reset_work,
				      msecs_to_jiffies(PSC2965_PD_HARD_RESET_MS));
	}
}

static void psc2965_charger_pd_hard_reset_work(struct work_struct *work)
{
	struct delayed_work *dwork = to_delayed_work(work);
	struct psc2965_charger_info *info = container_of(dwork,
			struct psc2965_charger_info, pd_hard_reset_work);

	if (!info->pd_hard_reset) {
		if (info->usb_phy->chg_state == USB_CHARGER_PRESENT)
			return;

		dev_info(info->dev, "Not USB PD hard reset, charger out\n");

		info->limit = 0;
		schedule_work(&info->work);
	}
	info->pd_hard_reset = false;
}

static int psc2965_charger_register_pd_extcon(struct device *dev,
					      struct psc2965_charger_info *info)
{
	int ret = 0;

	info->pd_extcon_enable = device_property_read_bool(dev, "pd-extcon-enable");

	if (!info->pd_extcon_enable)
		return 0;

	INIT_DELAYED_WORK(&info->pd_hard_reset_work, psc2965_charger_pd_hard_reset_work);

	if (of_property_read_bool(dev->of_node, "extcon")) {
		info->pd_extcon = extcon_get_edev_by_phandle(dev, 1);
		if (IS_ERR(info->pd_extcon)) {
			dev_err(info->dev, "failed to find pd extcon device.\n");
			return -EPROBE_DEFER;
		}

		info->pd_extcon_nb.notifier_call = psc2965_charger_pd_extcon_event;
		ret = devm_extcon_register_notifier_all(dev,
							info->pd_extcon,
							&info->pd_extcon_nb);
		if (ret)
			dev_err(info->dev, "Can't register pd extcon\n");
	}

	return ret;
}

static void psc2965_charger_typec_extcon_work(struct work_struct *work)
{
	struct delayed_work *dwork = to_delayed_work(work);
	struct psc2965_charger_info *info =
		container_of(dwork, struct psc2965_charger_info, typec_extcon_work);

	if (!extcon_get_state(info->typec_extcon, EXTCON_USB)) {
		info->limit = 0;
		info->typec_online = false;
		pm_wakeup_event(info->dev, PSC2965_WAKE_UP_MS);
		dev_info(info->dev, "typec disconnect while pd hard reset.\n");
		schedule_work(&info->work);
	}
}

static int psc2965_charger_typec_extcon_event(struct notifier_block *nb,
					      unsigned long event, void *param)
{
	struct psc2965_charger_info *info =
		container_of(nb, struct psc2965_charger_info, typec_extcon_nb);

	if (info->typec_online) {
		dev_info(info->dev, "typec status change.\n");
		schedule_delayed_work(&info->typec_extcon_work, 0);
	}

	return NOTIFY_OK;
}

static int psc2965_charger_register_typec_extcon(struct device *dev,
						 struct psc2965_charger_info *info)
{
	int ret = 0;

	if (!info->pd_extcon_enable)
		return 0;

	INIT_DELAYED_WORK(&info->typec_extcon_work, psc2965_charger_typec_extcon_work);

	if (of_property_read_bool(dev->of_node, "extcon")) {
		info->typec_extcon = extcon_get_edev_by_phandle(dev, 0);
		if (IS_ERR(info->typec_extcon)) {
			dev_err(info->dev, "failed to find typec extcon device.\n");
			return -EPROBE_DEFER;
		}

		info->typec_extcon_nb.notifier_call = psc2965_charger_typec_extcon_event;
		ret = devm_extcon_register_notifier_all(dev,
							info->typec_extcon,
							&info->typec_extcon_nb);
		if (ret) {
			dev_err(info->dev, "Can't register typec extcon\n");
			return ret;
		}
	}

	return 0;
}
#else
static void psc2965_charger_detect_pd_extcon_status(struct psc2965_charger_info *info)
{

}

static int psc2965_charger_register_pd_extcon(struct device *dev,
					      struct psc2965_charger_info *info)
{
	return 0;
}

static int psc2965_charger_register_typec_extcon(struct device *dev,
						 struct psc2965_charger_info *info)
{
	return 0;
}
#endif

static int psc2965_charger_usb_change(struct notifier_block *nb,
				      unsigned long limit, void *data)
{
	struct psc2965_charger_info *info =
		container_of(nb, struct psc2965_charger_info, usb_notify);

	if (!info) {
		pr_err("%s:line%d: NULL pointer!!!\n", __func__, __LINE__);
		return NOTIFY_OK;
	}

	/*
	 * only master should do work when vbus change.
	 * let info->limit = limit, slave will online, too.
	 */
	if (info->role == PSC2965_ROLE_SLAVE)
		return NOTIFY_OK;

	if (!info->pd_hard_reset) {
		info->limit = limit;
		if (info->typec_online) {
			info->typec_online = false;
			dev_info(info->dev, "typec not disconnect while pd hard reset.\n");
		}

		pm_wakeup_event(info->dev, PSC2965_WAKE_UP_MS);

		schedule_work(&info->work);
	} else {
		info->pd_hard_reset = false;
		if (info->usb_phy->chg_state == USB_CHARGER_PRESENT) {
			dev_err(info->dev, "The adapter is not PD adapter.\n");
			info->limit = limit;
			pm_wakeup_event(info->dev, PSC2965_WAKE_UP_MS);
			schedule_work(&info->work);
		} else if (!extcon_get_state(info->typec_extcon, EXTCON_USB)) {
			dev_err(info->dev, "typec disconnect before pd hard reset.\n");
			info->limit = 0;
			pm_wakeup_event(info->dev, PSC2965_WAKE_UP_MS);
			schedule_work(&info->work);
		} else {
			info->typec_online = true;
			dev_err(info->dev, "USB PD hard reset, ignore vbus off\n");
			cancel_delayed_work_sync(&info->pd_hard_reset_work);
		}
	}

	return NOTIFY_OK;
}

static int psc2965_charger_usb_get_property(struct power_supply *psy,
					    enum power_supply_property psp,
					    union power_supply_propval *val)
{
	struct psc2965_charger_info *info = power_supply_get_drvdata(psy);
	u32 cur = 0, online, health, enabled = 0;
	enum usb_charger_type type;
	int ret = 0;

	if (!info) {
		pr_err("%s:line%d: NULL pointer!!!\n", __func__, __LINE__);
		return -EINVAL;
	}

	mutex_lock(&info->lock);

	switch (psp) {
	case POWER_SUPPLY_PROP_STATUS:
		if (val->intval == CM_POWER_PATH_ENABLE_CMD ||
		    val->intval == CM_POWER_PATH_DISABLE_CMD) {
			val->intval = psc2965_charger_get_power_path_status(info);
			break;
		}

		if (info->limit || info->is_wireless_charge)
			val->intval = psc2965_charger_get_status(info);
		else
			val->intval = POWER_SUPPLY_STATUS_DISCHARGING;
		break;

	case POWER_SUPPLY_PROP_CONSTANT_CHARGE_CURRENT:
		if (!info->charging) {
			val->intval = 0;
		} else {
			ret = psc2965_charger_get_current(info, &cur);
			if (ret)
				goto out;

			val->intval = cur;
		}
		break;

	case POWER_SUPPLY_PROP_INPUT_CURRENT_LIMIT:
		if (!info->charging) {
			val->intval = 0;
		} else {
			ret = psc2965_charger_get_limit_current(info, &cur);
			if (ret)
				goto out;

			val->intval = cur;
		}
		break;

	case POWER_SUPPLY_PROP_ONLINE:
		ret = psc2965_charger_get_online(info, &online);
		if (ret)
			goto out;

		val->intval = online;

		break;

	case POWER_SUPPLY_PROP_HEALTH:
		if (info->charging) {
			val->intval = 0;
		} else {
			ret = psc2965_charger_get_health(info, &health);
			if (ret)
				goto out;

			val->intval = health;
		}
		break;

	case POWER_SUPPLY_PROP_USB_TYPE:
		type = info->usb_phy->chg_type;

		switch (type) {
		case SDP_TYPE:
			val->intval = POWER_SUPPLY_USB_TYPE_SDP;
			break;

		case DCP_TYPE:
			val->intval = POWER_SUPPLY_USB_TYPE_DCP;
			break;

		case CDP_TYPE:
			val->intval = POWER_SUPPLY_USB_TYPE_CDP;
			break;

		default:
			val->intval = POWER_SUPPLY_USB_TYPE_UNKNOWN;
		}

		break;

	case POWER_SUPPLY_PROP_CALIBRATE:
		if (info->role == PSC2965_ROLE_SLAVE) {
			enabled = gpiod_get_value_cansleep(info->gpiod);
			val->intval = !enabled;
		}

		break;
	default:
		ret = -EINVAL;
	}

out:
	mutex_unlock(&info->lock);
	return ret;
}

static int psc2965_charger_usb_set_property(struct power_supply *psy,
				enum power_supply_property psp,
				const union power_supply_propval *val)
{
	struct psc2965_charger_info *info = power_supply_get_drvdata(psy);
	int ret = 0;
	u32 input_vol;
	bool bat_present;

	if (!info) {
		pr_err("%s:line%d: NULL pointer!!!\n", __func__, __LINE__);
		return -EINVAL;
	}

	/*
	 * input_vol and bat_present should be assigned a value, only if psp is
	 * POWER_SUPPLY_PROP_STATUS and POWER_SUPPLY_PROP_CALIBRATE.
	 */
	if (psp == POWER_SUPPLY_PROP_STATUS || psp == POWER_SUPPLY_PROP_CALIBRATE) {
		bat_present = psc2965_charger_is_bat_present(info);
		ret = psc2965_charger_get_charge_voltage(info, &input_vol);
		if (ret) {
			input_vol = 0;
			dev_err(info->dev, "failed to get charge voltage! ret = %d\n", ret);
		}
	}

	mutex_lock(&info->lock);

	switch (psp) {
	case POWER_SUPPLY_PROP_CONSTANT_CHARGE_CURRENT:
		if (info->is_wireless_charge) {
			cancel_delayed_work_sync(&info->cur_work);
			info->new_charge_limit_cur = val->intval;
			pm_wakeup_event(info->dev, PSC2965_WAKE_UP_MS);
			schedule_delayed_work(&info->cur_work, PSC2965_CURRENT_WORK_MS * 2);
			break;
		}

		ret = psc2965_charger_set_current(info, val->intval);
		if (ret < 0)
			dev_err(info->dev, "set charge current failed\n");
		break;
	case POWER_SUPPLY_PROP_INPUT_CURRENT_LIMIT:
		if (info->is_wireless_charge) {
			cancel_delayed_work_sync(&info->cur_work);
			info->new_input_limit_cur = val->intval;
			pm_wakeup_event(info->dev, PSC2965_WAKE_UP_MS);
			schedule_delayed_work(&info->cur_work, PSC2965_CURRENT_WORK_MS * 2);
			break;
		}

		ret = psc2965_charger_set_limit_current(info, val->intval);
		if (ret < 0)
			dev_err(info->dev, "set input current limit failed\n");
		break;

	case POWER_SUPPLY_PROP_STATUS:
		if (val->intval == CM_POWER_PATH_ENABLE_CMD) {
			ret = psc2965_charger_set_power_path_status(info, true);
			break;
		} else if (val->intval == CM_POWER_PATH_DISABLE_CMD) {
			ret = psc2965_charger_set_power_path_status(info, false);
			break;
		}

		ret = psc2965_charger_set_status(info, val->intval, input_vol, bat_present);
		if (ret < 0)
			dev_err(info->dev, "set charge status failed\n");
		break;

	case POWER_SUPPLY_PROP_CONSTANT_CHARGE_VOLTAGE_MAX:
		ret = psc2965_charger_set_termina_vol(info, val->intval / 1000);
		if (ret < 0)
			dev_err(info->dev, "failed to set terminate voltage\n");
		break;

	case POWER_SUPPLY_PROP_CALIBRATE:
		if (val->intval == true) {
			psc2965_check_wireless_charge(info, true);
			ret = psc2965_charger_start_charge(info);
			if (ret)
				dev_err(info->dev, "start charge failed\n");
		} else if (val->intval == false) {
			psc2965_check_wireless_charge(info, false);
			psc2965_charger_stop_charge(info, bat_present);
		}
		break;
	case POWER_SUPPLY_PROP_TYPE:
		if (val->intval == POWER_SUPPLY_WIRELESS_CHARGER_TYPE_BPP) {
			info->is_wireless_charge = true;
			ret = psc2965_charger_set_ovp(info, PSC2965_FCHG_OVP_6V);
		} else if (val->intval == POWER_SUPPLY_WIRELESS_CHARGER_TYPE_EPP) {
			info->is_wireless_charge = true;
			ret = psc2965_charger_set_ovp(info, PSC2965_FCHG_OVP_14V);
		} else {
			info->is_wireless_charge = false;
			ret = psc2965_charger_set_ovp(info, PSC2965_FCHG_OVP_6V);
		}
		if (ret)
			dev_err(info->dev, "failed to set fast charge ovp\n");

		break;
	case POWER_SUPPLY_PROP_PRESENT:
		info->limit = val->intval;
		if (val->intval == true)
			schedule_delayed_work(&info->wdt_work, 0);
		else
			cancel_delayed_work_sync(&info->wdt_work);
		break;
	default:
		ret = -EINVAL;
	}

	mutex_unlock(&info->lock);
	return ret;
}

static int psc2965_charger_property_is_writeable(struct power_supply *psy,
						 enum power_supply_property psp)
{
	int ret;

	switch (psp) {
	case POWER_SUPPLY_PROP_CONSTANT_CHARGE_CURRENT:
	case POWER_SUPPLY_PROP_INPUT_CURRENT_LIMIT:
	case POWER_SUPPLY_PROP_CALIBRATE:
	case POWER_SUPPLY_PROP_TYPE:
	case POWER_SUPPLY_PROP_STATUS:
	case POWER_SUPPLY_PROP_PRESENT:
		ret = 1;
		break;

	default:
		ret = 0;
	}

	return ret;
}

static enum power_supply_usb_type psc2965_charger_usb_types[] = {
	POWER_SUPPLY_USB_TYPE_UNKNOWN,
	POWER_SUPPLY_USB_TYPE_SDP,
	POWER_SUPPLY_USB_TYPE_DCP,
	POWER_SUPPLY_USB_TYPE_CDP,
	POWER_SUPPLY_USB_TYPE_C,
	POWER_SUPPLY_USB_TYPE_PD,
	POWER_SUPPLY_USB_TYPE_PD_DRP,
	POWER_SUPPLY_USB_TYPE_APPLE_BRICK_ID
};

static enum power_supply_property psc2965_usb_props[] = {
	POWER_SUPPLY_PROP_STATUS,
	POWER_SUPPLY_PROP_CONSTANT_CHARGE_CURRENT,
	POWER_SUPPLY_PROP_INPUT_CURRENT_LIMIT,
	POWER_SUPPLY_PROP_ONLINE,
	POWER_SUPPLY_PROP_HEALTH,
	POWER_SUPPLY_PROP_USB_TYPE,
	POWER_SUPPLY_PROP_CALIBRATE,
	POWER_SUPPLY_PROP_TYPE,
};

static const struct power_supply_desc psc2965_slave_charger_desc = {
	.name			= "psc2965_slave_charger",
	.type			= POWER_SUPPLY_TYPE_USB,
	.properties		= psc2965_usb_props,
	.num_properties		= ARRAY_SIZE(psc2965_usb_props),
	.get_property		= psc2965_charger_usb_get_property,
	.set_property		= psc2965_charger_usb_set_property,
	.property_is_writeable	= psc2965_charger_property_is_writeable,
	.usb_types		= psc2965_charger_usb_types,
	.num_usb_types		= ARRAY_SIZE(psc2965_charger_usb_types),
};

static ssize_t psc2965_register_value_show(struct device *dev,
					   struct device_attribute *attr,
					   char *buf)
{
	struct psc2965_charger_sysfs *psc2965_sysfs =
		container_of(attr, struct psc2965_charger_sysfs,
			     attr_psc2965_reg_val);
	struct  psc2965_charger_info *info =  psc2965_sysfs->info;
	u8 val;
	int ret;

	if (!info)
		return snprintf(buf, PAGE_SIZE, "%s  psc2965_sysfs->info is null\n", __func__);

	ret = psc2965_read(info, reg_tab[info->reg_id].addr, &val);
	if (ret) {
		dev_err(info->dev, "fail to get  PSC2965_REG_0x%.2x value, ret = %d\n",
			reg_tab[info->reg_id].addr, ret);
		return snprintf(buf, PAGE_SIZE, "fail to get  PSC2965_REG_0x%.2x value\n",
			       reg_tab[info->reg_id].addr);
	}

	return snprintf(buf, PAGE_SIZE, "PSC2965_REG_0x%.2x = 0x%.2x\n",
			reg_tab[info->reg_id].addr, val);
}

static ssize_t psc2965_register_value_store(struct device *dev,
					    struct device_attribute *attr,
					    const char *buf, size_t count)
{
	struct psc2965_charger_sysfs *psc2965_sysfs =
		container_of(attr, struct psc2965_charger_sysfs,
			     attr_psc2965_reg_val);
	struct psc2965_charger_info *info = psc2965_sysfs->info;
	u8 val;
	int ret;

	if (!info) {
		dev_err(dev, "%s psc2965_sysfs->info is null\n", __func__);
		return count;
	}

	ret =  kstrtou8(buf, 16, &val);
	if (ret) {
		dev_err(info->dev, "fail to get addr, ret = %d\n", ret);
		return count;
	}

	ret = psc2965_write(info, reg_tab[info->reg_id].addr, val);
	if (ret) {
		dev_err(info->dev, "fail to wite 0x%.2x to REG_0x%.2x, ret = %d\n",
				val, reg_tab[info->reg_id].addr, ret);
		return count;
	}

	dev_info(info->dev, "wite 0x%.2x to REG_0x%.2x success\n", val, reg_tab[info->reg_id].addr);
	return count;
}

static ssize_t psc2965_register_id_store(struct device *dev,
					 struct device_attribute *attr,
					 const char *buf, size_t count)
{
	struct psc2965_charger_sysfs *psc2965_sysfs =
		container_of(attr, struct psc2965_charger_sysfs,
			     attr_psc2965_sel_reg_id);
	struct psc2965_charger_info *info = psc2965_sysfs->info;
	int ret, id;

	if (!info) {
		dev_err(dev, "%s psc2965_sysfs->info is null\n", __func__);
		return count;
	}

	ret =  kstrtoint(buf, 10, &id);
	if (ret) {
		dev_err(info->dev, "%s store register id fail\n", psc2965_sysfs->name);
		return count;
	}

	if (id < 0 || id >= PSC2965_REG_NUM) {
		dev_err(info->dev, "%s store register id fail, id = %d is out of range\n",
			psc2965_sysfs->name, id);
		return count;
	}

	info->reg_id = id;

	dev_info(info->dev, "%s store register id = %d success\n", psc2965_sysfs->name, id);
	return count;
}

static ssize_t psc2965_register_id_show(struct device *dev,
					struct device_attribute *attr,
					char *buf)
{
	struct psc2965_charger_sysfs *psc2965_sysfs =
		container_of(attr, struct psc2965_charger_sysfs,
			     attr_psc2965_sel_reg_id);
	struct psc2965_charger_info *info = psc2965_sysfs->info;

	if (!info)
		return snprintf(buf, PAGE_SIZE, "%s psc2965_sysfs->info is null\n", __func__);

	return snprintf(buf, PAGE_SIZE, "Curent register id = %d\n", info->reg_id);
}

static ssize_t psc2965_register_table_show(struct device *dev,
					   struct device_attribute *attr,
					   char *buf)
{
	struct psc2965_charger_sysfs *psc2965_sysfs =
		container_of(attr, struct psc2965_charger_sysfs,
			     attr_psc2965_lookup_reg);
	struct psc2965_charger_info *info = psc2965_sysfs->info;
	int i, len, idx = 0;
	char reg_tab_buf[1024];

	if (!info)
		return snprintf(buf, PAGE_SIZE, "%s psc2965_sysfs->info is null\n", __func__);

	memset(reg_tab_buf, '\0', sizeof(reg_tab_buf));
	len = snprintf(reg_tab_buf + idx, sizeof(reg_tab_buf) - idx,
		       "Format: [id] [addr] [desc]\n");
	idx += len;

	for (i = 0; i < PSC2965_REG_NUM; i++) {
		len = snprintf(reg_tab_buf + idx, sizeof(reg_tab_buf) - idx,
			       "[%d] [REG_0x%.2x] [%s];\n",
			       reg_tab[i].id, reg_tab[i].addr, reg_tab[i].name);
		idx += len;
	}

	return snprintf(buf, PAGE_SIZE, "%s\n", reg_tab_buf);
}

static ssize_t psc2965_dump_register_show(struct device *dev,
					  struct device_attribute *attr,
					  char *buf)
{
	struct psc2965_charger_sysfs *psc2965_sysfs =
		container_of(attr, struct psc2965_charger_sysfs,
			     attr_psc2965_dump_reg);
	struct psc2965_charger_info *info = psc2965_sysfs->info;

	if (!info)
		return snprintf(buf, PAGE_SIZE, "%s psc2965_sysfs->info is null\n", __func__);

	psc2965_dump_register(info);

	return snprintf(buf, PAGE_SIZE, "%s\n", psc2965_sysfs->name);
}

static int psc2965_register_sysfs(struct psc2965_charger_info *info)
{
	struct psc2965_charger_sysfs *psc2965_sysfs;
	int ret;

	psc2965_sysfs = devm_kzalloc(info->dev, sizeof(*psc2965_sysfs), GFP_KERNEL);
	if (!psc2965_sysfs)
		return -ENOMEM;

	info->sysfs = psc2965_sysfs;
	psc2965_sysfs->name = "psc2965_sysfs";
	psc2965_sysfs->info = info;
	psc2965_sysfs->attrs[0] = &psc2965_sysfs->attr_psc2965_dump_reg.attr;
	psc2965_sysfs->attrs[1] = &psc2965_sysfs->attr_psc2965_lookup_reg.attr;
	psc2965_sysfs->attrs[2] = &psc2965_sysfs->attr_psc2965_sel_reg_id.attr;
	psc2965_sysfs->attrs[3] = &psc2965_sysfs->attr_psc2965_reg_val.attr;
	psc2965_sysfs->attrs[4] = NULL;
	psc2965_sysfs->attr_g.name = "debug";
	psc2965_sysfs->attr_g.attrs = psc2965_sysfs->attrs;

	sysfs_attr_init(&psc2965_sysfs->attr_psc2965_dump_reg.attr);
	psc2965_sysfs->attr_psc2965_dump_reg.attr.name = "psc2965_dump_reg";
	psc2965_sysfs->attr_psc2965_dump_reg.attr.mode = 0444;
	psc2965_sysfs->attr_psc2965_dump_reg.show = psc2965_dump_register_show;

	sysfs_attr_init(&psc2965_sysfs->attr_psc2965_lookup_reg.attr);
	psc2965_sysfs->attr_psc2965_lookup_reg.attr.name = "psc2965_lookup_reg";
	psc2965_sysfs->attr_psc2965_lookup_reg.attr.mode = 0444;
	psc2965_sysfs->attr_psc2965_lookup_reg.show = psc2965_register_table_show;

	sysfs_attr_init(&psc2965_sysfs->attr_psc2965_sel_reg_id.attr);
	psc2965_sysfs->attr_psc2965_sel_reg_id.attr.name = "psc2965_sel_reg_id";
	psc2965_sysfs->attr_psc2965_sel_reg_id.attr.mode = 0644;
	psc2965_sysfs->attr_psc2965_sel_reg_id.show = psc2965_register_id_show;
	psc2965_sysfs->attr_psc2965_sel_reg_id.store = psc2965_register_id_store;

	sysfs_attr_init(&psc2965_sysfs->attr_psc2965_reg_val.attr);
	psc2965_sysfs->attr_psc2965_reg_val.attr.name = "psc2965_reg_val";
	psc2965_sysfs->attr_psc2965_reg_val.attr.mode = 0644;
	psc2965_sysfs->attr_psc2965_reg_val.show = psc2965_register_value_show;
	psc2965_sysfs->attr_psc2965_reg_val.store = psc2965_register_value_store;

	ret = sysfs_create_group(&info->psy_usb->dev.kobj, &psc2965_sysfs->attr_g);
	if (ret < 0)
		dev_err(info->dev, "Cannot create sysfs , ret = %d\n", ret);

	return ret;
}

static void psc2965_charger_detect_status(struct psc2965_charger_info *info)
{
	unsigned int min, max;

	/*
	 * If the USB charger status has been USB_CHARGER_PRESENT before
	 * registering the notifier, we should start to charge with getting
	 * the charge current.
	 */
	if (info->usb_phy->chg_state != USB_CHARGER_PRESENT)
		return;

	usb_phy_get_charger_current(info->usb_phy, &min, &max);
	info->limit = min;

	/*
	 * slave no need to start charge when vbus change.
	 * due to charging in shut down will check each psy
	 * whether online or not, so let info->limit = min.
	 */
	if (info->role == PSC2965_ROLE_SLAVE)
		return;
	schedule_work(&info->work);
}

static void
psc2965_charger_feed_watchdog_work(struct work_struct *work)
{
	struct delayed_work *dwork = to_delayed_work(work);
	struct psc2965_charger_info *info = container_of(dwork,
							 struct psc2965_charger_info,
							 wdt_work);
	int ret;

	ret = psc2965_charger_feed_watchdog(info);
	if (ret)
		schedule_delayed_work(&info->wdt_work, HZ * 5);
	else
		schedule_delayed_work(&info->wdt_work, HZ * 15);
}

#if IS_ENABLED(CONFIG_REGULATOR)
static bool psc2965_charger_check_otg_valid(struct psc2965_charger_info *info)
{
	int ret;
	u8 value = 0;
	bool status = false;

	ret = psc2965_read(info, PSC2965_REG_1, &value);
	if (ret) {
		dev_err(info->dev, "get psc2965 charger otg valid status failed\n");
		return status;
	}

	if (value & PSC2965_REG_OTG_MASK)
		status = true;
	else
		dev_err(info->dev, "otg is not valid, REG_1 = 0x%x\n", value);

	return status;
}

static bool psc2965_charger_check_otg_fault(struct psc2965_charger_info *info)
{
	int ret;
	u8 value = 0;
	bool status = true;

	ret = psc2965_read(info, PSC2965_REG_9, &value);
	if (ret) {
		dev_err(info->dev, "get psc2965 charger otg fault status failed\n");
		return status;
	}

	if (!(value & PSC2965_REG_BOOST_FAULT_MASK))
		status = false;
	else
		dev_err(info->dev, "boost fault occurs, REG_9 = 0x%x\n", value);

	return status;
}

static void psc2965_charger_otg_work(struct work_struct *work)
{
	struct delayed_work *dwork = to_delayed_work(work);
	struct psc2965_charger_info *info = container_of(dwork,
			struct psc2965_charger_info, otg_work);
	bool otg_valid = psc2965_charger_check_otg_valid(info);
	bool otg_fault;
	int ret, retry = 0;

	if (otg_valid)
		goto out;

	do {
		otg_fault = psc2965_charger_check_otg_fault(info);
		if (!otg_fault) {
			ret = psc2965_update_bits(info, PSC2965_REG_1,
						  PSC2965_REG_OTG_MASK,
						  PSC2965_REG_OTG_MASK);
			if (ret)
				dev_err(info->dev, "restart psc2965 charger otg failed\n");
		}

		otg_valid = psc2965_charger_check_otg_valid(info);
	} while (!otg_valid && retry++ < PSC2965_OTG_RETRY_TIMES);

	if (retry >= PSC2965_OTG_RETRY_TIMES) {
		dev_err(info->dev, "Restart OTG failed\n");
		return;
	}

out:
	schedule_delayed_work(&info->otg_work, msecs_to_jiffies(1500));
}

static int psc2965_charger_enable_otg(struct regulator_dev *dev)
{
	struct psc2965_charger_info *info = rdev_get_drvdata(dev);
	int ret = 0;

	if (!info) {
		pr_err("%s:line%d: NULL pointer!!!\n", __func__, __LINE__);
		return -EINVAL;
	}

	mutex_lock(&info->lock);

	/*
	 * Disable charger detection function in case
	 * affecting the OTG timing sequence.
	 */
	ret = regmap_update_bits(info->pmic, info->charger_detect,
				 BIT_DP_DM_BC_ENB, BIT_DP_DM_BC_ENB);
	if (ret) {
		dev_err(info->dev, "failed to disable bc1.2 detect function.\n");
		goto out;
	}

	ret = psc2965_update_bits(info, PSC2965_REG_1,
				  PSC2965_REG_OTG_MASK,
				  PSC2965_REG_OTG_MASK);
	if (ret) {
		dev_err(info->dev, "enable psc2965 otg failed\n");
		regmap_update_bits(info->pmic, info->charger_detect,
				   BIT_DP_DM_BC_ENB, 0);
		goto out;
	}

	info->otg_enable = true;
	schedule_delayed_work(&info->wdt_work,
			      msecs_to_jiffies(PSC2965_FEED_WATCHDOG_VALID_MS));
	schedule_delayed_work(&info->otg_work,
			      msecs_to_jiffies(PSC2965_OTG_VALID_MS));
out:
	mutex_unlock(&info->lock);
	return ret;
}

static int psc2965_charger_disable_otg(struct regulator_dev *dev)
{
	struct psc2965_charger_info *info = rdev_get_drvdata(dev);
	int ret = 0;

	if (!info) {
		pr_err("%s:line%d: NULL pointer!!!\n", __func__, __LINE__);
		return -EINVAL;
	}

	mutex_lock(&info->lock);

	info->otg_enable = false;
	cancel_delayed_work_sync(&info->wdt_work);
	cancel_delayed_work_sync(&info->otg_work);
	ret = psc2965_update_bits(info, PSC2965_REG_1,
				  PSC2965_REG_OTG_MASK,
				  0);
	if (ret) {
		dev_err(info->dev, "disable psc2965 otg failed\n");
		goto out;
	}

	/* Enable charger detection function to identify the charger type */
	ret = regmap_update_bits(info->pmic, info->charger_detect, BIT_DP_DM_BC_ENB, 0);
	if (ret)
		dev_err(info->dev, "enable BC1.2 failed\n");

out:
	mutex_unlock(&info->lock);
	return ret;


}

static int psc2965_charger_vbus_is_enabled(struct regulator_dev *dev)
{
	struct psc2965_charger_info *info = rdev_get_drvdata(dev);
	int ret;
	u8 val;

	if (!info) {
		pr_err("%s:line%d: NULL pointer!!!\n", __func__, __LINE__);
		return -EINVAL;
	}

	mutex_lock(&info->lock);

	ret = psc2965_read(info, PSC2965_REG_1, &val);
	if (ret) {
		dev_err(info->dev, "failed to get psc2965 otg status\n");
		mutex_unlock(&info->lock);
		return ret;
	}

	val &= PSC2965_REG_OTG_MASK;

	mutex_unlock(&info->lock);
	return val;
}

static const struct regulator_ops psc2965_charger_vbus_ops = {
	.enable = psc2965_charger_enable_otg,
	.disable = psc2965_charger_disable_otg,
	.is_enabled = psc2965_charger_vbus_is_enabled,
};

static const struct regulator_desc psc2965_charger_vbus_desc = {
	.name = "otg-vbus",
	.of_match = "otg-vbus",
	.type = REGULATOR_VOLTAGE,
	.owner = THIS_MODULE,
	.ops = &psc2965_charger_vbus_ops,
	.fixed_uV = 5000000,
	.n_voltages = 1,
};
#endif

static int psc2965_charger_probe(struct i2c_client *client,
		const struct i2c_device_id *id)
{
	struct i2c_adapter *adapter = to_i2c_adapter(client->dev.parent);
	struct device *dev = &client->dev;
	struct power_supply_config charger_cfg = { };
	struct psc2965_charger_info *info;
	struct device_node *regmap_np;
	struct platform_device *regmap_pdev;
	int ret;
	bool bat_present;

	if (!adapter) {
		pr_err("%s:line%d: NULL pointer!!!\n", __func__, __LINE__);
		return -EINVAL;
	}

	if (!i2c_check_functionality(adapter, I2C_FUNC_SMBUS_BYTE_DATA)) {
		dev_err(dev, "No support for SMBUS_BYTE_DATA\n");
		return -ENODEV;
	}

	info = devm_kzalloc(dev, sizeof(*info), GFP_KERNEL);
	if (!info)
		return -ENOMEM;

	info->client = client;
	info->dev = dev;

	i2c_set_clientdata(client, info);
	power_path_control(info);

	info->usb_phy = devm_usb_get_phy_by_phandle(dev, "phys", 0);
	if (IS_ERR(info->usb_phy)) {
		dev_err(dev, "failed to find USB phy\n");
		return -EPROBE_DEFER;
	}

	ret = psc2965_charger_register_pd_extcon(info->dev, info);
	if (ret) {
		dev_err(info->dev, "failed to register pd extcon\n");
		return -EPROBE_DEFER;
	}

	ret = psc2965_charger_register_typec_extcon(info->dev, info);
	if (ret) {
		dev_err(info->dev, "failed to register typec extcon\n");
		return -EPROBE_DEFER;
	}

	ret = psc2965_charger_is_fgu_present(info);
	if (ret) {
		dev_err(dev, "sc27xx_fgu not ready.\n");
		return -EPROBE_DEFER;
	}

	info->role = PSC2965_ROLE_SLAVE;

	if (info->role == PSC2965_ROLE_SLAVE) {
		info->gpiod = devm_gpiod_get(dev, "enable", GPIOD_OUT_HIGH);
		if (IS_ERR(info->gpiod)) {
			dev_err(dev, "failed to get enable gpio\n");
			return PTR_ERR(info->gpiod);
		}
	}

	regmap_np = of_find_compatible_node(NULL, NULL, "sprd,sc27xx-syscon");
	if (!regmap_np)
		regmap_np = of_find_compatible_node(NULL, NULL, "sprd,ump962x-syscon");

	if (regmap_np) {
		if (of_device_is_compatible(regmap_np->parent, "sprd,sc2721"))
			info->charger_pd_mask = PSC2965_DISABLE_PIN_MASK_2721;
		else
			info->charger_pd_mask = PSC2965_DISABLE_PIN_MASK;
	} else {
		dev_err(dev, "unable to get syscon node\n");
		return -ENODEV;
	}

	ret = of_property_read_u32_index(regmap_np, "reg", 1,
					 &info->charger_detect);
	if (ret) {
		dev_err(dev, "failed to get charger_detect\n");
		return -EINVAL;
	}

	ret = of_property_read_u32_index(regmap_np, "reg", 2,
					 &info->charger_pd);
	if (ret) {
		dev_err(dev, "failed to get charger_pd reg\n");
		return ret;
	}

	regmap_pdev = of_find_device_by_node(regmap_np);
	if (!regmap_pdev) {
		of_node_put(regmap_np);
		dev_err(dev, "unable to get syscon device\n");
		return -ENODEV;
	}

	of_node_put(regmap_np);
	info->pmic = dev_get_regmap(regmap_pdev->dev.parent, NULL);
	if (!info->pmic) {
		dev_err(dev, "unable to get pmic regmap device\n");
		return -ENODEV;
	}

	bat_present = psc2965_charger_is_bat_present(info);

	mutex_init(&info->lock);
	mutex_lock(&info->lock);

	charger_cfg.drv_data = info;
	charger_cfg.of_node = dev->of_node;

	if (info->role == PSC2965_ROLE_SLAVE) {
		info->psy_usb = devm_power_supply_register(dev,
							   &psc2965_slave_charger_desc,
							   &charger_cfg);
	}

	if (IS_ERR(info->psy_usb)) {
		dev_err(dev, "failed to register power supply\n");
		ret = PTR_ERR(info->psy_usb);
		goto err_regmap_exit;
	}

	ret = psc2965_charger_hw_init(info);
	if (ret) {
		dev_err(dev, "failed to psc2965_charger_hw_init\n");
		goto err_psy_usb;
	}

	psc2965_charger_stop_charge(info, bat_present);

	device_init_wakeup(info->dev, true);

	alarm_init(&info->otg_timer, ALARM_BOOTTIME, NULL);
	INIT_DELAYED_WORK(&info->otg_work, psc2965_charger_otg_work);
	INIT_DELAYED_WORK(&info->wdt_work, psc2965_charger_feed_watchdog_work);


	INIT_WORK(&info->work, psc2965_charger_work);
	INIT_DELAYED_WORK(&info->cur_work, psc2965_current_work);

	info->usb_notify.notifier_call = psc2965_charger_usb_change;
	ret = usb_register_notifier(info->usb_phy, &info->usb_notify);
	if (ret) {
		dev_err(dev, "failed to register notifier:%d\n", ret);
		goto err_psy_usb;
	}

	ret = psc2965_register_sysfs(info);
	if (ret) {
		dev_err(info->dev, "register sysfs fail, ret = %d\n", ret);
		goto error_sysfs;
	}

	info->irq_gpio = of_get_named_gpio(info->dev->of_node, "irq-gpio", 0);
	if (gpio_is_valid(info->irq_gpio)) {
		ret = devm_gpio_request_one(info->dev, info->irq_gpio,
					    GPIOF_DIR_IN, "psc2965_int");
		if (!ret)
			info->client->irq = gpio_to_irq(info->irq_gpio);
		else
			dev_err(dev, "int request failed, ret = %d\n", ret);

		if (info->client->irq < 0) {
			dev_err(dev, "failed to get irq no\n");
			gpio_free(info->irq_gpio);
		} else {
			ret = devm_request_threaded_irq(&info->client->dev, info->client->irq,
							NULL, psc2965_int_handler,
							IRQF_TRIGGER_FALLING | IRQF_ONESHOT,
							"psc2965 interrupt", info);
			if (ret)
				dev_err(info->dev, "Failed irq = %d ret = %d\n",
					info->client->irq, ret);
			else
				enable_irq_wake(client->irq);
		}
	} else {
		dev_err(dev, "failed to get irq gpio\n");
	}

	mutex_unlock(&info->lock);
	psc2965_charger_detect_status(info);
	psc2965_charger_detect_pd_extcon_status(info);
	
	//this is only for slave chg open
	ret = psc2965_update_bits(info, PSC2965_REG_1, PSC2965_REG_CHG_MASK, 1 << PSC2965_REG_CHG_SHIFT);
	if (ret)
		dev_err(info->dev, "enable slave chg failed\n");
		
	printk(" %s end \n",__func__);

	return 0;

error_sysfs:
	sysfs_remove_group(&info->psy_usb->dev.kobj, &info->sysfs->attr_g);
	usb_unregister_notifier(info->usb_phy, &info->usb_notify);
err_psy_usb:
	if (info->irq_gpio)
		gpio_free(info->irq_gpio);
err_regmap_exit:
	mutex_unlock(&info->lock);
	mutex_destroy(&info->lock);
	return ret;
}

static void psc2965_charger_shutdown(struct i2c_client *client)
{
	struct psc2965_charger_info *info = i2c_get_clientdata(client);
	int ret = 0;

	cancel_delayed_work_sync(&info->wdt_work);
	if (info->otg_enable) {
		info->otg_enable = false;
		cancel_delayed_work_sync(&info->otg_work);
		ret = psc2965_update_bits(info, PSC2965_REG_1,
					  PSC2965_REG_OTG_MASK,
					  0);
		if (ret)
			dev_err(info->dev, "disable psc2965 otg failed ret = %d\n", ret);

		/* Enable charger detection function to identify the charger type */
		ret = regmap_update_bits(info->pmic, info->charger_detect,
					 BIT_DP_DM_BC_ENB, 0);
		if (ret)
			dev_err(info->dev,
				"enable charger detection function failed ret = %d\n", ret);
	}
}

static int psc2965_charger_remove(struct i2c_client *client)
{
	struct psc2965_charger_info *info = i2c_get_clientdata(client);

	cancel_delayed_work_sync(&info->wdt_work);
	cancel_delayed_work_sync(&info->otg_work);
	usb_unregister_notifier(info->usb_phy, &info->usb_notify);

	return 0;
}

#if IS_ENABLED(CONFIG_PM_SLEEP)
static int psc2965_charger_suspend(struct device *dev)
{
	struct psc2965_charger_info *info = dev_get_drvdata(dev);
	ktime_t now, add;
	unsigned int wakeup_ms = PSC2965_OTG_ALARM_TIMER_MS;

	if (!info) {
		pr_err("%s:line%d: NULL pointer!!!\n", __func__, __LINE__);
		return -EINVAL;
	}

	if (info->otg_enable || info->limit)
		psc2965_charger_feed_watchdog(info);

	if (!info->otg_enable)
		return 0;

	cancel_delayed_work_sync(&info->wdt_work);
	cancel_delayed_work_sync(&info->cur_work);

	now = ktime_get_boottime();
	add = ktime_set(wakeup_ms / MSEC_PER_SEC,
			(wakeup_ms % MSEC_PER_SEC) * NSEC_PER_MSEC);
	alarm_start(&info->otg_timer, ktime_add(now, add));

	return 0;
}

static int psc2965_charger_resume(struct device *dev)
{
	struct psc2965_charger_info *info = dev_get_drvdata(dev);

	if (!info) {
		pr_err("%s:line%d: NULL pointer!!!\n", __func__, __LINE__);
		return -EINVAL;
	}

	if (info->otg_enable || info->limit)
		psc2965_charger_feed_watchdog(info);

	if (!info->otg_enable)
		return 0;

	alarm_cancel(&info->otg_timer);

	schedule_delayed_work(&info->wdt_work, HZ * 15);
	schedule_delayed_work(&info->cur_work, 0);

	return 0;
}
#endif

static const struct dev_pm_ops psc2965_charger_pm_ops = {
	SET_SYSTEM_SLEEP_PM_OPS(psc2965_charger_suspend,
				psc2965_charger_resume)
};

static const struct i2c_device_id psc2965_i2c_id[] = {
	{"psc2965_slave_chg", 0},
	{}
};

static const struct of_device_id psc2965_charger_of_match[] = {
	{ .compatible = "psc2965_slave_chg", },
	{ }
};

MODULE_DEVICE_TABLE(of, psc2965_charger_of_match);

static struct i2c_driver psc2965_charger_driver = {
	.driver = {
		.name = "psc2965_slave_chg",
		.of_match_table = psc2965_charger_of_match,
		.pm = &psc2965_charger_pm_ops,
	},
	.probe = psc2965_charger_probe,
	.shutdown = psc2965_charger_shutdown,
	.remove = psc2965_charger_remove,
	.id_table = psc2965_i2c_id,
};

module_i2c_driver(psc2965_charger_driver);
MODULE_DESCRIPTION("PSC2965 Charger Driver");
MODULE_LICENSE("GPL v2");
