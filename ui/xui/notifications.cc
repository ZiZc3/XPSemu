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
#include "notifications.hh"
#include "common.hh"
#ifdef __PROSPERO__
#include <algorithm>
#include <string>
#include <vector>
#include "font-manager.hh"
#include "ui-sounds.hh"
#endif

#include "../xemu-notifications.h"

NotificationManager notification_manager;

NotificationManager::NotificationManager()
{
    m_active = false;
}

void NotificationManager::QueueNotification(const char *msg)
{
    m_notification_queue.push_back(strdup(msg));
}

void NotificationManager::QueueError(const char *msg)
{
    m_error_queue.push_back(strdup(msg));
}

void NotificationManager::Draw()
{
    uint32_t now = SDL_GetTicks();

    if (m_active) {
        // Currently displaying a notification
        float t =
            (m_notification_end_time - now) / (float)kNotificationDuration;
        if (t > 1.0) {
            // Notification delivered, free it
            free((void *)m_msg);
            m_active = false;
        } else {
            // Notification should be displayed
            DrawNotification(t, m_msg);
        }
    } else {
        // Check to see if a notification is pending
        if (m_notification_queue.size() > 0) {
            m_msg = m_notification_queue[0];
            m_active = true;
            m_notification_end_time = now + kNotificationDuration;
            m_notification_queue.pop_front();
#ifdef __PROSPERO__
            if (g_config.display.ui.show_notifications) {
                UiSoundPlay(UI_SOUND_NOTIFY);
            }
#endif
        }
    }

    ImGuiIO& io = ImGui::GetIO();

    if (m_error_queue.size() > 0) {
        ImGui::OpenPopup("Error");
        ImGui::SetNextWindowPos(ImVec2(io.DisplaySize.x/2, io.DisplaySize.y/2),
                                ImGuiCond_Always, ImVec2(0.5, 0.5));
    }
    if (ImGui::BeginPopupModal("Error", NULL, ImGuiWindowFlags_AlwaysAutoResize))
    {
        ImGui::Text("%s", m_error_queue[0]);
        ImGui::Dummy(ImVec2(0,16));
        ImGui::SetItemDefaultFocus();
        ImGuiStyle &style = ImGui::GetStyle();
        ImGui::SetCursorPosX(ImGui::GetWindowWidth()-(120+2*style.FramePadding.x));
        if (ImGui::Button("Ok", ImVec2(120, 0))) {
            ImGui::CloseCurrentPopup();
            free((void*)m_error_queue[0]);
            m_error_queue.pop_front();
        }
        ImGui::EndPopup();
    }
}

#ifdef __PROSPERO__
// XPSemu: notifications as small toasts in the dashboard's style, as the
// Xbox 360's: a dark green glass pill rising in at the bottom middle, a lime
// edge and a small glowing orb, the text in one or two lines; then gone.
void NotificationManager::DrawNotification(float t, const char *msg)
{
    if (!g_config.display.ui.show_notifications) {
        return;
    }
    ImDrawList *dl = ImGui::GetForegroundDrawList();
    ImVec2 size = ImGui::GetIO().DisplaySize;
    float s = size.y / 1080.0f;
    ImFont *font = g_font_mgr.m_menu_font_small;
    auto col = [](int r, int g, int b, float a) {
        return IM_COL32(r, g, b, (int)(255 * std::clamp(a, 0.f, 1.f)));
    };

    // t runs from 1 down to 0 over the notification's time.
    float shown = (1 - t) * kNotificationDuration / 1000.0f;
    float left = t * kNotificationDuration / 1000.0f;
    float in = std::clamp(shown / 0.3f, 0.f, 1.f);
    in = 1 - (1 - in) * (1 - in) * (1 - in);
    float a = in * std::clamp(left / 0.4f, 0.f, 1.f);

    // The text, in at most two lines.
    float fs = 26 * s, max_w = 900 * s;
    std::vector<std::string> lines;
    std::string line, text = msg;
    size_t i = 0;
    while (i < text.size()) {
        size_t j = text.find(' ', i);
        std::string word = text.substr(i, j == std::string::npos ? j : j - i);
        i = j == std::string::npos ? text.size() : j + 1;
        std::string next = line.empty() ? word : line + " " + word;
        if (line.empty() ||
            font->CalcTextSizeA(fs, FLT_MAX, 0, next.c_str()).x <= max_w) {
            line = next;
        } else {
            lines.push_back(line);
            line = word;
        }
    }
    if (!line.empty()) {
        lines.push_back(line);
    }
    if (lines.size() > 2) {
        lines.resize(2);
        while (!lines[1].empty() &&
               font->CalcTextSizeA(fs, FLT_MAX, 0, (lines[1] + "...").c_str())
                       .x > max_w) {
            lines[1].pop_back();
        }
        lines[1] += "...";
    }
    float text_w = 0;
    for (const auto &l : lines) {
        text_w = std::max(text_w, font->CalcTextSizeA(fs, FLT_MAX, 0,
                                                      l.c_str()).x);
    }
    float lh = fs * 1.15f;
    float pad = 22 * s, orb = 15 * s;
    float h = std::max(lh * lines.size() + 24 * s, 62 * s);
    float w = pad + orb * 2 + 16 * s + text_w + pad + 6 * s;
    float y = size.y - 150 * s - h + (1 - in) * 36 * s;
    ImVec2 p0(size.x / 2 - w / 2, y), p1(size.x / 2 + w / 2, y + h);
    float r = std::min(h / 2, 31 * s);

    dl->AddRectFilled(ImVec2(p0.x + 4 * s, p0.y + 6 * s),
                      ImVec2(p1.x + 4 * s, p1.y + 6 * s), col(0, 0, 0, a * 0.35f),
                      r);
    dl->AddRectFilled(p0, p1, col(6, 26, 6, a * 0.94f), r);
    dl->AddRectFilledMultiColor(ImVec2(p0.x + r, p0.y + 1 * s),
                                ImVec2(p1.x - r, p0.y + h * 0.45f),
                                col(90, 200, 30, a * 0.16f),
                                col(90, 200, 30, a * 0.16f),
                                col(90, 200, 30, 0), col(90, 200, 30, 0));
    dl->AddRect(p0, p1, col(205, 245, 45, a * 0.85f), r, 0, 2 * s);

    // The orb, with a pop as it arrives.
    float pop = 1 + 0.25f * sinf(std::min(shown / 0.35f, 1.f) * (float)M_PI);
    ImVec2 oc(p0.x + pad + orb, (p0.y + p1.y) / 2);
    dl->AddCircleFilled(oc, orb * 1.7f * pop, col(205, 245, 45, a * 0.15f), 24);
    dl->AddCircleFilled(oc, orb * pop, col(170, 235, 60, a), 24);
    dl->AddCircleFilled(oc, orb * 0.5f * pop, col(30, 90, 12, a), 20);
    dl->AddCircleFilled(ImVec2(oc.x - orb * 0.35f, oc.y - orb * 0.35f),
                        orb * 0.22f, col(255, 255, 255, a * 0.7f), 12);

    float tx = oc.x + orb + 16 * s;
    float ty = (p0.y + p1.y) / 2 - lh * lines.size() / 2 + (lh - fs) / 2;
    for (const auto &l : lines) {
        dl->AddText(font, fs, ImVec2(tx, ty), col(255, 255, 255, a), l.c_str());
        ty += lh;
    }
}
#else
void NotificationManager::DrawNotification(float t, const char *msg)
{
    if (!g_config.display.ui.show_notifications) {
        return;
    }

    const float DISTANCE = 10.0f;
    static int corner = 1;
    ImGuiIO& io = ImGui::GetIO();
    if (corner != -1)
    {
        ImVec2 window_pos = ImVec2((corner & 1) ? io.DisplaySize.x - DISTANCE : DISTANCE, (corner & 2) ? io.DisplaySize.y - DISTANCE : DISTANCE);
        window_pos.y = g_main_menu_height + DISTANCE;
        ImVec2 window_pos_pivot = ImVec2((corner & 1) ? 1.0f : 0.0f, (corner & 2) ? 1.0f : 0.0f);
        ImGui::SetNextWindowPos(window_pos, ImGuiCond_Always, window_pos_pivot);
    }

    const float fade_in  = 0.1;
    const float fade_out = 0.9;
    float fade = 0;

    if (t < fade_in) {
        // Linear fade in
        fade = t/fade_in;
    } else if (t >= fade_out) {
        // Linear fade out
        fade = 1-(t-fade_out)/(1-fade_out);
    } else {
        // Constant
        fade = 1.0;
    }

    ImVec4 color = ImGui::GetStyle().Colors[ImGuiCol_ButtonActive];
    color.w *= fade;
    ImGui::PushStyleVar(ImGuiStyleVar_PopupBorderSize, 1);
    ImGui::PushStyleColor(ImGuiCol_PopupBg, ImVec4(0,0,0,fade*0.9f));
    ImGui::PushStyleColor(ImGuiCol_Border, color);
    ImGui::PushStyleColor(ImGuiCol_Text, color);
    ImGui::SetNextWindowBgAlpha(0.90f * fade);
    if (ImGui::Begin("Notification", NULL,
        ImGuiWindowFlags_Tooltip |
        ImGuiWindowFlags_NoMove |
        ImGuiWindowFlags_NoDecoration |
        ImGuiWindowFlags_AlwaysAutoResize |
        ImGuiWindowFlags_NoSavedSettings |
        ImGuiWindowFlags_NoFocusOnAppearing |
        ImGuiWindowFlags_NoNav |
        ImGuiWindowFlags_NoInputs
        ))
    {
        ImGui::Text("%s", msg);
    }
    ImGui::PopStyleColor();
    ImGui::PopStyleColor();
    ImGui::PopStyleColor();
    ImGui::PopStyleVar();
    ImGui::End();
}

#endif

/* External interface, exposed via xemu-notifications.h */

void xemu_queue_notification(const char *msg)
{
    notification_manager.QueueNotification(msg);
}

void xemu_queue_error_message(const char *msg)
{
    notification_manager.QueueError(msg);
}
