#include "entities.h"

namespace esphome {
namespace evbox_max {

void EvboxCurrentNumber::control(float value) {
  if (this->parent_ == nullptr) {
    return;
  }

  switch (this->type_) {
    case EVBOX_NUMBER_MANUAL_CURRENT:
      this->parent_->set_manual_current(value);
      break;
    case EVBOX_NUMBER_MAX_CURRENT:
      this->parent_->set_max_current(value);
      break;
    case EVBOX_NUMBER_FAILSAFE_CURRENT:
      this->parent_->set_failsafe_current(value);
      break;
  }
  this->publish_state(value);
}

void EvboxModeSelect::control(const std::string &value) {
  if (this->parent_ == nullptr) {
    return;
  }

  if (this->type_ == EVBOX_SELECT_MODE) {
    if (value == "MANUAL") this->parent_->set_mode(CHARGING_MODE_MANUAL);
    if (value == "LOAD_BALANCING") this->parent_->set_mode(CHARGING_MODE_LOAD_BALANCING);
    if (value == "PV_SURPLUS") this->parent_->set_mode(CHARGING_MODE_PV_SURPLUS);
    if (value == "DISABLED") this->parent_->set_mode(CHARGING_MODE_DISABLED);
  } else {
    if (value == "STOP") this->parent_->set_failsafe_mode(FAILSAFE_MODE_STOP);
    if (value == "LIMIT_6A") this->parent_->set_failsafe_mode(FAILSAFE_MODE_LIMIT_6A);
  }
  this->publish_state(value);
}

void EvboxCommandSwitch::write_state(bool state) {
  if (this->parent_ == nullptr) {
    this->publish_state(false);
    return;
  }

  if (state) {
    switch (this->type_) {
      case EVBOX_SWITCH_START:
        this->parent_->start_session();
        break;
      case EVBOX_SWITCH_STOP:
        this->parent_->stop_session();
        break;
      case EVBOX_SWITCH_ENABLE_PV_MODE:
        this->parent_->set_pv_enabled(true);
        this->publish_state(true);
        return;
    }
  } else if (this->type_ == EVBOX_SWITCH_ENABLE_PV_MODE) {
    this->parent_->set_pv_enabled(false);
    this->publish_state(false);
    return;
  }

  this->publish_state(false);
}

}  // namespace evbox_max
}  // namespace esphome
