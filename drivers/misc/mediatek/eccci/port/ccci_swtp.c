/*
 * Copyright (C) 2015 MediaTek Inc.
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 as
 * published by the Free Software Foundation.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
 * GNU General Public License for more details.
 */
#include <linux/kernel.h>
#include <linux/delay.h>
#include <linux/gpio.h>
#include <linux/irq.h>
#include <linux/interrupt.h>
#include <linux/of.h>
#include <linux/of_fdt.h>
#include <linux/of_irq.h>
#include <linux/of_address.h>
#include <linux/of_gpio.h>

#include <mt-plat/mtk_boot_common.h>
#include "ccci_debug.h"
#include "ccci_config.h"
#include "ccci_modem.h"
#include "ccci_swtp.h"
#include "ccci_fsm.h"
/*Huaqin add for HQ-123513 by shiwenlong at 2021.4.01 start*/
#include <linux/proc_fs.h>
/*Huaqin add for HQ-123513 by shiwenlong at 2021.4.01 end*/
const struct of_device_id swtp_of_match[] = {
	{ .compatible = SWTP_COMPATIBLE_DEVICE_ID, },
	{},
};
#define SWTP_MAX_SUPPORT_MD 1
struct swtp_t swtp_data[SWTP_MAX_SUPPORT_MD];
/*Huaqin add for HQ-123513 by shiwenlong at 2021.4.01 start*/
int get_swtp_state = -1;
int get_swtp_gpio = -1;
static struct proc_dir_entry  *swtp_gpio_status;

static int swtp_gpio_proc_show(struct seq_file *file, void *data)
{
	get_swtp_state = gpio_get_value(get_swtp_gpio);
	seq_printf(file, "%d\n", get_swtp_state);

	return 0;
}
static int swtp_gpio_proc_open (struct inode *inode, struct file *file)
{
	return single_open(file, swtp_gpio_proc_show, inode->i_private);
}
static const struct file_operations swtp_gpio_status_ops = {
	.open = swtp_gpio_proc_open,
	.read = seq_read,
};
/*Huaqin add for HQ-123513 by shiwenlong at 2021.4.01 end*/
static int switch_Tx_Power(int md_id, unsigned int mode)
{
	int ret = 0;
	unsigned int resv = mode;

	ret = exec_ccci_kern_func_by_md_id(md_id, ID_UPDATE_TX_POWER,
		(char *)&resv, sizeof(resv));

	pr_debug("[swtp] switch_MD%d_Tx_Power(%d): ret[%d]\n",
		md_id + 1, resv, ret);

	CCCI_DEBUG_LOG(md_id, "ctl", "switch_MD%d_Tx_Power(%d): %d\n",
		md_id + 1, resv, ret);

	return ret;
}

int switch_MD1_Tx_Power(unsigned int mode)
{
	return switch_Tx_Power(0, mode);
}

int switch_MD2_Tx_Power(unsigned int mode)
{
	return switch_Tx_Power(1, mode);
}

static int swtp_switch_mode(struct swtp_t *swtp)
{
	unsigned long flags;
	int ret = 0;
	if (swtp == NULL) {
		CCCI_LEGACY_ERR_LOG(-1, SYS, "%s:swtp is null\n", __func__);
		return -1;
	}

	spin_lock_irqsave(&swtp->spinlock, flags);
	if (swtp->curr_mode == SWTP_EINT_PIN_PLUG_IN) {
		if (swtp->eint_type == IRQ_TYPE_LEVEL_HIGH)
			irq_set_irq_type(swtp->irq, IRQ_TYPE_LEVEL_HIGH);
		else
			irq_set_irq_type(swtp->irq, IRQ_TYPE_LEVEL_LOW);

		swtp->curr_mode = SWTP_EINT_PIN_PLUG_OUT;
	} else {
		if (swtp->eint_type == IRQ_TYPE_LEVEL_HIGH)
			irq_set_irq_type(swtp->irq, IRQ_TYPE_LEVEL_LOW);
		else
			irq_set_irq_type(swtp->irq, IRQ_TYPE_LEVEL_HIGH);
		swtp->curr_mode = SWTP_EINT_PIN_PLUG_IN;
	}
	CCCI_LEGACY_ALWAYS_LOG(swtp->md_id, SYS, "%s mode %d\n",
		__func__, swtp->curr_mode);
	spin_unlock_irqrestore(&swtp->spinlock, flags);

	return ret;
}

static int swtp_send_tx_power_mode(struct swtp_t *swtp)
{
	unsigned long flags;
	unsigned int md_state;
	int ret = 0;

	md_state = ccci_fsm_get_md_state(swtp->md_id);
	if (md_state != BOOT_WAITING_FOR_HS1 &&
		md_state != BOOT_WAITING_FOR_HS2 &&
		md_state != READY) {
		CCCI_LEGACY_ERR_LOG(swtp->md_id, SYS,
			"%s md_state=%d no ready\n", __func__, md_state);
		ret = 1;
		goto __ERR_HANDLE__;
	}
	if (swtp->md_id == 0)
		ret = switch_MD1_Tx_Power(swtp->curr_mode);
	else {
		CCCI_LEGACY_ERR_LOG(swtp->md_id, SYS,
			"%s md is no support\n", __func__);
		ret = 2;
		goto __ERR_HANDLE__;
	}

	if (ret >= 0)
		CCCI_LEGACY_ALWAYS_LOG(swtp->md_id, SYS,
			"%s send swtp to md ret=%d, mode=%d, rety_cnt=%d\n",
			__func__, ret, swtp->curr_mode, swtp->retry_cnt);
	spin_lock_irqsave(&swtp->spinlock, flags);
	if (ret >= 0)
		swtp->retry_cnt = 0;
	else
		swtp->retry_cnt++;
	spin_unlock_irqrestore(&swtp->spinlock, flags);

__ERR_HANDLE__:

	if (ret < 0) {
		CCCI_LEGACY_ERR_LOG(swtp->md_id, SYS,
			"%s send tx power failed, ret=%d,rety_cnt=%d schedule delayed work\n",
			__func__, ret, swtp->retry_cnt);
		schedule_delayed_work(&swtp->delayed_work, 5 * HZ);
	}

	return ret;
}

static int swtp_switch_state(int irq, struct swtp_t *swtp)
{
	unsigned long flags;
	int i;

	if (swtp == NULL) {
		CCCI_LEGACY_ERR_LOG(-1, SYS, "%s:data is null\n", __func__);
		return -1;
	}

	spin_lock_irqsave(&swtp->spinlock, flags);
	for (i = 0; i < MAX_PIN_NUM; i++) {
		if (swtp->irq[i] == irq)
			break;
	}
	if (i == MAX_PIN_NUM) {
		spin_unlock_irqrestore(&swtp->spinlock, flags);
		CCCI_LEGACY_ERR_LOG(-1, SYS,
			"%s:can't find match irq\n", __func__);
		return -1;
	}

	if (swtp->eint_type[i] == IRQ_TYPE_LEVEL_LOW) {
		irq_set_irq_type(swtp->irq[i], IRQ_TYPE_LEVEL_HIGH);
		swtp->eint_type[i] = IRQ_TYPE_LEVEL_HIGH;
	} else {
		irq_set_irq_type(swtp->irq[i], IRQ_TYPE_LEVEL_LOW);
		swtp->eint_type[i] = IRQ_TYPE_LEVEL_LOW;
	}

	if (swtp->gpio_state[i] == SWTP_EINT_PIN_PLUG_IN)
		swtp->gpio_state[i] = SWTP_EINT_PIN_PLUG_OUT;
	else
		swtp->gpio_state[i] = SWTP_EINT_PIN_PLUG_IN;

	swtp->tx_power_mode = SWTP_NO_TX_POWER;
	for (i = 0; i < MAX_PIN_NUM; i++) {
		if (swtp->gpio_state[i] == SWTP_EINT_PIN_PLUG_IN) {
			swtp->tx_power_mode = SWTP_DO_TX_POWER;
			break;
		}
	}
	inject_pin_status_event(swtp->curr_mode, rf_name);
	spin_unlock_irqrestore(&swtp->spinlock, flags);

	return swtp->tx_power_mode;
}

static void swtp_send_tx_power_state(struct swtp_t *swtp)
{
	int ret = 0;

	if (!swtp) {
		CCCI_LEGACY_ERR_LOG(-1, SYS,
			"%s:swtp is null\n", __func__);
		return;
	}

	if (swtp->md_id == 0) {
		ret = swtp_send_tx_power(swtp);
		if (ret < 0) {
			CCCI_LEGACY_ERR_LOG(swtp->md_id, SYS,
				"%s send tx power failed, ret=%d, schedule delayed work\n",
				__func__, ret);
			schedule_delayed_work(&swtp->delayed_work, 5 * HZ);
		}
	} else
		CCCI_LEGACY_ERR_LOG(swtp->md_id, SYS,
			"%s:md is no support\n", __func__);

}

static irqreturn_t swtp_irq_handler(int irq, void *data)
{
	struct swtp_t *swtp = (struct swtp_t *)data;
	int ret = 0;

	ret = swtp_switch_mode(swtp);
	if (ret < 0) {
		CCCI_LEGACY_ERR_LOG(swtp->md_id, SYS,
			"%s swtp_switch_state failed in irq, ret=%d\n",
			__func__, ret);
	} else
		swtp_send_tx_power_state(swtp);

	return IRQ_HANDLED;
}

static void swtp_tx_delayed_work(struct work_struct *work)
{
	struct swtp_t *swtp = container_of(to_delayed_work(work),
		struct swtp_t, delayed_work);
	int ret, retry_cnt = 0;

	while (retry_cnt < MAX_RETRY_CNT) {
		ret = swtp_send_tx_power(swtp);
		if (ret != 0) {
			msleep(2000);
			retry_cnt++;
		} else
			break;
	}
}

int swtp_md_tx_power_req_hdlr(int md_id, int data)
{
	struct swtp_t *swtp = NULL;

	if (md_id < 0 || md_id >= SWTP_MAX_SUPPORT_MD) {
		CCCI_LEGACY_ERR_LOG(md_id, SYS,
		"%s:md_id=%d not support\n",
		__func__, md_id);
		return -1;
	}

	swtp = &swtp_data[md_id];
	swtp_send_tx_power_state(swtp);

	return 0;
}

static void swtp_init_delayed_work(struct work_struct *work)
{
	int ret = 0;
	/*Huaqin add for HQ-123513 by shiwenlong at 2021.4.01 start*/
	int ret1 = 0;
	/*Huaqin add for HQ-123513 by shiwenlong at 2021.4.01 end*/
	struct device_node *node = NULL;
#ifdef CONFIG_MTK_EIC
	u32 ints[2] = { 0, 0 };
#else
	u32 ints[1] = { 0 };
#endif

	if (md_id < 0 || md_id >= SWTP_MAX_SUPPORT_MD) {
		CCCI_LEGACY_ERR_LOG(-1, SYS,
			"invalid md_id = %d\n", md_id);
		return -1;
	}
	swtp_data[md_id].md_id = md_id;
	swtp_data[md_id].curr_mode = SWTP_EINT_PIN_PLUG_OUT;
	spin_lock_init(&swtp_data[md_id].spinlock);
	INIT_DELAYED_WORK(&swtp_data[md_id].delayed_work, swtp_tx_work);

	node = of_find_matching_node(NULL, swtp_of_match);
	if (node) {
		ret = of_property_read_u32_array(node, "debounce",
				ints, ARRAY_SIZE(ints));
		ret |= of_property_read_u32_array(node, "interrupts",
				ints1, ARRAY_SIZE(ints1));
		if (ret)
			CCCI_LEGACY_ERR_LOG(md_id, SYS,
				"%s get property fail\n", __func__);

	md_id = swtp->md_id;

	if (md_id < 0 || md_id >= SWTP_MAX_SUPPORT_MD) {
		ret = -2;
		CCCI_LEGACY_ERR_LOG(-1, SYS,
			"%s: invalid md_id = %d\n", __func__, md_id);
		goto SWTP_INIT_END;
	}

	for (i = 0; i < MAX_PIN_NUM; i++)
		swtp_data[md_id].gpio_state[i] = SWTP_EINT_PIN_PLUG_OUT;

	for (i = 0; i < MAX_PIN_NUM; i++) {
		node = of_find_matching_node(NULL, &swtp_of_match[i]);
		if (node) {
			ret = of_property_read_u32_array(node, "debounce",
				ints, ARRAY_SIZE(ints));
			if (ret) {
				CCCI_LEGACY_ERR_LOG(md_id, SYS,
					"%s:swtp%d get debounce fail\n",
					__func__, i);
				break;
			}

			ret = of_property_read_u32_array(node, "interrupts",
				ints1, ARRAY_SIZE(ints1));
			if (ret) {
				CCCI_LEGACY_ERR_LOG(md_id, SYS,
					"%s:swtp%d get interrupts fail\n",
					__func__, i);
				break;
			}
#ifdef CONFIG_MTK_EIC /* for chips before mt6739 */
		swtp_data[md_id].gpiopin = ints[0];
		swtp_data[md_id].setdebounce = ints[1];
		swtp_data[md_id].eint_type = ints1[1];
#else /* for mt6739,and chips after mt6739 */
		swtp_data[md_id].setdebounce = ints[0];
		swtp_data[md_id].gpiopin =
			of_get_named_gpio(node, "deb-gpios", 0);
		swtp_data[md_id].eint_type = ints1[1];
#endif
		gpio_set_debounce(swtp_data[md_id].gpiopin,
			swtp_data[md_id].setdebounce);
		swtp_data[md_id].irq = irq_of_parse_and_map(node, 0);
		ret = request_irq(swtp_data[md_id].irq, swtp_irq_func,
			IRQF_TRIGGER_NONE, "swtp-eint", &swtp_data[md_id]);
		if (ret != 0) {
			CCCI_LEGACY_ERR_LOG(md_id, SYS,
				"swtp-eint IRQ LINE NOT AVAILABLE\n");
		} else {
			CCCI_LEGACY_ALWAYS_LOG(md_id, SYS,
				"swtp-eint set EINT finished, irq=%d, setdebounce=%d, eint_type=%d\n",
				swtp_data[md_id].irq,
				swtp_data[md_id].setdebounce,
				swtp_data[md_id].eint_type);
		}
	} else {
		CCCI_LEGACY_ERR_LOG(md_id, SYS,
			"%s can't find compatible node\n", __func__);
		ret = -1;
	}
	register_ccci_sys_call_back(md_id, MD_SW_MD1_TX_POWER_REQ,
		swtp_md_tx_power_req_hdlr);
	/*Huaqin add for HQ-123513 by shiwenlong at 2021.4.01 start*/
	get_swtp_gpio = of_get_named_gpio(node, "swtp-gpio", 0);
	ret1 =  gpio_request(get_swtp_gpio, "swtp-gpio");
	if (ret1) {
		pr_err("gpio_request_one get_swtp_gpio(%d)=%d\n",get_swtp_gpio, ret1);
	}
	/*Huaqin add for HQ-130137 by wangrui at 2021.4.13 start*/
	swtp_gpio_status = proc_create("gpio_status", 0644, NULL, &swtp_gpio_status_ops);
	/*Huaqin add for HQ-130137 by wangrui at 2021.4.13 end*/
	if (swtp_gpio_status == NULL) {
		printk("tpd, create_proc_entry swtp_gpio_status_ops failed\n");
	}
	/*Huaqin add for HQ-123513 by shiwenlong at 2021.4.01 end*/
	return ret;
}

int swtp_init(int md_id)
{
	/* parameter check */
	if (md_id < 0 || md_id >= SWTP_MAX_SUPPORT_MD) {
		CCCI_LEGACY_ERR_LOG(-1, SYS,
			"%s: invalid md_id = %d\n", __func__, md_id);
		return -1;
	}
	/* init woke setting */
	swtp_data[md_id].md_id = md_id;

	INIT_DELAYED_WORK(&swtp_data[md_id].init_delayed_work,
		swtp_init_delayed_work);
	/* tx work setting */
	INIT_DELAYED_WORK(&swtp_data[md_id].delayed_work,
		swtp_tx_delayed_work);
	swtp_data[md_id].tx_power_mode = SWTP_NO_TX_POWER;

	spin_lock_init(&swtp_data[md_id].spinlock);

	/* schedule init work */
	schedule_delayed_work(&swtp_data[md_id].init_delayed_work, HZ);

	CCCI_BOOTUP_LOG(md_id, SYS, "%s end, init_delayed_work scheduled\n",
		__func__);
	return 0;
}
