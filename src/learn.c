// SPDX-License-Identifier: MIT
/* Online thermal model. See include/lerwatt/learn.h. */
#include <math.h>
#include <string.h>

#include "lerwatt/learn.h"

#define TM_INTERVAL_S 10.0		/* fit the package on a slow clock */
#define TM_MIN_SAMPLES 8		/* a minute and a half, with the anchor below */
#define TM_HOT_MEMORY_S 60.0		/* long against any burst, short against the package */
#define TM_PEAK_MEMORY_S 30.0
#define TM_COV_INIT 1e3
#define TM_COV_TRACE_MAX 1e5		/* bound wind-up when the input is not exciting */

void lw_tm_init(struct lw_thermal_model *m)
{
	memset(m, 0, sizeof(*m));
	m->lambda = 0.98;		/* ~500 s memory: longer than the package time constant */
	for (int i = 0; i < LW_TM_N; i++)
		m->cov[i][i] = TM_COV_INIT;
}

static void observe_hot_spot(struct lw_thermal_model *m, double offset_c, double p_own_w,
			    double dt_s)
{
	double k = dt_s / TM_HOT_MEMORY_S, decay = exp(-dt_s / TM_PEAK_MEMORY_S);

	if (m->hot_s == 0) {
		m->offset_avg_c = offset_c;
		m->power_avg_w = p_own_w;
	}
	if (k > 1)
		k = 1;
	m->offset_avg_c += (offset_c - m->offset_avg_c) * k;
	m->power_avg_w += (p_own_w - m->power_avg_w) * k;
	m->hot_s += dt_s;
	m->peak_offset_c = fmax(offset_c, m->peak_offset_c * decay);
	m->peak_power_w = fmax(p_own_w, m->peak_power_w * decay);
}

static double rls_update(struct lw_thermal_model *m, const double *x, double y)
{
	double px[LW_TM_N], err = y, denom = m->lambda, trace = 0;

	for (int i = 0; i < LW_TM_N; i++)
		err -= m->theta[i] * x[i];
	for (int i = 0; i < LW_TM_N; i++) {
		px[i] = 0;
		for (int j = 0; j < LW_TM_N; j++)
			px[i] += m->cov[i][j] * x[j];
		denom += x[i] * px[i];
	}
	for (int i = 0; i < LW_TM_N; i++)
		m->theta[i] += px[i] * err / denom;
	for (int i = 0; i < LW_TM_N; i++)
		for (int j = 0; j < LW_TM_N; j++)
			m->cov[i][j] = (m->cov[i][j] - px[i] * px[j] / denom) / m->lambda;
	for (int i = 0; i < LW_TM_N; i++)
		trace += m->cov[i][i];
	if (trace > TM_COV_TRACE_MAX)
		for (int i = 0; i < LW_TM_N; i++)
			for (int j = 0; j < LW_TM_N; j++)
				m->cov[i][j] *= TM_COV_TRACE_MAX / trace;
	return err;
}

void lw_tm_observe(struct lw_thermal_model *m, double hot_c, double pkg_c, double p_own_w,
		   double p_other_w, double dt_s)
{
	double x[LW_TM_N], y, err, mean_w, mean_c, interval_s;

	if (!m->have_prev || dt_s <= 0) {
		/*
		 * A steady load cannot separate power from board heat, so anchor
		 * the fit: with no CPU power, about here is where it would stay.
		 * This holds when the board starts cool, and forgetting erases it
		 * when it does not.
		 */
		if (!m->have_prev) {
			const double rest[LW_TM_N] = { 0, pkg_c, 1 };

			rls_update(m, rest, 0);
		}
		m->have_prev = true;
		return;
	}
	observe_hot_spot(m, hot_c - pkg_c, p_own_w, dt_s);
	/* Interval means: they filter TSADC quantization without adding lag. */
	m->acc_s += dt_s;
	m->acc_energy_j += (p_own_w + p_other_w) * dt_s;
	m->acc_pkg += pkg_c * dt_s;
	if (m->acc_s < TM_INTERVAL_S)
		return;
	mean_w = m->acc_energy_j / m->acc_s;
	mean_c = m->acc_pkg / m->acc_s;
	interval_s = m->acc_s;
	m->acc_s = m->acc_energy_j = m->acc_pkg = 0;
	if (!m->have_mean) {
		m->have_mean = true;
		m->prev_mean_w = mean_w;
		m->prev_mean_c = mean_c;
		return;
	}
	/* The slope between two means, against the midpoint of both. */
	x[0] = (mean_w + m->prev_mean_w) / 2;
	x[1] = (mean_c + m->prev_mean_c) / 2;
	x[2] = 1.0;
	y = (mean_c - m->prev_mean_c) / interval_s;
	m->prev_mean_w = mean_w;
	m->prev_mean_c = mean_c;

	err = rls_update(m, x, y);
	m->samples++;
	m->resid_c_per_s += (fabs(err) - m->resid_c_per_s) * 0.01;
}

static double offset_gain(const struct lw_thermal_model *m)
{
	return m->power_avg_w > 0 ? m->offset_avg_c / m->power_avg_w : 0;
}

bool lw_tm_ready(const struct lw_thermal_model *m)
{
	double a = m->theta[0], c = m->theta[1];

	/* Heating with power, cooling toward a finite temperature, on enough data. */
	return m->samples >= TM_MIN_SAMPLES && a > 0 && c < -1e-4 && c > -10 &&
	       isfinite(lw_tm_package_steady(m, 0)) && offset_gain(m) >= 0;
}

double lw_tm_package_steady(const struct lw_thermal_model *m, double p_total_w)
{
	double c = m->theta[1];

	if (c >= 0)
		return INFINITY;
	return -(m->theta[0] * p_total_w + m->theta[2]) / c;
}

double lw_tm_package(const struct lw_thermal_model *m, double pkg_c, double p_total_w,
		     double horizon_s)
{
	double ss = lw_tm_package_steady(m, p_total_w);

	if (!isfinite(ss))
		return INFINITY;
	return ss + (pkg_c - ss) * exp(m->theta[1] * horizon_s);
}

double lw_tm_offset(const struct lw_thermal_model *m, double p_own_w)
{
	return offset_gain(m) * p_own_w;
}

double lw_tm_peak_offset(const struct lw_thermal_model *m, double p_own_w)
{
	double full = offset_gain(m) * m->peak_power_w;
	double gain = full > 0 ? m->peak_offset_c / full : 1;

	return lw_tm_offset(m, p_own_w) * (gain < 1 ? gain : 1);
}
