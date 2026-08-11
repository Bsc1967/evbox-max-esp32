#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace esphome {
namespace evbox_max {

static constexpr uint8_t EVBOX_MAX_SOF = 0x02;
static constexpr uint8_t EVBOX_MAX_EOF = 0x03;
static constexpr uint8_t EVBOX_MAX_TRAILER = 0xFE;
static constexpr size_t EVBOX_MAX_MAX_FRAME = 220;

enum class FrameType : uint8_t {
  UNKNOWN = 0x00,
  REGISTRATION = 0x11,
  INFO_RESPONSE = 0x13,
  CONFIG_RESPONSE = 0x33,
  HEARTBEAT = 0x21,
  STATE_UPDATE = 0x26,
  CURRENT_REQUEST = 0x6A,
  CURRENT_SETPOINT = 0x6B,
  METERING_START = 0x23,
  METERING_END = 0x24,
  METER_PUSH = 0x66,
  FAULT = 0xFF,
};

struct Frame {
  uint8_t dst{0};
  uint8_t src{0};
  uint8_t cmd{0};
  uint8_t address{0};
  FrameType type{FrameType::UNKNOWN};
  std::string data{};
};

uint8_t evbox_checksum(const std::string &payload);
uint8_t evbox_parity(const std::string &payload);
std::string hex_byte(uint8_t value);
std::string hex_word(uint16_t value);
std::string hex_dword(uint32_t value);
uint8_t parse_hex_byte(const std::string &text, size_t offset, uint8_t fallback = 0);
uint32_t parse_hex_uint(const std::string &text, size_t offset, size_t length, uint32_t fallback = 0);
FrameType frame_type_for_cmd(uint8_t cmd);
std::vector<uint8_t> encode_frame(uint8_t src, uint8_t dst, uint8_t cmd, const std::string &data = "");

class FrameParser {
 public:
  bool push(uint8_t byte, Frame *frame);
  void reset();

 protected:
  bool parse_buffer_(Frame *frame);

  bool in_frame_{false};
  bool pending_trailer_{false};
  std::vector<uint8_t> buffer_{};
};

}  // namespace evbox_max
}  // namespace esphome
