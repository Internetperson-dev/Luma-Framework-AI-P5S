#define GAME_PERSONA_5_STRIKERS 1

// P5S (Katana engine) creates a throwaway Direct3D 9 device on boot before it ever
// reaches its Direct3D 11 device (visible in ReShade.log as "Direct3DCreate9" +
// "IDirect3D9::CreateDevice" ~2s before "D3D11CreateDevice"). Luma only supports
// DX11, so this flag is what makes it skip the D3D9 device instead of asserting.
#define CHECK_GRAPHICS_API_COMPATIBILITY 1

// NOTE: deliberately NOT enabling:
// - ENABLE_GAME_PIPELINE_STATE_READBACK: it only exists so *other* mods can read back
//   states Luma changed. There is nothing to read back yet (no shader hashes), so it
//   just adds a per-draw hook surface to a port that isn't stable yet.
// - force_upgrade_linear_samplers / enable_samplers_upgrade: P5R needs a runtime code
//   patch to make sampler upgrades behave (see its "PatchSamplerStates"). Do not assume
//   this engine tolerates them unmodified.

#include "..\..\Core\core.hpp"

namespace
{
   // ---------------------------------------------------------------------------------
   // Shader hashes for this engine are NOT known yet.
   //
   // Bringing up a new game, step by step:
   //
   //   1. DUMP. Build "Development-Release|x64". That turns on ALLOW_SHADERS_DUMPING
   //      (core.hpp:150) and "auto_dump" defaults to it (core.hpp:388), so every shader
   //      the game creates is written as "0x<HASH8>.<type>_<major>_<minor>.cso" (+ a
   //      ".meta" with resource reflections, DEVELOPMENT only).
   //      -> Dumps land in "<folder of the game exe>\Luma\Persona 5 Strikers\Dump\"
   //         (GetShadersRootPath, core.hpp:1043). CI passes REMOTE_BUILD=1, which
   //         disables the local-only fallback to this repo's "Shaders" folder
   //         (core.hpp:1065), so on a CI build that is always the location above. Copy
   //         the dumps out of there afterwards; they are never shipped (Scripts/package.ps1
   //         skips "Dump*" folders and all ".cso" files).
   //         Do NOT bother with the "ShadersPath" ReShade.ini key: it is only honoured
   //         when the path is NOT an existing populated directory (core.hpp:1059), so it
   //         cannot redirect dumps into a populated repo "Shaders" folder.
   //         Dumps are safe from being cleaned: CleanShadersCache only deletes "0x*"
   //         .cso/.meta sitting directly in "Global\" or "Persona 5 Strikers\" (it goes
   //         through IsValidShadersSubPath, core.hpp:1114), never inside "Dump\".
   //
   //   2. IDENTIFY. In the ImGui dev UI, the "Captured Commands" tab lists every draw
   //      with its shader hash and the RTV/SRV/CB it binds, including their sizes and
   //      formats; the "Disassembly"/"HLSL" tabs confirm what a shader actually does.
   //      Useful tells: a swapchain copy draws to an RTV at swapchain resolution in the
   //      swapchain format while sampling scene colour; an encode pass writes the post
   //      process chain into the swapchain; the UI draws after that from a font/atlas SRV.
   //
   //   3. REPLACE. Write your replacement as an .hlsl file named
   //      "<EffectName>_0x<HASH8>.<type>_<major>_<minor>.hlsl" directly in
   //      "Shaders\Persona 5 Strikers\" (naming parsed at core.hpp:1849-1912; the hash
   //      must be exactly 8 hex digits). THESE are the files that get packaged.
   //      Prefer naming the file with the literal "########" and declaring
   //        redirected_shader_hashes["<EffectName>"] = { "AABBCCDD", "11223344", ... };
   //      in DllMain instead (core.hpp:598). That is the convention ~10 other game mods
   //      use (Heavy Rain, Watch Dogs 2, INSIDE, Thumper...): one shader can serve
   //      several hashes, and the pass keeps a readable name instead of a magic number.
   //
   //   4. LIST. Only if a pass needs game-specific handling (extra constants, a custom
   //      swapchain copy, etc) do the hash lists below matter, via the DllMain
   //      .emplace() calls.
   //
   // IMPORTANT: leave these EMPTY until then. Do not put placeholder values in them.
   // "0" is a legal shader hash (see the comment on CachedPipeline::shader_hashes in
   // includes/shaders.h), so a "harmless" 0x00000000 is a real match candidate, not a
   // no-op -- it can silently trigger pass logic on an unrelated draw.
   // ---------------------------------------------------------------------------------
   ShaderHashesList shader_hashes_SwapchainCopy;
   ShaderHashesList shader_hashes_PostProcessEncode;
   ShaderHashesList shader_hashes_Sky;

#if DEVELOPMENT
   // Logs which of this mod's shader hash lists are still unidentified, so the port's
   // remaining work is visible from ReShade.log without reading the source.
   void LogUnidentifiedShaderHashes()
   {
      // Generic on the list type on purpose: "shader_hashes_UI" is a core global declared as
      // ShaderHashesList<Multiple, Graphics>, which is a different type from ours.
      auto Report = [](const char* name, const auto& list)
      {
         if (list.Empty())
         {
            char line[160];
            std::snprintf(line, sizeof(line), "[P5S] No shader hashes identified yet for \"%s\".", name);
            reshade::log::message(reshade::log::level::info, line);
         }
      };
      Report("SwapchainCopy", shader_hashes_SwapchainCopy);
      Report("PostProcessEncode", shader_hashes_PostProcessEncode);
      Report("Sky", shader_hashes_Sky);
      Report("UI", shader_hashes_UI);
   }
#endif // DEVELOPMENT
} // namespace

struct Persona5StrikersDeviceData final : public GameDeviceData
{
};

class Persona5Strikers final : public Game
{
public:
   void OnInit(bool async) override
   {
#if DEVELOPMENT
      // Deliberately here rather than in "DllMain": this is the earliest point where
      // ReShade is guaranteed to have finished setting up, and no mod in this repository
      // logs from "DllMain". Doing it at load time risks a second add-on load failure
      // (ReShade reports those only as "Failed to load add-on ... with error code 998"),
      // and this mod had already lost exactly one load to that.
      LogUnidentifiedShaderHashes();
#endif // DEVELOPMENT

      // NOTE: the pipeline descriptions below are UNVERIFIED assumptions carried over
      // from the engine docs, not measured from the game. They only start to matter once
      // this mod ships real shaders, since that is what consumes them. Re-derive them
      // from your shader dump before trusting the output.
      GetShaderDefineData(POST_PROCESS_SPACE_TYPE_HASH).SetDefaultValue('0'); // Is the swapchain / post processing linear?
      GetShaderDefineData(EARLY_DISPLAY_ENCODING_HASH).SetDefaultValue('0');  // Gamma + paper white applied during post processing, or deferred to display composition?
      GetShaderDefineData(VANILLA_ENCODING_TYPE_HASH).SetDefaultValue('0');   // Which SDR transfer curve did the game use?
      GetShaderDefineData(GAMMA_CORRECTION_TYPE_HASH).SetDefaultValue('1');   // Which SDR transfer curve should we emulate?
      GetShaderDefineData(UI_DRAW_TYPE_HASH).SetDefaultValue('0');            // How does the UI draw in? ("2" implies a separate UI render target, don't claim that yet)

      // Cbuffer slots must be 0-13, or -1 to not set them at all. All valid ones must differ.
      luma_settings_cbuffer_index = 13;
      luma_data_cbuffer_index = 12;
      luma_ui_cbuffer_index = -1; // Only meaningful for UI_DRAW_TYPE 1.

      // Mirrored in "Shaders\Persona 5 Strikers\Includes\GameCBuffers.hlsl".
      default_luma_global_game_settings.GameSetting01 = cb_luma_global_settings.GameSettings.GameSetting01 = 0.5f;
      default_luma_global_game_settings.GameSetting02 = cb_luma_global_settings.GameSettings.GameSetting02 = 33;
   }

   void OnCreateDevice(ID3D11Device* native_device, DeviceData& device_data) override
   {
      device_data.game = new Persona5StrikersDeviceData;
   }

   DrawOrDispatchOverrideType OnDrawOrDispatch(ID3D11Device* native_device, ID3D11DeviceContext* native_device_context, CommandListData& cmd_list_data, DeviceData& device_data, reshade::api::shader_stage stages, const ShaderHashesList<OneShaderPerPipeline>& original_shader_hashes, bool is_custom_pass, bool& updated_cbuffers, std::function<void()>* original_draw_dispatch_func) override
   {
      // Nothing game-specific to do yet: this only gains meaning once the shader hashes
      // above are known. The template's pattern for it is in "_Template\main.cpp".
      return DrawOrDispatchOverrideType::None; // Don't cancel the original draw call
   }

   void OnPresent(ID3D11Device* native_device, DeviceData& device_data) override
   {
   }

   void LoadConfigs() override
   {
      reshade::api::effect_runtime* runtime = nullptr;
      reshade::get_config_value(runtime, NAME, "GameSetting01", cb_luma_global_settings.GameSettings.GameSetting01);
      reshade::get_config_value(runtime, NAME, "GameSetting02", cb_luma_global_settings.GameSettings.GameSetting02);
   }

   void DrawImGuiSettings(DeviceData& device_data) override
   {
      reshade::api::effect_runtime* runtime = nullptr;
      if (ImGui::SliderFloat("Game Setting #01", &cb_luma_global_settings.GameSettings.GameSetting01, 0.f, 1.f, "%.3f"))
         reshade::set_config_value(runtime, NAME, "GameSetting01", cb_luma_global_settings.GameSettings.GameSetting01);
      if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
      {
         ImGui::SetTooltip("Hi! This changes Game Setting #01.");
      }
      DrawResetButton(cb_luma_global_settings.GameSettings.GameSetting01, default_luma_global_game_settings.GameSetting01, "GameSetting01", runtime);

      uint GameSetting02_min = 0;
      uint GameSetting02_max = 50;
      if (ImGui::SliderScalar("Game Setting #02", ImGuiDataType_U32, &cb_luma_global_settings.GameSettings.GameSetting02, &GameSetting02_min, &GameSetting02_max, "%u"))
         reshade::set_config_value(runtime, NAME, "GameSetting02", cb_luma_global_settings.GameSettings.GameSetting02);
      if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
      {
         ImGui::SetTooltip("Hi! This changes Game Setting #02.");
      }
      DrawResetButton(cb_luma_global_settings.GameSettings.GameSetting02, default_luma_global_game_settings.GameSetting02, "GameSetting02", runtime);
   }

   void PrintImGuiAbout() override
   {
      ImGui::Text("Luma for \"Persona 5 Strikers\" - built on Luma Framework.\n\nCredits:\nLuma Framework contributors\n\nStatus: work in progress. This mod currently runs Luma's shared pipeline only: no shader from this engine has been identified or replaced yet, so there is nothing game-specific to show. Boot and crash reports on an unlisted game are welcome.");
   }
};

BOOL APIENTRY DllMain(HMODULE hModule, DWORD ul_reason_for_call, LPVOID lpReserved)
{
   if (ul_reason_for_call == DLL_PROCESS_ATTACH)
   {
      const char* project_name = PROJECT_NAME;
      const char* cleared_project_name = (project_name[0] == '_') ? (project_name + 1) : project_name;

      // NOTE: pass a real string for the website, not "nullptr". "SetGlobals" strncpy's it
      // unconditionally, and a null pointer here is an access violation inside "DllMain",
      // which ReShade reports only as "Failed to load add-on ... error code 998".
      Globals::SetGlobals(cleared_project_name, "Persona 5 Strikers Luma mod", "", 1);
      Globals::DEVELOPMENT_STATE = Globals::ModDevelopmentState::WorkInProgress;

      // ==============================================================================
      // Bring-up baseline.
      //
      // This is deliberately the most conservative thing that still boots: every value
      // below is either the core default or explicitly left at it, so there is nothing
      // for the game to trip over. On top of that:
      //
      //   - "swapchain_upgrade_type" is deliberately NOT set. Its core default is already
      //     SwapchainUpgradeType::scRGB (core.hpp:473), so setting it changes nothing.
      //     The line that actually matters is "swapchain_format_upgrade_type" below.
      //   - "swapchain_format_upgrade_type" and "texture_format_upgrades_type" both
      //     default to TextureFormatUpgradesType::None and are left that way. Letting
      //     them be AllowedEnabled makes Luma rewrite the backbuffer to R16G16B16A16_FLOAT
      //     (core.hpp:3141) and upgrade render target formats mid-flight. That is the most
      //     likely reason an earlier revision of this file died right after
      //     "IDXGIFactory::CreateSwapChain" in ReShade.log.
      //
      // Re-enable them ONE AT A TIME, testing the game boots between each, in this order:
      //
      //   1. swapchain_format_upgrade_type = TextureFormatUpgradesType::AllowedEnabled;
      //      -> Luma HDR output. Expect a visible change; if it crashes, this engine
      //         needs the swapchain recreated in a specific way (check ReShade.log for how
      //         the game recreates its runtime environment).
      //   2. texture_format_upgrades_type = TextureFormatUpgradesType::AllowedEnabled;
      //      texture_upgrade_formats = { reshade::api::format::r11g11b10_float };
      //      -> 11-bit float internal render targets. Only widen this list once the
      //         game's actual post-processing format is known.
      //   3. Once the post-process/swapchain hashes above are known, upgrade the
      //      post-processing targets rather than every RT in the game.
      //
      // texture_format_upgrades_2d_size_filters is also left at its default; only add
      // TextureFormatUpgrades2DSizeFilters::No1Px if legitimate upgrades are being missed.
      // ==============================================================================

      force_ignore_dpi = false;

      enable_samplers_upgrade = false; // Can't be changed after boot (core.hpp:577)

      // Shader hashes go here, once identified from a shader dump. Two separate things:
      //
      // (a) Mapping a name to the hashes that use that shader, so a single .hlsl file
      //     named "<Name>_0x########.<type>_<major>_<minor>.hlsl" can serve them all:
      //       redirected_shader_hashes["Tonemap"] = { "AABBCCDD", "11223344" };
      //     This is the usual way passes are declared (see Heavy Rain, Watch Dogs 2,
      //     INSIDE, Thumper, ...). Only fill it in for passes you actually replace.
      //
      // (b) The hash lists above, but ONLY for passes needing game-specific work in
      //     OnDrawOrDispatch (own constants, a custom swapchain copy, ...):
      //       shader_hashes_SwapchainCopy.pixel_shaders.emplace(std::stoul("XXXXXXXX", nullptr, 16));
      //       shader_hashes_PostProcessEncode.pixel_shaders.emplace(std::stoul("XXXXXXXX", nullptr, 16));
      //       shader_hashes_Sky.pixel_shaders.emplace(std::stoul("XXXXXXXX", nullptr, 16));
      //       shader_hashes_UI.pixel_shaders.emplace(std::stoul("XXXXXXXX", nullptr, 16));
      //     ("XXXXXXXX" is a placeholder for the 8 hex digits, upper case, no "0x".)
      // (UI_DRAW_TYPE 2 also needs "ui_separation_format" set to a concrete DXGI_FORMAT.)

#if DEVELOPMENT
      // Pin names to known hashes so they stay readable in the ImGui pipeline views:
      //   forced_shader_names.emplace(std::stoul("XXXXXXXX", nullptr, 16), "Swapchain Copy");
#endif // DEVELOPMENT

      game = new Persona5Strikers();
   }

   return CoreMain(hModule, ul_reason_for_call, lpReserved);
}