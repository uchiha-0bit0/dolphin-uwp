// Copyright 2026 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include "DolphinUWP/FifoAnalysis.h"

#include <bit>
#include <utility>

#include <fmt/format.h>

#include "Common/EnumMap.h"
#include "Common/Swap.h"
#include "Core/FifoPlayer/FifoDataFile.h"
#include "Core/FifoPlayer/FifoPlayer.h"
#include "Core/System.h"
#include "VideoCommon/BPMemory.h"
#include "VideoCommon/CPMemory.h"
#include "VideoCommon/OpcodeDecoding.h"
#include "VideoCommon/XFStructs.h"

namespace DolphinUWP::FifoAnalysis
{
namespace
{
FifoPlayer& GetPlayer()
{
  return Core::System::GetInstance().GetFifoPlayer();
}

std::string NameAndDescription(std::string_view kind, std::string_view name, std::string_view desc)
{
  return fmt::format("{} {}\n{}", kind, name, desc.empty() ? "No description available" : desc);
}

// One line per command.
class DetailCallback : public OpcodeDecoder::Callback
{
public:
  explicit DetailCallback(const CPState& cpmem) : m_cpmem(cpmem) {}

  OPCODE_CALLBACK(void OnCP(const u8 command, const u32 value))
  {
    const auto [name, desc] = GetCPRegInfo(command, value);
    text = fmt::format("CP  {:02x}  {:08x}  {}", command, value, name);
  }

  OPCODE_CALLBACK(void OnXF(const u16 address, const u8 count, const u8* data))
  {
    const auto [name, desc] = GetXFTransferInfo(address, count, data);
    text = fmt::format("XF  {:08x}  ", address | ((count - 1) << 16));
    for (u8 i = 0; i < count; i++)
      text += fmt::format("{:08x} ", Common::swap32(&data[i * 4]));
    text += "  " + name;
  }

  OPCODE_CALLBACK(void OnBP(const u8 command, const u32 value))
  {
    const auto [name, desc] = GetBPRegInfo(command, value);
    text = fmt::format("BP  {:02x}  {:06x}  {}", command, value, name);
  }

  OPCODE_CALLBACK(void OnIndexedLoad(const CPArray array, const u32 index, const u16 address,
                                     const u8 size))
  {
    const auto [desc, written] = GetXFIndexedLoadInfo(array, index, address, size);
    text = fmt::format("LOAD INDX {}   {}", array, desc);
  }

  OPCODE_CALLBACK(void OnPrimitiveCommand(const OpcodeDecoder::Primitive primitive, const u8 vat,
                                          const u32 vertex_size, const u16 num_vertices,
                                          const u8* vertex_data))
  {
    const u8 opcode =
        0x80 | std::to_underlying(primitive) << OpcodeDecoder::GX_PRIMITIVE_SHIFT | vat;
    text = fmt::format("PRIMITIVE {} ({:02x})  {} vertices {} bytes/vertex {} total bytes",
                       primitive, opcode, num_vertices, vertex_size, num_vertices * vertex_size);
  }

  OPCODE_CALLBACK(void OnDisplayList(const u32 address, const u32 size))
  {
    text = fmt::format("Call display list at {:08x} with size {:08x}", address, size);
  }

  OPCODE_CALLBACK(void OnNop(const u32 count))
  {
    text = count > 1 ? fmt::format("NOP ({}x)", count) : std::string("NOP");
  }

  OPCODE_CALLBACK(void OnUnknown(u8 opcode, const u8* data))
  {
    using OpcodeDecoder::Opcode;
    if (static_cast<Opcode>(opcode) == Opcode::GX_CMD_UNKNOWN_METRICS)
      text = "GX_CMD_UNKNOWN_METRICS";
    else if (static_cast<Opcode>(opcode) == Opcode::GX_CMD_INVL_VC)
      text = "GX_CMD_INVL_VC";
    else
      text = fmt::format("Unknown opcode {:02x}", opcode);
  }

  OPCODE_CALLBACK(void OnCommand(const u8* data, u32 size)) {}

  OPCODE_CALLBACK(CPState& GetCPState()) { return m_cpmem; }

  std::string text;
  CPState m_cpmem;
};

// The full description of a command, including the vertex data of primitives.
class DescriptionCallback : public OpcodeDecoder::Callback
{
public:
  explicit DescriptionCallback(const CPState& cpmem) : m_cpmem(cpmem) {}

  OPCODE_CALLBACK(void OnBP(const u8 command, const u32 value))
  {
    const auto [name, desc] = GetBPRegInfo(command, value);
    text = NameAndDescription("BP register", name, desc);
  }

  OPCODE_CALLBACK(void OnCP(const u8 command, const u32 value))
  {
    const auto [name, desc] = GetCPRegInfo(command, value);
    text = NameAndDescription("CP register", name, desc);
  }

  OPCODE_CALLBACK(void OnXF(const u16 address, const u8 count, const u8* data))
  {
    const auto [name, desc] = GetXFTransferInfo(address, count, data);
    text = NameAndDescription("XF register", name, desc);
  }

  OPCODE_CALLBACK(void OnIndexedLoad(const CPArray array, const u32 index, const u16 address,
                                     const u8 size))
  {
    const auto [desc, written] = GetXFIndexedLoadInfo(array, index, address, size);
    text = desc + '\n';
    switch (array)
    {
    case CPArray::XF_A:
      text += "Usually used for position matrices";
      break;
    case CPArray::XF_B:
      text += "Usually used for normal matrices";
      break;
    case CPArray::XF_C:
      text += "Usually used for tex coord matrices";
      break;
    case CPArray::XF_D:
      text += "Usually used for light objects";
      break;
    default:
      break;
    }
    text += '\n' + written;
  }

  OPCODE_CALLBACK(void OnPrimitiveCommand(OpcodeDecoder::Primitive primitive, u8 vat,
                                          const u32 vertex_size, const u16 num_vertices,
                                          const u8* vertex_data))
  {
    text = fmt::format("Primitive {} VAT {}\n", primitive, vat);

    const auto& [low, high] = m_cpmem.vtx_desc;
    const auto& vtx_attr = m_cpmem.vtx_attr[vat];

    u32 i = 0;
    const auto process_component = [&](const VertexComponentFormat cformat, ComponentFormat format,
                                       const u32 non_indexed_count, const u32 indexed_count = 1) {
      u32 count;
      if (cformat == VertexComponentFormat::NotPresent)
        return;
      if (cformat == VertexComponentFormat::Index8)
      {
        format = ComponentFormat::UByte;
        count = indexed_count;
      }
      else if (cformat == VertexComponentFormat::Index16)
      {
        format = ComponentFormat::UShort;
        count = indexed_count;
      }
      else
      {
        count = non_indexed_count;
      }

      const u32 component_size = GetElementSize(format);
      for (u32 j = 0; j < count; j++)
      {
        for (u32 component_off = 0; component_off < component_size; component_off++)
          text += fmt::format("{:02x}", vertex_data[i + component_off]);
        if (format == ComponentFormat::Float)
          text += fmt::format(" ({})", std::bit_cast<float>(Common::swap32(&vertex_data[i])));
        i += component_size;
        text += ' ';
      }
      text += ' ';
    };
    const auto process_simple_component = [&](const u32 size) {
      for (u32 component_off = 0; component_off < size; component_off++)
        text += fmt::format("{:02x}", vertex_data[i + component_off]);
      i += size;
      text += "  ";
    };

    for (u32 vertex_num = 0; vertex_num < num_vertices; vertex_num++)
    {
      text += '\n';
      if (low.PosMatIdx)
        process_simple_component(1);
      for (auto texmtxidx : low.TexMatIdx)
      {
        if (texmtxidx)
          process_simple_component(1);
      }
      process_component(low.Position, vtx_attr.g0.PosFormat,
                        vtx_attr.g0.PosElements == CoordComponentCount::XY ? 2 : 3);
      const u32 normal_component_count = low.Normal == VertexComponentFormat::Direct ? 3 : 1;
      const u32 normal_elements = vtx_attr.g0.NormalElements == NormalComponentCount::NTB ? 3 : 1;
      process_component(low.Normal, vtx_attr.g0.NormalFormat,
                        normal_component_count * normal_elements,
                        vtx_attr.g0.NormalIndex3 ? normal_elements : 1);
      for (u32 c = 0; c < low.Color.Size(); c++)
      {
        static constexpr Common::EnumMap<u32, ColorFormat::RGBA8888> component_sizes = {
            2,  // RGB565
            3,  // RGB888
            4,  // RGB888x
            2,  // RGBA4444
            3,  // RGBA6666
            4,  // RGBA8888
        };
        switch (low.Color[c])
        {
        case VertexComponentFormat::Index8:
          process_simple_component(1);
          break;
        case VertexComponentFormat::Index16:
          process_simple_component(2);
          break;
        case VertexComponentFormat::Direct:
          process_simple_component(component_sizes[vtx_attr.GetColorFormat(c)]);
          break;
        case VertexComponentFormat::NotPresent:
          break;
        }
      }
      for (u32 t = 0; t < high.TexCoord.Size(); t++)
      {
        process_component(high.TexCoord[t], vtx_attr.GetTexFormat(t),
                          vtx_attr.GetTexElements(t) == TexComponentCount::ST ? 2 : 1);
      }
    }
  }

  OPCODE_CALLBACK(void OnDisplayList(u32 address, u32 size)) { text = "No description available"; }
  OPCODE_CALLBACK(void OnNop(u32 count)) { text = "No description available"; }
  OPCODE_CALLBACK(void OnUnknown(u8 opcode, const u8* data)) { text = "No description available"; }
  OPCODE_CALLBACK(void OnCommand(const u8* data, u32 size)) {}
  OPCODE_CALLBACK(CPState& GetCPState()) { return m_cpmem; }

  std::string text;
  CPState m_cpmem;
};
}  // namespace

bool IsAvailable()
{
  return GetPlayer().IsPlaying() && GetPlayer().GetFile();
}

u32 GetFrameCount()
{
  return GetPlayer().GetFile()->GetFrameCount();
}

u32 GetObjectCount(u32 frame)
{
  const auto& counts = GetPlayer().GetAnalyzedFrameInfo(frame).part_type_counts;
  return counts[FramePartType::PrimitiveData] + counts[FramePartType::EFBCopy];
}

std::vector<Object> GetObjects(u32 frame)
{
  std::vector<Object> objects;
  const auto& [parts, part_type_counts] = GetPlayer().GetAnalyzedFrameInfo(frame);
  Common::EnumMap<u32, FramePartType::EFBCopy> part_counts;
  u32 first_part = 0;
  for (u32 part_nr = 0; part_nr < parts.size(); part_nr++)
  {
    const FramePart& part = parts[part_nr];
    const u32 number = part_counts[part.m_type]++;
    // Commands belong to the primitive data or EFB copy that follows them.
    if (part.m_type == FramePartType::Commands)
      continue;
    objects.push_back({part.m_type == FramePartType::PrimitiveData ?
                           fmt::format("Object {}", number) :
                           fmt::format("EFB copy {}", number),
                       frame, first_part, part_nr});
    first_part = part_nr + 1;
  }
  return objects;
}

std::vector<Command> GetCommands(const Object& object)
{
  std::vector<Command> commands;
  const auto& parts = GetPlayer().GetAnalyzedFrameInfo(object.frame).parts;
  const FifoFrameInfo& fifo_frame = GetPlayer().GetFile()->GetFrame(object.frame);
  const u32 object_start = parts[object.first_part].m_start;
  const u32 object_size = parts[object.last_part].m_end - object_start;

  // The state of the CP registers after the object matches the state its primitives use.
  DetailCallback callback(parts[object.last_part].m_cpmem);
  u32 offset = 0;
  while (offset < object_size)
  {
    const u32 start = offset;
    offset += OpcodeDecoder::RunCommand(&fifo_frame.fifoData[object_start + start],
                                        object_size - start, callback);
    commands.push_back({object_start + start, std::move(callback.text)});
    if (offset == start)
      break;
  }
  return commands;
}

std::string Describe(const Object& object, u32 offset)
{
  const auto& parts = GetPlayer().GetAnalyzedFrameInfo(object.frame).parts;
  const FifoFrameInfo& fifo_frame = GetPlayer().GetFile()->GetFrame(object.frame);
  const u32 object_end = parts[object.last_part].m_end;
  if (offset >= object_end)
    return {};

  DescriptionCallback callback(parts[object.last_part].m_cpmem);
  OpcodeDecoder::RunCommand(&fifo_frame.fifoData[offset], object_end - offset, callback);
  return callback.text;
}
}  // namespace DolphinUWP::FifoAnalysis
