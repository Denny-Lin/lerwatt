// SPDX-License-Identifier: MIT
#include <math.h>
#include <string.h>

#include "lerwatt/predict.h"

#define MAX_GAP_NS 250000000ull
#define SUPPORT_NS 30000000000ull
#define MIN_LABELS 100u
#define MAX_CONTROL_MARGIN_C 2.0

static bool close_power(double a, double b)
{
	return isfinite(a) && isfinite(b) && a >= 0 && b >= 0 &&
	       fabs(a - b) <= fmax(.05, .05 * fmax(a, b));
}

void lw_predictor_init(struct lw_predictor *p)
{
	/* Controller state is zero-initialized. Preserve an explicit backend
	 * binding; never store pointers into this movable built-in state. */
	const struct lw_prediction_ops *ops = p->ops;
	void *context = p->context;
	memset(p, 0, sizeof(*p));
	p->ops = ops; p->context = context;
	lw_tm_init(&p->thermal);
	p->point_c = p->base_c = NAN;
	p->point_source = -1;
}

bool lw_predictor_bind(struct lw_predictor *p, const struct lw_prediction_ops *ops, void *context)
{
	if (ops && (ops->api != LW_PREDICT_API || !ops->observe || !ops->ready || !ops->predict))
		return false;
	p->ops = ops;
	p->context = context;
	memset(&p->support, 0, sizeof(p->support));
	memset(&p->forecaster, 0, sizeof(p->forecaster));
	memset(&p->temporal, 0, sizeof(p->temporal));
	lw_tm_init(&p->thermal);
	p->observation_valid = false;
	p->point_c = p->base_c = NAN;
	return true;
}

static void support_observe(struct lw_predictor *p, const struct lw_prediction_observation *o)
{
	struct lw_prediction_support *s = &p->support;
	double total = o->own_power_w + o->other_power_w;
	bool same = s->initialized && o->now_ns > s->last_ns &&
		o->now_ns - s->last_ns <= MAX_GAP_NS &&
		o->observed_khz == s->observed_khz && o->ceiling_khz == s->ceiling_khz &&
		close_power(o->own_power_w, s->own_w) && close_power(total, s->total_w);

	if (!same) {
		uint64_t resets = s->resets + (s->initialized ? 1 : 0);
		uint64_t dropped = s->dropped + s->count;

		*s = (struct lw_prediction_support) {
			.initialized = true, .since_ns = o->now_ns, .resets = resets, .dropped = dropped,
			.observed_khz = o->observed_khz, .ceiling_khz = o->ceiling_khz,
			.own_w = o->own_power_w, .total_w = total,
		};
		/* Reset only control calibration. The existing forecaster and LMS
		 * retain their learned parameters and original point behavior. An
		 * epoch must validate their issued forecasts on its own action path. */
	}
	s->last_ns = o->now_ns;
	/* Use an issued point forecast once, after its five-second target. */
	unsigned int matured = 0;
	bool have_matured = false;
	while (s->count && s->pending[s->head].due_ns <= o->now_ns) {
		if (have_matured) s->dropped++;
		matured = s->head; have_matured = true;
		s->head = (s->head + 1) % 64; s->count--;
	}
	if (have_matured) {
		unsigned int h = matured;
		if (o->now_ns - s->pending[h].due_ns > MAX_GAP_NS || !isfinite(o->hot_c)) {
			s->dropped++; return;
		}
		double loss = pow(s->pending[h].point_c - o->hot_c, 2);
		double reference = pow(s->pending[h].reference_c - o->hot_c, 2);
		double difference = reference - loss;
		double alpha = s->labels ? 1 - exp(-((o->now_ns - s->last_label_ns) / 1e9) / 30) : 1;
		if (!isfinite(loss) || !isfinite(reference) || !isfinite(difference * difference)) {
			s->dropped++; return;
		}
		s->loss += (loss - s->loss) * alpha;
		s->reference_loss += (reference - s->reference_loss) * alpha;
		s->difference_squared += (difference * difference - s->difference_squared) * alpha;
		s->last_label_ns = o->now_ns; s->labels++;
	}
	s->advantage = s->reference_loss - s->loss;
	s->margin = sqrt(fmax(0, s->difference_squared - s->advantage * s->advantage) / 6);
}

static void support_issue(struct lw_predictor *p, const struct lw_prediction_observation *o)
{
	struct lw_prediction_support *s = &p->support;
	if (!o->cooperative || !isfinite(p->point_c) || !isfinite(p->base_c) ||
	    (s->count && o->now_ns - s->last_issue_ns < 100000000ull) ||
	    o->now_ns > UINT64_MAX - LW_FORECAST_HORIZON_NS)
		return;
	if (s->count == 64) { s->head = (s->head + 1) % 64; s->count--; s->dropped++; }
	unsigned int tail = (s->head + s->count) % 64;
	s->pending[tail].due_ns = o->now_ns + LW_FORECAST_HORIZON_NS;
	s->pending[tail].point_c = p->point_c;
	s->pending[tail].reference_c = p->base_c;
	s->count++; s->last_issue_ns = o->now_ns;
}

void lw_predictor_observe(struct lw_predictor *p, const struct lw_prediction_observation *o)
{
	if (p->version && p->observation.now_ns == o->now_ns)
		return;
	p->observation = *o;
	p->version++;
	p->observation_valid = isfinite(o->hot_c) && isfinite(o->own_power_w) &&
		isfinite(o->other_power_w) && o->own_power_w >= 0 && o->other_power_w >= 0 &&
		isfinite(o->dt_s) && o->dt_s > 0 && o->observed_khz > 0;
	if (!p->observation_valid) {
		memset(&p->support, 0, sizeof(p->support));
		p->point_c = p->base_c = NAN;
		p->point_correction_c = 0;
		return;
	}
	if (p->ops) {
		p->ops->observe(p->context, o);
		struct lw_prediction_query q = {
			.purpose = LW_PREDICT_POINT, .now_ns = o->now_ns, .horizon_s = 5,
			.package_c = o->package_c, .anchor_c = o->hot_c,
			.own_mean_w = o->own_power_w, .total_mean_w = o->own_power_w + o->other_power_w,
			.own_peak_w = o->own_power_w, .reference_peak_w = o->own_power_w,
			.candidate_khz = o->observed_khz,
		};
		struct lw_prediction_result r;
		p->point_c = p->base_c = NAN;
		p->point_correction_c = 0;
		p->point_source = -1;
		if (lw_predictor_predict(p, &q, &r)) {
			p->point_c = r.mean_c;
			p->base_c = r.nominal_c;
			p->point_correction_c = r.mean_c - r.nominal_c;
		}
		return;
	}
	if (o->thermal_update && isfinite(o->package_c) && isfinite(o->thermal_hot_c))
		lw_tm_observe(&p->thermal, o->thermal_hot_c, o->package_c,
			      o->own_power_w, o->other_power_w, o->dt_s);
	if (o->cooperative)
		support_observe(p, o);
	double observed[2] = { o->package_c, o->hot_c };
	double candidate[2][3] = { { observed[0], NAN, NAN }, { observed[1], NAN, NAN } };
	if (isfinite(o->package_c) && lw_tm_ready(&p->thermal)) {
		candidate[0][1] = lw_tm_package(&p->thermal, observed[0], p->thermal.last_total_w, 5);
		candidate[1][1] = candidate[0][1] + lw_tm_offset(&p->thermal, p->thermal.last_own_w);
		candidate[0][2] = lw_tm_forecast_package(&p->thermal, observed[0], 5);
		candidate[1][2] = lw_tm_forecast_hot(&p->thermal, observed[0], 5);
	}
	lw_forecast_step(&p->forecaster, o->now_ns, observed, candidate);
	p->point_c = p->base_c = p->forecaster.prediction[LW_FORECAST_HOT];
	p->point_source = (int)p->forecaster.selected[LW_FORECAST_HOT];
	p->point_correction_c = 0;
	if (o->temporal) {
		p->point_c = lw_temporal_step(&p->temporal, o->now_ns, o->hot_c, p->base_c, candidate[1]);
		p->point_correction_c = p->temporal.correction_c;
		if (p->temporal.active)
			p->point_source = LW_FORECAST_TEMPORAL;
	}
	support_issue(p, o);
}

bool lw_predictor_ready(const struct lw_predictor *p)
{
	return p->observation_valid && (p->ops ? p->ops->ready(p->context) : lw_tm_ready(&p->thermal));
}

static enum lw_prediction_reason correction_reason(const struct lw_predictor *p,
						    const struct lw_prediction_query *q)
{
	const struct lw_prediction_support *s = &p->support;
	const struct lw_temporal *t = &p->temporal;
	if (!p->observation_valid || !p->observation.temporal)
		return LW_PREDICT_UNCERTAIN;
	if (fabs(q->horizon_s - 5) > 1e-9)
		return LW_PREDICT_HORIZON;
	if (!s->initialized || q->now_ns < s->last_ns || q->now_ns - s->last_ns > MAX_GAP_NS)
		return LW_PREDICT_STALE;
	if (s->observed_khz != q->candidate_khz || s->ceiling_khz != q->candidate_khz)
		return LW_PREDICT_ACTION;
	if (!close_power(q->own_mean_w, s->own_w) || !close_power(q->total_mean_w, s->total_w) ||
	    !close_power(q->own_peak_w, q->own_mean_w))
		return LW_PREDICT_POWER;
	if (q->now_ns < s->since_ns || q->now_ns - s->since_ns < SUPPORT_NS || s->labels < MIN_LABELS)
		return LW_PREDICT_WARMUP;
	if (!t->active || !isfinite(s->loss) || !isfinite(p->point_c) ||
	    !isfinite(s->reference_loss) || s->advantage <= s->margin)
		return LW_PREDICT_UNCERTAIN;
	return LW_PREDICT_CORRECTED;
}

bool lw_predictor_predict(const struct lw_predictor *p, const struct lw_prediction_query *q,
			  struct lw_prediction_result *r)
{
	*r = (struct lw_prediction_result) { .reason = LW_PREDICT_INVALID };
	if (q->purpose != LW_PREDICT_CONTROL && q->purpose != LW_PREDICT_POINT)
		return false;
	if (q->purpose == LW_PREDICT_POINT && fabs(q->horizon_s - 5) > 1e-9)
		return false;
	if (!isfinite(q->horizon_s) || q->horizon_s <= 0 ||
	    (q->purpose == LW_PREDICT_CONTROL && !isfinite(q->package_c)) || !isfinite(q->anchor_c) ||
	    !isfinite(q->own_mean_w) || !isfinite(q->total_mean_w) ||
	    !isfinite(q->own_peak_w) || !isfinite(q->reference_peak_w) ||
	    q->own_mean_w < 0 || q->total_mean_w < q->own_mean_w ||
	    q->own_peak_w < 0 || q->reference_peak_w < 0 || !q->candidate_khz ||
	    !isfinite(q->work_capacity) || q->work_capacity < 0)
		return false;
	if (p->ops) {
		if (!p->ops->predict(p->context, q, r))
			return false;
	} else {
		if (!p->observation_valid || (q->purpose == LW_PREDICT_CONTROL && !lw_tm_ready(&p->thermal)))
			return false;
		r->cpu_power_w = q->own_mean_w;
		r->power_quality = q->power_quality;
		r->energy_valid = q->power_quality == LW_POWER_ENERGY_MODEL || q->power_quality == LW_POWER_CALIBRATED_MODEL;
		/* Proxy OPP tables are relative quantities, not physical joules. */
		r->cpu_energy_j = r->energy_valid ? q->own_mean_w * q->horizon_s : 0;
		r->work_capacity = q->work_capacity;
		r->model_version = p->version;
		r->reason = LW_PREDICT_NOMINAL;
		if (q->purpose == LW_PREDICT_POINT) {
			r->nominal_c = p->base_c;
			r->mean_c = r->upper_c = p->point_c;
		} else {
			r->hot_delta_c = lw_tm_peak_offset(&p->thermal, q->own_peak_w) -
				lw_tm_peak_offset(&p->thermal, q->reference_peak_w);
			r->nominal_c = q->anchor_c + lw_tm_package(&p->thermal, q->package_c,
				q->total_mean_w, q->horizon_s) - q->package_c + r->hot_delta_c;
			r->mean_c = r->upper_c = r->nominal_c;
		}
		if (q->purpose == LW_PREDICT_CONTROL && q->cooperative) {
			r->reason = correction_reason(p, q);
			if (r->reason == LW_PREDICT_CORRECTED) {
				/* The learned point target and the peak-based planner have
				 * different anchors. Take a conservative maximum, never add
				 * the point residual to the planner or lower its bound. This
				 * is an empirical error allowance, NOT a confidence bound. */
				r->uncertainty_c = 2 * sqrt(fmax(0, p->support.loss)) + .1;
				if (r->uncertainty_c > MAX_CONTROL_MARGIN_C) {
					r->reason = LW_PREDICT_UNCERTAIN;
				} else {
					r->upper_c = fmax(r->nominal_c, p->point_c + r->uncertainty_c);
					r->corrected = r->upper_c > r->nominal_c + 1e-9;
				}
			}
		}
	}
	r->valid = isfinite(r->nominal_c) && isfinite(r->mean_c) && isfinite(r->upper_c) &&
		isfinite(r->hot_delta_c) && isfinite(r->uncertainty_c) && r->uncertainty_c >= 0 &&
		isfinite(r->cpu_power_w) && r->cpu_power_w >= 0 &&
		isfinite(r->cpu_energy_j) && r->cpu_energy_j >= 0 &&
		isfinite(r->work_capacity) && r->work_capacity >= 0 &&
		r->upper_c >= r->mean_c &&
		(q->purpose != LW_PREDICT_CONTROL || r->upper_c >= r->nominal_c);
	if (!r->valid)
		r->reason = LW_PREDICT_INVALID;
	return r->valid;
}

const char *lw_prediction_reason_name(enum lw_prediction_reason reason)
{
	static const char *const names[] = {
		"nominal", "corrected", "warmup", "action", "power", "horizon", "stale", "uncertain", "invalid"
	};
	return (unsigned int)reason < sizeof(names) / sizeof(names[0]) ? names[reason] : "invalid";
}
