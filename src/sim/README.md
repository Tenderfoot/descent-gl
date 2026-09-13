# src/sim

Simulation: fixed-timestep state and integration, physics, collision,
projectiles, AI. Nothing in here may touch OpenGL.

Empty as of M0. The M0 fixed-step loop and its `SimState` live in
`src/game/` because they are app-level plumbing; M1 moves ship state here.
