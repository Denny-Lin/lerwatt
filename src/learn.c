// SPDX-License-Identifier: MIT
/* Online thermal model. See include/lerwatt/learn.h. */
#include <math.h>
#include <string.h>

#include "lerwatt/learn.h"

#define TM_INTERVAL_S 5.0		/* past the hot spot's ~1 s transient: fit the package */
#define TM_MIN_SAMPLES 24		/* two minutes */
#define TM_COV_INIT 1e3
#define TM_COV_TRACE_MAX 1e5		/* bound wind-up when the input is not exciting */

void lw_tm_init(struct lw_thermal_model *m)
{
	memset(m, 0, sizeof(*m));
	m->lambda = 0.99;		/* ~500 s memory: longer than the package time constant */
	for (int i = 0; i < LW_TM_N; i++)
		m->cov[i][i] = TM_COV_INIT;
}

void lw_tm_observe(struct lw_thermal_model *m, double temp_c, double p_own_w,
		   double p_other_w, double dt_s)
{
	double x[LW_TM_N], px[LW_TM_N], y, err, denom = m->lambda, trace = 0;

	if (!m->have_prev || dt_s <= 0) {
		m->have_prev = true;
		m->prev_t = temp_c;
		m->prev_own = p_own_w;
		m->prev_other = p_other_w;
		return;
	}
	/* Average the power over about a second before fitting one sample. */
	m->acc_s += dt_s;
	m->acc_energy_j += (p_own_w + p_other_w) * dt_s;
	if (m->acc_s < TM_INTERVAL_S)
		return;
	/* Mean power over the interval, against the temperature it started from. */
	x[0] = m->acc_energy_j / m->acc_s;
	x[1] = m->prev_t;
	x[2] = 1.0;
	y = (temp_c - m->prev_t) / m->acc_s;
	m->acc_s = m->acc_energy_j = 0;
	m->prev_t = temp_c;
	m->prev_own = p_own_w;
	m->prev_other = p_other_w;

	err = y;
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
	m->samples++;
	m->resid_c_per_s += (fabs(err) - m->resid_c_per_s) * 0.01;
}

bool lw_tm_ready(const struct lw_thermal_model *m)
{
	double a = m->theta[0], c = m->theta[1];

	/* Heating with power, cooling toward a finite temperature, on enough data. */
	return m->samples >= TM_MIN_SAMPLES && a > 0 && c < -1e-4 && c > -10 &&
	       isfinite(lw_tm_steady(m, 0, 0));
}

double lw_tm_steady(const struct lw_thermal_model *m, double p_own_w, double p_other_w)
{
	double c = m->theta[1];

	if (c >= 0)
		return INFINITY;
	return -(m->theta[0] * (p_own_w + p_other_w) + m->theta[2]) / c;
}

double lw_tm_predict(const struct lw_thermal_model *m, double temp_c, double p_own_w,
		     double p_other_w, double horizon_s)
{
	double ss = lw_tm_steady(m, p_own_w, p_other_w);

	if (!isfinite(ss))
		return INFINITY;
	return ss + (temp_c - ss) * exp(m->theta[1] * horizon_s);
}
