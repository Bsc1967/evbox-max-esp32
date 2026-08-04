#include "controller.h"
#include "esphome/core/helpers.h"

namespace esphome {
namespace evbox_max {

float ChargeController::calculate_current(const ControlInputs &inputs) const {
  // Janitza is required for dynamic modes. If the meter disappears, fall back
  // to the configured safe behavior instead of guessing available capacity.
  if (!inputs.janitza_online &&
      (this->mode_ == CHARGING_MODE_LOAD_BALANCING || this->mode_ == CHARGING_MODE_PV_SURPLUS)) {
    return this->failsafe_();
  }

  switch (this->mode_) {
    case CHARGING_MODE_MANUAL:
      return this->manual_(inputs);
    case CHARGING_MODE_LOAD_BALANCING:
      return this->load_balancing_(inputs);
    case CHARGING_MODE_PV_SURPLUS:
      return this->pv_surplus_(inputs);
    case CHARGING_MODE_DISABLED:
    default:
      return 0.0f;
  }
}

float ChargeController::manual_(const ControlInputs &inputs) const {
  return clamp(inputs.manual_current, 0.0f, inputs.max_current);
}

float ChargeController::load_balancing_(const ControlInputs &inputs) const {
  // Conservative first implementation: reduce 1 A per 230 W grid import.
  // This is deliberately simple until site fuse limits and phase mapping are
  // known. The protocol layer does not need to change when this gets smarter.
  const float reduction = inputs.grid_import_w > 0.0f ? inputs.grid_import_w / 230.0f : 0.0f;
  return clamp(inputs.max_current - reduction, 0.0f, inputs.max_current);
}

float ChargeController::pv_surplus_(const ControlInputs &inputs) const {
  if (!inputs.pv_enabled) {
    return 0.0f;
  }
  // Approximate three-phase current from surplus power.
  // Real PV mode should later include hysteresis and minimum-on/off timers.
  const float surplus_a = inputs.grid_export_w / (230.0f * 3.0f);
  return clamp(surplus_a, 0.0f, inputs.max_current);
}

float ChargeController::failsafe_() const {
  if (this->failsafe_mode_ == FAILSAFE_MODE_STOP) {
    return 0.0f;
  }
  return clamp(this->failsafe_current_, 0.0f, 6.0f);
}

}  // namespace evbox_max
}  // namespace esphome
