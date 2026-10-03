///////////////////////////////////////////////////////////////////////
// Persona 5 Strikers (Omega Force / P-Studio, 2021) Luma mod - initial bring-up.
//
// STATUS: work in progress. This is a first, untested pass at the game.
//
// The game runs on Koei Tecmo's "Katana Engine", shared with Omega Force and
// Team Ninja titles such as Nioh, Dynasty Warriors, Rise of the Ronin, Blue
// Reflection: Second Light, Fate/Samurai Remnant, ... Reference those Katana
// engine mods in this repo instead - in particular "Source\Games\Nioh" (compact,
// same approach as here: generic HDR output plus hash based game shader
// replacement) and "Source\Games\Blue Reflection Second Light" (an extensive
// example of everything a Katana engine integration can hook into).
//
// What this mod does right now (all engine agnostic, it just works because the
// game is plain DX11):
//  - Upgrades the swapchain to scRGB and upgrades the SDR render target formats
//    to float, then lets Luma do its own display composition pass on the back
//    buffer. That gives an HDR (10+ bit, no banding, display peak mapped) image
//    out of the box, which is the whole point of running the mod on a modern
//    OLED/HDR display.
//  - Nothing else yet. See the TODOs below.
//
// To continue the port you need the game and a development (or test) build of
// this mod: the game's shaders are dumped to ".\Shaders\Persona 5 Strikers" on
// load, named after their hash. Identify the passes with the mod's draw call /
// shader tracking tools (Home -> Luma -> Advanced/Dev), then replace them one by
// one, the same way the Nioh mod does.
///////////////////////////////////////////////////////////////////////

#define GAME_PERSONA_5_STRIKERS 1

// NOTE: "ALLOW_SHADERS_DUMPING" is deliberately left to its default value
// (enabled in development and test builds, disabled in publishing ones). The
// Katana engine's post processing shaders haven't been identified/replaced yet,
// so dumping them is how they'll be found: build this project in a development
// (or test) configuration, run the game, and the shaders will be written to
// ".\Shaders\Persona 5 Strikers" as decompiled HLSL, named after their hash
// (e.g. "Tonemap_0x12345678.ps_5_0.hlsl"). Once the replacements are in place,
// define "ALLOW_SHADERS_DUMPING 0" to drop the shader decompiler requirement
// and the load time cost from publishing builds.

// Don't pop up modal message boxes on assert failures: they'd steal focus from
// the game and make it lose input (the messages go to the in game overlay log
// instead).
#define AVOID_INPUT_LOSS 1

// An unfinished core feature that some games opt out of (as "Nioh" and the
// generic mod do), disabled while it's being reworked.
#define DISABLE_FOCUS_LOSS_SUPPRESSION 1

// Let Luma make sure the game is on DX11 (some engines try to create a DX9
// device first and fall back, which ReShade doesn't like).
#define CHECK_GRAPHICS_API_COMPATIBILITY 1

#include "..\..\Core\core.hpp"

namespace
{
   // TODO (P5S): once the post processing passes are identified, add their
   // hashes here and use them in Persona5Strikers::OnDrawOrDispatch. For
   // reference, the Nioh mod (same engine) tracks at least:
   //   - the final swapchain copy
   //   - the post processing "encode" pass (where Luma can scale the back
   //     buffer to the real output resolution - the game, like other Katana
   //     engine games, hardcodes its UI/backbuffer to 16:9 and has no native
   //     ultrawide support)
   //   - the tonemap, color grading and bloom blur passes
   //   - the sky pass and the start/end of the material (gbuffer) drawing
   //     phase, which is where sampler upgrades can be applied selectively
   [[maybe_unused]] ShaderHashesList shader_hashes_SwapchainCopy;
   [[maybe_unused]] ShaderHashesList shader_hashes_PostProcessEncode;
}

struct Persona5StrikersGameDeviceData final : public GameDeviceData
{
   // TODO (P5S): per frame state goes here (e.g. whether the main post
   // processing was seen this frame, upgraded render target views, ...).
};

class Persona5Strikers final : public Game // Go view "Source/Core/includes/game.h" for all the overridable virtual functions
{
   static Persona5StrikersGameDeviceData& GetGameDeviceData(DeviceData& device_data)
   {
      return *static_cast<Persona5StrikersGameDeviceData*>(device_data.game);
   }

public:
   void OnInit(bool async) override
   {
      // TODO (P5S): add the game's own shader defines once its post processing
      // shaders are replaced (see the Nioh mod for a good example: the Katana
      // engine's tonemap/grading/bloom shaders are very similar between games).
      //
      // Note that the core defaults for "POST_PROCESS_SPACE_TYPE",
      // "VANILLA_ENCODING_TYPE" and "GAMMA_CORRECTION_TYPE" already match what
      // the Nioh mod uses (SDR gamma space post processing, sRGB encoded by the
      // game, emulated as Gamma 2.2), which is a good guess for this engine
      // family - but they should be confirmed against a dumped shader.
      //
      // These get uploaded to the GPU on every pass Luma overrides. They must
      // not collide with a slot the game itself uses.
      luma_settings_cbuffer_index = 13;
      luma_data_cbuffer_index = 12;
   }

   void OnCreateDevice(ID3D11Device* native_device, DeviceData& device_data) override
   {
      device_data.game = new Persona5StrikersGameDeviceData;
   }

   DrawOrDispatchOverrideType OnDrawOrDispatch(ID3D11Device* native_device, ID3D11DeviceContext* native_device_context, CommandListData& cmd_list_data, DeviceData& device_data, reshade::api::shader_stage stages, const ShaderHashesList<OneShaderPerPipeline>& original_shader_hashes, bool is_custom_pass, bool& updated_cbuffers, std::function<void()>* original_draw_dispatch_func) override
   {
      // TODO (P5S): this is where the game's passes get tracked and hooked
      // (see the Nioh mod's handling of the backbuffer resolution and of the
      // 2D UI drawn after the post processing). Nothing to do for now: without
      // any replaced game shader, Luma's own display composition pass is enough
      // to get a correct HDR image.

      return DrawOrDispatchOverrideType::None; // Don't cancel the original draw call
   }

   void OnPresent(ID3D11Device* native_device, DeviceData& device_data) override
   {
      // TODO (P5S): release the resources grabbed from the game each frame here.
   }

   void LoadConfigs() override
   {
      // TODO (P5S): load the game's own settings from ReShade.ini here.
   }

   void DrawImGuiSettings(DeviceData& device_data) override
   {
      ImGui::NewLine();

      // The swapchain/texture format upgrades are what makes this mod do
      // anything at all, and they can't be changed at runtime as far as we know
      // (a resolution change is needed to fully apply them), so expose the same
      // toggles the generic mod does rather than hiding them.
      if (swapchain_format_upgrade_type > TextureFormatUpgradesType::None)
      {
         if (swapchain_format_upgrade_type == TextureFormatUpgradesType::AllowedEnabled ? ImGui::Button("Disable Swapchain Upgrade") : ImGui::Button("Enable Swapchain Upgrade"))
         {
            swapchain_format_upgrade_type = swapchain_format_upgrade_type == TextureFormatUpgradesType::AllowedEnabled ? TextureFormatUpgradesType::AllowedDisabled : TextureFormatUpgradesType::AllowedEnabled;
         }
         if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
         {
            ImGui::SetTooltip("Requires a resolution change (or a game restart) to fully apply.");
         }
      }
      if (texture_format_upgrades_type > TextureFormatUpgradesType::None)
      {
         if (texture_format_upgrades_type == TextureFormatUpgradesType::AllowedEnabled ? ImGui::Button("Disable Texture Format Upgrades") : ImGui::Button("Enable Texture Format Upgrades"))
         {
            texture_format_upgrades_type = texture_format_upgrades_type == TextureFormatUpgradesType::AllowedEnabled ? TextureFormatUpgradesType::AllowedDisabled : TextureFormatUpgradesType::AllowedEnabled;
         }
         if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
         {
            ImGui::SetTooltip("Requires a resolution change (or a game restart) to fully apply.");
         }
      }
   }

   void PrintImGuiAbout() override
   {
      ImGui::Text("Persona 5 Strikers Luma mod - about and credits section", "");
      ImGui::Text("Work in progress! Only the generic HDR output is implemented so far,\n"
                  "the game's own post processing hasn't been replaced yet.\n\n"
                  "Built on Luma (https://github.com/Filoppi/Luma-Framework),\n"
                  "which is based on the ReShade Addon system.\n\n"
                  "Credits:\n"
                  "Luma: Pumbo and contributors\n"
                  "ReShade: crosire and contributors", "");
   }
};

BOOL APIENTRY DllMain(HMODULE hModule, DWORD ul_reason_for_call, LPVOID lpReserved)
{
   if (ul_reason_for_call == DLL_PROCESS_ATTACH)
   {
      // Increase this to reset the game settings and shader binaries after making large changes to the mod
      Globals::SetGlobals(PROJECT_NAME, "Persona 5 Strikers Luma mod", nullptr /*E.g. Nexus link*/, 1);
      Globals::DEVELOPMENT_STATE = Globals::ModDevelopmentState::WorkInProgress;

      // HDR output: linear scRGB swapchain + a display composition pass at the
      // end of the frame. This is the same baseline the generic mod uses and
      // doesn't need any game specific knowledge.
      swapchain_format_upgrade_type = TextureFormatUpgradesType::AllowedEnabled;
      swapchain_upgrade_type = SwapchainUpgradeType::scRGB;
      texture_format_upgrades_type = TextureFormatUpgradesType::AllowedEnabled;
      enable_indirect_texture_format_upgrades = true; // Generally safer, as we don't know how the game creates its resources
      enable_chain_indirect_texture_format_upgrades = ChainTextureFormatUpgradesType::DirectDependencies;
      texture_upgrade_formats = {
            reshade::api::format::r8g8b8a8_unorm,
            reshade::api::format::r8g8b8a8_unorm_srgb,
            reshade::api::format::r8g8b8a8_typeless,
            reshade::api::format::r8g8b8x8_unorm,
            reshade::api::format::r8g8b8x8_unorm_srgb,
            reshade::api::format::b8g8r8a8_unorm,
            reshade::api::format::b8g8r8a8_unorm_srgb,
            reshade::api::format::b8g8r8a8_typeless,
            reshade::api::format::b8g8r8x8_unorm,
            reshade::api::format::b8g8r8x8_unorm_srgb,
            reshade::api::format::b8g8r8x8_typeless,

            reshade::api::format::r10g10b10a2_unorm,
            reshade::api::format::r10g10b10a2_typeless,

            reshade::api::format::r11g11b10_float,
      };
      // TODO (P5S): narrow this down once we know which of these the game
      // actually creates (see the Nioh mod, which only needed r11g11b10_float
      // and had to exclude several others on purpose).
      texture_format_upgrades_2d_size_filters = 0 | (uint32_t)TextureFormatUpgrades2DSizeFilters::SwapchainResolution | (uint32_t)TextureFormatUpgrades2DSizeFilters::SwapchainAspectRatio | (uint32_t)TextureFormatUpgrades2DSizeFilters::No1Px;

      // Make the game window match the display resolution regardless of the DPI
      // scaling, otherwise some resolutions aren't selectable (harmless if the
      // game already handles this itself).
      force_ignore_dpi = true;

      // TODO (P5S): DLSS/FSR can't be enabled yet ("UseLumaNGX"/"UseLumaFSR"
      // are off in the project properties), they need the game's motion vectors,
      // depth buffer and post processing structure to be hooked up first. The
      // game is capped at 60 FPS and renders internally well below its output
      // resolution, so this is where most of the remaining value is.
      //
      // Related: the Katana engine bakes a resolution dependent mip bias into its
      // samplers (in Nioh Luma works around it from c++), so the sampler upgrade
      // path needs revisiting once the materials drawing phase is tracked.
      enable_samplers_upgrade = false;

      game = new Persona5Strikers();
   }

   CoreMain(hModule, ul_reason_for_call, lpReserved);

   return TRUE;
}
