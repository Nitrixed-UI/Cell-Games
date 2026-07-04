#pragma once

#include <vector>

#include "types.hpp"

enum class CellState : u8
{
    Alive,
    Hindered,
    Dead
};

struct Cell
{
    CellState state = CellState::Dead;
    CellState nextState = CellState::Dead;

    // energy is the "committed" value from the previous completed Step().
    // nextEnergy is computed during Phase 1 and only written back into
    // energy during the Commit phase. This mirrors state/nextState and
    // guarantees every cell in a given Step() sees the same, consistent
    // snapshot of its neighbors' energy — independent of raster scan
    // order. Without this, a cell's energy could be partially updated
    // before its neighbors read it in the same pass, producing
    // order-dependent, non-reproducible results.
    float energy = 0.0f;
    float nextEnergy = 0.0f;
};

struct Neighborhood
{
    int alive = 0;
    int hindered = 0;
    int dead = 0;

    float totalEnergy = 0.0f;
    float averageEnergy = 0.0f;
};

class Cell_Manager
{
public:
    int width = 0;
    int height = 0;

    int total_population = 0;

    // Tuning parameters exposed to the UI. Step() reads these directly —
    // do NOT declare local variables with these same names inside Step(),
    // or the sliders will silently stop doing anything.
    float hinderThreshold = 0.5f;

    float maxEnergy = 1.0f;
    int overcrowdingThreshold = 5;
    float lonlinessPunishment = 0.5f;

    // A Dead cell becomes Alive if it has EXACTLY reviveAliveCount alive
    // neighbors, OR EXACTLY reviveHinderedCount hindered neighbors.
    int reviveAliveCount = 3;
    int reviveHinderedCount = 4;

    // When true, neighbor lookups wrap around the grid edges (toroidal
    // topology) instead of treating off-grid neighbors as missing.
    bool wrapEdges = true;

    // ============================================================
    // Hindered Cell Profile
    // ============================================================
    // Signed per-neighbor-type energy effects. Positive heals, negative
    // decays, scaled by how many neighbors of that type are present.
    float hinderedEffectFromAlive    = 0.20f;
    float hinderedEffectFromHindered = -0.10f;
    float hinderedEffectFromDead     = 0.0f;

    // ============================================================
    // Alive Cell Profile
    // ============================================================
    // Signed per-neighbor-type energy effects. Positive heals, negative
    // decays, scaled by how many neighbors of that type are present.
    float aliveEffectFromAlive    = 0.02f;
    float aliveEffectFromHindered = -0.05f;
    float aliveEffectFromDead     = 0.0f;

    std::vector<Cell> cells;

    static constexpr int dx[8] =
    {
        -1, 0, 1,
        -1,    1,
        -1, 0, 1
    };

    static constexpr int dy[8] =
    {
        -1,-1,-1,
         0,    0,
         1, 1, 1
    };

    inline int Index(int x, int y) const
    {
        return y * width + x;
    }

    Cell& At(int x, int y)
    {
        return cells[Index(x, y)];
    }

    const Cell& At(int x, int y) const
    {
        return cells[Index(x, y)];
    }

    Neighborhood GetNeighborhood(int x, int y) const
    {
        Neighborhood n;
        int validNeighbors = 0;

        for (int i = 0; i < 8; i++)
        {
            int nx = x + dx[i];
            int ny = y + dy[i];

            if (wrapEdges)
            {
                // Wrap into [0, width) / [0, height), handling negative
                // results from the initial modulo correctly.
                nx = ((nx % width) + width) % width;
                ny = ((ny % height) + height) % height;
            }
            else if (nx < 0 || ny < 0 || nx >= width || ny >= height)
            {
                continue;
            }

            validNeighbors++;

            const Cell& cell = At(nx, ny);

            switch (cell.state)
            {
                case CellState::Alive:
                    n.alive++;
                    break;

                case CellState::Hindered:
                    n.hindered++;
                    break;

                case CellState::Dead:
                    n.dead++;
                    break;
            }

            // Reads cell.energy — the last *committed* energy value, never
            // nextEnergy — so neighborhood energy stats are always based
            // on a consistent, fully-settled previous frame.
            n.totalEnergy += cell.energy;
        }

        if (validNeighbors > 0)
            n.averageEnergy = n.totalEnergy / validNeighbors;

        return n;
    }

    // mouseX/mouseY must already be relative to the grid's on-screen origin
    // (i.e. caller has subtracted the ImGui window's cursor-screen-pos).
    void PaintCell(float mouseX, float mouseY, float cellSize, CellState paintState)
    {
        if (mouseX < 0.0f || mouseY < 0.0f)
            return;

        int x = static_cast<int>(mouseX / cellSize);
        int y = static_cast<int>(mouseY / cellSize);

        if (x < 0 || y < 0 || x >= width || y >= height)
            return;

        Cell& c = At(x, y);

        c.state = paintState;
        c.nextState = paintState;
        c.energy = (paintState == CellState::Dead) ? 0.0f : maxEnergy;
        c.nextEnergy = c.energy;
    }

    void Step()
    {
        total_population = 0;

        // Phase 1: Compute next state AND next energy. Nothing here
        // mutates c.energy or c.state — only c.nextEnergy / c.nextState —
        // so every cell processed later in this same pass still sees
        // fully consistent (previous-frame) neighbor values.
        for (int y = 0; y < height; y++)
        {
            for (int x = 0; x < width; x++)
            {
                Cell& c = At(x, y);
                Neighborhood n = GetNeighborhood(x, y);

                //---------------------------------
                // Derived counts
                //---------------------------------

                int nonDead = n.alive + n.hindered;

                //---------------------------------
                // Energy Rules
                //---------------------------------
                // All rules read/write via a local accumulator seeded from
                // the committed energy, then get written to nextEnergy at
                // the end — c.energy itself is never touched here.

                float e = c.energy;

                // Hindered Profile: each neighbor type has its own signed
                // effect — positive heals, negative decays — scaled by
                // how many neighbors of that type are present.
                if (c.state == CellState::Hindered)
                {
                    e += n.alive * hinderedEffectFromAlive;
                    e += n.hindered * hinderedEffectFromHindered;
                    e += n.dead * hinderedEffectFromDead;
                }

                // Alive Profile: each neighbor type has its own signed
                // effect — positive heals, negative decays — scaled by
                // how many neighbors of that type are present.
                if (c.state == CellState::Alive)
                {
                    e += n.alive * aliveEffectFromAlive;
                    e += n.hindered * aliveEffectFromHindered;
                    e += n.dead * aliveEffectFromDead;
                }

                // Overcrowding kills outright, regardless of state.
                if (nonDead >= overcrowdingThreshold)
                    e = 0.0f;

                // Fully isolated cells lose everything.
                if (n.alive == 0 && n.hindered == 0)
                    e -= lonlinessPunishment;

                // Clamp energy to the tunable ceiling.
                if (e < 0.0f)
                    e = 0.0f;

                if (e > maxEnergy)
                    e = maxEnergy;

                //---------------------------------
                // State Rules
                //---------------------------------

                CellState previousState = c.state;

                if (c.state == CellState::Dead)
                {
                    // Dead cells always have zero energy, so they must be
                    // checked here rather than falling into the energy<=0
                    // branch below, which would catch them first and mask
                    // the revival rule entirely.
                    if (n.alive == reviveAliveCount || n.hindered == reviveHinderedCount)
                        c.nextState = CellState::Alive;
                    else
                        c.nextState = CellState::Dead;
                }
                else if (e <= 0.0f)
                {
                    c.nextState = CellState::Dead;
                }
                else if (e < hinderThreshold)
                {
                    c.nextState = CellState::Hindered;
                }
                else
                {
                    c.nextState = CellState::Alive;
                }

                // Newly alive cells start with full energy.
                if (previousState != CellState::Alive &&
                    c.nextState == CellState::Alive)
                {
                    e = maxEnergy;
                }

                // A cell that just died has no energy, regardless of what
                // the rules above computed.
                if (c.nextState == CellState::Dead)
                {
                    e = 0.0f;
                }

                c.nextEnergy = e;
            } // x loop
        } // y loop

        //---------------------------------
        // Commit
        //---------------------------------
        // State and energy are committed together, atomically from the
        // caller's point of view — no cell is ever left with a state from
        // one tick and energy from another.

        for (auto& c : cells) {
            c.state = c.nextState;
            c.energy = c.nextEnergy;

            if (c.state != CellState::Dead) {
                total_population++;
            }
        }
    }
};