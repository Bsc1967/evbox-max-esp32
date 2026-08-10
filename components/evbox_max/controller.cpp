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
  // Convert total exported watts to EV current per phase. For 3-phase charging:
  // 6900 W / (230 V * 3) = 10 A. For 1-phase charging: 2300 W / 230 V = 10 A.
  // Real PV mode should later include hysteresis and minimum-on/off timers.
  const float phase_count = inputs.charge_phases > 0 ? inputs.charge_phases : 1;
  const float surplus_a = inputs.grid_export_w / (230.0f * phase_count);
  return clamp(surplus_a, 0.0f, inputs.max_current);
}

float ChargeController::apply_safety_limits_(float requested_current, const ControlInputs &inputs) const {
  float allowed = clamp(requested_current, 0.0f, inputs.max_current);
  allowed = std::min(allowed, inputs.charger_breaker_current);

  const float main_limit = inputs.main_fuse_current;
  if (main_limit <= 0.0f) {
    return 0.0f;
  }

  // Janitza phase current is the measured current that already includes the
  // current EV setpoint. Therefore the EV allowance per phase is:
  // current EV setpoint + remaining headroom before the main fuse limit.
  const float ev_now = std::max(inputs.ev_current, 0.0f);
  const auto phase_allowance = [main_limit, ev_now](float measured_phase_current) {
    return ev_now + (main_limit - std::fabs(measured_phase_current));
  };

  const uint8_t phase_mask = inputs.active_phase_mask != 0 ? inputs.active_phase_mask : 0x07;
  if ((phase_mask & 0x01) != 0) {
    allowed = std::min(allowed, phase_allowance(inputs.l1_current));
  }
  if ((phase_mask & 0x02) != 0) {
    allowed = std::min(allowed, phase_allowance(inputs.l2_current));
  }
  if ((phase_mask & 0x04) != 0) {
    allowed = std::min(allowed, phase_allowance(inputs.l3_current));
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
