/* SPDX-License-Identifier: MIT */
#ifndef LERWATT_PREDICT_H
#define LERWATT_PREDICT_H

#include "lerwatt/learn.h"
#include "lerwatt/forecast.h"
#include "lerwatt/temporal.h"

#define LW_PREDICT_API 1u

enum lw_prediction_purpose { LW_PREDICT_CONTROL, LW_PREDICT_POINT };
enum lw_power_quality { LW_POWER_PROXY, LW_POWER_ENERGY_MODEL, LW_POWER_CALIBRATED_MODEL };
enum lw_prediction_reason {
	LW_PREDICT_NOMINAL, LW_PREDICT_CORRECTED, LW_PREDICT_WARMUP,
	LW_PREDICT_ACTION, LW_PREDICT_POWER, LW_PREDICT_HORIZON,
	LW_PREDICT_STALE, LW_PREDICT_UNCERTAIN, LW_PREDICT_INVALID
};

/* Candidate powers/energy are always predictions. Even a calibrated model is
 * not a physical measurement. A ceiling is not a commanded clock. */
struct lw_prediction_observation {
	uint64_t now_ns;
	double hot_c, thermal_hot_c, package_c, own_power_w, other_power_w, dt_s;
	uint32_t observed_khz, ceiling_khz;
	bool thermal_update, temporal, cooperative;
};

/* Immutable, action-conditioned query. MPC owns candidate construction and
 * its objective; a backend must not write knobs or modify state in predict. */
struct lw_prediction_query {
	enum lw_prediction_purpose purpose;
	uint64_t now_ns;
	double horizon_s, package_c, anchor_c;
	double own_mean_w, total_mean_w, own_peak_w, reference_peak_w;
	double work_capacity;
	uint32_t candidate_khz;
	bool cooperative;
	enum lw_power_quality power_quality;
};

struct lw_prediction_result {
	bool valid, corrected, energy_valid;
	double nominal_c, mean_c, upper_c, uncertainty_c, hot_delta_c;
	double cpu_power_w, cpu_energy_j, work_capacity;
	uint64_t model_version;
	enum lw_prediction_reason reason;
	enum lw_power_quality power_quality;
};

/* Replacement backends have private caller-owned state. No allocation or I/O
 * is permitted in these callbacks. predict must be const and bounded. Binding
 * lasts until the caller unbinds it; private weights are never checkpointed by
 * the built-in checkpoint implementation. */
struct lw_prediction_ops {
	unsigned int api;
	void (*observe)(void *context, const struct lw_prediction_observation *obs);
	bool (*ready)(const void *context);
	bool (*predict)(const void *context, const struct lw_prediction_query *query,
			struct lw_prediction_result *result);
};

struct lw_prediction_support {
	bool initialized;
	uint64_t since_ns, last_ns, resets;
	uint32_t observed_khz, ceiling_khz;
	double own_w, total_w;
	struct { uint64_t due_ns; double point_c, reference_c; } pending[64];
	unsigned int head, count;
	uint64_t last_issue_ns, last_label_ns, labels, dropped;
	double loss, reference_loss, difference_squared, advantage, margin;
};

struct lw_predictor {
	struct lw_thermal_model thermal;
	struct lw_forecaster forecaster;
	struct lw_temporal temporal;
	struct lw_prediction_support support;
	struct lw_prediction_observation observation;
	double point_c, base_c, point_correction_c;
	int point_source;
	uint64_t version;
	bool observation_valid;
	const struct lw_prediction_ops *ops;
	void *context;
};

void lw_predictor_init(struct lw_predictor *p);
bool lw_predictor_bind(struct lw_predictor *p, const struct lw_prediction_ops *ops, void *context);
void lw_predictor_observe(struct lw_predictor *p, const struct lw_prediction_observation *obs);
bool lw_predictor_ready(const struct lw_predictor *p);
bool lw_predictor_predict(const struct lw_predictor *p, const struct lw_prediction_query *query,
			  struct lw_prediction_result *result);
const char *lw_prediction_reason_name(enum lw_prediction_reason reason);

#endif
