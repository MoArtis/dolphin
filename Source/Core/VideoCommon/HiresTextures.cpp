// Copyright 2009 Dolphin Emulator Project
// Licensed under GPLv2+
// Refer to the license.txt file included.

#include "VideoCommon/HiresTextures.h"

#include <algorithm>
#include <memory>
#include <mutex>
#include <string>
#include <string_view>
#include <thread>
#include <unordered_map>
#include <utility>
#include <vector>
#include <xxhash.h>

#include <fmt/format.h>

#include "Common/CommonPaths.h"
#include "Common/File.h"
#include "Common/FileSearch.h"
#include "Common/FileUtil.h"
#include "Common/Flag.h"
#include "Common/Image.h"
#include "Common/Logging/Log.h"
#include "Common/MemoryUtil.h"
#include "Common/StringUtil.h"
#include "Common/Swap.h"
#include "Common/Thread.h"
#include "Common/Timer.h"
#include "Core/Config/GraphicsSettings.h"
#include "Core/ConfigManager.h"
#include "VideoCommon/OnScreenDisplay.h"
#include "VideoCommon/VideoConfig.h"

struct DiskTexture
{
  std::string path;
  bool has_arbitrary_mipmaps;
};

constexpr std::string_view s_format_prefix{"tex1_"};

static std::unordered_map<std::string, DiskTexture> s_textureMap;
static std::unordered_map<std::string, std::shared_ptr<HiresTexture>> s_textureCache;
static std::mutex s_textureCacheMutex;
static Common::Flag s_textureCacheAbortLoading;

static std::thread s_prefetcher;

void HiresTexture::Init()
{
  // Note: Update is not called here so that we handle dynamic textures on startup more gracefully
  
  // Mask hack for RE3
  ReThreeMaskHackInit();
  // ReThreeRoomIdOsdInit();
}

void HiresTexture::Shutdown()
{
  if (s_prefetcher.joinable())
  {
    s_textureCacheAbortLoading.Set();
    s_prefetcher.join();
  }

  s_textureMap.clear();
  s_textureCache.clear();
}

void HiresTexture::Update()
{
  if (s_prefetcher.joinable())
  {
    s_textureCacheAbortLoading.Set();
    s_prefetcher.join();
  }

  if (!g_ActiveConfig.bHiresTextures)
  {
    Clear();
    return;
  }

  if (!g_ActiveConfig.bCacheHiresTextures)
  {
    s_textureCache.clear();
  }

  const std::string& game_id = SConfig::GetInstance().GetGameID();
  const std::set<std::string> texture_directories =
      GetTextureDirectoriesWithGameId(File::GetUserPath(D_HIRESTEXTURES_IDX), game_id);
  const std::vector<std::string> extensions{".png", ".dds"};

  for (const auto& texture_directory : texture_directories)
  {
    const auto texture_paths =
        Common::DoFileSearch({texture_directory}, extensions, /*recursive*/ true);

    bool failed_insert = false;
    for (auto& path : texture_paths)
    {
      std::string filename;
      SplitPath(path, nullptr, &filename, nullptr);

      if (filename.substr(0, s_format_prefix.length()) == s_format_prefix)
      {
        const size_t arb_index = filename.rfind("_arb");
        const bool has_arbitrary_mipmaps = arb_index != std::string::npos;
        if (has_arbitrary_mipmaps)
          filename.erase(arb_index, 4);

        const auto [it, inserted] =
            s_textureMap.try_emplace(filename, DiskTexture{path, has_arbitrary_mipmaps});
        if (!inserted)
        {
          failed_insert = true;
        }
      }
    }

    if (failed_insert)
    {
      ERROR_LOG_FMT(VIDEO, "One or more textures at path '{}' were already inserted",
                    texture_directory);
    }
  }

  if (g_ActiveConfig.bCacheHiresTextures)
  {
    // remove cached but deleted textures
    auto iter = s_textureCache.begin();
    while (iter != s_textureCache.end())
    {
      if (s_textureMap.find(iter->first) == s_textureMap.end())
      {
        iter = s_textureCache.erase(iter);
      }
      else
      {
        iter++;
      }
    }

    s_textureCacheAbortLoading.Clear();
    s_prefetcher = std::thread(Prefetch);
  }
}

void HiresTexture::Clear()
{
  s_textureMap.clear();
  s_textureCache.clear();
}

void HiresTexture::Prefetch()
{
  Common::SetCurrentThreadName("Prefetcher");

  size_t size_sum = 0;
  const size_t sys_mem = Common::MemPhysical();
  const size_t recommended_min_mem = 2 * size_t(1024 * 1024 * 1024);
  // keep 2GB memory for system stability if system RAM is 4GB+ - use half of memory in other cases
  const size_t max_mem =
      (sys_mem / 2 < recommended_min_mem) ? (sys_mem / 2) : (sys_mem - recommended_min_mem);

  const u32 start_time = Common::Timer::GetTimeMs();
  for (const auto& entry : s_textureMap)
  {
    const std::string& base_filename = entry.first;

    if (base_filename.find("_mip") == std::string::npos)
    {
      std::unique_lock<std::mutex> lk(s_textureCacheMutex);

      auto iter = s_textureCache.find(base_filename);
      if (iter == s_textureCache.end())
      {
        // unlock while loading a texture. This may result in a race condition where
        // we'll load a texture twice, but it reduces the stuttering a lot.
        lk.unlock();
        std::unique_ptr<HiresTexture> texture = Load(base_filename, 0, 0);
        lk.lock();
        if (texture)
        {
          std::shared_ptr<HiresTexture> ptr(std::move(texture));
          iter = s_textureCache.insert(iter, std::make_pair(base_filename, ptr));
        }
      }
      if (iter != s_textureCache.end())
      {
        for (const Level& l : iter->second->m_levels)
          size_sum += l.data.size();
      }
    }

    if (s_textureCacheAbortLoading.IsSet())
    {
      return;
    }

    if (size_sum > max_mem)
    {
      Config::SetCurrent(Config::GFX_HIRES_TEXTURES, false);

      OSD::AddMessage(
          fmt::format(
              "Custom Textures prefetching after {:.1f} MB aborted, not enough RAM available",
              size_sum / (1024.0 * 1024.0)),
          10000);
      return;
    }
  }

  const u32 stop_time = Common::Timer::GetTimeMs();
  OSD::AddMessage(fmt::format("Custom Textures loaded, {:.1f} MB in {:.1f}s",
                              size_sum / (1024.0 * 1024.0), (stop_time - start_time) / 1000.0),
                  10000);
}

std::string HiresTexture::GenBaseName(const u8* texture, size_t texture_size, const u8* tlut,
                                      size_t tlut_size, u32 width, u32 height, TextureFormat format,
                                      bool has_mipmaps, bool dump)
{
  if (!dump && s_textureMap.empty())
    return "";

  // checking for min/max on paletted textures
  u32 min = 0xffff;
  u32 max = 0;

  switch (tlut_size)
  {
  case 0:
    break;
  case 16 * 2:
    for (size_t i = 0; i < texture_size; i++)
    {
      const u32 low_nibble = texture[i] & 0xf;
      const u32 high_nibble = texture[i] >> 4;

      min = std::min({min, low_nibble, high_nibble});
      max = std::max({max, low_nibble, high_nibble});
    }
    break;
  case 256 * 2:
  {
    for (size_t i = 0; i < texture_size; i++)
    {
      const u32 texture_byte = texture[i];

      min = std::min(min, texture_byte);
      max = std::max(max, texture_byte);
    }
    break;
  }
  case 16384 * 2:
    for (size_t i = 0; i < texture_size; i += sizeof(u16))
    {
      const u32 texture_halfword = Common::swap16(texture[i]) & 0x3fff;

      min = std::min(min, texture_halfword);
      max = std::max(max, texture_halfword);
    }
    break;
  }
  if (tlut_size > 0)
  {
    tlut_size = 2 * (max + 1 - min);
    tlut += 2 * min;
  }

  const u64 tex_hash = XXH64(texture, texture_size, 0);
  const u64 tlut_hash = tlut_size ? XXH64(tlut, tlut_size, 0) : 0;

  //const std::string base_name = fmt::format("{}{}x{}{}_{:016x}", s_format_prefix, width, height,
  //                                          has_mipmaps ? "_m" : "", tex_hash);
  //const std::string tlut_name = tlut_size ? fmt::format("_{:016x}", tlut_hash) : "";
  //const std::string format_name = fmt::format("_{}", static_cast<int>(format));
  //const std::string full_name = base_name + tlut_name + format_name;

  //// try to match a wildcard template
  //if (!dump)
  //{
  //  const std::string texture_name = fmt::format("{}_${}", base_name, format_name);
  //  if (s_textureMap.find(texture_name) != s_textureMap.end())
  //    return texture_name;
  //}

  const std::string base_name = fmt::format("{}{}x{}{}", s_format_prefix, width, height, has_mipmaps ? "_m" : "");

  // RESHDP Hack - Separate the texname from the basename
  const std::string tex_name = fmt::format("_{:016x}", tex_hash);
  std::string tlut_name = tlut_size ? fmt::format("_{:016x}", tlut_hash) : "";
  const std::string format_name = fmt::format("_{}", static_cast<int>(format));

  ReThreeMaskHack(tex_name, tlut_name, width);

  const std::string full_name = base_name + tex_name + tlut_name + format_name;

  // RESHDP Hack - Display the room id in the OSD. Critical for debugging the pack.
  // ReThreeRoomIdOsd(fullname, width);

  // RESHDP Hack - Display the generated texture name in the OSD. Critical for debugging the pack.
  // OSD::AddMessage(fullname, 5000, 4278190080 + width * 5000);

  // try to match a wildcard template
  if (!dump && s_textureMap.find(base_name + tex_name + "_$" + format_name) != s_textureMap.end())
    return base_name + tex_name + "_$" + format_name;

  // RESHDP Hack - try to match the TLUT wildcard template (RE games / RESHDP ONLY)
  if (!dump && s_textureMap.find(base_name + "_$" + tlut_name + format_name) != s_textureMap.end())
    return base_name + "_$" + tlut_name + format_name;

  // else generate the complete texture
  if (dump || s_textureMap.find(full_name) != s_textureMap.end())
    return full_name;

  return "";
}

u32 HiresTexture::CalculateMipCount(u32 width, u32 height)
{
  u32 mip_width = width;
  u32 mip_height = height;
  u32 mip_count = 1;
  while (mip_width > 1 || mip_height > 1)
  {
    mip_width = std::max(mip_width / 2, 1u);
    mip_height = std::max(mip_height / 2, 1u);
    mip_count++;
  }

  return mip_count;
}

std::shared_ptr<HiresTexture> HiresTexture::Search(const u8* texture, size_t texture_size,
                                                   const u8* tlut, size_t tlut_size, u32 width,
                                                   u32 height, TextureFormat format,
                                                   bool has_mipmaps)
{
  std::string base_filename =
      GenBaseName(texture, texture_size, tlut, tlut_size, width, height, format, has_mipmaps);

  std::lock_guard<std::mutex> lk(s_textureCacheMutex);

  auto iter = s_textureCache.find(base_filename);
  if (iter != s_textureCache.end())
  {
    return iter->second;
  }

  std::shared_ptr<HiresTexture> ptr(Load(base_filename, width, height));

  if (ptr && g_ActiveConfig.bCacheHiresTextures)
  {
    s_textureCache[base_filename] = ptr;
  }

  return ptr;
}

std::unique_ptr<HiresTexture> HiresTexture::Load(const std::string& base_filename, u32 width,
                                                 u32 height)
{
  // We need to have a level 0 custom texture to even consider loading.
  auto filename_iter = s_textureMap.find(base_filename);
  if (filename_iter == s_textureMap.end())
    return nullptr;

  // Try to load level 0 (and any mipmaps) from a DDS file.
  // If this fails, it's fine, we'll just load level0 again using SOIL.
  // Can't use make_unique due to private constructor.
  std::unique_ptr<HiresTexture> ret = std::unique_ptr<HiresTexture>(new HiresTexture());
  const DiskTexture& first_mip_file = filename_iter->second;
  ret->m_has_arbitrary_mipmaps = first_mip_file.has_arbitrary_mipmaps;
  LoadDDSTexture(ret.get(), first_mip_file.path);

  // Load remaining mip levels, or from the start if it's not a DDS texture.
  for (u32 mip_level = static_cast<u32>(ret->m_levels.size());; mip_level++)
  {
    std::string filename = base_filename;
    if (mip_level != 0)
      filename += fmt::format("_mip{}", mip_level);

    filename_iter = s_textureMap.find(filename);
    if (filename_iter == s_textureMap.end())
      break;

    // Try loading DDS textures first, that way we maintain compression of DXT formats.
    // TODO: Reduce the number of open() calls here. We could use one fd.
    Level level;
    if (!LoadDDSTexture(level, filename_iter->second.path, mip_level))
    {
      File::IOFile file;
      file.Open(filename_iter->second.path, "rb");
      std::vector<u8> buffer(file.GetSize());
      file.ReadBytes(buffer.data(), file.GetSize());

      if (!LoadTexture(level, buffer))
      {
        ERROR_LOG_FMT(VIDEO, "Custom texture {} failed to load", filename);
        break;
      }
    }

    ret->m_levels.push_back(std::move(level));
  }

  // If we failed to load any mip levels, we can't use this texture at all.
  if (ret->m_levels.empty())
    return nullptr;

  // Verify that the aspect ratio of the texture hasn't changed, as this could have side-effects.
  const Level& first_mip = ret->m_levels[0];
  if (first_mip.width * height != first_mip.height * width)
  {
    ERROR_LOG_FMT(VIDEO,
                  "Invalid custom texture size {}x{} for texture {}. The aspect differs "
                  "from the native size {}x{}.",
                  first_mip.width, first_mip.height, first_mip_file.path, width, height);
  }

  // Same deal if the custom texture isn't a multiple of the native size.
  if (width != 0 && height != 0 && (first_mip.width % width || first_mip.height % height))
  {
    ERROR_LOG_FMT(VIDEO,
                  "Invalid custom texture size {}x{} for texture {}. Please use an integer "
                  "upscaling factor based on the native size {}x{}.",
                  first_mip.width, first_mip.height, first_mip_file.path, width, height);
  }

  // Verify that each mip level is the correct size (divide by 2 each time).
  u32 current_mip_width = first_mip.width;
  u32 current_mip_height = first_mip.height;
  for (u32 mip_level = 1; mip_level < static_cast<u32>(ret->m_levels.size()); mip_level++)
  {
    if (current_mip_width != 1 || current_mip_height != 1)
    {
      current_mip_width = std::max(current_mip_width / 2, 1u);
      current_mip_height = std::max(current_mip_height / 2, 1u);

      const Level& level = ret->m_levels[mip_level];
      if (current_mip_width == level.width && current_mip_height == level.height)
        continue;

      ERROR_LOG_FMT(
          VIDEO, "Invalid custom texture size {}x{} for texture {}. Mipmap level {} must be {}x{}.",
          level.width, level.height, first_mip_file.path, mip_level, current_mip_width,
          current_mip_height);
    }
    else
    {
      // It is invalid to have more than a single 1x1 mipmap.
      ERROR_LOG_FMT(VIDEO, "Custom texture {} has too many 1x1 mipmaps. Skipping extra levels.",
                    first_mip_file.path);
    }

    // Drop this mip level and any others after it.
    while (ret->m_levels.size() > mip_level)
      ret->m_levels.pop_back();
  }

  // All levels have to have the same format.
  if (std::any_of(ret->m_levels.begin(), ret->m_levels.end(),
                  [&ret](const Level& l) { return l.format != ret->m_levels[0].format; }))
  {
    ERROR_LOG_FMT(VIDEO, "Custom texture {} has inconsistent formats across mip levels.",
                  first_mip_file.path);

    return nullptr;
  }

  return ret;
}

bool HiresTexture::LoadTexture(Level& level, const std::vector<u8>& buffer)
{
  if (!Common::LoadPNG(buffer, &level.data, &level.width, &level.height))
    return false;

  if (level.data.empty())
    return false;

  // Loaded PNG images are converted to RGBA.
  level.format = AbstractTextureFormat::RGBA8;
  level.row_length = level.width;
  return true;
}

std::set<std::string> GetTextureDirectoriesWithGameId(const std::string& root_directory,
                                                      const std::string& game_id)
{
  std::set<std::string> result;
  const std::string texture_directory = root_directory + game_id;

  if (File::Exists(texture_directory))
  {
    result.insert(texture_directory);
  }
  else
  {
    // If there's no directory with the region-specific ID, look for a 3-character region-free one
    const std::string region_free_directory = root_directory + game_id.substr(0, 3);

    if (File::Exists(region_free_directory))
    {
      result.insert(region_free_directory);
    }
  }

  const auto match_gameid = [game_id](const std::string& filename) {
    std::string basename;
    SplitPath(filename, nullptr, &basename, nullptr);
    return basename == game_id || basename == game_id.substr(0, 3);
  };

  // Look for any other directories that might be specific to the given gameid
  const auto files = Common::DoFileSearch({root_directory}, {".txt"}, true);
  for (const auto& file : files)
  {
    if (match_gameid(file))
    {
      // The following code is used to calculate the top directory
      // of a found gameid.txt file
      // ex:  <root directory>/My folder/gameids/<gameid>.txt
      // would insert "<root directory>/My folder"
      const auto directory_path = file.substr(root_directory.size());
      const std::size_t first_path_separator_position = directory_path.find_first_of(DIR_SEP_CHR);
      result.insert(root_directory + directory_path.substr(0, first_path_separator_position));
    }
  }

  return result;
}

HiresTexture::~HiresTexture()
{
}

AbstractTextureFormat HiresTexture::GetFormat() const
{
  return m_levels.at(0).format;
}

bool HiresTexture::HasArbitraryMipmaps() const
{
  return m_has_arbitrary_mipmaps;
}

//** - Hacks for RE3 - **
static std::unordered_map<std::string, HiresTexture::ReThreeMaskHackTlutToId> reThreeHackTexMap;
HiresTexture::ReThreeMaskHackTlutToId* tlutToId = nullptr;

void HiresTexture::ReThreeMaskHack(const std::string& texname, std::string& tlutname, u32 width)
{
  switch (width)
  {
  case 320:
  case 640:
    if (reThreeHackTexMap.find(texname) != reThreeHackTexMap.end())
    {
      tlutToId = &reThreeHackTexMap.at(texname);
    }
    break;

  case 256:
    if (tlutToId != nullptr)
    {
      if (tlutToId->tlut == tlutname)
      {
        tlutname = tlutToId->id;
        tlutToId = nullptr;
      }
      else if (tlutToId->tlut_alt != "" && tlutToId->tlut_alt == tlutname)
      {
        tlutname = tlutToId->id;
        tlutToId = nullptr;
      }
    }
    break;
  }
}

void HiresTexture::ReThreeMaskHackInit()
{
  reThreeHackTexMap.clear();

  //                  BG Tex name hash  New Mask TLUT hash   Mask TLUT hash       
  reThreeHackTexMap["_20c67ecf1252aacb"] = {"_R11B01", "_e3c364c1425f893c"};
  reThreeHackTexMap["_54cfa79672366bd7"] = {"_R11B0A", "_e3c364c1425f893c"};

  reThreeHackTexMap["_c492e7939b95fdf2"] = {"_R21801", "_91fbb229c7fa0f59", "_338ef6c05709e506"};
  reThreeHackTexMap["_9b12ad33a0f7ad05"] = {"_R21807", "_91fbb229c7fa0f59", "_338ef6c05709e506"};

  reThreeHackTexMap["_61d5ab40c32e722f"] = {"_R40F07", "_35ad92fce547a1d0"};
  reThreeHackTexMap["_55d89429aa7e4838"] = {"_R40F09", "_35ad92fce547a1d0"};
}

static std::unordered_map<std::string, std::string> reThreeRoomOsdMap;

/*void HiresTexture::ReThreeRoomIdOsd(const std::string& fullname, u32 width)
{
  if (width == 320 || width == 640)
  {
    if (reThreeRoomOsdMap.find(fullname) != reThreeRoomOsdMap.end())
      OSD::AddMessage(reThreeRoomOsdMap.at(fullname), 5000, 4294901760);
  }
}*/

/*void HiresTexture::ReThreeRoomIdOsdInit()
{
  reThreeRoomOsdMap.clear();
  reThreeRoomOsdMap["tex1_640x480_9909f423a4da08d4_5"] = "R10000 ";
  reThreeRoomOsdMap["tex1_640x480_64bba5a5049b774d_5"] = "R10001 ";
  reThreeRoomOsdMap["tex1_640x480_6da42a8533f841c9_5"] = "R10002 ";
  reThreeRoomOsdMap["tex1_640x480_3315c4e005a15392_5"] = "R10100 ";
  reThreeRoomOsdMap["tex1_640x480_6bb498b28a174f3f_5"] = "R10101 ";
  reThreeRoomOsdMap["tex1_640x480_c75ee80054155e52_5"] = "R10102 ";
  reThreeRoomOsdMap["tex1_640x480_7442fb3ec822e14e_5"] = "R10103 ";
  reThreeRoomOsdMap["tex1_640x480_f74c616b31955c69_5"] = "R10104 ";
  reThreeRoomOsdMap["tex1_640x480_3000accad6a06508_5"] = "R10105 ";
  reThreeRoomOsdMap["tex1_640x480_67889480325b6265_5"] = "R10106 ";
  reThreeRoomOsdMap["tex1_640x480_b767cf467e26a0bf_5"] = "R10107 ";
  reThreeRoomOsdMap["tex1_640x480_e59f1a22d7ab3c62_5"] = "R10108 ";
  reThreeRoomOsdMap["tex1_640x480_a164ac9b0777e475_5"] = "R10109 ";
  reThreeRoomOsdMap["tex1_640x480_db964111a2771ae4_5"] = "R1010A ";
  reThreeRoomOsdMap["tex1_640x480_410866ab3f4b5b2c_5"] = "R1010B ";
  reThreeRoomOsdMap["tex1_640x480_241836ae48e9a5eb_5"] = "R1010C ";
  reThreeRoomOsdMap["tex1_640x480_c9168553009476c1_5"] = "R1010D ";
  reThreeRoomOsdMap["tex1_640x480_3f8f2acbd343e91a_5"] = "R10111 ";
  reThreeRoomOsdMap["tex1_640x480_dd383f36c7ce02ea_5"] = "R10113 ";
  reThreeRoomOsdMap["tex1_640x480_3a0fc310805132ad_5"] = "R10114 ";
  reThreeRoomOsdMap["tex1_640x480_331da3abb5e205d8_5"] = "R10115 ";
  reThreeRoomOsdMap["tex1_640x480_49857296f7cf2036_5"] = "R10116 ";
  reThreeRoomOsdMap["tex1_640x480_58e8adb9768d7b09_5"] = "R10118 ";
  reThreeRoomOsdMap["tex1_640x480_cd243aafc54a85bb_5"] = "R10119 ";
  reThreeRoomOsdMap["tex1_640x480_0c9cdbba10c3fb2c_5"] = "R1011A ";
  reThreeRoomOsdMap["tex1_640x480_0978c15dc3879899_5"] = "R1011B ";
  reThreeRoomOsdMap["tex1_640x480_e28b1e22b277a9c2_5"] = "R10200 ";
  reThreeRoomOsdMap["tex1_640x480_1065483f893b65a3_5"] = "R10201 ";
  reThreeRoomOsdMap["tex1_640x480_257ea52e914f34b6_5"] = "R10202 ";
  reThreeRoomOsdMap["tex1_640x480_9163bd845a2d5e54_5"] = "R10203 ";
  reThreeRoomOsdMap["tex1_640x480_92ca946801d76814_5"] = "R10204 ";
  reThreeRoomOsdMap["tex1_640x480_af56d9ab2b2df677_5"] = "R10300 ";
  reThreeRoomOsdMap["tex1_640x480_856a964592fa5589_5"] = "R10301 ";
  reThreeRoomOsdMap["tex1_640x480_3212989993014cf3_5"] = "R10302 ";
  reThreeRoomOsdMap["tex1_640x480_fb445796a331243c_5"] = "R10303 ";
  reThreeRoomOsdMap["tex1_640x480_d53166a01dbcf78b_5"] = "R10304 ";
  reThreeRoomOsdMap["tex1_640x480_84ac6b6eb829fab0_5"] = "R10305 ";
  reThreeRoomOsdMap["tex1_640x480_bf0164455cfc5297_5"] = "R10306 ";
  reThreeRoomOsdMap["tex1_640x480_444c9538dec18396_5"] = "R10307 ";
  reThreeRoomOsdMap["tex1_640x480_277b40cc499c714a_5"] = "R10308 ";
  reThreeRoomOsdMap["tex1_640x480_7b1ff6261fde4475_5"] = "R10309 ";
  reThreeRoomOsdMap["tex1_640x480_5fcc07387cf4f195_5"] = "R1030A ";
  reThreeRoomOsdMap["tex1_640x480_5ec19c168e3a4091_5"] = "R1030B ";
  reThreeRoomOsdMap["tex1_640x480_5257eaef781d262f_5"] = "R1030C ";
  reThreeRoomOsdMap["tex1_640x480_c7e69ce612b82685_5"] = "R1030D ";
  reThreeRoomOsdMap["tex1_640x480_3f86a87831c81f83_5"] = "R1030E ";
  reThreeRoomOsdMap["tex1_640x480_b746711c421151b2_5"] = "R10400 ";
  reThreeRoomOsdMap["tex1_640x480_b789a56d75f08e92_5"] = "R10401 ";
  reThreeRoomOsdMap["tex1_640x480_e42e74a9de7c596d_5"] = "R10402 ";
  reThreeRoomOsdMap["tex1_640x480_1a10660b072d80ba_5"] = "R10403 ";
  reThreeRoomOsdMap["tex1_640x480_190a3245d251e04b_5"] = "R10404 ";
  reThreeRoomOsdMap["tex1_640x480_4369adbd46448bdc_5"] = "R10405 ";
  reThreeRoomOsdMap["tex1_640x480_6500f59c146e5ed8_5"] = "R10406 ";
  reThreeRoomOsdMap["tex1_640x480_5b2350ad1a878498_5"] = "R10407 ";
  reThreeRoomOsdMap["tex1_640x480_e850cbbde918ff36_5"] = "R10408 ";
  reThreeRoomOsdMap["tex1_640x480_877a6ee607affe77_5"] = "R10409 ";
  reThreeRoomOsdMap["tex1_640x480_18b877e64123ff78_5"] = "R1040A ";
  reThreeRoomOsdMap["tex1_640x480_028da97c099f0830_5"] = "R1040B ";
  reThreeRoomOsdMap["tex1_640x480_e3d643d513380164_5"] = "R1040C ";
  reThreeRoomOsdMap["tex1_640x480_02a684e7eb68a409_5"] = "R1040D ";
  reThreeRoomOsdMap["tex1_640x480_66530ff631164e6b_5"] = "R1040E R11F0E ";
  reThreeRoomOsdMap["tex1_640x480_d184d29701f48a79_5"] = "R1040F ";
  reThreeRoomOsdMap["tex1_640x480_96b38a1413566a7e_5"] = "R10500 ";
  reThreeRoomOsdMap["tex1_640x480_26051770f6d8f613_5"] = "R10501 ";
  reThreeRoomOsdMap["tex1_640x480_5b8e335f5096ec24_5"] = "R10502 ";
  reThreeRoomOsdMap["tex1_640x480_f4e0972da8d91771_5"] = "R10503 ";
  reThreeRoomOsdMap["tex1_640x480_3384bdfebcf1cea5_5"] = "R10504 ";
  reThreeRoomOsdMap["tex1_640x480_a7d74cb84d9a3303_5"] = "R10505 ";
  reThreeRoomOsdMap["tex1_640x480_963c292ce1d9c9f3_5"] = "R10506 ";
  reThreeRoomOsdMap["tex1_640x480_c9e7a9acbe6334e4_5"] = "R10507 ";
  reThreeRoomOsdMap["tex1_640x480_590826844139bf24_5"] = "R10508 ";
  reThreeRoomOsdMap["tex1_640x480_9af95c9c146ac800_5"] = "R10509 ";
  reThreeRoomOsdMap["tex1_640x480_36cda0685b4b4ec1_5"] = "R1050A ";
  reThreeRoomOsdMap["tex1_640x480_ce6678559cdc7f42_5"] = "R1050B ";
  reThreeRoomOsdMap["tex1_640x480_637e0be28896d7f4_5"] = "R1050C ";
  reThreeRoomOsdMap["tex1_640x480_74bfc42e4c6e4389_5"] = "R1050D ";
  reThreeRoomOsdMap["tex1_640x480_9fc4515958f2c0f6_5"] = "R1050E ";
  reThreeRoomOsdMap["tex1_640x480_ed414ed97620e7a5_5"] = "R1050F ";
  reThreeRoomOsdMap["tex1_640x480_c4b653af7ee66bc2_5"] = "R10600 ";
  reThreeRoomOsdMap["tex1_640x480_a4839eb7bbed872a_5"] = "R10601 ";
  reThreeRoomOsdMap["tex1_640x480_55b9a735f585048c_5"] = "R10602 ";
  reThreeRoomOsdMap["tex1_640x480_c2f5052dcbc330b7_5"] = "R10603 ";
  reThreeRoomOsdMap["tex1_640x480_6c81d435714ee15e_5"] = "R10604 ";
  reThreeRoomOsdMap["tex1_640x480_0ea94fb8d6fc2235_5"] = "R10605 ";
  reThreeRoomOsdMap["tex1_640x480_d2e51ecf0a7168d4_5"] = "R10606 ";
  reThreeRoomOsdMap["tex1_640x480_b058d564fd75da72_5"] = "R10607 ";
  reThreeRoomOsdMap["tex1_640x480_7ebdc4e71641b0ab_5"] = "R10608 ";
  reThreeRoomOsdMap["tex1_640x480_afa3b5b6035353c4_5"] = "R10609 ";
  reThreeRoomOsdMap["tex1_640x480_650f9b1e57d27e8a_5"] = "R1060A ";
  reThreeRoomOsdMap["tex1_640x480_1f3a0a6599a72ecb_5"] = "R1060B ";
  reThreeRoomOsdMap["tex1_640x480_b5b5c2594936dd2e_5"] = "R1060C ";
  reThreeRoomOsdMap["tex1_640x480_ac17b064be87f70d_5"] = "R1060E ";
  reThreeRoomOsdMap["tex1_320x240_104e580a36619be9_5"] = "R10700 ";
  reThreeRoomOsdMap["tex1_320x240_d73a7bf3996fcba9_5"] = "R10701 ";
  reThreeRoomOsdMap["tex1_320x240_e78eaab37c8772d3_5"] = "R10702 ";
  reThreeRoomOsdMap["tex1_320x240_24d19767b654f2de_5"] = "R10703 ";
  reThreeRoomOsdMap["tex1_320x240_83c2df6f26fbf8c3_5"] = "R10704 ";
  reThreeRoomOsdMap["tex1_640x480_803dbcfcf02f3a86_5"] = "R10705 ";
  reThreeRoomOsdMap["tex1_640x480_89667c61a8fe15a2_5"] = "R10706 ";
  reThreeRoomOsdMap["tex1_640x480_197186fda7042502_5"] = "R10707 ";
  reThreeRoomOsdMap["tex1_640x480_a988b336639f4775_5"] = "R10708 ";
  reThreeRoomOsdMap["tex1_640x480_09009c7f47763bd6_5"] = "R10709 ";
  reThreeRoomOsdMap["tex1_640x480_dec6c0631177d5e5_5"] = "R10800 ";
  reThreeRoomOsdMap["tex1_640x480_8c30432685555a0f_5"] = "R10801 ";
  reThreeRoomOsdMap["tex1_640x480_3bf55bb20871dd87_5"] = "R10802 ";
  reThreeRoomOsdMap["tex1_640x480_f2317fc16e9601d5_5"] = "R10803 ";
  reThreeRoomOsdMap["tex1_640x480_cc877c67a8fdc279_5"] = "R10804 ";
  reThreeRoomOsdMap["tex1_640x480_fba1b5709304c4bc_5"] = "R10805 ";
  reThreeRoomOsdMap["tex1_640x480_d5f94da425f10e5e_5"] = "R10806 ";
  reThreeRoomOsdMap["tex1_640x480_70eabb81771561ba_5"] = "R10807 ";
  reThreeRoomOsdMap["tex1_640x480_88321c8fb46533f3_5"] = "R10900 ";
  reThreeRoomOsdMap["tex1_640x480_99e9ab20b9494bf5_5"] = "R10901 ";
  reThreeRoomOsdMap["tex1_640x480_d20b20d1f133e999_5"] = "R10902 ";
  reThreeRoomOsdMap["tex1_640x480_919e9a328184031d_5"] = "R10903 ";
  reThreeRoomOsdMap["tex1_640x480_c3c90a86fecc5b25_5"] = "R10904 ";
  reThreeRoomOsdMap["tex1_640x480_fae3aa73e4cd117e_5"] = "R10905 ";
  reThreeRoomOsdMap["tex1_640x480_542ad35c41748d8a_5"] = "R10908 R12308 ";
  reThreeRoomOsdMap["tex1_640x480_73b31c238d56594f_5"] = "R10A00 ";
  reThreeRoomOsdMap["tex1_640x480_2d62cbaffd60a52e_5"] = "R10A01 ";
  reThreeRoomOsdMap["tex1_640x480_4c1df9f1c2799909_5"] = "R10A02 ";
  reThreeRoomOsdMap["tex1_640x480_ae2a282c483052c5_5"] = "R10A03 ";
  reThreeRoomOsdMap["tex1_640x480_df3a76cd46793da1_5"] = "R10A04 ";
  reThreeRoomOsdMap["tex1_640x480_d62e0ecde7e65625_5"] = "R10A05 ";
  reThreeRoomOsdMap["tex1_640x480_d4b59ca23bd597aa_5"] = "R10A06 ";
  reThreeRoomOsdMap["tex1_640x480_c567fb329345e2ba_5"] = "R10A07 ";
  reThreeRoomOsdMap["tex1_640x480_51c7d7600a6d3fa5_5"] = "R10A08 ";
  reThreeRoomOsdMap["tex1_640x480_99743ac82c526475_5"] = "R10B00 ";
  reThreeRoomOsdMap["tex1_640x480_df5bfb4d388b6615_5"] = "R10B01 ";
  reThreeRoomOsdMap["tex1_640x480_e89e18c7e3bedf0d_5"] = "R10B02 ";
  reThreeRoomOsdMap["tex1_640x480_1a5a696fda081309_5"] = "R10B03 ";
  reThreeRoomOsdMap["tex1_640x480_bcd1a220efeccbab_5"] = "R10B04 ";
  reThreeRoomOsdMap["tex1_640x480_841edaa0a0a5f0c2_5"] = "R10B05 ";
  reThreeRoomOsdMap["tex1_640x480_2fa0712b5ef9c41d_5"] = "R10C00 ";
  reThreeRoomOsdMap["tex1_640x480_ad1de473ead02d9d_5"] = "R10D00 ";
  reThreeRoomOsdMap["tex1_640x480_28a19251b775a24c_5"] = "R10D01 ";
  reThreeRoomOsdMap["tex1_640x480_7259175add4d33b3_5"] = "R10D02 ";
  reThreeRoomOsdMap["tex1_640x480_b054ee5dc0705561_5"] = "R10D03 ";
  reThreeRoomOsdMap["tex1_640x480_94d49a8054fd1fcd_5"] = "R10D04 ";
  reThreeRoomOsdMap["tex1_640x480_ac4086caf035fef4_5"] = "R10D05 ";
  reThreeRoomOsdMap["tex1_640x480_8ad09b6686605955_5"] = "R10D06 ";
  reThreeRoomOsdMap["tex1_640x480_1c2377549c4d0785_5"] = "R10D07 ";
  reThreeRoomOsdMap["tex1_640x480_4e5095f1f551c839_5"] = "R10D08 ";
  reThreeRoomOsdMap["tex1_640x480_28c1ff5e9554e528_5"] = "R10D09 ";
  reThreeRoomOsdMap["tex1_640x480_b5c44d4b7cbcd7e1_5"] = "R10D0A ";
  reThreeRoomOsdMap["tex1_640x480_68cac99e28b78141_5"] = "R10D0B ";
  reThreeRoomOsdMap["tex1_640x480_96f35427f81da910_5"] = "R10D0C ";
  reThreeRoomOsdMap["tex1_640x480_c1b9f04a2971ddad_5"] = "R10E00 ";
  reThreeRoomOsdMap["tex1_640x480_8e3e20a594429e61_5"] = "R10E01 ";
  reThreeRoomOsdMap["tex1_640x480_fac94a0ffb46743b_5"] = "R10E02 ";
  reThreeRoomOsdMap["tex1_640x480_3e8e5662cf6988db_5"] = "R10E03 ";
  reThreeRoomOsdMap["tex1_640x480_89b471255702ced8_5"] = "R10E04 ";
  reThreeRoomOsdMap["tex1_640x480_8be65911371cac7c_5"] = "R10F00 ";
  reThreeRoomOsdMap["tex1_640x480_ddb3710f954f70b7_5"] = "R10F01 ";
  reThreeRoomOsdMap["tex1_640x480_060294efbd82e02a_5"] = "R10F02 ";
  reThreeRoomOsdMap["tex1_640x480_f3e939286e9b5c8c_5"] = "R10F03 ";
  reThreeRoomOsdMap["tex1_640x480_b8aa1110f71771db_5"] = "R10F04 ";
  reThreeRoomOsdMap["tex1_640x480_ae393b232a33ae89_5"] = "R10F05 ";
  reThreeRoomOsdMap["tex1_640x480_550ef7f04c17d6ee_5"] = "R10F06 ";
  reThreeRoomOsdMap["tex1_640x480_c617f4a449368965_5"] = "R10F07 ";
  reThreeRoomOsdMap["tex1_640x480_23875deec95bca44_5"] = "R10F08 ";
  reThreeRoomOsdMap["tex1_640x480_a666c51298874626_5"] = "R11000 ";
  reThreeRoomOsdMap["tex1_640x480_b7fefb01fe6625f3_5"] = "R11001 ";
  reThreeRoomOsdMap["tex1_640x480_9437320ea51bc2fb_5"] = "R11002 ";
  reThreeRoomOsdMap["tex1_640x480_74cd99cf0f320816_5"] = "R11003 ";
  reThreeRoomOsdMap["tex1_640x480_2d5b879e922db637_5"] = "R11004 ";
  reThreeRoomOsdMap["tex1_640x480_2331fbe27fdd1d1a_5"] = "R11005 ";
  reThreeRoomOsdMap["tex1_640x480_65106c85493d2941_5"] = "R11007 ";
  reThreeRoomOsdMap["tex1_640x480_3973720341844e08_5"] = "R11100 ";
  reThreeRoomOsdMap["tex1_640x480_a237c34e792b7cd8_5"] = "R11101 ";
  reThreeRoomOsdMap["tex1_640x480_db99a1c44510abf1_5"] = "R11102 ";
  reThreeRoomOsdMap["tex1_640x480_4313fb017933a823_5"] = "R11103 ";
  reThreeRoomOsdMap["tex1_640x480_83cc200849d68c59_5"] = "R11104 ";
  reThreeRoomOsdMap["tex1_640x480_21dbe9f7cda3d2ac_5"] = "R11105 ";
  reThreeRoomOsdMap["tex1_640x480_a3e7c088334532f1_5"] = "R11106 ";
  reThreeRoomOsdMap["tex1_640x480_a777eb68b8a12cc6_5"] = "R11107 ";
  reThreeRoomOsdMap["tex1_640x480_8ef9e96f5888b99d_5"] = "R11108 ";
  reThreeRoomOsdMap["tex1_640x480_4252e0662b8d951c_5"] = "R11109 ";
  reThreeRoomOsdMap["tex1_640x480_ca1207e5d63fee60_5"] = "R1110A ";
  reThreeRoomOsdMap["tex1_640x480_d63746f025570050_5"] = "R1110F ";
  reThreeRoomOsdMap["tex1_640x480_d2144c5a1c1479f5_5"] = "R11110 ";
  reThreeRoomOsdMap["tex1_640x480_83f487acf296d3cc_5"] = "R11200 ";
  reThreeRoomOsdMap["tex1_640x480_0d7d2b480205a38f_5"] = "R11201 ";
  reThreeRoomOsdMap["tex1_640x480_17bc96f44a31975c_5"] = "R11202 ";
  reThreeRoomOsdMap["tex1_640x480_4634a1a4ef47fa50_5"] = "R11203 ";
  reThreeRoomOsdMap["tex1_640x480_eefab2d205314ed6_5"] = "R11204 ";
  reThreeRoomOsdMap["tex1_640x480_caf981362391df1c_5"] = "R11205 ";
  reThreeRoomOsdMap["tex1_640x480_f8c1f8678dc3e915_5"] = "R11206 ";
  reThreeRoomOsdMap["tex1_640x480_f0d147377cf7ddbd_5"] = "R11300 ";
  reThreeRoomOsdMap["tex1_640x480_5e578e6f3e7e1502_5"] = "R11301 ";
  reThreeRoomOsdMap["tex1_640x480_301bea646243a256_5"] = "R11303 ";
  reThreeRoomOsdMap["tex1_640x480_f1204894f5133038_5"] = "R11304 ";
  reThreeRoomOsdMap["tex1_640x480_67b3abd52ed2ce03_5"] = "R11305 ";
  reThreeRoomOsdMap["tex1_640x480_b2c65bf8a6187d37_5"] = "R11306 ";
  reThreeRoomOsdMap["tex1_640x480_a8ea7e6933af2ef4_5"] = "R11307 ";
  reThreeRoomOsdMap["tex1_640x480_0e02d1db2bac7a1e_5"] = "R11308 ";
  reThreeRoomOsdMap["tex1_640x480_c54c0c23131c491a_5"] = "R11400 ";
  reThreeRoomOsdMap["tex1_640x480_a94df9a2cfddf2f7_5"] = "R11401 ";
  reThreeRoomOsdMap["tex1_640x480_d3e305eac3ea1f09_5"] = "R11402 ";
  reThreeRoomOsdMap["tex1_640x480_02c6e97dda9953a1_5"] = "R11403 ";
  reThreeRoomOsdMap["tex1_640x480_65a0fe18bdec4a8e_5"] = "R11404 ";
  reThreeRoomOsdMap["tex1_640x480_8ed2c72b2b3cd263_5"] = "R11406 ";
  reThreeRoomOsdMap["tex1_640x480_dbba60a0dc67b917_5"] = "R11500 ";
  reThreeRoomOsdMap["tex1_640x480_683a6b589287ebba_5"] = "R11501 ";
  reThreeRoomOsdMap["tex1_640x480_c363ea408c92db8d_5"] = "R11502 ";
  reThreeRoomOsdMap["tex1_640x480_6dd553221a02b051_5"] = "R11503 ";
  reThreeRoomOsdMap["tex1_640x480_b9b756891766d65f_5"] = "R11504 ";
  reThreeRoomOsdMap["tex1_640x480_1f95f2ecd727b72d_5"] = "R11505 ";
  reThreeRoomOsdMap["tex1_640x480_1c1329d3e22d8c0f_5"] = "R11506 ";
  reThreeRoomOsdMap["tex1_640x480_2c62e95c1dd017ac_5"] = "R11507 ";
  reThreeRoomOsdMap["tex1_640x480_02a227be9a0e9758_5"] = "R11508 ";
  reThreeRoomOsdMap["tex1_640x480_f9c139d8bd522bec_5"] = "R11600 ";
  reThreeRoomOsdMap["tex1_640x480_37a2b814b641755d_5"] = "R11601 ";
  reThreeRoomOsdMap["tex1_640x480_2f525790fa6716fd_5"] = "R11602 ";
  reThreeRoomOsdMap["tex1_640x480_9b56dcb320356270_5"] = "R11603 ";
  reThreeRoomOsdMap["tex1_640x480_db6f7ebbf58a77dc_5"] = "R11604 ";
  reThreeRoomOsdMap["tex1_640x480_79f114cba2351214_5"] = "R11605 ";
  reThreeRoomOsdMap["tex1_640x480_b271fd14d947c80e_5"] = "R11606 ";
  reThreeRoomOsdMap["tex1_640x480_77840bcffb839765_5"] = "R11607 ";
  reThreeRoomOsdMap["tex1_640x480_b9d4d730db036474_5"] = "R11609 ";
  reThreeRoomOsdMap["tex1_640x480_ecab04c3083af1d7_5"] = "R1160A ";
  reThreeRoomOsdMap["tex1_640x480_98fcfb3ec9a0cfe7_5"] = "R11700 ";
  reThreeRoomOsdMap["tex1_640x480_4c1f066af4a8840d_5"] = "R11701 ";
  reThreeRoomOsdMap["tex1_640x480_2b5a8720e609c82c_5"] = "R11702 ";
  reThreeRoomOsdMap["tex1_640x480_e35a0cc2a3b657e2_5"] = "R11703 ";
  reThreeRoomOsdMap["tex1_640x480_24dc7e678e4805ac_5"] = "R11704 ";
  reThreeRoomOsdMap["tex1_640x480_8234a94bc9c3f191_5"] = "R11800 ";
  reThreeRoomOsdMap["tex1_640x480_05e980e6ccae1bc5_5"] = "R11801 ";
  reThreeRoomOsdMap["tex1_640x480_71fa0186f93ec15d_5"] = "R11802 ";
  reThreeRoomOsdMap["tex1_640x480_4183fd9955132431_5"] = "R11803 ";
  reThreeRoomOsdMap["tex1_640x480_21a7ff638581fb9e_5"] = "R11805 ";
  reThreeRoomOsdMap["tex1_640x480_3c678f0b758c5cd7_5"] = "R11806 ";
  reThreeRoomOsdMap["tex1_640x480_d6efea462696c5df_5"] = "R11807 ";
  reThreeRoomOsdMap["tex1_640x480_cb0c06899333ae26_5"] = "R11808 ";
  reThreeRoomOsdMap["tex1_640x480_846bf00b306bb892_5"] = "R11900 ";
  reThreeRoomOsdMap["tex1_640x480_2b35c5d5495af0c4_5"] = "R11901 ";
  reThreeRoomOsdMap["tex1_640x480_28c7e56a5e4bfd8e_5"] = "R11902 ";
  reThreeRoomOsdMap["tex1_640x480_bcdf9c7081c1e45b_5"] = "R11903 ";
  reThreeRoomOsdMap["tex1_640x480_b6ff24f89220e34b_5"] = "R11904 ";
  reThreeRoomOsdMap["tex1_640x480_287b69d3f414ab6d_5"] = "R11905 ";
  reThreeRoomOsdMap["tex1_640x480_7b4a025e8af50d78_5"] = "R11A00 ";
  reThreeRoomOsdMap["tex1_640x480_f41462f0c284e094_5"] = "R11A01 ";
  reThreeRoomOsdMap["tex1_640x480_e92859b895038025_5"] = "R11A02 ";
  reThreeRoomOsdMap["tex1_640x480_225910845404947a_5"] = "R11A03 ";
  reThreeRoomOsdMap["tex1_640x480_1e29d3256cc86124_5"] = "R11A04 ";
  reThreeRoomOsdMap["tex1_640x480_ddc22c61470ca4df_5"] = "R11A05 ";
  reThreeRoomOsdMap["tex1_640x480_4b7c0d7e61674c9e_5"] = "R11A08 ";
  reThreeRoomOsdMap["tex1_640x480_2614bb13a772795c_5"] = "R11A0A ";
  reThreeRoomOsdMap["tex1_640x480_f785712ccdfd8e7f_5"] = "R11A0B ";
  reThreeRoomOsdMap["tex1_640x480_191ab789448240ca_5"] = "R11A0D ";
  reThreeRoomOsdMap["tex1_640x480_54a10778c15f2c06_5"] = "R11B00 ";
  reThreeRoomOsdMap["tex1_640x480_20c67ecf1252aacb_5"] = "R11B01 ";
  reThreeRoomOsdMap["tex1_640x480_12930f1f0d9e6ee3_5"] = "R11B02 ";
  reThreeRoomOsdMap["tex1_640x480_5cdd439eee5508fe_5"] = "R11B03 ";
  reThreeRoomOsdMap["tex1_640x480_f4e1452e9dfbd12f_5"] = "R11B04 ";
  reThreeRoomOsdMap["tex1_640x480_5880445329d6ab08_5"] = "R11B05 ";
  reThreeRoomOsdMap["tex1_640x480_df1eaa6186239269_5"] = "R11B07 ";
  reThreeRoomOsdMap["tex1_640x480_b25fecbd211d94fc_5"] = "R11B08 ";
  reThreeRoomOsdMap["tex1_640x480_64ae087aa18ec887_5"] = "R11B09 ";
  reThreeRoomOsdMap["tex1_640x480_54cfa79672366bd7_5"] = "R11B0A ";
  reThreeRoomOsdMap["tex1_640x480_851c5e6c46ad61df_5"] = "R11B0B ";
  reThreeRoomOsdMap["tex1_640x480_022e6f1480592a5a_5"] = "R11B0C ";
  reThreeRoomOsdMap["tex1_640x480_e4c6fe6b1756a46c_5"] = "R11B0D ";
  reThreeRoomOsdMap["tex1_640x480_9770c0f72595d8ec_5"] = "R11B0E ";
  reThreeRoomOsdMap["tex1_640x480_a542d9f1cfe75d68_5"] = "R11B0F ";
  reThreeRoomOsdMap["tex1_640x480_919beb1e8f989bb4_5"] = "R11B10 ";
  reThreeRoomOsdMap["tex1_640x480_5317fd08e30fabd3_5"] = "R11B11 ";
  reThreeRoomOsdMap["tex1_640x480_922ec4d0b51aa6d5_5"] = "R11B12 ";
  reThreeRoomOsdMap["tex1_640x480_01ef32aa4a8263c4_5"] = "R11B13 ";
  reThreeRoomOsdMap["tex1_640x480_fe18279e46cdd568_5"] = "R11B14 ";
  reThreeRoomOsdMap["tex1_640x480_7b09d77698218595_5"] = "R11B15 ";
  reThreeRoomOsdMap["tex1_640x480_605e74e501423b79_5"] = "R11B16 ";
  reThreeRoomOsdMap["tex1_640x480_6217a1bbe978a314_5"] = "R11C00 ";
  reThreeRoomOsdMap["tex1_640x480_f613e00aab8c3d25_5"] = "R11C01 ";
  reThreeRoomOsdMap["tex1_640x480_74e7c940448a25b2_5"] = "R11C02 ";
  reThreeRoomOsdMap["tex1_640x480_de09dd6a402fec13_5"] = "R11C03 ";
  reThreeRoomOsdMap["tex1_640x480_788acaab0ae0c9b8_5"] = "R11C04 ";
  reThreeRoomOsdMap["tex1_640x480_ec00cca0b2657d3e_5"] = "R11C05 ";
  reThreeRoomOsdMap["tex1_640x480_6f0934374b086d43_5"] = "R11C06 ";
  reThreeRoomOsdMap["tex1_640x480_6e7ba454b8fd86d2_5"] = "R11D00 ";
  reThreeRoomOsdMap["tex1_640x480_61325d4538d0a7a0_5"] = "R11D01 ";
  reThreeRoomOsdMap["tex1_640x480_75260d45a2248644_5"] = "R11D02 ";
  reThreeRoomOsdMap["tex1_640x480_4b9683725bbaf135_5"] = "R11D03 ";
  reThreeRoomOsdMap["tex1_640x480_e50841ec32b1ad2e_5"] = "R11D04 ";
  reThreeRoomOsdMap["tex1_640x480_1df0d70a754f9141_5"] = "R11E00 ";
  reThreeRoomOsdMap["tex1_640x480_f64c596f799c4522_5"] = "R11E01 ";
  reThreeRoomOsdMap["tex1_640x480_91e4ba42b6201001_5"] = "R11E02 ";
  reThreeRoomOsdMap["tex1_640x480_97c4cc68f8eed0bd_5"] = "R11E03 ";
  reThreeRoomOsdMap["tex1_640x480_8e0b0c64abfe06f1_5"] = "R11E04 ";
  reThreeRoomOsdMap["tex1_640x480_efa96e47118bf1c8_5"] = "R11E05 ";
  reThreeRoomOsdMap["tex1_640x480_26a7a0700d82cf20_5"] = "R11E06 ";
  reThreeRoomOsdMap["tex1_640x480_beb9ea343bfc2928_5"] = "R11E07 ";
  reThreeRoomOsdMap["tex1_640x480_843ec906c5aa02d1_5"] = "R11E08 ";
  reThreeRoomOsdMap["tex1_640x480_6dbbbca5a253c81d_5"] = "R11E09 ";
  reThreeRoomOsdMap["tex1_640x480_3ceaa8437b971ede_5"] = "R11E0A ";
  reThreeRoomOsdMap["tex1_640x480_6052c17c49a2f54c_5"] = "R11E0B ";
  reThreeRoomOsdMap["tex1_640x480_1ab243f268f4479f_5"] = "R11E0C ";
  reThreeRoomOsdMap["tex1_640x480_e5906e342e3ece33_5"] = "R11E0D ";
  reThreeRoomOsdMap["tex1_640x480_448f09996eb29a35_5"] = "R11E0E ";
  reThreeRoomOsdMap["tex1_640x480_c4d3905ab82f469c_5"] = "R11F00 ";
  reThreeRoomOsdMap["tex1_640x480_afba5fe1ed87f190_5"] = "R11F01 ";
  reThreeRoomOsdMap["tex1_640x480_c01fe9b0a4ef8a39_5"] = "R11F02 ";
  reThreeRoomOsdMap["tex1_640x480_f0c77fe8378177f3_5"] = "R11F03 ";
  reThreeRoomOsdMap["tex1_640x480_3b96ee95ff4da1fd_5"] = "R11F04 ";
  reThreeRoomOsdMap["tex1_640x480_7ff601d00857a56f_5"] = "R11F05 ";
  reThreeRoomOsdMap["tex1_640x480_385e6c6e7ce93742_5"] = "R11F06 ";
  reThreeRoomOsdMap["tex1_640x480_6a920944f45e2ee4_5"] = "R11F07 ";
  reThreeRoomOsdMap["tex1_640x480_ff4db54179b2d3e7_5"] = "R11F08 ";
  reThreeRoomOsdMap["tex1_640x480_49361e32d3d629d2_5"] = "R11F09 ";
  reThreeRoomOsdMap["tex1_640x480_0774f47bd0cb9d4d_5"] = "R11F0A ";
  reThreeRoomOsdMap["tex1_640x480_e612179febdbf211_5"] = "R11F0B ";
  reThreeRoomOsdMap["tex1_640x480_3763647359d39286_5"] = "R12000 ";
  reThreeRoomOsdMap["tex1_640x480_3b9f4b54f6484ba7_5"] = "R12001 ";
  reThreeRoomOsdMap["tex1_640x480_197fbabc6fc11ac6_5"] = "R12002 ";
  reThreeRoomOsdMap["tex1_640x480_16c0d6c02c633f1f_5"] = "R12003 ";
  reThreeRoomOsdMap["tex1_640x480_caeb646fa07ef370_5"] = "R12004 ";
  reThreeRoomOsdMap["tex1_640x480_ebe224ad0f16be61_5"] = "R12005 ";
  reThreeRoomOsdMap["tex1_640x480_a8ac49a2511e2ddb_5"] = "R12006 ";
  reThreeRoomOsdMap["tex1_640x480_a1f4bdd01a71c947_5"] = "R12007 ";
  reThreeRoomOsdMap["tex1_640x480_fe41044848faf170_5"] = "R12008 ";
  reThreeRoomOsdMap["tex1_640x480_dc822a6667c6dfbc_5"] = "R12009 ";
  reThreeRoomOsdMap["tex1_640x480_0408c0e479d8a6bc_5"] = "R1200A ";
  reThreeRoomOsdMap["tex1_640x480_fc41ba7f96ca5602_5"] = "R1200B ";
  reThreeRoomOsdMap["tex1_640x480_d2efda8a38a8339a_5"] = "R1200C ";
  reThreeRoomOsdMap["tex1_640x480_968c729a2d88cc40_5"] = "R1200D ";
  reThreeRoomOsdMap["tex1_640x480_ccb3270c9a9308ea_5"] = "R1200E ";
  reThreeRoomOsdMap["tex1_640x480_caf4f18c14f98d0f_5"] = "R1200F ";
  reThreeRoomOsdMap["tex1_640x480_188079b229b36867_5"] = "R12010 ";
  reThreeRoomOsdMap["tex1_640x480_2c488006c934ece3_5"] = "R12100 ";
  reThreeRoomOsdMap["tex1_640x480_02d430e149ce9717_5"] = "R12101 ";
  reThreeRoomOsdMap["tex1_640x480_8b3844466891bfba_5"] = "R12102 ";
  reThreeRoomOsdMap["tex1_640x480_d162d9e3c9495387_5"] = "R12103 ";
  reThreeRoomOsdMap["tex1_640x480_9872347db44e7bdb_5"] = "R12104 ";
  reThreeRoomOsdMap["tex1_640x480_9ed46f2657f51249_5"] = "R12105 ";
  reThreeRoomOsdMap["tex1_640x480_e55740a880c8e495_5"] = "R12106 ";
  reThreeRoomOsdMap["tex1_640x480_46ce8a7f4c1a33b3_5"] = "R12107 ";
  reThreeRoomOsdMap["tex1_640x480_7a167fb878e1e6ee_5"] = "R12108 ";
  reThreeRoomOsdMap["tex1_640x480_aef7c25b39923e45_5"] = "R12109 ";
  reThreeRoomOsdMap["tex1_640x480_539203bd9da4a2db_5"] = "R1210A ";
  reThreeRoomOsdMap["tex1_640x480_05a94bd7beca3dff_5"] = "R1210B ";
  reThreeRoomOsdMap["tex1_640x480_667a359bdfe44b32_5"] = "R1210E ";
  reThreeRoomOsdMap["tex1_640x480_7fd9336f4f293b69_5"] = "R12200 ";
  reThreeRoomOsdMap["tex1_640x480_75a3c8297fced79a_5"] = "R12201 ";
  reThreeRoomOsdMap["tex1_640x480_18398c57754e2926_5"] = "R12202 ";
  reThreeRoomOsdMap["tex1_640x480_1a16c9aa43612e83_5"] = "R12203 ";
  reThreeRoomOsdMap["tex1_640x480_0db53128b8a4c403_5"] = "R12204 ";
  reThreeRoomOsdMap["tex1_640x480_bb6c5207f93072e4_5"] = "R12300 ";
  reThreeRoomOsdMap["tex1_640x480_e5b267f97ab4f214_5"] = "R12301 ";
  reThreeRoomOsdMap["tex1_640x480_4fe5353016544486_5"] = "R12302 ";
  reThreeRoomOsdMap["tex1_640x480_a3787e04fdd8c03a_5"] = "R12303 ";
  reThreeRoomOsdMap["tex1_640x480_b3dfae8612552527_5"] = "R12304 R1230F ";
  reThreeRoomOsdMap["tex1_640x480_613757ca09958f3c_5"] = "R12305 R12310 ";
  reThreeRoomOsdMap["tex1_640x480_dde43096570c9879_5"] = "R12309 ";
  reThreeRoomOsdMap["tex1_640x480_db771f0b56c35c1a_5"] = "R1230A ";
  reThreeRoomOsdMap["tex1_640x480_3e0aa763f500d805_5"] = "R1230B ";
  reThreeRoomOsdMap["tex1_640x480_6141f6c976d69899_5"] = "R1230C ";
  reThreeRoomOsdMap["tex1_640x480_6833a965e0ecada3_5"] = "R1230D ";
  reThreeRoomOsdMap["tex1_640x480_80d78e66b30fae18_5"] = "R1230E ";
  reThreeRoomOsdMap["tex1_640x480_458e3bb0a38392a4_5"] = "R12311 ";
  reThreeRoomOsdMap["tex1_640x480_e15436761392abba_5"] = "R12312 ";
  reThreeRoomOsdMap["tex1_640x480_ac741539ae6128ae_5"] = "R12313 ";
  reThreeRoomOsdMap["tex1_640x480_e9b573807d66cba8_5"] = "R12400 ";
  reThreeRoomOsdMap["tex1_640x480_eae4d96c8bef40a7_5"] = "R12401 ";
  reThreeRoomOsdMap["tex1_640x480_a977e71ef0d89dd5_5"] = "R12402 ";
  reThreeRoomOsdMap["tex1_640x480_4b2823f3d05bdfa5_5"] = "R12403 ";
  reThreeRoomOsdMap["tex1_640x480_9d0371d103f0b60a_5"] = "R12404 ";
  reThreeRoomOsdMap["tex1_640x480_49eef4d97f06f274_5"] = "R12405 ";
  reThreeRoomOsdMap["tex1_640x480_658b25133a413461_5"] = "R12406 ";
  reThreeRoomOsdMap["tex1_640x480_cf1b6c914aeabed9_5"] = "R12407 ";
  reThreeRoomOsdMap["tex1_640x480_e06ef918c5fc0329_5"] = "R12408 ";
  reThreeRoomOsdMap["tex1_640x480_f59a09e0f12fc1fd_5"] = "R12500 ";
  reThreeRoomOsdMap["tex1_640x480_f36847f2b9175de0_5"] = "R12501 ";
  reThreeRoomOsdMap["tex1_640x480_186d2a91da1b23ed_5"] = "R12502 ";
  reThreeRoomOsdMap["tex1_640x480_e494a7f498da6103_5"] = "R20000 ";
  reThreeRoomOsdMap["tex1_640x480_af952f0f9a3d5283_5"] = "R20001 ";
  reThreeRoomOsdMap["tex1_640x480_dd69b9c75dcebdb9_5"] = "R20002 ";
  reThreeRoomOsdMap["tex1_640x480_e6f0397c70cc4751_5"] = "R20003 ";
  reThreeRoomOsdMap["tex1_640x480_6b09ea10fe4fff0a_5"] = "R20004 ";
  reThreeRoomOsdMap["tex1_640x480_d08ed73abfe570d2_5"] = "R20005 ";
  reThreeRoomOsdMap["tex1_640x480_cd9d4b7d07872a04_5"] = "R20006 ";
  reThreeRoomOsdMap["tex1_640x480_c0b9afc962212b74_5"] = "R20007 ";
  reThreeRoomOsdMap["tex1_640x480_40bb6199213a2286_5"] = "R20008 ";
  reThreeRoomOsdMap["tex1_640x480_1afb1e0037a6143f_5"] = "R20009 ";
  reThreeRoomOsdMap["tex1_640x480_43870088ebdf0428_5"] = "R2000A ";
  reThreeRoomOsdMap["tex1_640x480_d450dadb8e8b45f2_5"] = "R2000B ";
  reThreeRoomOsdMap["tex1_640x480_cdb5a64a4a751886_5"] = "R20100 ";
  reThreeRoomOsdMap["tex1_640x480_ad318a6c0b22d123_5"] = "R20101 ";
  reThreeRoomOsdMap["tex1_640x480_e3777d4cc5a36eb2_5"] = "R20102 ";
  reThreeRoomOsdMap["tex1_640x480_e8264a41dfda8383_5"] = "R20103 ";
  reThreeRoomOsdMap["tex1_640x480_e08aef809ca60977_5"] = "R20104 ";
  reThreeRoomOsdMap["tex1_640x480_8b0c2c1293fe1012_5"] = "R20105 ";
  reThreeRoomOsdMap["tex1_640x480_8be5c936d577c7b8_5"] = "R20108 ";
  reThreeRoomOsdMap["tex1_640x480_28ddce01ddb51951_5"] = "R20109 ";
  reThreeRoomOsdMap["tex1_640x480_90f9ae5f673994dd_5"] = "R2010A ";
  reThreeRoomOsdMap["tex1_640x480_172e3c67af4f3db1_5"] = "R2010B ";
  reThreeRoomOsdMap["tex1_640x480_f8f532923229b8d0_5"] = "R2010C ";
  reThreeRoomOsdMap["tex1_640x480_5ba4b4c77657ee2b_5"] = "R2010D ";
  reThreeRoomOsdMap["tex1_640x480_aeadafb190c51f8a_5"] = "R2010E ";
  reThreeRoomOsdMap["tex1_640x480_ac7e744ad73e557c_5"] = "R2010F ";
  reThreeRoomOsdMap["tex1_640x480_c18a3f9415ae613a_5"] = "R20110 ";
  reThreeRoomOsdMap["tex1_640x480_b858cb4fee880969_5"] = "R20111 ";
  reThreeRoomOsdMap["tex1_640x480_0908772b61719b35_5"] = "R20112 ";
  reThreeRoomOsdMap["tex1_640x480_dc31fda63a789d07_5"] = "R20113 ";
  reThreeRoomOsdMap["tex1_640x480_d65c6f95c5fe6685_5"] = "R20114 ";
  reThreeRoomOsdMap["tex1_640x480_4d97d4fa776e978e_5"] = "R20115 ";
  reThreeRoomOsdMap["tex1_640x480_b64f3390f8c4b934_5"] = "R20116 ";
  reThreeRoomOsdMap["tex1_640x480_42b550b6210c70d1_5"] = "R20117 ";
  reThreeRoomOsdMap["tex1_640x480_177359a863bbbf3a_5"] = "R20118 ";
  reThreeRoomOsdMap["tex1_640x480_1aad42f8e2a187a7_5"] = "R20119 ";
  reThreeRoomOsdMap["tex1_640x480_80750aa726f906f6_5"] = "R2011B ";
  reThreeRoomOsdMap["tex1_640x480_fc1794dff4ea134a_5"] = "R20200 ";
  reThreeRoomOsdMap["tex1_640x480_3da065105c1137ef_5"] = "R20201 ";
  reThreeRoomOsdMap["tex1_640x480_6b48dc05bbd59854_5"] = "R20202 ";
  reThreeRoomOsdMap["tex1_640x480_df2f13a25759b5b2_5"] = "R20203 ";
  reThreeRoomOsdMap["tex1_640x480_8b5846e9a414815b_5"] = "R20204 ";
  reThreeRoomOsdMap["tex1_640x480_09d1f985a6ca388a_5"] = "R20300 ";
  reThreeRoomOsdMap["tex1_640x480_1d3739ee1c0d5d57_5"] = "R20301 ";
  reThreeRoomOsdMap["tex1_640x480_95728242f256955c_5"] = "R20302 ";
  reThreeRoomOsdMap["tex1_640x480_4ac92a99ba89b3a4_5"] = "R20303 ";
  reThreeRoomOsdMap["tex1_640x480_5150e771fa5b65a2_5"] = "R20304 ";
  reThreeRoomOsdMap["tex1_640x480_a3e958572626ec64_5"] = "R20305 ";
  reThreeRoomOsdMap["tex1_640x480_f519c123441049c2_5"] = "R20306 ";
  reThreeRoomOsdMap["tex1_640x480_a77fec8b2e621ea2_5"] = "R20307 ";
  reThreeRoomOsdMap["tex1_640x480_20a05673caf8ccac_5"] = "R20308 ";
  reThreeRoomOsdMap["tex1_640x480_240609c05a8af78a_5"] = "R20309 ";
  reThreeRoomOsdMap["tex1_640x480_0f2fdcd293f5593f_5"] = "R2030A ";
  reThreeRoomOsdMap["tex1_640x480_f2d223cab2132f73_5"] = "R2030B ";
  reThreeRoomOsdMap["tex1_640x480_4e5c5194dcc3b7fb_5"] = "R2030C ";
  reThreeRoomOsdMap["tex1_640x480_2571ec9ae3acc5b6_5"] = "R20400 ";
  reThreeRoomOsdMap["tex1_640x480_d4ee2f2a969e19f6_5"] = "R20401 ";
  reThreeRoomOsdMap["tex1_640x480_2918d1bb3532d668_5"] = "R20402 ";
  reThreeRoomOsdMap["tex1_640x480_d17c53e3bbb8ecb5_5"] = "R20403 ";
  reThreeRoomOsdMap["tex1_640x480_946490b43b952c69_5"] = "R20404 ";
  reThreeRoomOsdMap["tex1_640x480_a4ac920aecd74432_5"] = "R20405 ";
  reThreeRoomOsdMap["tex1_640x480_abf237e6ec9782b4_5"] = "R20406 ";
  reThreeRoomOsdMap["tex1_640x480_184b675f566c20d6_5"] = "R20407 ";
  reThreeRoomOsdMap["tex1_640x480_2c9209cba52fafdd_5"] = "R20408 ";
  reThreeRoomOsdMap["tex1_640x480_00bda56bac63f99f_5"] = "R20409 ";
  reThreeRoomOsdMap["tex1_640x480_40f864e40af3aab1_5"] = "R2040A ";
  reThreeRoomOsdMap["tex1_640x480_6c4ca023d1b0054b_5"] = "R2040B ";
  reThreeRoomOsdMap["tex1_640x480_daa918fe8b8a6a3e_5"] = "R20500 ";
  reThreeRoomOsdMap["tex1_640x480_85d319917b8f23f4_5"] = "R20501 ";
  reThreeRoomOsdMap["tex1_640x480_66c76fa23b3cf10e_5"] = "R20502 ";
  reThreeRoomOsdMap["tex1_640x480_d6efbbc108d35e4b_5"] = "R20503 ";
  reThreeRoomOsdMap["tex1_640x480_f0d875f6ceceecc9_5"] = "R20504 ";
  reThreeRoomOsdMap["tex1_640x480_f58bdf483e646ae4_5"] = "R20505 ";
  reThreeRoomOsdMap["tex1_640x480_01bfb99965da231d_5"] = "R20506 ";
  reThreeRoomOsdMap["tex1_640x480_f67150f65b458e90_5"] = "R20507 ";
  reThreeRoomOsdMap["tex1_640x480_fc3517516c09b629_5"] = "R20508 ";
  reThreeRoomOsdMap["tex1_640x480_fcf8b72d886cf2a5_5"] = "R20509 ";
  reThreeRoomOsdMap["tex1_640x480_d37aba28948ce189_5"] = "R2050A ";
  reThreeRoomOsdMap["tex1_640x480_106172b9a9a7e471_5"] = "R2050B ";
  reThreeRoomOsdMap["tex1_640x480_feef716ff8d2abb0_5"] = "R2050C ";
  reThreeRoomOsdMap["tex1_640x480_ec65756caa54f6d8_5"] = "R2050D ";
  reThreeRoomOsdMap["tex1_640x480_a82d50bd491a36a8_5"] = "R2050E ";
  reThreeRoomOsdMap["tex1_640x480_f45b2283bed109ef_5"] = "R2050F ";
  reThreeRoomOsdMap["tex1_640x480_307bdc54f814cef0_5"] = "R20510 ";
  reThreeRoomOsdMap["tex1_640x480_11b5bca1d86e1535_5"] = "R20600 ";
  reThreeRoomOsdMap["tex1_640x480_33d11666daceafcb_5"] = "R20601 ";
  reThreeRoomOsdMap["tex1_640x480_691c6e2c4f09bc6f_5"] = "R20602 ";
  reThreeRoomOsdMap["tex1_640x480_c7c3e2628f8fa1f8_5"] = "R20603 ";
  reThreeRoomOsdMap["tex1_640x480_29447c11feae93a1_5"] = "R20604 ";
  reThreeRoomOsdMap["tex1_640x480_1ca6fac39c347259_5"] = "R20605 ";
  reThreeRoomOsdMap["tex1_640x480_110f2ca968f912dc_5"] = "R20606 ";
  reThreeRoomOsdMap["tex1_640x480_bb20edb91c1f0c47_5"] = "R20607 ";
  reThreeRoomOsdMap["tex1_640x480_a5863d7eb8128712_5"] = "R20608 ";
  reThreeRoomOsdMap["tex1_640x480_45faed098aabb01a_5"] = "R20609 ";
  reThreeRoomOsdMap["tex1_640x480_5012fe9aecb7268a_5"] = "R2060A ";
  reThreeRoomOsdMap["tex1_640x480_4d7f5f5516cd7ac3_5"] = "R2060B ";
  reThreeRoomOsdMap["tex1_640x480_4c0c5cb2c6c05a66_5"] = "R2060C ";
  reThreeRoomOsdMap["tex1_640x480_2fe66d8d0d8646ae_5"] = "R2060D ";
  reThreeRoomOsdMap["tex1_640x480_3063f90859627432_5"] = "R2060E ";
  reThreeRoomOsdMap["tex1_640x480_9f2f8d7da92e12ad_5"] = "R20700 ";
  reThreeRoomOsdMap["tex1_640x480_e5347aa1b97b6470_5"] = "R20701 ";
  reThreeRoomOsdMap["tex1_640x480_bc6b06565227025d_5"] = "R20702 ";
  reThreeRoomOsdMap["tex1_640x480_459ab84d64ccbeba_5"] = "R20703 ";
  reThreeRoomOsdMap["tex1_640x480_34bbec9d8fa764d1_5"] = "R20704 ";
  reThreeRoomOsdMap["tex1_640x480_6407e2d351c07bf9_5"] = "R20705 ";
  reThreeRoomOsdMap["tex1_640x480_cdee587becd2a506_5"] = "R20706 ";
  reThreeRoomOsdMap["tex1_640x480_97333f94a345e593_5"] = "R20707 ";
  reThreeRoomOsdMap["tex1_640x480_aa3171e2e26f50ad_5"] = "R20708 ";
  reThreeRoomOsdMap["tex1_640x480_dcdfa1f6f4e9eb21_5"] = "R20709 ";
  reThreeRoomOsdMap["tex1_640x480_e86b576e1e526a21_5"] = "R2070A ";
  reThreeRoomOsdMap["tex1_640x480_397dc0a212ba2936_5"] = "R2070B ";
  reThreeRoomOsdMap["tex1_640x480_0e551400651c55f9_5"] = "R2070C ";
  reThreeRoomOsdMap["tex1_640x480_b2d41d401f1fc473_5"] = "R2070D ";
  reThreeRoomOsdMap["tex1_640x480_c23a1993f29e141d_5"] = "R2070E ";
  reThreeRoomOsdMap["tex1_640x480_32cefc25d5a9991c_5"] = "R2070F ";
  reThreeRoomOsdMap["tex1_640x480_b2ba622ec86b1ffd_5"] = "R20710 ";
  reThreeRoomOsdMap["tex1_640x480_f9a6ab0f1fb1c660_5"] = "R20711 ";
  reThreeRoomOsdMap["tex1_640x480_74145e7a24aecf44_5"] = "R20712 ";
  reThreeRoomOsdMap["tex1_640x480_133afee59bfeb24f_5"] = "R20713 ";
  reThreeRoomOsdMap["tex1_640x480_d5da36b966078f24_5"] = "R20714 ";
  reThreeRoomOsdMap["tex1_640x480_5c515c6592cb81bd_5"] = "R20716 ";
  reThreeRoomOsdMap["tex1_640x480_9cb9013465bca510_5"] = "R20717 ";
  reThreeRoomOsdMap["tex1_640x480_c9845bb9af7f9104_5"] = "R20718 ";
  reThreeRoomOsdMap["tex1_640x480_44b822741f3a647a_5"] = "R20800 ";
  reThreeRoomOsdMap["tex1_640x480_e7b69c68ab4efba8_5"] = "R20801 ";
  reThreeRoomOsdMap["tex1_640x480_bebd41233c94aab7_5"] = "R20802 ";
  reThreeRoomOsdMap["tex1_640x480_53ddc06f1012dbf6_5"] = "R20803 ";
  reThreeRoomOsdMap["tex1_640x480_9aa0c816d4141fcc_5"] = "R20804 ";
  reThreeRoomOsdMap["tex1_640x480_96c8a9c3443b2e06_5"] = "R20805 ";
  reThreeRoomOsdMap["tex1_640x480_055b4d716157d67c_5"] = "R20806 ";
  reThreeRoomOsdMap["tex1_640x480_4c84b39613e60b9e_5"] = "R20807 ";
  reThreeRoomOsdMap["tex1_640x480_97178fb56ac2391b_5"] = "R20808 ";
  reThreeRoomOsdMap["tex1_640x480_5d10c860a0fd2336_5"] = "R20809 ";
  reThreeRoomOsdMap["tex1_640x480_890d6d6cfc6ea8f3_5"] = "R2080D ";
  reThreeRoomOsdMap["tex1_640x480_723a184f70dbe874_5"] = "R2080E ";
  reThreeRoomOsdMap["tex1_640x480_d88bfa870d5eb76b_5"] = "R2080F ";
  reThreeRoomOsdMap["tex1_640x480_b2731d7d20b45e6f_5"] = "R20810 ";
  reThreeRoomOsdMap["tex1_640x480_44f1435ccfc02ee7_5"] = "R20900 ";
  reThreeRoomOsdMap["tex1_640x480_ad5679c7ed0089c4_5"] = "R20901 ";
  reThreeRoomOsdMap["tex1_640x480_338eca1896fa1a07_5"] = "R20902 ";
  reThreeRoomOsdMap["tex1_640x480_818e6b1619147cb4_5"] = "R20903 ";
  reThreeRoomOsdMap["tex1_640x480_61c54899c479d1a8_5"] = "R20904 ";
  reThreeRoomOsdMap["tex1_640x480_a1cdd48d7c9fd033_5"] = "R20A00 ";
  reThreeRoomOsdMap["tex1_640x480_d26130290026648d_5"] = "R20A01 ";
  reThreeRoomOsdMap["tex1_640x480_8a1ac7e841dbf1f2_5"] = "R20A02 ";
  reThreeRoomOsdMap["tex1_640x480_105ab4e47ccd6069_5"] = "R20A03 ";
  reThreeRoomOsdMap["tex1_640x480_be2210f93708eb6d_5"] = "R20A04 ";
  reThreeRoomOsdMap["tex1_640x480_98d8a681230c8c22_5"] = "R20A05 ";
  reThreeRoomOsdMap["tex1_640x480_2a33ecf9e185fe71_5"] = "R20A08 ";
  reThreeRoomOsdMap["tex1_640x480_52b26ace319193a1_5"] = "R20A09 ";
  reThreeRoomOsdMap["tex1_640x480_59232e349134702b_5"] = "R20A0A ";
  reThreeRoomOsdMap["tex1_640x480_428ee9130b313de9_5"] = "R20A0B ";
  reThreeRoomOsdMap["tex1_640x480_e1f2ec2cb8c34b45_5"] = "R20A0C ";
  reThreeRoomOsdMap["tex1_640x480_a75669f60babe044_5"] = "R20A0D ";
  reThreeRoomOsdMap["tex1_640x480_bca9e89a5f28b671_5"] = "R20A0E ";
  reThreeRoomOsdMap["tex1_640x480_3a81e55674c7f29b_5"] = "R20A0F ";
  reThreeRoomOsdMap["tex1_640x480_31e848ef273d089b_5"] = "R20A10 ";
  reThreeRoomOsdMap["tex1_640x480_7752ca3b5fb08bb7_5"] = "R20A11 ";
  reThreeRoomOsdMap["tex1_640x480_f8597690715f1ff1_5"] = "R20A12 ";
  reThreeRoomOsdMap["tex1_640x480_798d77e8fc0f6be0_5"] = "R20A13 ";
  reThreeRoomOsdMap["tex1_640x480_fa740f9cb280d80e_5"] = "R20A14 ";
  reThreeRoomOsdMap["tex1_640x480_daf040a784f262af_5"] = "R20A15 ";
  reThreeRoomOsdMap["tex1_640x480_8a7b7d1268fbd10a_5"] = "R20A16 ";
  reThreeRoomOsdMap["tex1_640x480_5cb8b87b509d0985_5"] = "R20A17 ";
  reThreeRoomOsdMap["tex1_640x480_a947bcc8fff7a355_5"] = "R20A18 ";
  reThreeRoomOsdMap["tex1_640x480_db044e44fede4a63_5"] = "R20B00 ";
  reThreeRoomOsdMap["tex1_640x480_df3e22d1e90706af_5"] = "R20B01 ";
  reThreeRoomOsdMap["tex1_640x480_3f03076c6c2bcbe8_5"] = "R20B02 ";
  reThreeRoomOsdMap["tex1_640x480_2c69795188ab2657_5"] = "R20B03 ";
  reThreeRoomOsdMap["tex1_640x480_7f0cac2ef5b87ebf_5"] = "R20B04 ";
  reThreeRoomOsdMap["tex1_640x480_7f5eaa3900ef66fa_5"] = "R20B05 ";
  reThreeRoomOsdMap["tex1_640x480_a71984a3790f3a96_5"] = "R20B06 ";
  reThreeRoomOsdMap["tex1_640x480_362836165e7eebcb_5"] = "R20B07 ";
  reThreeRoomOsdMap["tex1_640x480_0b90c0478a4725fd_5"] = "R20B08 ";
  reThreeRoomOsdMap["tex1_640x480_2a2b58d1c90b8281_5"] = "R20B09 ";
  reThreeRoomOsdMap["tex1_640x480_5a31dafb6aa84914_5"] = "R20B0A ";
  reThreeRoomOsdMap["tex1_640x480_54ea22289b054b9e_5"] = "R20B0C ";
  reThreeRoomOsdMap["tex1_640x480_4461b5941a960a09_5"] = "R20B0D ";
  reThreeRoomOsdMap["tex1_640x480_41d9d92d56e2bc05_5"] = "R20B0F ";
  reThreeRoomOsdMap["tex1_640x480_70461bdf17cda8e2_5"] = "R20B10 ";
  reThreeRoomOsdMap["tex1_640x480_13b3c02f4355544a_5"] = "R20C00 R21500 ";
  reThreeRoomOsdMap["tex1_640x480_14469b507553ef9c_5"] = "R20C01 R21501 ";
  reThreeRoomOsdMap["tex1_640x480_2242561d69744b7c_5"] = "R20C02 R21502 ";
  reThreeRoomOsdMap["tex1_640x480_e86fa0f8786f6681_5"] = "R20C03 R21503 ";
  reThreeRoomOsdMap["tex1_640x480_d2462588ac897a88_5"] = "R20C04 R21504 ";
  reThreeRoomOsdMap["tex1_640x480_e8dc9aabf2ab6bdf_5"] = "R20C05 R21505 ";
  reThreeRoomOsdMap["tex1_640x480_19efa02e6dae53b1_5"] = "R20C07 R21507 ";
  reThreeRoomOsdMap["tex1_640x480_3436c6d7cc16a936_5"] = "R20C0A R2150A ";
  reThreeRoomOsdMap["tex1_640x480_f818068777805850_5"] = "R20C0B R2150B ";
  reThreeRoomOsdMap["tex1_640x480_b22533d67cc10dde_5"] = "R20C0C R2150C ";
  reThreeRoomOsdMap["tex1_640x480_e8b4e0632a0ac6d1_5"] = "R20C0D R2150D ";
  reThreeRoomOsdMap["tex1_640x480_022e098a2092e6de_5"] = "R20C0E R2150E ";
  reThreeRoomOsdMap["tex1_640x480_02c3e55fdac0af66_5"] = "R20C10 R2Q510 ";
  reThreeRoomOsdMap["tex1_640x480_153b5a5e6f110bb2_5"] = "R20C11 R21511 ";
  reThreeRoomOsdMap["tex1_640x480_39cc784e02cf81f9_5"] = "R20C12 R21512 ";
  reThreeRoomOsdMap["tex1_640x480_57a1ae2c8232d1a3_5"] = "R20C13 R21513 ";
  reThreeRoomOsdMap["tex1_640x480_76f718126a1b22e9_5"] = "R20C14 R21514 ";
  reThreeRoomOsdMap["tex1_640x480_535b7cd72cd64dc1_5"] = "R20C15 R21515 ";
  reThreeRoomOsdMap["tex1_640x480_fde056ccd210b102_5"] = "R20C17 R21517 ";
  reThreeRoomOsdMap["tex1_640x480_af372f0fc2aba4de_5"] = "R20C18 R21518 ";
  reThreeRoomOsdMap["tex1_640x480_380ddafcc24ece64_5"] = "R20C19 R21519 ";
  reThreeRoomOsdMap["tex1_640x480_7fae233699509998_5"] = "R20C1A R2151A ";
  reThreeRoomOsdMap["tex1_640x480_8a626475d731de90_5"] = "R20C1B R2151B ";
  reThreeRoomOsdMap["tex1_640x480_9e2ba002464ad6db_5"] = "R20C1C R2151C ";
  reThreeRoomOsdMap["tex1_640x480_b8f5448acbb942ae_5"] = "R20C1D R2151D ";
  reThreeRoomOsdMap["tex1_640x480_1554d8eee91216cf_5"] = "R20C1E R2151E ";
  reThreeRoomOsdMap["tex1_640x480_d2f7bc3a77b67917_5"] = "R20D00 ";
  reThreeRoomOsdMap["tex1_640x480_f387b921d058074c_5"] = "R20D01 ";
  reThreeRoomOsdMap["tex1_640x480_99451d855f1d76e5_5"] = "R20D02 ";
  reThreeRoomOsdMap["tex1_640x480_92a69a4ab7ebdfda_5"] = "R20D03 ";
  reThreeRoomOsdMap["tex1_640x480_57806c695eaf1c2b_5"] = "R20D04 ";
  reThreeRoomOsdMap["tex1_640x480_012c8115e6cdf33c_5"] = "R20D05 ";
  reThreeRoomOsdMap["tex1_640x480_6610b1d15b45d6e1_5"] = "R20D07 ";
  reThreeRoomOsdMap["tex1_640x480_a631bc7c006e3c42_5"] = "R20D08 ";
  reThreeRoomOsdMap["tex1_640x480_1319f26c82dd1dea_5"] = "R20D09 ";
  reThreeRoomOsdMap["tex1_640x480_2fcfedf3368bdffa_5"] = "R20D0A ";
  reThreeRoomOsdMap["tex1_640x480_197534aac8dc01ba_5"] = "R20D0B ";
  reThreeRoomOsdMap["tex1_640x480_fe2659aa0e190ef6_5"] = "R20D0C ";
  reThreeRoomOsdMap["tex1_640x480_8f6f1104bce5dc9b_5"] = "R20D0E ";
  reThreeRoomOsdMap["tex1_640x480_4eda3641e95b9118_5"] = "R20D0F ";
  reThreeRoomOsdMap["tex1_640x480_09e33e33ae96e899_5"] = "R20D10 ";
  reThreeRoomOsdMap["tex1_640x480_0cb4d040d7f1aaf1_5"] = "R20D11 ";
  reThreeRoomOsdMap["tex1_640x480_2f8733bf741d4c72_5"] = "R20E00 ";
  reThreeRoomOsdMap["tex1_640x480_39ea93654eff53a0_5"] = "R20E02 ";
  reThreeRoomOsdMap["tex1_640x480_a1b8e5279a823da5_5"] = "R20E03 ";
  reThreeRoomOsdMap["tex1_640x480_f6d1af1e965ba9a1_5"] = "R20E04 ";
  reThreeRoomOsdMap["tex1_640x480_c2b66d1281486538_5"] = "R20E04 ";
  reThreeRoomOsdMap["tex1_640x480_599308eabef1b5ac_5"] = "R20E06 ";
  reThreeRoomOsdMap["tex1_640x480_faa413acd523ac06_5"] = "R20E06 ";
  reThreeRoomOsdMap["tex1_640x480_292b5787b3414fcd_5"] = "R20E07 ";
  reThreeRoomOsdMap["tex1_640x480_328933cbe65eb095_5"] = "R20E08 ";
  reThreeRoomOsdMap["tex1_640x480_dc378609a9d8c60b_5"] = "R20F00 ";
  reThreeRoomOsdMap["tex1_640x480_91ad56be714a9f60_5"] = "R20F01 ";
  reThreeRoomOsdMap["tex1_640x480_b551b47888d0079e_5"] = "R20F03 ";
  reThreeRoomOsdMap["tex1_640x480_8324449d34a6330a_5"] = "R20F04 ";
  reThreeRoomOsdMap["tex1_640x480_3279e195b977f84b_5"] = "R20F05 ";
  reThreeRoomOsdMap["tex1_640x480_16e133d017823515_5"] = "R20F06 ";
  reThreeRoomOsdMap["tex1_640x480_5bf2a4f88126189b_5"] = "R20F07 ";
  reThreeRoomOsdMap["tex1_640x480_e0ffcdd0928ea09e_5"] = "R20F08 ";
  reThreeRoomOsdMap["tex1_640x480_8b9c972c14112209_5"] = "R20F09 ";
  reThreeRoomOsdMap["tex1_640x480_466f4c205a49f3f1_5"] = "R20F0A ";
  reThreeRoomOsdMap["tex1_640x480_13e2eb2e5d15e073_5"] = "R20F0B ";
  reThreeRoomOsdMap["tex1_640x480_de637a44ab6ada2c_5"] = "R20F0C ";
  reThreeRoomOsdMap["tex1_640x480_eb5bde491c0cbab9_5"] = "R20F0D ";
  reThreeRoomOsdMap["tex1_640x480_23dbb029c4390277_5"] = "R20F0E ";
  reThreeRoomOsdMap["tex1_640x480_04ad523622282ce7_5"] = "R20F11 ";
  reThreeRoomOsdMap["tex1_640x480_9af45201ccd83106_5"] = "R20F12 ";
  reThreeRoomOsdMap["tex1_640x480_bc4e047be3c9a7ef_5"] = "R20F13 ";
  reThreeRoomOsdMap["tex1_640x480_1899de21f37e5982_5"] = "R20F14 ";
  reThreeRoomOsdMap["tex1_640x480_f98efdf5651827ac_5"] = "R20F16 ";
  reThreeRoomOsdMap["tex1_640x480_70fd5e4b3d4ce27a_5"] = "R20F17 ";
  reThreeRoomOsdMap["tex1_640x480_35b6900a5348ab70_5"] = "R20F18 ";
  reThreeRoomOsdMap["tex1_640x480_f69889ba8829b780_5"] = "R21000 R21900 ";
  reThreeRoomOsdMap["tex1_640x480_7a45eab995e18827_5"] = "R21001 ";
  reThreeRoomOsdMap["tex1_640x480_ad757001ecfc9c02_5"] = "R21002 ";
  reThreeRoomOsdMap["tex1_640x480_9cd82419fe873970_5"] = "R21003 R21903 ";
  reThreeRoomOsdMap["tex1_640x480_e2e5621cf8403e8a_5"] = "R21004 R21904 ";
  reThreeRoomOsdMap["tex1_640x480_892b86be77ed3ac1_5"] = "R21005 R21905 ";
  reThreeRoomOsdMap["tex1_640x480_9c7f6aaf0a223f94_5"] = "R21006 R21906 ";
  reThreeRoomOsdMap["tex1_640x480_6bed8e3c2819c035_5"] = "R21007 R21907 ";
  reThreeRoomOsdMap["tex1_640x480_bc16f326d207e627_5"] = "R21008 ";
  reThreeRoomOsdMap["tex1_640x480_9332a39470102964_5"] = "R21009 R21909 ";
  reThreeRoomOsdMap["tex1_640x480_e525baabea186d85_5"] = "R2100A R2190A ";
  reThreeRoomOsdMap["tex1_640x480_be637ff1ac328ac4_5"] = "R21100 ";
  reThreeRoomOsdMap["tex1_640x480_952685faa504cfc2_5"] = "R21101 ";
  reThreeRoomOsdMap["tex1_640x480_cae21cd8d2b90110_5"] = "R21102 ";
  reThreeRoomOsdMap["tex1_640x480_8002f42e2e152fa6_5"] = "R21103 ";
  reThreeRoomOsdMap["tex1_640x480_19e0d202fa7215f3_5"] = "R21104 ";
  reThreeRoomOsdMap["tex1_640x480_a5da13056f58497a_5"] = "R21105 ";
  reThreeRoomOsdMap["tex1_640x480_a26d3fd77523cdc8_5"] = "R21106 ";
  reThreeRoomOsdMap["tex1_640x480_a4e23c0d7d5507e3_5"] = "R21107 ";
  reThreeRoomOsdMap["tex1_640x480_7222623e2a78a314_5"] = "R21108 ";
  reThreeRoomOsdMap["tex1_640x480_bff8cae37ce19843_5"] = "R21109 ";
  reThreeRoomOsdMap["tex1_640x480_adac401920b126b7_5"] = "R2110A ";
  reThreeRoomOsdMap["tex1_640x480_64bda52eec4d6a06_5"] = "R2110B ";
  reThreeRoomOsdMap["tex1_640x480_11cf4a85e312bc93_5"] = "R2110C ";
  reThreeRoomOsdMap["tex1_640x480_f38f7635ed152e68_5"] = "R2110D ";
  reThreeRoomOsdMap["tex1_640x480_4590ccbe705cf70b_5"] = "R2110E ";
  reThreeRoomOsdMap["tex1_640x480_cd9bebd40e981500_5"] = "R2110F ";
  reThreeRoomOsdMap["tex1_640x480_8774152ddafa6b1e_5"] = "R21110 ";
  reThreeRoomOsdMap["tex1_640x480_4f237abc4ed791cf_5"] = "R21111 ";
  reThreeRoomOsdMap["tex1_640x480_6c26f50ec3cd6b0a_5"] = "R21112 ";
  reThreeRoomOsdMap["tex1_640x480_7499d6fe0dd42496_5"] = "R21113 ";
  reThreeRoomOsdMap["tex1_640x480_c9aed19d339e8b3c_5"] = "R21114 ";
  reThreeRoomOsdMap["tex1_640x480_1e14d072e518a361_5"] = "R21115 ";
  reThreeRoomOsdMap["tex1_640x480_ec260b9c8144ceb4_5"] = "R21116 ";
  reThreeRoomOsdMap["tex1_640x480_08d42e70527f864d_5"] = "R21117 ";
  reThreeRoomOsdMap["tex1_640x480_54f66c04d268f62e_5"] = "R21118 ";
  reThreeRoomOsdMap["tex1_640x480_d7900de9e5045940_5"] = "R21119 ";
  reThreeRoomOsdMap["tex1_640x480_fad79ad0bdcbd7aa_5"] = "R2111A ";
  reThreeRoomOsdMap["tex1_640x480_aa7e3712d9905a1f_5"] = "R2111B ";
  reThreeRoomOsdMap["tex1_640x480_efdb7e543a68e56b_5"] = "R2111C ";
  reThreeRoomOsdMap["tex1_640x480_b449da115ee39a7a_5"] = "R2111E ";
  reThreeRoomOsdMap["tex1_640x480_feaa03fbb87f5f6d_5"] = "R2111F ";
  reThreeRoomOsdMap["tex1_320x240_61c435a18fdb2197_5"] = "R21200 ";
  reThreeRoomOsdMap["tex1_640x480_d4a31fae021f733a_5"] = "R21201 ";
  reThreeRoomOsdMap["tex1_640x480_b6f32ae0e50112e8_5"] = "R21202 ";
  reThreeRoomOsdMap["tex1_320x240_2c45da9127d4f5be_5"] = "R21203 ";
  reThreeRoomOsdMap["tex1_640x480_7f1762597c6eb3d1_5"] = "R21204 ";
  reThreeRoomOsdMap["tex1_640x480_4e894e8c6abbcda3_5"] = "R21205 ";
  reThreeRoomOsdMap["tex1_640x480_7d7db25885011209_5"] = "R21206 ";
  reThreeRoomOsdMap["tex1_640x480_c4c33443fbcdce24_5"] = "R21300 ";
  reThreeRoomOsdMap["tex1_640x480_93439ce98f486e35_5"] = "R21301 ";
  reThreeRoomOsdMap["tex1_640x480_ffabfe9271510ee3_5"] = "R21400 ";
  reThreeRoomOsdMap["tex1_640x480_a341c39b4d294755_5"] = "R21401 ";
  reThreeRoomOsdMap["tex1_640x480_5da4d0777cdacb7b_5"] = "R21402 ";
  reThreeRoomOsdMap["tex1_640x480_ae8538733b01296d_5"] = "R21403 ";
  reThreeRoomOsdMap["tex1_640x480_e9cf5e74c6f07b8e_5"] = "R21404 ";
  reThreeRoomOsdMap["tex1_640x480_a7bcec9f1f4d2941_5"] = "R21405 ";
  reThreeRoomOsdMap["tex1_640x480_3d52a956f6844d0e_5"] = "R21406 ";
  reThreeRoomOsdMap["tex1_640x480_b9a33601c726c856_5"] = "R21407 ";
  reThreeRoomOsdMap["tex1_640x480_4c9165e0cb1d6e78_5"] = "R21408 ";
  reThreeRoomOsdMap["tex1_640x480_5954ddf35ae5554d_5"] = "R21409 ";
  reThreeRoomOsdMap["tex1_640x480_356af204163b72d0_5"] = "R2140A ";
  reThreeRoomOsdMap["tex1_640x480_71fd08dfda27983f_5"] = "R2140B ";
  reThreeRoomOsdMap["tex1_640x480_7acd5b4cb7bb2e93_5"] = "R2140D ";
  reThreeRoomOsdMap["tex1_640x480_af9d1968dbdc962a_5"] = "R2140E ";
  reThreeRoomOsdMap["tex1_640x480_f01a1d850b49a310_5"] = "R2140F ";
  reThreeRoomOsdMap["tex1_640x480_d089b37250d1aad7_5"] = "R21410 ";
  reThreeRoomOsdMap["tex1_640x480_16025961a8bb486b_5"] = "R21411 ";
  reThreeRoomOsdMap["tex1_640x480_c4223fb9c7303756_5"] = "R21412 ";
  reThreeRoomOsdMap["tex1_640x480_562329c619f5d450_5"] = "R21413 ";
  reThreeRoomOsdMap["tex1_640x480_1a106a4114507594_5"] = "R21414 ";
  reThreeRoomOsdMap["tex1_640x480_a2019e94c7c7c2f6_5"] = "R21600 ";
  reThreeRoomOsdMap["tex1_640x480_9dc4833786f53d23_5"] = "R21700 ";
  reThreeRoomOsdMap["tex1_640x480_cb2bfa9c49925a92_5"] = "R21701 ";
  reThreeRoomOsdMap["tex1_640x480_636904cd776e8266_5"] = "R21702 ";
  reThreeRoomOsdMap["tex1_640x480_c2b66d1281486538_5"] = "R21704 ";
  reThreeRoomOsdMap["tex1_640x480_4252b00bbb4e8ab6_5"] = "R21705 ";
  reThreeRoomOsdMap["tex1_640x480_faa413acd523ac06_5"] = "R21706 ";
  reThreeRoomOsdMap["tex1_640x480_e7aa66bb966cc865_5"] = "R21707 ";
  reThreeRoomOsdMap["tex1_640x480_0fa344462aaf4e92_5"] = "R21709 ";
  reThreeRoomOsdMap["tex1_640x480_46ec442684840090_5"] = "R21800 ";
  reThreeRoomOsdMap["tex1_640x480_c492e7939b95fdf2_5"] = "R21801 ";
  reThreeRoomOsdMap["tex1_640x480_7deae8c96d188420_5"] = "R21802 ";
  reThreeRoomOsdMap["tex1_640x480_94e6178f6f9dfbb4_5"] = "R21803 ";
  reThreeRoomOsdMap["tex1_640x480_56fe688fc48d5ddc_5"] = "R21804 ";
  reThreeRoomOsdMap["tex1_640x480_e6b2247443c6cab5_5"] = "R21805 ";
  reThreeRoomOsdMap["tex1_640x480_de71726366daf8c1_5"] = "R21805 ";
  reThreeRoomOsdMap["tex1_640x480_de71726366daf8c1_5"] = "R21806 ";
  reThreeRoomOsdMap["tex1_640x480_9b12ad33a0f7ad05_5"] = "R21807 ";
  reThreeRoomOsdMap["tex1_640x480_28041dd2efb61d0e_5"] = "R21A00 ";
  reThreeRoomOsdMap["tex1_640x480_e6362e8516c36b62_5"] = "R21A01 ";
  reThreeRoomOsdMap["tex1_640x480_90105f8d163250a8_5"] = "R21A02 ";
  reThreeRoomOsdMap["tex1_640x480_e755b8b06ae21064_5"] = "R21A03 ";
  reThreeRoomOsdMap["tex1_640x480_7cea7b2eb491caf7_5"] = "R21A04 ";
  reThreeRoomOsdMap["tex1_640x480_478c352e2f543d04_5"] = "R21A05 ";
  reThreeRoomOsdMap["tex1_640x480_21f13e29e8b00e59_5"] = "R21B00 ";
  reThreeRoomOsdMap["tex1_640x480_ad058525ea44f67d_5"] = "R21B01 ";
  reThreeRoomOsdMap["tex1_640x480_1e1eef964fe37e5a_5"] = "R30000 R31000 ";
  reThreeRoomOsdMap["tex1_640x480_c0bafdfee0d6b11b_5"] = "R30001 ";
  reThreeRoomOsdMap["tex1_640x480_46808c76b6bfb377_5"] = "R30002 ";
  reThreeRoomOsdMap["tex1_640x480_8499a795cca51992_5"] = "R30003 ";
  reThreeRoomOsdMap["tex1_640x480_9c35404a3b18868e_5"] = "R30004 ";
  reThreeRoomOsdMap["tex1_640x480_852a0f248807792b_5"] = "R30005 ";
  reThreeRoomOsdMap["tex1_640x480_66ea3e350c4fb846_5"] = "R30006 R31006 ";
  reThreeRoomOsdMap["tex1_640x480_58253c95a93de640_5"] = "R30007 R31007 ";
  reThreeRoomOsdMap["tex1_640x480_a287513b8e68d075_5"] = "R30008 R31008 ";
  reThreeRoomOsdMap["tex1_640x480_678aa80193509218_5"] = "R30009 R31009 ";
  reThreeRoomOsdMap["tex1_640x480_a494a82cdb289d36_5"] = "R30100 ";
  reThreeRoomOsdMap["tex1_640x480_1da198506b1874d1_5"] = "R30101 ";
  reThreeRoomOsdMap["tex1_640x480_3ad19429d4377876_5"] = "R30102 ";
  reThreeRoomOsdMap["tex1_640x480_3010ac25da40cb31_5"] = "R30103 ";
  reThreeRoomOsdMap["tex1_640x480_26c088edb601b880_5"] = "R30106 ";
  reThreeRoomOsdMap["tex1_640x480_c1543867f73c8893_5"] = "R30107 ";
  reThreeRoomOsdMap["tex1_640x480_560ee7d848a850a4_5"] = "R30108 ";
  reThreeRoomOsdMap["tex1_640x480_b600e05a170900fe_5"] = "R30109 ";
  reThreeRoomOsdMap["tex1_640x480_21bb13c440f77c19_5"] = "R3010A ";
  reThreeRoomOsdMap["tex1_640x480_84be82992e3fbc0e_5"] = "R30200 ";
  reThreeRoomOsdMap["tex1_640x480_8f6967446640a175_5"] = "R30202 ";
  reThreeRoomOsdMap["tex1_640x480_4e04be47bc025f52_5"] = "R30203 R31703 ";
  reThreeRoomOsdMap["tex1_640x480_757ec75214ebde9a_5"] = "R30204 ";
  reThreeRoomOsdMap["tex1_640x480_66f80344d61ec246_5"] = "R30205 ";
  reThreeRoomOsdMap["tex1_640x480_b58721bf20a11c58_5"] = "R30206 ";
  reThreeRoomOsdMap["tex1_640x480_ead3252148f60d63_5"] = "R30207 ";
  reThreeRoomOsdMap["tex1_640x480_8071ab60ab364860_5"] = "R30300 ";
  reThreeRoomOsdMap["tex1_640x480_bbc2dce3b314f7cb_5"] = "R30301 ";
  reThreeRoomOsdMap["tex1_640x480_67f99cbb7910e973_5"] = "R30302 ";
  reThreeRoomOsdMap["tex1_640x480_c38ca0eab1b9a7c0_5"] = "R30303 ";
  reThreeRoomOsdMap["tex1_640x480_59c8417609004770_5"] = "R30304 ";
  reThreeRoomOsdMap["tex1_640x480_cb9b23e9a37aa515_5"] = "R30305 ";
  reThreeRoomOsdMap["tex1_640x480_f82a811c4f5087c5_5"] = "R30306 ";
  reThreeRoomOsdMap["tex1_640x480_85fe7d1b76ee20b6_5"] = "R30307 ";
  reThreeRoomOsdMap["tex1_640x480_810b433e66ebbc35_5"] = "R30308 ";
  reThreeRoomOsdMap["tex1_640x480_e417124bc99237e8_5"] = "R30309 ";
  reThreeRoomOsdMap["tex1_640x480_8c5cf94ff672b098_5"] = "R3030C ";
  reThreeRoomOsdMap["tex1_640x480_865e66a6ff361f00_5"] = "R3030D ";
  reThreeRoomOsdMap["tex1_640x480_42c8c58fc8c2a5fa_5"] = "R30400 ";
  reThreeRoomOsdMap["tex1_640x480_b3d1eb25898173c3_5"] = "R30401 ";
  reThreeRoomOsdMap["tex1_640x480_f7d2035099e86ce6_5"] = "R30402 ";
  reThreeRoomOsdMap["tex1_640x480_9dd59b2e58faee2c_5"] = "R30403 ";
  reThreeRoomOsdMap["tex1_640x480_9cab5f6f58a40366_5"] = "R30404 ";
  reThreeRoomOsdMap["tex1_640x480_6a7d92d41e7304b5_5"] = "R30405 ";
  reThreeRoomOsdMap["tex1_640x480_576b89d0bf859394_5"] = "R30406 ";
  reThreeRoomOsdMap["tex1_640x480_c2cf554be0b6a4c4_5"] = "R30407 ";
  reThreeRoomOsdMap["tex1_640x480_6ad19c3b3aeec574_5"] = "R30408 ";
  reThreeRoomOsdMap["tex1_640x480_6ab338a678e1de28_5"] = "R30500 R31100 ";
  reThreeRoomOsdMap["tex1_640x480_befe9208494a75ee_5"] = "R30501 R31101 ";
  reThreeRoomOsdMap["tex1_640x480_2cf76e007ded387a_5"] = "R30502 R31102 ";
  reThreeRoomOsdMap["tex1_640x480_ccef901759c79d3f_5"] = "R30503 R31103 ";
  reThreeRoomOsdMap["tex1_640x480_bc97f2e2a79bc872_5"] = "R30504 R31104 ";
  reThreeRoomOsdMap["tex1_640x480_71ba67d9096c4063_5"] = "R30505 R31105 ";
  reThreeRoomOsdMap["tex1_640x480_ea41b40299302c7e_5"] = "R30506 R31106 ";
  reThreeRoomOsdMap["tex1_640x480_13285c33b9b64d9a_5"] = "R30507 R31107 ";
  reThreeRoomOsdMap["tex1_640x480_d148cbba68a30fc9_5"] = "R30508 R31108 ";
  reThreeRoomOsdMap["tex1_640x480_52e80c096e2de82d_5"] = "R30600 R31200 ";
  reThreeRoomOsdMap["tex1_640x480_038357436e1a5810_5"] = "R30601 R31201 ";
  reThreeRoomOsdMap["tex1_640x480_6c2eb8e6348321a1_5"] = "R30602 R31202 ";
  reThreeRoomOsdMap["tex1_640x480_aac97bd9234b5983_5"] = "R30700 R31300 ";
  reThreeRoomOsdMap["tex1_640x480_0014b79c9561a6d2_5"] = "R30701 R31301 ";
  reThreeRoomOsdMap["tex1_640x480_920a4e54286fab54_5"] = "R30702 R31302 ";
  reThreeRoomOsdMap["tex1_640x480_72243db2a2141a29_5"] = "R30703 R31303 ";
  reThreeRoomOsdMap["tex1_640x480_800d4d899a84a77d_5"] = "R30704 R31304 ";
  reThreeRoomOsdMap["tex1_640x480_6a65c355fceb53be_5"] = "R30705 R31305 ";
  reThreeRoomOsdMap["tex1_640x480_fa5b0d95e817f18e_5"] = "R30706 R31306 ";
  reThreeRoomOsdMap["tex1_640x480_83473ff23f73c72a_5"] = "R30800 R31400 ";
  reThreeRoomOsdMap["tex1_640x480_6081c75010edf6d7_5"] = "R30801 R31401 ";
  reThreeRoomOsdMap["tex1_640x480_65b1409931f773d4_5"] = "R30802 R31402 ";
  reThreeRoomOsdMap["tex1_640x480_151ebe4b217becc3_5"] = "R30803 R31403 ";
  reThreeRoomOsdMap["tex1_640x480_b4e35f277022dafc_5"] = "R30804 R31404 ";
  reThreeRoomOsdMap["tex1_640x480_31b6d0835210d03c_5"] = "R30805 R31405 ";
  reThreeRoomOsdMap["tex1_640x480_21ec9a4aca6379f9_5"] = "R30900 R31500 ";
  reThreeRoomOsdMap["tex1_640x480_1403779b7d1898c2_5"] = "R30901 R31501 ";
  reThreeRoomOsdMap["tex1_640x480_7a5e5f8306e3d158_5"] = "R30902 R31502 ";
  reThreeRoomOsdMap["tex1_640x480_4194b19d74354cdf_5"] = "R30903 R31503 ";
  reThreeRoomOsdMap["tex1_640x480_326a4d0939829f8c_5"] = "R30904 R31504 ";
  reThreeRoomOsdMap["tex1_640x480_612c7f7749f04367_5"] = "R30906 R31506 ";
  reThreeRoomOsdMap["tex1_640x480_62d3c0f1f4334594_5"] = "R30907 R31507 ";
  reThreeRoomOsdMap["tex1_640x480_2d560481ef9ac247_5"] = "R30908 R31508 ";
  reThreeRoomOsdMap["tex1_640x480_de7eb3e9d985b236_5"] = "R30909 R31509 ";
  reThreeRoomOsdMap["tex1_640x480_bb1eda17078e1970_5"] = "R3090A R3150A ";
  reThreeRoomOsdMap["tex1_640x480_afdc747db1c56a63_5"] = "R3090B R3150B ";
  reThreeRoomOsdMap["tex1_640x480_c897e95590f499de_5"] = "R3090C R3150C ";
  reThreeRoomOsdMap["tex1_640x480_ec96cc484f3d40c3_5"] = "R3090D R3150D ";
  reThreeRoomOsdMap["tex1_640x480_7b759def798a3836_5"] = "R30A00 ";
  reThreeRoomOsdMap["tex1_640x480_71cc37ddf54f3a2b_5"] = "R30A01 ";
  reThreeRoomOsdMap["tex1_640x480_2a32a4266f130a11_5"] = "R30A02 ";
  reThreeRoomOsdMap["tex1_640x480_0a2d756c40fc3004_5"] = "R30A03 ";
  reThreeRoomOsdMap["tex1_640x480_f3cede5b99b116bc_5"] = "R30A04 ";
  reThreeRoomOsdMap["tex1_640x480_668a52dbc82e1317_5"] = "R30A05 ";
  reThreeRoomOsdMap["tex1_640x480_34cf4ebb193fe81f_5"] = "R30B00 ";
  reThreeRoomOsdMap["tex1_640x480_fac1e2e4efd26ff5_5"] = "R30B01 ";
  reThreeRoomOsdMap["tex1_640x480_520c95a4f90cf65f_5"] = "R30B02 ";
  reThreeRoomOsdMap["tex1_640x480_40ad9e0d5257ffe4_5"] = "R30B03 ";
  reThreeRoomOsdMap["tex1_640x480_cda81ff761d6e542_5"] = "R30B04 ";
  reThreeRoomOsdMap["tex1_640x480_8d37170d403f7eae_5"] = "R30B05 ";
  reThreeRoomOsdMap["tex1_640x480_6eee98084f9d27a0_5"] = "R30B06 ";
  reThreeRoomOsdMap["tex1_640x480_06125ddd8171ef9d_5"] = "R30B07 ";
  reThreeRoomOsdMap["tex1_640x480_f83e5545a724815b_5"] = "R30B08 ";
  reThreeRoomOsdMap["tex1_640x480_354031a1ef4503f0_5"] = "R30B09 ";
  reThreeRoomOsdMap["tex1_640x480_3b5f73f00a4c6153_5"] = "R30B0A ";
  reThreeRoomOsdMap["tex1_640x480_5c119c374acc9227_5"] = "R30B0B ";
  reThreeRoomOsdMap["tex1_640x480_687e3fbe52433f75_5"] = "R30B0C ";
  reThreeRoomOsdMap["tex1_640x480_acfee403e6dc189e_5"] = "R30B0E ";
  reThreeRoomOsdMap["tex1_640x480_0428d9d8bf89631f_5"] = "R30B0F ";
  reThreeRoomOsdMap["tex1_640x480_505d5d43d9e37ec5_5"] = "R30B11 ";
  reThreeRoomOsdMap["tex1_640x480_a004819069f31f55_5"] = "R30B12 ";
  reThreeRoomOsdMap["tex1_640x480_bb068247093474fb_5"] = "R30B13 ";
  reThreeRoomOsdMap["tex1_640x480_d1648597ddae3b3f_5"] = "R30B15 ";
  reThreeRoomOsdMap["tex1_640x480_30e11d232d39b02f_5"] = "R30B16 ";
  reThreeRoomOsdMap["tex1_640x480_69722e8037da40f2_5"] = "R30B17 ";
  reThreeRoomOsdMap["tex1_640x480_520c95a4f90cf65f_5"] = "R30B17 ";
  reThreeRoomOsdMap["tex1_640x480_764ed0f4b7da98fb_5"] = "R30B18 ";
  reThreeRoomOsdMap["tex1_640x480_b8297379d2a2bf31_5"] = "R30B19 ";
  reThreeRoomOsdMap["tex1_640x480_4b65d23cd6aa2cc5_5"] = "R30B1B ";
  reThreeRoomOsdMap["tex1_640x480_b65ed9bc37ad87eb_5"] = "R30B1C ";
  reThreeRoomOsdMap["tex1_640x480_a61d557cfdc02b0d_5"] = "R30C00 ";
  reThreeRoomOsdMap["tex1_640x480_31cabe92282c6f5b_5"] = "R30C01 ";
  reThreeRoomOsdMap["tex1_640x480_562065d1df3ae776_5"] = "R30C02 ";
  reThreeRoomOsdMap["tex1_640x480_8b39d354defc604e_5"] = "R30C03 ";
  reThreeRoomOsdMap["tex1_640x480_536c684ffb04d553_5"] = "R30C04 ";
  reThreeRoomOsdMap["tex1_640x480_79d05ed63069c265_5"] = "R30C05 ";
  reThreeRoomOsdMap["tex1_640x480_d5d864390359c28c_5"] = "R30C06 ";
  reThreeRoomOsdMap["tex1_640x480_682a64ef063becf3_5"] = "R30D01 ";
  reThreeRoomOsdMap["tex1_640x480_26e0c0d540449af4_5"] = "R30D02 ";
  reThreeRoomOsdMap["tex1_640x480_a10fcdf3e99e896c_5"] = "R30D03 ";
  reThreeRoomOsdMap["tex1_640x480_c38ca0eab1b9a7c0_5"] = "R30D03 ";
  reThreeRoomOsdMap["tex1_640x480_31fb00d6c4c2c72e_5"] = "R30D04 ";
  reThreeRoomOsdMap["tex1_640x480_8c1445fd28ee8617_5"] = "R30D05 ";
  reThreeRoomOsdMap["tex1_640x480_c239a2c7e4a86084_5"] = "R30D06 ";
  reThreeRoomOsdMap["tex1_640x480_4d33df4983e43cc1_5"] = "R30D07 ";
  reThreeRoomOsdMap["tex1_640x480_a3f722d2162a49b7_5"] = "R30D09 ";
  reThreeRoomOsdMap["tex1_640x480_bc974fed3fa2439f_5"] = "R30D0A ";
  reThreeRoomOsdMap["tex1_640x480_19861c071d693181_5"] = "R30D0B ";
  reThreeRoomOsdMap["tex1_640x480_8afd7d4b447ed397_5"] = "R30D0D ";
  reThreeRoomOsdMap["tex1_640x480_a2ea6235ba3237cf_5"] = "R30D0E ";
  reThreeRoomOsdMap["tex1_640x480_e386407e6bd64416_5"] = "R30D10 ";
  reThreeRoomOsdMap["tex1_640x480_f5f9177cf4920570_5"] = "R30D11 ";
  reThreeRoomOsdMap["tex1_640x480_d1da0490ceaeb01a_5"] = "R30D12 ";
  reThreeRoomOsdMap["tex1_640x480_18e18b7cadd18626_5"] = "R30D15 ";
  reThreeRoomOsdMap["tex1_640x480_9c95aac5bac02eb0_5"] = "R30D16 ";
  reThreeRoomOsdMap["tex1_640x480_1bdba56d1900f472_5"] = "R30D17 ";
  reThreeRoomOsdMap["tex1_640x480_3b51c4d280b2f3b6_5"] = "R30D18 ";
  reThreeRoomOsdMap["tex1_640x480_c1b93cc81bbf17e1_5"] = "R30D19 ";
  reThreeRoomOsdMap["tex1_640x480_9654ca8cf4af9d85_5"] = "R30E01 R31601 ";
  reThreeRoomOsdMap["tex1_640x480_23c6118f571314d1_5"] = "R30E02 R31602 ";
  reThreeRoomOsdMap["tex1_640x480_f574d4e02584bf1b_5"] = "R30E03 R31603 ";
  reThreeRoomOsdMap["tex1_640x480_395ac634d8441cbe_5"] = "R30E04 R31604 ";
  reThreeRoomOsdMap["tex1_640x480_0c390a123d3bc766_5"] = "R30E05 R31605 ";
  reThreeRoomOsdMap["tex1_640x480_86e5b136ecc36253_5"] = "R30E06 R31606 ";
  reThreeRoomOsdMap["tex1_640x480_dd9dba260e351628_5"] = "R30E07 R31607 ";
  reThreeRoomOsdMap["tex1_640x480_e5f519e5cdcbd692_5"] = "R30E09 R31609 ";
  reThreeRoomOsdMap["tex1_640x480_924dd6a1bebac823_5"] = "R30E0A R3160A ";
  reThreeRoomOsdMap["tex1_640x480_c55e30d1a15e893d_5"] = "R30E0B ";
  reThreeRoomOsdMap["tex1_640x480_9eb486e07bb68ea3_5"] = "R30E0C R3160C ";
  reThreeRoomOsdMap["tex1_320x240_ee84fa31ced8cf86_5"] = "R30E0D R3160D ";
  reThreeRoomOsdMap["tex1_640x480_106be49fc228cea7_5"] = "R30E0E ";
  reThreeRoomOsdMap["tex1_640x480_5ede6721e2f638c6_5"] = "R30E0F R3160F ";
  reThreeRoomOsdMap["tex1_640x480_8b9d7a30585fce2b_5"] = "R30E10 R31610 ";
  reThreeRoomOsdMap["tex1_640x480_c787f5532b8ac72f_5"] = "R30E12 R31612 ";
  reThreeRoomOsdMap["tex1_640x480_65355e97b66175a8_5"] = "R30F00 R31700 ";
  reThreeRoomOsdMap["tex1_640x480_5aaa13dc413e7b99_5"] = "R30F01 ";
  reThreeRoomOsdMap["tex1_640x480_886296d896cbd715_5"] = "R30F01 ";
  reThreeRoomOsdMap["tex1_640x480_6be8cb1ad29dd7b6_5"] = "R30F02 ";
  reThreeRoomOsdMap["tex1_640x480_4e04be47bc025f52_5"] = "R30F03 ";
  reThreeRoomOsdMap["tex1_640x480_7007229e35819dc5_5"] = "R30F03 ";
  reThreeRoomOsdMap["tex1_640x480_8d54a54992479361_5"] = "R30F04 R31704 ";
  reThreeRoomOsdMap["tex1_640x480_fe4c284c3e75d487_5"] = "R30F05 ";
  reThreeRoomOsdMap["tex1_640x480_f036bc903da60449_5"] = "R30F06 ";
  reThreeRoomOsdMap["tex1_640x480_e03dc52a03da272b_5"] = "R30F07 ";
  reThreeRoomOsdMap["tex1_640x480_5f0556dc8c29b1a2_5"] = "R30F09 R31709 ";
  reThreeRoomOsdMap["tex1_640x480_e03dc52a03da272b_5"] = "R30F0D R3170D ";
  reThreeRoomOsdMap["tex1_640x480_23c6118f571314d1_5"] = "R31602 ";
  reThreeRoomOsdMap["tex1_640x480_f574d4e02584bf1b_5"] = "R31603 ";
  reThreeRoomOsdMap["tex1_640x480_395ac634d8441cbe_5"] = "R31604 ";
  reThreeRoomOsdMap["tex1_640x480_0c390a123d3bc766_5"] = "R31605 ";
  reThreeRoomOsdMap["tex1_640x480_86e5b136ecc36253_5"] = "R31606 ";
  reThreeRoomOsdMap["tex1_640x480_dd9dba260e351628_5"] = "R31607 ";
  reThreeRoomOsdMap["tex1_640x480_e5f519e5cdcbd692_5"] = "R31609 ";
  reThreeRoomOsdMap["tex1_640x480_9eb486e07bb68ea3_5"] = "R3160C ";
  reThreeRoomOsdMap["tex1_320x240_ee84fa31ced8cf86_5"] = "R3160D ";
  reThreeRoomOsdMap["tex1_640x480_9654ca8cf4af9d85_5"] = "R3160E ";
  reThreeRoomOsdMap["tex1_640x480_5ede6721e2f638c6_5"] = "R3160F ";
  reThreeRoomOsdMap["tex1_640x480_8b9d7a30585fce2b_5"] = "R31610 ";
  reThreeRoomOsdMap["tex1_640x480_c787f5532b8ac72f_5"] = "R31612 ";
  reThreeRoomOsdMap["tex1_640x480_e8bc8e9c62788eaf_5"] = "R40000 ";
  reThreeRoomOsdMap["tex1_640x480_2c40abbef906d19e_5"] = "R40001 R41709 ";
  reThreeRoomOsdMap["tex1_640x480_f63bcbf7386e8f66_5"] = "R40002 R4170A ";
  reThreeRoomOsdMap["tex1_640x480_c67083940e1ff75c_5"] = "R40003 R4170B ";
  reThreeRoomOsdMap["tex1_640x480_7979c4f7393d4ce7_5"] = "R40004 ";
  reThreeRoomOsdMap["tex1_640x480_8f5dd5568c5850fe_5"] = "R40005 R41705 ";
  reThreeRoomOsdMap["tex1_640x480_efda8341d23ba0f5_5"] = "R40006 ";
  reThreeRoomOsdMap["tex1_640x480_3a6cc794c58f4fc8_5"] = "R40007 R4170C ";
  reThreeRoomOsdMap["tex1_640x480_dcdf9ad18b40fea9_5"] = "R40008 R41708 ";
  reThreeRoomOsdMap["tex1_640x480_0dbcd694ad53dd05_5"] = "R40100 ";
  reThreeRoomOsdMap["tex1_640x480_d3df287ed69704df_5"] = "R40101 ";
  reThreeRoomOsdMap["tex1_640x480_ef1552014ca209ba_5"] = "R40102 ";
  reThreeRoomOsdMap["tex1_640x480_d2ea87c09570eaef_5"] = "R40103 ";
  reThreeRoomOsdMap["tex1_640x480_b42b2834fc496f26_5"] = "R40200 ";
  reThreeRoomOsdMap["tex1_640x480_725dafd52d55e096_5"] = "R40201 ";
  reThreeRoomOsdMap["tex1_640x480_64a2156d776db4f4_5"] = "R40202 ";
  reThreeRoomOsdMap["tex1_640x480_db8a7c84dc353163_5"] = "R40203 ";
  reThreeRoomOsdMap["tex1_640x480_d8875fdc6a0557ca_5"] = "R40204 ";
  reThreeRoomOsdMap["tex1_640x480_6694ed3df6591da5_5"] = "R40205 ";
  reThreeRoomOsdMap["tex1_640x480_7038323a165beb38_5"] = "R40206 ";
  reThreeRoomOsdMap["tex1_640x480_0c9581830899aebc_5"] = "R40207 ";
  reThreeRoomOsdMap["tex1_640x480_6aaee035682e9fad_5"] = "R40300 ";
  reThreeRoomOsdMap["tex1_640x480_e89002bfd037e649_5"] = "R40301 ";
  reThreeRoomOsdMap["tex1_640x480_47ada7eb28ca99e8_5"] = "R40302 ";
  reThreeRoomOsdMap["tex1_640x480_2ad007fc424200e2_5"] = "R40400 ";
  reThreeRoomOsdMap["tex1_640x480_13db9c14b1d68940_5"] = "R40401 ";
  reThreeRoomOsdMap["tex1_640x480_7f391289dedc964a_5"] = "R40402 ";
  reThreeRoomOsdMap["tex1_640x480_8ac2edcfd15db90e_5"] = "R40403 ";
  reThreeRoomOsdMap["tex1_640x480_1f369585ae3fdc90_5"] = "R40404 ";
  reThreeRoomOsdMap["tex1_640x480_4d6666aa6351a3b2_5"] = "R40405 ";
  reThreeRoomOsdMap["tex1_640x480_4ef662d50a72fbbe_5"] = "R40406 ";
  reThreeRoomOsdMap["tex1_640x480_60a3251e9d102371_5"] = "R40407 ";
  reThreeRoomOsdMap["tex1_640x480_8360269857c11b15_5"] = "R40408 ";
  reThreeRoomOsdMap["tex1_640x480_ec417d00abb8c090_5"] = "R40409 ";
  reThreeRoomOsdMap["tex1_640x480_6e4854317f4926e7_5"] = "R4040A ";
  reThreeRoomOsdMap["tex1_640x480_79f5bcb779b6e13b_5"] = "R40500 ";
  reThreeRoomOsdMap["tex1_640x480_6694f9d1cf8668e0_5"] = "R40501 ";
  reThreeRoomOsdMap["tex1_640x480_7c16a1098f04d763_5"] = "R40502 ";
  reThreeRoomOsdMap["tex1_640x480_a4e672e713899018_5"] = "R40503 ";
  reThreeRoomOsdMap["tex1_640x480_dc99aa5478a3b1e6_5"] = "R40504 ";
  reThreeRoomOsdMap["tex1_640x480_4af66b9d013e381e_5"] = "R40505 ";
  reThreeRoomOsdMap["tex1_640x480_8dec2a8cf29ba25c_5"] = "R40506 ";
  reThreeRoomOsdMap["tex1_640x480_79e0e8242b22678e_5"] = "R40600 ";
  reThreeRoomOsdMap["tex1_640x480_1ad7c16d93e750ae_5"] = "R40600 ";
  reThreeRoomOsdMap["tex1_640x480_641a76481a845a94_5"] = "R40601 ";
  reThreeRoomOsdMap["tex1_640x480_4dc40ca508dda121_5"] = "R40602 ";
  reThreeRoomOsdMap["tex1_640x480_f71926df7bf9b79c_5"] = "R40603 ";
  reThreeRoomOsdMap["tex1_640x480_e396481f5012c3b1_5"] = "R40604 ";
  reThreeRoomOsdMap["tex1_640x480_c5259c50cb54f9fd_5"] = "R40605 ";
  reThreeRoomOsdMap["tex1_640x480_1ad7c16d93e750ae_5"] = "R40606 ";
  reThreeRoomOsdMap["tex1_640x480_f2bdee3c7ec5cda4_5"] = "R40607 ";
  reThreeRoomOsdMap["tex1_640x480_0376b9af5f5ac695_5"] = "R40608 ";
  reThreeRoomOsdMap["tex1_640x480_e76d211178bd228d_5"] = "R40700 ";
  reThreeRoomOsdMap["tex1_640x480_2bbb983c3aba81a0_5"] = "R40701 ";
  reThreeRoomOsdMap["tex1_640x480_9915c3462f99ce79_5"] = "R40800 ";
  reThreeRoomOsdMap["tex1_640x480_bfdbb60c770a83e3_5"] = "R40801 ";
  reThreeRoomOsdMap["tex1_640x480_6965b22701f4ec0f_5"] = "R40802 ";
  reThreeRoomOsdMap["tex1_640x480_7c52193201355927_5"] = "R40803 ";
  reThreeRoomOsdMap["tex1_640x480_029d54e53c3f3cc2_5"] = "R40804 ";
  reThreeRoomOsdMap["tex1_640x480_c6f0895c1ee0683c_5"] = "R40805 ";
  reThreeRoomOsdMap["tex1_640x480_4a0e7e036adb1c2e_5"] = "R40806 ";
  reThreeRoomOsdMap["tex1_640x480_2056988c730b1a4c_5"] = "R40808 ";
  reThreeRoomOsdMap["tex1_640x480_4e293e78bd6ce3fe_5"] = "R40809 ";
  reThreeRoomOsdMap["tex1_640x480_54fa98f4fbad2059_5"] = "R4080A ";
  reThreeRoomOsdMap["tex1_640x480_712209cd164070cc_5"] = "R4080B ";
  reThreeRoomOsdMap["tex1_640x480_01935c37135c6a36_5"] = "R4080C ";
  reThreeRoomOsdMap["tex1_640x480_7683725e6c464c9b_5"] = "R4080D ";
  reThreeRoomOsdMap["tex1_640x480_2f67c76ac0ed638e_5"] = "R4080E ";
  reThreeRoomOsdMap["tex1_640x480_6a32d0541725eb00_5"] = "R40900 ";
  reThreeRoomOsdMap["tex1_640x480_c33f88ff685387cc_5"] = "R40901 ";
  reThreeRoomOsdMap["tex1_640x480_9a2f2c32c8bba4ac_5"] = "R40902 ";
  reThreeRoomOsdMap["tex1_640x480_707907b7ba8d8325_5"] = "R40903 ";
  reThreeRoomOsdMap["tex1_640x480_f62b96d9db93031b_5"] = "R40904 ";
  reThreeRoomOsdMap["tex1_640x480_6fda372395740430_5"] = "R40905 ";
  reThreeRoomOsdMap["tex1_640x480_fb4d87abe1e359b5_5"] = "R40A00 ";
  reThreeRoomOsdMap["tex1_640x480_ec8a9b8fd4cd4bf6_5"] = "R40A01 ";
  reThreeRoomOsdMap["tex1_640x480_f898f068de7c44c2_5"] = "R40A02 ";
  reThreeRoomOsdMap["tex1_640x480_8c7e9e232803050a_5"] = "R40A03 ";
  reThreeRoomOsdMap["tex1_640x480_5cc5a6dae1066ec9_5"] = "R40A04 ";
  reThreeRoomOsdMap["tex1_640x480_00f35b1d933568c5_5"] = "R40A05 ";
  reThreeRoomOsdMap["tex1_640x480_abae225e31610f4a_5"] = "R40A06 ";
  reThreeRoomOsdMap["tex1_640x480_c57111e459dc7e49_5"] = "R40A07 ";
  reThreeRoomOsdMap["tex1_640x480_8f4bc27a8db65feb_5"] = "R40A08 ";
  reThreeRoomOsdMap["tex1_640x480_ab8324edbbcdece3_5"] = "R40A0A ";
  reThreeRoomOsdMap["tex1_640x480_904bd09e2d3f4408_5"] = "R40A0B ";
  reThreeRoomOsdMap["tex1_640x480_07645f958d32001e_5"] = "R40A0C ";
  reThreeRoomOsdMap["tex1_640x480_4f34ceced2537943_5"] = "R40A0E ";
  reThreeRoomOsdMap["tex1_640x480_a6171f5b6c50b2e7_5"] = "R40B00 ";
  reThreeRoomOsdMap["tex1_640x480_ecd48526fe6ae243_5"] = "R40B01 ";
  reThreeRoomOsdMap["tex1_640x480_311181290adcf4fd_5"] = "R40B02 ";
  reThreeRoomOsdMap["tex1_640x480_73f28d92d999f31f_5"] = "R40B03 ";
  reThreeRoomOsdMap["tex1_640x480_a4378184713204c0_5"] = "R40B04 ";
  reThreeRoomOsdMap["tex1_640x480_3310520aa140d3c9_5"] = "R40B05 ";
  reThreeRoomOsdMap["tex1_640x480_3542f41471664c1e_5"] = "R40B06 ";
  reThreeRoomOsdMap["tex1_640x480_3e4cdfda74a0ae77_5"] = "R40B07 ";
  reThreeRoomOsdMap["tex1_640x480_9cffa322240e5eca_5"] = "R40B08 ";
  reThreeRoomOsdMap["tex1_640x480_92476915ffabc0e1_5"] = "R40B09 ";
  reThreeRoomOsdMap["tex1_640x480_492fe387d51943e1_5"] = "R40B0A ";
  reThreeRoomOsdMap["tex1_640x480_85255ff0f6151bcc_5"] = "R40B0B ";
  reThreeRoomOsdMap["tex1_640x480_3d5324faa34272f8_5"] = "R40B0B ";
  reThreeRoomOsdMap["tex1_640x480_4e4d18e704d4f23b_5"] = "R40B0C ";
  reThreeRoomOsdMap["tex1_640x480_4ae8cea4a8cf3625_5"] = "R40B0D ";
  reThreeRoomOsdMap["tex1_640x480_7d4a97d0bc96379e_5"] = "R40B0E ";
  reThreeRoomOsdMap["tex1_640x480_b10f33b054c663ac_5"] = "R40B0F ";
  reThreeRoomOsdMap["tex1_640x480_3d5324faa34272f8_5"] = "R40B10 ";
  reThreeRoomOsdMap["tex1_640x480_efba14bca992b102_5"] = "R40B11 ";
  reThreeRoomOsdMap["tex1_640x480_4e4d18e704d4f23b_5"] = "R40B11 ";
  reThreeRoomOsdMap["tex1_640x480_a899f5609dd94f1a_5"] = "R40B12 ";
  reThreeRoomOsdMap["tex1_640x480_a4aec6d210430563_5"] = "R40B13 ";
  reThreeRoomOsdMap["tex1_640x480_dc7baf6b4bd2c823_5"] = "R40C00 ";
  reThreeRoomOsdMap["tex1_640x480_e1342a83f0833caf_5"] = "R40C01 ";
  reThreeRoomOsdMap["tex1_640x480_045ee87b7dbfed38_5"] = "R40C02 ";
  reThreeRoomOsdMap["tex1_640x480_45fe46fa99a10028_5"] = "R40C03 ";
  reThreeRoomOsdMap["tex1_640x480_0c516b797c4b67db_5"] = "R40C04 ";
  reThreeRoomOsdMap["tex1_640x480_2520ada939126ae3_5"] = "R40C05 ";
  reThreeRoomOsdMap["tex1_640x480_542b237d317430da_5"] = "R40C06 ";
  reThreeRoomOsdMap["tex1_640x480_afbb52e868848fe0_5"] = "R40C07 ";
  reThreeRoomOsdMap["tex1_640x480_cd51577ca96a4e5e_5"] = "R40C08 ";
  reThreeRoomOsdMap["tex1_640x480_4dd63820c7cae709_5"] = "R40C09 ";
  reThreeRoomOsdMap["tex1_640x480_3a2d7f554da2c796_5"] = "R40C0A ";
  reThreeRoomOsdMap["tex1_640x480_22a5e32b6910cfea_5"] = "R40D00 ";
  reThreeRoomOsdMap["tex1_640x480_b8c4d4a5ad94e861_5"] = "R40D01 ";
  reThreeRoomOsdMap["tex1_640x480_4e9983906b3d16d5_5"] = "R40D02 ";
  reThreeRoomOsdMap["tex1_640x480_477777849eb93a8a_5"] = "R40D03 ";
  reThreeRoomOsdMap["tex1_640x480_6364a747f52c7168_5"] = "R40D04 ";
  reThreeRoomOsdMap["tex1_640x480_2ea34704c875ee39_5"] = "R40D05 ";
  reThreeRoomOsdMap["tex1_640x480_85df19bb5f499561_5"] = "R40D06 ";
  reThreeRoomOsdMap["tex1_640x480_65e0b62f30df066c_5"] = "R40D07 ";
  reThreeRoomOsdMap["tex1_640x480_0667bdf559f30e50_5"] = "R40D08 ";
  reThreeRoomOsdMap["tex1_640x480_014a1c77c2558d5b_5"] = "R40D09 ";
  reThreeRoomOsdMap["tex1_640x480_4a634c4cce1ed10b_5"] = "R40D0A ";
  reThreeRoomOsdMap["tex1_640x480_2528c7a181c52d03_5"] = "R40D0B ";
  reThreeRoomOsdMap["tex1_640x480_40dd13ce893729c7_5"] = "R40E00 ";
  reThreeRoomOsdMap["tex1_640x480_5133ef8b80969e02_5"] = "R40E01 ";
  reThreeRoomOsdMap["tex1_640x480_acead199104b8958_5"] = "R40E02 ";
  reThreeRoomOsdMap["tex1_640x480_4e8537d9129a82b8_5"] = "R40E03 ";
  reThreeRoomOsdMap["tex1_640x480_9558fd7676e7ff4d_5"] = "R40E04 ";
  reThreeRoomOsdMap["tex1_640x480_73fd819318b53274_5"] = "R40E06 ";
  reThreeRoomOsdMap["tex1_640x480_44c4aa2a10466b64_5"] = "R40F00 ";
  reThreeRoomOsdMap["tex1_640x480_9b26668e23c4570b_5"] = "R40F01 ";
  reThreeRoomOsdMap["tex1_640x480_a8ef65dc2d0def84_5"] = "R40F02 ";
  reThreeRoomOsdMap["tex1_640x480_527362235a9ce6d2_5"] = "R40F03 ";
  reThreeRoomOsdMap["tex1_640x480_f8a9113b825513f3_5"] = "R40F04 ";
  reThreeRoomOsdMap["tex1_640x480_ea36d0f4b01f1c8a_5"] = "R40F05 ";
  reThreeRoomOsdMap["tex1_640x480_ab1f6a18b146757c_5"] = "R40F06 ";
  reThreeRoomOsdMap["tex1_640x480_61d5ab40c32e722f_5"] = "R40F07 ";
  reThreeRoomOsdMap["tex1_640x480_baf50fb301c5cd88_5"] = "R40F08 ";
  reThreeRoomOsdMap["tex1_640x480_55d89429aa7e4838_5"] = "R40F09 ";
  reThreeRoomOsdMap["tex1_640x480_81d23451e45fbc72_5"] = "R40F0A ";
  reThreeRoomOsdMap["tex1_640x480_ab1f6a18b146757c_5"] = "R40F0A ";
  reThreeRoomOsdMap["tex1_640x480_32a3d28d9e2bbca0_5"] = "R41000 ";
  reThreeRoomOsdMap["tex1_320x240_0cab36b220cff0da_5"] = "R41001 ";
  reThreeRoomOsdMap["tex1_320x240_7a354e2083824259_5"] = "R41002 ";
  reThreeRoomOsdMap["tex1_320x240_cffd46d31dc4d5bf_5"] = "R41002 ";
  reThreeRoomOsdMap["tex1_320x240_5ca57c788de3ade4_5"] = "R41003 ";
  reThreeRoomOsdMap["tex1_640x480_c18478010d31f684_5"] = "R41004 ";
  reThreeRoomOsdMap["tex1_640x480_d0f61487d98ea4f3_5"] = "R41005 ";
  reThreeRoomOsdMap["tex1_640x480_f1470d5ef854ad7e_5"] = "R41006 ";
  reThreeRoomOsdMap["tex1_640x480_a571fadbf3e1f233_5"] = "R41100 ";
  reThreeRoomOsdMap["tex1_640x480_6ca790b2515d2928_5"] = "R41101 ";
  reThreeRoomOsdMap["tex1_640x480_6acdd7b4178bc224_5"] = "R41102 ";
  reThreeRoomOsdMap["tex1_640x480_7f69b5edaf9bdc06_5"] = "R41103 ";
  reThreeRoomOsdMap["tex1_640x480_8dd6137d33e9f0c3_5"] = "R41104 ";
  reThreeRoomOsdMap["tex1_640x480_a7b64050a2c97bd8_5"] = "R41105 R4150B ";
  reThreeRoomOsdMap["tex1_640x480_28d22585dd44b3bf_5"] = "R41106 R4150C ";
  reThreeRoomOsdMap["tex1_640x480_13802a48830899e6_5"] = "R41107 ";
  reThreeRoomOsdMap["tex1_640x480_a560f866dad0ca98_5"] = "R41108 ";
  reThreeRoomOsdMap["tex1_640x480_84b4d5a4df36345b_5"] = "R41200 ";
  reThreeRoomOsdMap["tex1_640x480_3bc22a021afff450_5"] = "R41201 ";
  reThreeRoomOsdMap["tex1_640x480_fadf23f093325246_5"] = "R41202 ";
  reThreeRoomOsdMap["tex1_640x480_4327a86064e830a6_5"] = "R41203 ";
  reThreeRoomOsdMap["tex1_640x480_13960c48c7370043_5"] = "R41203 ";
  reThreeRoomOsdMap["tex1_640x480_f8cf55d657e16c2f_5"] = "R41204 ";
  reThreeRoomOsdMap["tex1_640x480_13960c48c7370043_5"] = "R41205 ";
  reThreeRoomOsdMap["tex1_640x480_eff56bae4fee3125_5"] = "R41206 ";
  reThreeRoomOsdMap["tex1_640x480_c89fe2dd382159bc_5"] = "R41207 ";
  reThreeRoomOsdMap["tex1_640x480_91bc12af66ee8e7b_5"] = "R41208 ";
  reThreeRoomOsdMap["tex1_640x480_1de992d1ec73d5a5_5"] = "R41209 ";
  reThreeRoomOsdMap["tex1_640x480_d65c121bfd8c3cc1_5"] = "R4120B ";
  reThreeRoomOsdMap["tex1_640x480_3cfe5ed785cb319d_5"] = "R41300 ";
  reThreeRoomOsdMap["tex1_640x480_b77e8167f6cf72b8_5"] = "R41400 ";
  reThreeRoomOsdMap["tex1_640x480_de860a9d4b762422_5"] = "R41401 ";
  reThreeRoomOsdMap["tex1_640x480_b9d7039bb5844b8a_5"] = "R41402 ";
  reThreeRoomOsdMap["tex1_640x480_667cf9b0d7e06b03_5"] = "R41403 ";
  reThreeRoomOsdMap["tex1_640x480_4aa9e6fd3dfb33f7_5"] = "R41404 ";
  reThreeRoomOsdMap["tex1_640x480_9d798eb72ca637a6_5"] = "R41405 ";
  reThreeRoomOsdMap["tex1_640x480_301e98bcf80c3b85_5"] = "R41500 ";
  reThreeRoomOsdMap["tex1_640x480_dbe4358bdce62a35_5"] = "R41501 ";
  reThreeRoomOsdMap["tex1_640x480_f95d2d6c379efa54_5"] = "R41502 ";
  reThreeRoomOsdMap["tex1_640x480_3a6a221e7399aae0_5"] = "R41503 ";
  reThreeRoomOsdMap["tex1_640x480_9cfca30cd8bce71c_5"] = "R41504 ";
  reThreeRoomOsdMap["tex1_640x480_51ef3df9cfea0b74_5"] = "R41505 ";
  reThreeRoomOsdMap["tex1_640x480_7a4669782901c9d6_5"] = "R41506 ";
  reThreeRoomOsdMap["tex1_640x480_c73c2bd44e88996e_5"] = "R41507 ";
  reThreeRoomOsdMap["tex1_640x480_c868141ffe7f9c21_5"] = "R4150A ";
  reThreeRoomOsdMap["tex1_640x480_22906747f3d6d5cb_5"] = "R41701 ";
  reThreeRoomOsdMap["tex1_640x480_79b1915c331107da_5"] = "R41702 ";
  reThreeRoomOsdMap["tex1_640x480_11ab242d23c84be4_5"] = "R41703 ";
  reThreeRoomOsdMap["tex1_640x480_ba81c4c34b6123f6_5"] = "R41707 ";
  reThreeRoomOsdMap["tex1_640x480_24a516908afbbfb9_5"] = "R50000 ";
  reThreeRoomOsdMap["tex1_640x480_f06de06a55c9a2c0_5"] = "R50001 ";
  reThreeRoomOsdMap["tex1_640x480_229041e273280a7c_5"] = "R50002 ";
  reThreeRoomOsdMap["tex1_640x480_9d5111be3a056c5e_5"] = "R50003 ";
  reThreeRoomOsdMap["tex1_640x480_72ffbd199c7840e5_5"] = "R50004 ";
  reThreeRoomOsdMap["tex1_640x480_af79c138a5c3e1a3_5"] = "R50005 ";
  reThreeRoomOsdMap["tex1_640x480_0beaac8b159500e5_5"] = "R50006 R50A14 ";
  reThreeRoomOsdMap["tex1_640x480_3c326a0c3fab1f69_5"] = "R50007 ";
  reThreeRoomOsdMap["tex1_640x480_ed25ef53775488f7_5"] = "R50008 ";
  reThreeRoomOsdMap["tex1_320x240_5b453ba738aac907_5"] = "R50009 ";
  reThreeRoomOsdMap["tex1_640x480_83c0aa59b4d08029_5"] = "R5000A ";
  reThreeRoomOsdMap["tex1_640x480_6ab616fa030b6054_5"] = "R5000B ";
  reThreeRoomOsdMap["tex1_640x480_01b0cf0178847524_5"] = "R50100 ";
  reThreeRoomOsdMap["tex1_640x480_2d3773cb5d20d3f5_5"] = "R50101 ";
  reThreeRoomOsdMap["tex1_640x480_19dfa78bdd801043_5"] = "R50102 ";
  reThreeRoomOsdMap["tex1_640x480_874698f3f4652482_5"] = "R50103 ";
  reThreeRoomOsdMap["tex1_640x480_60c831060600f831_5"] = "R50104 ";
  reThreeRoomOsdMap["tex1_640x480_12e6cbc527ffa213_5"] = "R50200 ";
  reThreeRoomOsdMap["tex1_640x480_d6ad16a26db5179e_5"] = "R50201 ";
  reThreeRoomOsdMap["tex1_640x480_34733bfe139ae653_5"] = "R50202 ";
  reThreeRoomOsdMap["tex1_640x480_4f099dd52d607afb_5"] = "R50203 ";
  reThreeRoomOsdMap["tex1_640x480_d85e64c9681e0fdf_5"] = "R50204 ";
  reThreeRoomOsdMap["tex1_640x480_2470bb0d93319a2b_5"] = "R50205 ";
  reThreeRoomOsdMap["tex1_640x480_be9d09965a040d3f_5"] = "R50206 ";
  reThreeRoomOsdMap["tex1_640x480_e6fc7e96eb38fece_5"] = "R50207 ";
  reThreeRoomOsdMap["tex1_640x480_f0421cbe327319c3_5"] = "R50208 ";
  reThreeRoomOsdMap["tex1_640x480_cbec7437b5121f7c_5"] = "R5020A ";
  reThreeRoomOsdMap["tex1_640x480_34e6acf7003ceaad_5"] = "R5020B ";
  reThreeRoomOsdMap["tex1_640x480_5d3824d1657a127a_5"] = "R5020C R50304 R50609 ";
  reThreeRoomOsdMap["tex1_640x480_fcbeb32ca212c303_5"] = "R50300 ";
  reThreeRoomOsdMap["tex1_640x480_3cc9765def289d1e_5"] = "R50301 ";
  reThreeRoomOsdMap["tex1_640x480_48eb560a3fde4617_5"] = "R50302 ";
  reThreeRoomOsdMap["tex1_640x480_7e8d075d382003f6_5"] = "R50303 ";
  reThreeRoomOsdMap["tex1_640x480_79c962c657dccd20_5"] = "R50305 ";
  reThreeRoomOsdMap["tex1_640x480_371349b6ee393ca8_5"] = "R50306 ";
  reThreeRoomOsdMap["tex1_640x480_44daf7ec2b198866_5"] = "R50307 ";
  reThreeRoomOsdMap["tex1_640x480_678dc871ec002639_5"] = "R50400 ";
  reThreeRoomOsdMap["tex1_640x480_d20b0ed113997737_5"] = "R50401 ";
  reThreeRoomOsdMap["tex1_640x480_acd3647ca64c79a2_5"] = "R50402 ";
  reThreeRoomOsdMap["tex1_640x480_58dd7bcf7e52b312_5"] = "R50403 ";
  reThreeRoomOsdMap["tex1_640x480_3e10f337ce10856c_5"] = "R50404 ";
  reThreeRoomOsdMap["tex1_640x480_e164311253977a1c_5"] = "R50405 ";
  reThreeRoomOsdMap["tex1_640x480_0cfa28f27018ec4f_5"] = "R50406 ";
  reThreeRoomOsdMap["tex1_640x480_ccd56cff09f01645_5"] = "R50407 ";
  reThreeRoomOsdMap["tex1_640x480_c29089736d356085_5"] = "R50408 ";
  reThreeRoomOsdMap["tex1_640x480_078067d9ad124b70_5"] = "R50409 ";
  reThreeRoomOsdMap["tex1_640x480_bd036c6e269ad135_5"] = "R5040A ";
  reThreeRoomOsdMap["tex1_640x480_2a06892dc00c751e_5"] = "R5040B ";
  reThreeRoomOsdMap["tex1_640x480_5f1eb96c925a7b01_5"] = "R5040C ";
  reThreeRoomOsdMap["tex1_640x480_c5cb5947326caf1c_5"] = "R5040D ";
  reThreeRoomOsdMap["tex1_640x480_8a5da116458d273f_5"] = "R5040E ";
  reThreeRoomOsdMap["tex1_640x480_6a5b0b23bfd2f8c0_5"] = "R5040F ";
  reThreeRoomOsdMap["tex1_640x480_f6bc0376f6248e1b_5"] = "R50500 ";
  reThreeRoomOsdMap["tex1_640x480_7f7a2274caf92880_5"] = "R50501 ";
  reThreeRoomOsdMap["tex1_640x480_aacebb0418fa077d_5"] = "R50502 ";
  reThreeRoomOsdMap["tex1_640x480_86b749f4084e0a59_5"] = "R50600 ";
  reThreeRoomOsdMap["tex1_640x480_dd871c5161900d69_5"] = "R50601 ";
  reThreeRoomOsdMap["tex1_640x480_34e728aea4e761f3_5"] = "R50602 ";
  reThreeRoomOsdMap["tex1_640x480_377f8f2aa8dc2cc6_5"] = "R50603 ";
  reThreeRoomOsdMap["tex1_640x480_c5687e50852025ca_5"] = "R50604 ";
  reThreeRoomOsdMap["tex1_640x480_b62474f0816d773a_5"] = "R50605 ";
  reThreeRoomOsdMap["tex1_640x480_f00feda0b309e8f2_5"] = "R50606 ";
  reThreeRoomOsdMap["tex1_640x480_1038fbd31b90f8d8_5"] = "R50607 ";
  reThreeRoomOsdMap["tex1_640x480_c211cf6a288fea05_5"] = "R50608 ";
  reThreeRoomOsdMap["tex1_640x480_52166095295cfc45_5"] = "R50700 ";
  reThreeRoomOsdMap["tex1_640x480_6f403a7287528d6b_5"] = "R50701 ";
  reThreeRoomOsdMap["tex1_640x480_faa8b992ac05ed6e_5"] = "R50702 ";
  reThreeRoomOsdMap["tex1_640x480_9be3a539157122a0_5"] = "R50703 ";
  reThreeRoomOsdMap["tex1_640x480_22bd055576765ba7_5"] = "R50704 ";
  reThreeRoomOsdMap["tex1_640x480_5e74bfb28b1c9a5c_5"] = "R50800 ";
  reThreeRoomOsdMap["tex1_640x480_d2c4814e7e4fee20_5"] = "R50801 ";
  reThreeRoomOsdMap["tex1_640x480_aff1c6292e5e7f05_5"] = "R50801 ";
  reThreeRoomOsdMap["tex1_640x480_edb807be43a21de4_5"] = "R50802 ";
  reThreeRoomOsdMap["tex1_640x480_b7c274f99b5e072f_5"] = "R50803 ";
  reThreeRoomOsdMap["tex1_640x480_b5d5aa181e89afba_5"] = "R50804 ";
  reThreeRoomOsdMap["tex1_640x480_2c85655aaa3c5f12_5"] = "R50805 ";
  reThreeRoomOsdMap["tex1_640x480_7da1e0c154293ba1_5"] = "R50806 ";
  reThreeRoomOsdMap["tex1_640x480_d215b0233ad3e0c7_5"] = "R50807 ";
  reThreeRoomOsdMap["tex1_640x480_0d865add62ac7fab_5"] = "R50808 ";
  reThreeRoomOsdMap["tex1_640x480_ffe2b1eb9e30755c_5"] = "R50809 ";
  reThreeRoomOsdMap["tex1_640x480_472e0e977dbf06bb_5"] = "R5080A ";
  reThreeRoomOsdMap["tex1_640x480_aff1c6292e5e7f05_5"] = "R5080B ";
  reThreeRoomOsdMap["tex1_640x480_c68e049219178c1a_5"] = "R5080C ";
  reThreeRoomOsdMap["tex1_640x480_607f6b5e084766d1_5"] = "R5080D ";
  reThreeRoomOsdMap["tex1_640x480_589f7fa7f2daf352_5"] = "R50900 ";
  reThreeRoomOsdMap["tex1_640x480_5adeae8150594270_5"] = "R50901 ";
  reThreeRoomOsdMap["tex1_640x480_b9692037af43dd7c_5"] = "R50902 ";
  reThreeRoomOsdMap["tex1_640x480_0267146624887b51_5"] = "R50903 ";
  reThreeRoomOsdMap["tex1_640x480_98e3c0b9233fd2b0_5"] = "R50904 ";
  reThreeRoomOsdMap["tex1_640x480_d8baafaf1000e507_5"] = "R50905 ";
  reThreeRoomOsdMap["tex1_640x480_275c1375f9a92381_5"] = "R50906 ";
  reThreeRoomOsdMap["tex1_640x480_511d711b1b6f6260_5"] = "R50907 ";
  reThreeRoomOsdMap["tex1_640x480_6916d2d67dd37d46_5"] = "R50908 ";
  reThreeRoomOsdMap["tex1_640x480_497f626d2c499c31_5"] = "R50909 ";
  reThreeRoomOsdMap["tex1_640x480_fcaf4d58654b3ba5_5"] = "R5090A ";
  reThreeRoomOsdMap["tex1_640x480_4b5b687cf761ae3f_5"] = "R5090B ";
  reThreeRoomOsdMap["tex1_640x480_ad90b795249125ba_5"] = "R5090C ";
  reThreeRoomOsdMap["tex1_640x480_87343d69521416c7_5"] = "R5090D ";
  reThreeRoomOsdMap["tex1_640x480_5243b8c4bfe816f6_5"] = "R5090E ";
  reThreeRoomOsdMap["tex1_640x480_1757784c5b8adb27_5"] = "R5090F ";
  reThreeRoomOsdMap["tex1_640x480_195ce962af6e8359_5"] = "R50910 ";
  reThreeRoomOsdMap["tex1_640x480_b192038c5be2887a_5"] = "R50911 ";
  reThreeRoomOsdMap["tex1_640x480_2c8e917928cb3e77_5"] = "R50912 ";
  reThreeRoomOsdMap["tex1_640x480_b1c076b785b8b750_5"] = "R50A00 ";
  reThreeRoomOsdMap["tex1_640x480_285fd0dc9de006dc_5"] = "R50A01 ";
  reThreeRoomOsdMap["tex1_640x480_13d0e3521cb93446_5"] = "R50A02 ";
  reThreeRoomOsdMap["tex1_640x480_a868d3ebb342a477_5"] = "R50A03 ";
  reThreeRoomOsdMap["tex1_640x480_a1b7be69574ed2a2_5"] = "R50A04 ";
  reThreeRoomOsdMap["tex1_640x480_29a5f201660cbb4d_5"] = "R50A05 ";
  reThreeRoomOsdMap["tex1_640x480_531c7b186c375164_5"] = "R50A07 ";
  reThreeRoomOsdMap["tex1_640x480_c75649b72c984c3b_5"] = "R50A08 ";
  reThreeRoomOsdMap["tex1_640x480_81b2c22a7eed7337_5"] = "R50A09 ";
  reThreeRoomOsdMap["tex1_640x480_28144df9a78e9223_5"] = "R50A0A ";
  reThreeRoomOsdMap["tex1_640x480_7a65c0dd1957dc7d_5"] = "R50A0B ";
  reThreeRoomOsdMap["tex1_640x480_a1b7be69574ed2a2_5"] = "R50A0C ";
  reThreeRoomOsdMap["tex1_640x480_aceff488f26ff1ba_5"] = "R50A0D ";
  reThreeRoomOsdMap["tex1_640x480_2f77cf65ed5675eb_5"] = "R50A0E ";
  reThreeRoomOsdMap["tex1_640x480_b8bb9af3e14c5c47_5"] = "R50A0F ";
  reThreeRoomOsdMap["tex1_640x480_bd5f1bdcf8be3b60_5"] = "R50A10 ";
  reThreeRoomOsdMap["tex1_640x480_f4e639ed24968a3b_5"] = "R50A11 ";
  reThreeRoomOsdMap["tex1_640x480_98bf9f138f2e0ace_5"] = "R50A12 ";
  reThreeRoomOsdMap["tex1_640x480_15212b85808b318a_5"] = "R50A13 ";
  reThreeRoomOsdMap["tex1_640x480_5044bdea773680b3_5"] = "R50B00 ";
  reThreeRoomOsdMap["tex1_640x480_dfacffcf2cf49a78_5"] = "R50B01 ";
  reThreeRoomOsdMap["tex1_640x480_409cbff7229aad5c_5"] = "R50B02 ";
  reThreeRoomOsdMap["tex1_640x480_76a11352dcca0e62_5"] = "R50B03 ";
  reThreeRoomOsdMap["tex1_640x480_a128cff00fac3ff4_5"] = "R50C00 ";
  reThreeRoomOsdMap["tex1_640x480_31db7688b2d2291c_5"] = "R50C01 ";
  reThreeRoomOsdMap["tex1_640x480_d21e15b8fae07863_5"] = "R50C02 ";
  reThreeRoomOsdMap["tex1_640x480_98f5eb033b20b997_5"] = "R50C03 ";
  reThreeRoomOsdMap["tex1_640x480_03daf01b533199ba_5"] = "R50C04 ";
  reThreeRoomOsdMap["tex1_640x480_1f986bc4b29dc581_5"] = "R50D00 ";
  reThreeRoomOsdMap["tex1_640x480_6f7df646b4c7b71b_5"] = "R50D01 ";
  reThreeRoomOsdMap["tex1_640x480_c3115c7a1cd345e8_5"] = "R50D02 ";
  reThreeRoomOsdMap["tex1_640x480_e7a582c158da3195_5"] = "R50D03 ";
  reThreeRoomOsdMap["tex1_640x480_13fd82539f22bed0_5"] = "R50D04 ";
  reThreeRoomOsdMap["tex1_640x480_7127cbc5875527e2_5"] = "R50D05 ";
  reThreeRoomOsdMap["tex1_640x480_7267559134a6a41f_5"] = "R50D06 ";
  reThreeRoomOsdMap["tex1_640x480_7f1306d1ac84878a_5"] = "R50D07 ";
  reThreeRoomOsdMap["tex1_640x480_f1544442be545edf_5"] = "R50D08 ";
  reThreeRoomOsdMap["tex1_640x480_6a9649890c3506cc_5"] = "R50D09 ";
  reThreeRoomOsdMap["tex1_640x480_e87b1a75eb39a2a0_5"] = "R50D0A ";
  reThreeRoomOsdMap["tex1_640x480_42441f7bf8904fd6_5"] = "R50D0B ";
  reThreeRoomOsdMap["tex1_640x480_5cf6d1c09e87c53d_5"] = "R50D0C ";
  reThreeRoomOsdMap["tex1_640x480_0b4da3bdeaac2445_5"] = "R50D0D ";
  reThreeRoomOsdMap["tex1_640x480_c790cd3cef36a28b_5"] = "R50D0E ";
  reThreeRoomOsdMap["tex1_640x480_f0fe1a135146d804_5"] = "R50D0F ";
  reThreeRoomOsdMap["tex1_640x480_74d02b16e77f56b6_5"] = "R50D10 ";
  reThreeRoomOsdMap["tex1_640x480_918b20fbbc5a3a1a_5"] = "R50D11 ";
  reThreeRoomOsdMap["tex1_640x480_e7e8ba32dba09d12_5"] = "R50D12 ";
  reThreeRoomOsdMap["tex1_640x480_bfc3a73250294548_5"] = "R50D13 ";
  reThreeRoomOsdMap["tex1_640x480_1d03a69884678c34_5"] = "R50D14 ";
  reThreeRoomOsdMap["tex1_640x480_c462d6d194be5086_5"] = "R50D15 ";
  reThreeRoomOsdMap["tex1_320x240_7ec81ed1730962ef_5"] = "R50D16 ";
  reThreeRoomOsdMap["tex1_640x480_09ab26fd2f17c8b3_5"] = "R50D17 ";
  reThreeRoomOsdMap["tex1_640x480_f01a6b137230e53b_5"] = "R50D18 ";
  reThreeRoomOsdMap["tex1_640x480_1e09c1fef924ab5a_5"] = "R50D19 ";
  reThreeRoomOsdMap["tex1_640x480_ca3476220809f2c2_5"] = "R50D1A ";
  reThreeRoomOsdMap["tex1_640x480_fd681fb5ed3bec62_5"] = "R50D1B ";
  reThreeRoomOsdMap["tex1_640x480_5323e95244d213f4_5"] = "R50D1C ";
  reThreeRoomOsdMap["tex1_640x480_010471c24d22f35f_5"] = "R50E00 ";
  reThreeRoomOsdMap["tex1_640x480_0749838095210ee8_5"] = "R50E01 ";
  reThreeRoomOsdMap["tex1_640x480_78a3595008eb675f_5"] = "R50E02 ";
  reThreeRoomOsdMap["tex1_640x480_f5149e9e67a8df89_5"] = "R50E03 ";
  reThreeRoomOsdMap["tex1_640x480_dafe2d8b8509afe1_5"] = "R50E05 ";
  reThreeRoomOsdMap["tex1_640x480_284608a4586552aa_5"] = "R50F00 ";
  reThreeRoomOsdMap["tex1_640x480_f3c1cdf074af4dc6_5"] = "R51000 ";
  reThreeRoomOsdMap["tex1_640x480_0a95a056770f5c57_5"] = "R51001 ";
  reThreeRoomOsdMap["tex1_640x480_50e7f09d7cc32e83_5"] = "R51002 ";
  reThreeRoomOsdMap["tex1_640x480_08ba6515ac4d86bc_5"] = "R51003 ";
  reThreeRoomOsdMap["tex1_640x480_1001de856686a3ad_5"] = "R51004 ";
  reThreeRoomOsdMap["tex1_640x480_b16f88a68ee5b8b3_5"] = "R51005 ";
  reThreeRoomOsdMap["tex1_640x480_4076fbe841a42ac3_5"] = "R51006 ";
  reThreeRoomOsdMap["tex1_640x480_74950137cbd93bc5_5"] = "R51007 ";
  reThreeRoomOsdMap["tex1_640x480_821b6b0297f7523b_5"] = "R51008 ";
  reThreeRoomOsdMap["tex1_640x480_c971286bb12232c7_5"] = "R51009 ";
  reThreeRoomOsdMap["tex1_640x480_7c0d03cdde39fa51_5"] = "R5100B ";
  reThreeRoomOsdMap["tex1_640x480_af6fad03cf8b1b91_5"] = "R5100C ";
  reThreeRoomOsdMap["tex1_640x480_83602711e9c93369_5"] = "R5100D ";
  reThreeRoomOsdMap["tex1_640x480_aae78399ebc7c841_5"] = "R5100E ";
  reThreeRoomOsdMap["tex1_640x480_0f5c24fb9be22ba8_5"] = "R5100F ";
  reThreeRoomOsdMap["tex1_640x480_06c82f48853f44d6_5"] = "R51010 ";
  reThreeRoomOsdMap["tex1_640x480_e77df58f6e035574_5"] = "R51011 ";
  reThreeRoomOsdMap["tex1_640x480_2d9e2c027d6bdf5b_5"] = "R51012 ";
  reThreeRoomOsdMap["tex1_640x480_a20c39c9a34aad95_5"] = "R51013 ";
  reThreeRoomOsdMap["tex1_640x480_c354cfc399fc54ef_5"] = "R51015 ";
} */
