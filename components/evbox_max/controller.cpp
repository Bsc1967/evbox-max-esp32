#include "controller.h"
#include <algorithm>
#include <cmath>
#include "esphome/core/helpers.h"

namespace esphome {
namespace evbox_max {

float ChargeController::calculate_current(const ControlInputs &inputs) const {
  switch (this->mode_) {
    case CHARGING_MODE_DISABLED:
      return 0.0f;
    default:
      break;
  }

  // Janitza is required for per-phase fuse protection. If the meter
  // disappears, fall back to the configured safe behavior in every active mode.
  if (!inputs.janitza_online) {
    return this->failsafe_();
  }

  switch (this->mode_) {
    case CHARGING_MODE_MANUAL:
      return this->apply_safety_limits_(this->manual_(inputs), inputs);
    case CHARGING_MODE_LOAD_BALANCING:
      return this->apply_safety_limits_(this->load_balancing_(inputs), inputs);
    case CHARGING_MODE_PV_SURPLUS:
      return this->apply_safety_limits_(this->pv_surplus_(inputs), inputs);
    default:
      return 0.0f;
  }
}

float ChargeController::manual_(const ControlInputs &inputs) const {
  return clamp(inputs.manual_current, 0.0f, inputs.max_current);
}

float ChargeController::load_balancing_(const ControlInputs &inputs) const {
  // Per-phase headroom is enforced in apply_safety_limits_(). This mode asks
  // for the configured maximum and lets the two safety guards reduce it.
  return clamp(inputs.max_current, 0.0f, inputs.max_current);
}

float ChargeController::pv_surplus_(const ControlInputs &inputs) const {
  if (!inputs.pv_enabled) {
    return 0.0f;
  }
  // Dutch net metering is settled over the three phases together. Use total
  // signed active power for PV surplus, then let apply_safety_limits_ protect
  // every active charging phase against the main fuse.
  const float phase_count = inputs.charge_phases > 0 ? inputs.charge_phases : 1;
  const float net_export_w = inputs.grid_total_power_w < 0.0f ? -inputs.grid_total_power_w : inputs.grid_export_w;
  const float surplus_a = net_export_w / (230.0f * phase_count);
  return clamp(surplus_a, 0.0f, inputs.max_current);
}

float ChargeController::apply_safety_limits_(float requested_current, const ControlInputs &inputs) const {
  float allowed = clamp(requested_current, 0.0f, inputs.max_current);
  allowed = std::min(allowed, inputs.charger_breaker_current);

  const float main_limit = inputs.main_fuse_current;
  if (main_limit <= 0.0f) {
    return 0.0f;
  }

  // Janitza current has no direction, but phase power does. Convert current to
  // signed current with power sign. The measured phase current already includes
  // the EV current, so remove the current EV contribution and calculate how much
  // new EV current still fits before import reaches the main fuse.
  const auto phase_allowance = [main_limit](float measured_phase_current, float measured_phase_power,
                                            float ev_phase_current) {
    const float signed_current = measured_phase_power < 0.0f ? -std::fabs(measured_phase_current)
                                                             : std::fabs(measured_phase_current);
    return std::max(ev_phase_current, 0.0f) + (main_limit - signed_current);
  };

  const uint8_t phase_mask = inputs.active_phase_mask != 0 ? inputs.active_phase_mask : 0x07;
  if ((phase_mask & 0x01) != 0) {
    allowed = std::min(allowed, phase_allowance(inputs.l1_current, inputs.l1_power_w, inputs.ev_l1_current));
  }
  if ((phase_mask & 0x02) != 0) {
    allowed = std::min(allowed, phase_allowance(inputs.l2_current, inputs.l2_power_w, inputs.ev_l2_current));
  }
  if ((phase_mask & 0x04) != 0) {
    allowed = std::min(allowed, phase_allowance(inputs.l3_current, inputs.l3_power_w, inputs.ev_l3_current));
  }

  return clamp(allowed, 0.0f, inputs.max_current);
}

float ChargeController::failsafe_() const {
  if (this->failsafe_mode_ == FAILSAFE_MODE_STOP) {
    return 0.0f;
  }
  return clamp(this->failsafe_current_, 0.0f, 6.0f);
}

}  // namespace evbox_max
}  // namespace esphome
