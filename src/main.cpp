#include <chrono>
#include <fstream>
#include <iostream>
#include <sstream>
#include <string>
#include <vector>

#include <GLFW/glfw3.h>

#include "imgui.h"
#include "backends/imgui_impl_glfw.h"
#include "backends/imgui_impl_opengl3.h"

#include "cell.hpp"

static Cell_Manager sim;

static void InitSim(int w, int h)
{
    sim.width = w;
    sim.height = h;
    sim.cells.resize(w * h);

    for (auto& c : sim.cells)
    {
        c.state = CellState::Dead;
        c.nextState = CellState::Dead;
        c.energy = 0.0f;
        c.nextEnergy = 0.0f;
    }
}

//---------------------------------------
// Settings persistence
//---------------------------------------
// Hand-rolled JSON reader/writer scoped to exactly the fields this app
// exposes. It's not a general-purpose parser — if you add a new tunable,
// add it to both SaveSettings and LoadSettings below.

static const char* kSettingsPath = "settings.json";

static void SaveSettings(const char* path, const Cell_Manager& sim,
                          float cellSize, float simRate)
{
    std::ofstream f(path);
    if (!f.is_open())
        return;

    f << "{\n";
    f << "  \"cellSize\": " << cellSize << ",\n";
    f << "  \"simRate\": " << simRate << ",\n";
    f << "  \"lonlinessPunishment\": " << sim.lonlinessPunishment << ",\n";
    f << "  \"hinderThreshold\": " << sim.hinderThreshold << ",\n";
    f << "  \"maxEnergy\": " << sim.maxEnergy << ",\n";
    f << "  \"overcrowdingThreshold\": " << sim.overcrowdingThreshold << ",\n";
    f << "  \"reviveAliveCount\": " << sim.reviveAliveCount << ",\n";
    f << "  \"reviveHinderedCount\": " << sim.reviveHinderedCount << ",\n";
    f << "  \"hinderedEffectFromAlive\": " << sim.hinderedEffectFromAlive << ",\n";
    f << "  \"hinderedEffectFromHindered\": " << sim.hinderedEffectFromHindered << ",\n";
    f << "  \"hinderedEffectFromDead\": " << sim.hinderedEffectFromDead << ",\n";
    f << "  \"aliveEffectFromAlive\": " << sim.aliveEffectFromAlive << ",\n";
    f << "  \"aliveEffectFromHindered\": " << sim.aliveEffectFromHindered << ",\n";
    f << "  \"aliveEffectFromDead\": " << sim.aliveEffectFromDead << ",\n";
    f << "  \"wrapEdges\": " << (sim.wrapEdges ? "true" : "false") << "\n";
    f << "}\n";
}

// Returns true if the file was found and at least parsed as JSON-ish.
static bool LoadSettings(const char* path, Cell_Manager& sim,
                          float& cellSize, float& simRate)
{
    std::ifstream f(path);
    if (!f.is_open())
        return false;

    std::string line;

    while (std::getline(f, line))
    {
        size_t keyStart = line.find('"');
        if (keyStart == std::string::npos)
            continue;

        size_t keyEnd = line.find('"', keyStart + 1);
        if (keyEnd == std::string::npos)
            continue;

        std::string key = line.substr(keyStart + 1, keyEnd - keyStart - 1);

        size_t colon = line.find(':', keyEnd);
        if (colon == std::string::npos)
            continue;

        std::string value = line.substr(colon + 1);

        // Trim trailing comma/whitespace and leading whitespace.
        while (!value.empty() &&
               (value.back() == ',' || value.back() == '\r' ||
                value.back() == '\n' || value.back() == ' '))
        {
            value.pop_back();
        }

        size_t valueStart = value.find_first_not_of(' ');
        if (valueStart != std::string::npos)
            value = value.substr(valueStart);

        if (value.empty())
            continue;

        try
        {
            if (key == "cellSize") cellSize = std::stof(value);
            else if (key == "simRate") simRate = std::stof(value);
            else if (key == "lonlinessPunishment") sim.lonlinessPunishment = std::stof(value);
            else if (key == "hinderThreshold") sim.hinderThreshold = std::stof(value);
            else if (key == "maxEnergy") sim.maxEnergy = std::stof(value);
            else if (key == "overcrowdingThreshold") sim.overcrowdingThreshold = std::stoi(value);
            else if (key == "reviveAliveCount") sim.reviveAliveCount = std::stoi(value);
            else if (key == "reviveHinderedCount") sim.reviveHinderedCount = std::stoi(value);
            else if (key == "hinderedEffectFromAlive") sim.hinderedEffectFromAlive = std::stof(value);
            else if (key == "hinderedEffectFromHindered") sim.hinderedEffectFromHindered = std::stof(value);
            else if (key == "hinderedEffectFromDead") sim.hinderedEffectFromDead = std::stof(value);
            else if (key == "aliveEffectFromAlive") sim.aliveEffectFromAlive = std::stof(value);
            else if (key == "aliveEffectFromHindered") sim.aliveEffectFromHindered = std::stof(value);
            else if (key == "aliveEffectFromDead") sim.aliveEffectFromDead = std::stof(value);
            else if (key == "wrapEdges") sim.wrapEdges = (value == "true");
        }
        catch (const std::exception&)
        {
            // Malformed value for this key — skip it, keep the current
            // in-memory default instead of crashing the load.
            continue;
        }
    }

    return true;
}

int main()
{
    if (!glfwInit())
        return -1;

    const char* glsl_version = "#version 130";

    GLFWwindow* window = glfwCreateWindow(1280, 720, "Cell Sim", nullptr, nullptr);
    if (!window)
    {
        glfwTerminate();
        return -1;
    }

    glfwMakeContextCurrent(window);
    glfwSwapInterval(1);

    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGui::StyleColorsDark();

    ImGui_ImplGlfw_InitForOpenGL(window, true);
    ImGui_ImplOpenGL3_Init(glsl_version);

    InitSim(200, 201);

    float cellSize = 6.0f;
    bool running = true;
    int paintMode = 0;

    // Simulation update rate, independent of render/UI frame rate — the
    // window keeps rendering (and stays responsive to input) at full
    // speed, only sim.Step() is throttled.
    float simRate = 10.0f; // steps per second

    // Load any saved settings over the defaults above before the loop
    // starts. If the file doesn't exist yet, defaults are kept as-is.
    std::string settingsStatus;
    if (LoadSettings(kSettingsPath, sim, cellSize, simRate))
        settingsStatus = "Loaded settings.json";

    double simAccumulator = 0.0;
    auto lastTime = std::chrono::steady_clock::now();

    // Windows can be dragged from anywhere in their body by default in
    // Dear ImGui, which makes painting on the grid also drag the window.
    // Restrict dragging to title bars only.
    ImGui::GetIO().ConfigWindowsMoveFromTitleBarOnly = true;

    // Updated once per frame after the "Grid" window is laid out, so mouse
    // coordinates can be converted from window-space to grid-space.
    ImVec2 gridOrigin(0.0f, 0.0f);

    while (!glfwWindowShouldClose(window))
    {
        auto now = std::chrono::steady_clock::now();
        double deltaTime = std::chrono::duration<double>(now - lastTime).count();
        lastTime = now;

        glfwPollEvents();

        // ImGui
        ImGui_ImplOpenGL3_NewFrame();
        ImGui_ImplGlfw_NewFrame();
        ImGui::NewFrame();

        //---------------------------------------
        // Controls
        //---------------------------------------

        ImGui::Begin("Controls");

        ImGui::Checkbox("Run", &running);

        ImGui::Text("Population: %d", sim.total_population);

        if (ImGui::Button("Step"))
            sim.Step();

        if (ImGui::Button("Reset"))
            InitSim(sim.width, sim.height);

        if (ImGui::Button("Save Settings"))
        {
            SaveSettings(kSettingsPath, sim, cellSize, simRate);
            settingsStatus = "Saved to settings.json";
        }

        ImGui::SameLine();

        if (ImGui::Button("Load Settings"))
        {
            settingsStatus = LoadSettings(kSettingsPath, sim, cellSize, simRate)
                ? "Loaded settings.json"
                : "No settings.json found";
        }

        if (!settingsStatus.empty())
            ImGui::TextDisabled("%s", settingsStatus.c_str());

        ImGui::SliderFloat("Cell Size", &cellSize, 2.0f, 12.0f, "%.1f",
                   ImGuiSliderFlags_AlwaysClamp);

        ImGui::SliderFloat("Sim Speed (steps/sec)", &simRate, 0.5f, 240.0f, "%.1f",
                   ImGuiSliderFlags_AlwaysClamp);

        ImGui::Separator();

        ImGui::Text("Paint Mode");

        ImGui::RadioButton("Alive", &paintMode, 0);
        ImGui::RadioButton("Hindered", &paintMode, 1);
        ImGui::RadioButton("Dead", &paintMode, 2);

        ImGui::Separator();

        //---------------------------------------
        // Live tuning of simulation variables
        //---------------------------------------

        ImGui::Text("Simulation Tuning");


        ImGui::Checkbox("Wrap Edges", &sim.wrapEdges);

        ImGui::SliderFloat("Lonliness Punishment", &sim.lonlinessPunishment, 0.0f, 1.0f, "%.3f", ImGuiSliderFlags_AlwaysClamp);

        ImGui::SliderFloat("Hinder Threshold", &sim.hinderThreshold, 0.0f, 1.0f, "%.3f",
                   ImGuiSliderFlags_AlwaysClamp);

        // Max Energy governs the ceiling every other energy value (and the
        // grid's color brightness) is measured against, so it's clamped to
        // a sane positive range rather than left editable only via
        // settings.json. See the render loop below: color brightness is
        // normalized by this value, not assumed to be 1.0.
        ImGui::SliderFloat("Max Energy", &sim.maxEnergy, 0.1f, 5.0f, "%.2f",
                   ImGuiSliderFlags_AlwaysClamp);

        ImGui::SliderInt("Overcrowding", &sim.overcrowdingThreshold, 1, 8, "%d",
                 ImGuiSliderFlags_AlwaysClamp);

        ImGui::SliderInt("Revive: Alive Neighbors", &sim.reviveAliveCount, 0, 8, "%d",
                 ImGuiSliderFlags_AlwaysClamp);

        ImGui::SliderInt("Revive: Hindered Neighbors", &sim.reviveHinderedCount, 0, 8, "%d",
                 ImGuiSliderFlags_AlwaysClamp);



        ImGui::Separator();
        ImGui::Text("Hindered Profile");

        ImGui::Text("Energy Effect (Alive)");
        ImGui::SliderFloat("##hinderedFromAlive",
                   &sim.hinderedEffectFromAlive,
                   -1.0f, 1.0f, "%.3f",
                   ImGuiSliderFlags_AlwaysClamp);

        ImGui::Text("Energy Effect (Hindered)");
        ImGui::SliderFloat("##hinderedFromHindered",
                   &sim.hinderedEffectFromHindered,
                   -1.0f, 1.0f, "%.3f",
                   ImGuiSliderFlags_AlwaysClamp);

        ImGui::Text("Energy Effect (Dead)");
        ImGui::SliderFloat("##hinderedFromDead",
                   &sim.hinderedEffectFromDead,
                   -1.0f, 1.0f, "%.3f",
                   ImGuiSliderFlags_AlwaysClamp);

        ImGui::Separator();
        ImGui::Text("Alive Profile");

        ImGui::Text("Energy Effect (Alive)");
        ImGui::SliderFloat("##aliveFromAlive",
                   &sim.aliveEffectFromAlive,
                   -1.0f, 1.0f, "%.3f",
                   ImGuiSliderFlags_AlwaysClamp);

        ImGui::Text("Energy Effect (Hindered)");
        ImGui::SliderFloat("##aliveFromHindered",
                   &sim.aliveEffectFromHindered,
                   -1.0f, 1.0f, "%.3f",
                   ImGuiSliderFlags_AlwaysClamp);

        ImGui::Text("Energy Effect (Dead)");
        ImGui::SliderFloat("##aliveFromDead",
                   &sim.aliveEffectFromDead,
                   -1.0f, 1.0f, "%.3f",
                   ImGuiSliderFlags_AlwaysClamp);

        ImGui::End();

        //---------------------------------------
        // Render grid
        //---------------------------------------

        ImGui::Begin("Grid");

        ImDrawList* draw = ImGui::GetWindowDrawList();
        gridOrigin = ImGui::GetCursorScreenPos();

        for (int y = 0; y < sim.height; y++)
        {
            for (int x = 0; x < sim.width; x++)
            {
                Cell& c = sim.At(x, y);

                ImU32 stateColor;

                switch (c.state)
                {
                    case CellState::Alive:
                        stateColor = IM_COL32(0, 255, 0, 255);
                        break;

                    case CellState::Hindered:
                        stateColor = IM_COL32(255, 255, 0, 255);
                        break;

                    case CellState::Dead:
                    default:
                        stateColor = IM_COL32(60, 60, 60, 255);
                        break;
                }

                // Blend the state color by energy instead of discarding it,
                // so Alive/Hindered/Dead stay visually distinct while still
                // showing energy as brightness.
                //
                // Normalize by maxEnergy (not a bare assumption of 1.0) so
                // the brightness scale stays correct if maxEnergy is tuned
                // above or below its default, and clamp the final channel
                // values before packing so a stale/out-of-range energy
                // value (e.g. hand-edited settings.json, or a brief
                // mid-tune transient) can never overflow a color channel
                // and bleed into the adjacent bits of the packed ImU32.
                float e = (sim.maxEnergy > 0.0f) ? (c.energy / sim.maxEnergy) : 0.0f;
                if (e < 0.0f) e = 0.0f;
                if (e > 1.0f) e = 1.0f;

                auto ClampChannel = [](float v) -> int
                {
                    if (v < 0.0f) return 0;
                    if (v > 255.0f) return 255;
                    return (int)v;
                };

                int r = ClampChannel(((stateColor >> IM_COL32_R_SHIFT) & 0xFF) * e);
                int g = ClampChannel(((stateColor >> IM_COL32_G_SHIFT) & 0xFF) * e);
                int b = ClampChannel(((stateColor >> IM_COL32_B_SHIFT) & 0xFF) * e);

                ImU32 color = IM_COL32(r, g, b, 255);

                ImVec2 p0(gridOrigin.x + x * cellSize, gridOrigin.y + y * cellSize);
                ImVec2 p1(p0.x + cellSize, p0.y + cellSize);

                draw->AddRectFilled(p0, p1, color);
            }
        }

        ImGui::Dummy(ImVec2(sim.width * cellSize, sim.height * cellSize));

        // Check hover on the canvas item specifically (not
        // io.WantCaptureMouse, which is true for the whole Grid window,
        // including this canvas — that would block painting entirely).
        bool canvasHovered = ImGui::IsItemHovered();

        ImGui::End();

        //---------------------------------------
        // Mouse painting
        //---------------------------------------
        // Only paint while the mouse is actually over the grid canvas, so
        // clicking sliders/buttons in other windows never paints, but
        // clicking inside the grid always does.

        if (canvasHovered && ImGui::IsMouseDown(ImGuiMouseButton_Left))
        {
            ImVec2 mouse = ImGui::GetMousePos();

            float localX = mouse.x - gridOrigin.x;
            float localY = mouse.y - gridOrigin.y;

            CellState state = CellState::Alive;

            if (paintMode == 1) state = CellState::Hindered;
            if (paintMode == 2) state = CellState::Dead;

            sim.PaintCell(localX, localY, cellSize, state);
        }

        if (running)
        {
            // Accumulate real time and only step the sim often enough to
            // match simRate, regardless of how fast frames are rendering.
            // Clamped so a long stall (e.g. window minimized) doesn't cause
            // a huge burst of steps all at once when it comes back.
            double simInterval = 1.0 / (double)simRate;
            simAccumulator += deltaTime;

            double maxBacklog = simInterval * 5.0;
            if (simAccumulator > maxBacklog)
                simAccumulator = maxBacklog;

            while (simAccumulator >= simInterval)
            {
                sim.Step();
                simAccumulator -= simInterval;
            }
        }

        //---------------------------------------
        // Render
        //---------------------------------------

        ImGui::Render();

        int w, h;
        glfwGetFramebufferSize(window, &w, &h);

        glViewport(0, 0, w, h);
        glClearColor(0.08f, 0.08f, 0.08f, 1.0f);
        glClear(GL_COLOR_BUFFER_BIT);

        ImGui_ImplOpenGL3_RenderDrawData(ImGui::GetDrawData());

        glfwSwapBuffers(window);
    }

    ImGui_ImplOpenGL3_Shutdown();
    ImGui_ImplGlfw_Shutdown();
    ImGui::DestroyContext();

    glfwDestroyWindow(window);
    glfwTerminate();

    return 0;
}