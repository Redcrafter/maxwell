#include "search.h"

// clang-format off
#include <Windows.h>          // for IMAGE_SECTION_HEADER, GetModuleHandleA
#include <cstring>            // for memcmp
#include <exception>          // for terminate
#include <functional>         // for _Func_impl_no_alloc<>::_Mybase, equal_to
#include <list>               // for _List_iterator, _List_const_iterator
#include <locale>             // for num_put
#include <new>                // for operator new
#include <span>               // for span
#include <sstream>            // for basic_ostream, basic_streambuf, basic_s...
#include <stdexcept>          // for logic_error
#include <tuple>              // for get, apply, tuple
#include <type_traits>        // for move
#include <unordered_map>      // for unordered_map, _Umap_traits<>::allocato...
#include <utility>            // for min, max, pair, tuple_element<>::type
#include <vector>             // for vector, _Vector_const_iterator, _Vector...
#include <fmt/format.h>
// clang-format on

#include "ghidra_byte_string.h" // for operator""_gh
#include "logger.h"             // for ByteStr, DEBUG
#include "memory.h"             // for Memory, function_start
#include "virtual_table.h"

// Decodes the program counter inside an instruction
// The default simple variant is 3 bytes instruction, 4 bytes rel. address, 0
// bytes suffix:
//      e.g.  movups xmm0, ptr[XXXXXXXX] = 0F1005 XXXXXXXX
// Some instructions have 2 bytes instruction, so specify 2 for opcode_offset
//      e.g.  call ptr[XXXXXXXX] = FF15 XXXXXXXX
// Some (write) instructions have a value after the program counter to be
// extracted, so specify the opcode_suffix_offset
//      e.g.  mov word ptr[XXXXXXXX], 1 = 66:C705 XXXXXXXX 0100
//      (opcode_suffix_offset = 2)
size_t decode_pc(const char *exe, size_t offset, uint8_t opcode_offset,
                 uint8_t opcode_suffix_offset, uint8_t opcode_addr_size) {
  ptrdiff_t rel;
  switch (opcode_addr_size) {
  case 1:
    rel = *(int8_t *)(&exe[offset + opcode_offset]);
    break;
  case 2:
    rel = *(int16_t *)(&exe[offset + opcode_offset]);
    break;
  case 4:
  default:
    rel = *(int32_t *)(&exe[offset + opcode_offset]);
    break;
  case 8:
    rel = *(int64_t *)(&exe[offset + opcode_offset]);
    break;
  }
  return offset + rel + opcode_offset + opcode_addr_size + opcode_suffix_offset;
}

size_t decode_imm(const char *exe, size_t offset, uint8_t opcode_offset,
                  uint8_t value_size) {
  switch (value_size) {
  case 1:
    return *(uint8_t *)(&exe[offset + opcode_offset]);
  case 2:
    return *(uint16_t *)(&exe[offset + opcode_offset]);
  case 4:
  default:
    return *(uint32_t *)(&exe[offset + opcode_offset]);
  case 8:
    return *(uint64_t *)(&exe[offset + opcode_offset]);
  }
}

PIMAGE_NT_HEADERS RtlImageNtHeader(_In_ PVOID Base) {
  static HMODULE ntdll_dll = GetModuleHandleA("ntdll.dll");
  static auto proc = (decltype(RtlImageNtHeader) *)GetProcAddress(
      ntdll_dll, "RtlImageNtHeader");
  return proc(Base);
}

const char *current_spelunky_version() {
  static const char *version = "unknown!";
  static bool version_searched = false;
  if (!version_searched) {
    version_searched = true;
    auto memory = Memory::get();
    PIMAGE_NT_HEADERS nt_header = RtlImageNtHeader((PVOID)memory.exe());
    size_t rdata_start = 0;
    size_t rdata_size = 0;
    IMAGE_SECTION_HEADER *section_header =
        (IMAGE_SECTION_HEADER *)(nt_header + 1);
    for (int i = 0; i < nt_header->FileHeader.NumberOfSections; i++) {
      char *name = (char *)section_header->Name;
      if (memcmp(name, ".rdata", 6) == 0) {
        rdata_start = (size_t)(memory.exe() + section_header->VirtualAddress);
        rdata_size = section_header->Misc.VirtualSize;
        break;
      }
      section_header++;
    }
    if (rdata_start > 0 && rdata_size > 0) {
      std::string_view needle = "1.2"sv;
      const size_t needle_length = needle.size();
      const char *rdata = (const char *)rdata_start;
      size_t offset = 0;
      for (size_t j = 0; j < rdata_size - needle_length; j++) {
        bool found = true;
        for (size_t k = 0; k < needle_length && found; k++) {
          found = needle[k] == '*' || needle[k] == *(rdata + j + k);
        }
        if (found) {
          offset = rdata_start + j;
          break;
        }
      }
      if (offset != 0) {
        version = static_cast<const char *>((void *)offset);
      }
    }
  }
  return version;
}

static std::vector<std::string> g_registered_applications = {};
void register_application_version(std::string s) {
  g_registered_applications.emplace_back(std::move(s));
}

std::string application_versions() {
  if (g_registered_applications.empty()) {
    return "No application versions registered";
  }
  std::stringstream ss;
  for (const auto &s : g_registered_applications) {
    ss << s << "\n";
  }
  return ss.str();
}

std::string get_error_information() {
  return ""; // TODO
  return fmt::format(
      "\n\nRunning Spelunky 2: {}\nSupported Spelunky 2: 1.28\n\n{}",
      current_spelunky_version(), application_versions());
}

size_t find_inst(const char *exe, std::string_view needle, size_t start,
                 std::optional<size_t> end, std::string_view pattern_name,
                 bool is_required) {
  static const std::size_t exe_size = [exe]() {
    if (PIMAGE_NT_HEADERS pinth = RtlImageNtHeader((PVOID)exe)) {
      return (std::size_t)(pinth->OptionalHeader.BaseOfCode) +
             pinth->OptionalHeader.SizeOfCode;
    }
    return 0ull;
  }();

  const std::size_t needle_length = needle.size();
  const std::size_t search_end = end.value_or(exe_size);

  for (std::size_t j = start; j < search_end - needle_length; j++) {
    bool found = true;
    for (std::size_t k = 0; k < needle_length && found; k++) {
      found = needle[k] == '*' || needle[k] == *(exe + j + k);
    }

    if (found) {
      return j;
    }
  }

  std::string error_message;
  if (pattern_name.empty()) {
    error_message = fmt::format("Failed finding pattern '{}' in exe{}",
                                ByteStr{needle}, get_error_information());
  } else {
    error_message =
        fmt::format("Failed finding pattern '{}' ('{}') in exe{}", pattern_name,
                    ByteStr{needle}, get_error_information());
  }

  if (is_required) {
    if (MessageBox(NULL, error_message.c_str(), NULL, MB_OKCANCEL) ==
        IDCANCEL) {
      std::terminate();
    }
    return 0ull;
  } else {
    throw std::logic_error{error_message};
  }
}

size_t find_after_bundle(size_t exe) {
  auto offset = 0x1000;
  return offset; // TODO

  while (true) {
    uint32_t *cur = (uint32_t *)(exe + offset);
    uint32_t l0 = cur[0], l1 = cur[1];
    if (l0 == 0 && l1 == 0) {
      break;
    }
    offset += (8 + l0 + l1);
  }

  return find_inst((char *)exe, "\x55\x41\x57\x41\x56\x41\x55\x41\x54"sv,
                   offset);
}

class PatternCommandBuffer {
public:
  PatternCommandBuffer() = default;
  PatternCommandBuffer(const PatternCommandBuffer &) = default;
  PatternCommandBuffer(PatternCommandBuffer &&) noexcept = default;
  PatternCommandBuffer &operator=(const PatternCommandBuffer &) = default;
  PatternCommandBuffer &operator=(PatternCommandBuffer &&) noexcept = default;

  PatternCommandBuffer &set_optional(bool optional) {
    commands.push_back({CommandType::SetOptional, {.optional = optional}});
    return *this;
  }
  PatternCommandBuffer &get_address(std::string_view address_name) {
    commands.push_back(
        {CommandType::GetAddress, {.address_name = address_name}});
    return *this;
  }
  PatternCommandBuffer &get_virtual_function_address(VTABLE_OFFSET table_offset,
                                                     VIRT_FUNC function_index) {
    commands.push_back(
        {CommandType::GetVirtualFunctionAddress,
         {.get_vfunc_addr_args = {.table_offset = table_offset,
                                  .function_index = function_index}}});
    return *this;
  }
  PatternCommandBuffer &find_inst(std::string_view pattern) {
    commands.push_back({CommandType::FindInst, {.find_inst_args = {pattern}}});
    return *this;
  }
  PatternCommandBuffer &find_after_inst(std::string_view pattern) {
    return find_inst(pattern).offset(pattern.size());
  }
  PatternCommandBuffer &find_next_inst(std::string_view pattern) {
    return offset(0x1).find_inst(pattern);
  }
  PatternCommandBuffer &find_inst_in_range(std::string_view pattern,
                                           size_t range) {
    commands.push_back(
        {CommandType::FindInst,
         {.find_inst_args = {.pattern = pattern, .range = range}}});
    return *this;
  }
  PatternCommandBuffer &find_after_inst_in_range(std::string_view pattern,
                                                 size_t range) {
    return find_inst_in_range(pattern, range).offset(pattern.size());
  }
  PatternCommandBuffer &find_next_inst_in_range(std::string_view pattern,
                                                size_t range) {
    return offset(0x1).find_inst_in_range(pattern, range);
  }
  PatternCommandBuffer &offset(int64_t offset) {
    commands.push_back({CommandType::Offset, {.offset = offset}});
    return *this;
  }
  PatternCommandBuffer &decode_pc(uint8_t opcode_prefix = 3,
                                  uint8_t opcode_suffix = 0,
                                  uint8_t opcode_addr = 4) {
    commands.push_back(
        {CommandType::DecodePC,
         {.decode_pc_args = {opcode_prefix, opcode_suffix, opcode_addr}}});
    return *this;
  }
  PatternCommandBuffer &decode_imm(uint8_t opcode_prefix = 3,
                                   uint8_t value_size = 4) {
    commands.push_back({CommandType::DecodeIMM,
                        {.decode_imm_args = {opcode_prefix, value_size}}});
    return *this;
  }
  PatternCommandBuffer &decode_call() {
    commands.push_back({CommandType::DecodeCall});
    return *this;
  }
  PatternCommandBuffer &at_exe() {
    commands.push_back({CommandType::AtExe});
    return *this;
  }
  PatternCommandBuffer &from_exe() {
    commands.push_back({CommandType::FromExe});
    return *this;
  }
  PatternCommandBuffer &function_start(uint8_t outside_byte = 0xcc) {
    commands.push_back(
        {CommandType::FunctionStart, {.outside_byte = outside_byte}});
    return *this;
  }

  // Rapid prototyping only please
  PatternCommandBuffer &from_exe_base(uint64_t offset) {
    commands.push_back({CommandType::FromExeBase, {.base_offset = offset}});
    commands.push_back({CommandType::AtExe});
    return *this;
  }

  std::optional<size_t> operator()(Memory mem, const char *exe,
                                   std::string_view address_name) const {
    size_t offset = mem.after_bundle;
    bool optional{false};

#ifdef DEBUG
    static constexpr auto debug_pattern = ""sv;
    if constexpr (!debug_pattern.empty()) {
      if (address_name == debug_pattern) {
        __debugbreak();
      }
    }
#endif // DEBUG

    for (auto &[command, data] : commands) {
      switch (command) {
      case CommandType::SetOptional:
        optional = data.optional;
        break;
      case CommandType::GetAddress:
        offset = ::get_address(data.address_name);
        if (optional && offset == 0) {
          return 0;
        } else {
          offset -= (size_t)exe;
        }
        break;
      case CommandType::GetVirtualFunctionAddress:
        offset = ::get_virtual_function_address(
            data.get_vfunc_addr_args.table_offset,
            static_cast<uint32_t>(data.get_vfunc_addr_args.function_index));
        break;
      case CommandType::FindInst:
        try {
          if (data.find_inst_args.range.has_value()) {
            offset = ::find_inst(exe, data.find_inst_args.pattern, offset,
                                 offset + data.find_inst_args.range.value(),
                                 address_name, !optional);
          } else {
            offset = ::find_inst(exe, data.find_inst_args.pattern, offset,
                                 std::nullopt, address_name, !optional);
          }
        } catch (const std::logic_error &) {
          return 0;
        }
        break;
      case CommandType::Offset:
        offset = offset + data.offset;
        break;
      case CommandType::DecodePC:
        offset = std::apply(
            [=](auto... args) { return ::decode_pc(exe, offset, args...); },
            data.decode_pc_args.as_tuple());
        break;
      case CommandType::DecodeIMM:
        offset = std::apply(
            [=](auto... args) { return ::decode_imm(exe, offset, args...); },
            data.decode_imm_args.as_tuple());
        break;
      case CommandType::DecodeCall:
        offset = mem.decode_call(offset);
        break;
      case CommandType::AtExe:
        offset = mem.at_exe(offset);
        break;
      case CommandType::FromExe:
        offset = offset - mem.exe_ptr;
        break;
      case CommandType::FunctionStart:
        offset = ::function_start(offset, data.outside_byte);
        break;
      case CommandType::FromExeBase:
        offset = data.base_offset;
        break;
      default:
        DEBUG("Unkown command...");
        break;
      }
    }

    return offset;
  }

private:
  struct DecodePcArgs {
    uint8_t opcode_offset;
    uint8_t opcode_suffix_offset;
    uint8_t opcode_addr_size;

    std::tuple<uint8_t, uint8_t, uint8_t> as_tuple() const {
      return {opcode_offset, opcode_suffix_offset, opcode_addr_size};
    }
  };
  struct DecodeImmArgs {
    uint8_t opcode_offset;
    uint8_t value_size;

    std::tuple<uint8_t, uint8_t> as_tuple() const {
      return {opcode_offset, value_size};
    }
  };
  struct FindInstArgs {
    std::string_view pattern;
    std::optional<size_t> range;
  };
  struct GetVirtualFunctionAddressArgs {
    VTABLE_OFFSET table_offset;
    VIRT_FUNC function_index;
  };

  enum class CommandType {
    SetOptional,
    GetAddress,
    GetVirtualFunctionAddress,
    FindInst,
    Offset,
    DecodePC,
    DecodeIMM,
    DecodeCall,
    AtExe,
    FromExe,
    FunctionStart,
    FromExeBase,
  };
  union CommandData {
    bool optional;
    std::string_view address_name;
    FindInstArgs find_inst_args;
    int64_t offset;
    DecodePcArgs decode_pc_args;
    DecodeImmArgs decode_imm_args;
    GetVirtualFunctionAddressArgs get_vfunc_addr_args;
    uint64_t base_offset;
    uint8_t outside_byte;
  };
  struct Command {
    CommandType command;
    CommandData data;
  };
  std::vector<Command> commands;
};

using AddressRule = std::function<std::optional<size_t>(
    Memory mem, const char *exe, std::string_view address_name)>;
std::unordered_map<std::string_view, AddressRule> g_address_rules{
    {
        // RE: Used in setupGame and updateGame
        "get_state_func"sv,
        PatternCommandBuffer{}
            .find_inst(
                "48 89 35 54 58 be 02 e8 7f 8b 04 00 48 81 c6 08 1c b0 00"_gh)
            .offset(7)
            .decode_call()
            .at_exe(),
    },
    {
        // RE: Check what writes 3 to player health at start, this is just
        // before that
        "slots"sv,
        PatternCommandBuffer{}
            .find_inst(
                "48 8b 05 .. .. .. .. 48 8b .. .. .. .. .. 48 89 8c 1f ec 05 00 00"_gh)
            .decode_pc()
            .at_exe(),
    },
    {
        // RE: Check what keeps messing up with your edits
        "check"sv,
        PatternCommandBuffer{}
            .find_inst("48 83 c3 10 48 81 fb 50 89 00 00"_gh)
            .offset(-6)
            .at_exe(),
    },
    {
        // RE:
        "warp"sv,
        PatternCommandBuffer{}
            .find_inst("4c 8d 6e 28 8a 86 1d 6b 02 00"_gh)
            .offset(12)
            .at_exe(),
    },
    {
        // RE: It's a call to GetKeyboardState...
        "keyboard"sv,
        PatternCommandBuffer{}.from_exe_base(0x13aba),
    },
    {
        // RE:
        "layer_base"sv,
        PatternCommandBuffer{}
            .find_inst("4c 8b 35 .. .. .. .. 48 8d 9e 10 36 03 00"_gh)
            .offset(14)
            .decode_call()
            .find_next_inst("48 8b 05"_gh)
            .decode_pc()
            .at_exe(),
    },
    {
        // RE:
        "layer_offset"sv,
        PatternCommandBuffer{}
            .find_inst("4c 8b 35 .. .. .. .. 48 8d 9e 10 36 03 00"_gh)
            .offset(14)
            .decode_call()
            .find_after_inst("8b 80"_gh)
            .at_exe(),
    },
};
std::unordered_map<std::string_view, size_t> g_cached_addresses;

void preload_addresses() {
  Memory mem = Memory::get();
  const char *exe = mem.exe();
  for (auto [address_name, rule] : g_address_rules) {
    if (auto address = rule(mem, exe, address_name)) {
      for (auto &[k, v] : g_cached_addresses) {
        if (v == address.value() && k != address_name) {
          DEBUG("Two patterns refer to the same address: {} & {}", k,
                address_name);
        }
      }
      g_cached_addresses[address_name] = address.value();
    }
  }
}
size_t load_address(std::string_view address_name) {
  auto it = g_address_rules.find(address_name);
  if (it != g_address_rules.end()) {
    Memory mem = Memory::get();
    if (auto address = it->second(mem, mem.exe(), address_name)) {
      g_cached_addresses[address_name] = address.value();
      return address.value();
    }
  }
  const std::string message =
      fmt::format("Tried to get unknown address '{}'{}", address_name,
                  get_error_information());
  MessageBox(NULL, message.c_str(), NULL, MB_OK);
  return 0ull;
}
size_t get_address(std::string_view address_name) {
  auto it = g_cached_addresses.find(address_name);
  if (it != g_cached_addresses.end()) {
    return it->second;
  }
  return load_address(address_name);
}
