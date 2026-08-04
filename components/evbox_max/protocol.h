#pragma once

#include <cstddef>
#include <cstdint>
#include <vector>

namespace esphome {
namespace evbox_max {

static constexpr uint8_t EVBOX_MAX_SOF = 0x7E;
static constexpr size_t EVBOX_MAX_MAX_PAYLOAD = 128;

enum class FrameType : uint8_t {
  UNKNOWN = 0x00,
  REGISTRATION = 0x10,
  ADDRESS_ASSIGNMENT = 0x11,
  INFO_REQUEST = 0x20,
  INFO_RESPONSE = 0x21,
  CONFIG_REQUEST = 0x30,
  CONFIG_RESPONSE = 0x31,
  HEARTBEAT = 0x40,
  CURRENT_SETPOINT = 0x50,
  SESSION_STATUS = 0x60,
  FAULT = 0x70,
};

struct Frame {
  uint8_t address{0};
  FrameType type{FrameType::UNKNOWN};
  std::vector<uint8_t> payload{};
};

uint8_t checksum(const uint8_t *data, size_t length);
std::vector<uint8_t> encode_frame(const Frame &frame);

class FrameParser {
 public:
  bool push(uint8_t byte, Frame *frame);
  void reset();

 protected:
  enum class ParseState : uint8_t {
    WAIT_SOF,
    ADDRESS,
    TYPE,
    LENGTH,
    PAYLOAD,
    CHECKSUM,
  };

  ParseState state_{ParseState::WAIT_SOF};
  Frame current_{};
  uint8_t expected_length_{0};
  std::vector<uint8_t> checksum_buffer_{};
};

}  // namespace evbox_max
}  // namespace esphome
