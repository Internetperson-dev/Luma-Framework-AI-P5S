#define GAME_PERSONA_5_STRIKERS 1

// The game may try to create a DX9 device on boot
#define CHECK_GRAPHICS_API_COMPATIBILITY 1

// Protect against games that cache pipeline state
#define ENABLE_GAME_PIPELINE_STATE_READBACK 1

#include "..\..\Core\core.hpp"

namespace
{
    ShaderHashesList shader_hashes_Copy;
    ShaderHashesList shader_hashes_SwapchainCopy;
    ShaderHashesList shader_hashes_PostProcessEncode;
    ShaderHashesList shader_hashes_Sky;
    ShaderHashesList shader_hashes_BeginMaterialsDrawing;
    ShaderHashesList shader_hashes_EndMaterialsDrawing;

    bool first_frame_draw_call = true;
    bool playing_video = false;
    bool has_drawn_post_process = false;
    int final_post_process_copy_draws = 0;
    constexpr size_t max_final_post_process_copy_draws = 3;

    com_ptr<ID3D11DepthStencilView> main_dsv;
    com_ptr<ID3D11RenderTargetView> post_process_rtvs[max_final_post_process_copy_draws];
    com_ptr<ID3D11RenderTargetView> upgraded_post_process_rtvs[max_final_post_process_copy_draws];
    com_ptr<ID3D11ShaderResourceView> upgraded_post_process_srvs[max_final_post_process_copy_draws];
    com_ptr<ID3D11Texture2D> upgraded_post_process_textures_2d[max_final_post_process_copy_draws];

    bool upgrade_materials_samplers = true;
}

class Persona5Strikers final : public Game
{
public:
    void OnInit(bool async) override
    {
        luma_settings_cbuffer_index = 13;
        luma_data_cbuffer_index = 12;

        std::vector<ShaderDefineData> game_shader_defines_data = {
            {"IMPROVED_TONEMAPPING_TYPE", '1', true, false, "Enable modern tonemapping code combinations.", 3},
            {"IMPROVED_COLOR_GRADING_TYPE", '0', true, false, "Improves the original grading.", 3},
            {"IMPROVED_BLOOM", '1', true, false, "Reduces the overly strong bloom effect.", 1},
            {"ENABLE_HDR_COLOR_GRADING", '1', true, false, "Enables color grading in HDR space.", 1},
            {"ENABLE_SDR_COLOR_GRADING", '1', true, false, "Enables color grading after vanilla SDR tonemapping.", 1},
            {"ENABLE_HDR_BOOST", '1', true, false, "Enable a \"Fake\" HDR boosting effect (applies to videos too).", 1},
            {"ENABLE_VIGNETTE", '1', true, false, "Allows disabling the game's vignette effect.", 1},
            {"ENABLE_FXAA", '1', true, false, "Adds FXAA anti-aliasing.", 1},
        };
        shader_defines_data.append_range(game_shader_defines_data);

        GetShaderDefineData(TEST_SDR_HDR_SPLIT_VIEW_MODE_NATIVE_IMPL_HASH).SetDefaultValue('1');
        GetShaderDefineData(POST_PROCESS_SPACE_TYPE_HASH).SetDefaultValue('0');
        GetShaderDefineData(VANILLA_ENCODING_TYPE_HASH).SetDefaultValue('0');
        GetShaderDefineData(GAMMA_CORRECTION_TYPE_HASH).SetDefaultValue('1');
        GetShaderDefineData(UI_DRAW_TYPE_HASH).SetDefaultValue('2');
    }

    DrawOrDispatchOverrideType OnDrawOrDispatch(ID3D11Device* native_device, ID3D11DeviceContext* native_device_context, CommandListData& cmd_list_data, DeviceData& device_data, reshade::api::shader_stage stages, const ShaderHashesList<OneShaderPerPipeline>& original_shader_hashes, bool is_custom_pass, bool& updated_cbuffers, std::function<void()>* original_draw_dispatch_func) override
    {
        if ((stages & reshade::api::shader_stage::pixel) != 0 && first_frame_draw_call && test_index != 14)
        {
            first_frame_draw_call = false;
            if (original_shader_hashes.Contains(shader_hashes_Copy))
            {
                com_ptr<ID3D11ShaderResourceView> srv;
                native_device_context->PSGetShaderResources(0, 1, &srv);
                uint4 size;
                DXGI_FORMAT format;
                GetResourceInfo(srv.get(), size, format);
                if (size.x == 1920 && size.y == 1080 && (format == DXGI_FORMAT_B8G8R8X8_UNORM || format == DXGI_FORMAT_B8G8R8X8_TYPELESS))
                {
                    playing_video = true;
                    if (is_custom_pass)
                    {
                        SetLumaConstantBuffers(native_device_context, cmd_list_data, device_data, stages, LumaConstantBufferType::LumaSettings);
                        SetLumaConstantBuffers(native_device_context, cmd_list_data, device_data, stages, LumaConstantBufferType::LumaData, 1);
                        updated_cbuffers = true;
                    }
                }
            }
        }

        if (original_shader_hashes.Contains(shader_hashes_BeginMaterialsDrawing) && upgrade_materials_samplers)
        {
            ignore_upgraded_samplers = false;
        }
        else if (!ignore_upgraded_samplers && original_shader_hashes.Contains(shader_hashes_EndMaterialsDrawing))
        {
            ignore_upgraded_samplers = true;
            com_ptr<ID3D11SamplerState> samplers[D3D11_COMMONSHADER_SAMPLER_SLOT_COUNT];
            native_device_context->PSGetSamplers(0, D3D11_COMMONSHADER_SAMPLER_SLOT_COUNT, &samplers[0]);

            std::shared_lock shared_lock_samplers(s_mutex_samplers);
            for (uint32_t i = 0; i < D3D11_COMMONSHADER_SAMPLER_SLOT_COUNT; i++)
            {
                for (auto& custom_samplers : device_data.custom_sampler_by_original_sampler)
                {
                    const auto it = custom_samplers.second.find(device_data.texture_mip_lod_bias_offset);
                    if (it != custom_samplers.second.end())
                    {
                        ID3D11SamplerState* native_sampler = reinterpret_cast<ID3D11SamplerState*>(custom_samplers.first);
                        if (it->second != nullptr && it->second == samplers[i])
                        {
                            samplers[i] = native_sampler;
                            break;
                        }
                    }
                }
            }
            ID3D11SamplerState* const* samplers_const = (ID3D11SamplerState**)std::addressof(samplers[0]);
            native_device_context->PSSetSamplers(0, D3D11_COMMONSHADER_SAMPLER_SLOT_COUNT, samplers_const);
        }

        if (original_shader_hashes.Contains(shader_hashes_Sky))
        {
            com_ptr<ID3D11RenderTargetView> rtv;
            main_dsv = nullptr;
            native_device_context->OMGetRenderTargets(1, &rtv, &main_dsv);
        }

        if (original_shader_hashes.Contains(shader_hashes_PostProcessEncode))
        {
            has_drawn_post_process = true;
            com_ptr<ID3D11RenderTargetView> rtv;
            com_ptr<ID3D11DepthStencilView> dsv;
            native_device_context->OMGetRenderTargets(1, &rtv, &dsv);

            uint4 size;
            DXGI_FORMAT format;
            GetResourceInfo(rtv.get(), size, format);

            if (rtv.get() && rtv.get() != post_process_rtvs[final_post_process_copy_draws] &&
                (size.x != uint(device_data.output_resolution.x + 0.5f) || size.y != uint(device_data.output_resolution.y + 0.5f)))
            {
                D3D11_VIEWPORT viewports[D3D11_VIEWPORT_AND_SCISSORRECT_OBJECT_COUNT_PER_PIPELINE];
                UINT viewports_num = 1;
                native_device_context->RSGetViewports(&viewports_num, nullptr);
                native_device_context->RSGetViewports(&viewports_num, &viewports[0]);
                if (viewports_num == 1)
                {
                    com_ptr<ID3D11Resource> post_process_resource;
                    rtv->GetResource(&post_process_resource);
                    if (post_process_resource)
                    {
                        com_ptr<ID3D11Texture2D> post_process_texture_2d;
                        post_process_resource->QueryInterface(&post_process_texture_2d);
                        if (post_process_texture_2d)
                        {
                            D3D11_TEXTURE2D_DESC texture_2d_desc;
                            post_process_texture_2d->GetDesc(&texture_2d_desc);
                            texture_2d_desc.Width = uint(device_data.output_resolution.x + 0.5f);
                            texture_2d_desc.Height = uint(device_data.output_resolution.y + 0.5f);

                            D3D11_RENDER_TARGET_VIEW_DESC rtv_desc;
                            rtv->GetDesc(&rtv_desc);
                            D3D11_SHADER_RESOURCE_VIEW_DESC srv_desc;
                            srv_desc.Format = rtv_desc.Format;
                            srv_desc.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
                            srv_desc.Texture2D.MipLevels = 1;
                            srv_desc.Texture2D.MostDetailedMip = 0;

                            upgraded_post_process_textures_2d[final_post_process_copy_draws] = nullptr;
                            upgraded_post_process_rtvs[final_post_process_copy_draws] = nullptr;
                            upgraded_post_process_srvs[final_post_process_copy_draws] = nullptr;
                            native_device->CreateTexture2D(&texture_2d_desc, nullptr, &upgraded_post_process_textures_2d[final_post_process_copy_draws]);
                            native_device->CreateRenderTargetView(upgraded_post_process_textures_2d[final_post_process_copy_draws].get(), &rtv_desc, &upgraded_post_process_rtvs[final_post_process_copy_draws]);
                            native_device->CreateShaderResourceView(upgraded_post_process_textures_2d[final_post_process_copy_draws].get(), &srv_desc, &upgraded_post_process_srvs[final_post_process_copy_draws]);
                            post_process_rtvs[final_post_process_copy_draws] = rtv;
                        }
                    }
                }
            }
            if (rtv.get() && rtv.get() == post_process_rtvs[final_post_process_copy_draws] && test_index != 12)
            {
                D3D11_VIEWPORT viewport;
                viewport.TopLeftX = 0.f; viewport.TopLeftY = 0.f;
                viewport.MinDepth = 0.f; viewport.MaxDepth = 1.f;
                viewport.Width = device_data.output_resolution.x;
                viewport.Height = device_data.output_resolution.y;
                native_device_context->RSSetViewports(1, &viewport);
                native_device_context->RSSetScissorRects(0, nullptr);
                dsv = dsv ? main_dsv : nullptr;
                ID3D11RenderTargetView* upgraded_post_process_rtv_const = upgraded_post_process_rtvs[final_post_process_copy_draws].get();
                native_device_context->OMSetRenderTargets(1, &upgraded_post_process_rtv_const, dsv.get());
            }
            else
            {
                post_process_rtvs[final_post_process_copy_draws] = nullptr;
                upgraded_post_process_textures_2d[final_post_process_copy_draws] = nullptr;
                upgraded_post_process_rtvs[final_post_process_copy_draws] = nullptr;
                upgraded_post_process_srvs[final_post_process_copy_draws] = nullptr;
            }
            final_post_process_copy_draws++;
            ASSERT_ONCE(final_post_process_copy_draws <= max_final_post_process_copy_draws);
        }
        else if (has_drawn_post_process && upgraded_post_process_rtvs[final_post_process_copy_draws-1])
        {
            com_ptr<ID3D11RenderTargetView> rtv;
            com_ptr<ID3D11DepthStencilView> dsv;
            native_device_context->OMGetRenderTargets(1, &rtv, &dsv);
            if (rtv && (rtv == post_process_rtvs[final_post_process_copy_draws-1] || rtv == upgraded_post_process_rtvs[final_post_process_copy_draws-1]))
            {
                D3D11_VIEWPORT viewport;
                viewport.MinDepth = 0.f; viewport.MaxDepth = 1.f;
                viewport.TopLeftX = 0.f; viewport.TopLeftY = 0.f;
                viewport.Width = device_data.output_resolution.x;
                viewport.Height = device_data.output_resolution.y;
                native_device_context->RSSetViewports(1, &viewport);
                native_device_context->RSSetScissorRects(0, nullptr);
                dsv = dsv ? main_dsv : nullptr;
                ID3D11RenderTargetView* upgraded_post_process_rtv_const = upgraded_post_process_rtvs[final_post_process_copy_draws-1].get();
                native_device_context->OMSetRenderTargets(1, &upgraded_post_process_rtv_const, dsv.get());
            }
        }

        bool is_swapchain_copy = original_shader_hashes.Contains(shader_hashes_SwapchainCopy);
        bool is_ui = original_shader_hashes.Contains(shader_hashes_UI);
        if (is_swapchain_copy || is_ui)
        {
            if (is_swapchain_copy && is_custom_pass)
            {
                SetLumaConstantBuffers(native_device_context, cmd_list_data, device_data, stages, LumaConstantBufferType::LumaSettings);
                SetLumaConstantBuffers(native_device_context, cmd_list_data, device_data, stages, LumaConstantBufferType::LumaData, playing_video ? 1 : 0);
                updated_cbuffers = true;
            }
            int i = 0;
            if (final_post_process_copy_draws >= 2)
                i = is_ui ? 0 : (final_post_process_copy_draws-1);
            if (has_drawn_post_process && upgraded_post_process_srvs[i])
            {
                com_ptr<ID3D11ShaderResourceView> ui_srv;
                native_device_context->PSGetShaderResources(0, 1, &ui_srv);
                if (!is_ui || AreViewsOfSameResource(ui_srv.get(), post_process_rtvs[i].get()))
                {
                    ID3D11ShaderResourceView* const upgraded_post_process_srv_const = upgraded_post_process_srvs[i].get();
                    native_device_context->PSSetShaderResources(0, 1, &upgraded_post_process_srv_const);
                }
                if (is_swapchain_copy)
                {
                    D3D11_VIEWPORT viewport;
                    viewport.TopLeftX = 0.f; viewport.TopLeftY = 0.f;
                    viewport.MinDepth = 0.f; viewport.MaxDepth = 1.f;
                    viewport.Width = device_data.output_resolution.x;
                    viewport.Height = device_data.output_resolution.y;
                    native_device_context->RSSetViewports(1, &viewport);
                    native_device_context->RSSetScissorRects(0, nullptr);
                }
            }
        }

        return DrawOrDispatchOverrideType::None;
    }

    void OnPresent(ID3D11Device* native_device, DeviceData& device_data) override
    {
        if (!has_drawn_post_process)
        {
            main_dsv = nullptr;
        }

        while (final_post_process_copy_draws > 1)
        {
            post_process_rtvs[final_post_process_copy_draws-1] = nullptr;
            upgraded_post_process_textures_2d[final_post_process_copy_draws-1] = nullptr;
            upgraded_post_process_rtvs[final_post_process_copy_draws-1] = nullptr;
            upgraded_post_process_srvs[final_post_process_copy_draws-1] = nullptr;
            final_post_process_copy_draws--;
        }

        ASSERT_ONCE(ignore_upgraded_samplers);

        first_frame_draw_call = true;
        playing_video = false;
        has_drawn_post_process = false;
        final_post_process_copy_draws = 0;
    }

    void LoadConfigs() override
    {
        reshade::api::effect_runtime* runtime = nullptr;
        reshade::get_config_value(runtime, NAME, "UpgradeMaterialsSamplers", upgrade_materials_samplers);
    }

    void DrawImGuiSettings(DeviceData& device_data) override
    {
        reshade::api::effect_runtime* runtime = nullptr;
        ImGui::NewLine();
        if (ImGui::Checkbox("Force Anisotropic Filtering", &upgrade_materials_samplers))
            reshade::set_config_value(runtime, NAME, "UpgradeMaterialsSamplers", upgrade_materials_samplers);
    }

    void PrintImGuiAbout() override
    {
        ImGui::Text("Luma for \"Persona 5 Strikers\" - built on Luma Framework.\n\nCredits:\nLuma Framework contributors\nPumbo (Nioh reference implementation)");
    }
};

BOOL APIENTRY DllMain(HMODULE hModule, DWORD ul_reason_for_call, LPVOID lpReserved)
{
    if (ul_reason_for_call == DLL_PROCESS_ATTACH)
    {
        Globals::SetGlobals(PROJECT_NAME, "Persona 5 Strikers Luma mod");
        Globals::VERSION = 1;

        swapchain_format_upgrade_type = TextureFormatUpgradesType::AllowedEnabled;
        swapchain_upgrade_type = SwapchainUpgradeType::scRGB;
        texture_format_upgrades_type = TextureFormatUpgradesType::AllowedEnabled;
        texture_upgrade_formats = {
            reshade::api::format::r11g11b10_float,
        };
        texture_format_upgrades_2d_size_filters = 0 | (uint32_t)TextureFormatUpgrades2DSizeFilters::SwapchainResolution | (uint32_t)TextureFormatUpgrades2DSizeFilters::SwapchainAspectRatio | (uint32_t)TextureFormatUpgrades2DSizeFilters::No1Px;

        force_ignore_dpi = true;
        enable_samplers_upgrade = true;
        force_upgrade_linear_samplers = true;
        ignore_upgraded_samplers = true;

        // NOTE: These shader hashes are placeholders for Persona 5 Strikers.
        // Run in Development-Release/Development-Debug and let Luma dump shaders to identify correct hashes.
        // Replace with actual values from dumped shader files (hash in filename).
        shader_hashes_Copy.pixel_shaders = { 0x00000000 };
        shader_hashes_SwapchainCopy.pixel_shaders = { 0x00000000 };
        shader_hashes_PostProcessEncode.pixel_shaders = { 0x00000000 };
        shader_hashes_UI.pixel_shaders = { 0x00000000 };
        shader_hashes_Sky.pixel_shaders = { 0x00000000 };
        shader_hashes_BeginMaterialsDrawing.compute_shaders = { 0x00000000 };
        shader_hashes_EndMaterialsDrawing.pixel_shaders = { 0x00000000 };

        game = new Persona5Strikers();
    }

    CoreMain(hModule, ul_reason_for_call, lpReserved);
    return TRUE;
}