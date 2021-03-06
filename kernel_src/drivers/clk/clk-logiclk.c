/*
 * Driver for Xylon logiCLK IP Core Programmable Clock Generator
 *
 * Copyright (C) 2014 Xylon d.o.o.
 * Author: Davor Joja <davor.joja@logicbricks.com>
 *
 * This software is licensed under the terms of the GNU General Public
 * License version 2, as published by the Free Software Foundation, and
 * may be copied, distributed, and modified under those terms.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 */

#include <linux/clk-provider.h>
#include <linux/delay.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/platform_device.h>

/* logiCLK registers */
#define LOGICLK_REG_STRIDE		4
#define LOGICLK_PLL_REG_OFF		1
#define LOGICLK_PLL_MAN_REG_OFF		3

#define LOGICLK_MANUAL_REGS		21

/* logiCLK PLL bits */
#define LOGICLK_PLL_LOCK		BIT(0)
#define LOGICLK_PLL_CONFIG		BIT(0)
#define LOGICLK_PLL_CONFIG_SW		BIT(1)

/* logiCLK parameters for 7 series */
#define MMCM_INPUT_FREQ_MIN		10000000UL
#define MMCM_INPUT_FREQ_MAX		800000000UL
#define MMCM_OUTPUT_FREQ_MIN		4690000UL
#define MMCM_OUTPUT_FREQ_MAX		800000000UL
#define MMCM_VCO_FREQ_MIN		600000000ULL
#define MMCM_VCO_FREQ_MAX		1600000000ULL

#define MMCM_PHASE_MIN			-360000L
#define MMCM_PHASE_MAX			360000L

#define MMCM_CLKOUT_DIVIDE_MIN		1
#define MMCM_CLKOUT_DIVIDE_MAX		128
#define MMCM_CLKOUT_DUTY_MIN		100
#define MMCM_CLKOUT_DUTY_MAX		99900
#define MMCM_CLKOUT_PHASE_MIN		MMCM_PHASE_MIN
#define MMCM_CLKOUT_PHASE_MAX		MMCM_PHASE_MAX
#define MMCM_DIVCLK_DIVIDE_MIN		1
#define MMCM_DIVCLK_DIVIDE_MAX		56
#define MMCM_FBOUT_PHASE_MIN		MMCM_PHASE_MIN
#define MMCM_FBOUT_PHASE_MAX		MMCM_PHASE_MAX
#define MMCM_FBOUT_MULTIPLY_MIN		2
#define MMCM_FBOUT_MULTIPLY_MAX		64

#define LOGICLK_FRACTION_PRECISION	10
#define LOGICLK_OUTPUTS			6

#define PLL_LOCK_TIME_MS		1
#define PLL_LOCK_TIME_INTERVALS		50

#define LOGICLK_CONFIG_SW		true
#define LOGICLK_CONFIG_HW		false
#define LOGICLK_STACK_PUSH		true
#define LOGICLK_STACK_POP		false

/**
 * struct logiclk_input:
 * @clk_freq:		Input clock frequency
 * @clkfbout_mult:	Input clock multiplier for changing VCO output frequency
 * @clkfbout_phase:	Input clock phase
 * @divclk_divide:	Input clock divider
 * @bw_high:		Bandwidth setting
 */
struct logiclk_input {
	u32 clk_freq;
	u32 clkfbout_mult;
	u32 clkfbout_phase;
	u32 divclk_divide;
	bool bw_high;
};

/**
 * struct logiclk_output:
 * @hw:			Clock hw
 * @data:		Pointer to parent structure
 * @clkout_freq:	Output clock frequency
 * @clkout_divide:	Output clock divider
 * @clkout_duty:	Output clock duty cycle
 * @clkout_phase:	Output clock phase
 * @precise:		Flag for precision clock
 * @id:			Output ID
 */
struct logiclk_output {
	struct clk_hw hw;
	struct device_node *dn;
	struct logiclk_data *data;
	u32 clkout_freq;
	u32 clkout_divide;
	u32 clkout_duty;
	u32 clkout_phase;
	u8 id;
	bool precise;
};

/**
 * struct logiclk_data:
 * @input:		Input clock configuration parameters
 * @output:		Output clock configuration parameters
 * @output_stack:	Output clock configuration parameters stack
 * @pdev:		Platform device
 * @base:		Registers base
 * @man_regs:		Manual registers
 * @man_regs_stack:	Manual registers stack
 */
struct logiclk_data {
	struct logiclk_input input;
	struct logiclk_output output[LOGICLK_OUTPUTS];
	struct logiclk_output output_stack;
	struct platform_device *pdev;
	void __iomem *base;
	u32 man_regs[LOGICLK_MANUAL_REGS];
	u32 man_regs_stack[LOGICLK_MANUAL_REGS];
};

#define to_logiclk_output(_hw) container_of(_hw, struct logiclk_output, hw)

static void logiclk_stack_params(struct logiclk_output *output, bool flag)
{
	struct logiclk_data *data = output->data;

	if (flag == LOGICLK_STACK_PUSH) {
		memcpy(&data->output_stack, output,
			sizeof(struct logiclk_output));
		memcpy(&data->man_regs_stack, data->man_regs,
		       (sizeof(u32) * LOGICLK_MANUAL_REGS));
	} else {
		memcpy(output, &data->output_stack,
			sizeof(struct logiclk_output));
		memcpy(data->man_regs, &data->man_regs_stack,
		       (sizeof(u32) * LOGICLK_MANUAL_REGS));
	}
}

static u32 logiclk_calc_freq(struct logiclk_output *output)
{
	struct logiclk_data *data = output->data;
	struct logiclk_input *input = &data->input;
	u64 clk_freq_mult = (u64)input->clk_freq * (u64)input->clkfbout_mult;
	u32 clk_freq_div = input->divclk_divide * output->clkout_divide;

	return (u32)(div_u64(clk_freq_mult, clk_freq_div));
}

static inline u32 logiclk_get_bits(u64 input, u32 msb, u32 lsb)
{
	return (u32)((input >> lsb) & ((1 << (msb - lsb + 1)) - 1));
}

static u32 logiclk_round_fraction(u32 decimal, u32 precision)
{
	unsigned int prec = 1 << (LOGICLK_FRACTION_PRECISION - precision - 1);

	if (decimal & prec)
		return (decimal + prec);
	else
		return decimal;
}

static u32 logiclk_pll_div(u32 divide, u32 duty)
{
	u32 duty_fix, edge, high_time, low_time, no_count, temp;

	duty_fix = (duty << LOGICLK_FRACTION_PRECISION) / 100000;

	if (divide == 1) {
		edge = 0;
		high_time = 1;
		low_time = 1;
		no_count = 1;
	} else {
		temp = logiclk_round_fraction(duty_fix * divide, 1);

		edge = logiclk_get_bits(temp,
					(LOGICLK_FRACTION_PRECISION - 1),
					(LOGICLK_FRACTION_PRECISION - 1));
		high_time = logiclk_get_bits(temp,
					     (LOGICLK_FRACTION_PRECISION + 6),
					     LOGICLK_FRACTION_PRECISION);

		if (high_time == 0) {
			edge = 0;
			high_time = 1;
		}
		if (high_time == divide) {
			edge = 1;
			high_time = divide - 1;
		}
		low_time = divide - high_time;
		no_count = 0;
	}

	return ((low_time & 0x3F) | ((high_time & 0x3F) << 6) |
		((no_count & 0x1) << 12) | ((edge & 0x1) << 13));
}

static u32 logiclk_pll_phase(u32 divide, s32 phase)
{
	u32 delay_time, phase_cycles, phase_fixed, phase_mux, temp;

	if (phase < 0)
		phase_fixed = ((phase + MMCM_PHASE_MAX) <<
			       LOGICLK_FRACTION_PRECISION) / 1000;
	else
		phase_fixed = (phase << LOGICLK_FRACTION_PRECISION) / 1000;

	phase_cycles = (phase_fixed * divide) / (MMCM_PHASE_MAX / 1000);

	temp = logiclk_round_fraction(phase_cycles, 3);

	delay_time = logiclk_get_bits(temp, (LOGICLK_FRACTION_PRECISION + 5),
				      LOGICLK_FRACTION_PRECISION);
	phase_mux = logiclk_get_bits(temp, (LOGICLK_FRACTION_PRECISION - 1),
				     (LOGICLK_FRACTION_PRECISION - 3));

	return (delay_time & 0x3F) | ((phase_mux & 0x7) << 6);
}

static u32 logiclk_pll_count(u32 divide, u32 duty, s32 phase)
{
	u32 pll_div = logiclk_pll_div(divide, duty);
	u32 pll_phase = logiclk_pll_phase(divide, phase);

	return (logiclk_get_bits(pll_div, 11, 0) |
		(logiclk_get_bits(pll_phase, 8, 6) << 13) |
		(logiclk_get_bits(pll_phase, 5, 0) << 16) |
		(logiclk_get_bits(pll_div, 13, 12) << 22) |
		(logiclk_get_bits(pll_phase, 10, 9) << 24));
}

static u32 logiclk_pll_lut_filter(u32 divide, bool bw_high)
{
	static const u32 lut_high[] = {
		0x17C,
		0x3FC,
		0x3F4,
		0x3E4,
		0x3F8,
		0x3C4,
		0x3C4,
		0x3D8,
		0x3E8,
		0x3E8,
		0x3E8,
		0x3B0,
		0x3F0,
		0x3F0,
		0x3F0,
		0x3F0,
		0x3F0,
		0x3F0,
		0x3F0,
		0x3F0,
		0x3B0,
		0x3B0,
		0x3B0,
		0x3E8,
		0x370,
		0x308,
		0x370,
		0x370,
		0x3E8,
		0x3E8,
		0x3E8,
		0x1C8,
		0x330,
		0x330,
		0x3A8,
		0x188,
		0x188,
		0x188,
		0x1F0,
		0x188,
		0x110,
		0x110,
		0x110,
		0x110,
		0x110,
		0x110,
		0xE0,
		0xE0,
		0xE0,
		0xE0,
		0xE0,
		0xE0,
		0xE0,
		0xE0,
		0xE0,
		0xE0,
		0xE0,
		0xE0,
		0xE0,
		0xE0,
		0xE0,
		0xE0,
		0xE0,
		0xE0
	};
	static const u32 lut_low[] = {
		0x5F,
		0x57,
		0x7B,
		0x5B,
		0x6B,
		0x73,
		0x73,
		0x73,
		0x73,
		0x4B,
		0x4B,
		0x4B,
		0xB3,
		0x53,
		0x53,
		0x53,
		0x53,
		0x53,
		0x53,
		0x53,
		0x53,
		0x53,
		0x53,
		0x63,
		0x63,
		0x63,
		0x63,
		0x63,
		0x63,
		0x63,
		0x63,
		0x63,
		0x63,
		0x63,
		0x63,
		0x63,
		0x63,
		0x93,
		0x93,
		0x93,
		0x93,
		0x93,
		0x93,
		0x93,
		0x93,
		0x93,
		0x93,
		0xA3,
		0xA3,
		0xA3,
		0xA3,
		0xA3,
		0xA3,
		0xA3,
		0xA3,
		0xA3,
		0xA3,
		0xA3,
		0xA3,
		0xA3,
		0xA3,
		0xA3,
		0xA3,
		0xA3
	};

	if (bw_high)
		return lut_high[divide - 1];
	else
		return lut_low[divide - 1];
}

static u64 logiclk_pll_lut_lock(u32 divide)
{
	static const u64 lut[] = {
		0x31BE8FA401ULL,
		0x31BE8FA401ULL,
		0x423E8FA401ULL,
		0x5AFE8FA401ULL,
		0x73BE8FA401ULL,
		0x8C7E8FA401ULL,
		0x9CFE8FA401ULL,
		0xB5BE8FA401ULL,
		0xCE7E8FA401ULL,
		0xE73E8FA401ULL,
		0xFFF84FA401ULL,
		0xFFF39FA401ULL,
		0xFFEEEFA401ULL,
		0xFFEBCFA401ULL,
		0xFFE8AFA401ULL,
		0xFFE71FA401ULL,
		0xFFE3FFA401ULL,
		0xFFE26FA401ULL,
		0xFFE0DFA401ULL,
		0xFFDF4FA401ULL,
		0xFFDDBFA401ULL,
		0xFFDC2FA401ULL,
		0xFFDA9FA401ULL,
		0xFFD90FA401ULL,
		0xFFD90FA401ULL,
		0xFFD77FA401ULL,
		0xFFD5EFA401ULL,
		0xFFD5EFA401ULL,
		0xFFD45FA401ULL,
		0xFFD45FA401ULL,
		0xFFD2CFA401ULL,
		0xFFD2CFA401ULL,
		0xFFD2CFA401ULL,
		0xFFD13FA401ULL,
		0xFFD13FA401ULL,
		0xFFD13FA401ULL,
		0xFFCFAFA401ULL,
		0xFFCFAFA401ULL,
		0xFFCFAFA401ULL,
		0xFFCFAFA401ULL,
		0xFFCFAFA401ULL,
		0xFFCFAFA401ULL,
		0xFFCFAFA401ULL,
		0xFFCFAFA401ULL,
		0xFFCFAFA401ULL,
		0xFFCFAFA401ULL,
		0xFFCFAFA401ULL,
		0xFFCFAFA401ULL,
		0xFFCFAFA401ULL,
		0xFFCFAFA401ULL,
		0xFFCFAFA401ULL,
		0xFFCFAFA401ULL,
		0xFFCFAFA401ULL,
		0xFFCFAFA401ULL,
		0xFFCFAFA401ULL,
		0xFFCFAFA401ULL,
		0xFFCFAFA401ULL,
		0xFFCFAFA401ULL,
		0xFFCFAFA401ULL,
		0xFFCFAFA401ULL,
		0xFFCFAFA401ULL,
		0xFFCFAFA401ULL,
		0xFFCFAFA401ULL,
		0xFFCFAFA401ULL
	};

	return lut[divide - 1];
}

static int logiclk_pll_input_mult_div(struct logiclk_output *output)
{
	struct logiclk_data *data = output->data;
	struct logiclk_input *input = &data->input;
	u64 clk_freq_input = input->clk_freq;
	u64 freq_err = ((u64)-1);
	u64 freq_out = output->clkout_freq;
	u64 clk_freq, clkfbout_mult, freq_err_new;
	u32 clkout_divide, divclk_divide;

	input->clkfbout_mult = 0;
	input->divclk_divide = 0;

	for (divclk_divide = MMCM_DIVCLK_DIVIDE_MIN;
	     divclk_divide <= MMCM_DIVCLK_DIVIDE_MAX;
	     divclk_divide++) {
		for (clkfbout_mult = MMCM_FBOUT_MULTIPLY_MIN;
		     clkfbout_mult <= MMCM_FBOUT_MULTIPLY_MAX;
		     clkfbout_mult++) {
			for (clkout_divide = MMCM_CLKOUT_DIVIDE_MIN;
			     clkout_divide <= MMCM_CLKOUT_DIVIDE_MAX;
			     clkout_divide++) {
				clk_freq = clk_freq_input * clkfbout_mult;
				clk_freq = div_u64(clk_freq, divclk_divide);

				if ((clk_freq < MMCM_VCO_FREQ_MIN) ||
				    (clk_freq > MMCM_VCO_FREQ_MAX))
					continue;

				clk_freq = div_u64(clk_freq, clkout_divide);
				freq_err_new = abs64((clk_freq - freq_out));

				if (freq_err_new < freq_err) {
					input->clkfbout_mult = clkfbout_mult;
					input->divclk_divide = divclk_divide;

					if (freq_err_new == 0)
						return 0;
					else
						freq_err = freq_err_new;
				}
			}
		}
	}

	if ((input->clkfbout_mult == 0) || (input->divclk_divide == 0))
		return -EINVAL;

	return 0;
}

static u32 logiclk_pll_output_div(struct logiclk_output *output)
{
	struct logiclk_data *data = output->data;
	struct logiclk_input *input = &data->input;
	u64 clk_freq_mult = (u64)input->clk_freq * (u64)input->clkfbout_mult;
	u64 freq_err = ((u64)-1);
	u64 freq_out = output->clkout_freq;
	u64 freq, freq_err_new, freq_mult_div;
	u32 clkout_div = 0;
	u32 clkout_divide;

	freq_mult_div = div_u64(clk_freq_mult, input->divclk_divide);

	for (clkout_divide = MMCM_CLKOUT_DIVIDE_MIN;
	     clkout_divide <= MMCM_CLKOUT_DIVIDE_MAX;
	     clkout_divide++) {
		freq = div_u64(freq_mult_div, clkout_divide);
		freq_err_new = abs64((freq - freq_out));

		if (freq_err_new < freq_err) {
			clkout_div = clkout_divide;

			if (freq_err_new == 0)
				return clkout_div;
			else
				freq_err = freq_err_new;
		}
	}

	return clkout_div;
}

static void logiclk_man_reg_params_id(struct logiclk_input *input,
				      struct logiclk_output *output,
				      unsigned int id)
{
	struct logiclk_data *data = output->data;
	u64 clk_freq_mult, clk_freq_mult_div;
	u32 clkout;

	clk_freq_mult = (u64)input->clk_freq * (u64)input->clkfbout_mult;
	clk_freq_mult_div = div_u64(clk_freq_mult, input->divclk_divide);

	output->clkout_divide = logiclk_pll_output_div(output);

	clkout = logiclk_pll_count(output->clkout_divide,
				   output->clkout_duty,
				   output->clkout_phase);

	data->man_regs[LOGICLK_PLL_REG_OFF + (id * 2)] =
		logiclk_get_bits(clkout, 15, 0);
	data->man_regs[(LOGICLK_PLL_REG_OFF + 1) + (id * 2)] =
		logiclk_get_bits(clkout, 31, 16);

	output->clkout_freq = (u32)div_u64(clk_freq_mult_div,
					   output->clkout_divide);
}

static void logiclk_man_reg_params(struct logiclk_input *input,
				   struct logiclk_output *output)
{
	struct logiclk_data *data = output->data;
	u64 lock;
	u32 clkfbout, divclk, filter;

	clkfbout = logiclk_pll_count(input->clkfbout_mult,
				     output->clkout_duty,
				     input->clkfbout_phase);
	divclk = logiclk_pll_count(input->divclk_divide,
				   output->clkout_duty,
				   output->clkout_phase);

	filter = logiclk_pll_lut_filter((input->clkfbout_mult - 1),
					input->bw_high);

	lock = logiclk_pll_lut_lock(input->clkfbout_mult - 1);

	data->man_regs[0] = 0xFFFF;

	data->man_regs[13] = (logiclk_get_bits(divclk, 23, 22) << 12) |
			     (logiclk_get_bits(divclk, 11, 0) << 0);
	data->man_regs[14] = logiclk_get_bits(clkfbout, 15, 0);
	data->man_regs[15] = logiclk_get_bits(clkfbout, 31, 16);
	data->man_regs[16] = logiclk_get_bits(lock, 29, 20);
	data->man_regs[17] = (logiclk_get_bits(lock, 34, 30) << 10) |
			     logiclk_get_bits(lock, 9, 0);
	data->man_regs[18] = (logiclk_get_bits(lock, 39, 35) << 10) |
			     logiclk_get_bits(lock, 19, 10);
	data->man_regs[19] = (logiclk_get_bits(filter, 6, 6) << 8)  |
			     (logiclk_get_bits(filter, 8, 7) << 11) |
			     (logiclk_get_bits(filter, 9, 9) << 15);
	data->man_regs[20] = (logiclk_get_bits(filter, 0, 0) << 4)  |
			     (logiclk_get_bits(filter, 2, 1) << 7)  |
			     (logiclk_get_bits(filter, 4, 3) << 11) |
			     (logiclk_get_bits(filter, 5, 5) << 15);
}

static int logiclk_calc_params(struct logiclk_output *output)
{
	struct logiclk_data *data = output->data;
	struct logiclk_input *input = &data->input;
	struct device *dev = &data->pdev->dev;
	int i, ret;

	if ((output->clkout_freq < MMCM_OUTPUT_FREQ_MIN) ||
	    (output->clkout_freq > MMCM_OUTPUT_FREQ_MAX)) {
		dev_err(dev, "invalid output frequency %u Hz\n",
			output->clkout_freq);
		return -EINVAL;
	}

	if (output->precise) {
		ret = logiclk_pll_input_mult_div(output);
		if (ret)
			return ret;
		logiclk_man_reg_params(input, output);
		/*
		 * recalculate all output parameters with new input
		 * multiplier and divider
		 */
		for (i = 0; i < LOGICLK_OUTPUTS; i++)
			logiclk_man_reg_params_id(input, &data->output[i],
						  data->output[i].id);
	} else {
		logiclk_man_reg_params_id(input, output, output->id);
	}

	return 0;
}

static int logiclk_hw_config(struct logiclk_output *output, bool config)
{
	struct logiclk_data *data = output->data;
	struct device *dev = &data->pdev->dev;
	int cnt = PLL_LOCK_TIME_INTERVALS;
	u32 cfg = LOGICLK_PLL_CONFIG;
	u32 val;
	int i;

	if (config == LOGICLK_CONFIG_SW) {
		for (i = 0; i < LOGICLK_MANUAL_REGS; i++)
			clk_writel(data->man_regs[i],
				   (data->base +
				   ((i + LOGICLK_PLL_MAN_REG_OFF) *
				   LOGICLK_REG_STRIDE)));

		cfg |= LOGICLK_PLL_CONFIG_SW;
	}

	while (1) {
		val = clk_readl(data->base + (LOGICLK_PLL_REG_OFF *
				LOGICLK_REG_STRIDE));
		if (val & LOGICLK_PLL_LOCK) {
			clk_writel(cfg, (data->base + (LOGICLK_PLL_REG_OFF *
				   LOGICLK_REG_STRIDE)));
			break;
		}
		if (cnt-- == 0) {
			dev_err(dev, "failed pll lock\n");
			return -EIO;
		}
		mdelay(PLL_LOCK_TIME_MS);
	}

	return 0;
}

static unsigned long logiclk_recalc_rate(struct clk_hw *hw,
					 unsigned long parent_rate)
{
	struct logiclk_output *output = to_logiclk_output(hw);
	struct logiclk_data *data = output->data;
	struct logiclk_input *input = &data->input;
	u64 clk_freq_mult = (u64)input->clk_freq * (u64)input->clkfbout_mult;
	unsigned long rate;

	if (output->clkout_freq == 0) {
		/*
		 * Instead IO access, take parameters from struct logiclk_data.
		 * The same parameters are set in logiCLK hw registers.
		 */
		rate = (unsigned long)(div_u64(clk_freq_mult,
					       input->divclk_divide));
		output->clkout_freq = (rate / output->clkout_divide);
	}

	logiclk_man_reg_params(input, output);
	logiclk_man_reg_params_id(input, output, output->id);

	return (unsigned long)output->clkout_freq;
}

static long logiclk_round_rate(struct clk_hw *hw, unsigned long rate,
			       unsigned long *parent_rate)
{
	struct logiclk_output *output = to_logiclk_output(hw);
	struct logiclk_data *data = output->data;
	struct device *dev = &data->pdev->dev;

	logiclk_stack_params(output, LOGICLK_STACK_PUSH);

	output->clkout_freq = rate;

	if (logiclk_calc_params(output)) {
		dev_err(dev, "failed parameters calculation\n");
		logiclk_stack_params(output, LOGICLK_STACK_POP);
		return -EINVAL;
	}

	return (long)output->clkout_freq;
}

static int logiclk_set_rate(struct clk_hw *hw, unsigned long rate,
			    unsigned long parent_rate)
{
	struct logiclk_output *output = to_logiclk_output(hw);
	struct logiclk_data *data = output->data;
	struct device *dev = &data->pdev->dev;

	if (rate != output->clkout_freq) {
		logiclk_stack_params(output, LOGICLK_STACK_PUSH);
		output->clkout_freq = rate;
		if (logiclk_calc_params(output)) {
			dev_err(dev, "failed parameters calculation\n");
			logiclk_stack_params(output, LOGICLK_STACK_POP);
			return -EINVAL;
		}
	}

	return logiclk_hw_config(output, LOGICLK_CONFIG_SW);
}

static const struct clk_ops logiclk_clk_ops = {
	.recalc_rate = logiclk_recalc_rate,
	.round_rate = logiclk_round_rate,
	.set_rate = logiclk_set_rate,
};

static int logiclk_get_of_config(struct device_node *dn,
				 struct logiclk_data *data, bool *set_freq)
{
	struct device *dev = &data->pdev->dev;
	struct device_node *output_dn = NULL;
	struct logiclk_input *input = &data->input;
	struct logiclk_output *output = data->output;
	struct device_node *precise_dn;
	int i, err, outputs;

	outputs = of_get_child_count(dn);
	if (outputs != LOGICLK_OUTPUTS) {
		dev_err(dev, "invalid outputs number\n");
		return -EINVAL;
	}

	err = of_property_read_u32(dn, "input-frequency", &input->clk_freq);
	if (err) {
		dev_err(dev, "failed get input-frequency\n");
		return err;
	}
	if ((input->clk_freq < MMCM_INPUT_FREQ_MIN) ||
	    (input->clk_freq > MMCM_INPUT_FREQ_MAX)) {
		dev_err(dev, "invalid input frequency\n");
		return -EINVAL;
	}

	err = of_property_read_u32(dn, "input-divide", &input->divclk_divide);
	if (err) {
		dev_err(dev, "failed get input-divide\n");
		return err;
	}
	if ((input->divclk_divide < MMCM_DIVCLK_DIVIDE_MIN) ||
	    (input->divclk_divide > MMCM_DIVCLK_DIVIDE_MAX)) {
		dev_err(dev, "invalid input divide\n");
		return -EINVAL;
	}

	err = of_property_read_u32(dn, "input-multiply", &input->clkfbout_mult);
	if (err) {
		dev_err(dev, "failed get input-multiply\n");
		return err;
	}
	if ((input->clkfbout_mult < MMCM_FBOUT_MULTIPLY_MIN) ||
	    (input->clkfbout_mult > MMCM_FBOUT_MULTIPLY_MAX)) {
		dev_err(dev, "invalid input multiply\n");
		return -EINVAL;
	}

	err = of_property_read_u32(dn, "input-phase", &input->clkfbout_phase);
	if (err) {
		dev_err(dev, "failed get input-phase\n");
		return err;
	}
	if (((s32)input->clkfbout_phase < MMCM_FBOUT_PHASE_MIN) ||
	    (input->clkfbout_phase > MMCM_FBOUT_PHASE_MAX)) {
		dev_err(dev, "invalid input phase\n");
		return -EINVAL;
	}

	if (of_property_read_bool(dn, "bandwidth-high"))
		input->bw_high = true;

	precise_dn = of_parse_phandle(dn, "precise-output", 0);
	if (!precise_dn) {
		dev_err(dev, "failed get precise-output\n");
		return -EINVAL;
	}

	for (i = 0; i < outputs; i++) {
		output_dn = of_get_next_child(dn, output_dn);
		if (!output_dn)
			break;

		output[i].dn = output_dn;
		output[i].data = data;
		output[i].id = i;

		if (output_dn == precise_dn)
			output[i].precise = true;

		of_property_read_u32(output_dn, "frequency",
				     &output[i].clkout_freq);
		if ((output[i].clkout_freq != 0) &&
		    ((output[i].clkout_freq < MMCM_OUTPUT_FREQ_MIN) ||
		    (output[i].clkout_freq > MMCM_OUTPUT_FREQ_MAX))) {
			dev_warn(dev, "unsupported output frequency\n");
			output[i].clkout_freq = 0;
		}
		if (output[i].clkout_freq != 0)
			*set_freq = true;

		err = of_property_read_u32(output_dn, "divide",
					   &output[i].clkout_divide);
		if (err) {
			dev_err(dev, "failed get output %d divide\n", i);
			goto clk_node_err;
		}
		if ((output[i].clkout_divide < MMCM_CLKOUT_DIVIDE_MIN) ||
		    (output[i].clkout_divide > MMCM_CLKOUT_DIVIDE_MAX)) {
			dev_err(dev, "invalid output %d divide\n", i);
			goto clk_node_err;
		}

		err = of_property_read_u32(output_dn, "duty",
					   &output[i].clkout_duty);
		if (err) {
			dev_err(dev, "failed get output %d duty\n", i);
			goto clk_node_err;
		}
		if ((output[i].clkout_duty < MMCM_CLKOUT_DUTY_MIN) ||
		    (output[i].clkout_duty > MMCM_CLKOUT_DUTY_MAX)) {
			dev_err(dev, "invalid output %d duty\n", i);
			goto clk_node_err;
		}

		err = of_property_read_u32(output_dn, "phase",
					   &output[i].clkout_phase);
		if (err) {
			dev_err(dev, "failed get output %d phase\n", i);
			goto clk_node_err;
		}
		if (((int)output[i].clkout_phase < MMCM_CLKOUT_PHASE_MIN)
			||
		    (output[i].clkout_phase > MMCM_CLKOUT_PHASE_MAX)) {
			dev_err(dev, "invalid output %d phase\n", i);
			goto clk_node_err;
		}

clk_node_err:
		of_node_put(output_dn);
	}

	if (i != LOGICLK_OUTPUTS)
		return -EINVAL;

	return 0;
}

static int logiclk_probe(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	struct device_node *dn = pdev->dev.of_node;
	struct clk_init_data init;
	struct clk *clk;
	struct logiclk_data *data;
	struct resource *res;
	void __iomem *base;
	int i, err;
	int prec_id = 0;
	char name[10];
	bool set_freq = false;

	res = platform_get_resource(pdev, IORESOURCE_MEM, 0);
	base = devm_ioremap_resource(dev, res);
	if (IS_ERR(base))
		return PTR_ERR(base);

	data = devm_kzalloc(dev, sizeof(*data), GFP_KERNEL);
	if (!data) {
		dev_err(dev, "failed allocate internal data\n");
		return -ENOMEM;
	}
	data->base = base;
	data->pdev = pdev;

	dev_set_drvdata(dev, data);

	err = logiclk_get_of_config(dn, data, &set_freq);
	if (err)
		return err;

	memset(&init, 0, sizeof(init));

	init.name = name;
	init.ops = &logiclk_clk_ops;
	init.flags = CLK_IS_ROOT;

	for (i = 0; i < LOGICLK_OUTPUTS; i++) {
		sprintf(name, "clkout_%d", i);

		data->output[i].hw.init = &init;

		clk = devm_clk_register(dev, &data->output[i].hw);
		if (IS_ERR(clk)) {
			dev_err(dev, "failed clk register\n");
			err = PTR_ERR(clk);
			goto err_clk;
		}
		err = of_clk_add_provider(data->output[i].dn,
					  of_clk_src_simple_get, clk);
		if (err) {
			dev_err(dev, "failed clk add provider\n");
			goto err_clk;
		}

		if (data->output[i].precise)
			prec_id = i;
	}

	dev_info(dev, "precise output frequency %u Hz\n",
		 logiclk_calc_freq(&data->output[prec_id]));

	if (set_freq) {
		if (logiclk_calc_params(&data->output[prec_id])) {
			dev_err(dev, "failed parameters calculation\n");
			goto err_clk;
		}
		logiclk_hw_config(&data->output[prec_id], LOGICLK_CONFIG_SW);
	}

	return 0;

err_clk:
	for (--i; i >= 0; i--)
		of_clk_del_provider(data->output[i].dn);

	return err;
}

static int logiclk_remove(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	struct logiclk_data *data = dev_get_drvdata(dev);
	int i;

	for (i = (LOGICLK_OUTPUTS - 1); i >= 0; i--)
		of_clk_del_provider(data->output[i].dn);

	return 0;
}

static const struct of_device_id logiclk_of_match[] = {
	{ .compatible = "xylon,logiclk-1.02.b" },
	{ },
};
MODULE_DEVICE_TABLE(of, logiclk_of_match);

static struct platform_driver logiclk_driver = {
	.driver = {
		.name = "logiclk",
		.of_match_table = logiclk_of_match,
	},
	.probe = logiclk_probe,
	.remove = logiclk_remove,
};
module_platform_driver(logiclk_driver);

MODULE_DESCRIPTION("logiCLK clock generator driver");
MODULE_LICENSE("GPL");
MODULE_VERSION("1.0");
