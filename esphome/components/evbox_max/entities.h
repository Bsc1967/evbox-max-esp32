#pragma once

#include "evbox_max.h"
#include "esphome/components/number/number.h"
#include "esphome/components/select/select.h"
#include "esphome/components/switch/switch.h"
#include "esphome/core/component.h"

namespace esphome {
namespace evbox_max {

enum EvboxNumberType : uint8_t {
  EVBOX_NUMBER_MANUAL_CURRENT = 0,
  EVBOX_NUMBER_MAX_CURRENT = 1,
  EVBOX_NUMBER_FAILSAFE_CURRENT = 2,
  EVBOX_NUMBER_CHARGER_BREAKER_CURRENT = 3,
  EVBOX_NUMBER_MAIN_FUSE_CURRENT = 4,
};

enum EvboxSelectType : uint8_t {
  EVBOX_SELECT_MODE = 0,
  EVBOX_SELECT_FAILSAFE_MODE = 1,
  EVBOX_SELECT_EVBOX_L1_GRID_PHASE = 2,
  EVBOX_SELECT_EVBOX_L2_GRID_PHASE = 3,
  EVBOX_SELECT_EVBOX_L3_GRID_PHASE = 4,
};

enum EvboxSwitchType : uint8_t {
  EVBOX_SWITCH_START = 0,
  EVBOX_SWITCH_STOP = 1,
  EVBOX_SWITCH_ENABLE_PV_MODE = 2,
  EVBOX_SWITCH_PAUSE = 3,
};

class EvboxCurrentNumber : public number::Number, public Component {
 public:
  void setup() override;
  void set_parent(EvboxMaxComponent *parent) { this->parent_ = parent; }
  void set_type(EvboxNumberType type) { this->type_ = type; }

 protected:
  void control(float value) override;

  EvboxMaxComponent *parent_{nullptr};
  EvboxNumberType type_{EVBOX_NUMBER_MANUAL_CURRENT};
};

class EvboxModeSelect : public select::Select, public Component {
 public:
  void setup() override;
  void set_parent(EvboxMaxComponent *parent) { this->parent_ = parent; }
  void set_type(EvboxSelectType type) { this->type_ = type; }

 protected:
  void control(const std::string &value) override;

  EvboxMaxComponent *parent_{nullptr};
  EvboxSelectType type_{EVBOX_SELECT_MODE};
};

class EvboxCommandSwitch : public switch_::Switch, public Component {
 public:
  void setup() override;
  void set_parent(EvboxMaxComponent *parent) { this->parent_ = parent; }
  void set_type(EvboxSwitchType type) { this->type_ = type; }

 protected:
  void write_state(bool state) override;

  EvboxMaxComponent *parent_{nullptr};
  EvboxSwitchType type_{EVBOX_SWITCH_START};
};

}  // namespace evbox_max
}  // namespace esphome
