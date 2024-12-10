// SPDX-License-Identifier: GPL-2.0-only
/* gain-time-scale conversion helpers for IIO light sensors
 *
 * Copyright (c) 2023 Matti Vaittinen <mazziesaccount@gmail.com>
 */

#include <linux/device.h>
#include <linux/errno.h>
#include <linux/export.h>
#include <linux/minmax.h>
#include <linux/module.h>
#include <linux/overflow.h>
#include <linux/slab.h>
#include <linux/sort.h>
#include <linux/types.h>
#include <linux/units.h>

#include <linux/iio/iio-gts-helper.h>
#include <linux/iio/types.h>

/**
 * iio_gts_get_gain - Convert scale to total gain
 *
 * Internal helper for converting scale to total gain.
 *
 * @max:	Maximum linearized scale. As an example, when scale is created
 *		in magnitude of NANOs and max scale is 64.1 - The linearized
 *		scale is 64 100 000 000.
 * @scale:	Linearized scale to compute the gain for.
 *
 * Return:	(floored) gain corresponding to the scale. -EINVAL if scale
 *		is invalid.
 */
static int iio_gts_get_gain(const u64 max, const u64 scale)
{
	u64 full = max;

	if (scale > full || !scale)
		return -EINVAL;

	return div64_u64(full, scale);
}

/**
 * gain_get_scale_fraction - get the gain or time based on scale and known one
 *
 * @max:	Maximum linearized scale. As an example, when scale is created
 *		in magnitude of NANOs and max scale is 64.1 - The linearized
 *		scale is 64 100 000 000.
 * @scale:	Linearized scale to compute the gain/time for.
 * @known:	Either integration time or gain depending on which one is known
 * @unknown:	Pointer to variable where the computed gain/time is stored
 *
 * Internal helper for computing unknown fraction of total gain.
 * Compute either gain or time based on scale and either the gain or time
 * depending on which one is known.
 *
 * Return:	0 on success.
 */
static int gain_get_scale_fraction(const u64 max, u64 scale, int known,
				   int *unknown)
{
	int tot_gain;

	tot_gain = iio_gts_get_gain(max, scale);
	if (tot_gain < 0)
		return tot_gain;

	*unknown = tot_gain / known;

	/* We require total gain to be exact multiple of known * unknown */
	if (!*unknown || *unknown * known != tot_gain)
		return -EINVAL;

	return 0;
}

static int iio_gts_delinearize(u64 lin_scale, unsigned long scaler,
			       int *scale_whole, int *scale_nano)
{
	int frac;

	if (scaler > NANO)
		return -EOVERFLOW;

	if (!scaler)
		return -EINVAL;

	frac = do_div(lin_scale, scaler);

	*scale_whole = lin_scale;
	*scale_nano = frac * (NANO / scaler);

	return 0;
}

static int iio_gts_linearize(int scale_whole, int scale_nano,
			     unsigned long scaler, u64 *lin_scale)
{
	/*
	 * Expect scale to be (mostly) NANO or MICRO. Divide divider instead of
	 * multiplication followed by division to avoid overflow.
	 */
	if (scaler > NANO || !scaler)
		return -EINVAL;

	*lin_scale = (u64)scale_whole * (u64)scaler +
		     (u64)(scale_nano / (NANO / scaler));

	return 0;
}

/**
 * iio_gts_total_gain_to_scale - convert gain to scale
 * @gts:	Gain time scale descriptor
 * @total_gain:	the gain to be converted
 * @scale_int:	Pointer to integral part of the scale (typically val1)
 * @scale_nano:	Pointer to fractional part of the scale (nano or ppb)
 *
 * Convert the total gain value to scale. NOTE: This does not separate gain
 * generated by HW-gain or integration time. It is up to caller to decide what
 * part of the total gain is due to integration time and what due to HW-gain.
 *
 * Return: 0 on success. Negative errno on failure.
 */
int iio_gts_total_gain_to_scale(struct iio_gts *gts, int total_gain,
				int *scale_int, int *scale_nano)
{
	u64 tmp;

	tmp = gts->max_scale;

	do_div(tmp, total_gain);

	return iio_gts_delinearize(tmp, NANO, scale_int, scale_nano);
}
EXPORT_SYMBOL_NS_GPL(iio_gts_total_gain_to_scale, IIO_GTS_HELPER);

/**
 * iio_gts_purge_avail_scale_table - free-up the available scale tables
 * @gts:	Gain time scale descriptor
 *
 * Free the space reserved by iio_gts_build_avail_scale_table().
 */
static void iio_gts_purge_avail_scale_table(struct iio_gts *gts)
{
	int i;

	if (gts->per_time_avail_scale_tables) {
		for (i = 0; i < gts->num_itime; i++)
			kfree(gts->per_time_avail_scale_tables[i]);

		kfree(gts->per_time_avail_scale_tables);
		gts->per_time_avail_scale_tables = NULL;
	}

	kfree(gts->avail_all_scales_table);
	gts->avail_all_scales_table = NULL;

	gts->num_avail_all_scales = 0;
}

static int iio_gts_gain_cmp(const void *a, const void *b)
{
	return *(int *)a - *(int *)b;
}

static int gain_to_scaletables(struct iio_gts *gts, int **gains, int **scales)
{
	int ret, i, j, new_idx, time_idx;
	int *all_gains;
	size_t gain_bytes;

	for (i = 0; i < gts->num_itime; i++) {
		/*
		 * Sort the tables for nice output and for easier finding of
		 * unique values.
		 */
		sort(gains[i], gts->num_hwgain, sizeof(int), iio_gts_gain_cmp,
		     NULL);

		/* Convert gains to scales */
		for (j = 0; j < gts->num_hwgain; j++) {
			ret = iio_gts_total_gain_to_scale(gts, gains[i][j],
							  &scales[i][2 * j],
							  &scales[i][2 * j + 1]);
			if (ret)
				return ret;
		}
	}

	gain_bytes = array_size(gts->num_hwgain, sizeof(int));
	all_gains = kcalloc(gts->num_itime, gain_bytes, GFP_KERNEL);
	if (!all_gains)
		return -ENOMEM;

	/*
	 * We assume all the gains for same integration time were unique.
	 * It is likely the first time table had greatest time multiplier as
	 * the times are in the order of preference and greater times are
	 * usually preferred. Hence we start from the last table which is likely
	 * to have the smallest total gains.
	 */
	time_idx = gts->num_itime - 1;
	memcpy(all_gains, gains[time_idx], gain_bytes);
	new_idx = gts->num_hwgain;

	while (time_idx--) {
		for (j = 0; j < gts->num_hwgain; j++) {
			int candidate = gains[time_idx][j];
			int chk;

			if (candidate > all_gains[new_idx - 1]) {
				all_gains[new_idx] = candidate;
				new_idx++;

				continue;
			}
			for (chk = 0; chk < new_idx; chk++)
				if (candidate <= all_gains[chk])
					break;

			if (candidate == all_gains[chk])
				continue;

			memmove(&all_gains[chk + 1], &all_gains[chk],
				(new_idx - chk) * sizeof(int));
			all_gains[chk] = candidate;
			new_idx++;
		}
	}

	gts->avail_all_scales_table = kcalloc(new_idx, 2 * sizeof(int),
					      GFP_KERNEL);
	if (!gts->avail_all_scales_table) {
		ret = -ENOMEM;
		goto free_out;
	}
	gts->num_avail_all_scales = new_idx;

	for (i = 0; i < gts->num_avail_all_scales; i++) {
		ret = iio_gts_total_gain_to_scale(gts, all_gains[i],
					&gts->avail_all_scales_table[i * 2],
					&gts->avail_all_scales_table[i * 2 + 1]);

		if (ret) {
			kfree(gts->avail_all_scales_table);
			gts->num_avail_all_scales = 0;
			goto free_out;
		}
	}

free_out:
	kfree(all_gains);

	return ret;
}

/**
 * iio_gts_build_avail_scale_table - create tables of available scales
 * @gts:	Gain time scale descriptor
 *
 * Build the tables which can represent the available scales based on the
 * originally given gain and time tables. When both time and gain tables are
 * given this results:
 * 1. A set of tables representing available scales for each supported
 *    integration time.
 * 2. A single table listing all the unique scales that any combination of
 *    supported gains and times can provide.
 *
 * NOTE: Space allocated for the tables must be freed using
 * iio_gts_purge_avail_scale_table() when the tables are no longer needed.
 *
 * Return: 0 on success.
 */
static int iio_gts_build_avail_scale_table(struct iio_gts *gts)
{
	int **per_time_gains, **per_time_scales, i, j, ret = -ENOMEM;

	per_time_gains = kcalloc(gts->num_itime, sizeof(*per_time_gains), GFP_KERNEL);
	if (!per_time_gains)
		return ret;

	per_time_scales = kcalloc(gts->num_itime, sizeof(*per_time_scales), GFP_KERNEL);
	if (!per_time_scales)
		goto free_gains;

	for (i = 0; i < gts->num_itime; i++) {
		per_time_scales[i] = kcalloc(gts->num_hwgain, 2 * sizeof(int),
					     GFP_KERNEL);
		if (!per_time_scales[i])
			goto err_free_out;

		per_time_gains[i] = kcalloc(gts->num_hwgain, sizeof(int),
					    GFP_KERNEL);
		if (!per_time_gains[i]) {
			kfree(per_time_scales[i]);
			goto err_free_out;
		}

		for (j = 0; j < gts->num_hwgain; j++)
			per_time_gains[i][j] = gts->hwgain_table[j].gain *
					       gts->itime_table[i].mul;
	}

	ret = gain_to_scaletables(gts, per_time_gains, per_time_scales);
	if (ret)
		goto err_free_out;

	for (i = 0; i < gts->num_itime; i++)
		kfree(per_time_gains[i]);
	kfree(per_time_gains);
	gts->per_time_avail_scale_tables = per_time_scales;

	return 0;

err_free_out:
	for (i--; i >= 0; i--) {
		kfree(per_time_scales[i]);
		kfree(per_time_gains[i]);
	}
	kfree(per_time_scales);
free_gains:
	kfree(per_time_gains);

	return ret;
}

static void iio_gts_us_to_int_micro(int *time_us, int *int_micro_times,
				    int num_times)
{
	int i;

	for (i = 0; i < num_times; i++) {
		int_micro_times[i * 2] = time_us[i] / 1000000;
		int_micro_times[i * 2 + 1] = time_us[i] % 1000000;
	}
}

/**
 * iio_gts_build_avail_time_table - build table of available integration times
 * @gts:	Gain time scale descriptor
 *
 * Build the table which can represent the available times to be returned
 * to users using the read_avail-callback.
 *
 * NOTE: Space allocated for the tables must be freed using
 * iio_gts_purge_avail_time_table() when the tables are no longer needed.
 *
 * Return: 0 on success.
 */
static int iio_gts_build_avail_time_table(struct iio_gts *gts)
{
	int *times, i, j, idx = 0, *int_micro_times;

	if (!gts->num_itime)
		return 0;

	times = kcalloc(gts->num_itime, sizeof(int), GFP_KERNEL);
	if (!times)
		return -ENOMEM;

	/* Sort times from all tables to one and remove duplicates */
	for (i = gts->num_itime - 1; i >= 0; i--) {
		int new = gts->itime_table[i].time_us;

		if (idx == 0 || times[idx - 1] < new) {
			times[idx++] = new;
			continue;
		}

		for (j = 0; j < idx; j++) {
			if (times[j] == new)
				break;
			if (times[j] > new) {
				memmove(&times[j + 1], &times[j],
					(idx - j) * sizeof(int));
				times[j] = new;
				idx++;
				break;
			}
		}
	}

	/* create a list of times formatted as list of IIO_VAL_INT_PLUS_MICRO */
	int_micro_times = kcalloc(idx, sizeof(int) * 2, GFP_KERNEL);
	if (int_micro_times) {
		/*
		 * This is just to survive a unlikely corner-case where times in
		 * the given time table were not unique. Else we could just
		 * trust the gts->num_itime.
		 */
		gts->num_avail_time_tables = idx;
		iio_gts_us_to_int_micro(times, int_micro_times, idx);
	}

	gts->avail_time_tables = int_micro_times;
	kfree(times);

	if (!int_micro_times)
		return -ENOMEM;

	return 0;
}

/**
 * iio_gts_purge_avail_time_table - free-up the available integration time table
 * @gts:	Gain time scale descriptor
 *
 * Free the space reserved by iio_gts_build_avail_time_table().
 */
static void iio_gts_purge_avail_time_table(struct iio_gts *gts)
{
	if (gts->num_avail_time_tables) {
		kfree(gts->avail_time_tables);
		gts->avail_time_tables = NULL;
		gts->num_avail_time_tables = 0;
	}
}

/**
 * iio_gts_build_avail_tables - create tables of available scales and int times
 * @gts:	Gain time scale descriptor
 *
 * Build the tables which can represent the available scales and available
 * integration times. Availability tables are built based on the originally
 * given gain and given time tables.
 *
 * When both time and gain tables are
 * given this results:
 * 1. A set of sorted tables representing available scales for each supported
 *    integration time.
 * 2. A single sorted table listing all the unique scales that any combination
 *    of supported gains and times can provide.
 * 3. A sorted table of supported integration times
 *
 * After these tables are built one can use the iio_gts_all_avail_scales(),
 * iio_gts_avail_scales_for_time() and iio_gts_avail_times() helpers to
 * implement the read_avail operations.
 *
 * NOTE: Space allocated for the tables must be freed using
 * iio_gts_purge_avail_tables() when the tables are no longer needed.
 *
 * Return: 0 on success.
 */
static int iio_gts_build_avail_tables(struct iio_gts *gts)
{
	int ret;

	ret = iio_gts_build_avail_scale_table(gts);
	if (ret)
		return ret;

	ret = iio_gts_build_avail_time_table(gts);
	if (ret)
		iio_gts_purge_avail_scale_table(gts);

	return ret;
}

/**
 * iio_gts_purge_avail_tables - free-up the availability tables
 * @gts:	Gain time scale descriptor
 *
 * Free the space reserved by iio_gts_build_avail_tables(). Frees both the
 * integration time and scale tables.
 */
static void iio_gts_purge_avail_tables(struct iio_gts *gts)
{
	iio_gts_purge_avail_time_table(gts);
	iio_gts_purge_avail_scale_table(gts);
}

static void devm_iio_gts_avail_all_drop(void *res)
{
	iio_gts_purge_avail_tables(res);
}

/**
 * devm_iio_gts_build_avail_tables - manged add availability tables
 * @dev:	Pointer to the device whose lifetime tables are bound
 * @gts:	Gain time scale descriptor
 *
 * Build the tables which can represent the available scales and available
 * integration times. Availability tables are built based on the originally
 * given gain and given time tables.
 *
 * When both time and gain tables are given this results:
 * 1. A set of sorted tables representing available scales for each supported
 *    integration time.
 * 2. A single sorted table listing all the unique scales that any combination
 *    of supported gains and times can provide.
 * 3. A sorted table of supported integration times
 *
 * After these tables are built one can use the iio_gts_all_avail_scales(),
 * iio_gts_avail_scales_for_time() and iio_gts_avail_times() helpers to
 * implement the read_avail operations.
 *
 * The tables are automatically released upon device detach.
 *
 * Return: 0 on success.
 */
static int devm_iio_gts_build_avail_tables(struct device *dev,
					   struct iio_gts *gts)
{
	int ret;

	ret = iio_gts_build_avail_tables(gts);
	if (ret)
		return ret;

	return devm_add_action_or_reset(dev, devm_iio_gts_avail_all_drop, gts);
}

static int sanity_check_time(const struct iio_itime_sel_mul *t)
{
	if (t->sel < 0 || t->time_us < 0 || t->mul <= 0)
		return -EINVAL;

	return 0;
}

static int sanity_check_gain(const struct iio_gain_sel_pair *g)
{
	if (g->sel < 0 || g->gain <= 0)
		return -EINVAL;

	return 0;
}

static int iio_gts_sanity_check(struct iio_gts *gts)
{
	int g, t, ret;

	if (!gts->num_hwgain && !gts->num_itime)
		return -EINVAL;

	for (t = 0; t < gts->num_itime; t++) {
		ret = sanity_check_time(&gts->itime_table[t]);
		if (ret)
			return ret;
	}

	for (g = 0; g < gts->num_hwgain; g++) {
		ret = sanity_check_gain(&gts->hwgain_table[g]);
		if (ret)
			return ret;
	}

	for (g = 0; g < gts->num_hwgain; g++) {
		for (t = 0; t < gts->num_itime; t++) {
			int gain, mul, res;

			gain = gts->hwgain_table[g].gain;
			mul = gts->itime_table[t].mul;

			if (check_mul_overflow(gain, mul, &res))
				return -EOVERFLOW;
		}
	}

	return 0;
}

static int iio_init_iio_gts(int max_scale_int, int max_scale_nano,
			const struct iio_gain_sel_pair *gain_tbl, int num_gain,
			const struct iio_itime_sel_mul *tim_tbl, int num_times,
			struct iio_gts *gts)
{
	int ret;

	memset(gts, 0, sizeof(*gts));

	ret = iio_gts_linearize(max_scale_int, max_scale_nano, NANO,
				   &gts->max_scale);
	if (ret)
		return ret;

	gts->hwgain_table = gain_tbl;
	gts->num_hwgain = num_gain;
	gts->itime_table = tim_tbl;
	gts->num_itime = num_times;

	return iio_gts_sanity_check(gts);
}

/**
 * devm_iio_init_iio_gts - Initialize the gain-time-scale helper
 * @dev:		Pointer to the device whose lifetime gts resources are
 *			bound
 * @max_scale_int:	integer part of the maximum scale value
 * @max_scale_nano:	fraction part of the maximum scale value
 * @gain_tbl:		table describing supported gains
 * @num_gain:		number of gains in the gain table
 * @tim_tbl:		table describing supported integration times. Provide
 *			the integration time table sorted so that the preferred
 *			integration time is in the first array index. The search
 *			functions like the
 *			iio_gts_find_time_and_gain_sel_for_scale() start search
 *			from first provided time.
 * @num_times:		number of times in the time table
 * @gts:		pointer to the helper struct
 *
 * Initialize the gain-time-scale helper for use. Note, gains, times, selectors
 * and multipliers must be positive. Negative values are reserved for error
 * checking. The total gain (maximum gain * maximum time multiplier) must not
 * overflow int. The allocated resources will be released upon device detach.
 *
 * Return: 0 on success.
 */
int devm_iio_init_iio_gts(struct device *dev, int max_scale_int, int max_scale_nano,
			  const struct iio_gain_sel_pair *gain_tbl, int num_gain,
			  const struct iio_itime_sel_mul *tim_tbl, int num_times,
			  struct iio_gts *gts)
{
	int ret;

	ret = iio_init_iio_gts(max_scale_int, max_scale_nano, gain_tbl,
			       num_gain, tim_tbl, num_times, gts);
	if (ret)
		return ret;

	return devm_iio_gts_build_avail_tables(dev, gts);
}
EXPORT_SYMBOL_NS_GPL(devm_iio_init_iio_gts, IIO_GTS_HELPER);

/**
 * iio_gts_all_avail_scales - helper for listing all available scales
 * @gts:	Gain time scale descriptor
 * @vals:	Returned array of supported scales
 * @type:	Type of returned scale values
 * @length:	Amount of returned values in array
 *
 * Return: a value suitable to be returned from read_avail or a negative error.
 */
int iio_gts_all_avail_scales(struct iio_gts *gts, const int **vals, int *type,
			     int *length)
{
	if (!gts->num_avail_all_scales)
		return -EINVAL;

	*vals = gts->avail_all_scales_table;
	*type = IIO_VAL_INT_PLUS_NANO;
	*length = gts->num_avail_all_scales * 2;

	return IIO_AVAIL_LIST;
}
EXPORT_SYMBOL_NS_GPL(iio_gts_all_avail_scales, IIO_GTS_HELPER);

/**
 * iio_gts_avail_scales_for_time - list scales for integration time
 * @gts:	Gain time scale descriptor
 * @time:	Integration time for which the scales are listed
 * @vals:	Returned array of supported scales
 * @type:	Type of returned scale values
 * @length:	Amount of returned values in array
 *
 * Drivers which do not allow scale setting to change integration time can
 * use this helper to list only the scales which are valid for given integration
 * time.
 *
 * Return: a value suitable to be returned from read_avail or a negative error.
 */
int iio_gts_avail_scales_for_time(struct iio_gts *gts, int time,
				  const int **vals, int *type, int *length)
{
	int i;

	for (i = 0; i < gts->num_itime; i++)
		if (gts->itime_table[i].time_us == time)
			break;

	if (i == gts->num_itime)
		return -EINVAL;

	*vals = gts->per_time_avail_scale_tables[i];
	*type = IIO_VAL_INT_PLUS_NANO;
	*length = gts->num_hwgain * 2;

	return IIO_AVAIL_LIST;
}
EXPORT_SYMBOL_NS_GPL(iio_gts_avail_scales_for_time, IIO_GTS_HELPER);

/**
 * iio_gts_avail_times - helper for listing available integration times
 * @gts:	Gain time scale descriptor
 * @vals:	Returned array of supported times
 * @type:	Type of returned scale values
 * @length:	Amount of returned values in array
 *
 * Return: a value suitable to be returned from read_avail or a negative error.
 */
int iio_gts_avail_times(struct iio_gts *gts,  const int **vals, int *type,
			int *length)
{
	if (!gts->num_avail_time_tables)
		return -EINVAL;

	*vals = gts->avail_time_tables;
	*type = IIO_VAL_INT_PLUS_MICRO;
	*length = gts->num_avail_time_tables * 2;

	return IIO_AVAIL_LIST;
}
EXPORT_SYMBOL_NS_GPL(iio_gts_avail_times, IIO_GTS_HELPER);

/**
 * iio_gts_find_sel_by_gain - find selector corresponding to a HW-gain
 * @gts:	Gain time scale descriptor
 * @gain:	HW-gain for which matching selector is searched for
 *
 * Return:	a selector matching given HW-gain or -EINVAL if selector was
 *		not found.
 */
int iio_gts_find_sel_by_gain(struct iio_gts *gts, int gain)
{
	int i;

	for (i = 0; i < gts->num_hwgain; i++)
		if (gts->hwgain_table[i].gain == gain)
			return gts->hwgain_table[i].sel;

	return -EINVAL;
}
EXPORT_SYMBOL_NS_GPL(iio_gts_find_sel_by_gain, IIO_GTS_HELPER);

/**
 * iio_gts_find_gain_by_sel - find HW-gain corresponding to a selector
 * @gts:	Gain time scale descriptor
 * @sel:	selector for which matching HW-gain is searched for
 *
 * Return:	a HW-gain matching given selector or -EINVAL if HW-gain was not
 *		found.
 */
int iio_gts_find_gain_by_sel(struct iio_gts *gts, int sel)
{
	int i;

	for (i = 0; i < gts->num_hwgain; i++)
		if (gts->hwgain_table[i].sel == sel)
			return gts->hwgain_table[i].gain;

	return -EINVAL;
}
EXPORT_SYMBOL_NS_GPL(iio_gts_find_gain_by_sel, IIO_GTS_HELPER);

/**
 * iio_gts_get_min_gain - find smallest valid HW-gain
 * @gts:	Gain time scale descriptor
 *
 * Return:	The smallest HW-gain -EINVAL if no HW-gains were in the tables.
 */
int iio_gts_get_min_gain(struct iio_gts *gts)
{
	int i, min = -EINVAL;

	for (i = 0; i < gts->num_hwgain; i++) {
		int gain = gts->hwgain_table[i].gain;

		if (min == -EINVAL)
			min = gain;
		else
			min = min(min, gain);
	}

	return min;
}
EXPORT_SYMBOL_NS_GPL(iio_gts_get_min_gain, IIO_GTS_HELPER);

/**
 * iio_find_closest_gain_low - Find the closest lower matching gain
 * @gts:	Gain time scale descriptor
 * @gain:	HW-gain for which the closest match is searched
 * @in_range:	indicate if the @gain was actually in the range of
 *		supported gains.
 *
 * Search for closest supported gain that is lower than or equal to the
 * gain given as a parameter. This is usable for drivers which do not require
 * user to request exact matching gain but rather for rounding to a supported
 * gain value which is equal or lower (setting lower gain is typical for
 * avoiding saturation)
 *
 * Return:	The closest matching supported gain or -EINVAL if @gain
 *		was smaller than the smallest supported gain.
 */
int iio_find_closest_gain_low(struct iio_gts *gts, int gain, bool *in_range)
{
	int i, diff = 0;
	int best = -1;

	*in_range = false;

	for (i = 0; i < gts->num_hwgain; i++) {
		if (gain == gts->hwgain_table[i].gain) {
			*in_range = true;
			return gain;
		}

		if (gain > gts->hwgain_table[i].gain) {
			if (!diff) {
				diff = gain - gts->hwgain_table[i].gain;
				best = i;
			} else {
				int tmp = gain - gts->hwgain_table[i].gain;

				if (tmp < diff) {
					diff = tmp;
					best = i;
				}
			}
		} else {
			/*
			 * We found valid HW-gain which is greater than
			 * reference. So, unless we return a failure below we
			 * will have found an in-range gain
			 */
			*in_range = true;
		}
	}
	/* The requested gain was smaller than anything we support */
	if (!diff) {
		*in_range = false;

		return -EINVAL;
	}

	return gts->hwgain_table[best].gain;
}
EXPORT_SYMBOL_NS_GPL(iio_find_closest_gain_low, IIO_GTS_HELPER);

static int iio_gts_get_int_time_gain_multiplier_by_sel(struct iio_gts *gts,
						       int sel)
{
	const struct iio_itime_sel_mul *time;

	time = iio_gts_find_itime_by_sel(gts, sel);
	if (!time)
		return -EINVAL;

	return time->mul;
}

/**
 * iio_gts_find_gain_for_scale_using_time - Find gain by time and scale
 * @gts:	Gain time scale descriptor
 * @time_sel:	Integration time selector corresponding to the time gain is
 *		searched for
 * @scale_int:	Integral part of the scale (typically val1)
 * @scale_nano:	Fractional part of the scale (nano or ppb)
 * @gain:	Pointer to value where gain is stored.
 *
 * In some cases the light sensors may want to find a gain setting which
 * corresponds given scale and integration time. Sensors which fill the
 * gain and time tables may use this helper to retrieve the gain.
 *
 * Return:	0 on success. -EINVAL if gain matching the parameters is not
 *		found.
 */
static int iio_gts_find_gain_for_scale_using_time(struct iio_gts *gts, int time_sel,
						  int scale_int, int scale_nano,
						  int *gain)
{
	u64 scale_linear;
	int ret, mul;

	ret = iio_gts_linearize(scale_int, scale_nano, NANO, &scale_linear);
	if (ret)
		return ret;

	ret = iio_gts_get_int_time_gain_multiplier_by_sel(gts, time_sel);
	if (ret < 0)
		return ret;

	mul = ret;

	ret = gain_get_scale_fraction(gts->max_scale, scale_linear, mul, gain);
	if (ret)
		return ret;

	if (!iio_gts_valid_gain(gts, *gain))
		return -EINVAL;

	return 0;
}

/**
 * iio_gts_find_gain_sel_for_scale_using_time - Fetch gain selector.
 * @gts:	Gain time scale descriptor
 * @time_sel:	Integration time selector corresponding to the time gain is
 *		searched for
 * @scale_int:	Integral part of the scale (typically val1)
 * @scale_nano:	Fractional part of the scale (nano or ppb)
 * @gain_sel:	Pointer to value where gain selector is stored.
 *
 * See iio_gts_find_gain_for_scale_using_time() for more information
 */
int iio_gts_find_gain_sel_for_scale_using_time(struct iio_gts *gts, int time_sel,
					       int scale_int, int scale_nano,
					       int *gain_sel)
{
	int gain, ret;

	ret = iio_gts_find_gain_for_scale_using_time(gts, time_sel, scale_int,
						     scale_nano, &gain);
	if (ret)
		return ret;

	ret = iio_gts_find_sel_by_gain(gts, gain);
	if (ret < 0)
		return ret;

	*gain_sel = ret;

	return 0;
}
EXPORT_SYMBOL_NS_GPL(iio_gts_find_gain_sel_for_scale_using_time, IIO_GTS_HELPER);

static int iio_gts_get_total_gain(struct iio_gts *gts, int gain, int time)
{
	const struct iio_itime_sel_mul *itime;

	if (!iio_gts_valid_gain(gts, gain))
		return -EINVAL;

	if (!gts->num_itime)
		return gain;

	itime = iio_gts_find_itime_by_time(gts, time);
	if (!itime)
		return -EINVAL;

	return gain * itime->mul;
}

static int iio_gts_get_scale_linear(struct iio_gts *gts, int gain, int time,
				    u64 *scale)
{
	int total_gain;
	u64 tmp;

	total_gain = iio_gts_get_total_gain(gts, gain, time);
	if (total_gain < 0)
		return total_gain;

	tmp = gts->max_scale;

	do_div(tmp, total_gain);

	*scale = tmp;

	return 0;
}

/**
 * iio_gts_get_scale - get scale based on integration time and HW-gain
 * @gts:	Gain time scale descriptor
 * @gain:	HW-gain for which the scale is computed
 * @time:	Integration time for which the scale is computed
 * @scale_int:	Integral part of the scale (typically val1)
 * @scale_nano:	Fractional part of the scale (nano or ppb)
 *
 * Compute scale matching the integration time and HW-gain given as parameter.
 *
 * Return: 0 on success.
 */
int iio_gts_get_scale(struct iio_gts *gts, int gain, int time, int *scale_int,
		      int *scale_nano)
{
	u64 lin_scale;
	int ret;

	ret = iio_gts_get_scale_linear(gts, gain, time, &lin_scale);
	if (ret)
		return ret;

	return iio_gts_delinearize(lin_scale, NANO, scale_int, scale_nano);
}
EXPORT_SYMBOL_NS_GPL(iio_gts_get_scale, IIO_GTS_HELPER);

/**
 * iio_gts_find_new_gain_sel_by_old_gain_time - compensate for time change
 * @gts:		Gain time scale descriptor
 * @old_gain:		Previously set gain
 * @old_time_sel:	Selector corresponding previously set time
 * @new_time_sel:	Selector corresponding new time to be set
 * @new_gain:		Pointer to value where new gain is to be written
 *
 * We may want to mitigate the scale change caused by setting a new integration
 * time (for a light sensor) by also updating the (HW)gain. This helper computes
 * new gain value to maintain the scale with new integration time.
 *
 * Return: 0 if an exactly matching supported new gain was found. When a
 * non-zero value is returned, the @new_gain will be set to a negative or
 * positive value. The negative value means that no gain could be computed.
 * Positive value will be the "best possible new gain there could be". There
 * can be two reasons why finding the "best possible" new gain is not deemed
 * successful. 1) This new value cannot be supported by the hardware. 2) The new
 * gain required to maintain the scale would not be an integer. In this case,
 * the "best possible" new gain will be a floored optimal gain, which may or
 * may not be supported by the hardware.
 */
int iio_gts_find_new_gain_sel_by_old_gain_time(struct iio_gts *gts,
					       int old_gain, int old_time_sel,
					       int new_time_sel, int *new_gain)
{
	const struct iio_itime_sel_mul *itime_old, *itime_new;
	u64 scale;
	int ret;

	*new_gain = -1;

	itime_old = iio_gts_find_itime_by_sel(gts, old_time_sel);
	if (!itime_old)
		return -EINVAL;

	itime_new = iio_gts_find_itime_by_sel(gts, new_time_sel);
	if (!itime_new)
		return -EINVAL;

	ret = iio_gts_get_scale_linear(gts, old_gain, itime_old->time_us,
				       &scale);
	if (ret)
		return ret;

	ret = gain_get_scale_fraction(gts->max_scale, scale, itime_new->mul,
				      new_gain);
	if (ret)
		return ret;

	if (!iio_gts_valid_gain(gts, *new_gain))
		return -EINVAL;

	return 0;
}
EXPORT_SYMBOL_NS_GPL(iio_gts_find_new_gain_sel_by_old_gain_time, IIO_GTS_HELPER);

/**
 * iio_gts_find_new_gain_by_old_gain_time - compensate for time change
 * @gts:		Gain time scale descriptor
 * @old_gain:		Previously set gain
 * @old_time:		Selector corresponding previously set time
 * @new_time:		Selector corresponding new time to be set
 * @new_gain:		Pointer to value where new gain is to be written
 *
 * We may want to mitigate the scale change caused by setting a new integration
 * time (for a light sensor) by also updating the (HW)gain. This helper computes
 * new gain value to maintain the scale with new integration time.
 *
 * Return: 0 if an exactly matching supported new gain was found. When a
 * non-zero value is returned, the @new_gain will be set to a negative or
 * positive value. The negative value means that no gain could be computed.
 * Positive value will be the "best possible new gain there could be". There
 * can be two reasons why finding the "best possible" new gain is not deemed
 * successful. 1) This new value cannot be supported by the hardware. 2) The new
 * gain required to maintain the scale would not be an integer. In this case,
 * the "best possible" new gain will be a floored optimal gain, which may or
 * may not be supported by the hardware.
 */
int iio_gts_find_new_gain_by_old_gain_time(struct iio_gts *gts, int old_gain,
					   int old_time, int new_time,
					   int *new_gain)
{
	const struct iio_itime_sel_mul *itime_new;
	u64 scale;
	int ret;

	*new_gain = -1;

	itime_new = iio_gts_find_itime_by_time(gts, new_time);
	if (!itime_new)
		return -EINVAL;

	ret = iio_gts_get_scale_linear(gts, old_gain, old_time, &scale);
	if (ret)
		return ret;

	ret = gain_get_scale_fraction(gts->max_scale, scale, itime_new->mul,
				      new_gain);
	if (ret)
		return ret;

	if (!iio_gts_valid_gain(gts, *new_gain))
		return -EINVAL;

	return 0;
}
EXPORT_SYMBOL_NS_GPL(iio_gts_find_new_gain_by_old_gain_time, IIO_GTS_HELPER);

MODULE_LICENSE("GPL");
MODULE_AUTHOR("Matti Vaittinen <mazziesaccount@gmail.com>");
MODULE_DESCRIPTION("IIO light sensor gain-time-scale helpers");
