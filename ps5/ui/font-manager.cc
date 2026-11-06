//
// xemu User Interface
//
// Copyright (C) 2020-2022 Matt Borgerson
//
// This program is free software; you can redistribute it and/or modify
// it under the terms of the GNU General Public License as published by
// the Free Software Foundation; either version 2 of the License, or
// (at your option) any later version.
//
// This program is distributed in the hope that it will be useful,
// but WITHOUT ANY WARRANTY; without even the implied warranty of
// MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
// GNU General Public License for more details.
//
// You should have received a copy of the GNU General Public License
// along with this program.  If not, see <http://www.gnu.org/licenses/>.
//
#include "font-manager.hh"
#include "viewport-manager.hh"

#include "data/Roboto-Medium.ttf.h"
#include "data/RobotoCondensed-Regular.ttf.h"
#include "data/font_awesome_6_1_1_solid.min.otf.h"
#include "data/abxy.ttf.h"

FontManager g_font_mgr;

FontManager::FontManager()
{
    m_last_viewport_scale = 1;
    m_last_pixel_density = 1;
}

void FontManager::Rebuild()
{
    ImGuiIO &io = ImGui::GetIO();

    io.Fonts->Clear();

    float scale = g_viewport_mgr.m_scale;
    float pixel_density = g_viewport_mgr.m_pixel_density;

    {
        ImFontConfig config;
        config.FontDataOwnedByAtlas = false;
        config.RasterizerDensity = pixel_density;
        m_default_font = io.Fonts->AddFontFromMemoryTTF(
            (void *)Roboto_Medium_data, Roboto_Medium_size,
            16.0f * scale, &config);
        m_menu_font_small = io.Fonts->AddFontFromMemoryTTF(
            (void *)RobotoCondensed_Regular_data, RobotoCondensed_Regular_size,
            22.0f * scale, &config);
    }
    {
        ImFontConfig config;
        config.FontDataOwnedByAtlas = false;
        config.RasterizerDensity = pixel_density;
        m_menu_font_medium = io.Fonts->AddFontFromMemoryTTF(
            (void *)RobotoCondensed_Regular_data, RobotoCondensed_Regular_size,
            26.0f * scale, &config);
        m_menu_font = io.Fonts->AddFontFromMemoryTTF(
            (void *)RobotoCondensed_Regular_data, RobotoCondensed_Regular_size,
            34.0f * scale, &config);
    }
    {
        ImFontConfig config = ImFontConfig();
        config.OversampleH = config.OversampleV = 1;
        config.PixelSnapH = true;
        config.RasterizerDensity = pixel_density;
        config.SizePixels = 13.0f * scale;
        m_fixed_width_font = io.Fonts->AddFontDefault(&config);
    }

    xemu_vk_rebuild_fonts();
}

void FontManager::Update()
{
    if (g_viewport_mgr.m_scale != m_last_viewport_scale ||
        g_viewport_mgr.m_pixel_density != m_last_pixel_density) {
        Rebuild();
        m_last_viewport_scale = g_viewport_mgr.m_scale;
        m_last_pixel_density = g_viewport_mgr.m_pixel_density;
    }
}
