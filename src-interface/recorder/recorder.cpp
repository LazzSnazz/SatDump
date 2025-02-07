#include "recorder.h"
#include "core/config.h"
#include "common/utils.h"
#include "core/style.h"
#include "logger.h"
#include "imgui/imgui_stdlib.h"
#include "core/pipeline.h"

#include "main_ui.h"

#include "imgui/imgui_internal.h"

#include "processing.h"

#include "resources.h"

namespace satdump
{
    RecorderApplication::RecorderApplication()
        : Application("recorder"), pipeline_selector(true)
    {
        dsp::registerAllSources();

        automated_live_output_dir = config::main_cfg["satdump_output_directories"]["live_processing_autogen"]["value"].get<bool>();
        processing_modules_floating_windows = config::main_cfg["user_interface"]["recorder_floating_windows"]["value"].get<bool>();

        sources = dsp::getAllAvailableSources();

        for (dsp::SourceDescriptor src : sources)
        {
            logger->debug("Device " + src.name);
            sdr_select_string += src.name + '\0';
        }

        for (int i = 0; i < (int)sources.size(); i++)
        {
            try
            {
                source_ptr = dsp::getSourceFromDescriptor(sources[i]);
                source_ptr->open();
                sdr_select_id = i;
                break;
            }
            catch (std::runtime_error &e)
            {
                logger->error(e.what());
            }
        }

        // Load waterfall palettes
        {
            waterfall_palettes_str = "";
            std::filesystem::recursive_directory_iterator paletteIterator(resources::getResourcePath("waterfall"));
            std::error_code iteratorError;
            while (paletteIterator != std::filesystem::recursive_directory_iterator())
            {
                if (!std::filesystem::is_directory(paletteIterator->path()))
                {
                    auto map = colormaps::loadMap(paletteIterator->path().string());
                    waterfall_palettes.push_back(map);
                    waterfall_palettes_str += map.name + " [" + map.author + "]" + '\0';
                }

                paletteIterator.increment(iteratorError);
                if (iteratorError)
                    logger->critical(iteratorError.message());
            }
        }

        source_ptr->set_frequency(frequency_mhz * 1e6);
        try_load_sdr_settings();

        splitter = std::make_shared<dsp::SplitterBlock>(source_ptr->output_stream);
        splitter->set_output_2nd(false);
        splitter->set_output_3rd(false);

        fft = std::make_shared<dsp::FFTPanBlock>(splitter->output_stream);
        fft->set_fft_settings(fft_size, current_samplerate, fft_rate);
        fft->avg_rate = 0.1;
        fft->start();

        file_sink = std::make_shared<dsp::FileSinkBlock>(splitter->output_stream_2);
        file_sink->set_output_sample_type(dsp::CF_32);
        file_sink->start();

        fft_plot = std::make_shared<widgets::FFTPlot>(fft->output_stream->writeBuf, fft_size, -10, 20, 10);
        waterfall_plot = std::make_shared<widgets::WaterfallPlot>(fft->output_stream->writeBuf, fft_size, 500);

        if (config::main_cfg["user"].contains("recorder_state"))
            deserialize_config(config::main_cfg["user"]["recorder_state"]);
    }

    RecorderApplication::~RecorderApplication()
    {
        stop_processing();
        if (is_started)
        {
            splitter->stop_tmp();
            source_ptr->stop();
            is_started = false;
        }
        source_ptr->close();
        splitter->input_stream = std::make_shared<dsp::stream<complex_t>>();
        splitter->stop();
        fft->stop();
        file_sink->stop();
    }

    void RecorderApplication::drawUI()
    {
        ImVec2 recorder_size = ImGui::GetContentRegionAvail();
        // recorder_size.y -= ImGui::GetCursorPosY();

        ImGui::BeginGroup();

        float wf_size = recorder_size.y - ((is_processing && !processing_modules_floating_windows) ? 240 * ui_scale : 0);
        ImGui::BeginChild("RecorderChildPanel", {float(recorder_size.x * panel_ratio), wf_size}, false);
        {
            if (ImGui::CollapsingHeader("Device", ImGuiTreeNodeFlags_DefaultOpen))
            {
                ImGui::Spacing();
                if (is_started)
                    style::beginDisabled();
                if (ImGui::Combo("##Source", &sdr_select_id, sdr_select_string.c_str()))
                {
                    source_ptr = getSourceFromDescriptor(sources[sdr_select_id]);

                    // Try to open a device, if it doesn't work, we re-open a device we can
                    try
                    {
                        source_ptr->open();
                    }
                    catch (std::runtime_error &e)
                    {
                        logger->error(e.what());

                        for (int i = 0; i < (int)sources.size(); i++)
                        {
                            try
                            {
                                source_ptr = dsp::getSourceFromDescriptor(sources[i]);
                                source_ptr->open();
                                sdr_select_id = i;
                                break;
                            }
                            catch (std::runtime_error &e)
                            {
                                logger->error(e.what());
                            }
                        }
                    }

                    source_ptr->set_frequency(frequency_mhz * 1e6);
                    try_load_sdr_settings();
                }
                ImGui::SameLine();
                if (ImGui::Button(u8" \uead2 "))
                {
                    sources = dsp::getAllAvailableSources();

                    sdr_select_string.clear();
                    for (dsp::SourceDescriptor src : sources)
                    {
                        logger->debug("Device " + src.name);
                        sdr_select_string += src.name + '\0';
                    }

                    while (sdr_select_id >= (int)sources.size())
                        sdr_select_id--;

                    source_ptr = getSourceFromDescriptor(sources[sdr_select_id]);
                    source_ptr->open();
                    source_ptr->set_frequency(frequency_mhz * 1e6);
                    try_load_sdr_settings();
                }
                /*
                if (ImGui::IsItemHovered())
                    {
                        ImGui::BeginTooltip();
                        ImGui::PushTextWrapPos(ImGui::GetFontSize() * 35.0f);
                        ImGui::TextUnformatted("Refresh source list");
                        ImGui::PopTextWrapPos();
                        ImGui::EndTooltip();
                    }
                */
                if (is_started)
                    style::endDisabled();

                ImGui::Spacing();
                ImGui::Separator();
                ImGui::Spacing();

                if (ImGui::InputDouble("MHz", &frequency_mhz))
                    source_ptr->set_frequency(frequency_mhz * 1e6);

                source_ptr->drawControlUI();

                if (!is_started)
                {
                    if (ImGui::Button("Start"))
                    {
                        source_ptr->set_frequency(frequency_mhz * 1e6);
                        try
                        {
                            source_ptr->start();

                            current_samplerate = source_ptr->get_samplerate();
                            fft->set_fft_settings(fft_size, current_samplerate, fft_rate);

                            splitter->input_stream = source_ptr->output_stream;
                            splitter->start();
                            is_started = true;
                            sdr_error = "";
                        }
                        catch (std::runtime_error &e)
                        {
                            sdr_error = e.what();
                            logger->error(e.what());
                        }
                    }
                }
                else
                {
                    if (ImGui::Button("Stop"))
                    {
                        splitter->stop_tmp();
                        source_ptr->stop();
                        is_started = false;
                        config::main_cfg["user"]["recorder_sdr_settings"][sources[sdr_select_id].name] = source_ptr->get_settings();
                        config::main_cfg["user"]["recorder_sdr_settings"][sources[sdr_select_id].name]["samplerate"] = source_ptr->get_samplerate();
                        config::main_cfg["user"]["recorder_sdr_settings"][sources[sdr_select_id].name]["frequency"] = frequency_mhz * 1e6;
                        config::saveUserConfig();
                    }
                }
                ImGui::SameLine();
                ImGui::TextColored(ImColor(255, 0, 0), "%s", sdr_error.c_str());
            }

            if (ImGui::CollapsingHeader("FFT", ImGuiTreeNodeFlags_DefaultOpen))
            {
                if (ImGui::Combo("FFT Size", &selected_fft_size, "8192\0"
                                                                 "4096\0"
                                                                 "2048\0"
                                                                 "1024\0"))
                {
                    fft_size = fft_sizes_lut[selected_fft_size];

                    fft->set_fft_settings(fft_size, current_samplerate, fft_rate);
                    fft_plot->set_size(fft_size);
                    waterfall_plot->set_size(fft_size);

                    logger->info("Set FFT size to {:d}", fft_size);
                }
                if (ImGui::InputInt("FFT Rate", &fft_rate))
                {
                    fft_size = fft_sizes_lut[selected_fft_size];

                    fft->set_fft_settings(fft_size, current_samplerate, fft_rate);

                    logger->info("Set FFT rate to {:d}", fft_rate);
                }
                ImGui::SliderFloat("FFT Max", &fft_plot->scale_max, -150, 150);
                ImGui::SliderFloat("FFT Min", &fft_plot->scale_min, -150, 150);
                ImGui::SliderFloat("Avg Rate", &fft->avg_rate, 0.01, 0.99);
                if (ImGui::Combo("Palette", &selected_waterfall_palette, waterfall_palettes_str.c_str()))
                    waterfall_plot->set_palette(waterfall_palettes[selected_waterfall_palette]);
                ImGui::Checkbox("Show Waterfall", &show_waterfall);
            }

            if (fft_plot->scale_max < fft_plot->scale_min)
            {
                fft_plot->scale_min = waterfall_plot->scale_min;
                fft_plot->scale_max = waterfall_plot->scale_max;
            }
            else if (fft_plot->scale_min > fft_plot->scale_max)
            {
                fft_plot->scale_min = waterfall_plot->scale_min;
                fft_plot->scale_max = waterfall_plot->scale_max;
            }
            else
            {
                waterfall_plot->scale_min = fft_plot->scale_min;
                waterfall_plot->scale_max = fft_plot->scale_max;
            }

            if (ImGui::CollapsingHeader("Processing"))
            {
                // Settings & Selection menu
                if (is_processing)
                    style::beginDisabled();
                pipeline_selector.renderSelectionBox(ImGui::GetContentRegionAvail().x);
                if (!automated_live_output_dir)
                    pipeline_selector.drawMainparamsLive();
                pipeline_selector.renderParamTable();
                if (is_processing)
                    style::endDisabled();

                // Preset Menu
                Pipeline selected_pipeline = pipelines[pipeline_selector.pipeline_id];
                if (selected_pipeline.preset.frequencies.size() > 0)
                {
                    if (ImGui::BeginCombo("Freq###presetscombo", ""))
                    {
                        for (int n = 0; n < (int)selected_pipeline.preset.frequencies.size(); n++)
                        {
                            const bool is_selected = (pipeline_preset_id == n);
                            if (ImGui::Selectable(selected_pipeline.preset.frequencies[n].first.c_str(), is_selected))
                            {
                                pipeline_preset_id = n;

                                if (selected_pipeline.preset.frequencies[pipeline_preset_id].second != 0)
                                {
                                    frequency_mhz = double(selected_pipeline.preset.frequencies[pipeline_preset_id].second) / 1e6;
                                    source_ptr->set_frequency(frequency_mhz * 1e6);
                                }
                            }

                            if (is_selected)
                                ImGui::SetItemDefaultFocus();
                        }
                        ImGui::EndCombo();
                    }
                }

                if (!is_started)
                    style::beginDisabled();

                if (!is_processing)
                {
                    if (ImGui::Button("Start###startprocessing"))
                        start_processing();
                }
                else
                {
                    if (ImGui::Button("Stop##stopprocessing"))
                        stop_processing();
                }

                ImGui::SameLine();
                ImGui::TextColored(ImColor(255, 0, 0), "%s", error.c_str());

                if (!is_started)
                    style::endDisabled();
            }

            if (ImGui::CollapsingHeader("Recording"))
            {
                if (is_recording)
                    style::beginDisabled();
                if (ImGui::Combo("Format", &select_sample_format, "f32\0"
                                                                  "s16\0"
                                                                  "s8\0"
                                                                  "wav16\0"
#ifdef BUILD_ZIQ
                                                                  "ziq s8\0"
                                                                  "ziq s16\0"
                                                                  "ziq f32\0"
#endif
                                 ))
                {
                    if (select_sample_format == 0)
                        file_sink->set_output_sample_type(dsp::CF_32);
                    else if (select_sample_format == 1)
                        file_sink->set_output_sample_type(dsp::IS_16);
                    else if (select_sample_format == 2)
                        file_sink->set_output_sample_type(dsp::IS_8);
                    else if (select_sample_format == 3)
                        file_sink->set_output_sample_type(dsp::WAV_16);
#ifdef BUILD_ZIQ
                    else if (select_sample_format == 4)
                    {
                        file_sink->set_output_sample_type(dsp::ZIQ);
                        ziq_bit_depth = 8;
                    }
                    else if (select_sample_format == 5)
                    {
                        file_sink->set_output_sample_type(dsp::ZIQ);
                        ziq_bit_depth = 16;
                    }
                    else if (select_sample_format == 6)
                    {
                        file_sink->set_output_sample_type(dsp::ZIQ);
                        ziq_bit_depth = 32;
                    }
#endif
                }
                if (is_recording)
                    style::endDisabled();

                if (file_sink->get_written() < 1e9)
                    ImGui::Text("Size : %.2f MB", file_sink->get_written() / 1e6);
                else
                    ImGui::Text("Size : %.2f GB", file_sink->get_written() / 1e9);

#ifdef BUILD_ZIQ
                if (select_sample_format == 4 || select_sample_format == 5 || select_sample_format == 6)
                {
                    if (file_sink->get_written_raw() < 1e9)
                        ImGui::Text("Size (raw) : %.2f MB", file_sink->get_written_raw() / 1e6);
                    else
                        ImGui::Text("Size (raw) : %.2f GB", file_sink->get_written_raw() / 1e9);
                }
#endif

                ImGui::Text("File : %s", recorder_filename.c_str());

                ImGui::Spacing();

                if (!is_recording)
                    ImGui::TextColored(ImColor(255, 0, 0), "IDLE");
                else
                    ImGui::TextColored(ImColor(0, 255, 0), "RECORDING");

                ImGui::Spacing();

                if (!is_recording)
                {
                    if (ImGui::Button("Start###startrecording"))
                        start_recording();
                }
                else
                {
                    if (ImGui::Button("Stop##stoprecording"))
                        stop_recording();
                }
            }
        }
        ImGui::EndChild();
        ImGui::EndGroup();

        ImGui::SameLine();

        ImGui::BeginGroup();
        ImGui::BeginChild("RecorderFFT", {float(recorder_size.x * (1.0 - panel_ratio)), wf_size}, false, ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse);
        {
            float fft_height = wf_size * (show_waterfall ? (1.0 - waterfall_ratio) : 1.0);
            float wf_height = wf_size * waterfall_ratio;
            fft_plot->draw({float(recorder_size.x * (1.0 - panel_ratio) - 4), fft_height});
            if (show_waterfall)
                waterfall_plot->draw({float(recorder_size.x * (1.0 - panel_ratio) - 4), wf_height}, is_started);

            float offset = 35 * ui_scale;

            ImVec2 mouse_pos = ImGui::GetMousePos();
            if ((mouse_pos.y > offset + fft_height - 10 * ui_scale &&
                 mouse_pos.y < offset + fft_height + 10 * ui_scale &&
                 mouse_pos.x > recorder_size.x * 0.20) ||
                dragging_waterfall)
            {
                ImGui::SetMouseCursor(ImGuiMouseCursor_ResizeNS);
                if (ImGui::IsMouseDown(ImGuiMouseButton_Left))
                {
                    float new_y = mouse_pos.y - offset;
                    float new_ratio = new_y / (fft_height + wf_height);
                    if (new_ratio > 0.1 && new_ratio < 0.9)
                        waterfall_ratio = 1.0 - new_ratio;
                }
                else
                    dragging_waterfall = false;
                if (ImGui::IsMouseClicked(ImGuiMouseButton_Left))
                    dragging_waterfall = true;
            }
            else if (ImGui::GetMouseCursor() != ImGuiMouseCursor_ResizeEW)
                ImGui::SetMouseCursor(ImGuiMouseCursor_Arrow);
        }
        ImGui::EndChild();
        ImGui::EndGroup();

        ImVec2 mouse_pos = ImGui::GetMousePos();
        if ((mouse_pos.x > recorder_size.x * panel_ratio + 15 * ui_scale - 10 * ui_scale &&
             mouse_pos.x < recorder_size.x * panel_ratio + 15 * ui_scale + 10 * ui_scale &&
             mouse_pos.y > 35 * ui_scale) ||
            dragging_panel)
        {
            ImGui::SetMouseCursor(ImGuiMouseCursor_ResizeEW);
            if (ImGui::IsMouseDown(ImGuiMouseButton_Left))
            {
                float new_x = mouse_pos.x;
                float new_ratio = new_x / recorder_size.x;
                if (new_ratio > 0.1 && new_ratio < 0.9)
                    panel_ratio = new_ratio;
            }
            else
                dragging_panel = false;
            if (ImGui::IsMouseClicked(ImGuiMouseButton_Left))
                dragging_panel = true;
        }
        else if (ImGui::GetMouseCursor() != ImGuiMouseCursor_ResizeNS)
            ImGui::SetMouseCursor(ImGuiMouseCursor_Arrow);

        if (is_processing)
        {
            if (processing_modules_floating_windows)
            {
                for (std::shared_ptr<ProcessingModule> &module : live_pipeline->modules)
                    module->drawUI(true);
            }
            else
            {
                float y_pos = ImGui::GetCursorPosY() + 35 * ui_scale;
                float live_width = recorder_size.x + 16 * ui_scale;
                float live_height = 250 * ui_scale;
                float winwidth = live_pipeline->modules.size() > 0 ? live_width / live_pipeline->modules.size() : live_width;
                float currentPos = 0;
                for (std::shared_ptr<ProcessingModule> &module : live_pipeline->modules)
                {
                    ImGui::SetNextWindowPos({currentPos, y_pos});
                    ImGui::SetNextWindowSize({(float)winwidth, (float)live_height});
                    module->drawUI(false);
                    currentPos += winwidth;

                    // if (ImGui::GetCurrentContext()->last_window != NULL)
                    //     currentPos += ImGui::GetCurrentContext()->last_window->Size.x;
                    //  logger->info(ImGui::GetCurrentContext()->last_window->Name);
                }
            }
        }
    }
};
