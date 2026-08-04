#pragma once

#include <cstdint>

namespace esphome {
namespace evbox_max {

enum ChargingMode : uint8_t {
  CHARGING_MODE_MANUAL = 0,
  CHARGING_MODE_LOAD_BALANCING = 1,
  CHARGING_MODE_PV_SURPLUS = 2,
  CHARGING_MODE_DISABLED = 3,
};

enum FailsafeMode : uint8_t {
  FAILSAFE_MODE_STOP = 0,
  FAILSAFE_MODE_LIMIT_6A = 1,
};

struct ControlInputs {
  float manual_current{6.0f};
  float max_current{16.0f};
  float grid_import_w{0.0f};
  float grid_export_w{0.0f};
  bool janitza_online{false};
  bool pv_enabled{false};
};

class ChargeController {
 public:
  void set_mode(ChargingMode mode) { this->mode_ = mode; }
  void set_failsafe_mode(FailsafeMode mode) { this->failsafe_mode_ = mode; }
  void set_failsafe_current(float current) { this->failsafe_current_ = current; }

  float calculate_current(const ControlInputs &inputs) const;

 protected:
  float manual_(const ControlInputs &inputs) const;
  float load_balancing_(const ControlInputs &inputs) const;
  float pv_surplus_(const ControlInputs &inputs) const;
  float failsafe_() const;

  ChargingMode mode_{CHARGING_MODE_MANUAL};
  FailsafeMode failsafe_mode_{FAILSAFE_MODE_LIMIT_6A};
  float failsafe_current_{6.0f};
};

}  // namespace evbox_max
}  // namespace esphome
