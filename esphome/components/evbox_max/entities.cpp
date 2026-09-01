#include "entities.h"

namespace esphome {
namespace evbox_max {

namespace {
const char *grid_phase_option(GridPhase phase) {
  switch (phase) {
    case GRID_PHASE_L2: return "L2";
    case GRID_PHASE_L3: return "L3";
    case GRID_PHASE_L1:
    default: return "L1";
  }
}

GridPhase parse_grid_phase_option(const std::string &value) {
  if (value == "L2") return GRID_PHASE_L2;
  if (value == "L3") return GRID_PHASE_L3;
  return GRID_PHASE_L1;
}
}  // namespace

void EvboxCurrentNumber::setup() {
  if (this->parent_ == nullptr) return;

  switch (this->type_) {
    case EVBOX_NUMBER_MANUAL_CURRENT:
      this->publish_state(this->parent_->get_manual_current());
      break;
    case EVBOX_NUMBER_MAX_CURRENT:
      this->publish_state(this->parent_->get_max_current());
      break;
    case EVBOX_NUMBER_FAILSAFE_CURRENT:
      this->publish_state(this->parent_->get_failsafe_current());
      break;
    case EVBOX_NUMBER_CHARGER_BREAKER_CURRENT:
      this->publish_state(this->parent_->get_charger_breaker_current());
      break;
    case EVBOX_NUMBER_MAIN_FUSE_CURRENT:
      this->publish_state(this->parent_->get_main_fuse_current());
      break;
  }
}

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
    case EVBOX_NUMBER_CHARGER_BREAKER_CURRENT:
      this->parent_->set_charger_breaker_current(value);
      break;
    case EVBOX_NUMBER_MAIN_FUSE_CURRENT:
      this->parent_->set_main_fuse_current(value);
      break;
  }
  this->publish_state(value);
}

void EvboxModeSelect::setup() {
  if (this->parent_ == nullptr) return;

  if (this->type_ == EVBOX_SELECT_MODE) {
    switch (this->parent_->get_mode()) {
      case CHARGING_MODE_MANUAL:
        this->publish_state("MANUAL");
        break;
      case CHARGING_MODE_LOAD_BALANCING:
        this->publish_state("LOAD_BALANCING");
        break;
      case CHARGING_MODE_PV_SURPLUS:
        this->publish_state("PV_SURPLUS");
        break;
      case CHARGING_MODE_DISABLED:
        this->publish_state("DISABLED");
        break;
    }
  } else if (this->type_ == EVBOX_SELECT_FAILSAFE_MODE) {
    this->publish_state(this->parent_->get_failsafe_mode() == FAILSAFE_MODE_STOP ? "STOP" : "LIMIT_6A");
  } else if (this->type_ == EVBOX_SELECT_EVBOX_L1_GRID_PHASE) {
    this->publish_state(grid_phase_option(this->parent_->get_evbox_l1_grid_phase()));
  } else if (this->type_ == EVBOX_SELECT_EVBOX_L2_GRID_PHASE) {
    this->publish_state(grid_phase_option(this->parent_->get_evbox_l2_grid_phase()));
  } else if (this->type_ == EVBOX_SELECT_EVBOX_L3_GRID_PHASE) {
    this->publish_state(grid_phase_option(this->parent_->get_evbox_l3_grid_phase()));
  }
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
  } else if (this->type_ == EVBOX_SELECT_FAILSAFE_MODE) {
    if (value == "STOP") this->parent_->set_failsafe_mode(FAILSAFE_MODE_STOP);
    if (value == "LIMIT_6A") this->parent_->set_failsafe_mode(FAILSAFE_MODE_LIMIT_6A);
  } else if (this->type_ == EVBOX_SELECT_EVBOX_L1_GRID_PHASE) {
    this->parent_->set_evbox_l1_grid_phase(parse_grid_phase_option(value));
  } else if (this->type_ == EVBOX_SELECT_EVBOX_L2_GRID_PHASE) {
    this->parent_->set_evbox_l2_grid_phase(parse_grid_phase_option(value));
  } else if (this->type_ == EVBOX_SELECT_EVBOX_L3_GRID_PHASE) {
    this->parent_->set_evbox_l3_grid_phase(parse_grid_phase_option(value));
  }
  this->publish_state(value);
}

void EvboxCommandSwitch::setup() {
  if (this->parent_ == nullptr) return;
  if (this->type_ == EVBOX_SWITCH_ENABLE_PV_MODE) {
    this->publish_state(this->parent_->get_pv_enabled());
  } else {
    this->publish_state(false);
  }
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
      case EVBOX_SWITCH_PAUSE:
        this->parent_->pause_session();
        break;
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
