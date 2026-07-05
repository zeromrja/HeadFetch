#pragma once

#include <cstddef>

namespace hf::Game {
constexpr std::size_t HudCursor_RenderSlot = 17;
constexpr std::size_t PlayerListPacket_ReadSlot = 17;
constexpr std::size_t PLP_EntriesBegin = 0x30;
constexpr std::size_t PLP_EntriesEnd = 0x38;
constexpr std::size_t PLP_Action = 0x48;
constexpr std::size_t PLP_EntrySize = 0x90;
constexpr std::size_t PLP_EntryUuid = 0x08;
constexpr std::size_t PLP_EntryName = 0x18;
// Inferred, not disassembled: PLP_EntryName (0x18) + sizeof(std::string) (0x18)
// lands exactly on PLP_EntryXuid, and XUID + PlatformChatId + BuildPlatform(+pad)
// land exactly on PLP_EntrySkinRef (0x68), which is confirmed working. Verify
// against your libminecraftpe.so build before relying on it -- if the head
// fetch silently never triggers, this is the first thing to check.
constexpr std::size_t PLP_EntryXuid = 0x30;
constexpr std::size_t PLP_EntrySkinRef = 0x68;
constexpr std::size_t Image_BytesOffset = 0x18;
constexpr std::size_t Image_BytesSizeOffset = 0x28;
constexpr std::size_t MaxSkinImageScan = 0x180;
} // namespace hf::Game
